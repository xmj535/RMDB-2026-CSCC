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

#include <limits>

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "executor_seq_scan.h"  // 复用 check_all_conds
#include "index/ix.h"
#include "record/rm_scan.h"
#include "system/sm.h"

// 利用 (id, name, score) 等索引的最左前缀做范围扫描，剩余条件落到记录上再过滤
class IndexScanExecutor : public AbstractExecutor {
 private:
  std::string tab_name_;
  // 显示名：无别名时等于 tab_name_。谓词/投影里的列都按显示名标注，而
  // 元数据、文件句柄、索引句柄、SSI 读跟踪一律用真实表名 tab_name_。
  std::string alias_;
  TabMeta tab_;
  std::vector<Condition> conds_;
  RmFileHandle* fh_;
  std::vector<ColMeta> cols_;
  size_t len_;
  std::vector<Condition> fed_conds_;
  std::vector<Condition> ssi_conds_;  // SER 读跟踪谓词(见 set_ssi_conds)

  std::vector<std::string> index_col_names_;
  IndexMeta index_meta_;
  IxIndexHandle* ih_;

  Rid rid_;
  std::unique_ptr<IxScan> scan_;
  SmManager* sm_manager_;

  // past_stop_key 每行复用的键缓冲，避免逐行堆分配
  std::vector<char> cur_key_buf_;

  // 用于扫到范围之外时提前停止
  std::vector<char> stop_key_;
  bool has_stop_key_ = false;
  bool stop_inclusive_ = false;
  std::vector<ColType> stop_types_;
  std::vector<int> stop_lens_;

  // 缓存 advance_until_match 里已经取出的当前行，避免 Next() 再 fetch 一遍
  std::unique_ptr<RmRecord> cur_rec_;

  // 越过 stop_key 后直接置 true 终止扫描，避免为了让底层迭代器 is_end()
  // 而把剩余整个索引 next() 走一遍（点查会退化成 O(索引大小)）
  bool done_ = false;

  // ---- 跳跃扫描（skip scan）状态 ----
  //
  // 谓词绑住了索引的第 2..k 列却没绑首列时，最左前缀匹配不上，计划只能退化成
  // 全表顺序扫描。TPC-C Delivery 的
  //   select sum(ol_amount) from order_line where ol_d_id=? and ol_o_id=?
  // 就是这个形状：索引是 (ol_w_id,ol_d_id,ol_o_id,ol_number)，ol_w_id 没绑，
  // 于是扫满 1500 万行（本机实测 1395.6 ms，绑上 ol_w_id 只要 0.115 ms）。
  //
  // 做法：把首列的**不同取值**从索引本身枚举出来，每个取值做一次正常的等值前缀
  // 区间扫描。首列基数低时（ol_w_id 只有 50 个仓库）总代价是 D 次索引下降，
  // 而不是 N 行。思路对应 Oracle / MySQL 8.0 的 index skip scan（loose index scan）。
  //
  // 只支持**恰好一列**未绑定且该列为 INT：后继值就是 v+1，无需按类型推导后继，
  // 也不必处理多列进位。多列/字符串跳跃留给后续轮次，风险不对等。
  // planner 显式授权后才允许进入跳跃扫描。执行器只看得到 conds_，而 INLJ 内表的
  // 索引首列是被【连接条件】绑定的（is_rhs_val=false，运行时才由外行喂值），
  // 从条件里看去与"首列完全无谓词"无法区分——自行推断会把内表点查变成扫遍首列
  // 全部取值的跳跃扫描。判定权归 planner。
  bool skip_scan_allowed_ = false;
  int skip_cols_ = 0;             // >0 表示启用；当前实现恒为 1
  int skip_probes_ = 0;           // 已做的不同取值探测次数
  int next_skip_value_ = 0;       // 下一次探测的首列下界
  bool skip_exhausted_ = false;   // 首列已枚举完（或已降级）
  std::vector<char> skip_eq_key_;  // 首列之后那段等值列的键模板
  int skip_eq_cols_ = 0;          // 首列之后连续的等值列数

  // 失控闸门。首列基数极高时，每个取值一次索引下降会比顺序扫描更贵。超过这个
  // 上限就**降级为从当前位置一路扫到索引末尾**（不再下降、不设 stop key），
  // 结果仍然正确——两条路径都按索引序访问匹配键的超集，再由 fed_conds_ 过滤。
  // TPC-C 里首列是 w_id（50 个仓库），永远够不到这个数。
  static constexpr int kMaxSkipProbes = 4096;

 public:
  IndexScanExecutor(SmManager* sm_manager, std::string tab_name,
                    std::vector<Condition> conds,
                    std::vector<std::string> index_col_names,
                    Context* context)
      : IndexScanExecutor(sm_manager, std::move(tab_name), std::string(),
                          std::move(conds), std::move(index_col_names),
                          context) {}

  // alias 为空时退回 tab_name；cols_ 的 tab_name 改写为 alias，
  // 与 SeqScanExecutor 对齐，使带别名的查询也能走索引扫描。
  IndexScanExecutor(SmManager* sm_manager, std::string tab_name,
                    std::string alias, std::vector<Condition> conds,
                    std::vector<std::string> index_col_names,
                    Context* context) {
    sm_manager_ = sm_manager;
    context_ = context;
    tab_name_ = std::move(tab_name);
    alias_ = alias.empty() ? tab_name_ : std::move(alias);
    tab_ = sm_manager_->db_.get_table(tab_name_);
    conds_ = std::move(conds);
    index_col_names_ = std::move(index_col_names);
    index_meta_ = *(tab_.get_index_meta(index_col_names_));
    fh_ = sm_manager_->fhs_.at(tab_name_).get();
    ssi_conds_ = conds_;
    cols_ = tab_.cols;
    for (auto& c : cols_) c.tab_name = alias_;
    len_ = cols_.empty() ? 0 : (cols_.back().offset + cols_.back().len);

    auto ix = sm_manager_->get_ix_manager();
    ih_ = sm_manager_->ihs_.at(ix->get_index_name(tab_name_, index_meta_.cols))
              .get();

    // lhs 必须落在本表上；条件反着写时交换两边并翻比较符
    static const std::map<CompOp, CompOp> swap_op = {
        {OP_EQ, OP_EQ}, {OP_NE, OP_NE}, {OP_LT, OP_GT},
        {OP_GT, OP_LT}, {OP_LE, OP_GE}, {OP_GE, OP_LE},
    };
    for (auto& cond : conds_) {
      if (cond.lhs_col.tab_name != alias_) {
        assert(!cond.is_rhs_val && cond.rhs_col.tab_name == alias_);
        std::swap(cond.lhs_col, cond.rhs_col);
        cond.op = swap_op.at(cond.op);
      }
    }
    fed_conds_ = conds_;
  }

  const std::vector<ColMeta>& cols() const override { return cols_; }
  size_t tupleLen() const override { return len_; }
  // SER 读跟踪用的本表谓词(语义同 SeqScanExecutor::set_ssi_conds)
  void set_ssi_conds(std::vector<Condition> conds) {
    ssi_conds_ = std::move(conds);
  }

  // 由 planner 经 ScanPlan::use_skip_scan_ 授权，见该成员处的说明
  void set_skip_scan_allowed(bool allowed) { skip_scan_allowed_ = allowed; }

  std::string getType() override { return "IndexScanExecutor"; }
  bool is_end() const override {
    if (scan_ == nullptr || done_) return true;
    // 跳跃扫描里 scan_ 是**某一个首列取值**的子扫描，它结束只代表该取值扫完，
    // 不代表整趟结束。advance_until_match 保证取不出行时一定置 done_。
    if (skip_cols_ > 0) return false;
    return scan_->is_end();
  }
  Rid& rid() override { return rid_; }

  ColMeta get_col_offset(const TabCol& target) override {
    for (auto& c : cols_) {
      if (c.tab_name == target.tab_name && c.name == target.col_name) {
        return c;
      }
    }
    throw ColumnNotFoundError(target.tab_name + "." + target.col_name);
  }

  void beginTuple() override {
    // SER：登记本次扫描的谓词，让其他事务的写入可以做 phantom 比较；
    // 与 SeqScan 行为对齐，避免索引路径漏报范围/空结果读。
    if (context_ != nullptr && context_->txn_ != nullptr &&
        context_->txn_mgr_ != nullptr &&
        context_->txn_->get_isolation_level() ==
            IsolationLevel::SERIALIZABLE) {
      context_->txn_mgr_->SsiRecordPredicate(context_->txn_, tab_name_,
                                              ssi_conds_, cols_);
    }
    // 首列没有任何谓词、而后面有连续等值列时走跳跃扫描（判定与 planner 的
    // get_index_cols 同源：都只看 conds_ 与 index_meta_，不额外传计划字段，
    // 两侧不可能算出不同结论）
    if (detect_skip_prefix()) {
      skip_probes_ = 0;
      skip_exhausted_ = false;
      next_skip_value_ = std::numeric_limits<int>::min();
      if (!start_next_skip_range()) {
        done_ = true;
        // scan_ 必须非空：is_end() 用 scan_==nullptr 也算结束，但 Next() 之前
        // 的调用方会先问 is_end()，这里给一个空区间保持不变式简单
        scan_ = std::make_unique<IxScan>(ih_, ih_->leaf_end(), ih_->leaf_end(),
                                         sm_manager_->get_bpm());
        return;
      }
      advance_until_match();
      return;
    }

    // 收集每个索引列上能用到的谓词，按"最左 EQ 前缀 + 可选范围列"展开
    std::vector<char> low_key(index_meta_.col_tot_len, 0);
    std::vector<char> high_key(index_meta_.col_tot_len, 0);
    int prefix_eq_cols = 0;  // 连续的 EQ 列数
    int prefix_eq_bytes = 0;
    bool has_range_col = false;   // 范围列存在
    bool has_range_low = false;   // 范围列上有下界谓词
    bool has_range_high = false;  // 范围列上有上界谓词
    bool low_inclusive = true;
    bool high_inclusive = true;

    for (auto& idx_col : index_meta_.cols) {
      const Condition* eq = nullptr;
      const Condition* low_c = nullptr;
      const Condition* high_c = nullptr;
      for (auto& cond : conds_) {
        if (!cond.is_rhs_val) continue;
        if (cond.lhs_col.tab_name != alias_) continue;
        if (cond.lhs_col.col_name != idx_col.name) continue;
        if (cond.op == OP_EQ) {
          eq = &cond;
          break;
        }
        if (cond.op == OP_GT || cond.op == OP_GE) {
          if (low_c == nullptr) low_c = &cond;
        } else if (cond.op == OP_LT || cond.op == OP_LE) {
          if (high_c == nullptr) high_c = &cond;
        }
      }

      if (eq != nullptr) {
        memcpy(low_key.data() + prefix_eq_bytes, eq->rhs_val.raw->data,
               idx_col.len);
        memcpy(high_key.data() + prefix_eq_bytes, eq->rhs_val.raw->data,
               idx_col.len);
        prefix_eq_bytes += idx_col.len;
        prefix_eq_cols += 1;
        continue;
      }

      if (low_c != nullptr) {
        memcpy(low_key.data() + prefix_eq_bytes, low_c->rhs_val.raw->data,
               idx_col.len);
        if (low_c->op == OP_GT) low_inclusive = false;
        has_range_low = true;
        has_range_col = true;
      }
      if (high_c != nullptr) {
        memcpy(high_key.data() + prefix_eq_bytes, high_c->rhs_val.raw->data,
               idx_col.len);
        if (high_c->op == OP_LT) high_inclusive = false;
        has_range_high = true;
        has_range_col = true;
      }
      // 不论是否有范围谓词，遇到非 EQ 就停止前缀匹配
      break;
    }

    // 起始位置：EQ 前缀 + 可选下界
    Iid lower;
    if (has_range_low) {
      // EQ 前缀已经填好，再加上范围列下界。范围列若是严格
      // >，跳过等于该值的所有键
      lower = low_inclusive ? ih_->lower_bound(low_key.data())
                            : ih_->upper_bound(low_key.data());
    } else if (prefix_eq_cols > 0) {
      // 只有 EQ 前缀，定位到第一个 prefix 等于该值的 key
      lower = ih_->lower_bound(low_key.data());
    } else {
      lower = ih_->leaf_begin();
    }

    // 停止条件：以 EQ 前缀 + 可选范围列上界 为界
    if (has_range_high) {
      stop_key_ = high_key;
      stop_inclusive_ = high_inclusive;
      for (int i = 0; i < prefix_eq_cols + 1; ++i) {
        stop_types_.push_back(index_meta_.cols[i].type);
        stop_lens_.push_back(index_meta_.cols[i].len);
      }
      has_stop_key_ = true;
    } else if (prefix_eq_cols > 0 && !has_range_col) {
      // 纯 EQ 前缀：扫到 prefix 比 EQ 值大就停
      stop_key_ = low_key;  // 此时 low_key == high_key 在前缀部分
      stop_inclusive_ = true;
      for (int i = 0; i < prefix_eq_cols; ++i) {
        stop_types_.push_back(index_meta_.cols[i].type);
        stop_lens_.push_back(index_meta_.cols[i].len);
      }
      has_stop_key_ = true;
    } else if (prefix_eq_cols > 0 && has_range_col && !has_range_high) {
      // EQ 前缀 + 范围只有下界：扫到 prefix 比 EQ 值大就停，范围列上不设上界
      stop_key_ = low_key;
      stop_inclusive_ = true;
      for (int i = 0; i < prefix_eq_cols; ++i) {
        stop_types_.push_back(index_meta_.cols[i].type);
        stop_lens_.push_back(index_meta_.cols[i].len);
      }
      has_stop_key_ = true;
    }
    // 其它情况（无 EQ 前缀且仅有下界 / 完全没条件）扫到 leaf_end

    scan_ = std::make_unique<IxScan>(ih_, lower, ih_->leaf_end(),
                                     sm_manager_->get_bpm());
    advance_until_match();
  }

  void nextTuple() override {
    if (scan_ == nullptr || done_ || scan_->is_end()) {
      return;
    }
    scan_->next();
    advance_until_match();
  }

  std::unique_ptr<RmRecord> Next() override {
    // advance_until_match 已经把当前行 fetch 出来了，直接搬走，避免再读一次堆
    return std::move(cur_rec_);
  }

 private:
  // 该索引列上是否有本表的等值谓词
  const Condition* eq_cond_for(const std::string& col_name) const {
    for (auto& cond : conds_) {
      if (!cond.is_rhs_val) continue;
      if (cond.lhs_col.tab_name != alias_) continue;
      if (cond.lhs_col.col_name != col_name) continue;
      if (cond.op == OP_EQ) return &cond;
    }
    return nullptr;
  }

  // 该索引列上是否有**任何**本表谓词（等值或范围）。首列只要沾上一点谓词就
  // 说明普通最左前缀路径能用，不该走跳跃扫描。
  bool has_any_cond(const std::string& col_name) const {
    for (auto& cond : conds_) {
      if (!cond.is_rhs_val) continue;
      if (cond.lhs_col.tab_name != alias_) continue;
      if (cond.lhs_col.col_name == col_name) return true;
    }
    return false;
  }

  // 判定是否为跳跃扫描形状：首列 INT 且完全无谓词，第二列起有连续等值列。
  bool detect_skip_prefix() {
    skip_cols_ = 0;
    skip_eq_cols_ = 0;
    if (!skip_scan_allowed_) return false;
    const auto& cols = index_meta_.cols;
    if (cols.size() < 2) return false;
    if (cols[0].type != TYPE_INT) return false;
    if (cols[0].len != static_cast<int>(sizeof(int))) return false;
    if (has_any_cond(cols[0].name)) return false;
    // 首列之后必须紧跟至少一个等值列，否则跳过首列也没有可用的区间
    int eq_run = 0;
    for (size_t i = 1; i < cols.size(); ++i) {
      if (eq_cond_for(cols[i].name) == nullptr) break;
      ++eq_run;
    }
    if (eq_run == 0) return false;
    skip_cols_ = 1;
    skip_eq_cols_ = eq_run;
    // 键模板：首列留空（每轮填当前取值），其后按自然偏移填等值
    skip_eq_key_.assign(index_meta_.col_tot_len, 0);
    int offset = cols[0].len;
    for (int i = 1; i <= eq_run; ++i) {
      const Condition* eq = eq_cond_for(cols[i].name);
      memcpy(skip_eq_key_.data() + offset, eq->rhs_val.raw->data, cols[i].len);
      offset += cols[i].len;
    }
    return true;
  }

  // 定位首列的下一个不同取值，并把 scan_ 摆到该取值对应的等值区间上。
  // 返回 false 表示首列已经枚举完，整趟扫描结束。
  bool start_next_skip_range() {
    if (skip_exhausted_) return false;
    auto* bpm = sm_manager_->get_bpm();
    const auto& cols = index_meta_.cols;

    // 第一次下降：找出 >= next_skip_value_ 的第一个键，读出它的首列取值
    std::vector<char> probe(index_meta_.col_tot_len, 0);
    memcpy(probe.data(), &next_skip_value_, sizeof(int));
    IxScan probe_scan(ih_, ih_->lower_bound(probe.data()), ih_->leaf_end(), bpm);
    if (probe_scan.is_end()) {
      skip_exhausted_ = true;
      return false;
    }
    if (cur_key_buf_.size() != static_cast<size_t>(index_meta_.col_tot_len)) {
      cur_key_buf_.resize(index_meta_.col_tot_len);
    }
    probe_scan.copy_current_key(cur_key_buf_.data());
    // copy_current_key 在取不到页/slot 越界时会把迭代器判为结束，必须复查
    if (probe_scan.is_end()) {
      skip_exhausted_ = true;
      return false;
    }
    int value = 0;
    memcpy(&value, cur_key_buf_.data(), sizeof(int));

    if (++skip_probes_ > kMaxSkipProbes) {
      // 降级：首列基数太高，继续按取值下降只会更慢。从当前位置一路扫到索引
      // 末尾，不设 stop key，全部交给 fed_conds_ 过滤——慢，但结果正确。
      has_stop_key_ = false;
      stop_types_.clear();
      stop_lens_.clear();
      skip_exhausted_ = true;
      scan_ = std::make_unique<IxScan>(ih_, probe_scan.iid(), ih_->leaf_end(),
                                       bpm);
      return true;
    }

    // 下一轮从 value+1 起。首列已经取到 INT_MAX 时没有后继，本轮是最后一轮。
    if (value == std::numeric_limits<int>::max()) {
      skip_exhausted_ = true;
    } else {
      next_skip_value_ = value + 1;
    }

    // 第二次下降：定位 (value, eq...) 这个等值区间的起点，停止条件同样按
    // 「首列 + 等值列」比较，越界即止
    std::vector<char> low_key = skip_eq_key_;
    memcpy(low_key.data(), &value, sizeof(int));
    stop_key_ = low_key;
    stop_inclusive_ = true;
    stop_types_.clear();
    stop_lens_.clear();
    for (int i = 0; i < skip_cols_ + skip_eq_cols_; ++i) {
      stop_types_.push_back(cols[i].type);
      stop_lens_.push_back(cols[i].len);
    }
    has_stop_key_ = true;
    scan_ = std::make_unique<IxScan>(ih_, ih_->lower_bound(low_key.data()),
                                     ih_->leaf_end(), bpm);
    return true;
  }

  // 判断当前 iid 对应的 key 是否已经越过 stop_key_
  bool past_stop_key() {
    if (!has_stop_key_) return false;
    // 复用成员缓冲：本函数每扫一行调用一次，这里原本每行 new/delete 一个
    // vector（500 个分区查询合计约 1500 万次分配），是索引扫描每行成本
    // 高于顺序扫描的主要来源之一。
    if (cur_key_buf_.size() !=
        static_cast<size_t>(index_meta_.col_tot_len)) {
      cur_key_buf_.resize(index_meta_.col_tot_len);
    }
    scan_->copy_current_key(cur_key_buf_.data());
    int cmp = ix_compare(cur_key_buf_.data(), stop_key_.data(), stop_types_,
                         stop_lens_);
    if (stop_inclusive_) return cmp > 0;
    return cmp >= 0;
  }

  void advance_until_match() {
    cur_rec_.reset();
    const bool ser = (context_ != nullptr && context_->txn_ != nullptr &&
                      context_->txn_mgr_ != nullptr &&
                      context_->txn_->get_isolation_level() ==
                          IsolationLevel::SERIALIZABLE);
    // 外层循环负责跳跃扫描换首列取值。写成循环而不是递归：探测上限 4096，
    // 递归会压 4096 层栈。非跳跃模式下外层只走一遍。
    while (true) {
      while (!scan_->is_end()) {
        if (past_stop_key()) break;
        // past_stop_key() 内部的 copy_current_key 可能因取不到页/slot 越界把迭代器
        // 判定为结束。它比 rid() 先执行，若不在这里复查，下面的 rid() 会正好在被加固
        // 的那条路径上抛 IndexEntryNotFoundError。
        if (scan_->is_end()) break;
        rid_ = scan_->rid();
        auto rec = mvcc_visible_record(context_, tab_name_, fh_, rid_);
        if (rec == nullptr) {
          if (ser) {
            context_->txn_mgr_->SsiCheckInvisibleWriters(
                context_->txn_, tab_name_, rid_, ssi_conds_, cols_);
          }
          scan_->next();
          continue;
        }
        if (ser) {
          if (ssi_conds_.empty() || check_all_conds(*rec, cols_, ssi_conds_)) {
            context_->txn_mgr_->SsiRecordRead(context_->txn_, tab_name_, rid_);
          } else {
            context_->txn_mgr_->SsiCheckInvisibleWriters(
                context_->txn_, tab_name_, rid_, ssi_conds_, cols_);
          }
        }
        if (check_all_conds(*rec, cols_, fed_conds_)) {
          cur_rec_ = std::move(rec);
          return;
        }
        scan_->next();
      }
      // 本段扫完（撞上 stop key 或走到索引末尾）。跳跃扫描换下一个首列取值，
      // 普通扫描到此为止。
      if (skip_cols_ > 0 && start_next_skip_range()) continue;
      done_ = true;
      return;
    }
  }
};
