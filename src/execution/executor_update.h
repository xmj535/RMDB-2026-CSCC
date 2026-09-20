/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once

#include <chrono>
#include <thread>

#include "execution_common.h"
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"
#include "transaction/transaction_manager.h"

class UpdateExecutor : public AbstractExecutor {
 private:
  TabMeta tab_;
  std::vector<Condition> conds_;
  RmFileHandle* fh_;
  std::vector<Rid> rids_;
  std::string tab_name_;
  std::vector<SetClause> set_clauses_;
  SmManager* sm_manager_;

 public:
  UpdateExecutor(SmManager* sm_manager, const std::string& tab_name,
                 std::vector<SetClause> set_clauses,
                 std::vector<Condition> conds, std::vector<Rid> rids,
                 Context* context) {
    sm_manager_ = sm_manager;
    tab_name_ = tab_name;
    set_clauses_ = std::move(set_clauses);
    tab_ = sm_manager_->db_.get_table(tab_name);
    fh_ = sm_manager_->fhs_.at(tab_name).get();
    conds_ = std::move(conds);
    rids_ = std::move(rids);
    context_ = context;
  }

  std::unique_ptr<RmRecord> Next() override {
    Transaction* txn = context_ ? context_->txn_ : nullptr;
    TransactionManager* tm = context_ ? context_->txn_mgr_ : nullptr;
    const bool mvcc = (txn != nullptr && tm != nullptr &&
                       TransactionManager::IsMvccIso(txn->get_isolation_level()));
    const bool ser = (txn != nullptr && tm != nullptr &&
                      txn->get_isolation_level() == IsolationLevel::SERIALIZABLE);

    // 由给定旧行套用 SET 子句算出 new_image。抽成 lambda 以便 SNAPSHOT 乐观重试
    // 在【最新已提交值】之上反复重算（算术 SET 的操作数取传入的 old_rec）。
    auto compute_new_image =
        [&](const RmRecord& old_rec) -> std::shared_ptr<RmRecord> {
      auto new_image = std::make_shared<RmRecord>(old_rec);
      for (auto& set : set_clauses_) {
        auto col = tab_.get_col(set.lhs.col_name);
        if (set.rhs_is_col) {
          // SQL SET 子句的各右值都读取语句开始处理该行时的 old_rec，和现有
          // 算术更新保持一致。即使 lhs/rhs 是同一列，后续仍会执行 BeginWrite、
          // SSI、索引一致性检查和 WAL，不在这里把自赋值优化掉。
          auto rcol = tab_.get_col(set.rhs_col.col_name);
          if (col->type == rcol->type) {
            memcpy(new_image->data + col->offset,
                   old_rec.data + rcol->offset, col->len);
          } else if (col->type == TYPE_FLOAT && rcol->type == TYPE_INT) {
            float value = static_cast<float>(*reinterpret_cast<const int*>(
                old_rec.data + rcol->offset));
            memcpy(new_image->data + col->offset, &value, sizeof(value));
          } else if (col->type == TYPE_INT && rcol->type == TYPE_FLOAT) {
            int value = static_cast<int>(*reinterpret_cast<const float*>(
                old_rec.data + rcol->offset));
            memcpy(new_image->data + col->offset, &value, sizeof(value));
          } else {
            throw IncompatibleTypeError(coltype2str(col->type),
                                        coltype2str(rcol->type));
          }
          continue;
        }
        if (set.rhs.type != col->type) {
          if (col->type == TYPE_FLOAT && set.rhs.type == TYPE_INT) {
            set.rhs.set_float(static_cast<float>(set.rhs.int_val));
          } else if (col->type == TYPE_INT && set.rhs.type == TYPE_FLOAT) {
            set.rhs.set_int(static_cast<int>(set.rhs.float_val));
          } else {
            throw IncompatibleTypeError(coltype2str(col->type),
                                        coltype2str(set.rhs.type));
          }
        }
        if (set.rhs.raw == nullptr) set.rhs.init_raw(col->len);
        if (set.op != 0) {
          // 算术更新的基列取传入的 old_rec（SI 路径=快照旧值；SNAPSHOT
          // 乐观路径=最新已提交值）。首项和 trailing_terms 按 SQL 顺序左结合；
          // 多个 SET 子句仍全部读取同一个 old_rec。仅支持数值列。
          auto rcol = tab_.get_col(set.rhs_col.col_name);
          auto apply_int = [](int lhs, char op, int rhs) {
            if (op == '+') return lhs + rhs;
            if (op == '-') return lhs - rhs;
            if (op == '*') return lhs * rhs;
            throw RMDBError("unsupported UPDATE arithmetic operator");
          };
          auto apply_float = [](float lhs, char op, float rhs) {
            if (op == '+') return lhs + rhs;
            if (op == '-') return lhs - rhs;
            if (op == '*') return lhs * rhs;
            throw RMDBError("unsupported UPDATE arithmetic operator");
          };
          if (col->type == TYPE_INT && rcol->type == TYPE_INT) {
            int result =
                *reinterpret_cast<const int*>(old_rec.data + rcol->offset);
            result = apply_int(result, set.op, set.rhs.int_val);
            for (const auto& term : set.trailing_terms) {
              result = apply_int(result, term.op, term.rhs.int_val);
            }
            memcpy(new_image->data + col->offset, &result, sizeof(result));
          } else if (col->type == TYPE_FLOAT) {
            float result =
                (rcol->type == TYPE_FLOAT)
                    ? *reinterpret_cast<const float*>(old_rec.data +
                                                      rcol->offset)
                    : static_cast<float>(*reinterpret_cast<const int*>(
                          old_rec.data + rcol->offset));
            result = apply_float(result, set.op, set.rhs.float_val);
            for (const auto& term : set.trailing_terms) {
              result = apply_float(result, term.op, term.rhs.float_val);
            }
            memcpy(new_image->data + col->offset, &result, sizeof(result));
          } else {
            throw IncompatibleTypeError(coltype2str(col->type),
                                        coltype2str(rcol->type));
          }
        } else {
          memcpy(new_image->data + col->offset, set.rhs.raw->data, col->len);
        }
      }
      return new_image;
    };

    for (auto& rid : rids_) {
      std::shared_ptr<RmRecord> old_rec_sp;
      std::shared_ptr<RmRecord> new_image;
      std::shared_ptr<RmRecord> pre_image;

      // SNAPSHOT 的写必须以【本事务快照】的行版本为基准，绝不能改用更新后的
      // 版本重算。规范第三章第（三）节第 2 条：「在 SNAPSHOT ISOLATION 下，若更新
      // 所依据的行版本已经过期，必须返回 TRANSACTION_ABORT，不得把该更新应用到
      // 更新后的行版本」。此处曾有一条"读最新已提交值重算 + 有界重试"的乐观路径
      // （SNAPSHOT_WRITE_REBASE），用来让 w_ytd+=/d_ytd+= 这类可交换累加在撞冲突时
      // 叠加而不作废会破坏快照写冲突语义，并可能产生 lost update 或重复累加。
      // 该路径已连同其开关与 LatestCommitted() 接口一并删除，不留可被重新打开的旋钮。
      if (mvcc) {
        old_rec_sp = tm->VisibleRecord(tab_name_, rid, txn);
        if (old_rec_sp == nullptr) continue;
      } else {
        auto tmp = fh_->get_record(rid, context_);
        old_rec_sp = std::make_shared<RmRecord>(*tmp);
      }
      new_image = compute_new_image(*old_rec_sp);
      if (ser) {
        tm->SsiRecordWrite(txn, tab_name_, rid, old_rec_sp.get(),
                            /*op_is_insert=*/false);
      }
      if (mvcc) {
        auto wr = tm->BeginWrite(tab_name_, rid, txn, /*op_is_update=*/true,
                                  new_image, &pre_image);
        if (wr == TransactionManager::WriteResult::WW_CONFLICT) {
          throw TransactionAbortException(txn->get_transaction_id(),
                                           AbortReason::DEADLOCK_PREVENTION);
        }
        if (wr == TransactionManager::WriteResult::KEY_NOT_FOUND) continue;
      }

      if (mvcc) {

        // 索引策略：UPDATE 时仅 INSERT 新键，保留旧键，commit 时再清掉旧键。
        // - 保留旧键：让另一事务用旧键做 IndexScan 时仍能定位到 R，进而通过
        //   VisibleRecord 拿到老快照数据；进入 UpdateExecutor 后才能撞上 ww
        //   触发 abort。否则它的 IndexScan 直接查不到，UPDATE 静默 no-op，
        //   该 abort 没法触发。这是 si/Deadlock / UpdateTest 在 IndexScan 路径
        //   上失分的最可能原因。
        // - commit 时清理：避免旧键残留导致后续 INSERT 同旧值被误判 Duplicate。
        //   见 transaction_manager.cpp::FinalizeCommit。
        // - abort 时清理新键：旧键既然没动，abort 时也不用恢复。
        //   见 transaction_manager.cpp::abort UPDATE 分支。
        for (auto& index : tab_.indexes) {
          auto ih = get_index_handle(index);
          std::vector<char> old_key = make_key(index, old_rec_sp->data);
          std::vector<char> new_key = make_key(index, new_image->data);
          if (memcmp(old_key.data(), new_key.data(), index.col_tot_len) == 0) {
            continue;
          }
          // 唯一性预检：新键是否已存在 (排除自己 + 排除自己历史快照下的陈旧项)
          std::vector<Rid> tmp;
          if (ih->get_value(new_key.data(), &tmp, txn)) {
            bool truly_conflict = false;
            for (auto& existing : tmp) {
              if (existing == rid) continue;
              auto probe =
                  tm->ProbeRow(tab_name_, existing, txn->get_start_ts());
              // 只有当该 rid 当前已提交版本(chain[0])实际仍处于"未删除"状态时
              // 才算冲突；committed_deleted 表示它已被逻辑删除，可以接管。
              if (probe.exists && !probe.committed_deleted &&
                  probe.committed_ts > 0) {
                truly_conflict = true;
                break;
              }
            }
            if (truly_conflict) {
              throw RMDBError("Duplicate key violates unique index on " +
                                tab_name_);
            }
          }
          ih->insert_entry(new_key.data(), rid, txn);
        }
        // 堆暂不写。FinalizeCommit 时把 new_image 写入堆；abort 时无操作。
      } else {
        // 非 MVCC：保留原 2PL 行为（直接动堆+索引，append write_set）
        for (auto& index : tab_.indexes) {
          auto ih = get_index_handle(index);
          std::vector<char> key = make_key(index, old_rec_sp->data);
          ih->delete_entry(key.data(), txn);
        }
        fh_->update_record(rid, new_image->data, context_);
        for (auto& index : tab_.indexes) {
          auto ih = get_index_handle(index);
          std::vector<char> key = make_key(index, new_image->data);
          ih->insert_entry(key.data(), rid, txn);
        }
        if (txn != nullptr) {
          txn->append_write_record(
              new WriteRecord(WType::UPDATE_TUPLE, tab_name_, rid, *old_rec_sp));
        }
      }

      // WAL：记录 UPDATE 日志（旧值供 undo，新值供 redo）。
      // MVCC 下堆写延后到 commit，这里属于先写日志后写数据，符合 WAL。
      if (enable_logging.load() && context_ != nullptr &&
          context_->log_mgr_ != nullptr && txn != nullptr) {
        UpdateLogRecord log_rec(txn->get_transaction_id(), *old_rec_sp,
                                *new_image, rid, tab_name_);
        log_rec.prev_lsn_ = txn->get_prev_lsn();
        lsn_t lsn = context_->log_mgr_->add_log_to_buffer(&log_rec);
        txn->set_prev_lsn(lsn);
        txn->mark_wal_data();
      }

      if (ser) {
        tm->SsiRecordWrite(txn, tab_name_, rid, new_image.get(),
                            /*op_is_insert=*/false);
      }
    }
    return nullptr;
  }

  Rid& rid() override { return _abstract_rid; }

 private:
  IxIndexHandle* get_index_handle(const IndexMeta& index) {
    auto ix_name =
        sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols);
    return sm_manager_->ihs_.at(ix_name).get();
  }

  std::vector<char> make_key(const IndexMeta& index, const char* rec_data) {
    std::vector<char> key(index.col_tot_len);
    int off = 0;
    for (auto& col : index.cols) {
      memcpy(key.data() + off, rec_data + col.offset, col.len);
      off += col.len;
    }
    return key;
  }
};
