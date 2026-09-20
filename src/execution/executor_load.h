/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

// load file_path into tab_name; —— 批量 CSV 导入。
// 语义:作为已提交基表数据直接写堆 + 维护全部索引(无 MVCC 版本链,
// VisibleRecordForScan 对无版本行返回堆记录=对所有读者可见)。导入结束统一
// flush 堆与索引页确保落盘;崩溃恢复时索引可由堆重建(rebuild_table_indexes)。
class LoadExecutor : public AbstractExecutor {
 private:
  TabMeta tab_;
  RmFileHandle* fh_;
  std::string tab_name_;
  std::string file_path_;
  SmManager* sm_manager_;
  Rid rid_;

  // 按列类型把单个 CSV 字段写入记录缓冲
  void write_field(char* dst, const ColMeta& col, const std::string& field) {
    switch (col.type) {
      case TYPE_INT: {
        int v = std::atoi(field.c_str());
        memcpy(dst, &v, sizeof(int));
        break;
      }
      case TYPE_FLOAT: {
        float v = static_cast<float>(std::atof(field.c_str()));
        memcpy(dst, &v, sizeof(float));
        break;
      }
      case TYPE_STRING:
      default: {
        memset(dst, 0, col.len);
        size_t n = std::min(field.size(), static_cast<size_t>(col.len));
        memcpy(dst, field.data(), n);
        break;
      }
    }
  }

  IxIndexHandle* get_index_handle(const IndexMeta& index) {
    auto ix_name =
        sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols);
    return sm_manager_->ihs_.at(ix_name).get();
  }

  std::vector<char> make_key(const IndexMeta& index, const char* rec_data) {
    std::vector<char> key(index.col_tot_len);
    int off = 0;
    for (auto& col : index.cols) {
      memcpy(key.data() + off, rec_data + col.offset, col.len);
      off += col.len;
    }
    return key;
  }

 public:
  LoadExecutor(SmManager* sm_manager, const std::string& tab_name,
               const std::string& file_path, Context* context) {
    sm_manager_ = sm_manager;
    tab_ = sm_manager_->db_.get_table(tab_name);
    tab_name_ = tab_name;
    file_path_ = file_path;
    fh_ = sm_manager_->fhs_.at(tab_name).get();
    context_ = context;
  }

  std::unique_ptr<RmRecord> Next() override {
    std::ifstream ifs(file_path_);
    if (!ifs.is_open()) {
      throw RMDBError("load: cannot open file " + file_path_);
    }

    const size_t ncol = tab_.cols.size();
    const int rec_size = fh_->get_file_hdr().record_size;

    std::string line;
    bool header_skipped = false;
    std::vector<std::string> fields;
    fields.reserve(ncol);

    while (std::getline(ifs, line)) {
      // 去掉行尾 \r(兼容 CRLF)
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.empty()) continue;
      // 首行是表头(列名),跳过
      if (!header_skipped) {
        header_skipped = true;
        continue;
      }

      // 按 ',' 切分(TPCC 字符串为字母数字,无内嵌逗号/引号)
      fields.clear();
      size_t start = 0;
      while (true) {
        size_t comma = line.find(',', start);
        if (comma == std::string::npos) {
          fields.push_back(line.substr(start));
          break;
        }
        fields.push_back(line.substr(start, comma - start));
        start = comma + 1;
      }
      if (fields.size() != ncol) {
        throw RMDBError("load: column count mismatch in " + tab_name_);
      }

      // 拼记录
      RmRecord rec(rec_size);
      for (size_t i = 0; i < ncol; ++i) {
        write_field(rec.data + tab_.cols[i].offset, tab_.cols[i], fields[i]);
      }

      // 写堆(基表数据,无 MVCC 版本)
      rid_ = fh_->insert_record(rec.data, context_);
      if (rid_.page_no < 0 || rid_.slot_no < 0) {
        throw InternalError("load: insert_record returned invalid rid");
      }

      // 维护全部索引(含复合键)
      for (auto& index : tab_.indexes) {
        auto ih = get_index_handle(index);
        std::vector<char> key = make_key(index, rec.data);
        ih->insert_entry(key.data(), rid_, nullptr);
      }
    }
    ifs.close();

    // 落盘:堆 + 全部索引页,保证导入数据持久(崩溃后索引亦可由堆重建)
    auto* bpm = sm_manager_->get_bpm();
    bpm->flush_all_pages(fh_->GetFd());
    for (auto& index : tab_.indexes) {
      bpm->flush_all_pages(get_index_handle(index)->GetFd());
    }
    return nullptr;
  }

  Rid& rid() override { return rid_; }
};
