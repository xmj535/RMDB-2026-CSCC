/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL
v2. You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */
#include "buffer_pool_manager.h"

#include "common/config.h"

// 全局配置项的定义（在 common/config.h 中声明为 extern）。放在 storage 库里，
// 保证 rmdb 与 unit_test 都能链接到。
std::atomic<bool> enable_logging{false};
std::chrono::milliseconds cycle_detection_interval = std::chrono::milliseconds(50);
std::chrono::duration<int64_t> log_timeout = std::chrono::seconds(1);

// 找一个可用的 frame：优先该分片的 free_list，没有就让该分片的 replacer 淘汰
bool BufferPoolManager::find_victim_page(Shard& shard, frame_id_t* frame_id) {
  if (!shard.free_list.empty()) {
    *frame_id = shard.free_list.front();
    shard.free_list.pop_front();
    return true;
  }
  return shard.replacer->victim(frame_id);
}

// 把 frame 上原来的页（脏则写回）替换成 new_page_id，更新该分片的 page_table。
// 被淘汰的旧页与新页同属一个分片（frame 静态归属 + page_id 静态归属），故零跨片。
void BufferPoolManager::update_page(Shard& shard, Page* page, PageId new_page_id,
                                    frame_id_t new_frame_id) {
  if (page->is_dirty_) {
    // WAL：把脏页写回磁盘前，先确保相关日志已落盘
    if (flush_log_cb_) flush_log_cb_();
    const PageId& old = page->get_page_id();
    disk_manager_->write_page(old.fd, old.page_no, page->get_data(), PAGE_SIZE);
    page->is_dirty_ = false;
  }
  shard.page_table.erase(page->get_page_id());
  shard.page_table[new_page_id] = new_frame_id;
  page->id_ = new_page_id;
  page->pin_count_ = 0;
}

// 从 buffer pool 拿到指定页：命中则
// pin++，不命中则从磁盘读入；池满且无可淘汰则返回 nullptr
Page* BufferPoolManager::fetch_page(PageId page_id) {
  Shard& shard = shard_for(page_id);
  std::scoped_lock lock{shard.latch};

  auto it = shard.page_table.find(page_id);
  if (it != shard.page_table.end()) {
    Page* page = &pages_[it->second];
    if (page->pin_count_++ == 0) {
      shard.replacer->pin(it->second);
    }
    return page;
  }

  frame_id_t frame_id = INVALID_FRAME_ID;
  if (!find_victim_page(shard, &frame_id)) {
    return nullptr;
  }

  Page* page = &pages_[frame_id];
  update_page(shard, page, page_id, frame_id);
  disk_manager_->read_page(page_id.fd, page_id.page_no, page->get_data(),
                           PAGE_SIZE);
  shard.replacer->pin(frame_id);
  page->pin_count_ = 1;
  return page;
}

// 释放一次引用：pin_count-1；归零后交给 replacer 让它可被淘汰，并按需标脏
bool BufferPoolManager::unpin_page(PageId page_id, bool is_dirty) {
  Shard& shard = shard_for(page_id);
  std::scoped_lock lock{shard.latch};

  auto it = shard.page_table.find(page_id);
  if (it == shard.page_table.end()) {
    return true;
  }
  Page* page = &pages_[it->second];

  if (page->pin_count_ <= 0) {
    return false;
  }

  if (--page->pin_count_ == 0) {
    shard.replacer->unpin(it->second);
  }
  page->is_dirty_ = page->is_dirty_ || is_dirty;
  return true;
}

// 将指定页写回磁盘（不管脏不脏），并清掉脏标志；页不在池中返回 false
bool BufferPoolManager::flush_page(PageId page_id) {
  Shard& shard = shard_for(page_id);
  std::scoped_lock lock{shard.latch};
  auto it = shard.page_table.find(page_id);
  if (it == shard.page_table.end()) {
    return false;
  }
  Page* page = &pages_[it->second];
  if (flush_log_cb_) flush_log_cb_();
  disk_manager_->write_page(page_id.fd, page_id.page_no, page->get_data(),
                            PAGE_SIZE);
  page->is_dirty_ = false;
  return true;
}

// 在 page_id->fd 文件里分配一个新页号，并占用一个 frame 返回（pin_count=1）
Page* BufferPoolManager::new_page(PageId* page_id) {
  // 页号由 allocate_page 决定，而归属分片依赖页号，故先分配页号再选分片。
  // allocate_page 是非原子自增，用专用 alloc_latch_ 串行化（频率远低于 fetch）。
  {
    std::scoped_lock alloc_lock{alloc_latch_};
    page_id->page_no = disk_manager_->allocate_page(page_id->fd);
  }
  Shard& shard = shard_for(*page_id);
  std::scoped_lock lock{shard.latch};

  frame_id_t frame_id = INVALID_FRAME_ID;
  if (!find_victim_page(shard, &frame_id)) {
    // 池满且无可淘汰（实际近乎不可能，等同致命）。此时已分配的 page_no 会被泄漏，
    // 与原实现一样不回收 page_no；保持行为不引入回滚复杂度。
    return nullptr;
  }

  Page* page = &pages_[frame_id];
  update_page(shard, page, *page_id, frame_id);
  page->reset_memory();
  shard.replacer->pin(frame_id);
  page->pin_count_ = 1;
  return page;
}

// 把页从 buffer pool 移除并归还 frame；若仍被 pin 住则失败
bool BufferPoolManager::delete_page(PageId page_id) {
  Shard& shard = shard_for(page_id);
  std::scoped_lock lock{shard.latch};
  auto it = shard.page_table.find(page_id);
  if (it == shard.page_table.end()) {
    return true;
  }
  Page* page = &pages_[it->second];
  if (page->pin_count_ != 0) {
    return false;
  }
  shard.free_list.push_back(it->second);
  shard.page_table.erase(it);
  page->reset_memory();
  return true;
}

// 把某文件 (fd) 在池中的所有有效页都写回磁盘，常用于 close_file 前。
// 逐分片加锁遍历各自 frame：frame 静态归属分片，持本分片锁即可安全访问其 frame。
void BufferPoolManager::flush_all_pages(int fd) {
  if (flush_log_cb_) flush_log_cb_();
  for (size_t s = 0; s < num_shards_; ++s) {
    Shard& shard = *shards_[s];
    std::scoped_lock lock{shard.latch};
    for (size_t i = s; i < pool_size_; i += num_shards_) {
      Page* page = &pages_[i];
      const PageId& pid = page->get_page_id();
      if (pid.fd == fd && pid.page_no != INVALID_PAGE_ID) {
        disk_manager_->write_page(pid.fd, pid.page_no, page->get_data(),
                                  PAGE_SIZE);
        page->is_dirty_ = false;
      }
    }
  }
}

// 只写回某文件在池中的脏页。与 flush_all_pages 不同，干净页不会产生重复 I/O；
// 恢复重建索引后可用它在开放服务前清掉残留脏页，避免后续查询淘汰这些页时承担写回。
size_t BufferPoolManager::flush_dirty_pages(int fd) {
  if (flush_log_cb_) flush_log_cb_();
  size_t flushed = 0;
  for (size_t s = 0; s < num_shards_; ++s) {
    Shard& shard = *shards_[s];
    std::scoped_lock lock{shard.latch};
    for (size_t i = s; i < pool_size_; i += num_shards_) {
      Page* page = &pages_[i];
      const PageId& pid = page->get_page_id();
      if (pid.fd == fd && pid.page_no != INVALID_PAGE_ID &&
          page->is_dirty_) {
        disk_manager_->write_page(pid.fd, pid.page_no, page->get_data(),
                                  PAGE_SIZE);
        page->is_dirty_ = false;
        ++flushed;
      }
    }
  }
  return flushed;
}

// 用于 drop/close 文件：把该 fd 在池中、且无人 pin 的页驱逐，避免 fd
// 复用后读到旧数据 pin_count>0 的页跳过，不影响仍在使用它的调用方
void BufferPoolManager::delete_all_pages(int fd) {
  if (flush_log_cb_) flush_log_cb_();
  for (size_t s = 0; s < num_shards_; ++s) {
    Shard& shard = *shards_[s];
    std::scoped_lock lock{shard.latch};
    for (size_t i = s; i < pool_size_; i += num_shards_) {
      Page* page = &pages_[i];
      const PageId& pid = page->get_page_id();
      if (pid.fd != fd || pid.page_no == INVALID_PAGE_ID) {
        continue;
      }
      if (page->pin_count_ != 0) {
        continue;
      }
      if (page->is_dirty_) {
        disk_manager_->write_page(pid.fd, pid.page_no, page->get_data(),
                                  PAGE_SIZE);
        page->is_dirty_ = false;
      }
      shard.page_table.erase(pid);
      shard.replacer->pin(static_cast<frame_id_t>(i));
      page->reset_memory();
      page->id_ = PageId{-1, INVALID_PAGE_ID};
      shard.free_list.push_back(static_cast<frame_id_t>(i));
    }
  }
}
