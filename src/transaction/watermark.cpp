/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "transaction/watermark.h"

#include <stdexcept>

// 追加一个读时间戳。watermark_ 始终维持为 = 最小活跃读时间戳（current_reads_
// 是有序 map，begin() 即最小），与 GetWatermark() 的口径一致。
void Watermark::AddTxn(timestamp_t read_ts) {
  if (read_ts < commit_ts_) {
    // 不应该出现：读时间戳必须 >= 当前已知的最后提交时间戳
    throw std::runtime_error("read_ts < commit_ts_ in Watermark::AddTxn");
  }
  current_reads_[read_ts] += 1;
  watermark_ = current_reads_.begin()->first;
}

// 移除一个读时间戳；如果该 ts 的计数清零，则推进 watermark
void Watermark::RemoveTxn(timestamp_t read_ts) {
  auto it = current_reads_.find(read_ts);
  if (it == current_reads_.end()) return;
  it->second -= 1;
  if (it->second <= 0) {
    current_reads_.erase(it);
  }
  if (current_reads_.empty()) {
    watermark_ = commit_ts_;
  } else {
    watermark_ = current_reads_.begin()->first;
  }
}

void Watermark::UpdateCommitTs(timestamp_t commit_ts) {
  if (commit_ts > commit_ts_) commit_ts_ = commit_ts;
  if (current_reads_.empty()) watermark_ = commit_ts_;
}

timestamp_t Watermark::GetWatermark() {
  if (current_reads_.empty()) return commit_ts_;
  return current_reads_.begin()->first;
}