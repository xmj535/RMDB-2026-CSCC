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

#include "execution_defs.h"
#include "execution_common.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "executor_seq_scan.h"  // check_all_conds
#include "index/ix.h"
#include "system/sm.h"

// 索引嵌套循环连接 (INLJ)：外侧任意算子驱动，内侧用唯一 / 普通索引做点查或前缀
// 查找。计划侧已经把内侧 ScanPlan 升级成 T_IndexScan 并填好 probe_bindings_，
// portal 在这里把 Project(IndexScan) 子树打包成 right_render_，仅用于
// EXPLAIN ANALYZE 渲染——真正的扫描动作由本算子自维护的 IxScan + RmFileHandle
// 完成，匹配命中时手动累加 right_render_/inner Scan 的 rows_out_。
class IndexJoinExecutor : public AbstractExecutor {
 private:
  std::unique_ptr<AbstractExecutor> left_;
  std::unique_ptr<AbstractExecutor> right_render_;
  AbstractExecutor* right_scan_stub_;  // = right_render_->child()，IndexScan 占位

  SmManager* sm_manager_;
  std::string inner_tab_name_;
  std::string inner_alias_;
  IndexMeta index_meta_;
  RmFileHandle* fh_;
  IxIndexHandle* ih_;

  // 内表完整行布局（tab_name 已改写为 alias），用于从 fh_ 读出的 RmRecord 取列
  std::vector<ColMeta> inner_full_cols_;
  size_t inner_full_len_;

  // 投影后的内侧列布局（= right_render_->cols()），决定连接输出宽度
  std::vector<ColMeta> inner_proj_cols_;
  size_t inner_proj_len_;
  // 每个投影列在完整内表行中的源偏移
  std::vector<size_t> proj_src_offsets_;

  // 连接输出：左 cols + 投影后内 cols
  std::vector<ColMeta> cols_;
  size_t len_;
  size_t left_len_;

  // 探测条件：每个绑定告诉我们「索引第 i 列的值从外行哪段字节拷贝」
  struct ProbeBinding {
    int idx_col_index;
    size_t outer_offset;
    int outer_len;
  };
  std::vector<ProbeBinding> bindings_;
  int probe_prefix_len_ = 0;

  // 所有 join 条件：兜底校验，覆盖非等值 / 未被探测前缀覆盖的等值条件
  std::vector<Condition> join_conds_;
  // 内表固定条件：既为复合 key 提供常量前缀，也保留阈值等残余过滤。
  std::vector<Condition> inner_conds_;

  // 运行时状态
  std::unique_ptr<RmRecord> left_rec_;
  std::unique_ptr<IxScan> ix_scan_;
  std::unique_ptr<RmRecord> cur_inner_full_;
  std::vector<char> stop_key_;
  std::vector<ColType> stop_types_;
  std::vector<int> stop_lens_;
  bool has_stop_key_ = false;
  bool isend_ = false;

 public:
  IndexJoinExecutor(std::unique_ptr<AbstractExecutor> left,
                    std::unique_ptr<AbstractExecutor> right_render,
                    SmManager* sm_manager, std::string inner_tab_name,
                    std::string inner_alias,
                    std::vector<std::string> index_col_names,
                    std::vector<std::pair<int, TabCol>> probe_bindings,
                    int probe_prefix_len, std::vector<Condition> inner_conds,
                    std::vector<Condition> join_conds, Context* context) {
    left_ = std::move(left);
    right_render_ = std::move(right_render);
    right_scan_stub_ = right_render_->child();
    sm_manager_ = sm_manager;
    inner_tab_name_ = std::move(inner_tab_name);
    inner_alias_ = std::move(inner_alias);
    probe_prefix_len_ = probe_prefix_len;
    inner_conds_ = std::move(inner_conds);
    join_conds_ = std::move(join_conds);
    context_ = context;

    TabMeta& tab = sm_manager_->db_.get_table(inner_tab_name_);
    index_meta_ = *(tab.get_index_meta(index_col_names));
    fh_ = sm_manager_->fhs_.at(inner_tab_name_).get();
    auto ix = sm_manager_->get_ix_manager();
    ih_ = sm_manager_->ihs_.at(ix->get_index_name(inner_tab_name_, index_meta_.cols))
              .get();

    // 内表完整行布局
    inner_full_cols_ = tab.cols;
    for (auto& c : inner_full_cols_) c.tab_name = inner_alias_;
    inner_full_len_ = inner_full_cols_.empty()
                          ? 0
                          : (inner_full_cols_.back().offset +
                             inner_full_cols_.back().len);

    // 投影列：直接拿 right_render_ 的 cols（ProjectionExecutor 已 reset offset）
    inner_proj_cols_ = right_render_->cols();
    inner_proj_len_ = inner_proj_cols_.empty()
                          ? 0
                          : (inner_proj_cols_.back().offset +
                             inner_proj_cols_.back().len);
    proj_src_offsets_.reserve(inner_proj_cols_.size());
    for (auto& pc : inner_proj_cols_) {
      size_t src_off = 0;
      bool found = false;
      for (auto& fc : inner_full_cols_) {
        if (fc.tab_name == pc.tab_name && fc.name == pc.name) {
          src_off = fc.offset;
          found = true;
          break;
        }
      }
      if (!found) {
        throw InternalError("INLJ: projected inner col not in inner row");
      }
      proj_src_offsets_.push_back(src_off);
    }

    // 输出列：left cols 原样 + 投影内 cols 偏移加 left_len
    auto left_cols = left_->cols();
    left_len_ = left_->tupleLen();
    cols_ = left_cols;
    for (auto& c : inner_proj_cols_) {
      ColMeta nc = c;
      nc.offset += left_len_;
      cols_.push_back(nc);
    }
    len_ = left_len_ + inner_proj_len_;

    // 解析探测绑定的外侧列偏移
    for (auto& [idx, tc] : probe_bindings) {
      bool found = false;
      for (auto& lc : left_cols) {
        if (lc.tab_name == tc.tab_name && lc.name == tc.col_name) {
          bindings_.push_back({idx, (size_t)lc.offset, lc.len});
          found = true;
          break;
        }
      }
      if (!found) {
        throw InternalError("INLJ: probe outer col not in left tuple");
      }
    }
  }

  const std::vector<ColMeta>& cols() const override { return cols_; }
  size_t tupleLen() const override { return len_; }
  std::string getType() override { return "IndexJoinExecutor"; }
  bool is_end() const override { return isend_; }
  Rid& rid() override { return _abstract_rid; }

  AbstractExecutor* left_child() const override { return left_.get(); }
  AbstractExecutor* right_child() const override { return right_render_.get(); }

  void beginTuple() override {
    left_->beginTuple();
    if (left_->is_end()) {
      isend_ = true;
      return;
    }
    left_rec_ = left_->Next();
    open_probe();
    seek_next_match();
  }

  void nextTuple() override {
    if (isend_) return;
    if (ix_scan_ != nullptr && !ix_scan_->is_end()) {
      ix_scan_->next();
    }
    seek_next_match();
  }

  std::unique_ptr<RmRecord> Next() override {
    auto out = std::make_unique<RmRecord>((int)len_);
    memcpy(out->data, left_rec_->data, left_len_);
    char* dst = out->data + left_len_;
    for (size_t i = 0; i < inner_proj_cols_.size(); ++i) {
      memcpy(dst + inner_proj_cols_[i].offset,
             cur_inner_full_->data + proj_src_offsets_[i],
             inner_proj_cols_[i].len);
    }
    return out;
  }

 private:
  void open_probe() {
    std::vector<char> probe_key(index_meta_.col_tot_len, 0);
    size_t key_off = 0;
    for (int idx = 0; idx < probe_prefix_len_; ++idx) {
      const auto& index_col = index_meta_.cols[idx];
      const ProbeBinding* outer_binding = nullptr;
      for (auto& binding : bindings_) {
        if (binding.idx_col_index == idx) {
          outer_binding = &binding;
          break;
        }
      }
      if (outer_binding != nullptr) {
        if (outer_binding->outer_len != index_col.len) {
          throw InternalError("INLJ: probe column width mismatch");
        }
        memcpy(probe_key.data() + key_off,
               left_rec_->data + outer_binding->outer_offset, index_col.len);
      } else {
        const Condition* fixed = nullptr;
        for (auto& condition : inner_conds_) {
          if (condition.is_rhs_val && condition.op == OP_EQ &&
              condition.lhs_col.tab_name == inner_alias_ &&
              condition.lhs_col.col_name == index_col.name) {
            fixed = &condition;
            break;
          }
        }
        if (fixed == nullptr || fixed->rhs_val.raw == nullptr ||
            fixed->rhs_val.raw->size != index_col.len) {
          throw InternalError("INLJ: fixed probe prefix is not bound");
        }
        memcpy(probe_key.data() + key_off, fixed->rhs_val.raw->data,
               index_col.len);
      }
      key_off += index_col.len;
    }

    stop_key_ = probe_key;
    stop_types_.clear();
    stop_lens_.clear();
    for (int i = 0; i < probe_prefix_len_; ++i) {
      stop_types_.push_back(index_meta_.cols[i].type);
      stop_lens_.push_back(index_meta_.cols[i].len);
    }
    has_stop_key_ = probe_prefix_len_ > 0;

    Iid lower = ih_->lower_bound(probe_key.data());
    ix_scan_ = std::make_unique<IxScan>(ih_, lower, ih_->leaf_end(),
                                        sm_manager_->get_bpm());
  }

  bool past_stop_key() {
    if (!has_stop_key_) return false;
    std::vector<char> cur_key(index_meta_.col_tot_len);
    ix_scan_->copy_current_key(cur_key.data());
    int cmp = ix_compare(cur_key.data(), stop_key_.data(), stop_types_,
                         stop_lens_);
    return cmp > 0;
  }

  // 拼接 (left_rec_, full_inner_rec) 后跑 join_conds_ 兜底校验
  bool joined_passes(const RmRecord& inner_full) {
    if (!check_all_conds(inner_full, inner_full_cols_, inner_conds_)) {
      return false;
    }
    if (join_conds_.empty()) return true;
    RmRecord joined((int)(left_len_ + inner_full_len_));
    memcpy(joined.data, left_rec_->data, left_len_);
    memcpy(joined.data + left_len_, inner_full.data, inner_full_len_);
    std::vector<ColMeta> check_cols = left_->cols();
    for (auto& c : inner_full_cols_) {
      ColMeta nc = c;
      nc.offset += left_len_;
      check_cols.push_back(nc);
    }
    return check_all_conds(joined, check_cols, join_conds_);
  }

  void seek_next_match() {
    while (true) {
      while (ix_scan_ != nullptr && !ix_scan_->is_end()) {
        if (past_stop_key()) {
          // 直接弃用本次探测迭代器，下一次循环推进左侧；不能用 next()
          // 排空——那会把内表索引剩余部分整个走一遍，探测退化成 O(索引大小)
          ix_scan_.reset();
          break;
        }
        Rid rid = ix_scan_->rid();
        auto inner_rec =
            mvcc_visible_record(context_, inner_tab_name_, fh_, rid);
        if (inner_rec == nullptr) {
          ix_scan_->next();
          continue;
        }
        if (joined_passes(*inner_rec)) {
          cur_inner_full_ = std::move(inner_rec);
          ++rows_out_;
          ++right_render_->rows_out_;
          if (right_scan_stub_ != nullptr) ++right_scan_stub_->rows_out_;
          return;
        }
        ix_scan_->next();
      }
      // 内侧扫完：推进外侧
      left_->nextTuple();
      if (left_->is_end()) {
        isend_ = true;
        return;
      }
      left_rec_ = left_->Next();
      open_probe();
    }
  }
};
