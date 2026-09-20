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

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "log_manager.h"
#include "storage/disk_manager.h"
#include "system/sm_manager.h"
#include "transaction/transaction_manager.h"

// 故障恢复：基于 WAL、REDO/UNDO 与静态检查点。
//
// 设计要点：
//  - 两遍流式扫描日志（analyze=pass1, redo=pass2），滚动缓冲分块读文件，
//    驻留内存 ∝ 崩溃时在途事务数，与日志总量无关（巨量日志不 OOM）。
//  - MVCC 下 UPDATE 的堆写延后到 commit，但 **FinalizeCommit 写堆发生在 COMMIT
//    日志落盘之前**：这中间的脏页会被缓冲池正常淘汰路径写盘（update_page 只保证
//    已追加的日志先落盘，而该事务的 UPDATE 日志确实已追加）。因此"未提交事务
//    从未改过堆"并不成立——未提交的 UPDATE 值能进磁盘，崩溃后既不 redo（tid 在
//    未提交集里）也无人 undo，就会永久留在堆上，破坏崩溃后的数据一致性。
//    undo 因此必须按 ARIES 用日志里的 old_value_ 回滚未提交 UPDATE。
//  - 未提交 DELETE 仍无需 undo：MVCC DELETE 全程不动堆（墓碑只在版本链里），
//    堆上的物理删除只发生在 redo 重放已提交 DELETE 时。
//  - MVCC 版本不在恢复期播种：恢复后"内存无版本条目的堆记录 = 已提交数据"，
//    由运行期 VisibleRecord/ProbeRow 惰性补种（恢复时间与表大小解耦）。
//  - 无检查点：堆/索引磁盘状态不可信，重放全量日志 + 全表重建空闲链/索引。
//  - 有检查点（db.ckpt 偏移>0）：检查点已静止并把堆/索引/空闲链一致刷盘，
//    只重放检查点后增量、只重建被触及表的索引、不重算空闲链
//    → t2 ∝ 检查点后增量，远小于全量 t1（t2 < 0.7·t1 的来源）。
class RecoveryManager {
public:
    RecoveryManager(DiskManager* disk_manager, BufferPoolManager* buffer_pool_manager,
                    SmManager* sm_manager, TransactionManager* txn_manager = nullptr,
                    LogManager* log_manager = nullptr)
        : disk_manager_(disk_manager),
          buffer_pool_manager_(buffer_pool_manager),
          sm_manager_(sm_manager),
          txn_manager_(txn_manager),
          log_manager_(log_manager) {}

    void analyze();
    void redo();
    void undo();

    // 重启文件：记录最近一次静态检查点在日志文件中的字节偏移
    static const std::string RESTART_FILE_NAME;

private:
    void ensure_pages_on_disk();   // redo 前把各表文件物理扩展到 header.num_pages
    void rebuild_after_recovery(); // 恢复尾声：重建空闲链/索引 + 复位时间戳 + 落盘
    void reset_runtime_state();    // 复位内存时间戳 / 事务号 / 全局 lsn

    // 流式扫描日志：固定大小滚动缓冲分块读文件，对每条完整日志记录回调
    // fn(record_bytes, type, lsn, tid)；内存 O(缓冲)，与日志总量无关。
    template <typename Fn>
    void scan_log(int64_t start_offset, Fn&& fn);
    uint64_t processed_ = 0;  // scan_log 已处理记录数（周期性 malloc_trim 用）

    DiskManager* disk_manager_;
    BufferPoolManager* buffer_pool_manager_;
    SmManager* sm_manager_;
    TransactionManager* txn_manager_;   // 恢复后复位时间戳/事务号
    LogManager* log_manager_;           // 恢复后复位全局 lsn

    // ---- analyze(pass1) 产物 ----
    int64_t start_offset_ = 0;                      // 扫描起点（检查点偏移）
    bool from_checkpoint_ = false;                  // db.ckpt 存在且偏移>0
    std::unordered_set<txn_id_t> uncommitted_txns_; // 崩溃时未提交(含 abort)事务
    struct UncommittedInsert {
        std::string tab_name;
        Rid rid{-1, -1};
    };
    std::vector<UncommittedInsert> uncommitted_inserts_; // 其 INSERT 操作（undo 用）

    // 未提交 UPDATE 的前像，按 ARIES 逆序回滚。驻留内存 ∝ 崩溃时在途事务的
    // 写集，与日志总量无关：某事务的 COMMIT 记录一出现，它的条目立刻丢弃。
    struct UncommittedUpdate {
        lsn_t lsn = INVALID_LSN;
        std::string tab_name;
        Rid rid{-1, -1};
        std::vector<char> old_image;
    };
    std::vector<UncommittedUpdate> uncommitted_updates_;

    // undo 防误删：对每个"未提交 INSERT 的 rid"，在 redo 中跟踪它最终是否被
    // 某个已提交操作占据（true=被接管，undo 不得删除）。
    std::unordered_map<std::string, std::unordered_map<int64_t, bool>> watched_;

    // ---- redo(pass2) 产物 ----
    // 每个 (表, rid) 上**已提交**数据日志的最大 lsn。undo 逆序回滚未提交 UPDATE
    // 时，若该行后来被一条 lsn 更大的已提交记录写过，redo 已经放好了正确的值，
    // 此时再写回前像就会把已提交数据覆盖掉。只回滚 lsn 大于该水位的前像。
    std::unordered_map<std::string, std::unordered_map<int64_t, lsn_t>>
        last_committed_write_;
    // 检查点之后被数据日志触及的表名（其索引需从堆重建）。仅检查点路径填充。
    std::unordered_set<std::string> touched_tabs_;

    lsn_t max_lsn_ = INVALID_LSN;
    txn_id_t max_tid_ = INVALID_TXN_ID;

    static int64_t rid_key(const Rid& rid) {
        return (static_cast<int64_t>(rid.page_no) << 20) | (rid.slot_no & 0xFFFFF);
    }
};
