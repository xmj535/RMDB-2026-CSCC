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

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "executor_seq_scan.h"  // 复用 compare_raw
#include "index/ix.h"
#include "system/sm.h"

// 物化全部下层记录后按多列排序
class SortExecutor : public AbstractExecutor {
 private:
  std::unique_ptr<AbstractExecutor> prev_;
  std::vector<ColMeta> key_cols_;
  std::vector<bool> is_descs_;
  std::vector<std::unique_ptr<RmRecord>> buffer_;
  size_t cursor_ = 0;

 public:
  // 单列构造（向后兼容）
  SortExecutor(std::unique_ptr<AbstractExecutor> prev, TabCol sel_col,
               bool is_desc) {
    prev_ = std::move(prev);
    key_cols_.push_back(prev_->get_col_offset(sel_col));
    is_descs_.push_back(is_desc);
  }
  // 多列构造
  SortExecutor(std::unique_ptr<AbstractExecutor> prev,
               const std::vector<TabCol>& sel_cols,
               const std::vector<bool>& is_descs) {
    prev_ = std::move(prev);
    for (auto& sc : sel_cols) {
      key_cols_.push_back(prev_->get_col_offset(sc));
    }
    is_descs_ = is_descs;
  }

  const std::vector<ColMeta>& cols() const override { return prev_->cols(); }
  size_t tupleLen() const override { return prev_->tupleLen(); }
  std::string getType() override { return "SortExecutor"; }
  bool is_end() const override { return cursor_ >= buffer_.size(); }
  Rid& rid() override { return _abstract_rid; }

  void beginTuple() override {
    buffer_.clear();
    for (prev_->beginTuple(); !prev_->is_end(); prev_->nextTuple()) {
      buffer_.push_back(prev_->Next());
    }
    std::sort(buffer_.begin(), buffer_.end(),
              [this](const std::unique_ptr<RmRecord>& a,
                     const std::unique_ptr<RmRecord>& b) {
                for (size_t i = 0; i < key_cols_.size(); ++i) {
                  const auto& kc = key_cols_[i];
                  int cmp = compare_raw(a->data + kc.offset,
                                        b->data + kc.offset, kc.len, kc.type);
                  if (cmp == 0) continue;
                  return is_descs_[i] ? cmp > 0 : cmp < 0;
                }
                return false;
              });
    cursor_ = 0;
  }

  void nextTuple() override {
    if (cursor_ < buffer_.size()) {
      ++cursor_;
    }
  }

  // 返回当前位置元组的拷贝
  std::unique_ptr<RmRecord> Next() override {
    if (cursor_ >= buffer_.size()) {
      return nullptr;
    }
    return std::make_unique<RmRecord>(*buffer_[cursor_]);
  }
};
