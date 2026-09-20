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

#include <memory>
#include <vector>

#include "common/common.h"
#include "common/context.h"
#include "record/rm_file_handle.h"
#include "transaction/transaction.h"
#include "transaction/transaction_manager.h"

// 在 (tab, rid) 上按 MVCC 语义取本事务可见的记录；不可见 / 已删返回 nullptr。
// 非 MVCC 隔离级别直接走堆。
inline std::unique_ptr<RmRecord> mvcc_visible_record(Context* context,
                                                     const std::string& tab_name,
                                                     RmFileHandle* fh, Rid rid) {
  Transaction* txn = context ? context->txn_ : nullptr;
  TransactionManager* tm = context ? context->txn_mgr_ : nullptr;
  if (txn == nullptr || tm == nullptr ||
      !TransactionManager::IsMvccIso(txn->get_isolation_level())) {
    return fh->get_record(rid, context);
  }
  // 只读扫描路径：用不补种、不插版本表的可见性，避免全表扫把整张表灌进版本存储
  // (内存复涨、扫描超线性变慢)。写执行器(insert/update/delete)仍各自走 VisibleRecord。
  // VisibleRecordForScan 返回 unique_ptr，直接返回，不再拷贝记录。
  return tm->VisibleRecordForScan(tab_name, rid, txn);
}
