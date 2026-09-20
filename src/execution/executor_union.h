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
#include <unordered_set>

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

// 物化所有分支元组后做去重；列按公共超类型提升
class UnionExecutor : public AbstractExecutor {
 private:
  std::vector<std::unique_ptr<AbstractExecutor>> children_;
  std::vector<ColMeta> out_cols_;
  size_t len_;
  std::vector<std::unique_ptr<RmRecord>> buffer_;
  size_t cursor_ = 0;

 public:
  UnionExecutor(std::vector<std::unique_ptr<AbstractExecutor>> children,
                std::vector<ColMeta> out_cols) {
    children_ = std::move(children);
    out_cols_ = std::move(out_cols);
    len_ = out_cols_.empty()
               ? 0
               : (out_cols_.back().offset + out_cols_.back().len);
  }

  const std::vector<ColMeta>& cols() const override { return out_cols_; }
  size_t tupleLen() const override { return len_; }
  std::string getType() override { return "UnionExecutor"; }
  bool is_end() const override { return cursor_ >= buffer_.size(); }
  Rid& rid() override { return _abstract_rid; }

  ColMeta get_col_offset(const TabCol& target) override {
    for (auto& c : out_cols_) {
      if (c.tab_name == target.tab_name && c.name == target.col_name) {
        return c;
      }
    }
    throw ColumnNotFoundError(target.tab_name + "." + target.col_name);
  }

  void beginTuple() override {
    buffer_.clear();
    std::unordered_set<std::string> seen;
    for (auto& ch : children_) {
      const auto& child_cols = ch->cols();
      for (ch->beginTuple(); !ch->is_end(); ch->nextTuple()) {
        auto in_rec = ch->Next();
        auto out_rec = std::make_unique<RmRecord>((int)len_);
        std::memset(out_rec->data, 0, len_);
        for (size_t i = 0; i < out_cols_.size(); ++i) {
          const auto& src = child_cols[i];
          const auto& dst = out_cols_[i];
          const char* sp = in_rec->data + src.offset;
          char* dp = out_rec->data + dst.offset;
          if (src.type == dst.type) {
            if (src.type == TYPE_STRING) {
              int n = std::min(src.len, dst.len);
              std::memcpy(dp, sp, n);
            } else {
              std::memcpy(dp, sp, dst.len);
            }
          } else if (src.type == TYPE_INT && dst.type == TYPE_FLOAT) {
            int iv = *reinterpret_cast<const int*>(sp);
            float fv = static_cast<float>(iv);
            std::memcpy(dp, &fv, sizeof(float));
          } else {
            // 分析器已保证类型兼容，理论上不会到达此分支
            std::memcpy(dp, sp, std::min<int>(src.len, dst.len));
          }
        }
        std::string key(out_rec->data, len_);
        if (seen.insert(std::move(key)).second) {
          buffer_.push_back(std::move(out_rec));
          ++rows_out_;
        }
      }
    }
    cursor_ = 0;
  }

  void nextTuple() override {
    if (cursor_ < buffer_.size()) ++cursor_;
  }

  std::unique_ptr<RmRecord> Next() override {
    if (cursor_ >= buffer_.size()) return nullptr;
    return std::make_unique<RmRecord>(*buffer_[cursor_]);
  }
};
