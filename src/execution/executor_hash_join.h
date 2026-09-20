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

#include <cstdint>
#include <vector>

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "executor_seq_scan.h"  // 复用 check_all_conds
#include "system/sm.h"

// 等值连接的哈希连接。用途：连接列不在内表任何索引的最左前缀上时，
// planner 无法升级成 INLJ，朴素嵌套循环会退化成 |outer|×|inner|
// （实测 stock⋈order_line 为 10 万×1500 万，单条语句永不返回）。
//
// 形态：在右（内）侧建哈希表，左侧流式探测。右侧不是一次性全读进内存，而是
// 按内存预算分块：装满一块就把左侧完整探测一遍，再清空装下一块。因此
//   - 右侧能整块装下时 = 标准哈希连接，两侧各扫一遍；
//   - 装不下时自动退化为"分块哈希连接"，代价 = ceil(右侧字节/预算) × |左侧|，
//     仍远优于嵌套循环，且内存有硬上界，不会 OOM、不会抛异常。
// 左侧必须可重复 beginTuple()（所有扫描/连接执行器都支持，嵌套循环本来也这么用）。
class HashJoinExecutor : public AbstractExecutor {
 private:
  // 哈希表内存预算。只统计建表侧的元组字节 + 索引开销；调大可减少分块轮数、
  // 调小可压低峰值内存。改这里即可，不影响正确性，只影响扫描轮数。
  static constexpr size_t kBuildMemoryBudget = 256UL * 1024 * 1024;

  // 一对等值连接列在拼接后元组里的偏移（左侧列 / 右侧列），长度相同
  struct EquiKey {
    size_t left_offset;
    size_t right_offset;
    size_t len;
  };

  std::unique_ptr<AbstractExecutor> left_;
  std::unique_ptr<AbstractExecutor> right_;
  size_t len_;
  std::vector<ColMeta> cols_;
  std::vector<Condition> fed_conds_;
  std::vector<EquiKey> equi_;

  size_t left_len_ = 0;
  size_t right_len_ = 0;

  // 当前块：右侧元组连续存放 + 桶头/链表索引
  std::vector<char> build_buf_;
  std::vector<int32_t> bucket_head_;
  std::vector<int32_t> next_;
  uint64_t bucket_mask_ = 0;
  size_t build_rows_ = 0;
  size_t rows_per_block_ = 0;

  bool right_exhausted_ = false;  // 右侧已全部读完，无需再装块
  bool isend_ = false;
  int32_t probe_cursor_ = -1;     // 当前左行在本块中命中的候选链位置
  std::unique_ptr<RmRecord> left_rec_;
  std::unique_ptr<RmRecord> cur_;  // 当前已确认匹配的拼接结果

 public:
  HashJoinExecutor(std::unique_ptr<AbstractExecutor> left,
                   std::unique_ptr<AbstractExecutor> right,
                   std::vector<Condition> conds) {
    left_ = std::move(left);
    right_ = std::move(right);
    left_len_ = left_->tupleLen();
    right_len_ = right_->tupleLen();
    len_ = left_len_ + right_len_;
    cols_ = left_->cols();
    auto right_cols = right_->cols();
    for (auto& col : right_cols) {
      col.offset += left_len_;
    }
    cols_.insert(cols_.end(), right_cols.begin(), right_cols.end());
    fed_conds_ = std::move(conds);

    // 抽出可用于哈希的等值列对。只收 INT/CHAR：这两类"值相等 ⇔ 字节相等"，
    // 按字节哈希/比较安全。FLOAT 不收（-0.0 与 +0.0 值相等但字节不同，
    // 按字节分桶会漏行），它留在 fed_conds_ 里由 check_all_conds 兜底。
    for (auto& cond : fed_conds_) {
      if (cond.is_rhs_val || cond.op != OP_EQ) continue;
      const ColMeta* lhs = find_col(cond.lhs_col);
      const ColMeta* rhs = find_col(cond.rhs_col);
      if (lhs == nullptr || rhs == nullptr) continue;
      if (lhs->type != rhs->type || lhs->len != rhs->len) continue;
      if (lhs->type != TYPE_INT && lhs->type != TYPE_STRING) continue;
      // 一侧必须落在左元组、另一侧落在右元组，否则不是跨表等值
      bool lhs_left = static_cast<size_t>(lhs->offset) < left_len_;
      bool rhs_left = static_cast<size_t>(rhs->offset) < left_len_;
      if (lhs_left == rhs_left) continue;
      const ColMeta* l = lhs_left ? lhs : rhs;
      const ColMeta* r = lhs_left ? rhs : lhs;
      equi_.push_back({static_cast<size_t>(l->offset),
                       static_cast<size_t>(r->offset) - left_len_,
                       static_cast<size_t>(l->len)});
    }

    // 每块能放多少行：元组字节 + next_(4B) + 桶头摊销(约 8B/行)
    size_t per_row = right_len_ + sizeof(int32_t) + 8;
    rows_per_block_ = kBuildMemoryBudget / (per_row == 0 ? 1 : per_row);
    if (rows_per_block_ == 0) rows_per_block_ = 1;
  }

  const std::vector<ColMeta>& cols() const override { return cols_; }
  size_t tupleLen() const override { return len_; }
  std::string getType() override { return "HashJoinExecutor"; }
  bool is_end() const override { return isend_; }
  Rid& rid() override { return _abstract_rid; }

  AbstractExecutor* left_child() const override { return left_.get(); }
  AbstractExecutor* right_child() const override { return right_.get(); }

  // 是否真的能用哈希连接：至少要有一个可哈希的等值列对，否则退回调用方选嵌套循环。
  // 不可用时调用方用下面两个接口把子执行器取回，改建 NestedLoopJoinExecutor。
  bool usable() const { return !equi_.empty(); }
  std::unique_ptr<AbstractExecutor> release_left() { return std::move(left_); }
  std::unique_ptr<AbstractExecutor> release_right() { return std::move(right_); }

  void beginTuple() override {
    right_->beginTuple();
    if (!load_next_block()) {  // 右侧一行都没有 → 内连接结果为空
      isend_ = true;
      return;
    }
    if (!restart_probe()) {
      isend_ = true;
    }
  }

  void nextTuple() override {
    if (isend_) return;
    if (!advance()) {
      isend_ = true;
    }
  }

  std::unique_ptr<RmRecord> Next() override { return std::move(cur_); }

 private:
  const ColMeta* find_col(const TabCol& target) const {
    for (auto& c : cols_) {
      if (c.tab_name == target.tab_name && c.name == target.col_name) {
        return &c;
      }
    }
    return nullptr;
  }

  static uint64_t hash_bytes(const char* p, size_t n) {
    uint64_t h = 1469598103934665603ULL;  // FNV-1a
    for (size_t i = 0; i < n; ++i) {
      h ^= static_cast<unsigned char>(p[i]);
      h *= 1099511628211ULL;
    }
    return h;
  }

  uint64_t key_hash(const char* tuple, bool is_left) const {
    uint64_t h = 1469598103934665603ULL;
    for (auto& k : equi_) {
      size_t off = is_left ? k.left_offset : k.right_offset;
      h ^= hash_bytes(tuple + off, k.len);
      h *= 1099511628211ULL;
    }
    return h;
  }

  // 从右侧继续读，装满一块（或读完）。返回本块是否有行。
  bool load_next_block() {
    build_buf_.clear();
    next_.clear();
    build_rows_ = 0;
    if (right_exhausted_) return false;

    build_buf_.reserve(rows_per_block_ * right_len_);
    while (!right_->is_end() && build_rows_ < rows_per_block_) {
      auto rec = right_->Next();
      size_t base = build_buf_.size();
      build_buf_.resize(base + right_len_);
      memcpy(build_buf_.data() + base, rec->data, right_len_);
      ++build_rows_;
      right_->nextTuple();
    }
    if (right_->is_end()) right_exhausted_ = true;
    if (build_rows_ == 0) return false;

    size_t slots = 1;
    while (slots < build_rows_ * 2) slots <<= 1;
    bucket_mask_ = slots - 1;
    bucket_head_.assign(slots, -1);
    next_.assign(build_rows_, -1);
    for (size_t i = 0; i < build_rows_; ++i) {
      uint64_t h = key_hash(build_buf_.data() + i * right_len_, false);
      size_t b = static_cast<size_t>(h & bucket_mask_);
      next_[i] = bucket_head_[b];
      bucket_head_[b] = static_cast<int32_t>(i);
    }
    return true;
  }

  bool keys_equal(const char* left_tuple, const char* right_tuple) const {
    for (auto& k : equi_) {
      if (memcmp(left_tuple + k.left_offset, right_tuple + k.right_offset,
                 k.len) != 0) {
        return false;
      }
    }
    return true;
  }

  // 用本块从头探测左侧
  bool restart_probe() {
    left_->beginTuple();
    if (left_->is_end()) return next_block_or_end();
    left_rec_ = left_->Next();
    probe_cursor_ = bucket_head_[key_hash(left_rec_->data, true) & bucket_mask_];
    return seek_match();
  }

  // 本块对左侧探测完了：还有下一块就换块重探，否则整体结束
  bool next_block_or_end() {
    while (load_next_block()) {
      left_->beginTuple();
      if (left_->is_end()) continue;
      left_rec_ = left_->Next();
      probe_cursor_ =
          bucket_head_[key_hash(left_rec_->data, true) & bucket_mask_];
      if (seek_match()) return true;
    }
    return false;
  }

  bool advance() {
    if (probe_cursor_ >= 0) probe_cursor_ = next_[probe_cursor_];
    return seek_match();
  }

  // 沿候选链找下一条真正匹配的；链走完就推进左行；左侧走完就换块
  bool seek_match() {
    while (true) {
      while (probe_cursor_ >= 0) {
        const char* rrow = build_buf_.data() +
                           static_cast<size_t>(probe_cursor_) * right_len_;
        if (keys_equal(left_rec_->data, rrow)) {
          auto joined = std::make_unique<RmRecord>(static_cast<int>(len_));
          memcpy(joined->data, left_rec_->data, left_len_);
          memcpy(joined->data + left_len_, rrow, right_len_);
          // 残余条件（非等值、FLOAT 等值、跨表不等式）在拼接后统一兜底，
          // 保证与嵌套循环逐位同语义
          if (check_all_conds(*joined, cols_, fed_conds_)) {
            cur_ = std::move(joined);
            ++rows_out_;
            return true;
          }
        }
        probe_cursor_ = next_[probe_cursor_];
      }
      left_->nextTuple();
      if (left_->is_end()) return next_block_or_end();
      left_rec_ = left_->Next();
      probe_cursor_ =
          bucket_head_[key_hash(left_rec_->data, true) & bucket_mask_];
    }
  }
};
