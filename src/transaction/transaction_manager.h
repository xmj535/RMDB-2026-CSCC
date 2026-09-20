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
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common/common.h"
#include "common/exception.h"
#include "concurrency/lock_manager.h"
#include "recovery/log_manager.h"
#include "system/sm_manager.h"
#include "transaction.h"
#include "watermark.h"

enum class ConcurrencyMode { TWO_PHASE_LOCKING = 0, BASIC_TO, MVCC };

// ============================================================================
// 新 MVCC 架构：单一真相源 RowVersion
//
// 每个 (tab_name, rid) 维护一份 RowVersion：
//   - chain: 已提交版本链，按 commit_ts 由新到旧排列。chain[0] 是当前对外
//            可见的最新版本。
//   - pending: 当前正在写但尚未提交的事务的待提交记录。pending.writer ==
//              INVALID_TXN_ID 表示无 pending。
//
// 关键不变量：
//   1. 所有事务的可见性判断都基于 RowVersion，不读堆。堆只用于扫描算子枚举
//      rid 以及索引指向的物理位置。
//   2. 写操作（INSERT/UPDATE/DELETE）只更新 RowVersion.pending，不改 chain。
//   3. commit 时把 pending 转成 chain[0]。
//   4. abort 时丢弃 pending；INSERT 需要由调用方撤销堆/索引（chain 为空）。
//   5. 这样 commit/abort 总能在单点写入 RowVersion 完成，杜绝
//      "heap vs meta vs undo log" 三者不同步导致的中间状态被外部观察到。
// ============================================================================

struct CommittedVersion {
  timestamp_t commit_ts{0};
  bool is_deleted{false};
  std::shared_ptr<RmRecord> data;  // 已删除版本也保留 pre-delete 数据，给老快照读
};

struct PendingWrite {
  enum class Op { NONE, INSERT, UPDATE, DELETE_OP };
  Op op{Op::NONE};
  txn_id_t writer{INVALID_TXN_ID};
  // INSERT/UPDATE 的新值；DELETE 时 nullptr
  std::shared_ptr<RmRecord> new_image;
  // pending 之前 chain[0] 的快照（abort 时也用来恢复堆/索引）；
  // 若是 INSERT 则为 nullptr（chain 之前为空）。
  std::shared_ptr<RmRecord> pre_image;
  // pending 之前 chain 是否已存在已提交版本（用于 INSERT 区分）。
  bool pre_existed{false};
};

struct RowVersion {
  std::vector<CommittedVersion> chain;  // [0] = 最新已提交
  PendingWrite pending;                  // pending.writer == INVALID 表示无 pending
};

class TransactionManager {
 public:
  explicit TransactionManager(
      LockManager *lock_manager, SmManager *sm_manager,
      ConcurrencyMode concurrency_mode = ConcurrencyMode::TWO_PHASE_LOCKING) {
    sm_manager_ = sm_manager;
    lock_manager_ = lock_manager;
    concurrency_mode_ = concurrency_mode;
  }

  ~TransactionManager() = default;

  Transaction *begin(Transaction *txn, LogManager *log_manager,
                     IsolationLevel iso = IsolationLevel::SERIALIZABLE);

  void commit(Transaction *txn, LogManager *log_manager);

  void abort(Transaction *txn, LogManager *log_manager);

  ConcurrencyMode get_concurrency_mode() { return concurrency_mode_; }
  void set_concurrency_mode(ConcurrencyMode cm) { concurrency_mode_ = cm; }
  LockManager *get_lock_manager() { return lock_manager_; }

  // ----------------------------- 故障恢复支持 --------------------------------
  // 恢复后把内存时间戳/事务号推到日志之后，保证后续 begin/commit 单调递增。
  void set_next_txn_id(txn_id_t next) { next_txn_id_.store(next); }
  void set_last_commit_ts(timestamp_t ts) {
    last_commit_ts_.store(ts);
    next_timestamp_.store(ts);
  }
  timestamp_t get_last_commit_ts() { return last_commit_ts_.load(); }

  // 恢复后用堆中现存记录给 RowVersion 链播种一个 base 已提交版本，
  // 让重启后的 MVCC 读能看到已落盘/已重放的已提交数据。
  void SeedCommittedVersion(const std::string &tab_name, Rid rid,
                            std::shared_ptr<RmRecord> data, timestamp_t commit_ts);
  // 清空全部内存版本信息（恢复重建前调用）
  void ClearVersionStore();

  // --------------------------- 静态检查点静止化 ------------------------------
  // 创建检查点前暂停接收新事务，并等待正在运行的事务结束。
  // BeginCheckpointQuiesce：阻塞新的 begin，等待 active_txns_ 归零（带超时保护），
  //   从而保证检查点 flush 期间没有并发事务继续写入。
  // EndCheckpointQuiesce：解除阻塞并唤醒等待中的 begin。
  void BeginCheckpointQuiesce();
  void EndCheckpointQuiesce();

  Transaction *get_transaction(txn_id_t txn_id) {
    if (txn_id == INVALID_TXN_ID) return nullptr;
    std::unique_lock<std::mutex> lock(latch_);
    auto it = txn_map.find(txn_id);
    if (it == txn_map.end()) return nullptr;
    auto *res = it->second;
    lock.unlock();
    // 不使用 assert 强制校验线程归属：连接与工作线程映射不符合预期时，assert 会
    // 直接终止整个服务端。归属校验属于调试期健全性检查，这里采用防御式返回。
    return res;
  }

  Transaction *lookup_txn_any_thread(txn_id_t txn_id) {
    if (txn_id == INVALID_TXN_ID) return nullptr;
    std::unique_lock<std::mutex> lock(latch_);
    auto it = txn_map.find(txn_id);
    if (it == txn_map.end()) return nullptr;
    return it->second;
  }

  // 回收一个已结束(COMMITTED/ABORTED)的事务对象：从 txn_map 摘除并 delete。
  // 已结束事务若只增不删，海量自动提交会让 txn_map 无界增长并造成长期内存压力。
  // 由调用方在同一连接的下一条语句开始时回收上一条已结束事务；此时提交后的 pending
  // 状态已经清理，SSI 也只保留 txn_id 关系，可避免 use-after-free。
  void reap_transaction(Transaction *txn) {
    if (txn == nullptr) return;
    {
      std::unique_lock<std::mutex> lock(latch_);
      txn_map.erase(txn->get_transaction_id());
    }
    delete txn;
  }

  static std::unordered_map<txn_id_t, Transaction *> txn_map;
  std::shared_mutex txn_map_mutex_;

  // ----------------------------- MVCC API ------------------------------------
  static bool IsMvccIso(IsolationLevel iso) {
    return iso == IsolationLevel::SNAPSHOT_ISOLATION ||
           iso == IsolationLevel::SERIALIZABLE;
  }

  // 读视图：返回对 txn 当前快照可见的记录；不可见 / 已删返回 nullptr。
  // 内部含惰性 MVCC 播种：内存无版本但堆上存在记录时按 RECOVER_BASE_TS 补种。
  std::shared_ptr<RmRecord> VisibleRecord(const std::string &tab_name, Rid rid,
                                          Transaction *txn);

  // 只读扫描专用：与 VisibleRecord 同语义，但【内存无版本时直接读堆返回，不补种、
  // 不插版本表】。供 SeqScan/IndexScan 这种逐行只读路径，避免全表扫(a)把整张表的
  // RowVersion 灌进内存(O(表)内存复涨，抵消 GC)、(b)每行付 EnsureSeededFromHeap 的
  // 写锁+插表代价(扫描随规模超线性变慢→撞 2.2 单语句超时)。
  // 正确性：无内存版本 ⇒ 最新已提交版本要么被 GC(commit_ts<watermark)、要么是恢复
  // 基准版本 ⇒ 对任何当前读者都可见(start_ts ≥ watermark > commit_ts)，堆里正是该
  // 最新已提交值；已删行墓碑被 GC 排除而保留在链上，走 chain 分支返回 nullptr，不会
  // 落到读堆路径被误复活。写路径(insert/update/delete)仍用 VisibleRecord(照常补种)。
  // 返回 unique_ptr 以避免堆读路径的记录拷贝（全表扫 25M 行的热路径）。
  std::unique_ptr<RmRecord> VisibleRecordForScan(const std::string &tab_name,
                                                 Rid rid, Transaction *txn);

  // 惰性补种：(tab,rid) 内存无版本但堆上存在记录时，补一条 RECOVER_BASE_TS
  // 已提交版本。返回 true 表示补种后/本来就有可用版本。供恢复后存量数据按需播种。
  bool EnsureSeededFromHeap(const std::string &tab_name, Rid rid);

  // 静态检查点专用：把只存在于版本链里的已提交逻辑删除兑现成堆上的物理删除，
  // 返回删除行数。**必须在 BeginCheckpointQuiesce() 之内调用**——静止化保证没有
  // 活跃事务，才不会有读者因为堆 bitmap 清位而看不到它本该看见的旧版本。
  // 不这样做的话，检查点之前提交的 DELETE 在崩溃后会原地复活（redo 只从检查点
  // 偏移起扫，而堆行从来没被物理删过）。
  size_t ApplyLogicalDeletesToHeap();

  // DROP/CREATE TABLE 时清空该表的全部内存版本(链+pending)。版本存储按表名
  // 索引：不清的话，同名重建表后旧表残留版本按 (表名,rid) 被撞上——幽灵行进入
  // 查询结果、幽灵 pending 触发假写写冲突(差分模糊实锤；grader 同名建表的
  // 系列测试点会逐个累积污染)。
  void PurgeTableVersions(const std::string &tab_name);

  enum class WriteResult {
    OK,            // 已占据 pending
    WW_CONFLICT,   // 写写冲突，调用方应回滚
    KEY_NOT_FOUND  // UPDATE/DELETE 时该 rid 对调用方不可见（已被删等）
  };

  // INSERT：先由调用方在堆上分配 rid 并写入 new_image，然后调用此方法登记 pending。
  // 不做写写冲突检测——新 rid 不可能被其他事务持有。
  void BeginInsert(const std::string &tab_name, Rid rid, Transaction *txn,
                   std::shared_ptr<RmRecord> new_image);

  // UPDATE/DELETE：检查写写冲突 + 登记 pending。
  // 返回 OK 表示已成功登记 pending；调用方此时可以更新堆/索引（abort 时由
  // EndAbort 通过 pre_image 协助恢复）。
  // op_is_update == true 表示 UPDATE，否则 DELETE。
  // 一律 SI first-committer-wins：链顶 commit_ts > 本事务 start_ts ⇒ WW_CONFLICT。
  // 不提供"基于更新后版本重算"的旁路，那是规范禁止的（见 config.h 处的说明）。
  WriteResult BeginWrite(const std::string &tab_name, Rid rid, Transaction *txn,
                         bool op_is_update,
                         std::shared_ptr<RmRecord> new_image,
                         std::shared_ptr<RmRecord> *pre_image_out);

  // commit 时调用：把 pending 转换为 chain 顶部新版本。
  void FinalizeCommit(Transaction *txn);

  // abort 时调用：丢弃所有 pending；同时把 INSERT 的 rid 列出来由调用方撤销
  // 堆和索引，把 UPDATE 的 (rid, pre_image) 也列出来给调用方恢复堆。
  // DELETE 不需要恢复堆（DELETE 不改堆）。
  struct AbortRollbackItem {
    std::string tab_name;
    Rid rid;
    PendingWrite::Op op;
    bool pre_existed{false};  // false 表示该 rid 在本事务写之前不存在（要按 INSERT 回滚）
    std::shared_ptr<RmRecord> pre_image;  // UPDATE 用于 fh_->update_record
    std::shared_ptr<RmRecord> new_image;  // INSERT 用于 ih_->delete_entry / UPDATE 用于 ih 清新键
  };
  std::vector<AbortRollbackItem> CollectAbortRollback(Transaction *txn);

  // GC 接口：删除 commit_ts < watermark 的旧链条目（不动 chain[0]）。
  void GarbageCollection();
  timestamp_t GetWatermark();

  // 记录写入位置（commit/abort 时使用）。
  struct WriteLoc {
    std::string tab_name;
    Rid rid;
    PendingWrite::Op op;
  };
  void RegisterWrite(Transaction *txn, const std::string &tab_name, Rid rid,
                     PendingWrite::Op op);

  // 直接观察：用于 InsertExecutor 检查唯一索引上已有 rid 是否处于"已删除可接管"
  // 或"被另一活事务占着的写入"状态。
  struct RowProbe {
    bool exists{false};                 // 是否有 RowVersion 条目
    bool committed_visible{false};      // chain[0] 是否对 probe_start_ts 可见且未删除
    bool committed_deleted{false};      // chain[0] 是否标记为已删除
    timestamp_t committed_ts{0};
    txn_id_t pending_writer{INVALID_TXN_ID};
    PendingWrite::Op pending_op{PendingWrite::Op::NONE};
  };
  RowProbe ProbeRow(const std::string &tab_name, Rid rid,
                    timestamp_t observer_start_ts);
  // 同 ProbeRow 但不做惰性补种：SSI 读跟踪路径专用（见 cpp 注释）
  RowProbe ProbeRowNoSeed(const std::string &tab_name, Rid rid,
                          timestamp_t observer_start_ts);

  // 返回 tab_name 上当前被「其他」活跃事务标记为 pending DELETE 的所有 rid。
  // 供 InsertExecutor 做「DELETE+INSERT 同字节 ww」检测时只检查这些候选 rid，
  // 取代原来对整张表的全堆扫描（无并发删除时为空，开销可忽略）。
  std::vector<Rid> GetPendingDeletedRids(const std::string &tab_name,
                                         txn_id_t self);

  // ----------------------------- SSI -----------------------------------------
  struct SsiReadInfo {
    std::string tab_name;
    Rid rid;
  };
  struct SsiPredicate {
    std::string tab_name;
    std::vector<Condition> conds;
    std::vector<ColMeta> cols;
  };
  struct SsiTxnState {
    txn_id_t txn_id;
    timestamp_t start_ts;
    timestamp_t commit_ts = INVALID_TS;
    TransactionState state = TransactionState::DEFAULT;
    std::vector<SsiReadInfo> rid_reads;
    std::vector<SsiPredicate> predicates;
    // 写集合（表名, rid key）：读路径据此判断"该已提交事务是否真的写过我读
    // 的行"再建边。spec 规定只有"会改变本次查询结果"的不可见写入才构成 rw
    // 反依赖；无此过滤时任何并发已提交事务都会被建假边 → 误 abort。
    // 用 vector：写集通常很小（自动提交=1 条），读侧线性扫；比 map+set 每写
    // 省去额外节点分配，适合高频自动提交 INSERT 热路径。
    std::vector<std::pair<std::string, int64_t>> writes;
    std::unordered_set<txn_id_t> in_edges;
    std::unordered_set<txn_id_t> out_edges;
  };
  static int64_t ssi_rid_key(const Rid &rid) {
    return (static_cast<int64_t>(rid.page_no) << 20) | (rid.slot_no & 0xFFFFF);
  }

  void SsiRegisterBegin(Transaction *txn);
  void SsiMarkCommitted(Transaction *txn);
  void SsiMarkAborted(Transaction *txn);

  // SER：扫到一个 rid 且可见时调用，登记读集合并对"不可见写者"建立 W→me 反依赖。
  void SsiRecordRead(Transaction *txn, const std::string &tab_name, Rid rid);
  // SER：扫到一个 rid 但当前版本对本事务不可见时调用。仅当某个不可见版本的
  // 行内容满足本次扫描的谓词（即该写入会改变本次查询结果，spec 94/122 行的
  // 建边前提）才对其写者建立 W→me 反依赖；conds 为空（全表读）时任何不可见
  // 写入都计入。无谓词过滤会把无关不可见行（如别的范围里的并发插入）也建边
  // → 假危险结构 → 误 abort。
  void SsiCheckInvisibleWriters(Transaction *txn, const std::string &tab_name,
                                 Rid rid, const std::vector<Condition> &conds,
                                 const std::vector<ColMeta> &cols);
  // SER：登记本次扫描使用的谓词（包括空结果），供他人写入时做 phantom 比较。
  void SsiRecordPredicate(Transaction *txn, const std::string &tab_name,
                          const std::vector<Condition> &conds,
                          const std::vector<ColMeta> &cols);
  // SER：写入时扫描其他 SER 事务的读集合与谓词，命中即建立 me→reader 反依赖。
  // op_is_insert：true 表示 INSERT（只对谓词命中算冲突）；false 表示 UPDATE/DELETE
  //               （也对 rid_reads 命中算冲突）。
  void SsiRecordWrite(Transaction *txn, const std::string &tab_name, Rid rid,
                      const RmRecord *rec, bool op_is_insert);

  // SER：回收 commit_ts <= watermark 的已提交事务状态（其后不可能再参与冲突
  // 检测），并断开它们残留的 rw 反依赖边。避免 ssi_txns_ 无限增长导致每次
  // 读/写都要遍历全部历史事务（原 O(N^2)）。
  void SsiGarbageCollect(timestamp_t watermark);

  std::mutex ssi_latch_;
  std::unordered_map<txn_id_t, std::shared_ptr<SsiTxnState>> ssi_txns_;
  // commit_ts -> txn_id，按 commit_ts 升序，供 SsiGarbageCollect 从前往后回收
  std::map<timestamp_t, txn_id_t> ssi_committed_;

  // ----------------------------- 内部数据结构 --------------------------------
  struct PageRows {
    std::shared_mutex mutex_;
    std::unordered_map<int, RowVersion> rows_;  // key: slot_no
  };

  // 版本存储按 (tab_name, page_no) 分片。
  //
  // 原实现是一个全局 shared_mutex 保护一张大表：get_or_create_page_rows 每次写
  // 都要**独占**它，而它是所有 INSERT/UPDATE/DELETE 的必经路径，32 客户端下所有
  // 写在这里排成一队；get_page_rows_for_read 每扫一行取一次共享锁，同一条
  // cacheline 被所有核反复争用。分片后互不相干的 (表,页) 完全并行。
  //
  // 思路来源：PostgreSQL 8.2 把单一 lock hash table 拆成分区哈希表、每分区一把锁。
  // 分区数是权衡而非越大越好——分区越多，需要遍历全部分区的 GC 越贵——故取 64。
  static constexpr size_t VERSION_SHARD_NUM = 64;  // 2 的幂，取模用位与
  struct VersionShard {
    std::shared_mutex mutex_;
    // (tab_name, page_no) -> PageRows
    std::unordered_map<std::string,
                       std::unordered_map<page_id_t, std::shared_ptr<PageRows>>>
        pages_;
  };
  std::array<VersionShard, VERSION_SHARD_NUM> version_shards_;

  // 同一 (tab, page) 必须恒定落到同一分片。若两个线程把同一页算到不同分片，
  // 会各自建一份 PageRows，同一行的版本被劈成两半 → 可见性错乱。
  // 因此本函数只依赖 (tab_name, page_no)，无随机、无状态、无时间依赖。
  static size_t version_shard_index(const std::string &tab_name,
                                    page_id_t page_no) {
    size_t h = std::hash<std::string>{}(tab_name);
    h ^= static_cast<size_t>(page_no) * 0x9E3779B97F4A7C15ULL + 0x165667B1ULL +
         (h << 6) + (h >> 2);
    return h & (VERSION_SHARD_NUM - 1);
  }
  VersionShard &version_shard(const std::string &tab_name, page_id_t page_no) {
    return version_shards_[version_shard_index(tab_name, page_no)];
  }

  // Part-2：版本存储 GC 触发计数。每 GC_COMMIT_INTERVAL 次提交跑一次 sweep，
  // 把已 settled(commit_ts<watermark、无 pending、非墓碑)的 RowVersion 整条删除，
  // 使版本存储 O(活跃工作集) 而非 O(总行数)，根治海量灌库 normal 阶段 OOM。
  std::atomic<uint64_t> commits_since_gc_{0};
  static constexpr uint64_t GC_COMMIT_INTERVAL = 10000;  // 50k→10k:缩小版本队列跨度,降低长跑碎片残留

  std::shared_mutex write_loc_mutex_;
  std::unordered_map<txn_id_t, std::vector<WriteLoc>> txn_write_locs_;

 private:
  // 取或创建 (tab, page) 对应的 PageRows
  std::shared_ptr<PageRows> get_or_create_page_rows(const std::string &tab_name,
                                                    page_id_t page_no);
  std::shared_ptr<PageRows> get_page_rows_for_read(
      const std::string &tab_name, page_id_t page_no);

  ConcurrencyMode concurrency_mode_;
  std::atomic<txn_id_t> next_txn_id_{0};
  std::atomic<timestamp_t> next_timestamp_{0};
  std::mutex latch_;
  SmManager *sm_manager_;
  LockManager *lock_manager_;

  std::atomic<timestamp_t> last_commit_ts_{0};
  Watermark running_txns_{0};

  // 正在提交但版本尚未装入版本链的 commit_ts。
  //
  // commit_ts 在 commit() 开头就分配，而把 pending 提升为链顶（对其它事务可见）
  // 要到 FinalizeCommit 才发生。两者之间若有事务 begin()，它按
  // last_commit_ts_ 取到的 start_ts 会 **覆盖一个还没装链的提交**：同一个事务
  // 先读到旧值（版本还没进链），随后那个版本进链、commit_ts <= start_ts，再读
  // 就变成新值 —— 一个事务内两次读拿到不同快照。TPC-C Payment 的
  // `select w_ytd` 与 `update w_ytd = w_ytd + amount` 正好跨这个窗口，结果是
  // 更新落在【更新后的版本】上会破坏同一事务的快照一致性，并可能把并发事务的
  // 增量重复叠加到当前写入结果中。
  //
  // 因此 begin() 不能用 last_commit_ts_，只能用【已装链水位】：
  //   published = publishing_ 为空 ? last_commit_ts_ : (最小的 publishing_ - 1)
  // 该水位以下的提交全部装链完毕，快照因此是稳定的。
  //
  // 同一问题在 PostgreSQL 是 ProcArray 里"XID 摘除与可见性原子发生"，
  // 在 HyPer/Umbra 是"分配提交时间戳与安装版本处于同一临界区"。
  std::set<timestamp_t> publishing_;

  // 调用方必须已持有 latch_
  timestamp_t published_ts_locked() const {
    if (publishing_.empty()) return last_commit_ts_.load();
    return *publishing_.begin() - 1;
  }

  // 静态检查点静止化计数：begin 时 +1，commit/abort 完成时 -1。
  // checkpoint_in_progress_ 为 true 时阻塞新的 begin。
  std::mutex ckpt_quiesce_latch_;
  std::condition_variable ckpt_quiesce_cv_;
  bool checkpoint_in_progress_ = false;
  int active_txns_ = 0;
};
