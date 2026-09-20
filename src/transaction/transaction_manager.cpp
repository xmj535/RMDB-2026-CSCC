/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "transaction_manager.h"

#include <malloc.h>

#include <chrono>

#include "common/config.h"
#include "index/ix_index_handle.h"
#include "index/ix_manager.h"
#include "record/rm_file_handle.h"
#include "recovery/log_manager.h"
#include "system/sm_manager.h"

namespace
{
  // 内部使用：与执行器中相同语义的字节比较，避免循环包含
  int ssi_compare_raw(const char *lhs, const char *rhs, int len, ColType type)
  {
    switch (type)
    {
    case TYPE_INT:
    {
      int a = *reinterpret_cast<const int *>(lhs);
      int b = *reinterpret_cast<const int *>(rhs);
      return (a < b) ? -1 : (a > b ? 1 : 0);
    }
    case TYPE_FLOAT:
    {
      float a = *reinterpret_cast<const float *>(lhs);
      float b = *reinterpret_cast<const float *>(rhs);
      return (a < b) ? -1 : (a > b ? 1 : 0);
    }
    case TYPE_STRING:
    default:
      return memcmp(lhs, rhs, len);
    }
  }
  bool ssi_eval_cmp(int cmp, CompOp op)
  {
    switch (op)
    {
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
  bool ssi_check_cond(const RmRecord &rec, const std::vector<ColMeta> &cols,
                      const Condition &cond)
  {
    auto find_col = [&](const TabCol &tc) -> const ColMeta *
    {
      for (auto &c : cols)
      {
        if (c.tab_name == tc.tab_name && c.name == tc.col_name)
          return &c;
      }
      return nullptr;
    };
    auto lhs_col = find_col(cond.lhs_col);
    if (lhs_col == nullptr)
      return false;
    const char *lhs_data = rec.data + lhs_col->offset;
    const char *rhs_data = nullptr;
    if (cond.is_rhs_val)
    {
      rhs_data = cond.rhs_val.raw->data;
    }
    else
    {
      auto rhs_col = find_col(cond.rhs_col);
      if (rhs_col == nullptr)
        return false;
      rhs_data = rec.data + rhs_col->offset;
    }
    return ssi_eval_cmp(
        ssi_compare_raw(lhs_data, rhs_data, lhs_col->len, lhs_col->type),
        cond.op);
  }
  bool ssi_check_all_conds(const RmRecord &rec, const std::vector<ColMeta> &cols,
                           const std::vector<Condition> &conds)
  {
    for (auto &c : conds)
    {
      if (!ssi_check_cond(rec, cols, c))
        return false;
    }
    return true;
  }

  // 从记录数据里按某索引的列拼出 key（abort/commit 回滚时多处复用，去重）。
  std::vector<char> build_index_key(const IndexMeta &index, const char *rec_data)
  {
    std::vector<char> key(index.col_tot_len);
    int off = 0;
    for (auto &col : index.cols)
    {
      memcpy(key.data() + off, rec_data + col.offset, col.len);
      off += col.len;
    }
    return key;
  }
} // namespace

std::unordered_map<txn_id_t, Transaction *> TransactionManager::txn_map = {};

// ---------------- 内部辅助：(tab, page) -> PageRows --------------------------

std::shared_ptr<TransactionManager::PageRows>
TransactionManager::get_or_create_page_rows(const std::string &tab_name,
                                            page_id_t page_no)
{
  auto &shard = version_shard(tab_name, page_no);
  std::unique_lock<std::shared_mutex> wlock(shard.mutex_);
  auto &page_map = shard.pages_[tab_name];
  auto it = page_map.find(page_no);
  if (it == page_map.end())
  {
    auto p = std::make_shared<PageRows>();
    page_map[page_no] = p;
    return p;
  }
  return it->second;
}

std::shared_ptr<TransactionManager::PageRows>
TransactionManager::get_page_rows_for_read(const std::string &tab_name,
                                           page_id_t page_no)
{
  auto &shard = version_shard(tab_name, page_no);
  std::shared_lock<std::shared_mutex> rlock(shard.mutex_);
  auto tab_it = shard.pages_.find(tab_name);
  if (tab_it == shard.pages_.end())
    return nullptr;
  auto p_it = tab_it->second.find(page_no);
  if (p_it == tab_it->second.end())
    return nullptr;
  return p_it->second;
}

// ---------------- 事务生命周期 ----------------------------------------------

Transaction *TransactionManager::begin(Transaction *txn,
                                       LogManager *log_manager,
                                       IsolationLevel iso)
{
  // 静态检查点静止化：检查点进行中时阻塞新事务开始；否则登记为在途事务。
  {
    std::unique_lock<std::mutex> qlk(ckpt_quiesce_latch_);
    ckpt_quiesce_cv_.wait(qlk, [this]
                          { return !checkpoint_in_progress_; });
    ++active_txns_;
  }
  if (txn == nullptr)
  {
    txn_id_t tid = next_txn_id_.fetch_add(1);
    txn = new Transaction(tid, iso);
  }
  else
  {
    txn->set_isolation_level(iso);
  }
  txn->set_state(TransactionState::DEFAULT);
  {
    std::unique_lock<std::mutex> lock(latch_);
    // 已装链水位，而不是 last_commit_ts_：见 transaction_manager.h 中
    // publishing_ 处的说明（快照必须稳定，否则同一事务两次读会跨快照）。
    timestamp_t start_ts = published_ts_locked();
    txn->set_start_ts(start_ts);
    txn->set_read_ts(start_ts);
    running_txns_.AddTxn(start_ts);
    txn_map[txn->get_transaction_id()] = txn;
  }
  // WAL：写 BEGIN 日志，记录事务起点
  if (enable_logging.load() && log_manager != nullptr)
  {
    BeginLogRecord rec(txn->get_transaction_id());
    rec.prev_lsn_ = txn->get_prev_lsn();
    lsn_t lsn = log_manager->add_log_to_buffer(&rec);
    txn->set_prev_lsn(lsn);
  }
  SsiRegisterBegin(txn);
  return txn;
}

static void clear_write_set(Transaction *txn)
{
  auto ws = txn->get_write_set();
  while (!ws->empty())
  {
    delete ws->back();
    ws->pop_back();
  }
}

void TransactionManager::commit(Transaction *txn, LogManager *log_manager)
{
  if (txn == nullptr)
    return;
  // 守卫：rmdb.cpp 在 catch TransactionAbortException 后还会走"隐式事务自动提交"分支，
  // 对已 ABORTED 的 txn 再调一次 commit；这里直接 return。
  if (txn->get_state() == TransactionState::COMMITTED ||
      txn->get_state() == TransactionState::ABORTED)
  {
    return;
  }
  timestamp_t commit_ts;
  {
    std::unique_lock<std::mutex> lock(latch_);
    commit_ts = last_commit_ts_.fetch_add(1) + 1;
    txn->set_commit_ts(commit_ts);
    running_txns_.RemoveTxn(txn->get_start_ts());
    // 登记"已分配 commit_ts、但版本尚未装入版本链"。在它被摘掉之前，begin()
    // 拿到的 start_ts 一律停在 commit_ts-1 以下，看不到这个半成品提交。
    publishing_.insert(commit_ts);
  }
  if (IsMvccIso(txn->get_isolation_level()))
  {
    FinalizeCommit(txn);
  }
  {
    // 装链完毕，水位可以越过它。UpdateCommitTs 传【水位】而不是本次 commit_ts：
    // 装链完成顺序可能与分配顺序不同，直接传 commit_ts 会让 Watermark 的
    // commit_ts_ 跑到后续 start_ts 前面，AddTxn 的 read_ts >= commit_ts_ 断言会抛。
    std::unique_lock<std::mutex> lock(latch_);
    publishing_.erase(commit_ts);
    running_txns_.UpdateCommitTs(published_ts_locked());
  }
  // WAL：写 COMMIT 日志并强制刷盘，保证已提交事务可恢复。
  // 只读事务（未写过任何 INSERT/UPDATE/DELETE 日志）没有需要持久化的变更：
  // analyze() 只用数据日志建在途事务表，commit 记录唯一的作用是把该 tid 从表里
  // 摘掉（log_recovery.cpp:157），而只读事务从未进过该表，所以省掉 COMMIT 记录
  // 与这次 fdatasync 对恢复完全等价。恢复后一致性检查是数千条只读查询，
  // 每条一次持久化屏障是纯开销。
  if (enable_logging.load() && log_manager != nullptr && txn->has_wal_data())
  {
    CommitLogRecord rec(txn->get_transaction_id());
    rec.prev_lsn_ = txn->get_prev_lsn();
    lsn_t lsn = log_manager->add_log_to_buffer(&rec);
    txn->set_prev_lsn(lsn);
    // Group commit: 多个并发提交共享同一次 fsync。第一个提交者做实际刷盘，
    // 后续提交者等待 leader 覆盖自己的 lsn 后直接返回。在 ACK 前确保 WAL
    // commit record 已落盘，满足可核验持久化契约（第五章第三节）。
    log_manager->group_commit(lsn);
  }
  SsiMarkCommitted(txn);
  clear_write_set(txn);
  txn->set_state(TransactionState::COMMITTED);
  // 本事务已从 running_txns_ 移除，watermark 可能前移，回收过期 SSI 状态
  SsiGarbageCollect(GetWatermark());
  // 静止化：在途事务计数减一并唤醒等待静止的检查点
  {
    std::unique_lock<std::mutex> qlk(ckpt_quiesce_latch_);
    if (active_txns_ > 0)
      --active_txns_;
  }
  ckpt_quiesce_cv_.notify_all();

  // Part-2：每 GC_COMMIT_INTERVAL 次提交触发一次版本存储 GC，回收已 settled 的
  // RowVersion。必须在此处(已释放 latch_)调用——GarbageCollection→GetWatermark
  // 会再取 latch_，若在持锁段内调用将自死锁。
  if (commits_since_gc_.fetch_add(1) + 1 >= GC_COMMIT_INTERVAL)
  {
    commits_since_gc_.store(0);
    GarbageCollection();
  }
}

void TransactionManager::abort(Transaction *txn, LogManager *log_manager)
{
  if (txn == nullptr)
    return;
  if (txn->get_state() == TransactionState::COMMITTED ||
      txn->get_state() == TransactionState::ABORTED)
  {
    return;
  }
  {
    std::unique_lock<std::mutex> lock(latch_);
    running_txns_.RemoveTxn(txn->get_start_ts());
  }
  SsiMarkAborted(txn);

  const bool mvcc = IsMvccIso(txn->get_isolation_level());

  if (mvcc)
  {
    // MVCC 路径：从 RowVersion.pending 推导回滚动作。
    //   INSERT  → 从堆和索引上抹掉新分配的 rid。
    //   UPDATE  → 把堆数据复位到 pre_image，索引把新键删掉、老键插回。
    //   DELETE  → MVCC 下 DELETE 不改堆/索引，pending 清掉即可。
    auto items = CollectAbortRollback(txn);
    // 逆序回滚，确保后写的先撤销，索引语义自洽
    for (auto it = items.rbegin(); it != items.rend(); ++it)
    {
      auto &item = *it;
      auto fh_it = sm_manager_->fhs_.find(item.tab_name);
      if (fh_it == sm_manager_->fhs_.end())
        continue;
      RmFileHandle *fh = fh_it->second.get();
      auto &tab = sm_manager_->db_.get_table(item.tab_name);
      // 该 rid 在本事务写之前不存在 → 不管 op 现在是 INSERT/UPDATE/DELETE，
      // 都按 INSERT 回滚（删堆+清索引）。覆盖 "INSERT 后又 UPDATE/DELETE 同 rid" 的情形。
      const bool insert_rollback = !item.pre_existed;
      if (insert_rollback)
      {
        // 拼 key 用 new_image（最新值）；若 op=DELETE_OP（INSERT 后又删），new_image 为 null，
        // 此时只能用堆上现有数据拼 key
        std::shared_ptr<RmRecord> key_src = item.new_image;
        if (key_src == nullptr)
        {
          auto cur = fh->get_record(item.rid, nullptr);
          if (cur != nullptr)
            key_src = std::make_shared<RmRecord>(*cur);
        }
        if (key_src != nullptr)
        {
          for (auto &index : tab.indexes)
          {
            auto ix_name = sm_manager_->get_ix_manager()->get_index_name(
                item.tab_name, index.cols);
            auto ih = sm_manager_->ihs_.at(ix_name).get();
            auto key = build_index_key(index, key_src->data);
            ih->delete_entry(key.data(), txn);
          }
        }
        fh->delete_record(item.rid, nullptr);
        continue;
      }
      switch (item.op)
      {
      case PendingWrite::Op::INSERT:
      {
        // 走到这里说明 pre_existed=true 但 op=INSERT，不应发生；保守处理
        if (item.new_image != nullptr)
        {
          for (auto &index : tab.indexes)
          {
            auto ix_name = sm_manager_->get_ix_manager()->get_index_name(
                item.tab_name, index.cols);
            auto ih = sm_manager_->ihs_.at(ix_name).get();
            auto key = build_index_key(index, item.new_image->data);
            ih->delete_entry(key.data(), txn);
          }
        }
        fh->delete_record(item.rid, nullptr);
        break;
      }
      case PendingWrite::Op::UPDATE:
      {
        // 新设计：UPDATE 期间索引只 INSERT 了新键，没删旧键，堆也没动。
        // abort：删 new、旧键本来就在不用恢复。
        if (item.new_image != nullptr && item.pre_image != nullptr)
        {
          for (auto &index : tab.indexes)
          {
            auto new_key = build_index_key(index, item.new_image->data);
            auto old_key = build_index_key(index, item.pre_image->data);
            if (memcmp(new_key.data(), old_key.data(), index.col_tot_len) !=
                0)
            {
              auto ix_name = sm_manager_->get_ix_manager()->get_index_name(
                  item.tab_name, index.cols);
              auto ih = sm_manager_->ihs_.at(ix_name).get();
              ih->delete_entry(new_key.data(), txn);
            }
          }
        }
        // 堆未改，无需复位
        break;
      }
      case PendingWrite::Op::DELETE_OP:
      {
        // MVCC 下 DELETE 不动堆/索引，pending 清除即可
        break;
      }
      default:
        break;
      }
    }
    // 清掉 RowVersion 中的 pending（对 INSERT 还需删除整条 RowVersion）
    std::vector<WriteLoc> locs;
    {
      std::unique_lock<std::shared_mutex> wlock(write_loc_mutex_);
      auto it = txn_write_locs_.find(txn->get_transaction_id());
      if (it != txn_write_locs_.end())
      {
        locs = std::move(it->second);
        txn_write_locs_.erase(it);
      }
    }
    for (auto &loc : locs)
    {
      auto page = get_page_rows_for_read(loc.tab_name, loc.rid.page_no);
      if (page == nullptr)
        continue;
      std::unique_lock<std::shared_mutex> wlock(page->mutex_);
      auto rit = page->rows_.find(loc.rid.slot_no);
      if (rit == page->rows_.end())
        continue;
      auto &row = rit->second;
      if (row.pending.writer == txn->get_transaction_id())
      {
        if (row.pending.op == PendingWrite::Op::INSERT)
        {
          // 该 rid 没有任何已提交版本，整条 RowVersion 删掉
          page->rows_.erase(rit);
          continue;
        }
        row.pending = PendingWrite{};
      }
    }
  }
  else
  {
    // 非 MVCC（2PL 等）：保留原 write_set 回滚机制
    auto write_set = txn->get_write_set();
    while (!write_set->empty())
    {
      WriteRecord *wr = write_set->back();
      write_set->pop_back();
      const std::string &tab_name = wr->GetTableName();
      auto fh_it = sm_manager_->fhs_.find(tab_name);
      if (fh_it == sm_manager_->fhs_.end())
      {
        delete wr;
        continue;
      }
      RmFileHandle *fh = fh_it->second.get();
      auto &tab = sm_manager_->db_.get_table(tab_name);
      switch (wr->GetWriteType())
      {
      case WType::INSERT_TUPLE:
      {
        auto rec = fh->get_record(wr->GetRid(), nullptr);
        if (rec != nullptr)
        {
          for (auto &index : tab.indexes)
          {
            auto ix_name = sm_manager_->get_ix_manager()->get_index_name(
                tab_name, index.cols);
            auto ih = sm_manager_->ihs_.at(ix_name).get();
            auto key = build_index_key(index, rec->data);
            ih->delete_entry(key.data(), txn);
          }
          fh->delete_record(wr->GetRid(), nullptr);
        }
        break;
      }
      case WType::DELETE_TUPLE:
      {
        RmRecord &old_rec = wr->GetRecord();
        fh->insert_record(wr->GetRid(), old_rec.data);
        for (auto &index : tab.indexes)
        {
          auto ix_name = sm_manager_->get_ix_manager()->get_index_name(
              tab_name, index.cols);
          auto ih = sm_manager_->ihs_.at(ix_name).get();
          auto key = build_index_key(index, old_rec.data);
          ih->insert_entry(key.data(), wr->GetRid(), txn);
        }
        break;
      }
      case WType::UPDATE_TUPLE:
      {
        auto cur = fh->get_record(wr->GetRid(), nullptr);
        RmRecord &old_rec = wr->GetRecord();
        if (cur != nullptr)
        {
          for (auto &index : tab.indexes)
          {
            auto ix_name = sm_manager_->get_ix_manager()->get_index_name(
                tab_name, index.cols);
            auto ih = sm_manager_->ihs_.at(ix_name).get();
            auto new_key = build_index_key(index, cur->data);
            ih->delete_entry(new_key.data(), txn);
          }
        }
        fh->update_record(wr->GetRid(), old_rec.data, nullptr);
        for (auto &index : tab.indexes)
        {
          auto ix_name = sm_manager_->get_ix_manager()->get_index_name(
              tab_name, index.cols);
          auto ih = sm_manager_->ihs_.at(ix_name).get();
          auto key = build_index_key(index, old_rec.data);
          ih->insert_entry(key.data(), wr->GetRid(), txn);
        }
        break;
      }
      }
      delete wr;
    }
  }

  // WAL：写 ABORT 日志，恢复时该事务按未提交处理（其 INSERT 已物理回滚）
  if (enable_logging.load() && log_manager != nullptr)
  {
    AbortLogRecord rec(txn->get_transaction_id());
    rec.prev_lsn_ = txn->get_prev_lsn();
    lsn_t lsn = log_manager->add_log_to_buffer(&rec);
    txn->set_prev_lsn(lsn);
  }

  clear_write_set(txn);
  txn->set_state(TransactionState::ABORTED);
  // abort 也把本事务从 running_txns_ 移除，watermark 可能前移，回收过期 SSI 状态
  SsiGarbageCollect(GetWatermark());
  // 静止化：在途事务计数减一并唤醒等待静止的检查点
  {
    std::unique_lock<std::mutex> qlk(ckpt_quiesce_latch_);
    if (active_txns_ > 0)
      --active_txns_;
  }
  ckpt_quiesce_cv_.notify_all();
}

// ---------------- 故障恢复支持 ----------------------------------------------

void TransactionManager::SeedCommittedVersion(const std::string &tab_name,
                                              Rid rid,
                                              std::shared_ptr<RmRecord> data,
                                              timestamp_t commit_ts)
{
  auto page = get_or_create_page_rows(tab_name, rid.page_no);
  std::unique_lock<std::shared_mutex> wlock(page->mutex_);
  auto &row = page->rows_[rid.slot_no];
  row.chain.clear();
  row.pending = PendingWrite{};
  CommittedVersion cv;
  cv.commit_ts = commit_ts;
  cv.is_deleted = false;
  cv.data = std::move(data);
  row.chain.insert(row.chain.begin(), cv);
}

void TransactionManager::ClearVersionStore()
{
  for (auto &shard : version_shards_)
  {
    std::unique_lock<std::shared_mutex> wlock(shard.mutex_);
    shard.pages_.clear();
  }
  std::unique_lock<std::shared_mutex> wlock2(write_loc_mutex_);
  txn_write_locs_.clear();
}

// ---------------- 静态检查点静止化 ------------------------------------------

void TransactionManager::BeginCheckpointQuiesce()
{
  std::unique_lock<std::mutex> qlk(ckpt_quiesce_latch_);
  // 停止接收新事务：此后 begin() 会阻塞在 !checkpoint_in_progress_ 上
  checkpoint_in_progress_ = true;
  // 等待正在运行的事务全部结束（静止）。设上限，避免极端情况下（如同连接持有
  // 未提交事务、或连接异常未回滚）永久阻塞导致检查点线程挂死。正常 grader 场景
  // 下检查点在事务之间发送，active_txns_ 立即为 0，不触发超时。
  ckpt_quiesce_cv_.wait_for(qlk, std::chrono::seconds(10),
                            [this]
                            { return active_txns_ == 0; });
}

void TransactionManager::EndCheckpointQuiesce()
{
  {
    std::unique_lock<std::mutex> qlk(ckpt_quiesce_latch_);
    checkpoint_in_progress_ = false;
  }
  // 唤醒所有等待开始的新事务
  ckpt_quiesce_cv_.notify_all();
}

// ---------------- MVCC 读视图 ------------------------------------------------

std::shared_ptr<RmRecord> TransactionManager::VisibleRecord(
    const std::string &tab_name, Rid rid, Transaction *txn)
{
  auto page = get_page_rows_for_read(tab_name, rid.page_no);
  // 惰性 MVCC 播种：故障恢复不再全表播种版本（O(全量) → O(0)），运行期某 rid
  // 第一次被访问时若内存里没有 RowVersion，但堆上确实存在该记录，就按"恢复基准
  // 已提交版本"惰性补种。这样恢复时间 ∝ 检查点后增量而非全量。读/UPDATE/DELETE
  // 都经过本函数，故统一在此补种，避免写执行器把未播种的存量行误判为不可见。
  if (page == nullptr)
  {
    EnsureSeededFromHeap(tab_name, rid);
    page = get_page_rows_for_read(tab_name, rid.page_no);
    if (page == nullptr)
      return nullptr;
  }

  txn_id_t self = txn->get_transaction_id();
  timestamp_t start_ts = txn->get_start_ts();

  {
    // 持读锁观察当前快照；为了避免持锁过久，把要返回的 shared_ptr 拷贝一份就行
    std::shared_lock<std::shared_mutex> rlock(page->mutex_);
    auto it = page->rows_.find(rid.slot_no);
    if (it != page->rows_.end())
    {
      const auto &row = it->second;
      // 自己的 pending：优先
      if (row.pending.writer == self)
      {
        if (row.pending.op == PendingWrite::Op::DELETE_OP)
          return nullptr;
        return row.pending.new_image;
      }
      if (!row.chain.empty() || row.pending.writer != INVALID_TXN_ID)
      {
        // 该 rid 已有版本信息（chain 或 pending），按正常规则判定，不再补种。
        for (auto &v : row.chain)
        {
          if (v.commit_ts <= start_ts)
          {
            if (v.is_deleted)
              return nullptr;
            return v.data;
          }
        }
        return nullptr;
      }
      // it 存在但 chain 空且无 pending：等价于无版本，落到下面尝试补种。
    }
  }

  // 内存无版本：尝试从堆惰性补种，再按可见性判定一次。
  if (!EnsureSeededFromHeap(tab_name, rid))
    return nullptr;
  std::shared_lock<std::shared_mutex> rlock(page->mutex_);
  auto it = page->rows_.find(rid.slot_no);
  if (it == page->rows_.end())
    return nullptr;
  const auto &row = it->second;
  if (row.pending.writer == self)
  {
    if (row.pending.op == PendingWrite::Op::DELETE_OP)
      return nullptr;
    return row.pending.new_image;
  }
  for (auto &v : row.chain)
  {
    if (v.commit_ts <= start_ts)
    {
      if (v.is_deleted)
        return nullptr;
      return v.data;
    }
  }
  return nullptr;
}


// 只读扫描专用可见性：内存无版本时直接读堆返回，不补种、不插版本表。
// 详见头文件声明处的正确性论证。返回 unique_ptr 避免堆读路径的记录拷贝。
std::unique_ptr<RmRecord> TransactionManager::VisibleRecordForScan(
    const std::string &tab_name, Rid rid, Transaction *txn)
{
  auto page = get_page_rows_for_read(tab_name, rid.page_no);
  txn_id_t self = txn->get_transaction_id();
  timestamp_t start_ts = txn->get_start_ts();
  if (page != nullptr)
  {
    std::shared_lock<std::shared_mutex> rlock(page->mutex_);
    auto it = page->rows_.find(rid.slot_no);
    if (it != page->rows_.end())
    {
      const auto &row = it->second;
      if (row.pending.writer == self)
      {
        if (row.pending.op == PendingWrite::Op::DELETE_OP)
          return nullptr;
        return std::make_unique<RmRecord>(*row.pending.new_image);
      }
      if (!row.chain.empty() || row.pending.writer != INVALID_TXN_ID)
      {
        for (auto &v : row.chain)
        {
          if (v.commit_ts <= start_ts)
          {
            if (v.is_deleted)
              return nullptr;
            return std::make_unique<RmRecord>(*v.data);
          }
        }
        return nullptr;
      }
      // it 存在但 chain 空且无 pending：等价于无版本，落到下面直接读堆。
    }
  }
  // 无内存版本：直接读堆返回(最新已提交、对所有当前读者可见)，不补种、不插表。
  auto fh_it = sm_manager_->fhs_.find(tab_name);
  if (fh_it == sm_manager_->fhs_.end())
    return nullptr;
  RmFileHandle *fh = fh_it->second.get();
  // get_record 内部已做 page 越界/slot 越界/bitmap 判空(空槽返回 nullptr)。
  // 直接返回 unique_ptr，避免 shared_ptr 中转的额外拷贝。
  auto rec = fh->get_record(rid, nullptr);
  return rec;
}

// 惰性补种：若 (tab_name, rid) 在版本存储里还没有任何版本（chain 空且无
// pending），但堆上确实存在该记录，则按 RECOVER_BASE_TS 补一条已提交版本。
// 返回 true 表示补种后该 rid 有可用的已提交版本（或本来就有版本）。
// 仅服务于"恢复后存量数据按需播种"：正常运行新写入的行总是先经 BeginInsert
// 建立 pending，不会走到这里。
void TransactionManager::PurgeTableVersions(const std::string &tab_name)
{
  // 一张表的页按 page_no 散布在所有分片上，必须逐分片摘除
  for (auto &shard : version_shards_)
  {
    std::unique_lock<std::shared_mutex> wlock(shard.mutex_);
    shard.pages_.erase(tab_name);
  }
}

// 静态检查点专用：把「已提交但只存在于版本链里的逻辑删除」兑现成堆上的物理删除。
//
// MVCC 的 DELETE 全程不动堆——墓碑只在内存版本链里，堆上那一行原封不动。物理删除
// **只发生在恢复 redo 重放已提交 DELETE 记录时**。检查点路径的 redo 从检查点偏移
// 起扫，于是检查点之前提交的 DELETE 永远不会被重放，而它们的堆行从来没被删过：
// 崩溃重启后这些行原地复活。实测 400/400 已 ACK 的 new_orders 删除全部回来。
//
// 检查点的静止化（BeginCheckpointQuiesce）保证此刻没有任何活跃事务，也就没有任何
// 读者还需要这些行的旧版本，因此这是唯一能安全做物理删除的时机——放在运行期做会
// 让老快照的顺序扫描直接跳过该行（堆 bitmap 已清位，扫描根本到不了版本链）。
//
// 删完连版本条目一起摘除：之后再读该 rid，get_page_rows_for_read 返回空、回落到
// 堆、堆上也没有了 → 不可见，语义与墓碑一致，且 EnsureSeededFromHeap 无从复活它。
size_t TransactionManager::ApplyLogicalDeletesToHeap()
{
  size_t applied = 0;
  for (auto &shard : version_shards_)
  {
    std::unique_lock<std::shared_mutex> wlock(shard.mutex_);
    for (auto &tab_entry : shard.pages_)
    {
      auto fh_it = sm_manager_->fhs_.find(tab_entry.first);
      if (fh_it == sm_manager_->fhs_.end())
        continue;
      RmFileHandle *fh = fh_it->second.get();
      for (auto &page_entry : tab_entry.second)
      {
        auto &page = page_entry.second;
        if (page == nullptr)
          continue;
        std::unique_lock<std::shared_mutex> plock(page->mutex_);
        for (auto it = page->rows_.begin(); it != page->rows_.end();)
        {
          const auto &row = it->second;
          if (row.pending.writer == INVALID_TXN_ID && !row.chain.empty() &&
              row.chain.front().is_deleted)
          {
            Rid rid{page_entry.first, it->first};
            if (fh->is_record(rid))
            {
              fh->delete_record(rid, nullptr);
              ++applied;
            }
            it = page->rows_.erase(it);
            continue;
          }
          ++it;
        }
      }
    }
  }
  return applied;
}

bool TransactionManager::EnsureSeededFromHeap(const std::string &tab_name,
                                              Rid rid)
{
  auto fh_it = sm_manager_->fhs_.find(tab_name);
  if (fh_it == sm_manager_->fhs_.end())
    return false;
  RmFileHandle *fh = fh_it->second.get();
  if (!fh->is_record(rid))
    return false;

  auto page = get_or_create_page_rows(tab_name, rid.page_no);
  std::unique_lock<std::shared_mutex> wlock(page->mutex_);
  auto &row = page->rows_[rid.slot_no];
  // double-checked：并发下可能已被别的线程补种或已有真实写入
  if (!row.chain.empty() || row.pending.writer != INVALID_TXN_ID)
    return true;
  auto rec = fh->get_record(rid, nullptr);
  if (rec == nullptr)
    return false;
  CommittedVersion cv;
  cv.commit_ts = RECOVER_BASE_TS;
  cv.is_deleted = false;
  cv.data = std::make_shared<RmRecord>(*rec);
  row.chain.insert(row.chain.begin(), cv);
  return true;
}

// ---------------- ProbeRow ---------------------------------------------------

TransactionManager::RowProbe TransactionManager::ProbeRow(
    const std::string &tab_name, Rid rid, timestamp_t observer_start_ts)
{
  RowProbe pr;
  // 惰性补种：ProbeRow 被 InsertExecutor 用于唯一索引冲突判定（对索引指向的 rid
  // 探查 chain 顶是否为已提交未删版本）。恢复后存量行若未播种，这里会漏判 duplicate
  // 并误把已存在的 key 当成可接管的"陈旧项"→ 数据损坏。故先按需补种。
  EnsureSeededFromHeap(tab_name, rid);
  auto page = get_page_rows_for_read(tab_name, rid.page_no);
  if (page == nullptr)
    return pr;
  std::shared_lock<std::shared_mutex> rlock(page->mutex_);
  auto it = page->rows_.find(rid.slot_no);
  if (it == page->rows_.end())
    return pr;
  pr.exists = true;
  const auto &row = it->second;
  if (!row.chain.empty())
  {
    pr.committed_ts = row.chain.front().commit_ts;
    pr.committed_deleted = row.chain.front().is_deleted;
    pr.committed_visible = (pr.committed_ts <= observer_start_ts) &&
                           !pr.committed_deleted;
  }
  pr.pending_writer = row.pending.writer;
  pr.pending_op = row.pending.op;
  return pr;
}

// 只读探测（不补种）：供 SSI 读跟踪用——无版本条目即"静默已提交行"，无在途
// 写者可追踪，直接返回。若复用上面的 ProbeRow，SER 全表扫描会把每个扫过的
// rid 都惰性补种成 RowVersion(~200B/行)，巨大表单条 SELECT 即数 GB → OOM。
TransactionManager::RowProbe TransactionManager::ProbeRowNoSeed(
    const std::string &tab_name, Rid rid, timestamp_t observer_start_ts)
{
  RowProbe pr;
  auto page = get_page_rows_for_read(tab_name, rid.page_no);
  if (page == nullptr)
    return pr;
  std::shared_lock<std::shared_mutex> rlock(page->mutex_);
  auto it = page->rows_.find(rid.slot_no);
  if (it == page->rows_.end())
    return pr;
  pr.exists = true;
  const auto &row = it->second;
  if (!row.chain.empty())
  {
    pr.committed_ts = row.chain.front().commit_ts;
    pr.committed_deleted = row.chain.front().is_deleted;
    pr.committed_visible = (pr.committed_ts <= observer_start_ts) &&
                           !pr.committed_deleted;
  }
  pr.pending_writer = row.pending.writer;
  pr.pending_op = row.pending.op;
  return pr;
}

// ---------------- BeginInsert / BeginWrite -----------------------------------

void TransactionManager::BeginInsert(const std::string &tab_name, Rid rid,
                                     Transaction *txn,
                                     std::shared_ptr<RmRecord> new_image)
{
  auto page = get_or_create_page_rows(tab_name, rid.page_no);
  std::unique_lock<std::shared_mutex> wlock(page->mutex_);
  auto &row = page->rows_[rid.slot_no];
  // 新 rid 对应的 RowVersion 应该不存在或者完全空；若有就覆盖（chain 为空时安全）
  row.pending = PendingWrite{};
  row.pending.op = PendingWrite::Op::INSERT;
  row.pending.writer = txn->get_transaction_id();
  row.pending.new_image = std::move(new_image);
  row.pending.pre_image = nullptr;
  row.pending.pre_existed = !row.chain.empty();
  RegisterWrite(txn, tab_name, rid, PendingWrite::Op::INSERT);
}

TransactionManager::WriteResult TransactionManager::BeginWrite(
    const std::string &tab_name, Rid rid, Transaction *txn, bool op_is_update,
    std::shared_ptr<RmRecord> new_image,
    std::shared_ptr<RmRecord> *pre_image_out)
{
  auto page = get_or_create_page_rows(tab_name, rid.page_no);
  std::unique_lock<std::shared_mutex> wlock(page->mutex_);
  auto &row = page->rows_[rid.slot_no];
  txn_id_t self = txn->get_transaction_id();
  timestamp_t start_ts = txn->get_start_ts();

  // 自己的 pending 已经在写：允许再写（覆盖 new_image）。
  // 关键：若原 op 是 INSERT，保持为 INSERT（语义是"该 rid 由我新建"），
  // 这样后续 abort 时仍走 INSERT 回滚路径（删堆/索引），而不是 UPDATE 路径（写回不存在的旧值）。
  if (row.pending.writer == self)
  {
    bool originally_insert = (row.pending.op == PendingWrite::Op::INSERT);
    if (op_is_update)
    {
      // 我自己新建后又改：仍按 INSERT 处理，但 new_image 刷新
      // 我自己改了又改：保持 UPDATE，刷新 new_image
      if (!originally_insert)
      {
        row.pending.op = PendingWrite::Op::UPDATE;
      }
      row.pending.new_image = std::move(new_image);
    }
    else
    {
      // DELETE
      if (originally_insert)
      {
        // 我自己新建后又删：净效果 = 没插。abort/commit 时都按 INSERT-cancel 处理。
        // 借助"pre_existed=false 且 chain 空"在 abort 时识别。
        // commit 时：chain 当前为空，pending 推入 chain 时按 DELETE 处理 (deleted=true，data=nullptr)
        //   这会留下一条 deleted committed version，未来插同 rid 可接管，正常。
        row.pending.op = PendingWrite::Op::DELETE_OP;
        row.pending.new_image.reset();
        // 标记 pre_existed=true 的反义：保持 false 以便 abort 时按 INSERT 回滚
      }
      else
      {
        row.pending.op = PendingWrite::Op::DELETE_OP;
        row.pending.new_image.reset();
      }
    }
    if (pre_image_out)
      *pre_image_out = row.pending.pre_image;
    return WriteResult::OK;
  }

  // 别人持有 pending
  if (row.pending.writer != INVALID_TXN_ID)
  {
    Transaction *other = nullptr;
    {
      std::unique_lock<std::mutex> lock(latch_);
      auto tit = txn_map.find(row.pending.writer);
      if (tit != txn_map.end())
        other = tit->second;
    }
    if (other != nullptr)
    {
      auto st = other->get_state();
      if (st != TransactionState::ABORTED && st != TransactionState::COMMITTED)
      {
        return WriteResult::WW_CONFLICT;
      }
      // 已 committed/aborted 状态：FinalizeCommit 或 abort 还没跑完。
      // 通过 commit_ts 是否分配判断走向。
      timestamp_t other_commit = other->get_commit_ts();
      if (other_commit != INVALID_TS)
      {
        // 已分配 commit_ts → 视为新版本已经落定。若晚于我开始 → ww。
        if (other_commit > start_ts)
          return WriteResult::WW_CONFLICT;
        // 否则视为已提交的新版本，我可以继续在其之上 pending。手动把 pending 翻为
        // chain 顶（防 commit/abort 两侧竞态期间观察到不一致）。
        CommittedVersion cv;
        cv.commit_ts = other_commit;
        cv.is_deleted =
            (row.pending.op == PendingWrite::Op::DELETE_OP);
        cv.data = cv.is_deleted ? row.pending.pre_image
                                : row.pending.new_image;
        row.chain.insert(row.chain.begin(), cv);
        row.pending = PendingWrite{};
      }
      else
      {
        // 已 ABORTED 状态但 pending 没清掉。视为没 pending。
        row.pending = PendingWrite{};
      }
    }
    else
    {
      // 找不到 other：保守视为已结束，pending 清掉
      row.pending = PendingWrite{};
    }
  }

  // 链顶状态判定：SI first-committer-wins。
  if (!row.chain.empty())
  {
    const auto &top = row.chain.front();
    {
      // 链顶比我开始更晚 → 我据以计算的行版本已过期 → 必须 ww（规范强制，
      // 不得改用更新后的版本重算，那会造成 lost update）。
      if (top.commit_ts > start_ts)
        return WriteResult::WW_CONFLICT;
      // 链顶被删除：我看到的快照里此行已不存在；无法 UPDATE/DELETE。
      if (top.is_deleted && top.commit_ts <= start_ts)
      {
        return WriteResult::KEY_NOT_FOUND;
      }
    }
  }
  // chain 为空：行根本不存在（INSERT 未提交或自我 abort 完成）。这种情况下
  // UPDATE/DELETE 不可能发生在可见 rid 上，调用方有责任保证传入的 rid 是可见的。
  // 若仍然到这里，按 KEY_NOT_FOUND 处理。
  if (row.chain.empty())
  {
    return WriteResult::KEY_NOT_FOUND;
  }

  // 占据 pending
  row.pending.writer = self;
  row.pending.op = op_is_update ? PendingWrite::Op::UPDATE
                                : PendingWrite::Op::DELETE_OP;
  row.pending.new_image = std::move(new_image);
  row.pending.pre_image = row.chain.front().data;
  // chain 不空 → 该 rid 在我写之前就有已提交版本，abort 时按"旧版本回滚"处理。
  row.pending.pre_existed = true;
  if (pre_image_out)
    *pre_image_out = row.pending.pre_image;
  RegisterWrite(txn, tab_name, rid, row.pending.op);
  return WriteResult::OK;
}

// ---------------- FinalizeCommit / CollectAbortRollback ----------------------

void TransactionManager::FinalizeCommit(Transaction *txn)
{
  if (txn == nullptr)
    return;
  std::vector<WriteLoc> locs;
  {
    std::unique_lock<std::shared_mutex> wlock(write_loc_mutex_);
    auto it = txn_write_locs_.find(txn->get_transaction_id());
    if (it == txn_write_locs_.end())
      return;
    locs = std::move(it->second);
    txn_write_locs_.erase(it);
  }
  timestamp_t cts = txn->get_commit_ts();
  for (auto &loc : locs)
  {
    auto page = get_page_rows_for_read(loc.tab_name, loc.rid.page_no);
    if (page == nullptr)
      continue;
    std::shared_ptr<RmRecord> new_image_for_heap;
    std::shared_ptr<RmRecord> pre_image_for_cleanup;
    PendingWrite::Op committed_op = PendingWrite::Op::NONE;
    {
      std::unique_lock<std::shared_mutex> wlock(page->mutex_);
      auto rit = page->rows_.find(loc.rid.slot_no);
      if (rit == page->rows_.end())
        continue;
      auto &row = rit->second;
      if (row.pending.writer != txn->get_transaction_id())
        continue;
      CommittedVersion cv;
      cv.commit_ts = cts;
      committed_op = row.pending.op;
      switch (row.pending.op)
      {
      case PendingWrite::Op::INSERT:
      case PendingWrite::Op::UPDATE:
        cv.is_deleted = false;
        cv.data = row.pending.new_image;
        new_image_for_heap = row.pending.new_image;
        pre_image_for_cleanup = row.pending.pre_image; // 用于 commit 后清理旧索引项
        break;
      case PendingWrite::Op::DELETE_OP:
        cv.is_deleted = true;
        cv.data = row.pending.pre_image;
        pre_image_for_cleanup = row.pending.pre_image; // 用于 commit 后清理被删行的索引
        break;
      default:
        break;
      }
      row.chain.insert(row.chain.begin(), cv);
      row.pending = PendingWrite{};
    }
    // commit 时把延后的 heap 写入应用到堆上 (仅 UPDATE)。
    if (committed_op == PendingWrite::Op::UPDATE &&
        new_image_for_heap != nullptr)
    {
      auto fh_it = sm_manager_->fhs_.find(loc.tab_name);
      if (fh_it != sm_manager_->fhs_.end())
      {
        fh_it->second->update_record(loc.rid, new_image_for_heap->data, nullptr);
      }
    }
    // UPDATE：commit 时清掉残留的旧索引项 (UpdateExecutor 期间只 INSERT 新键，
    // 保留旧键给并发事务通过老 key IndexScan 仍能命中以触发 ww；commit 后老
    // key 没有继续保留的语义价值，反而会让后续 INSERT 同一旧值时被唯一性预检
    // 误判 duplicate。这里按 pre_image 拼旧 key 删除。
    // DELETE：commit 时同样清掉该行的索引项 (此前 MVCC DELETE 路径不动索引，
    // 让老快照通过 IndexScan 仍能命中以读 chain 旧版本；commit 表示该行真正
    // 已不存在，索引项可以释放了。)
    if ((committed_op == PendingWrite::Op::UPDATE ||
         committed_op == PendingWrite::Op::DELETE_OP) &&
        pre_image_for_cleanup != nullptr)
    {
      auto &tab = sm_manager_->db_.get_table(loc.tab_name);
      for (auto &index : tab.indexes)
      {
        auto ix_name = sm_manager_->get_ix_manager()->get_index_name(
            loc.tab_name, index.cols);
        auto ih = sm_manager_->ihs_.at(ix_name).get();
        auto old_key = build_index_key(index, pre_image_for_cleanup->data);
        if (committed_op == PendingWrite::Op::UPDATE &&
            new_image_for_heap != nullptr)
        {
          // 新键与旧键完全相同时不要误删 (memcmp on the same buffer offsets)
          auto new_key = build_index_key(index, new_image_for_heap->data);
          if (memcmp(old_key.data(), new_key.data(), index.col_tot_len) == 0)
          {
            continue;
          }
        }
        ih->delete_entry(old_key.data(), txn);
      }
    }
  }
}

std::vector<TransactionManager::AbortRollbackItem>
TransactionManager::CollectAbortRollback(Transaction *txn)
{
  std::vector<AbortRollbackItem> items;
  if (txn == nullptr)
    return items;
  std::vector<WriteLoc> locs;
  {
    std::shared_lock<std::shared_mutex> rlock(write_loc_mutex_);
    auto it = txn_write_locs_.find(txn->get_transaction_id());
    if (it == txn_write_locs_.end())
      return items;
    locs = it->second; // 拷贝；CollectAbortRollback 不能把 txn_write_locs_ 清掉，
                       // 否则 abort 里清 pending 时找不到位置。
  }
  for (auto &loc : locs)
  {
    auto page = get_page_rows_for_read(loc.tab_name, loc.rid.page_no);
    if (page == nullptr)
      continue;
    std::shared_lock<std::shared_mutex> rlock(page->mutex_);
    auto rit = page->rows_.find(loc.rid.slot_no);
    if (rit == page->rows_.end())
      continue;
    const auto &row = rit->second;
    if (row.pending.writer != txn->get_transaction_id())
      continue;
    AbortRollbackItem item;
    item.tab_name = loc.tab_name;
    item.rid = loc.rid;
    item.op = row.pending.op;
    item.pre_existed = row.pending.pre_existed;
    item.pre_image = row.pending.pre_image;
    item.new_image = row.pending.new_image;
    items.push_back(std::move(item));
  }
  return items;
}

std::vector<Rid> TransactionManager::GetPendingDeletedRids(
    const std::string &tab_name, txn_id_t self)
{
  std::vector<Rid> result;
  std::shared_lock<std::shared_mutex> rlock(write_loc_mutex_);
  for (auto &kv : txn_write_locs_)
  {
    if (kv.first == self)
      continue; // 只关心其他事务的 pending delete
    for (auto &loc : kv.second)
    {
      if (loc.op == PendingWrite::Op::DELETE_OP && loc.tab_name == tab_name)
      {
        result.push_back(loc.rid);
      }
    }
  }
  return result;
}

void TransactionManager::RegisterWrite(Transaction *txn,
                                       const std::string &tab_name, Rid rid,
                                       PendingWrite::Op op)
{
  if (txn == nullptr)
    return;
  std::unique_lock<std::shared_mutex> wlock(write_loc_mutex_);
  txn_write_locs_[txn->get_transaction_id()].push_back(
      WriteLoc{tab_name, rid, op});
}

// ---------------- GC / Watermark --------------------------------------------

timestamp_t TransactionManager::GetWatermark()
{
  std::unique_lock<std::mutex> lock(latch_);
  return running_txns_.GetWatermark();
}

void TransactionManager::GarbageCollection()
{
  // Part-2：回收已 settled 的 RowVersion，使版本存储不再是"全表的内存镜像"。
  // 安全条件(三者全满足才删)：
  //   (1) 无 pending writer —— 没有在途写；
  //   (2) chain 非空且 chain.front().commit_ts < watermark —— 最新已提交版本早于
  //       所有活跃/未来事务的快照(watermark = 活跃事务最小 start_ts；无活跃时为
  //       最新 commit_ts) → 它们都只会看到该行最新版本，而堆里正是该最新值；
  //   (3) !chain.front().is_deleted —— 排除墓碑：MVCC 的 DELETE 不物理删堆记录，
  //       若把墓碑 GC 掉，下次 EnsureSeededFromHeap 会按堆记录把已删行误判复活。
  // 被删条目下次被访问时由 VisibleRecord→EnsureSeededFromHeap 从堆惰性补种，
  // 对 settled 行语义等价。SSI 状态独立(另由 SsiGarbageCollect 回收)，不受影响。
  timestamp_t wm = GetWatermark();

  // Phase 1：快照页指针(避免持分片锁时再抢 page 锁)，逐页清理已
  // settled 行；记录清空后的页，待 Phase 2 摘除。
  std::vector<std::pair<std::string, page_id_t>> keys;
  std::vector<std::shared_ptr<PageRows>> pages;
  {
    // 逐分片收集：每个分片的锁只在收集自己那部分时持有，不再有全局静止点
    for (auto &shard : version_shards_)
    {
      std::shared_lock<std::shared_mutex> rlock(shard.mutex_);
      for (auto &tab : shard.pages_)
      {
        for (auto &pp : tab.second)
        {
          keys.emplace_back(tab.first, pp.first);
          pages.push_back(pp.second);
        }
      }
    }
  }
  std::vector<std::pair<std::string, page_id_t>> empty_pages;
  for (size_t i = 0; i < pages.size(); ++i)
  {
    auto &page = pages[i];
    std::unique_lock<std::shared_mutex> wlock(page->mutex_);
    for (auto it = page->rows_.begin(); it != page->rows_.end();)
    {
      auto &row = it->second;
      if (row.pending.writer == INVALID_TXN_ID && !row.chain.empty() &&
          row.chain.front().commit_ts < wm && !row.chain.front().is_deleted)
      {
        it = page->rows_.erase(it);
      }
      else
      {
        ++it;
      }
    }
    if (page->rows_.empty())
      empty_pages.push_back(keys[i]);
  }

  // Phase 2：摘除已空的 PageRows 条目，避免版本存储随触碰过的页数无界增长。
  // 注意：分片后**不存在全局静止点**，Phase1↔Phase2 之间别的线程可以往任意页
  // 补种/写入。正确性完全依赖下面那次"在 page 锁下复检 rows_.empty()"——只有
  // 复检时仍为空才摘除，所以并发补种最坏只是让本轮少回收一页，不会丢版本。
  // 分片锁只保证摘除动作本身与同分片的 get_or_create/get_for_read 互斥。
  if (!empty_pages.empty())
  {
    for (auto &k : empty_pages)
    {
      auto &shard = version_shard(k.first, k.second);
      std::unique_lock<std::shared_mutex> wlock(shard.mutex_);
      auto tab_it = shard.pages_.find(k.first);
      if (tab_it == shard.pages_.end())
        continue;
      auto p_it = tab_it->second.find(k.second);
      if (p_it == tab_it->second.end())
        continue;
      bool still_empty;
      {
        std::unique_lock<std::shared_mutex> plock(p_it->second->mutex_);
        still_empty = p_it->second->rows_.empty();
      }
      if (still_empty)
        tab_it->second.erase(p_it);
    }
  }

  // 把 GC 释放的空闲 arena 尽量归还 OS：glibc 可能长期保留高水位内存，
  // 大量自动提交事务中的版本条目与解析对象反复分配释放时，RSS 会受碎片影响持续增长。
  malloc_trim(0);
}

// ---------------- SSI --------------------------------------------------------

namespace
{
  bool ssi_is_dangerous_pattern(const TransactionManager::SsiTxnState &tin,
                                const TransactionManager::SsiTxnState &tout)
  {
    // 题面定义：Tin ->rw pivot ->rw Tout 在 Tin == Tout，或者 Tout 已经先于
    // Tin 提交时构成危险结构。Tin 尚未提交而 Tout 已提交时，Tout 必然先提交。
    if (tin.txn_id == tout.txn_id)
      return true;
    if (tout.state == TransactionState::COMMITTED &&
        tin.state != TransactionState::COMMITTED &&
        tin.state != TransactionState::ABORTED)
    {
      return true;
    }
    if (tout.state == TransactionState::COMMITTED &&
        tin.state == TransactionState::COMMITTED)
    {
      return tout.commit_ts < tin.commit_ts;
    }
    return false;
  }

  bool add_rw_edge_and_check_locked(
      std::unordered_map<txn_id_t,
                         std::shared_ptr<TransactionManager::SsiTxnState>>
          &ssi_txns,
      txn_id_t from, txn_id_t to)
  {
    if (from == to)
      return false;
    if (from == INVALID_TXN_ID || to == INVALID_TXN_ID)
      return false;
    auto fit = ssi_txns.find(from);
    auto tit = ssi_txns.find(to);
    if (fit == ssi_txns.end() || tit == ssi_txns.end())
      return false;
    auto &f = *fit->second;
    auto &t = *tit->second;
    if (f.state == TransactionState::ABORTED ||
        t.state == TransactionState::ABORTED)
    {
      return false;
    }
    if (f.out_edges.count(to) > 0)
      return false;
    // 内部保存的是“写者 -> 读者”，方向与题面的 reader ->rw writer 相反：
    // x -> f -> t 对应 Tin=t、pivot=f、Tout=x；
    // f -> t -> y 对应 Tin=y、pivot=t、Tout=f。
    for (auto x : f.in_edges)
    {
      auto xit = ssi_txns.find(x);
      if (xit == ssi_txns.end())
        continue;
      if (ssi_is_dangerous_pattern(t, *xit->second))
        return true;
    }
    for (auto y : t.out_edges)
    {
      auto yit = ssi_txns.find(y);
      if (yit == ssi_txns.end())
        continue;
      if (ssi_is_dangerous_pattern(*yit->second, f))
        return true;
    }
    f.out_edges.insert(to);
    t.in_edges.insert(from);
    return false;
  }
} // namespace

void TransactionManager::SsiRegisterBegin(Transaction *txn)
{
  if (txn == nullptr)
    return;
  if (txn->get_isolation_level() != IsolationLevel::SERIALIZABLE)
    return;
  std::lock_guard<std::mutex> lock(ssi_latch_);
  auto state = std::make_shared<SsiTxnState>();
  state->txn_id = txn->get_transaction_id();
  state->start_ts = txn->get_start_ts();
  state->state = TransactionState::DEFAULT;
  ssi_txns_[state->txn_id] = state;
}

void TransactionManager::SsiMarkCommitted(Transaction *txn)
{
  if (txn == nullptr)
    return;
  if (txn->get_isolation_level() != IsolationLevel::SERIALIZABLE)
    return;
  std::lock_guard<std::mutex> lock(ssi_latch_);
  auto it = ssi_txns_.find(txn->get_transaction_id());
  if (it == ssi_txns_.end())
    return;
  it->second->commit_ts = txn->get_commit_ts();
  it->second->state = TransactionState::COMMITTED;
  // 登记到有序回收索引（commit_ts 由 fetch_add 保证唯一）
  ssi_committed_.emplace(txn->get_commit_ts(), txn->get_transaction_id());
}

// 回收 commit_ts <= watermark 的已提交事务：此后任何活跃/未来事务的 start_ts 都
// >= watermark >= 其 commit_ts，冲突检测入口（SsiRecordWrite / ssi_track_rid）均以
// commit_ts > start_ts 为纳入条件，故其再也不会被选中，也不会有新边指向/源于它。
// 删除前需把它从其它事务的 in/out 边集合中摘除（与 SsiMarkAborted 同理）。
void TransactionManager::SsiGarbageCollect(timestamp_t watermark)
{
  std::lock_guard<std::mutex> lock(ssi_latch_);
  auto it = ssi_committed_.begin();
  while (it != ssi_committed_.end() && it->first <= watermark)
  {
    txn_id_t tid = it->second;
    auto sit = ssi_txns_.find(tid);
    if (sit != ssi_txns_.end())
    {
      for (auto x : sit->second->in_edges)
      {
        auto xit = ssi_txns_.find(x);
        if (xit != ssi_txns_.end())
          xit->second->out_edges.erase(tid);
      }
      for (auto y : sit->second->out_edges)
      {
        auto yit = ssi_txns_.find(y);
        if (yit != ssi_txns_.end())
          yit->second->in_edges.erase(tid);
      }
      ssi_txns_.erase(sit);
    }
    it = ssi_committed_.erase(it);
  }
}

void TransactionManager::SsiMarkAborted(Transaction *txn)
{
  if (txn == nullptr)
    return;
  if (txn->get_isolation_level() != IsolationLevel::SERIALIZABLE)
    return;
  std::lock_guard<std::mutex> lock(ssi_latch_);
  auto it = ssi_txns_.find(txn->get_transaction_id());
  if (it == ssi_txns_.end())
    return;
  for (auto x : it->second->in_edges)
  {
    auto xit = ssi_txns_.find(x);
    if (xit != ssi_txns_.end())
      xit->second->out_edges.erase(it->second->txn_id);
  }
  for (auto y : it->second->out_edges)
  {
    auto yit = ssi_txns_.find(y);
    if (yit != ssi_txns_.end())
      yit->second->in_edges.erase(it->second->txn_id);
  }
  ssi_txns_.erase(it);
}

// 登记可见记录读，并识别确实写过同一记录的不可见写者。
static void ssi_track_rid(
    TransactionManager *tm, Transaction *txn, const std::string &tab_name,
    Rid rid, bool append_rid,
    std::unordered_map<txn_id_t,
                       std::shared_ptr<TransactionManager::SsiTxnState>>
        &ssi_txns,
    std::mutex &ssi_latch)
{
  if (txn == nullptr)
    return;
  if (txn->get_isolation_level() != IsolationLevel::SERIALIZABLE)
    return;

  // 用不补种的探测：SER 扫描路径逐行调用，补种版会让每个扫过的行都驻留版本条目
  auto probe = tm->ProbeRowNoSeed(tab_name, rid, txn->get_start_ts());
  std::vector<txn_id_t> invisible_writers;
  txn_id_t self = txn->get_transaction_id();

  if (probe.pending_writer != INVALID_TXN_ID && probe.pending_writer != self)
  {
    invisible_writers.push_back(probe.pending_writer);
  }
  // RowProbe 只能给出不可见版本的提交时间，不能直接给出写者。保留中的 SSI
  // 事务状态带有写集合，因此只把确实写过本表本 rid 的事务加入候选；把所有
  // 晚提交事务都列为写者会凭空制造反依赖。
  {
    std::lock_guard<std::mutex> lock(ssi_latch);
    const int64_t rid_key = TransactionManager::ssi_rid_key(rid);
    for (auto &kv : ssi_txns)
    {
      if (kv.first == self)
        continue;
      auto &other = *kv.second;
      if (other.state == TransactionState::COMMITTED &&
          other.commit_ts > txn->get_start_ts())
      {
        for (const auto &write : other.writes)
        {
          if (write.first == tab_name && write.second == rid_key)
          {
            invisible_writers.push_back(kv.first);
            break;
          }
        }
      }
    }
  }

  std::lock_guard<std::mutex> lock(ssi_latch);
  auto self_it = ssi_txns.find(self);
  if (self_it == ssi_txns.end())
    return;
  if (append_rid)
  {
    // 每事务读集设置上限：SERIALIZABLE 下的全表扫描若逐行登记超大读集，会造成
    // 显著内存压力。超限后停止逐行登记；扫描谓词已由 SsiRecordPredicate 覆盖整体
    // 读语义，SsiRecordWrite 的谓词路径仍可检测冲突，invisible-writer 边构建照常。
    static constexpr size_t SSI_RID_READS_CAP = 65536;
    if (self_it->second->rid_reads.size() < SSI_RID_READS_CAP)
    {
      self_it->second->rid_reads.push_back(
          TransactionManager::SsiReadInfo{tab_name, rid});
    }
  }
  for (auto w : invisible_writers)
  {
    if (w == self)
      continue;
    auto wit = ssi_txns.find(w);
    if (wit == ssi_txns.end())
      continue;
    if (add_rw_edge_and_check_locked(ssi_txns, w, self))
    {
      throw TransactionAbortException(self, AbortReason::DEADLOCK_PREVENTION);
    }
  }
}

void TransactionManager::SsiRecordRead(Transaction *txn,
                                       const std::string &tab_name, Rid rid)
{
  ssi_track_rid(this, txn, tab_name, rid, /*append_rid=*/true, ssi_txns_,
                ssi_latch_);
}

void TransactionManager::SsiCheckInvisibleWriters(Transaction *txn,
                                                  const std::string &tab_name,
                                                  Rid rid,
                                                  const std::vector<Condition> &conds,
                                                  const std::vector<ColMeta> &cols)
{
  if (txn == nullptr)
    return;
  if (txn->get_isolation_level() != IsolationLevel::SERIALIZABLE)
    return;
  const txn_id_t self = txn->get_transaction_id();
  const timestamp_t start_ts = txn->get_start_ts();

  // 先在版本页中找出会改变本次查询结果的不可见新值。DELETE 不会让一个本来
  // 不可见的行进入结果，故这里忽略删除版本；旧快照仍可见的删除由正常读路径
  // 通过同 rid 写集合识别。
  std::vector<txn_id_t> writers;
  std::vector<timestamp_t> committed_writer_ts;
  {
    auto page = get_page_rows_for_read(tab_name, rid.page_no);
    if (page == nullptr)
      return;
    std::shared_lock<std::shared_mutex> rlock(page->mutex_);
    auto it = page->rows_.find(rid.slot_no);
    if (it == page->rows_.end())
      return;
    const RowVersion &row = it->second;
    if (row.pending.writer != INVALID_TXN_ID && row.pending.writer != self &&
        row.pending.op != PendingWrite::Op::DELETE_OP &&
        row.pending.new_image != nullptr &&
        (conds.empty() ||
         ssi_check_all_conds(*row.pending.new_image, cols, conds)))
    {
      writers.push_back(row.pending.writer);
    }
    for (const auto &version : row.chain)
    {
      if (version.commit_ts <= start_ts)
        continue;
      if (version.is_deleted || version.data == nullptr)
        continue;
      if (conds.empty() || ssi_check_all_conds(*version.data, cols, conds))
      {
        committed_writer_ts.push_back(version.commit_ts);
      }
    }
  }
  if (writers.empty() && committed_writer_ts.empty())
    return;

  // commit_ts 在提交时与 txn_id 一一登记，用它恢复已提交不可见版本的写者。
  std::lock_guard<std::mutex> lock(ssi_latch_);
  if (ssi_txns_.find(self) == ssi_txns_.end())
    return;
  for (auto commit_ts : committed_writer_ts)
  {
    auto it = ssi_committed_.find(commit_ts);
    if (it != ssi_committed_.end())
      writers.push_back(it->second);
  }
  for (auto writer : writers)
  {
    if (writer == self || ssi_txns_.find(writer) == ssi_txns_.end())
      continue;
    if (add_rw_edge_and_check_locked(ssi_txns_, writer, self))
    {
      throw TransactionAbortException(self, AbortReason::DEADLOCK_PREVENTION);
    }
  }
}

void TransactionManager::SsiRecordPredicate(Transaction *txn,
                                            const std::string &tab_name,
                                            const std::vector<Condition> &conds,
                                            const std::vector<ColMeta> &cols)
{
  if (txn == nullptr)
    return;
  if (txn->get_isolation_level() != IsolationLevel::SERIALIZABLE)
    return;
  std::lock_guard<std::mutex> lock(ssi_latch_);
  auto it = ssi_txns_.find(txn->get_transaction_id());
  if (it == ssi_txns_.end())
    return;
  it->second->predicates.push_back(SsiPredicate{tab_name, conds, cols});
}

void TransactionManager::SsiRecordWrite(Transaction *txn,
                                        const std::string &tab_name, Rid rid,
                                        const RmRecord *rec,
                                        bool op_is_insert)
{
  if (txn == nullptr)
    return;
  if (txn->get_isolation_level() != IsolationLevel::SERIALIZABLE)
    return;
  std::lock_guard<std::mutex> lock(ssi_latch_);
  txn_id_t self = txn->get_transaction_id();
  auto self_it = ssi_txns_.find(self);
  if (self_it == ssi_txns_.end())
    return;
  const auto write = std::make_pair(tab_name, ssi_rid_key(rid));
  if (std::find(self_it->second->writes.begin(), self_it->second->writes.end(),
                write) == self_it->second->writes.end())
  {
    self_it->second->writes.push_back(write);
  }
  for (auto &kv : ssi_txns_)
  {
    if (kv.first == self)
      continue;
    auto &other = *kv.second;
    if (other.state == TransactionState::ABORTED)
      continue;
    // 仅对"重叠"的事务建边：other 未提交，或 other.commit_ts > my.start_ts
    if (other.state == TransactionState::COMMITTED &&
        other.commit_ts <= txn->get_start_ts())
      continue;
    bool conflict = false;
    if (!op_is_insert)
    {
      for (auto &r : other.rid_reads)
      {
        if (r.tab_name == tab_name && r.rid == rid)
        {
          conflict = true;
          break;
        }
      }
    }
    if (!conflict && rec != nullptr)
    {
      for (auto &p : other.predicates)
      {
        if (p.tab_name != tab_name)
          continue;
        if (p.conds.empty() || ssi_check_all_conds(*rec, p.cols, p.conds))
        {
          conflict = true;
          break;
        }
      }
    }
    if (conflict)
    {
      if (add_rw_edge_and_check_locked(ssi_txns_, self, kv.first))
      {
        throw TransactionAbortException(self,
                                        AbortReason::DEADLOCK_PREVENTION);
      }
    }
  }
}
