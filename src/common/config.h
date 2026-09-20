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

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

#define BUFFER_LENGTH 8192

/** Cycle detection is performed every CYCLE_DETECTION_INTERVAL milliseconds. */
extern std::chrono::milliseconds cycle_detection_interval;

/** True if logging should be enabled, false otherwise. */
extern std::atomic<bool> enable_logging;

/** 历史文本输出通道。Wire v3 不写 output.txt，也不实现 SET OUTPUT_FILE。 */
extern std::atomic<bool> g_output_enabled;

/** If ENABLE_LOGGING is true, the log should be flushed to disk every
 * LOG_TIMEOUT. */
extern std::chrono::duration<int64_t> log_timeout;

static constexpr int INVALID_FRAME_ID = -1;   // invalid frame id
static constexpr int INVALID_PAGE_ID = -1;    // invalid page id
static constexpr int INVALID_TXN_ID = -1;     // invalid transaction id
static constexpr int INVALID_TIMESTAMP = -1;  // invalid transaction timestamp
static constexpr int64_t INVALID_LSN = -1;    // invalid log sequence number
static constexpr int64_t TXN_START_ID = 1LL << 62;  // first txn id
static constexpr int64_t INVALID_TS = -1;  // invalid log sequence number
static constexpr int HEADER_PAGE_ID = 0;   // the header page id
static constexpr int PAGE_SIZE = 4096;     // size of a data page in byte  4KB
// 大规模数据集配较小 Buffer Pool 时命中率偏低，恢复后的整表聚合和分区扫描
// 容易受缺页影响。这里使用更大的 Buffer Pool，以空间换取稳定的页命中率。
// static constexpr int BUFFER_POOL_SIZE = 65536;   // 256MB
// static constexpr int BUFFER_POOL_SIZE = 131072;  // 512MB
static constexpr int BUFFER_POOL_SIZE = 262144;  // size of buffer pool 1GB
// 缓冲池分片数：把全局 latch + page_table + free_list + replacer 切成多片，
// 按 hash(page_id) 选片，降低 40/16 线程下全局 BPM latch 的串行化争用。
static constexpr int BUFFER_POOL_SHARD_NUM = 16;

// 曾经这里有 SNAPSHOT_WRITE_REBASE / SNAPSHOT_WRITE_MAX_RETRY：UPDATE 撞写写冲突时
// 读最新已提交值重算再写。它违反规范第三章第（三）节第 2 条（陈旧写必须 ABORT，
// 不得应用到更新后的版本），在高并发下造成 lost update。已整体删除，不保留开关——
// 留着开关等于留一个能把违规行为重新打开的旋钮。回归测试见
// tmp/record/rounds/R032_si_lost_update/artifacts/si_lost_update.py。
static constexpr int LOG_BUFFER_SIZE =
    (1024 * PAGE_SIZE);                 // size of a log buffer in byte
static constexpr int BUCKET_SIZE = 50;  // size of extendible hash bucket

using frame_id_t = int32_t;    // frame id type, 帧页ID,
                               // 页在BufferPool中的存储单元称为帧,一帧对应一页
using page_id_t = int32_t;     // page id type , 页ID
using txn_id_t = int64_t;      // transaction id type
using lsn_t = int32_t;         // log sequence number type
using slot_offset_t = size_t;  // slot offset type
using oid_t = uint16_t;
using timestamp_t =
    int64_t;  // timestamp type, used for transaction concurrency

// 故障恢复后给已提交数据播种 MVCC 版本时使用的基准 commit_ts。
// 取 1（>0）以便插入唯一性预检 (committed_ts > 0) 正常工作；恢复尾声把
// last_commit_ts_ 也置为它，保证后续事务 start_ts >= RECOVER_BASE_TS 能看见 seed 版本。
static constexpr timestamp_t RECOVER_BASE_TS = 1;

// log file
static const std::string LOG_FILE_NAME = "db.log";

// replacer
static const std::string REPLACER_TYPE = "LRU";

static const std::string DB_META_NAME = "db.meta";
