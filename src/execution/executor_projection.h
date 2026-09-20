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
#include "index/ix.h"
#include "system/sm.h"

class ProjectionExecutor : public AbstractExecutor {
 private:
  std::unique_ptr<AbstractExecutor> prev_;
  std::vector<ColMeta> cols_;
  size_t len_;
  std::vector<size_t> sel_idxs_;  // 投影列在 prev_->cols() 中的下标

 public:
  ProjectionExecutor(std::unique_ptr<AbstractExecutor> prev,
                     const std::vector<TabCol>& sel_cols) {
    prev_ = std::move(prev);

    size_t offset = 0;
    auto& prev_cols = prev_->cols();
    for (auto& sel_col : sel_cols) {
      auto pos = get_col(prev_cols, sel_col);
      sel_idxs_.push_back(pos - prev_cols.begin());
      ColMeta col = *pos;
      col.offset = offset;
      offset += col.len;
      cols_.push_back(col);
    }
    len_ = offset;
  }

  const std::vector<ColMeta>& cols() const override { return cols_; }
  size_t tupleLen() const override { return len_; }
  std::string getType() override { return "ProjectionExecutor"; }
  bool is_end() const override { return prev_->is_end(); }
  Rid& rid() override { return _abstract_rid; }

  AbstractExecutor* child() const override { return prev_.get(); }

  void beginTuple() override {
    prev_->beginTuple();
    if (!prev_->is_end()) ++rows_out_;
  }
  void nextTuple() override {
    prev_->nextTuple();
    if (!prev_->is_end()) ++rows_out_;
  }

  // 按 sel_idxs_ 从 prev 的记录里挑出对应字段、拼成新 record
  std::unique_ptr<RmRecord> Next() override {
    auto in_rec = prev_->Next();
    auto out = std::make_unique<RmRecord>((int)len_);
    const auto& prev_cols = prev_->cols();
    for (size_t i = 0; i < sel_idxs_.size(); ++i) {
      const auto& src = prev_cols[sel_idxs_[i]];
      const auto& dst = cols_[i];
      memcpy(out->data + dst.offset, in_rec->data + src.offset, dst.len);
    }
    return out;
  }
};
