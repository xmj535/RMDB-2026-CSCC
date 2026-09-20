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
#include "system/sm.h"

// 谓词过滤算子：把通过条件的元组逐条向上输出
class FilterExecutor : public AbstractExecutor {
 private:
  std::unique_ptr<AbstractExecutor> prev_;
  std::vector<Condition> conds_;
  std::vector<ColMeta> cols_;
  size_t len_;

  std::unique_ptr<RmRecord> cur_rec_;

 public:
  FilterExecutor(std::unique_ptr<AbstractExecutor> prev,
                 std::vector<Condition> conds) {
    prev_ = std::move(prev);
    conds_ = std::move(conds);
    cols_ = prev_->cols();
    len_ = prev_->tupleLen();
  }

  const std::vector<ColMeta>& cols() const override { return cols_; }
  size_t tupleLen() const override { return len_; }
  std::string getType() override { return "FilterExecutor"; }
  bool is_end() const override { return prev_->is_end() && cur_rec_ == nullptr; }
  Rid& rid() override { return prev_->rid(); }

  ColMeta get_col_offset(const TabCol& target) override {
    return prev_->get_col_offset(target);
  }

  AbstractExecutor* child() const override { return prev_.get(); }

  // 从前驱拉取记录，过滤掉不满足条件的；每次 beginTuple/nextTuple 都推进到下一条符合的
  void beginTuple() override {
    prev_->beginTuple();
    advance_until_match();
  }

  void nextTuple() override {
    cur_rec_.reset();
    if (prev_->is_end()) {
      return;
    }
    prev_->nextTuple();
    advance_until_match();
  }

  std::unique_ptr<RmRecord> Next() override {
    if (cur_rec_) {
      return std::move(cur_rec_);
    }
    return prev_->Next();
  }

 private:
  void advance_until_match() {
    cur_rec_.reset();
    while (!prev_->is_end()) {
      auto rec = prev_->Next();
      if (check_all_conds(*rec, cols_, conds_)) {
        cur_rec_ = std::move(rec);
        ++rows_out_;
        return;
      }
      prev_->nextTuple();
    }
  }
};
