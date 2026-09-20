/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL
v2. You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "execution_manager.h"

#include <sstream>

#include "executor_delete.h"
#include "executor_index_scan.h"
#include "executor_insert.h"
#include "executor_nestedloop_join.h"
#include "executor_projection.h"
#include "executor_seq_scan.h"
#include "executor_update.h"
#include "explain_printer.h"
#include "index/ix.h"
#include "record_printer.h"
#include "common/wire_protocol.h"

const char* help_info =
    "Supported SQL syntax:\n"
    "  command ;\n"
    "command:\n"
    "  CREATE TABLE table_name (column_name type [, column_name type ...])\n"
    "  DROP TABLE table_name\n"
    "  CREATE INDEX table_name (column_name)\n"
    "  DROP INDEX table_name (column_name)\n"
    "  INSERT INTO table_name VALUES (value [, value ...])\n"
    "  DELETE FROM table_name [WHERE where_clause]\n"
    "  UPDATE table_name SET column_name = value [, column_name = value ...] "
    "[WHERE where_clause]\n"
    "  SELECT selector FROM table_name [WHERE where_clause]\n"
    "type:\n"
    "  {INT | FLOAT | CHAR(n)}\n"
    "where_clause:\n"
    "  condition [AND condition ...]\n"
    "condition:\n"
    "  column op {column | value}\n"
    "column:\n"
    "  [table_name.]column_name\n"
    "op:\n"
    "  {= | <> | < | > | <= | >=}\n"
    "selector:\n"
    "  {* | column [, column ...]}\n";

// 主要负责执行DDL语句
void QlManager::run_mutli_query(std::shared_ptr<Plan> plan, Context* context) {
  if (auto x = std::dynamic_pointer_cast<DDLPlan>(plan)) {
    switch (x->tag) {
      case T_CreateTable: {
        sm_manager_->create_table(x->tab_name_, x->cols_, context);
        break;
      }
      case T_DropTable: {
        sm_manager_->drop_table(x->tab_name_, context);
        break;
      }
      case T_CreateIndex: {
        sm_manager_->create_index(x->tab_name_, x->tab_col_names_, context);
        break;
      }
      case T_DropIndex: {
        sm_manager_->drop_index(x->tab_name_, x->tab_col_names_, context);
        break;
      }
      default:
        throw InternalError("Unexpected field type");
        break;
    }
  }
}

// 执行help; show tables; desc table; begin; commit; abort;语句
void QlManager::run_cmd_utility(std::shared_ptr<Plan> plan, txn_id_t* txn_id,
                                Context* context) {
  if (auto x = std::dynamic_pointer_cast<OtherPlan>(plan)) {
    switch (x->tag) {
      case T_Help: {
        if (context->wire_writer_ != nullptr) {
          context->wire_writer_->begin_result({{"Help", wire::kChar}});
          context->wire_writer_->send_char_row({help_info});
          break;
        }
        memcpy(context->data_send_ + *(context->offset_), help_info,
               strlen(help_info));
        *(context->offset_) = strlen(help_info);
        break;
      }
      case T_ShowTable: {
        sm_manager_->show_tables(context);
        break;
      }
      case T_ShowIndex: {
        sm_manager_->show_index(x->tab_name_, context);
        break;
      }
      case T_DescTable: {
        sm_manager_->desc_table(x->tab_name_, context);
        break;
      }
      case T_Transaction_begin: {
        // 显示开启一个事务
        context->txn_->set_txn_mode(true);
        break;
      }
      case T_Transaction_commit: {
        context->txn_ = txn_mgr_->get_transaction(*txn_id);
        txn_mgr_->commit(context->txn_, context->log_mgr_);
        break;
      }
      case T_Transaction_rollback: {
        context->txn_ = txn_mgr_->get_transaction(*txn_id);
        txn_mgr_->abort(context->txn_, context->log_mgr_);
        break;
      }
      case T_Transaction_abort: {
        context->txn_ = txn_mgr_->get_transaction(*txn_id);
        txn_mgr_->abort(context->txn_, context->log_mgr_);
        break;
      }
      default:
        throw InternalError("Unexpected field type");
        break;
    }

  } else if (auto x = std::dynamic_pointer_cast<SetKnobPlan>(plan)) {
    switch (x->set_knob_type_) {
      case ast::SetKnobType::EnableNestLoop: {
        planner_->set_enable_nestedloop_join(x->bool_value_);
        break;
      }
      case ast::SetKnobType::EnableSortMerge: {
        planner_->set_enable_sortmerge_join(x->bool_value_);
        break;
      }
      default: {
        throw RMDBError("Not implemented!\n");
        break;
      }
    }
  } else if (auto x = std::dynamic_pointer_cast<SetIsolationPlan>(plan)) {
    // SET TRANSACTION ISOLATION LEVEL：只更新会话级隔离级别，立即提交隐式事务
    if (context->session_iso_ != nullptr) {
      *(context->session_iso_) = x->iso_;
    }
    if (context->txn_ != nullptr) {
      context->txn_->set_isolation_level(x->iso_);
    }
  }
}

// 执行select语句，select语句的输出除了需要返回客户端外，还需要写入output.txt文件中
void QlManager::select_from(std::unique_ptr<AbstractExecutor> executorTreeRoot,
                            std::vector<TabCol> sel_cols, Context* context) {
  std::vector<std::string> captions;
  const auto& result_cols = executorTreeRoot->cols();
  captions.reserve(result_cols.size());
  for (size_t i = 0; i < result_cols.size(); ++i) {
    // 聚合列优先用 alias 作为表头；普通列用 col_name
    if (i < sel_cols.size()) {
      captions.push_back(sel_cols[i].alias.empty() ? sel_cols[i].col_name
                                                   : sel_cols[i].alias);
    } else {
      captions.push_back(result_cols[i].name);
    }
  }

  // Wire v3 直接从执行器的原始 tuple 编码类型化 ROW，避免文本格式化丢失
  // FLOAT32 位模式或把 CHAR 的页内 padding 发到网络。RESULT_END 在事务提交
  // 成功后由连接层发送。
  if (context->wire_writer_ != nullptr) {
    context->wire_writer_->begin_record_result(captions, result_cols);
    for (executorTreeRoot->beginTuple(); !executorTreeRoot->is_end();
         executorTreeRoot->nextTuple()) {
      auto tuple = executorTreeRoot->Next();
      context->wire_writer_->send_record(*tuple);
    }
    return;
  }

  // Print header into client buffer
  RecordPrinter rec_printer(sel_cols.size());
  rec_printer.print_separator(context);
  rec_printer.print_record(captions, context);
  rec_printer.print_separator(context);

  // 先把 output.txt 内容缓冲在内存里：如果扫描中途因 SSI/MVCC 抛 abort，
  // 整段头+数据都不写文件，避免与标准答案的"半截表头"差异。
  std::ostringstream file_buf;
  file_buf << "|";
  for (int i = 0; i < captions.size(); ++i) {
    file_buf << " " << captions[i] << " |";
  }
  file_buf << "\n";

  size_t num_rec = 0;
  for (executorTreeRoot->beginTuple(); !executorTreeRoot->is_end();
       executorTreeRoot->nextTuple()) {
    auto Tuple = executorTreeRoot->Next();
    std::vector<std::string> columns;
    for (auto& col : executorTreeRoot->cols()) {
      std::string col_str;
      char* rec_buf = Tuple->data + col.offset;
      if (col.type == TYPE_INT) {
        // 8 字节 INT 列 = SUM(整型) 的 int64 聚合输出(防大表溢出);其余为常规 int32
        if (col.len == (int)sizeof(int64_t)) {
          col_str = std::to_string(*(int64_t*)rec_buf);
        } else {
          col_str = std::to_string(*(int*)rec_buf);
        }
      } else if (col.type == TYPE_FLOAT) {
        col_str = std::to_string(*(float*)rec_buf);
      } else if (col.type == TYPE_STRING) {
        col_str = std::string((char*)rec_buf, col.len);
        col_str.resize(strlen(col_str.c_str()));
      }
      columns.push_back(col_str);
    }
    rec_printer.print_record(columns, context);
    file_buf << "|";
    for (int i = 0; i < columns.size(); ++i) {
      file_buf << " " << columns[i] << " |";
    }
    file_buf << "\n";
    num_rec++;
  }

  // 扫描成功结束后再一次性 flush 到 output.txt(set output_file off 时跳过写盘)
  if (g_output_enabled.load()) {
    std::fstream outfile;
    outfile.open("output.txt", std::ios::out | std::ios::app);
    outfile << file_buf.str();
    outfile.close();
  }
  rec_printer.print_separator(context);
  RecordPrinter::print_record_count(num_rec, context);
}

// 执行DML语句
void QlManager::run_dml(std::unique_ptr<AbstractExecutor> exec) {
  exec->Next();
}

// EXPLAIN ANALYZE：消费整棵算子树，但不输出结果行；遍历完后渲染计划树和 rows
void QlManager::explain_analyze(std::unique_ptr<AbstractExecutor> exec,
                                std::shared_ptr<Plan> plan, Context* context) {
  for (exec->beginTuple(); !exec->is_end(); exec->nextTuple()) {
    (void)exec->Next();
  }

  auto dml = std::dynamic_pointer_cast<DMLPlan>(plan);
  Plan* root_plan = dml ? dml->subplan_.get() : plan.get();

  ExplainPrinter printer;
  std::string tree = printer.render(root_plan, exec.get());

  if (context != nullptr && context->wire_writer_ != nullptr) {
    context->wire_writer_->begin_result({{"QUERY PLAN", wire::kChar}});
    context->wire_writer_->send_char_row({tree});
    return;
  }

  // 写客户端响应缓冲
  if (context != nullptr) {
    memcpy(context->data_send_ + *(context->offset_), tree.data(),
           tree.size());
    *(context->offset_) += tree.size();
  }
  // 写 output.txt（与 select_from 的写法保持一致；set output_file off 时跳过）
  if (g_output_enabled.load()) {
    std::fstream outfile;
    outfile.open("output.txt", std::ios::out | std::ios::app);
    outfile << tree;
    outfile.close();
  }
}
