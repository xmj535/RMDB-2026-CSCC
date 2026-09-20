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
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cassert>
#include <functional>
#include <list>
#include <memory>
#include <unordered_map>
#include <vector>

#include "common/config.h"
#include "disk_manager.h"
#include "errors.h"
#include "page.h"
#include "replacer/lru_replacer.h"
#include "replacer/replacer.h"

class BufferPoolManager {
 private:
  // 一个分片：拥有独立的 latch / page_table / free_list / replacer。
  // 某个 frame 永远只属于一个分片（按 frame_id % num_shards_ 静态划分），
  // 且某个 page_id 永远映射到固定分片（按 hash(page_id) % num_shards_），
  // 因此装入/淘汰都在同一分片内完成，分片之间互不加锁、零跨片操作。
  struct Shard {
    std::mutex latch;
    std::unordered_map<PageId, frame_id_t, PageIdHash> page_table;
    std::list<frame_id_t> free_list;
    Replacer* replacer = nullptr;
    ~Shard() { delete replacer; }
  };

  size_t pool_size_;  // buffer_pool中可容纳页面的个数，即帧的个数
  size_t num_shards_;  // 分片数（= min(BUFFER_POOL_SHARD_NUM, pool_size_)）
  Page*
      pages_;  // buffer_pool中的Page对象数组，在构造空间中申请内存空间，在析构函数中释放，大小为BUFFER_POOL_SIZE
  std::vector<std::unique_ptr<Shard>> shards_;  // 各分片
  DiskManager* disk_manager_;
  std::mutex alloc_latch_;  // 仅保护 disk_manager_->allocate_page（非原子自增）
  // WAL：写脏数据页到磁盘前，先把日志刷盘。回调由 rmdb 启动时注入，
  // 用 std::function 解耦，避免 storage 依赖 recovery。
  std::function<void()> flush_log_cb_;

  // 选择某 page_id 归属的分片。低位足够分散：相邻 page_no 轮转落到不同分片，
  // 对顺序扫描天然均摊争用。
  Shard& shard_for(const PageId& page_id) {
    return *shards_[PageIdHash{}(page_id) % num_shards_];
  }

 public:
  BufferPoolManager(size_t pool_size, DiskManager* disk_manager)
      : pool_size_(pool_size), disk_manager_(disk_manager) {
    // 为buffer pool分配一块连续的内存空间
    pages_ = new Page[pool_size_];
    // 静态分片：每分片 capacity = pool_size_/num_shards_ 固定，故只在每分片仍能
    // 分到充足 frame（≥ kMinFramesPerShard）时才分片，否则退化为单池（与原实现
    // 逐字等价），避免小池下分片容量过薄触发早淘汰/取不到 frame。
    // 生产池 65536 → 16 片 × 4096；小池（如单测 10）→ 1 片 = 原行为。
    constexpr size_t kMinFramesPerShard = 1024;
    num_shards_ = std::max<size_t>(
        1, std::min<size_t>(BUFFER_POOL_SHARD_NUM,
                            pool_size_ / kMinFramesPerShard));
    shards_.reserve(num_shards_);
    for (size_t s = 0; s < num_shards_; ++s) {
      auto sh = std::make_unique<Shard>();
      // REPLACER_TYPE 当前仅 LRU；每分片一把独立 replacer（哈希表按 frame_id 索引，
      // 容忍全局稀疏 frame_id）。
      sh->replacer = new LRUReplacer(pool_size_);
      shards_.push_back(std::move(sh));
    }
    // 初始化时，所有 frame 都在各分片的 free_list 中（按 i % num_shards_ 划分）。
    for (size_t i = 0; i < pool_size_; ++i) {
      shards_[i % num_shards_]->free_list.emplace_back(
          static_cast<frame_id_t>(i));
    }
  }

  ~BufferPoolManager() {
    delete[] pages_;
    // shards_ 中的 unique_ptr 自动析构，Shard 析构里 delete replacer。
  }

  /**
   * @description: 将目标页面标记为脏页
   * @param {Page*} page 脏页
   */
  static void mark_dirty(Page* page) { page->is_dirty_ = true; }

  // 注入 WAL 日志刷盘回调（写任何脏数据页前调用，保证先写日志后写数据）
  void set_log_flush_callback(std::function<void()> cb) {
    flush_log_cb_ = std::move(cb);
  }

 public:
  Page* fetch_page(PageId page_id);

  bool unpin_page(PageId page_id, bool is_dirty);

  bool flush_page(PageId page_id);

  Page* new_page(PageId* page_id);

  bool delete_page(PageId page_id);

  void flush_all_pages(int fd);

  // 只写回指定文件当前驻留在 buffer pool 中的脏页，并返回写回页数。
  // 调用方必须保证这些页没有并发修改（恢复收尾和静态检查点均满足）。
  size_t flush_dirty_pages(int fd);

  void delete_all_pages(int fd);

 private:
  bool find_victim_page(Shard& shard, frame_id_t* frame_id);

  void update_page(Shard& shard, Page* page, PageId new_page_id,
                   frame_id_t new_frame_id);
};
