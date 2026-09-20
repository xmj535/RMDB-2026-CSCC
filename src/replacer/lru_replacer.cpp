/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL
v2. You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "lru_replacer.h"

LRUReplacer::LRUReplacer(size_t num_pages) : max_size_(num_pages) {}

LRUReplacer::~LRUReplacer() = default;

// 挑一个最久未用的 frame 淘汰，写回 *frame_id；无可淘汰则返回 false
bool LRUReplacer::victim(frame_id_t* frame_id) {
  std::scoped_lock lock{latch_};
  if (LRUlist_.empty()) {
    return false;
  }
  // 链表尾部是最久未使用的
  *frame_id = LRUlist_.back();
  LRUlist_.pop_back();
  LRUhash_.erase(*frame_id);
  return true;
}

// 固定 frame，使它不再参与淘汰（从 LRU 队列里摘掉）
void LRUReplacer::pin(frame_id_t frame_id) {
  std::scoped_lock lock{latch_};
  auto it = LRUhash_.find(frame_id);
  if (it == LRUhash_.end()) {
    return;
  }
  LRUlist_.erase(it->second);
  LRUhash_.erase(it);
}

// 取消固定，把 frame 加回到 LRU 队首（最新使用位置）
void LRUReplacer::unpin(frame_id_t frame_id) {
  std::scoped_lock lock{latch_};
  if (LRUlist_.size() >= max_size_) {
    return;
  }
  if (LRUhash_.count(frame_id)) {
    return;
  }
  LRUlist_.emplace_front(frame_id);
  LRUhash_[frame_id] = LRUlist_.begin();
}

// 当前可被淘汰的 frame 数量
size_t LRUReplacer::Size() { return LRUlist_.size(); }
