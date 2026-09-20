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

#include "transaction/transaction.h"
#include "transaction/concurrency/lock_manager.h"
#include "transaction/txn_defs.h"
#include "recovery/log_manager.h"

class TransactionManager;
namespace wire {
class ResultWriter;
}

// class TransactionManager;

// used for data_send
static int const_offset = -1;

class Context {
public:
    Context (LockManager *lock_mgr, LogManager *log_mgr, 
            Transaction *txn, char *data_send = nullptr, int *offset = &const_offset)
        : lock_mgr_(lock_mgr), log_mgr_(log_mgr), txn_(txn),
          data_send_(data_send), offset_(offset) {
            ellipsis_ = false;
          }

    LockManager *lock_mgr_;
    LogManager *log_mgr_;
    Transaction *txn_;
    char *data_send_;
    int *offset_;
    bool ellipsis_;
    // 会话级隔离级别指针，由 client_handler 维护；新事务用此值初始化
    IsolationLevel *session_iso_ = nullptr;
    // MVCC：executor 通过 txn_mgr_ 访问版本链/元组元数据
    TransactionManager *txn_mgr_ = nullptr;
    // Wire v3 EXEC_STREAM 的类型化结果出口；非网络测试保持 nullptr。
    wire::ResultWriter *wire_writer_ = nullptr;
};
