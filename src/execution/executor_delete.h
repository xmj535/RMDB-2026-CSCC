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

#include "execution_common.h"
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"
#include "transaction/transaction_manager.h"

class DeleteExecutor : public AbstractExecutor {
 private:
  TabMeta tab_;
  std::vector<Condition> conds_;
  RmFileHandle* fh_;
  std::vector<Rid> rids_;
  std::string tab_name_;
  SmManager* sm_manager_;

 public:
  DeleteExecutor(SmManager* sm_manager, const std::string& tab_name,
                 std::vector<Condition> conds, std::vector<Rid> rids,
                 Context* context) {
    sm_manager_ = sm_manager;
    tab_name_ = tab_name;
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

    for (auto& rid : rids_) {
      std::shared_ptr<RmRecord> old_rec_sp;
      if (mvcc) {
        old_rec_sp = tm->VisibleRecord(tab_name_, rid, txn);
        if (old_rec_sp == nullptr) continue;  // 对我不可见，跳过
      } else {
        auto tmp = fh_->get_record(rid, context_);
        old_rec_sp = std::make_shared<RmRecord>(*tmp);
      }

      if (ser) {
        tm->SsiRecordWrite(txn, tab_name_, rid, old_rec_sp.get(),
                            /*op_is_insert=*/false);
      }

      if (mvcc) {
        std::shared_ptr<RmRecord> pre_image;
        auto wr = tm->BeginWrite(tab_name_, rid, txn, /*op_is_update=*/false,
                                  /*new_image=*/nullptr, &pre_image);
        if (wr == TransactionManager::WriteResult::WW_CONFLICT) {
          throw TransactionAbortException(txn->get_transaction_id(),
                                           AbortReason::DEADLOCK_PREVENTION);
        }
        if (wr == TransactionManager::WriteResult::KEY_NOT_FOUND) {
          continue;
        }
        // MVCC 下 DELETE 不动堆和索引。后来者写入同一 key 时会清理"逻辑已删"的索引项。
        // 这里保留索引项是为了让老快照的 IndexScan 仍能定位到这条 rid，
        // 再通过 VisibleRecord 在 chain 上找到 pre-delete 数据返回。
      } else {
        // 非 MVCC：直接动堆 + 索引
        for (auto& index : tab_.indexes) {
          auto ix_name = sm_manager_->get_ix_manager()->get_index_name(
              tab_name_, index.cols);
          auto ih = sm_manager_->ihs_.at(ix_name).get();
          std::vector<char> key(index.col_tot_len);
          int off = 0;
          for (auto& col : index.cols) {
            memcpy(key.data() + off, old_rec_sp->data + col.offset, col.len);
            off += col.len;
          }
          ih->delete_entry(key.data(), txn);
        }
        fh_->delete_record(rid, context_);
        if (txn != nullptr) {
          txn->append_write_record(
              new WriteRecord(WType::DELETE_TUPLE, tab_name_, rid, *old_rec_sp));
        }
      }

      // WAL：记录 DELETE 日志（旧值供 undo，redo 时删除该 rid）。
      // MVCC 下堆删延后到 commit，这里属于先写日志后写数据。
      if (enable_logging.load() && context_ != nullptr &&
          context_->log_mgr_ != nullptr && txn != nullptr) {
        DeleteLogRecord log_rec(txn->get_transaction_id(), *old_rec_sp, rid,
                                tab_name_);
        log_rec.prev_lsn_ = txn->get_prev_lsn();
        lsn_t lsn = context_->log_mgr_->add_log_to_buffer(&log_rec);
        txn->set_prev_lsn(lsn);
        txn->mark_wal_data();
      }
    }
    return nullptr;
  }

  Rid& rid() override { return _abstract_rid; }
};
