/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL
v2. You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sm_manager.h"

#include "transaction/transaction_manager.h"

#include <sys/stat.h>
#include <unistd.h>

#include <fstream>

#include "index/ix.h"
#include "record/rm.h"
#include "record_printer.h"
#include "common/wire_protocol.h"

// path 是否为已存在的目录
bool SmManager::is_dir(const std::string& db_name) {
  struct stat st;
  return stat(db_name.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// 建同名目录、写入空 db.meta 和 db.log
void SmManager::create_db(const std::string& db_name) {
  if (is_dir(db_name)) {
    throw DatabaseExistsError(db_name);
  }
  std::string cmd = "mkdir " + db_name;
  if (system(cmd.c_str()) < 0) {
    throw UnixError();
  }
  if (chdir(db_name.c_str()) < 0) {
    throw UnixError();
  }

  DbMeta new_db;
  new_db.name_ = db_name;
  std::ofstream ofs(DB_META_NAME);
  ofs << new_db;
  ofs.close();

  disk_manager_->create_file(LOG_FILE_NAME);

  if (chdir("..") < 0) {
    throw UnixError();
  }
}

// rm -r 整个目录
void SmManager::drop_db(const std::string& db_name) {
  if (!is_dir(db_name)) {
    throw DatabaseNotFoundError(db_name);
  }
  std::string cmd = "rm -r " + db_name;
  if (system(cmd.c_str()) < 0) {
    throw UnixError();
  }
}

// chdir 进 db 目录、加载 db.meta、把所有表和索引文件 open
void SmManager::open_db(const std::string& db_name) {
  if (!is_dir(db_name)) {
    throw DatabaseNotFoundError(db_name);
  }
  if (chdir(db_name.c_str()) < 0) {
    throw UnixError();
  }
  std::ifstream ifs(DB_META_NAME);
  ifs >> db_;
  ifs.close();

  for (auto& entry : db_.tabs_) {
    auto& tab = entry.second;
    fhs_.emplace(tab.name, rm_manager_->open_file(tab.name));
    for (auto& index : tab.indexes) {
      auto ix_name = ix_manager_->get_index_name(tab.name, index.cols);
      ihs_.emplace(ix_name, ix_manager_->open_index(tab.name, index.cols));
    }
  }
}

// 把内存中的元数据写回 db.meta
void SmManager::flush_meta() {
  std::ofstream ofs(DB_META_NAME);
  ofs << db_;
}

// 把所有 .rm/.idx 落盘并关闭，清空内存中的元数据
void SmManager::close_db() {
  if (db_.name_.empty()) {
    return;
  }
  flush_meta();
  for (auto& entry : fhs_) {
    rm_manager_->close_file(entry.second.get());
  }
  fhs_.clear();
  for (auto& entry : ihs_) {
    ix_manager_->close_index(entry.second.get());
  }
  ihs_.clear();
  db_.name_.clear();
  db_.tabs_.clear();
}

// 打印所有表名，同时保留兼容输出文件的写入路径。
void SmManager::show_tables(Context* context) {
  if (context != nullptr && context->wire_writer_ != nullptr) {
    context->wire_writer_->begin_result({{"Tables", wire::kChar}});
    for (const auto& entry : db_.tabs_) {
      context->wire_writer_->send_char_row({entry.second.name});
    }
    return;
  }
  bool ow = g_output_enabled.load();  // set output_file off 时不写 output.txt
  std::fstream outfile;
  if (ow) {
    outfile.open("output.txt", std::ios::out | std::ios::app);
    outfile << "| Tables |\n";
  }

  RecordPrinter printer(1);
  printer.print_separator(context);
  printer.print_record({"Tables"}, context);
  printer.print_separator(context);
  for (auto& entry : db_.tabs_) {
    auto& tab = entry.second;
    printer.print_record({tab.name}, context);
    if (ow) outfile << "| " << tab.name << " |\n";
  }
  printer.print_separator(context);
  if (ow) outfile.close();
}

// 打印 tab_name 上所有索引：| tab | unique | (c1,c2) |
void SmManager::show_index(const std::string& tab_name, Context* context) {
  TabMeta& tab = db_.get_table(tab_name);

  if (context != nullptr && context->wire_writer_ != nullptr) {
    context->wire_writer_->begin_result(
        {{"Table", wire::kChar}, {"Unique", wire::kChar},
         {"Columns", wire::kChar}});
    for (const auto& index : tab.indexes) {
      std::string cols = "(";
      for (size_t i = 0; i < index.cols.size(); ++i) {
        if (i > 0) cols += ",";
        cols += index.cols[i].name;
      }
      cols += ")";
      context->wire_writer_->send_char_row({tab_name, "unique", cols});
    }
    return;
  }

  bool ow = g_output_enabled.load();  // set output_file off 时不写 output.txt
  std::fstream outfile;
  if (ow) outfile.open("output.txt", std::ios::out | std::ios::app);

  RecordPrinter printer(3);
  for (auto& index : tab.indexes) {
    std::string cols = "(";
    for (size_t i = 0; i < index.cols.size(); ++i) {
      if (i > 0) {
        cols += ",";
      }
      cols += index.cols[i].name;
    }
    cols += ")";

    printer.print_record({tab_name, "unique", cols}, context);
    if (ow) outfile << "| " << tab_name << " | unique | " << cols << " |\n";
  }
  if (ow) outfile.close();
}

// 打印指定表的字段信息
void SmManager::desc_table(const std::string& tab_name, Context* context) {
  TabMeta& tab = db_.get_table(tab_name);

  if (context != nullptr && context->wire_writer_ != nullptr) {
    context->wire_writer_->begin_result(
        {{"Field", wire::kChar}, {"Type", wire::kChar},
         {"Index", wire::kChar}});
    for (const auto& col : tab.cols) {
      context->wire_writer_->send_char_row(
          {col.name, coltype2str(col.type), col.index ? "YES" : "NO"});
    }
    return;
  }

  std::vector<std::string> captions = {"Field", "Type", "Index"};
  RecordPrinter printer(captions.size());
  printer.print_separator(context);
  printer.print_record(captions, context);
  printer.print_separator(context);
  for (auto& col : tab.cols) {
    printer.print_record(
        {col.name, coltype2str(col.type), col.index ? "YES" : "NO"}, context);
  }
  printer.print_separator(context);
}

// 创建表：建 .rm 数据文件并把 TabMeta 落盘
void SmManager::create_table(const std::string& tab_name,
                             const std::vector<ColDef>& col_defs,
                             Context* context) {
  if (db_.is_table(tab_name)) {
    throw TableExistsError(tab_name);
  }
  int offset = 0;
  TabMeta tab;
  tab.name = tab_name;
  for (auto& def : col_defs) {
    ColMeta col = {.tab_name = tab_name,
                   .name = def.name,
                   .type = def.type,
                   .len = def.len,
                   .offset = offset,
                   .index = false};
    offset += def.len;
    tab.cols.push_back(col);
  }

  rm_manager_->create_file(tab_name, offset);
  db_.tabs_[tab_name] = tab;
  fhs_.emplace(tab_name, rm_manager_->open_file(tab_name));
  flush_meta();
  // 防御：同名表此前被 drop 过时，确保旧版本存储不会污染新表
  if (context != nullptr && context->txn_mgr_ != nullptr) {
    context->txn_mgr_->PurgeTableVersions(tab_name);
  }
}

// 关闭并销毁数据文件、删掉该表上所有索引、再从元数据移除
void SmManager::drop_table(const std::string& tab_name, Context* context) {
  if (!db_.is_table(tab_name)) {
    throw TableNotFoundError(tab_name);
  }
  // 清空该表的内存版本存储：同名重建表后旧版本/旧 pending 残留会产生幽灵行
  // 避免重复建表场景中的元数据残留与伪写写冲突。
  if (context != nullptr && context->txn_mgr_ != nullptr) {
    context->txn_mgr_->PurgeTableVersions(tab_name);
  }
  TabMeta tab = db_.get_table(tab_name);

  auto fh_it = fhs_.find(tab_name);
  if (fh_it != fhs_.end()) {
    rm_manager_->close_file(fh_it->second.get());
    fhs_.erase(fh_it);
  }
  rm_manager_->destroy_file(tab_name);

  for (auto& index : tab.indexes) {
    auto ix_name = ix_manager_->get_index_name(tab_name, index.cols);
    auto ih_it = ihs_.find(ix_name);
    if (ih_it != ihs_.end()) {
      ix_manager_->close_index(ih_it->second.get());
      ihs_.erase(ih_it);
    }
    ix_manager_->destroy_index(tab_name, index.cols);
  }

  db_.tabs_.erase(tab_name);
  flush_meta();
}

// 在 tab_name(col_names) 上建唯一索引；用现有记录回填
void SmManager::create_index(const std::string& tab_name,
                             const std::vector<std::string>& col_names,
                             Context* context) {
  TabMeta& tab = db_.get_table(tab_name);
  if (tab.is_index(col_names)) {
    throw IndexExistsError(tab_name, col_names);
  }

  IndexMeta index_meta;
  index_meta.tab_name = tab_name;
  index_meta.col_num = (int)col_names.size();
  index_meta.col_tot_len = 0;
  for (auto& cname : col_names) {
    auto col = tab.get_col(cname);
    index_meta.cols.push_back(*col);
    index_meta.col_tot_len += col->len;
  }

  ix_manager_->create_index(tab_name, index_meta.cols);
  auto ih = ix_manager_->open_index(tab_name, index_meta.cols);

  auto fh = fhs_.at(tab_name).get();
  std::vector<char> key(index_meta.col_tot_len);
  for (RmScan scan(fh); !scan.is_end(); scan.next()) {
    Rid rid = scan.rid();
    auto rec = fh->get_record(rid, context);
    int off = 0;
    for (auto& col : index_meta.cols) {
      memcpy(key.data() + off, rec->data + col.offset, col.len);
      off += col.len;
    }
    ih->insert_entry(key.data(), rid, context ? context->txn_ : nullptr);
  }

  auto ix_name = ix_manager_->get_index_name(tab_name, index_meta.cols);
  ihs_.emplace(ix_name, std::move(ih));
  tab.indexes.push_back(index_meta);
  flush_meta();
}

// 删除指定字段名集合对应的索引
void SmManager::drop_index(const std::string& tab_name,
                           const std::vector<std::string>& col_names,
                           Context* context) {
  TabMeta& tab = db_.get_table(tab_name);
  auto index_it =
      tab.get_index_meta(col_names);  // 找不到会抛 IndexNotFoundError

  auto ix_name = ix_manager_->get_index_name(tab_name, col_names);
  auto ih_it = ihs_.find(ix_name);
  if (ih_it != ihs_.end()) {
    ix_manager_->close_index(ih_it->second.get());
    ihs_.erase(ih_it);
  }
  ix_manager_->destroy_index(tab_name, col_names);
  tab.indexes.erase(index_it);
  flush_meta();
}

// ColMeta 版重载：把列名抽出来转给上面的实现
void SmManager::drop_index(const std::string& tab_name,
                           const std::vector<ColMeta>& cols, Context* context) {
  std::vector<std::string> col_names;
  col_names.reserve(cols.size());
  for (auto& c : cols) {
    col_names.push_back(c.name);
  }
  drop_index(tab_name, col_names, context);
}
