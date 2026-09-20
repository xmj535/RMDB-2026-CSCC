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

#include <cassert>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "parser/ast.h"
#include "parser/parser.h"
#include "transaction/txn_defs.h"

typedef enum PlanTag {
  T_Invalid = 1,
  T_Help,
  T_ShowTable,
  T_ShowIndex,
  T_DescTable,
  T_CreateTable,
  T_DropTable,
  T_CreateIndex,
  T_DropIndex,
  T_SetKnob,
  T_Insert,
  T_Update,
  T_Delete,
  T_select,
  T_Transaction_begin,
  T_Transaction_commit,
  T_Transaction_abort,
  T_Transaction_rollback,
  T_SeqScan,
  T_IndexScan,
  T_NestLoop,
  T_SortMerge,  // sort merge join
  T_Sort,
  T_Projection,
  T_Filter,
  T_Aggregate,
  T_Limit,
  T_Union,
  T_Explain,
  T_Set_Isolation,
  T_Load,
} PlanTag;

// 查询执行计划
class Plan {
 public:
  PlanTag tag;
  virtual ~Plan() = default;
};

class ScanPlan : public Plan {
 public:
  ScanPlan(PlanTag tag, SmManager* sm_manager, std::string tab_name,
           std::vector<Condition> conds,
           std::vector<std::string> index_col_names)
      : ScanPlan(tag, sm_manager, std::move(tab_name), std::string(),
                 std::move(conds), std::move(index_col_names)) {}

  // alias 表示显示名，空串则取 tab_name；cols_ 中 ColMeta.tab_name 用显示名标注
  ScanPlan(PlanTag tag, SmManager* sm_manager, std::string tab_name,
           std::string alias, std::vector<Condition> conds,
           std::vector<std::string> index_col_names) {
    Plan::tag = tag;
    tab_name_ = std::move(tab_name);
    alias_ = alias.empty() ? tab_name_ : std::move(alias);
    conds_ = std::move(conds);
    TabMeta& tab = sm_manager->db_.get_table(tab_name_);
    cols_ = tab.cols;
    for (auto& c : cols_) c.tab_name = alias_;
    len_ = cols_.empty() ? 0 : (cols_.back().offset + cols_.back().len);
    fed_conds_ = conds_;
    ssi_conds_ = conds_;
    index_col_names_ = std::move(index_col_names);
  }
  ~ScanPlan() {}
  // 以下变量同ScanExecutor中的变量
  std::string tab_name_;
  std::string alias_;
  std::vector<ColMeta> cols_;
  std::vector<Condition> conds_;
  size_t len_;
  std::vector<Condition> fed_conds_;
  // Serializable 读跟踪使用的本表谓词。SELECT 计划将条件放在 Scan 之上的
  // Filter 节点时，fed_conds_ 可能为空；若扫描层 SSI 钩子按空谓词登记，
  // 会退化成"全表读"——任何并发写都与之建边，
  // 拼出假危险结构。planner 在套 Filter 时把单表条件同步进这里，使
  // 记录读/谓词读/不可见写者检查保持行级精确，而不影响执行与统计。
  std::vector<Condition> ssi_conds_;
  std::vector<std::string> index_col_names_;

  // 跳跃扫描标记。**由 planner 显式置位，执行器不得自行推断**：执行器只看得到
  // conds_，而 INLJ 内表的索引首列是被【连接条件】绑定的（is_rhs_val=false，
  // 运行时才由外行喂值），从条件里看去和"首列完全无谓词"一模一样。让执行器自己
  // 判断，内表点查会被误判成跳跃扫描，一次探测扫遍首列的所有取值。
  bool use_skip_scan_ = false;

  // INLJ 探测标记：planner 把该 Scan 升级成 IndexScan 后置 true，
  // 此时 portal 会走 IndexJoinExecutor 路径，而不是单独构造 IndexScanExecutor。
  bool is_join_probe_ = false;
  // 索引列下标 -> 外侧 TabCol（tab_name 取外表 alias）
  std::vector<std::pair<int, TabCol>> probe_bindings_;
  // INLJ 实际使用的复合索引前缀长度。前缀列可来自内表固定等值谓词，
  // 也可来自 probe_bindings_ 指向的外表列。
  int probe_prefix_len_ = 0;
};

class FilterPlan : public Plan {
 public:
  FilterPlan(std::shared_ptr<Plan> subplan, std::vector<Condition> conds) {
    Plan::tag = T_Filter;
    subplan_ = std::move(subplan);
    conds_ = std::move(conds);
  }
  ~FilterPlan() {}
  std::shared_ptr<Plan> subplan_;
  std::vector<Condition> conds_;
};

class JoinPlan : public Plan {
 public:
  JoinPlan(PlanTag tag, std::shared_ptr<Plan> left, std::shared_ptr<Plan> right,
           std::vector<Condition> conds) {
    Plan::tag = tag;
    left_ = std::move(left);
    right_ = std::move(right);
    conds_ = std::move(conds);
    type = INNER_JOIN;
  }
  ~JoinPlan() {}
  // 左节点
  std::shared_ptr<Plan> left_;
  // 右节点
  std::shared_ptr<Plan> right_;
  // 连接条件
  std::vector<Condition> conds_;
  // future TODO: 后续可以支持的连接类型
  JoinType type;
  // 内表连接列不在任何索引最左前缀上（升级不了 INLJ）且内表足够大时置位，
  // portal 据此改用 HashJoinExecutor，避免 |outer|×|inner| 的无界重扫。
  // 仍是 JoinPlan/T_NestLoop，EXPLAIN 的 Join(...) 文本形态不变。
  bool use_hash_ = false;
  // 无跨表等值（哈希连接不适用）但朴素代价过大时置位：改走分块嵌套循环，
  // 内表扫描次数从 |outer| 降到 ceil(|outer|/块行数)，同样保持 JoinPlan 形态。
  bool use_block_ = false;
};

class ProjectionPlan : public Plan {
 public:
  ProjectionPlan(PlanTag tag, std::shared_ptr<Plan> subplan,
                 std::vector<TabCol> sel_cols) {
    Plan::tag = tag;
    subplan_ = std::move(subplan);
    sel_cols_ = std::move(sel_cols);
    is_star_ = false;
  }
  ~ProjectionPlan() {}
  std::shared_ptr<Plan> subplan_;
  std::vector<TabCol> sel_cols_;
  // SELECT * 时由上层标记，EXPLAIN 输出 columns=[*]
  bool is_star_;
};

class SortPlan : public Plan {
 public:
  // 单列构造（向后兼容）
  SortPlan(PlanTag tag, std::shared_ptr<Plan> subplan, TabCol sel_col,
           bool is_desc) {
    Plan::tag = tag;
    subplan_ = std::move(subplan);
    sel_cols_.push_back(sel_col);
    is_descs_.push_back(is_desc);
  }
  // 多列排序构造。
  SortPlan(PlanTag tag, std::shared_ptr<Plan> subplan,
           std::vector<TabCol> sel_cols, std::vector<bool> is_descs) {
    Plan::tag = tag;
    subplan_ = std::move(subplan);
    sel_cols_ = std::move(sel_cols);
    is_descs_ = std::move(is_descs);
  }
  ~SortPlan() {}
  std::shared_ptr<Plan> subplan_;
  std::vector<TabCol> sel_cols_;
  std::vector<bool> is_descs_;
};

// 聚合 / 分组算子；sel_cols_ 中每一项要么命中 group_by_cols_，要么
// agg!=AGG_NONE
class AggregatePlan : public Plan {
 public:
  AggregatePlan(std::shared_ptr<Plan> subplan, std::vector<TabCol> sel_cols,
                std::vector<TabCol> group_by_cols,
                std::vector<HavingCond> having_conds) {
    Plan::tag = T_Aggregate;
    subplan_ = std::move(subplan);
    sel_cols_ = std::move(sel_cols);
    group_by_cols_ = std::move(group_by_cols);
    having_conds_ = std::move(having_conds);
  }
  ~AggregatePlan() {}
  std::shared_ptr<Plan> subplan_;
  std::vector<TabCol> sel_cols_;
  std::vector<TabCol> group_by_cols_;
  std::vector<HavingCond> having_conds_;
};

// LIMIT N
class LimitPlan : public Plan {
 public:
  LimitPlan(std::shared_ptr<Plan> subplan, int limit) {
    Plan::tag = T_Limit;
    subplan_ = std::move(subplan);
    limit_ = limit;
  }
  ~LimitPlan() {}
  std::shared_ptr<Plan> subplan_;
  int limit_;
};

// UNION 集合算子：n 个子计划，输出列采用提升后的公共超类型
class UnionPlan : public Plan {
 public:
  UnionPlan(std::vector<std::shared_ptr<Plan>> branches,
            std::vector<ColMeta> out_cols) {
    Plan::tag = T_Union;
    branches_ = std::move(branches);
    out_cols_ = std::move(out_cols);
  }
  ~UnionPlan() {}
  std::vector<std::shared_ptr<Plan>> branches_;
  std::vector<ColMeta> out_cols_;  // tab_name = alias，按公共超类型展开
};

// dml语句，包括insert; delete; update; select语句
class DMLPlan : public Plan {
 public:
  DMLPlan(PlanTag tag, std::shared_ptr<Plan> subplan, std::string tab_name,
          std::vector<Value> values, std::vector<Condition> conds,
          std::vector<SetClause> set_clauses) {
    Plan::tag = tag;
    subplan_ = std::move(subplan);
    tab_name_ = std::move(tab_name);
    values_ = std::move(values);
    conds_ = std::move(conds);
    set_clauses_ = std::move(set_clauses);
    is_explain_ = false;
  }
  ~DMLPlan() {}
  std::shared_ptr<Plan> subplan_;
  std::string tab_name_;
  std::vector<Value> values_;
  std::vector<Condition> conds_;
  std::vector<SetClause> set_clauses_;
  // T_select 时由 do_planner 设置；为 true 表示外层是 EXPLAIN ANALYZE 包装
  bool is_explain_;
};

// ddl语句, 包括create/drop table; create/drop index;
class DDLPlan : public Plan {
 public:
  DDLPlan(PlanTag tag, std::string tab_name, std::vector<std::string> col_names,
          std::vector<ColDef> cols) {
    Plan::tag = tag;
    tab_name_ = std::move(tab_name);
    cols_ = std::move(cols);
    tab_col_names_ = std::move(col_names);
  }
  ~DDLPlan() {}
  std::string tab_name_;
  std::vector<std::string> tab_col_names_;
  std::vector<ColDef> cols_;
};

// help; show tables; desc tables; begin; abort; commit; rollback语句对应的plan
class OtherPlan : public Plan {
 public:
  OtherPlan(PlanTag tag, std::string tab_name) {
    Plan::tag = tag;
    tab_name_ = std::move(tab_name);
  }
  ~OtherPlan() {}
  std::string tab_name_;
};

// load file_path into tab_name; —— 批量 CSV 导入
class LoadPlan : public Plan {
 public:
  LoadPlan(std::string file_path, std::string tab_name) {
    Plan::tag = T_Load;
    file_path_ = std::move(file_path);
    tab_name_ = std::move(tab_name);
  }
  ~LoadPlan() {}
  std::string file_path_;
  std::string tab_name_;
};

// Set Knob Plan
class SetKnobPlan : public Plan {
 public:
  SetKnobPlan(ast::SetKnobType knob_type, bool bool_value) {
    Plan::tag = T_SetKnob;
    set_knob_type_ = knob_type;
    bool_value_ = bool_value;
  }
  ast::SetKnobType set_knob_type_;
  bool bool_value_;
};

// SET TRANSACTION ISOLATION LEVEL <SNAPSHOT ISOLATION | SERIALIZABLE>
class SetIsolationPlan : public Plan {
 public:
  SetIsolationPlan(IsolationLevel iso) : iso_(iso) {
    Plan::tag = T_Set_Isolation;
  }
  IsolationLevel iso_;
};

class plannerInfo {
 public:
  std::shared_ptr<ast::SelectStmt> parse;
  std::vector<Condition> where_conds;
  std::vector<TabCol> sel_cols;
  std::shared_ptr<Plan> plan;
  std::vector<std::shared_ptr<Plan>> table_scan_executors;
  std::vector<SetClause> set_clauses;
  plannerInfo(std::shared_ptr<ast::SelectStmt> parse_)
      : parse(std::move(parse_)) {}
};
