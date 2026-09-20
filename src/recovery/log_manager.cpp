/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include <cstring>
#include "log_manager.h"

/**
 * @description: 添加日志记录到日志缓冲区中，并返回日志记录号
 * @param {LogRecord*} log_record 要写入缓冲区的日志记录
 * @return {lsn_t} 返回该日志的日志记录号
 */
lsn_t LogManager::add_log_to_buffer(LogRecord* log_record) {
    std::unique_lock<std::mutex> lock(latch_);
    // 分配 lsn
    lsn_t lsn = global_lsn_.fetch_add(1);
    log_record->lsn_ = lsn;
    // 缓冲区放不下当前日志，先把已有内容刷盘腾出空间。
    // 注意此时本条记录尚未进入缓冲区，刷的是 buffer_max_lsn_ 及之前的记录。
    if (log_buffer_.is_full(log_record->log_tot_len_)) {
        flush_log_to_disk_unlocked();
    }
    log_record->serialize(log_buffer_.buffer_ + log_buffer_.offset_);
    log_buffer_.offset_ += log_record->log_tot_len_;
    // 本条已在缓冲区内，可作为下次刷盘覆盖到的最大 lsn
    buffer_max_lsn_ = lsn;
    return lsn;
}

/**
 * @description: 把日志缓冲区的内容刷到磁盘中，由于目前只设置了一个缓冲区，因此需要阻塞其他日志操作
 */
void LogManager::flush_log_to_disk() {
    std::unique_lock<std::mutex> lock(latch_);
    flush_log_to_disk_unlocked();
}

void LogManager::flush_log_to_disk_unlocked() {
    if (log_buffer_.offset_ == 0) {
        return;
    }
    disk_manager_->write_log(log_buffer_.buffer_, log_buffer_.offset_);
    // The public durability contract requires the WAL write and its stable
    // storage barrier to finish before the corresponding COMMIT ACK. All
    // callers (including TransactionManager::commit) return only after this
    // synchronous barrier succeeds.
    disk_manager_->sync_log();
    log_buffer_.offset_ = 0;
    // 只能宣称"刚写出去的那些记录"已持久化，即缓冲区内最大的 lsn。
    // 不能用 global_lsn_-1：add_log_to_buffer 先 fetch_add 分配 lsn、再在缓冲区
    // 满时刷盘、最后才 serialize，那一瞬间 global_lsn_-1 恰好等于这条"尚未进入
    // 缓冲区"的记录号。若据此更新 persist_lsn_，group_commit 会走快速路径直接
    // 返回，导致 COMMIT ACK 先于该记录落盘——违反可核验持久化契约。
    persist_lsn_ = buffer_max_lsn_;
}

// Group commit 实现：
//  - 本事务 lsn 已被覆盖（persist_lsn_ >= my_lsn）→ 直接返回
//  - 有其它线程正在刷盘 → 等它完成，检查是否已覆盖我的 lsn
//  - 没人刷 → 自己当 leader，释放 latch_ 做 I/O（不阻塞其他线程 add_log），
//    刷完后唤醒所有等待者
void LogManager::group_commit(lsn_t my_lsn) {
    std::unique_lock<std::mutex> lock(latch_);
    // Fast path
    if (persist_lsn_ >= my_lsn) return;
    // Wait for an in-progress flush to cover our LSN
    while (flush_in_progress_ && persist_lsn_ < my_lsn) {
        group_commit_cv_.wait(lock);
    }
    if (persist_lsn_ >= my_lsn) return;
    // Become the flush leader; release latch_ so others can keep adding logs
    // while we do the blocking I/O.
    //
    // flush_in_progress_ 必须由 RAII 复位。原实现在 flush_log_to_disk() 之后才
    // 手写复位，一旦刷盘抛异常（磁盘满、fd 失效、write 短写），该标志就永久停在
    // true：此后每个提交线程都在上面的 while 里等一个永远不会到来的
    // notify_all()，整个服务端静默死锁，对外只表现为 response read timeout。
    // 异常本身必须继续上抛——吞掉它等于在 WAL 没落盘的情况下让 COMMIT 成功返回，
    // 直接违反持久化契约。
    struct FlushLeaderGuard {
        LogManager* mgr;
        ~FlushLeaderGuard() {
            std::unique_lock<std::mutex> relock(mgr->latch_);
            mgr->flush_in_progress_ = false;
            mgr->group_commit_cv_.notify_all();
        }
    };
    flush_in_progress_ = true;
    lock.unlock();
    {
        FlushLeaderGuard guard{this};
        flush_log_to_disk();  // acquires latch_ internally
    }
}
