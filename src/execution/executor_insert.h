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
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "record/rm_scan.h"
#include "system/sm.h"
#include "transaction/transaction_manager.h"

class InsertExecutor : public AbstractExecutor {
 private:
  TabMeta tab_;
  std::vector<Value> values_;
  RmFileHandle* fh_;
  std::string tab_name_;
  Rid rid_;
  SmManager* sm_manager_;

 public:
  InsertExecutor(SmManager* sm_manager, const std::string& tab_name,
                 std::vector<Value> values, Context* context) {
    sm_manager_ = sm_manager;
    tab_ = sm_manager_->db_.get_table(tab_name);
    values_ = values;
    tab_name_ = tab_name;
    if (values.size() != tab_.cols.size()) {
      throw InvalidValueCountError();
    }
    fh_ = sm_manager_->fhs_.at(tab_name).get();
    context_ = context;
  }

  // 拼好记录 → 唯一索引冲突检测 → 写堆 → 写索引 → 登记 MVCC pending
  std::unique_ptr<RmRecord> Next() override {
    RmRecord rec(fh_->get_file_hdr().record_size);
    for (size_t i = 0; i < values_.size(); i++) {
      auto& col = tab_.cols[i];
      auto& val = values_[i];
      if (col.type != val.type) {
        if (col.type == TYPE_FLOAT && val.type == TYPE_INT) {
          val.set_float(static_cast<float>(val.int_val));
        } else if (col.type == TYPE_INT && val.type == TYPE_FLOAT) {
          val.set_int(static_cast<int>(val.float_val));
        } else {
          throw IncompatibleTypeError(coltype2str(col.type),
                                      coltype2str(val.type));
        }
      }
      val.init_raw(col.len);
      memcpy(rec.data + col.offset, val.raw->data, col.len);
    }

    Transaction* txn = context_ ? context_->txn_ : nullptr;
    TransactionManager* tm = context_ ? context_->txn_mgr_ : nullptr;
    const bool mvcc = (txn != nullptr && tm != nullptr &&
                       TransactionManager::IsMvccIso(txn->get_isolation_level()));

    // 唯一索引检测：与现有索引项做冲突判断
    for (auto& index : tab_.indexes) {
      auto ih = get_index_handle(index);
      std::vector<char> key = make_key(index, rec.data);
      std::vector<Rid> tmp;
      if (!ih->get_value(key.data(), &tmp, txn)) continue;
      // 索引上有同 key：在 MVCC 下逐一检查指向的 rid 状态
      if (!mvcc) {
        throw RMDBError("Duplicate key violates unique index on " + tab_name_);
      }
      bool truly_conflict = false;
      bool ww = false;
      bool need_remove_stale = false;
      for (auto& existing : tmp) {
        auto probe = tm->ProbeRow(tab_name_, existing, txn->get_start_ts());
        // 自己刚写过同 key：根据 pending op 决定
        if (probe.pending_writer == txn->get_transaction_id()) {
          if (probe.pending_op == PendingWrite::Op::DELETE_OP) {
            // 我刚删除了这个 key → 可以接管
            need_remove_stale = true;
            continue;
          }
          // INSERT/UPDATE 自己又重复同 key → duplicate
          truly_conflict = true;
          break;
        }
        // 其他事务还在写这条 rid：可能未提交，可能正提交
        if (probe.pending_writer != INVALID_TXN_ID) {
          Transaction* other = tm->lookup_txn_any_thread(probe.pending_writer);
          if (other != nullptr) {
            auto st = other->get_state();
            timestamp_t oc = other->get_commit_ts();
            if (st != TransactionState::ABORTED &&
                st != TransactionState::COMMITTED) {
              ww = true;
              break;
            }
            if (oc != INVALID_TS && oc > txn->get_start_ts()) {
              ww = true;
              break;
            }
          }
        }
        // chain 顶 (最新已提交) 状态：若存在已提交且未删的版本 → duplicate
        // （索引上的物理唯一性是无视快照视角的；本事务无论看不看见都得 fail）
        if (probe.exists && probe.committed_ts > 0 &&
            !probe.committed_deleted) {
          // 但要注意 committed_ts==0 是 "默认值"，需要区分。RowVersion 的 chain
          // 顶应该总有有效 commit_ts；这里通过 committed_visible || (!committed_deleted) 判
          truly_conflict = true;
          break;
        }
        // 已提交删除 / 无活跃版本：可以接管这个 key
        need_remove_stale = true;
      }
      if (ww) {
        throw TransactionAbortException(txn->get_transaction_id(),
                                         AbortReason::DEADLOCK_PREVENTION);
      }
      if (truly_conflict) {
        throw RMDBError("Duplicate key violates unique index on " + tab_name_);
      }
      if (need_remove_stale) {
        // 把指向"已逻辑删除"的旧索引项摘掉，让新插入接管
        ih->delete_entry(key.data(), txn);
      }
    }

    // 无唯一索引兜底：若发现某 rid 正被另一活事务标记 DELETE_OP，且该 rid 的
    // RowVersion.chain 顶 (= 我能看见的旧版本) 字节与 new rec 相同，则视为
    // DELETE+INSERT 同一逻辑行的写写冲突。
    // 这是 si/WriteWriteConflictDeleteInsertTest 的语义：DELETE 和 INSERT 操作的
    // 是"同一行"（无主键时以全字段字节相等作为判定），二者并发须 abort 后写者。
    // 仅检查其他活跃事务的 pending-delete 候选 rid，避免对整张表全堆扫描
    // （否则每条 insert 都是 O(表大小)，批量插入退化为 O(N^2)）。
    if (mvcc) {
      for (auto& r : tm->GetPendingDeletedRids(tab_name_,
                                               txn->get_transaction_id())) {
        auto probe = tm->ProbeRow(tab_name_, r, txn->get_start_ts());
        if (probe.pending_writer != INVALID_TXN_ID &&
            probe.pending_writer != txn->get_transaction_id() &&
            probe.pending_op == PendingWrite::Op::DELETE_OP) {
          Transaction* other = tm->lookup_txn_any_thread(probe.pending_writer);
          if (other != nullptr) {
            auto st = other->get_state();
            timestamp_t oc = other->get_commit_ts();
            bool other_alive_or_late_commit =
                (st != TransactionState::ABORTED &&
                 st != TransactionState::COMMITTED) ||
                (oc != INVALID_TS && oc > txn->get_start_ts());
            if (other_alive_or_late_commit) {
              // 以 chain 顶 (pre-delete 数据) 作字节比较；找不到 chain 时跳过
              auto old_vis = tm->VisibleRecord(tab_name_, r, txn);
              if (old_vis != nullptr && old_vis->size == rec.size &&
                  memcmp(old_vis->data, rec.data, rec.size) == 0) {
                throw TransactionAbortException(
                    txn->get_transaction_id(),
                    AbortReason::DEADLOCK_PREVENTION);
              }
            }
          }
        }
      }
    }

    rid_ = fh_->insert_record(rec.data, context_);
    if (rid_.page_no < 0 || rid_.slot_no < 0) {
      // 绝不能带着非法 rid 继续：写进 WAL 会变成恢复毒丸
      throw InternalError("insert_record returned invalid rid");
    }

    // WAL：插入是 eager 写堆，立刻记 INSERT 日志（redo 重放、undo 删除）
    if (enable_logging.load() && context_ != nullptr &&
        context_->log_mgr_ != nullptr && txn != nullptr) {
      InsertLogRecord log_rec(txn->get_transaction_id(), rec, rid_, tab_name_);
      log_rec.prev_lsn_ = txn->get_prev_lsn();
      lsn_t lsn = context_->log_mgr_->add_log_to_buffer(&log_rec);
      txn->set_prev_lsn(lsn);
      txn->mark_wal_data();
    }

    // MVCC：先登记 pending，再加索引。这样并发观察 RowVersion 时不会先看到没有 pending 标记的可见行。
    if (mvcc) {
      auto new_image = std::make_shared<RmRecord>(rec);
      tm->BeginInsert(tab_name_, rid_, txn, std::move(new_image));
    }

    for (auto& index : tab_.indexes) {
      auto ih = get_index_handle(index);
      std::vector<char> key = make_key(index, rec.data);
      ih->insert_entry(key.data(), rid_, txn);
    }

    if (txn != nullptr) {
      if (tm != nullptr &&
          txn->get_isolation_level() == IsolationLevel::SERIALIZABLE) {
        tm->SsiRecordWrite(txn, tab_name_, rid_, &rec, /*op_is_insert=*/true);
      }
      // 非 MVCC 路径下保留 write_set，便于 2PL 回滚
      if (!mvcc) {
        txn->append_write_record(
            new WriteRecord(WType::INSERT_TUPLE, tab_name_, rid_));
      }
    }
    return nullptr;
  }

  Rid& rid() override { return rid_; }

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
