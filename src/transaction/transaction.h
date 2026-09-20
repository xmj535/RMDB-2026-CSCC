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

#include <atomic>
#include <deque>
#include <memory>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "common/common.h"
#include "transaction/txn_defs.h"
#include "record/rm_defs.h"

/** 表示此tuple的前一个版本的链接 */
struct UndoLink {
  /* 之前的版本可以在其中的事务中找到 */
  txn_id_t prev_txn_{INVALID_TXN_ID};
  /* 在 `prev_txn_` 中前一个版本的日志索引 */
  int prev_log_idx_{0};

  friend auto operator==(const UndoLink &a, const UndoLink &b) {
    return a.prev_txn_ == b.prev_txn_ && a.prev_log_idx_ == b.prev_log_idx_;
  }

  friend auto operator!=(const UndoLink &a, const UndoLink &b) { return !(a == b); }

  /* Checks if the undo link points to something. */
  bool IsValid() { return prev_txn_ != INVALID_TXN_ID; }
};

struct UndoLog {
  /* 此日志对应的旧版本是否为"已删除/不存在" */
  bool is_deleted_{false};
  /* 此撤销日志修改的字段（保留接口，本实现暂不按列存差量） */
  std::vector<bool> modified_fields_;
  /* 修改后的字段（保留接口） */
  std::vector<Value> tuple_;
  /* 旧版本完整记录的快照（is_deleted_ 为 true 时为 nullptr） */
  std::shared_ptr<RmRecord> tuple_test_;
  /* 此撤销日志对应的旧版本的提交时间戳；INVALID_TS 表示无更老版本 */
  timestamp_t ts_{INVALID_TS};
  /* 撤销日志的前一个版本 */
  UndoLink prev_version_{};
};


class Transaction {
   public:
    explicit Transaction(txn_id_t txn_id, IsolationLevel isolation_level = IsolationLevel::SERIALIZABLE)
        : state_(TransactionState::DEFAULT), isolation_level_(isolation_level), txn_id_(txn_id) {
        write_set_ = std::make_shared<std::deque<WriteRecord *>>();
        lock_set_ = std::make_shared<std::unordered_set<LockDataId>>();
        index_latch_page_set_ = std::make_shared<std::deque<Page *>>();
        index_deleted_page_set_ = std::make_shared<std::deque<Page*>>();
        prev_lsn_ = INVALID_LSN;
        thread_id_ = std::this_thread::get_id();
    }

    ~Transaction() = default;

    inline txn_id_t get_transaction_id() { return txn_id_; }

    inline std::thread::id get_thread_id() { return thread_id_; }

    inline void set_txn_mode(bool txn_mode) { txn_mode_ = txn_mode; }
    inline bool get_txn_mode() { return txn_mode_; }

    inline void set_start_ts(timestamp_t start_ts) { start_ts_ = start_ts; }
    inline timestamp_t get_start_ts() { return start_ts_; }

    inline IsolationLevel get_isolation_level() { return isolation_level_; }
    inline void set_isolation_level(IsolationLevel level) { isolation_level_ = level; }

    inline TransactionState get_state() { return state_; }
    inline void set_state(TransactionState state) { state_ = state; }

    inline lsn_t get_prev_lsn() { return prev_lsn_; }
    inline void set_prev_lsn(lsn_t prev_lsn) { prev_lsn_ = prev_lsn; }

    // 本事务是否写过 INSERT/UPDATE/DELETE 日志。只读事务没有任何需要持久化的
    // 变更，提交时可以跳过 COMMIT 记录和 WAL 强制刷盘（见 TransactionManager::commit）。
    inline bool has_wal_data() { return wal_data_written_; }
    inline void mark_wal_data() { wal_data_written_ = true; }

    inline std::shared_ptr<std::deque<WriteRecord *>> get_write_set() { return write_set_; }  
    inline void append_write_record(WriteRecord* write_record) { write_set_->push_back(write_record); }

    inline std::shared_ptr<std::deque<Page*>> get_index_deleted_page_set() { return index_deleted_page_set_; }
    inline void append_index_deleted_page(Page* page) { index_deleted_page_set_->push_back(page); }

    inline std::shared_ptr<std::deque<Page*>> get_index_latch_page_set() { return index_latch_page_set_; }
    inline void append_index_latch_page_set(Page* page) { index_latch_page_set_->push_back(page); }

    inline std::shared_ptr<std::unordered_set<LockDataId>> get_lock_set() { return lock_set_; }

    inline timestamp_t get_read_ts() const { return read_ts_; }
    inline void set_read_ts(timestamp_t ts) { read_ts_ = ts; }
    inline timestamp_t get_commit_ts() const { return commit_ts_; }
    inline void set_commit_ts(timestamp_t ts) { commit_ts_ = ts; }

    /** 修改现有的撤销日志 */
    inline auto ModifyUndoLog(int log_idx, UndoLog new_log) {
        std::scoped_lock<std::mutex> lck(latch_);
        undo_logs_[log_idx] = std::move(new_log);
      }

    /** @return 此事务中撤销日志的索引 */
    inline auto AppendUndoLog(UndoLog log) -> UndoLink {
        std::scoped_lock<std::mutex> lck(latch_);
        undo_logs_.emplace_back(std::move(log));
        return {txn_id_, static_cast<int>(undo_logs_.size() - 1)};
      }
    inline auto GetUndoLog(size_t log_id) -> UndoLog {
        std::scoped_lock<std::mutex> lck(latch_);
        return undo_logs_[log_id];
      }

    /** @return 撤销日志的数量 */
    inline auto GetUndoLogNum() -> size_t {
        std::scoped_lock<std::mutex> lck(latch_);
        return undo_logs_.size();
      }


   private:
    bool txn_mode_;                   // 用于标识当前事务为显式事务还是单条SQL语句的隐式事务
    TransactionState state_;          // 事务状态
    IsolationLevel isolation_level_;  // 事务的隔离级别，默认隔离级别为可串行化
    std::thread::id thread_id_;       // 当前事务对应的线程id
    lsn_t prev_lsn_;                  // 当前事务执行的最后一条操作对应的lsn，用于系统故障恢复
    bool wal_data_written_ = false;   // 是否已写过数据日志，决定提交时要不要落 COMMIT 记录并刷盘
    txn_id_t txn_id_;                 // 事务的ID，唯一标识符
    timestamp_t start_ts_;            // 事务的开始时间戳

    std::shared_ptr<std::deque<WriteRecord *>> write_set_;  // 事务包含的所有写操作
    std::shared_ptr<std::unordered_set<LockDataId>> lock_set_;  // 事务申请的所有锁
    std::shared_ptr<std::deque<Page*>> index_latch_page_set_;          // 维护事务执行过程中加锁的索引页面
    std::shared_ptr<std::deque<Page*>> index_deleted_page_set_;    // 维护事务执行过程中删除的索引页面

  std::atomic<timestamp_t> read_ts_{0};
  /** 提交时间戳 */
  std::atomic<timestamp_t> commit_ts_{INVALID_TS};
  /**
  * @brief 存储撤销日志。
  * 其他撤销日志/表堆将存储 (txn_id, index) 对，因此只能向此vector中追加内容或就地更新内容，而不能删除任何内容。
  */
  std::vector<UndoLog> undo_logs_;
  /** 用于访问事务级撤销日志的锁。 */
  std::mutex latch_;
};
