/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL
v2. You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "planner.h"

#include <memory>
#include <set>

#include "execution/executor_delete.h"
#include "execution/executor_index_scan.h"
#include "execution/executor_insert.h"
#include "execution/executor_nestedloop_join.h"
#include "execution/executor_projection.h"
#include "execution/executor_seq_scan.h"
#include "execution/executor_update.h"
#include "index/ix.h"
#include "record_printer.h"
#include "transaction/transaction_manager.h"

// 决定读写路径是否强制 SeqScan。
// SERIALIZABLE 模式下采用更保守的 SeqScan：SSI 的读集/谓词登记依赖完整的表级
// 观察语义。IndexScan 只命中当前物理索引键，并发改键或改过滤列时，旧快照可能经
// 索引漏行并改变反依赖图，从而影响可串行化冲突判定。
//
// SNAPSHOT 模式可使用 IndexScan：快照隔离允许相应的幻读语义，索引/表扫描
// 的读集差异在快照语义下合法;② TPC-C 索引建在主键上,而事务只改非键列(d_next_o_id/
// s_quantity/c_balance…),键列不可变 → 经索引绝不漏行。强制 SeqScan 会让每笔 NewOrder
// 对百万级表做多次全表扫 + 表粒度读集致并发全 abort → tpmC=0;放开后点查走索引、读写
// 足迹收窄,吞吐恢复。
static bool force_seqscan_iso(Context* context) {
  if (context == nullptr) return false;
  if (context->txn_ != nullptr) {
    return context->txn_->get_isolation_level() ==
           IsolationLevel::SERIALIZABLE;
  }
  // PREPARE_SET 在 BEGIN 前生成计划，此时还没有事务对象，但连接级隔离
  // 已经设置。不能因 txn_ 为空而把 SERIALIZABLE prepared plan 当作 SI。
  return context->session_iso_ != nullptr &&
         *context->session_iso_ == IsolationLevel::SERIALIZABLE;
}

// 最左匹配：在 tab_name 的所有索引中挑能匹配最长前缀的；不限定单点
// 命中时 index_col_names 写回该索引的全部列名（供执行器复用）
// 跳跃扫描的启用门槛：表小于这个行数时顺序扫描本来就便宜，不值得为每个首列
// 取值付一次索引下降。也保证功能测试的小表计划形态与 EXPLAIN 输出完全不变。
static constexpr int64_t kSkipScanMinRows = 100000;

bool Planner::get_index_cols(const std::string& tab_name,
                             const std::string& disp,
                             const std::vector<Condition>& curr_conds,
                             std::vector<std::string>& index_col_names,
                             bool allow_skip_scan) {
  index_col_names.clear();
  TabMeta& tab = sm_manager_->db_.get_table(tab_name);

  auto cond_kind = [&](const std::string& col_name) {
    // 0: 无；1: 有范围；2: 有等值
    int kind = 0;
    for (auto& cond : curr_conds) {
      if (!cond.is_rhs_val) continue;
      if (cond.lhs_col.tab_name != disp) continue;
      if (cond.lhs_col.col_name != col_name) continue;
      if (cond.op == OP_EQ) return 2;
      if (cond.op == OP_LT || cond.op == OP_GT || cond.op == OP_LE ||
          cond.op == OP_GE) {
        kind = std::max(kind, 1);
      }
    }
    return kind;
  };

  int best_match = 0;
  const IndexMeta* best_idx = nullptr;
  for (auto& idx : tab.indexes) {
    int match = 0;
    for (auto& col : idx.cols) {
      int kind = cond_kind(col.name);
      if (kind == 0) break;
      match++;
      if (kind == 1) break;  // 范围列之后不再匹配
    }
    if (match > best_match) {
      best_match = match;
      best_idx = &idx;
    }
  }

  // 没有任何索引能被最左前缀匹配上时，再看一眼跳跃扫描：首列完全没谓词、
  // 第二列起有连续等值列。此时最左前缀路径不存在，唯一的替代是全表顺序扫描
  // ——TPC-C Delivery 的
  //   select sum(ol_amount) from order_line where ol_d_id=? and ol_o_id=?
  // 正是如此，索引 (ol_w_id,ol_d_id,ol_o_id,ol_number) 的首列 ol_w_id 没绑，
  // 本机实测该语句扫满 1500 万行要 1395.6 ms，绑上首列只要 0.115 ms。
  // 判定条件与 IndexScanExecutor::detect_skip_prefix() 逐条对应，两边同源。
  if (best_match == 0 || best_idx == nullptr) {
    const IndexMeta* skip_idx = nullptr;
    int skip_eq_run = 0;
    // 两道门槛：
    // 1) 调用方必须是单表扫描（allow_skip_scan）。多表时把跳跃扫描算作"有固定
    //    前缀"会污染 join 规划，见 planner.h 处的说明。
    // 2) 表要足够大：小表顺序扫描本来就便宜，而跳跃扫描每个首列取值要付一次
    //    索引下降；这也让功能测试的小表计划形态与 EXPLAIN 输出完全不变。
    if (allow_skip_scan && estimated_rows(tab_name) > kSkipScanMinRows) {
      for (auto& idx : tab.indexes) {
        if (idx.cols.size() < 2) continue;
        // 首列必须是 INT 且完全无谓词（执行器按 v+1 求后继，不推导通用后继）
        if (idx.cols[0].type != TYPE_INT) continue;
        if (idx.cols[0].len != static_cast<int>(sizeof(int))) continue;
        if (cond_kind(idx.cols[0].name) != 0) continue;
        int eq_run = 0;
        for (size_t i = 1; i < idx.cols.size(); ++i) {
          if (cond_kind(idx.cols[i].name) != 2) break;
          ++eq_run;
        }
        // 等值列越多，每次探测圈定的区间越窄
        if (eq_run > skip_eq_run) {
          skip_eq_run = eq_run;
          skip_idx = &idx;
        }
      }
    }
    if (skip_idx == nullptr) return false;
    best_idx = skip_idx;
  }

  for (auto& col : best_idx->cols) {
    index_col_names.push_back(col.name);
  }
  return true;
}

// 单表 Condition：lhs 在该表上，且 rhs 是值；或者 rhs 也是该表的列
static bool cond_only_for(const Condition& c, const std::string& disp) {
  if (c.lhs_col.tab_name != disp) return false;
  if (c.is_rhs_val) return true;
  return c.rhs_col.tab_name == disp;
}

// 跨两表 Condition：lhs 在 a，rhs 是列且在 b（或反过来）
static bool cond_joins(const Condition& c, const std::string& a,
                       const std::string& b) {
  if (c.is_rhs_val) return false;
  if (c.lhs_col.tab_name == a && c.rhs_col.tab_name == b) return true;
  if (c.lhs_col.tab_name == b && c.rhs_col.tab_name == a) return true;
  return false;
}

// 把 picked 中的等值 join 条件按"lhs=inner, rhs=outer"方向重整理一份副本，
// 供 INLJ 升级时按索引列名查找对应外侧 TabCol。
static std::vector<Condition> equi_join_inner_first(
    const std::vector<Condition>& picked, const std::string& inner_disp) {
  std::vector<Condition> out;
  for (auto& c : picked) {
    if (c.op != OP_EQ || c.is_rhs_val) continue;
    if (c.lhs_col.tab_name == inner_disp) {
      out.push_back(c);
    } else if (c.rhs_col.tab_name == inner_disp) {
      Condition r = c;
      std::swap(r.lhs_col, r.rhs_col);
      out.push_back(r);
    }
  }
  return out;
}

// 尝试把 inner_scan 升级为 INLJ 探测扫描：按 inner 表的索引列序数，
// 用内表固定等值谓词和"inner.col 等值绑定到外表列"共同形成最长 EQ 前缀。
// 命中则改写 tag + index_col_names_ + probe_bindings_ 并返回 true。
static bool try_upgrade_inlj(SmManager* sm_manager,
                             std::shared_ptr<ScanPlan> inner_scan,
                             const std::vector<Condition>& picked) {
  if (inner_scan == nullptr) return false;
  TabMeta& tab = sm_manager->db_.get_table(inner_scan->tab_name_);
  if (tab.indexes.empty()) return false;
  auto equi = equi_join_inner_first(picked, inner_scan->alias_);
  if (equi.empty()) return false;

  // 按列名查 picked 里能提供探测值的外侧列
  auto outer_for = [&](const std::string& inner_col) -> const TabCol* {
    for (auto& c : equi) {
      if (c.lhs_col.col_name == inner_col) return &c.rhs_col;
    }
    return nullptr;
  };
  auto fixed_eq_for = [&](const std::string& inner_col) -> const Condition* {
    for (auto& c : inner_scan->conds_) {
      if (c.is_rhs_val && c.op == OP_EQ &&
          c.lhs_col.tab_name == inner_scan->alias_ &&
          c.lhs_col.col_name == inner_col) {
        return &c;
      }
    }
    return nullptr;
  };

  const IndexMeta* best_idx = nullptr;
  int best_prefix = 0;
  for (auto& idx : tab.indexes) {
    int prefix = 0;
    bool has_outer_probe = false;
    for (auto& ic : idx.cols) {
      if (fixed_eq_for(ic.name) != nullptr) {
        ++prefix;
        continue;
      }
      if (outer_for(ic.name) != nullptr) {
        ++prefix;
        has_outer_probe = true;
        continue;
      }
      break;
    }
    // 只有固定谓词而没有外行依赖时不是 join probe，交给普通 IndexScan。
    if (!has_outer_probe) {
      continue;
    }
    if (prefix > best_prefix) {
      best_prefix = prefix;
      best_idx = &idx;
    }
  }
  if (best_prefix == 0 || best_idx == nullptr) return false;

  inner_scan->tag = T_IndexScan;
  inner_scan->index_col_names_.clear();
  inner_scan->probe_bindings_.clear();
  inner_scan->probe_prefix_len_ = best_prefix;
  for (auto& col : best_idx->cols) {
    inner_scan->index_col_names_.push_back(col.name);
  }
  for (int i = 0; i < best_prefix; ++i) {
    if (const TabCol* outer = outer_for(best_idx->cols[i].name)) {
      inner_scan->probe_bindings_.emplace_back(i, *outer);
    }
  }
  inner_scan->is_join_probe_ = true;
  return true;
}

// tab 降为内侧后能否真的变成索引探测。口径与 try_upgrade_inlj 一致：用它自己的
// 固定等值谓词加上与 outer_disp 的等值连接列去覆盖某个索引的最左前缀，且至少有
// 一列来自连接（只有固定谓词的不是 probe）。
// 换外侧前必须先过这一关：否则原首表降到内侧却升级不成 probe，就会被外侧每一行
// 重扫一遍（或退化成哈希连接多扫一遍），比不换更差。
static bool can_probe_as_inner(SmManager* sm_manager,
                               const std::string& tab_name,
                               const std::string& disp,
                               const std::vector<Condition>& own_conds,
                               const std::vector<Condition>& join_conds,
                               const std::string& outer_disp) {
  TabMeta& tab = sm_manager->db_.get_table(tab_name);
  auto fixed_eq = [&](const std::string& col) {
    for (auto& c : own_conds) {
      if (c.is_rhs_val && c.op == OP_EQ && c.lhs_col.tab_name == disp &&
          c.lhs_col.col_name == col) {
        return true;
      }
    }
    return false;
  };
  auto probe_from_outer = [&](const std::string& col) {
    for (auto& c : join_conds) {
      if (c.op != OP_EQ || c.is_rhs_val) continue;
      if (c.lhs_col.tab_name == disp && c.lhs_col.col_name == col &&
          c.rhs_col.tab_name == outer_disp) {
        return true;
      }
      if (c.rhs_col.tab_name == disp && c.rhs_col.col_name == col &&
          c.lhs_col.tab_name == outer_disp) {
        return true;
      }
    }
    return false;
  };
  for (auto& idx : tab.indexes) {
    int prefix = 0;
    bool has_probe = false;
    for (auto& ic : idx.cols) {
      if (fixed_eq(ic.name)) {
        ++prefix;
        continue;
      }
      if (probe_from_outer(ic.name)) {
        ++prefix;
        has_probe = true;
        continue;
      }
      break;
    }
    if (has_probe && prefix > 0) return true;
  }
  return false;
}

// 朴素嵌套循环的代价是 |outer|x|inner| 次行读。超过这个乘积就改用
// 有界算法（有跨表等值就哈希连接，否则分块嵌套循环）。
// 低于该阈值继续走既有朴素路径，保持小查询计划形态与 EXPLAIN 统计稳定。
// 1e8 约当于数秒级工作量；功能测试的小表连接远低于此。
static constexpr int64_t kMaxNaiveJoinRowReads = 100000000;

// 改写左深树初始外侧的表规模下限。FROM 首表小于它时一律保持 FROM 顺序，
// 使小表连接的计划形态与 EXPLAIN 输出保持稳定。
static constexpr int64_t kInitialOuterReorderRows = 10000;

// 该子计划是否已经是 INLJ 探测扫描（此时内表按索引点查，不需要哈希）
static bool inner_is_index_probe(const std::shared_ptr<Plan>& plan) {
  auto scan = std::dynamic_pointer_cast<ScanPlan>(plan);
  return scan != nullptr && scan->is_join_probe_;
}

// picked 里是否存在跨表等值条件（哈希连接的前提）
static bool has_cross_equi_cond(const std::vector<Condition>& picked) {
  for (auto& c : picked) {
    if (c.op == OP_EQ && !c.is_rhs_val) return true;
  }
  return false;
}

// 用文件头估算表行数上界：(num_pages-1) × num_records_per_page。
// 只用于选计划，宁可高估也不低估——高估只会多用一次哈希连接，不影响正确性。
int64_t Planner::estimated_rows(const std::string& tab_name) const {
  auto it = sm_manager_->fhs_.find(tab_name);
  if (it == sm_manager_->fhs_.end()) return 0;
  const RmFileHdr& hdr = it->second->get_file_hdr();
  int64_t pages = hdr.num_pages > 1 ? hdr.num_pages - 1 : 0;
  return pages * static_cast<int64_t>(hdr.num_records_per_page);
}

// 条件按需要"翻转"两侧：lhs 应当落到 left 表上
static Condition normalize_join_cond(const Condition& c,
                                     const std::string& left_disp) {
  static const std::map<CompOp, CompOp> swap_op = {
      {OP_EQ, OP_EQ}, {OP_NE, OP_NE}, {OP_LT, OP_GT},
      {OP_GT, OP_LT}, {OP_LE, OP_GE}, {OP_GE, OP_LE},
  };
  if (c.lhs_col.tab_name == left_disp) return c;
  Condition r = c;
  std::swap(r.lhs_col, r.rhs_col);
  r.op = swap_op.at(r.op);
  return r;
}

std::shared_ptr<Query> Planner::logical_optimization(
    std::shared_ptr<Query> query, Context* context) {
  // 谓词下推在 build_one_rel 内处理；这里仅保留扩展点
  return query;
}

std::shared_ptr<Plan> Planner::physical_optimization(
    std::shared_ptr<Query> query, Context* context) {
  std::shared_ptr<Plan> plan = make_one_rel(query, context);

  // 处理orderby
  plan = generate_sort_plan(query, std::move(plan));

  return plan;
}

// 收集 plan 子树涉及的所有显示表名（用于 Join.tables 输出）
static void collect_disp_names(const std::shared_ptr<Plan>& plan,
                               std::set<std::string>& out) {
  if (auto x = std::dynamic_pointer_cast<ScanPlan>(plan)) {
    out.insert(x->alias_);
  } else if (auto x = std::dynamic_pointer_cast<FilterPlan>(plan)) {
    collect_disp_names(x->subplan_, out);
  } else if (auto x = std::dynamic_pointer_cast<JoinPlan>(plan)) {
    collect_disp_names(x->left_, out);
    collect_disp_names(x->right_, out);
  } else if (auto x = std::dynamic_pointer_cast<ProjectionPlan>(plan)) {
    collect_disp_names(x->subplan_, out);
  } else if (auto x = std::dynamic_pointer_cast<SortPlan>(plan)) {
    collect_disp_names(x->subplan_, out);
  }
}

// 把 ColMeta 列扁平展开（多表 join 时用）
static std::vector<ColMeta> plan_cols(const std::shared_ptr<Plan>& plan) {
  if (auto x = std::dynamic_pointer_cast<ScanPlan>(plan)) {
    return x->cols_;
  }
  if (auto x = std::dynamic_pointer_cast<FilterPlan>(plan)) {
    return plan_cols(x->subplan_);
  }
  if (auto x = std::dynamic_pointer_cast<ProjectionPlan>(plan)) {
    std::vector<ColMeta> cols;
    auto sub = plan_cols(x->subplan_);
    size_t off = 0;
    for (auto& sc : x->sel_cols_) {
      for (auto& c : sub) {
        if (c.tab_name == sc.tab_name && c.name == sc.col_name) {
          ColMeta cm = c;
          cm.offset = off;
          off += cm.len;
          cols.push_back(cm);
          break;
        }
      }
    }
    return cols;
  }
  if (auto x = std::dynamic_pointer_cast<JoinPlan>(plan)) {
    auto left = plan_cols(x->left_);
    auto right = plan_cols(x->right_);
    size_t left_len = 0;
    for (auto& c : left)
      left_len = std::max(left_len, (size_t)(c.offset + c.len));
    for (auto& c : right) c.offset += left_len;
    left.insert(left.end(), right.begin(), right.end());
    return left;
  }
  return {};
}

// 在子树上插入 Project：仅保留 keep_cols 中列出的列（按字母序输出）
static std::shared_ptr<Plan> push_project(std::shared_ptr<Plan> sub,
                                          std::set<TabCol> keep_cols) {
  // keep_cols 中可能包含 sub 不含的列，需要做过滤
  auto sub_cols = plan_cols(sub);
  std::vector<TabCol> pick;
  for (auto& c : sub_cols) {
    TabCol tc{c.tab_name, c.name};
    if (keep_cols.count(tc) > 0) pick.push_back(tc);
  }
  // 按 tab.col 字母序输出
  std::sort(pick.begin(), pick.end());
  return std::make_shared<ProjectionPlan>(T_Projection, std::move(sub),
                                          std::move(pick));
}

// 把 Conditions 中涉及的列收集到 set 里
static void collect_cond_cols(const std::vector<Condition>& conds,
                              std::set<TabCol>& out) {
  for (auto& c : conds) {
    out.insert(c.lhs_col);
    if (!c.is_rhs_val) out.insert(c.rhs_col);
  }
}

std::shared_ptr<Plan> Planner::make_one_rel(std::shared_ptr<Query> query, Context *context) {
  std::vector<std::string> tables = query->tables;
  std::vector<std::string> aliases = query->aliases;
  if (aliases.size() < tables.size()) {
    aliases.resize(tables.size(), std::string());
  }
  // 显示名（alias 或 tab_name）
  std::vector<std::string> disps(tables.size());
  for (size_t i = 0; i < tables.size(); ++i) {
    disps[i] = aliases[i].empty() ? tables[i] : aliases[i];
  }
  const bool indexed_distinct_join =
      tables.size() == 2 && query->has_agg &&
      std::any_of(query->cols.begin(), query->cols.end(), [](const TabCol& col) {
        return col.agg == AGG_COUNT && col.is_distinct;
      }) &&
      std::all_of(aliases.begin(), aliases.end(),
                  [](const std::string& alias) { return alias.empty(); }) &&
      !force_seqscan_iso(context);

  // 先按表分类条件：disps[i] -> 该表的单表谓词
  std::vector<std::vector<Condition>> single_conds(tables.size());
  std::vector<Condition> join_conds;
  for (auto& c : query->conds) {
    bool placed = false;
    for (size_t i = 0; i < tables.size(); ++i) {
      if (cond_only_for(c, disps[i])) {
        single_conds[i].push_back(c);
        placed = true;
        break;
      }
    }
    if (!placed) {
      join_conds.push_back(c);
    }
  }

  // 每张表的固定谓词能否凑出某个索引最左前缀。这一遍先算，因为初始外侧的
  // 选择要用它，而选择结果又决定下面哪张表可以直接启用固定谓词 IndexScan。
  // 索引判断仍用真实表名 + 单表条件（IndexScan 内部按列名匹配）
  // 跳跃扫描只对单表查询开放：多表时把它算作"有固定前缀"会污染下面的初始外侧
  // 选择与 try_upgrade_inlj，见 planner.h 处 allow_skip_scan 的说明。
  const bool single_table = (tables.size() == 1);
  std::vector<std::vector<std::string>> idx_cols(tables.size());
  std::vector<bool> has_fixed_prefix(tables.size(), false);
  for (size_t i = 0; i < tables.size(); ++i) {
    has_fixed_prefix[i] = get_index_cols(tables[i], disps[i], single_conds[i],
                                         idx_cols[i], single_table);
  }

  // 左深树初始外侧：默认是 FROM 首表。但首表的固定谓词凑不出任何索引前缀时它
  // 只能全表扫，而外侧每次执行都要扫满整张表。此时若另有一张表能靠固定谓词走
  // 索引，改由它起步更划算：原首表退到内侧后，try_upgrade_inlj 能用外行值补上
  // 它缺失的前缀列。典型形态是 `FROM A, B WHERE B.k=:v AND A.k1=B.k AND A.k2=:v2`
  // ——A 的索引首列 k1 只由跨表等值绑定，get_index_cols 只认 is_rhs_val，看不见它。
  size_t start = 0;
  if (tables.size() > 1 && !force_seqscan_iso(context) &&
      !has_fixed_prefix[0] &&
      estimated_rows(tables[0]) > kInitialOuterReorderRows) {
    int64_t best_est = -1;
    for (size_t i = 1; i < tables.size(); ++i) {
      if (!has_fixed_prefix[i]) continue;
      // 候选必须与别的表有连接条件，否则从它起步会造出笛卡尔中间结果
      bool connected = false;
      for (auto& c : join_conds) {
        for (size_t j = 0; j < tables.size() && !connected; ++j) {
          if (j != i && cond_joins(c, disps[i], disps[j])) connected = true;
        }
        if (connected) break;
      }
      if (!connected) continue;
      // 换过去必须确实能把原首表变成索引探测，否则不换
      if (!can_probe_as_inner(sm_manager_, tables[0], disps[0], single_conds[0],
                              join_conds, disps[i])) {
        continue;
      }
      int64_t est = estimated_rows(tables[i]);
      if (best_est < 0 || est < best_est) {
        best_est = est;
        start = i;
      }
    }
  }

  // 为每张表构建 Scan + 可选 Filter
  std::vector<std::shared_ptr<Plan>> sub_plans(tables.size());
  for (size_t i = 0; i < tables.size(); ++i) {
    std::vector<std::string>& index_col_names = idx_cols[i];
    bool has_idx = has_fixed_prefix[i];
    // 谓词命中索引最左前缀 → IndexScan，残余条件由 IndexScanExecutor 内部过滤。
    // 普通 join 只在左深树的初始外侧直接启用固定谓词 IndexScan；后续内侧保持
    // Filter+SeqScan，只有 try_upgrade_inlj 能绑定外行探测值时才升级。这样避免
    // 无 probe 的普通 IndexScan 被 NestedLoop 反复重置，同时覆盖大表外侧点查。
    // 两表 DISTINCT 保留已验证的双侧索引路径。
    // 别名不再是障碍：IndexScanExecutor 现在按显示名匹配条件（见其 alias_）。
    const bool allow_fixed_index =
        tables.size() == 1 || i == start || indexed_distinct_join;
    if (has_idx && allow_fixed_index && !force_seqscan_iso(context)) {
      auto idx_scan = std::make_shared<ScanPlan>(T_IndexScan, sm_manager_,
                                                 tables[i], disps[i],
                                                 single_conds[i],
                                                 index_col_names);
      // 单表时才可能选中跳跃扫描的索引，执行器据此授权（默认关闭）
      idx_scan->use_skip_scan_ = single_table;
      sub_plans[i] = std::move(idx_scan);
      continue;
    }
    // Scan 自身不带条件，使 Scan rows 表示底层实际扫描到的总行数。
    std::vector<Condition> empty;
    auto scan =
        std::make_shared<ScanPlan>(T_SeqScan, sm_manager_, tables[i], disps[i],
                                   empty, std::vector<std::string>());
    if (!single_conds[i].empty()) {
      // 条件在上层 Filter 过滤，但 SSI 读跟踪仍需行级谓词（见 ScanPlan::ssi_conds_）
      scan->ssi_conds_ = single_conds[i];
      sub_plans[i] =
          std::make_shared<FilterPlan>(std::move(scan), single_conds[i]);
    } else {
      sub_plans[i] = std::move(scan);
    }
  }

  // 单表查询：直接返回
  if (tables.size() == 1) {
    return sub_plans[0];
  }

  // 左深 join：以 tables[start] 起步（start 见上方初始外侧选择，默认为 0），
  // 后续每一步优先挑「与已 join 集存在连接条件」的表， 找不到则退化到剩余表中
  // 下标最小的一个（保留 FROM 顺序的确定性）。 这样避免出现中间笛卡尔积，例如
  // FROM A,B,C WHERE A.x=C.x AND B.y=C.y 时 能自动按 A→C→B 的顺序连接，
  // 而不是 A×B 再 join C。
  std::vector<bool> used(tables.size(), false);
  used[start] = true;
  std::vector<std::string> joined_disps;
  joined_disps.push_back(disps[start]);
  std::shared_ptr<Plan> cur = sub_plans[start];
  // 左深树累积到当前为止的外侧规模估算，用于判断下一步连接的朴素代价
  int64_t outer_est = estimated_rows(tables[start]);

  for (size_t step = 1; step < tables.size(); ++step) {
    // 在未加入的表中挑下一个：优先「有连接条件」的最小下标
    int next_idx = -1;
    for (size_t i = 0; i < tables.size(); ++i) {
      if (used[i]) continue;
      bool connected = false;
      for (auto& c : join_conds) {
        for (auto& a : joined_disps) {
          if (cond_joins(c, a, disps[i])) {
            connected = true;
            break;
          }
        }
        if (connected) break;
      }
      if (connected) {
        next_idx = (int)i;
        break;
      }
    }
    if (next_idx < 0) {
      for (size_t i = 0; i < tables.size(); ++i) {
        if (!used[i]) {
          next_idx = (int)i;
          break;
        }
      }
    }
    used[next_idx] = true;

    // 把同时涉及当前外表集合与 disps[next_idx] 的连接条件挑出来
    std::vector<Condition> picked;
    auto it = join_conds.begin();
    while (it != join_conds.end()) {
      bool match = false;
      for (auto& a : joined_disps) {
        if (cond_joins(*it, a, disps[next_idx])) {
          match = true;
          break;
        }
      }
      if (match) {
        // 按 spec「Condition 的输出格式和原始的 SQL 语句保持一致」，
        // 不再 swap lhs/rhs，直接保留原始顺序。
        picked.push_back(*it);
        it = join_conds.erase(it);
      } else {
        ++it;
      }
    }
    // INLJ 升级：右侧若是裸 ScanPlan，且能在 picked 等值条件 + 表索引上对齐
    // 最左 EQ 前缀，则改写为 IndexScan，portal 据此走 IndexJoinExecutor。
    if (auto right_scan =
            std::dynamic_pointer_cast<ScanPlan>(sub_plans[next_idx])) {
      try_upgrade_inlj(sm_manager_, right_scan, picked);
    } else if (auto right_filter = std::dynamic_pointer_cast<FilterPlan>(
                   sub_plans[next_idx])) {
      // 内表自带单表谓词时 sub_plans 是 Filter(Scan)，裸 ScanPlan 的 cast 会落空。
      // 不还原就退化成"每个外行重扫一遍内表全表"（EXPLAIN 里 Scan rows 会等于
      // |outer|×|inner|）。先把谓词还给 Scan 再试升级，成功后 Filter 可以摘掉：
      // IndexScanExecutor / IndexJoinExecutor 都用 conds_ 自己过滤内表。
      // 别名不再是障碍：两个执行器都按显示名匹配条件（见各自的 alias_）。
      auto inner_scan =
          std::dynamic_pointer_cast<ScanPlan>(right_filter->subplan_);
      if (inner_scan != nullptr && inner_scan->tag == T_SeqScan &&
          !force_seqscan_iso(context)) {
        inner_scan->conds_ = right_filter->conds_;
        if (try_upgrade_inlj(sm_manager_, inner_scan, picked)) {
          sub_plans[next_idx] = inner_scan;
        } else {
          // 未升级：恢复原状，避免 Scan 和 Filter 重复过滤同一批条件
          inner_scan->conds_.clear();
        }
      }
    }
    auto join = std::make_shared<JoinPlan>(T_NestLoop, std::move(cur),
                                           sub_plans[next_idx], picked);
    // 没升级成 INLJ 时，内表会被每个外行整表重扫一遍，代价
    // |outer|x|inner|。注意是乘积而不只是内表大小：内表很小但外侧
    // （左深树累积结果）巨大时同样会爆。超过阈值就换有界算法：
    //   有跨表等值 → 哈希连接（两侧各扫一遍）
    //   无等值（如纯不等式连接）→ 分块嵌套循环（内表扫描次数
    //   从 |outer| 降到 ceil(|outer|/块行数)）
    // 二者共同保证：无论谓词形态如何，连接都不再是无界的。
    const int64_t inner_est = estimated_rows(tables[next_idx]);
    const int64_t naive_cost = outer_est > 0 && inner_est > 0
                                   ? outer_est * inner_est
                                   : 0;
    if (!inner_is_index_probe(sub_plans[next_idx]) &&
        naive_cost > kMaxNaiveJoinRowReads) {
      if (has_cross_equi_cond(picked)) {
        join->use_hash_ = true;
      } else {
        join->use_block_ = true;
      }
    }
    // 粗略更新外侧规模估算：等值连接结果通常不超过两侧较大者，
    // 无等值时按乘积算。只用于选算法，不影响正确性。
    outer_est = has_cross_equi_cond(picked)
                    ? std::max(outer_est, inner_est)
                    : (naive_cost > 0 ? naive_cost : outer_est);
    cur = std::move(join);
    joined_disps.push_back(disps[next_idx]);
  }
  return cur;
}

// 计算 plan 子树需要"暴露"给上层的列：根据上层用到的列集合，
// 把 plan 内部涉及的列裁剪到最小，并在 Join 子树的每个分支上插入 Project。
// under_join 表示当前节点是否处于某个 Join 的子树中——只有跨过 Join（笛卡尔积）
// 时才需要插入下推的 Project，单表查询不需要内层 Project。
static std::shared_ptr<Plan> project_pushdown(std::shared_ptr<Plan> plan,
                                              std::set<TabCol> needed,
                                              bool under_join) {
  if (auto x = std::dynamic_pointer_cast<SortPlan>(plan)) {
    // Sort 透明：往下继续推 Project，保持 under_join 状态
    x->subplan_ = project_pushdown(x->subplan_, std::move(needed), under_join);
    return plan;
  }
  if (auto x = std::dynamic_pointer_cast<JoinPlan>(plan)) {
    // Join 自身的条件也要用到这些列
    collect_cond_cols(x->conds_, needed);
    auto left_cols = plan_cols(x->left_);
    auto right_cols = plan_cols(x->right_);
    auto in_subtree = [&](const std::vector<ColMeta>& cs, const TabCol& tc) {
      for (auto& c : cs) {
        if (c.tab_name == tc.tab_name && c.name == tc.col_name) return true;
      }
      return false;
    };
    std::set<TabCol> left_needed, right_needed;
    for (auto& tc : needed) {
      if (in_subtree(left_cols, tc)) left_needed.insert(tc);
      if (in_subtree(right_cols, tc)) right_needed.insert(tc);
    }
    x->left_ = project_pushdown(x->left_, left_needed, true);
    x->right_ = project_pushdown(x->right_, right_needed, true);
    return plan;
  }
  if (auto x = std::dynamic_pointer_cast<FilterPlan>(plan)) {
    if (!under_join) return plan;
    // Project 放在 Filter 之上：Filter 读取 Scan 全部列做条件判断，
    // Project 只把 needed（不含 filter 列）暴露给上层 Join。
    // 即便所有列都被保留也显式 wrap，使「投影下推」始终出现在计划树中。
    return push_project(plan, std::move(needed));
  }
  if (std::dynamic_pointer_cast<ScanPlan>(plan)) {
    if (!under_join) return plan;
    return push_project(plan, std::move(needed));
  }
  return plan;
}

std::shared_ptr<Plan> Planner::generate_sort_plan(std::shared_ptr<Query> query,
                                                  std::shared_ptr<Plan> plan) {
  auto x = std::dynamic_pointer_cast<ast::SelectStmt>(query->parse);
  if (!x || !x->has_sort || !x->order) {
    return plan;
  }
  std::vector<std::string> tables = query->tables;
  std::vector<ColMeta> all_cols;
  for (auto& sel_tab_name : tables) {
    const auto& sel_tab_cols = sm_manager_->db_.get_table(sel_tab_name).cols;
    all_cols.insert(all_cols.end(), sel_tab_cols.begin(), sel_tab_cols.end());
  }
  std::vector<TabCol> sel_cols;
  std::vector<bool> is_descs;
  for (auto& item : x->order->items) {
    if (!item || !item->col) continue;
    TabCol sc{item->col->tab_name, item->col->col_name};
    if (sc.tab_name.empty()) {
      for (auto& col : all_cols) {
        if (col.name == sc.col_name) {
          sc.tab_name = col.tab_name;
          break;
        }
      }
    }
    sel_cols.push_back(sc);
    is_descs.push_back(item->dir == ast::OrderBy_DESC);
  }
  return std::make_shared<SortPlan>(T_Sort, std::move(plan), std::move(sel_cols),
                                    std::move(is_descs));
}

/**
 * @brief select plan 生成
 */
std::shared_ptr<Plan> Planner::generate_select_plan(
    std::shared_ptr<Query> query, Context* context) {
  query = logical_optimization(std::move(query), context);

  auto sel_cols = query->cols;

  // 派生 UNION：递归构建每个分支的子计划，再套 Union/Sort/Limit/Projection
  if (!query->union_branches.empty()) {
    std::vector<std::shared_ptr<Plan>> branch_plans;
    for (auto& bq : query->union_branches) {
      // 每个分支递归走 do_planner，返回 DMLPlan(T_select)；取其 subplan_ 作为子节点
      std::shared_ptr<Plan> bp = do_planner(bq, context);
      if (auto dml = std::dynamic_pointer_cast<DMLPlan>(bp)) {
        branch_plans.push_back(dml->subplan_);
      } else {
        branch_plans.push_back(bp);
      }
    }
    std::shared_ptr<Plan> root = std::make_shared<UnionPlan>(
        std::move(branch_plans), query->union_out_cols);

    if (auto stmt = std::dynamic_pointer_cast<ast::SelectStmt>(query->parse)) {
      if (stmt->has_sort && stmt->order) {
        std::vector<TabCol> sort_cols;
        std::vector<bool> sort_descs;
        for (auto& item : stmt->order->items) {
          if (!item || !item->col) continue;
          sort_cols.push_back(
              TabCol{item->col->tab_name, item->col->col_name});
          sort_descs.push_back(item->dir == ast::OrderBy_DESC);
        }
        if (!sort_cols.empty()) {
          root = std::make_shared<SortPlan>(T_Sort, std::move(root),
                                            std::move(sort_cols),
                                            std::move(sort_descs));
        }
      }
    }
    if (query->limit >= 0) {
      root = std::make_shared<LimitPlan>(std::move(root), query->limit);
    }
    auto top = std::make_shared<ProjectionPlan>(T_Projection, std::move(root),
                                                std::move(sel_cols));
    top->is_star_ = query->select_all;
    return top;
  }

  // 聚合查询：先构建 Scan/Filter/Join 子树，再放 AggregatePlan，最后 Projection
  if (query->has_agg) {
    std::shared_ptr<Plan> sub = make_one_rel(query, context);
    auto agg = std::make_shared<AggregatePlan>(
        std::move(sub), sel_cols, query->group_by_cols, query->having_conds);
    std::shared_ptr<Plan> root = agg;
    // ORDER BY / LIMIT 套在 Aggregate 之上
    if (auto stmt = std::dynamic_pointer_cast<ast::SelectStmt>(query->parse)) {
      if (stmt->has_sort && stmt->order) {
        std::vector<TabCol> sort_cols;
        std::vector<bool> sort_descs;
        for (auto& item : stmt->order->items) {
          if (!item || !item->col) continue;
          sort_cols.push_back(
              TabCol{item->col->tab_name, item->col->col_name});
          sort_descs.push_back(item->dir == ast::OrderBy_DESC);
        }
        if (!sort_cols.empty()) {
          root = std::make_shared<SortPlan>(T_Sort, std::move(root),
                                            std::move(sort_cols),
                                            std::move(sort_descs));
        }
      }
    }
    if (query->limit >= 0) {
      root = std::make_shared<LimitPlan>(std::move(root), query->limit);
    }
    // 顶层 Projection：对聚合列把 tab_name 留空 / col_name 用 alias，
    // 这样 ProjectionExecutor 能在 Aggregate 的输出中按名定位到对应字段
    std::vector<TabCol> proj_cols;
    for (auto& sel : sel_cols) {
      TabCol pc = sel;
      if (sel.agg != AGG_NONE) {
        pc.tab_name = "";
        pc.col_name = sel.alias;
      }
      proj_cols.push_back(pc);
    }
    auto top = std::make_shared<ProjectionPlan>(T_Projection, std::move(root),
                                                std::move(proj_cols));
    top->is_star_ = false;
    return top;
  }

  std::shared_ptr<Plan> plannerRoot = physical_optimization(query, context);

  // 投影下推：SELECT * 不下推
  if (!query->select_all) {
    std::set<TabCol> needed(sel_cols.begin(), sel_cols.end());
    plannerRoot = project_pushdown(plannerRoot, needed, false);
  }

  // LIMIT N（非聚合路径）
  if (query->limit >= 0) {
    plannerRoot =
        std::make_shared<LimitPlan>(std::move(plannerRoot), query->limit);
  }

  // 顶层 Projection（select_all 时 sel_cols_ 仍保留具体列，但打印为 *）
  auto top = std::make_shared<ProjectionPlan>(
      T_Projection, std::move(plannerRoot), std::move(sel_cols));
  top->is_star_ = query->select_all;
  plannerRoot = top;

  return plannerRoot;
}

// 生成DDL语句和DML语句的查询执行计划
std::shared_ptr<Plan> Planner::do_planner(std::shared_ptr<Query> query,
                                          Context* context) {
  std::shared_ptr<Plan> plannerRoot;
  if (auto x = std::dynamic_pointer_cast<ast::CreateTable>(query->parse)) {
    std::vector<ColDef> col_defs;
    for (auto& field : x->fields) {
      if (auto sv_col_def = std::dynamic_pointer_cast<ast::ColDef>(field)) {
        ColDef col_def = {.name = sv_col_def->col_name,
                          .type = interp_sv_type(sv_col_def->type_len->type),
                          .len = sv_col_def->type_len->len};
        col_defs.push_back(col_def);
      } else {
        throw InternalError("Unexpected field type");
      }
    }
    plannerRoot = std::make_shared<DDLPlan>(
        T_CreateTable, x->tab_name, std::vector<std::string>(), col_defs);
  } else if (auto x = std::dynamic_pointer_cast<ast::DropTable>(query->parse)) {
    plannerRoot = std::make_shared<DDLPlan>(T_DropTable, x->tab_name,
                                            std::vector<std::string>(),
                                            std::vector<ColDef>());
  } else if (auto x =
                 std::dynamic_pointer_cast<ast::CreateIndex>(query->parse)) {
    plannerRoot = std::make_shared<DDLPlan>(
        T_CreateIndex, x->tab_name, x->col_names, std::vector<ColDef>());
  } else if (auto x = std::dynamic_pointer_cast<ast::DropIndex>(query->parse)) {
    plannerRoot = std::make_shared<DDLPlan>(
        T_DropIndex, x->tab_name, x->col_names, std::vector<ColDef>());
  } else if (auto x =
                 std::dynamic_pointer_cast<ast::InsertStmt>(query->parse)) {
    plannerRoot = std::make_shared<DMLPlan>(
        T_Insert, std::shared_ptr<Plan>(), x->tab_name, query->values,
        std::vector<Condition>(), std::vector<SetClause>());
  } else if (auto x =
                 std::dynamic_pointer_cast<ast::DeleteStmt>(query->parse)) {
    std::shared_ptr<Plan> table_scan_executors;
    std::vector<std::string> index_col_names;
    bool index_exist =
        get_index_cols(x->tab_name, x->tab_name, query->conds, index_col_names);

    // MVCC 下 DELETE 可走 IndexScan：DELETE 不动堆和索引（见 executor_delete），
    // 扫描自身无 Halloween；IndexScan 内部走 mvcc_visible_record 选快照可见行，
    // 写写/陈旧写冲突检测在 DML 执行器按 rid 进行，与 SeqScan 路径一致。
    // 残余风险仅当并发事务更新"索引键列"（commit 清旧键→旧快照经索引漏行），
    // 因此只有在并发修改索引键列的场景下需要额外注意该风险。
    if (index_exist == false || force_seqscan_iso(context)) {
      index_col_names.clear();
      table_scan_executors = std::make_shared<ScanPlan>(
          T_SeqScan, sm_manager_, x->tab_name, query->conds, index_col_names);
    } else {
      table_scan_executors = std::make_shared<ScanPlan>(
          T_IndexScan, sm_manager_, x->tab_name, query->conds, index_col_names);
    }

    plannerRoot = std::make_shared<DMLPlan>(
        T_Delete, table_scan_executors, x->tab_name, std::vector<Value>(),
        query->conds, std::vector<SetClause>());
  } else if (auto x =
                 std::dynamic_pointer_cast<ast::UpdateStmt>(query->parse)) {
    std::shared_ptr<Plan> table_scan_executors;
    std::vector<std::string> index_col_names;
    bool index_exist =
        get_index_cols(x->tab_name, x->tab_name, query->conds, index_col_names);

    // UPDATE 走 IndexScan 需额外保证：SET 不修改所选索引的任何列。
    // executor_update 对键未变的索引不插新项（memcmp 相等即 continue），
    // 因此该护栏成立时扫描迭代器不会见到本语句新插的键（无 Halloween）。
    // 跨事务改键漏行的残余风险同上方 DELETE 分支说明。
    bool set_touches_index = false;
    if (index_exist) {
      for (auto& sc : query->set_clauses) {
        for (auto& icol : index_col_names) {
          if (sc.lhs.col_name == icol) {
            set_touches_index = true;
            break;
          }
        }
        if (set_touches_index) break;
      }
    }
    if (index_exist == false || set_touches_index ||
        force_seqscan_iso(context)) {
      index_col_names.clear();
      table_scan_executors = std::make_shared<ScanPlan>(
          T_SeqScan, sm_manager_, x->tab_name, query->conds, index_col_names);
    } else {
      table_scan_executors = std::make_shared<ScanPlan>(
          T_IndexScan, sm_manager_, x->tab_name, query->conds, index_col_names);
    }
    plannerRoot = std::make_shared<DMLPlan>(T_Update, table_scan_executors,
                                            x->tab_name, std::vector<Value>(),
                                            query->conds, query->set_clauses);
  } else if (auto x =
                 std::dynamic_pointer_cast<ast::SelectStmt>(query->parse)) {
    std::shared_ptr<Plan> projection = generate_select_plan(query, context);
    auto dml = std::make_shared<DMLPlan>(
        T_select, projection, std::string(), std::vector<Value>(),
        std::vector<Condition>(), std::vector<SetClause>());
    dml->is_explain_ = query->is_explain;
    plannerRoot = dml;
  } else {
    throw InternalError("Unexpected AST root");
  }
  return plannerRoot;
}
