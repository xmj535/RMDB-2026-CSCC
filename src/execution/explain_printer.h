/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL
v2. You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once

#include <algorithm>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "executor_abstract.h"
#include "optimizer/plan.h"

// EXPLAIN ANALYZE 用的计划树渲染器：与算子树并行遍历，从算子读出 rows_out_
class ExplainPrinter {
 public:
  std::string render(Plan* plan, AbstractExecutor* exec) {
    std::ostringstream oss;
    render_node(plan, exec, 0, oss);
    return oss.str();
  }

 private:
  static std::string indent(int depth) { return std::string(depth, '\t'); }

  static const char* op_str(CompOp op) {
    switch (op) {
      case OP_EQ: return "=";
      case OP_NE: return "<>";
      case OP_LT: return "<";
      case OP_GT: return ">";
      case OP_LE: return "<=";
      case OP_GE: return ">=";
    }
    return "?";
  }

  // 把单条 Condition 还原成 SQL 文本形式（列名用别名/显示名）
  static std::string cond_text(const Condition& c) {
    std::ostringstream oss;
    oss << c.lhs_col.tab_name << "." << c.lhs_col.col_name << op_str(c.op);
    if (c.is_rhs_val) {
      oss << c.rhs_raw;
    } else {
      oss << c.rhs_col.tab_name << "." << c.rhs_col.col_name;
    }
    return oss.str();
  }

  static std::string render_conds(const std::vector<Condition>& conds) {
    std::vector<std::string> texts;
    texts.reserve(conds.size());
    for (auto& c : conds) texts.push_back(cond_text(c));
    std::sort(texts.begin(), texts.end());
    std::ostringstream oss;
    for (size_t i = 0; i < texts.size(); ++i) {
      if (i > 0) oss << ", ";
      oss << texts[i];
    }
    return oss.str();
  }

  static std::string render_cols(const std::vector<TabCol>& cols) {
    std::vector<std::string> texts;
    texts.reserve(cols.size());
    for (auto& c : cols) texts.push_back(c.tab_name + "." + c.col_name);
    std::sort(texts.begin(), texts.end());
    std::ostringstream oss;
    for (size_t i = 0; i < texts.size(); ++i) {
      if (i > 0) oss << ", ";
      oss << texts[i];
    }
    return oss.str();
  }

  // 收集 plan 子树下所有 Scan 节点的真实表名
  static void collect_real_tables(Plan* plan, std::set<std::string>& out) {
    if (auto x = dynamic_cast<ScanPlan*>(plan)) {
      out.insert(x->tab_name_);
    } else if (auto x = dynamic_cast<FilterPlan*>(plan)) {
      collect_real_tables(x->subplan_.get(), out);
    } else if (auto x = dynamic_cast<JoinPlan*>(plan)) {
      collect_real_tables(x->left_.get(), out);
      collect_real_tables(x->right_.get(), out);
    } else if (auto x = dynamic_cast<ProjectionPlan*>(plan)) {
      collect_real_tables(x->subplan_.get(), out);
    } else if (auto x = dynamic_cast<SortPlan*>(plan)) {
      collect_real_tables(x->subplan_.get(), out);
    } else if (auto x = dynamic_cast<AggregatePlan*>(plan)) {
      collect_real_tables(x->subplan_.get(), out);
    } else if (auto x = dynamic_cast<LimitPlan*>(plan)) {
      collect_real_tables(x->subplan_.get(), out);
    }
  }

  void render_node(Plan* plan, AbstractExecutor* exec, int depth,
                   std::ostringstream& oss) {
    if (plan == nullptr) return;
    if (auto p = dynamic_cast<ProjectionPlan*>(plan)) {
      oss << indent(depth) << "Project(columns=[";
      if (p->is_star_) {
        oss << "*";
      } else {
        oss << render_cols(p->sel_cols_);
      }
      oss << "], rows=" << (exec ? exec->rows_out_ : 0) << ")\n";
      render_node(p->subplan_.get(), exec ? exec->child() : nullptr, depth + 1,
                  oss);
    } else if (auto p = dynamic_cast<JoinPlan*>(plan)) {
      std::set<std::string> tables;
      collect_real_tables(plan, tables);
      std::vector<std::string> ts(tables.begin(), tables.end());
      std::sort(ts.begin(), ts.end());
      oss << indent(depth) << "Join(tables=[";
      for (size_t i = 0; i < ts.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << ts[i];
      }
      oss << "], condition=[" << render_conds(p->conds_)
          << "], rows=" << (exec ? exec->rows_out_ : 0) << ")\n";
      render_node(p->left_.get(), exec ? exec->left_child() : nullptr,
                  depth + 1, oss);
      render_node(p->right_.get(), exec ? exec->right_child() : nullptr,
                  depth + 1, oss);
    } else if (auto p = dynamic_cast<FilterPlan*>(plan)) {
      oss << indent(depth) << "Filter(condition=[" << render_conds(p->conds_)
          << "], rows=" << (exec ? exec->rows_out_ : 0) << ")\n";
      render_node(p->subplan_.get(), exec ? exec->child() : nullptr, depth + 1,
                  oss);
    } else if (auto p = dynamic_cast<ScanPlan*>(plan)) {
      oss << indent(depth) << "Scan(table=" << p->tab_name_;
      if (p->tag == T_IndexScan && !p->index_col_names_.empty()) {
        oss << ", type=IndexScan, using_index=(";
        for (size_t i = 0; i < p->index_col_names_.size(); ++i) {
          if (i > 0) oss << ",";
          oss << p->index_col_names_[i];
        }
        oss << ")";
      } else {
        oss << ", type=SeqScan";
      }
      oss << ", rows=" << (exec ? exec->rows_out_ : 0) << ")\n";
    } else if (auto p = dynamic_cast<SortPlan*>(plan)) {
      render_node(p->subplan_.get(), exec ? exec->child() : nullptr, depth,
                  oss);
    } else if (auto p = dynamic_cast<AggregatePlan*>(plan)) {
      // 聚合节点在 q5 中不要求出现在 EXPLAIN 输出，透传到下层
      render_node(p->subplan_.get(), exec ? exec->child() : nullptr, depth,
                  oss);
    } else if (auto p = dynamic_cast<LimitPlan*>(plan)) {
      render_node(p->subplan_.get(), exec ? exec->child() : nullptr, depth,
                  oss);
    }
  }
};
