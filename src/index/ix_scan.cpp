/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL
v2. You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "ix_scan.h"

// 取叶子结点：命中缓存(同页且已 pin)直接复用；否则先换出旧页，再取新页并保持 pin。
// 返回的指针归本对象所有，调用方不得 unpin/delete。
IxNodeHandle* IxScan::node_for(int page_no) const {
  if (cur_node_ != nullptr && cur_node_page_ == page_no) {
    return cur_node_;
  }
  release_node();
  IxNodeHandle* node = ih_->fetch_node(page_no);  // pin +1
  if (node == nullptr) return nullptr;
  cur_node_ = node;
  cur_node_page_ = page_no;
  return cur_node_;
}

// 释放缓存的叶子(幂等)。换页与析构都必须经此，保证 pin 严格守恒。
void IxScan::release_node() const {
  if (cur_node_ != nullptr) {
    bpm_->unpin_page(cur_node_->get_page_id(), false);
    delete cur_node_;
    cur_node_ = nullptr;
    cur_node_page_ = -1;
  }
}

// 走到当前叶子的下一项；到末尾且不是最后一片叶子，则跳到下一片叶子开头。
//
// 注意：本框架以 -O0 -g 构建（无 -DNDEBUG），assert 全部生效。若并发事务正在对
// 同一索引做 split/merge，扫描位置可能瞬时越界（slot_no >= size）或落到非叶子页上。
// 原先这里用 assert 断言这些条件，一旦被并发触发会直接 abort 整个 server 进程。
// 这里改为防御式处理：遇到越界或异常位置时推进到下一叶子或判定扫描结束，
// 避免单次索引并发异常导致整个服务退出。
// 跨叶。哨兵页 IX_LEAF_HEADER_PAGE 是环状叶子链的接头：最后一片叶子的 next_leaf
// 指向它，而它的 next_leaf 又指回 first_leaf_。走进哨兵就等于绕回索引开头，所以
// 把"下一片是哨兵"直接判定为扫描结束——这是唯一能在不改索引结构、不加锁的前提下
// 切断环的位置。IX_NO_PAGE 同理。
void IxScan::step_to_leaf(page_id_t next_leaf) {
  if (next_leaf == IX_LEAF_HEADER_PAGE || next_leaf == IX_NO_PAGE) {
    ended_ = true;
    return;
  }
  // 兜底闸门：换叶次数超过索引总页数说明链上有环（或 last_leaf_ 不可达）。
  if (++leaf_steps_ > ih_->file_hdr_->num_pages_) {
    ended_ = true;
    return;
  }
  iid_.slot_no = 0;
  iid_.page_no = next_leaf;
}

void IxScan::next() {
  if (is_end()) return;
  // 游标已经落在哨兵上（例如构造时给的 lower 就指向它）：直接结束，绝不前进。
  if (iid_.page_no == IX_LEAF_HEADER_PAGE || iid_.page_no == IX_NO_PAGE) {
    ended_ = true;
    return;
  }
  IxNodeHandle* node = node_for(iid_.page_no);
  if (node == nullptr) {  // 页已不可用（并发回收），保守置为末尾
    ended_ = true;
    return;
  }
  if (!node->is_leaf_page()) {
    release_node();
    ended_ = true;
    return;
  }
  int size = node->get_size();
  page_id_t next_leaf = node->get_next_leaf();
  bool is_last_leaf = (iid_.page_no == ih_->file_hdr_->last_leaf_);

  // 当前 slot 越界（并发删除/合并使该叶子变小）：直接跳到下一片叶子开头；
  // 已是最后一片则到末尾。
  if (iid_.slot_no >= size) {
    if (is_last_leaf) {
      ended_ = true;
    } else {
      step_to_leaf(next_leaf);
    }
    return;
  }

  iid_.slot_no++;
  if (!is_last_leaf && iid_.slot_no >= size) {
    step_to_leaf(next_leaf);
  }
}

// 与 IxIndexHandle::get_rid 同语义（越界抛 IndexEntryNotFoundError），
// 但复用缓存的叶子，省掉逐行的 fetch_node/new/delete/unpin。
Rid IxScan::rid() const {
  IxNodeHandle* node = node_for(iid_.page_no);
  if (node == nullptr || iid_.slot_no >= node->get_size()) {
    throw IndexEntryNotFoundError();
  }
  return *node->get_rid(iid_.slot_no);
}

// 越界保护。原先这里既不判 node 为空、也不判 slot 越界：
//  * node == nullptr（缓冲池该分片取不到帧）时直接 return，把 out **原样留着**，
//    而调用方 past_stop_key() 复用同一个成员缓冲，于是拿上一行的键去比 stop_key，
//    极可能判成"还没越界"而继续扫——点查/范围查退化成走完整个索引；
//  * slot_no 越界时会读到活键之外的槽位，得到的是垃圾键，同样会骗过 stop_key。
// 两者都不会自己死循环，但都会让 stop_key 提前退出失效，是把有界查询变成
// "永不返回"的最短路径。这里一律判定为扫描结束（ended_ 不可逆）。
void IxScan::copy_current_key(char* out) const {
  IxNodeHandle* node = node_for(iid_.page_no);
  if (node == nullptr || iid_.slot_no < 0 || iid_.slot_no >= node->get_size()) {
    ended_ = true;
    return;
  }
  memcpy(out, node->get_key(iid_.slot_no), ih_->file_hdr_->col_tot_len_);
}
