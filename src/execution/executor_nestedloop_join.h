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
#include "execution_manager.h"
#include "executor_abstract.h"
#include "executor_seq_scan.h"  // 复用 check_all_conds
#include "index/ix.h"
#include "system/sm.h"

class NestedLoopJoinExecutor : public AbstractExecutor {
 private:
  std::unique_ptr<AbstractExecutor> left_;
  std::unique_ptr<AbstractExecutor> right_;
  size_t len_;
  std::vector<ColMeta> cols_;

  std::vector<Condition> fed_conds_;
  bool isend_ = false;

  // 缓存当前 left 行，右侧迭代时反复使用
  std::unique_ptr<RmRecord> left_rec_;

  // ---- 分块模式（blocked_ 为真时启用）------------------------------------
  // 朴素嵌套循环对每个外行重扫一遍内表，代价 |outer|×|inner|，大表下无界。
  // 分块模式改为：缓存一批外行，内表每扫一遍就与整批比对，
  // 于是内表扫描次数从 |outer| 降到 ceil(|outer|/块行数)。
  // 语义完全相同（同样的配对集合），只是产出顺序不同（行序本就不作要求）。
  // 仅在 planner 判断朴素代价过大时开启，小表连接保持原路径与 EXPLAIN 计数。
  static constexpr size_t kBlockMemoryBudget = 256UL * 1024 * 1024;
  bool blocked_ = false;
  std::vector<char> block_;       // 连续存放的外行
  size_t block_rows_ = 0;         // 本块外行数
  size_t block_capacity_ = 0;     // 每块最多几行
  size_t bi_ = 0;                 // 当前右行已与块内第几行比对过
  bool left_done_ = false;
  std::unique_ptr<RmRecord> cur_right_;
  std::unique_ptr<RmRecord> cur_;

 public:
  NestedLoopJoinExecutor(std::unique_ptr<AbstractExecutor> left,
                         std::unique_ptr<AbstractExecutor> right,
                         std::vector<Condition> conds, bool blocked = false) {
    blocked_ = blocked;
    left_ = std::move(left);
    right_ = std::move(right);
    len_ = left_->tupleLen() + right_->tupleLen();
    cols_ = left_->cols();
    auto right_cols = right_->cols();
    for (auto& col : right_cols) {
      col.offset += left_->tupleLen();
    }
    cols_.insert(cols_.end(), right_cols.begin(), right_cols.end());
    fed_conds_ = std::move(conds);

    size_t lw = left_->tupleLen();
    block_capacity_ = kBlockMemoryBudget / (lw == 0 ? 1 : lw);
    if (block_capacity_ == 0) block_capacity_ = 1;
  }

  const std::vector<ColMeta>& cols() const override { return cols_; }
  size_t tupleLen() const override { return len_; }
  std::string getType() override { return "NestedLoopJoinExecutor"; }
  bool is_end() const override { return isend_; }
  Rid& rid() override { return _abstract_rid; }

  AbstractExecutor* left_child() const override { return left_.get(); }
  AbstractExecutor* right_child() const override { return right_.get(); }

  void beginTuple() override {
    if (blocked_) {
      left_->beginTuple();
      if (!load_block()) {
        isend_ = true;
        return;
      }
      right_->beginTuple();
      bi_ = 0;
      if (!blocked_seek()) isend_ = true;
      return;
    }
    left_->beginTuple();
    if (left_->is_end()) {
      isend_ = true;
      return;
    }
    left_rec_ = left_->Next();
    right_->beginTuple();
    seek_next_match();
  }

  void nextTuple() override {
    if (isend_) {
      return;
    }
    if (blocked_) {
      if (!blocked_seek()) isend_ = true;
      return;
    }
    right_->nextTuple();
    seek_next_match();
  }

  // 拼接当前 (left_rec_, right 当前行)
  std::unique_ptr<RmRecord> Next() override {
    if (blocked_) return std::move(cur_);
    auto right_rec = right_->Next();
    auto out = std::make_unique<RmRecord>((int)len_);
    memcpy(out->data, left_rec_->data, left_->tupleLen());
    memcpy(out->data + left_->tupleLen(), right_rec->data, right_->tupleLen());
    return out;
  }

 private:
  // 从左侧继续读满一块（或读完）。返回本块是否有行。
  bool load_block() {
    block_rows_ = 0;
    block_.clear();
    if (left_done_) return false;
    const size_t lw = left_->tupleLen();
    block_.reserve(block_capacity_ * lw);
    while (!left_->is_end() && block_rows_ < block_capacity_) {
      auto rec = left_->Next();
      size_t base = block_.size();
      block_.resize(base + lw);
      memcpy(block_.data() + base, rec->data, lw);
      ++block_rows_;
      left_->nextTuple();
    }
    if (left_->is_end()) left_done_ = true;
    return block_rows_ > 0;
  }

  // 分块模式的推进：对当前右行遍历块内剩余外行；块内走完就取下一右行；
  // 右侧走完就换下一块并重扫右侧。
  bool blocked_seek() {
    const size_t lw = left_->tupleLen();
    while (true) {
      while (!right_->is_end()) {
        if (cur_right_ == nullptr) cur_right_ = right_->Next();
        while (bi_ < block_rows_) {
          auto joined = std::make_unique<RmRecord>((int)len_);
          memcpy(joined->data, block_.data() + bi_ * lw, lw);
          memcpy(joined->data + lw, cur_right_->data, right_->tupleLen());
          ++bi_;
          if (check_all_conds(*joined, cols_, fed_conds_)) {
            cur_ = std::move(joined);
            ++rows_out_;
            return true;
          }
        }
        bi_ = 0;
        cur_right_.reset();
        right_->nextTuple();
      }
      if (!load_block()) return false;
      right_->beginTuple();
      bi_ = 0;
      cur_right_.reset();
    }
  }

  // 从右侧当前位置继续找下一对匹配；不够就推进左侧
  void seek_next_match() {
    while (true) {
      while (!right_->is_end()) {
        auto right_rec = right_->Next();
        RmRecord joined((int)len_);
        memcpy(joined.data, left_rec_->data, left_->tupleLen());
        memcpy(joined.data + left_->tupleLen(), right_rec->data,
               right_->tupleLen());
        if (check_all_conds(joined, cols_, fed_conds_)) {
          ++rows_out_;
          return;
        }
        right_->nextTuple();
      }
      left_->nextTuple();
      if (left_->is_end()) {
        isend_ = true;
        return;
      }
      left_rec_ = left_->Next();
      right_->beginTuple();
    }
  }
};
