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

#include "execution_common.h"
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "record/rm_scan.h"
#include "system/sm.h"

// 按类型把两段 raw 字节比较，返回 -1 / 0 / +1
inline int compare_raw(const char* lhs, const char* rhs, int len,
                       ColType type) {
  switch (type) {
    case TYPE_INT: {
      // 8 字节 INT = SUM(整型) 的 int64 聚合输出;其余为常规 int32
      if (len == (int)sizeof(int64_t)) {
        int64_t a = *reinterpret_cast<const int64_t*>(lhs);
        int64_t b = *reinterpret_cast<const int64_t*>(rhs);
        return (a < b) ? -1 : (a > b ? 1 : 0);
      }
      int a = *reinterpret_cast<const int*>(lhs);
      int b = *reinterpret_cast<const int*>(rhs);
      return (a < b) ? -1 : (a > b ? 1 : 0);
    }
    case TYPE_FLOAT: {
      float a = *reinterpret_cast<const float*>(lhs);
      float b = *reinterpret_cast<const float*>(rhs);
      return (a < b) ? -1 : (a > b ? 1 : 0);
    }
    case TYPE_STRING:
    default:
      return memcmp(lhs, rhs, len);
  }
}

// 把 -1/0/+1 比较结果套到 SQL 比较符
inline bool eval_cmp(int cmp, CompOp op) {
  switch (op) {
    case OP_EQ:
      return cmp == 0;
    case OP_NE:
      return cmp != 0;
    case OP_LT:
      return cmp < 0;
    case OP_GT:
      return cmp > 0;
    case OP_LE:
      return cmp <= 0;
    case OP_GE:
      return cmp >= 0;
  }
  return false;
}

// 在 rec 上判断一条条件是否成立
inline bool check_cond(const RmRecord& rec,
                       const std::vector<ColMeta>& rec_cols,
                       const Condition& cond) {
  auto find_col = [&](const TabCol& tc) -> const ColMeta* {
    for (auto& c : rec_cols) {
      if (c.tab_name == tc.tab_name && c.name == tc.col_name) {
        return &c;
      }
    }
    return nullptr;
  };
  auto lhs_col = find_col(cond.lhs_col);
  if (lhs_col == nullptr) {
    return false;
  }

  const char* lhs_data = rec.data + lhs_col->offset;
  const char* rhs_data = nullptr;
  if (cond.is_rhs_val) {
    rhs_data = cond.rhs_val.raw->data;
  } else {
    auto rhs_col = find_col(cond.rhs_col);
    if (rhs_col == nullptr) {
      return false;
    }
    rhs_data = rec.data + rhs_col->offset;
  }
  return eval_cmp(compare_raw(lhs_data, rhs_data, lhs_col->len, lhs_col->type),
                  cond.op);
}

// 一组条件全部为真才算通过
inline bool check_all_conds(const RmRecord& rec,
                            const std::vector<ColMeta>& rec_cols,
                            const std::vector<Condition>& conds) {
  for (auto& c : conds) {
    if (!check_cond(rec, rec_cols, c)) {
      return false;
    }
  }
  return true;
}

class SeqScanExecutor : public AbstractExecutor {
 private:
  std::string tab_name_;
  std::string alias_;
  std::vector<Condition> conds_;
  RmFileHandle* fh_;
  std::vector<ColMeta> cols_;
  size_t len_;
  std::vector<Condition> fed_conds_;
  std::vector<Condition> ssi_conds_;  // SER 读跟踪谓词(见 set_ssi_conds)

  Rid rid_;
  std::unique_ptr<RecScan> scan_;
  SmManager* sm_manager_;
  // 缓存 advance_until_match 拿到的可见记录，Next() 直接返回
  std::unique_ptr<RmRecord> cur_rec_;

 public:
  SeqScanExecutor(SmManager* sm_manager, std::string tab_name,
                  std::vector<Condition> conds, Context* context)
      : SeqScanExecutor(sm_manager, std::move(tab_name), std::string(),
                        std::move(conds), context) {}

  // alias 为空时退回 tab_name；cols_ 的 tab_name 改写为 alias，便于上层按显示名定位
  SeqScanExecutor(SmManager* sm_manager, std::string tab_name,
                  std::string alias, std::vector<Condition> conds,
                  Context* context) {
    sm_manager_ = sm_manager;
    tab_name_ = std::move(tab_name);
    alias_ = alias.empty() ? tab_name_ : std::move(alias);
    conds_ = std::move(conds);
    TabMeta& tab = sm_manager_->db_.get_table(tab_name_);
    fh_ = sm_manager_->fhs_.at(tab_name_).get();
    cols_ = tab.cols;
    for (auto& c : cols_) c.tab_name = alias_;
    len_ = cols_.empty() ? 0 : (cols_.back().offset + cols_.back().len);
    context_ = context;
    fed_conds_ = conds_;
    ssi_conds_ = conds_;
  }

  // SER 读跟踪用的本表谓词：SELECT 计划把条件放在上层 Filter（fed_conds_ 为空）
  // 时由 portal 注入，保证记录读/谓词读/不可见写者检查仍是行级精确而非全表。
  void set_ssi_conds(std::vector<Condition> conds) {
    ssi_conds_ = std::move(conds);
  }

  const std::vector<ColMeta>& cols() const override { return cols_; }
  size_t tupleLen() const override { return len_; }
  std::string getType() override { return "SeqScanExecutor"; }
  bool is_end() const override { return scan_ == nullptr || scan_->is_end(); }
  Rid& rid() override { return rid_; }

  ColMeta get_col_offset(const TabCol& target) override {
    for (auto& c : cols_) {
      if (c.tab_name == target.tab_name && c.name == target.col_name) {
        return c;
      }
    }
    throw ColumnNotFoundError(target.tab_name + "." + target.col_name);
  }

  // 从头扫，停在第一条满足谓词的记录
  void beginTuple() override {
    scan_ = std::make_unique<RmScan>(fh_);
    // SER：登记一次本次扫描的谓词，让其他事务的写入可以做 phantom 比较
    if (context_ != nullptr && context_->txn_ != nullptr &&
        context_->txn_mgr_ != nullptr &&
        context_->txn_->get_isolation_level() ==
            IsolationLevel::SERIALIZABLE) {
      context_->txn_mgr_->SsiRecordPredicate(context_->txn_, tab_name_,
                                              ssi_conds_, cols_);
    }
    advance_until_match();
  }

  void nextTuple() override {
    if (scan_ == nullptr || scan_->is_end()) {
      return;
    }
    scan_->next();
    advance_until_match();
  }

  std::unique_ptr<RmRecord> Next() override {
    if (cur_rec_ != nullptr) return std::move(cur_rec_);
    return fh_->get_record(rid_, context_);
  }

 private:
  // 从当前 scan_ 位置往后找，直到命中或扫完；MVCC 下还要做可见性过滤
  void advance_until_match() {
    cur_rec_.reset();
    const bool ser = (context_ != nullptr && context_->txn_ != nullptr &&
                      context_->txn_mgr_ != nullptr &&
                      context_->txn_->get_isolation_level() ==
                          IsolationLevel::SERIALIZABLE);
    while (!scan_->is_end()) {
      rid_ = scan_->rid();
      auto rec = mvcc_visible_record(context_, tab_name_, fh_, rid_);
      if (rec == nullptr) {
        // 行对本事务不可见：可能是其他 SER 事务未提交/晚于本事务的写者造成；
        // 对"内容满足本扫描谓词"的不可见写者建立 W→me 反依赖（不计入 rid_reads）
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
          // 当前可见旧值不在查询结果中，但不可见新值可能被更新进查询范围。
          context_->txn_mgr_->SsiCheckInvisibleWriters(
              context_->txn_, tab_name_, rid_, ssi_conds_, cols_);
        }
      }
      if (fed_conds_.empty() || check_all_conds(*rec, cols_, fed_conds_)) {
        cur_rec_ = std::move(rec);
        ++rows_out_;
        return;
      }
      scan_->next();
    }
  }
};
