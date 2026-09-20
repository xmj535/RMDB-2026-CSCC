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
#include "executor_abstract.h"
#include "system/sm.h"

// 截断输出：从 prev_ 转发前 limit_ 条记录
class LimitExecutor : public AbstractExecutor {
 private:
  std::unique_ptr<AbstractExecutor> prev_;
  int limit_;
  int emitted_ = 0;

 public:
  LimitExecutor(std::unique_ptr<AbstractExecutor> prev, int limit)
      : prev_(std::move(prev)), limit_(limit) {}

  const std::vector<ColMeta>& cols() const override { return prev_->cols(); }
  size_t tupleLen() const override { return prev_->tupleLen(); }
  std::string getType() override { return "LimitExecutor"; }
  bool is_end() const override {
    return emitted_ >= limit_ || prev_->is_end();
  }
  Rid& rid() override { return prev_->rid(); }
  AbstractExecutor* child() const override { return prev_.get(); }

  void beginTuple() override {
    emitted_ = 0;
    prev_->beginTuple();
    if (!prev_->is_end() && emitted_ < limit_) {
      ++rows_out_;
    }
  }

  void nextTuple() override {
    if (prev_->is_end()) return;
    ++emitted_;
    if (emitted_ >= limit_) return;
    prev_->nextTuple();
    if (!prev_->is_end()) ++rows_out_;
  }

  std::unique_ptr<RmRecord> Next() override { return prev_->Next(); }
};
