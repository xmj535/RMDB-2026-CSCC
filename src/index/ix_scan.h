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

#include "ix_defs.h"
#include "ix_index_handle.h"

// class IxIndexHandle;

// 用于遍历叶子结点
// 用于直接遍历叶子结点，而不用findleafpage来得到叶子结点
// TODO：对page遍历时，要加上读锁
class IxScan : public RecScan {
  const IxIndexHandle* ih_;
  Iid iid_;  // 初始为lower（用于遍历的指针）
  Iid end_;  // 初始为upper
  BufferPoolManager* bpm_;

  // 缓存当前已 pin 的叶子结点。原先 next()/rid()/copy_current_key() 各自
  // fetch_node 一次（= fetch_page + new IxNodeHandle），逐行三次取页三次
  // new/delete；一片叶子容纳数百个键，同页复用后降为每片叶子三次。
  // 与 RmScan::fetch_page_pinned 同一手法，pin 严格守恒：缓存期间恰好持有
  // 一个 pin，换页与析构时释放。
  mutable IxNodeHandle* cur_node_ = nullptr;
  mutable int cur_node_page_ = -1;

  // 终止标志。原先"到末尾"靠把 iid_ 赋成 end_ 再用 iid_ == end_ 判定，这是唯一
  // 的终止条件，且是**精确相等**——一旦游标越过 end_ 就再也回不到相等，扫描会
  // 继续往后跑。叶子链又是**环状**的（ix_manager 建库时 first_leaf 与哨兵页
  // IX_LEAF_HEADER_PAGE 互指，且哨兵 is_leaf=true，绕不开 is_leaf_page() 那道
  // 防御），于是越界的游标会经哨兵绕回 first_leaf_ 无限打转：CPU 打满、进程存活、
  // 永不返回结果帧。改为显式终止标志：任何"已越过末尾"的判定都置位且不可逆。
  mutable bool ended_ = false;

  // 换叶计数。正向扫描每片叶子最多访问一次，故换叶次数不可能超过索引总页数；
  // 超了必定是链上出现了环。作为兜底闸门，不依赖对成因的正确判断。
  mutable int leaf_steps_ = 0;

  // 取 page_no 对应的叶子；命中缓存直接复用，否则换出旧页再取新页并保持 pin。
  // 返回的指针由本对象持有，调用方不得 unpin/delete。
  IxNodeHandle* node_for(int page_no) const;
  void release_node() const;

  // 跨到下一片叶子；遇到哨兵/无效页/超出换叶上限一律终止。
  void step_to_leaf(page_id_t next_leaf);

 public:
  IxScan(const IxIndexHandle* ih, const Iid& lower, const Iid& upper,
         BufferPoolManager* bpm)
      : ih_(ih), iid_(lower), end_(upper), bpm_(bpm) {}

  ~IxScan() override { release_node(); }

  IxScan(const IxScan&) = delete;
  IxScan& operator=(const IxScan&) = delete;

  void next() override;

  // 单调终止判定。注意**不能**用 page_no > end_.page_no 做跨页比较：叶子页号是
  // 按分裂时的分配顺序产生的，与叶子在链上的先后无关，后面的叶子页号完全可能更
  // 小。跨页终止一律由 ended_ 负责；页内则用 slot_no >= end_.slot_no，slot_no 在
  // 页内只增不减，是真正单调的，可挡住"并发删除使 end_ 那一格被跳过"的越界。
  bool is_end() const override {
    return ended_ ||
           (iid_.page_no == end_.page_no && iid_.slot_no >= end_.slot_no);
  }

  Rid rid() const override;

  const Iid& iid() const { return iid_; }

  // 把当前 iid 指向的键拷贝到 out（缓冲区大小由调用方保证 = col_tot_len）
  void copy_current_key(char* out) const;
};