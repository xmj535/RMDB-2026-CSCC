/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL
v2. You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "ix_index_handle.h"

#include "ix_scan.h"

// 当前节点中第一个 >= target 的 key 位置
int IxNodeHandle::lower_bound(const char* target) const {
  // 在 [0, num_key) 上做闭开区间二分；ix_compare
  // 用列类型/长度按字典序比较多列拼接的 key
  int lo = 0, hi = page_hdr->num_key;
  while (lo < hi) {
    int mid = (lo + hi) / 2;
    // 中点 key 严格小于 target：丢弃左半（含 mid），向右收缩
    if (ix_compare(get_key(mid), target, file_hdr->col_types_,
                   file_hdr->col_lens_) < 0) {
      lo = mid + 1;
    }
    // 中点 key >= target：mid 可能就是答案，保留，向左收缩
    else {
      hi = mid;
    }
  }
  return lo;
}

// 当前节点中第一个 > target 的 key 位置
int IxNodeHandle::upper_bound(const char* target) const {
  // 和 lower_bound 同套二分模板，只把比较从 < 改成 <=，从而跳过所有等于 target
  // 的位置
  int lo = 0, hi = page_hdr->num_key;
  while (lo < hi) {
    int mid = (lo + hi) / 2;
    // mid <= target：mid 不满足"严格大于"，丢弃
    if (ix_compare(get_key(mid), target, file_hdr->col_types_,
                   file_hdr->col_lens_) <= 0) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo;
}

// 叶子节点：按 key 取 rid；命中写出 *value
bool IxNodeHandle::leaf_lookup(const char* key, Rid** value) {
  // 先用 lower_bound 定位"理论上 key 应该在的槽位"
  int idx = lower_bound(key);
  // 落到尾部 = 节点里没有 >= key 的项，肯定不存在
  if (idx == page_hdr->num_key) {
    return false;
  }
  // 这一槽不严格等于 key（只是 >= key 的最小者），同样视为不存在
  if (ix_compare(get_key(idx), key, file_hdr->col_types_,
                 file_hdr->col_lens_) != 0) {
    return false;
  }
  // 命中：把对应 rid 的地址回写出去（注意是指针的指针，调用方拿到 Rid*
  // 后可直接读）
  *value = get_rid(idx);
  return true;
}

// 内部节点：返回 key 所属的孩子页号；keys[i] 视作 children[i] 的最小键
page_id_t IxNodeHandle::internal_lookup(const char* key) {
  // upper_bound 给出第一个 > key 的位置；它的左邻槽就是 key 落入的子树
  int idx = upper_bound(key);
  // 比所有 key 都小：去最左孩子
  if (idx == 0) {
    return value_at(0);
  }
  // 否则 children[idx-1] 是覆盖 key 的子树
  return value_at(idx - 1);
}

// 把 n 个连续键值对插入到 pos 位置
void IxNodeHandle::insert_pairs(int pos, const char* key, const Rid* rid,
                                int n) {
  // 越界保护：pos 允许等于 num_key（尾插），但不能更大
  if (pos < 0 || pos > page_hdr->num_key) {
    return;
  }
  int n_key = page_hdr->num_key;
  int key_len = file_hdr->col_tot_len_;
  int tail = n_key - pos;
  // 把 [pos, num_key) 的 key/rid 整体右移 n 个槽位，腾出连续空位
  // memmove 处理 src/dst 区间重叠的情形（memcpy 在重叠时是未定义行为）
  if (tail > 0) {
    memmove(keys + (pos + n) * key_len, keys + pos * key_len, tail * key_len);
    memmove(rids + pos + n, rids + pos, tail * sizeof(Rid));
  }
  // 把外部传入的 n 组 key/rid 拷贝到刚腾出的空位
  memcpy(keys + pos * key_len, key, n * key_len);
  memcpy(rids + pos, rid, n * sizeof(Rid));
  // 维护 num_key 计数
  page_hdr->num_key = n_key + n;
}

// 单个 key 插入：唯一索引，重复返回原 size
int IxNodeHandle::insert(const char* key, const Rid& value) {
  // 二分定位预期插入槽
  int idx = lower_bound(key);
  // 唯一索引语义：若该位置 key
  // 完全相等，直接放弃插入（不报错，由调用方根据返回值判断）
  if (idx < page_hdr->num_key &&
      ix_compare(get_key(idx), key, file_hdr->col_types_,
                 file_hdr->col_lens_) == 0) {
    return page_hdr->num_key;
  }
  // 不冲突：调单条版本插入并维护 num_key
  insert_pair(idx, key, value);
  return page_hdr->num_key;
}

void IxNodeHandle::erase_pair(int pos) {
  // 越界保护：pos 必须落在 [0, num_key)
  if (pos < 0 || pos >= page_hdr->num_key) {
    return;
  }
  int n_key = page_hdr->num_key;
  int key_len = file_hdr->col_tot_len_;
  int tail = n_key - pos - 1;
  // 把 (pos, num_key) 的 key/rid 整体左移一格，覆盖掉 pos 槽
  if (tail > 0) {
    memmove(keys + pos * key_len, keys + (pos + 1) * key_len, tail * key_len);
    memmove(rids + pos, rids + pos + 1, tail * sizeof(Rid));
  }
  // num_key 减一
  page_hdr->num_key = n_key - 1;
}

int IxNodeHandle::remove(const char* key) {
  // 二分定位
  int idx = lower_bound(key);
  // 严格相等才删；只是 ">=" 不算命中
  if (idx < page_hdr->num_key &&
      ix_compare(get_key(idx), key, file_hdr->col_types_,
                 file_hdr->col_lens_) == 0) {
    erase_pair(idx);
  }
  // 返回操作后的剩余键数，便于上层判断是否需要 merge/redistribute
  return page_hdr->num_key;
}

IxIndexHandle::IxIndexHandle(DiskManager* disk_manager,
                             BufferPoolManager* buffer_pool_manager, int fd)
    : disk_manager_(disk_manager),
      buffer_pool_manager_(buffer_pool_manager),
      fd_(fd) {
  char* buf = new char[PAGE_SIZE];
  memset(buf, 0, PAGE_SIZE);
  disk_manager_->read_page(fd, IX_FILE_HDR_PAGE, buf, PAGE_SIZE);
  file_hdr_ = new IxFileHdr();
  file_hdr_->deserialize(buf);
  delete[] buf;

  // 从落盘的 file_hdr 里恢复"下一个可分配页号"，避免重启 / fd 复用后撞到已用页
  disk_manager_->set_fd2pageno(fd, file_hdr_->num_pages_);
}

// 从根节点下行到叶子节点；调用方负责对返回 node 调用 unpin + delete
std::pair<IxNodeHandle*, bool> IxIndexHandle::find_leaf_page(
    const char* key, Operation operation, Transaction* transaction,
    bool find_first) {
  // 从根开始，fetch_node 内部会 buffer_pool->fetch_page 并 new IxNodeHandle
  // 包一层
  IxNodeHandle* node = fetch_node(file_hdr_->root_page_);
  // 沿内部节点一路下行：internal_lookup 给出 key 所属的子树页号，逐层替换 node
  while (!node->is_leaf_page()) {
    page_id_t child_page = node->internal_lookup(key);
    // 当前层的页在 buffer pool 里 pin 了一次，下行前必须 unpin + delete
    // handle，避免泄漏
    buffer_pool_manager_->unpin_page(node->get_page_id(), false);
    delete node;
    node = fetch_node(child_page);
  }
  // 第二个 bool 是占位：原本用来告诉调用方根锁是否仍持有，本实现简单不需要
  return std::make_pair(node, false);
}

bool IxIndexHandle::get_value(const char* key, std::vector<Rid>* result,
                              Transaction* transaction) {
  // root_latch_ 用
  // scoped_lock：构造时上锁，函数返回时自动释放，保证整次查询期间索引结构稳定
  std::scoped_lock lock{root_latch_};
  // 下行到 key 所在的叶子节点
  auto pair = find_leaf_page(key, Operation::FIND, transaction);
  IxNodeHandle* leaf = pair.first;
  // 在叶子里做相等查找；命中时 rid 指向叶子页内部的 Rid 槽
  Rid* rid = nullptr;
  bool found = leaf->leaf_lookup(key, &rid);
  if (found) {
    // 拷贝一份再 push（紧接的 unpin 之后 rid 指向的内存可能被淘汰）
    result->push_back(*rid);
  }
  // 释放叶子页 pin 并 delete handle
  buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
  delete leaf;
  return found;
}

// 把 node 的右半部分迁到一个新建的右兄弟节点上；调用方需 unpin + delete 返回值
IxNodeHandle* IxIndexHandle::split(IxNodeHandle* node) {
  IxNodeHandle* new_node = create_node();
  new_node->page_hdr->is_leaf = node->page_hdr->is_leaf;
  new_node->page_hdr->parent = node->page_hdr->parent;
  new_node->page_hdr->num_key = 0;
  new_node->page_hdr->next_free_page_no = IX_NO_PAGE;
  new_node->page_hdr->prev_leaf = IX_NO_PAGE;
  new_node->page_hdr->next_leaf = IX_NO_PAGE;

  int total = node->get_size();
  int split_pos = total / 2;
  int move_n = total - split_pos;

  new_node->insert_pairs(0, node->get_key(split_pos), node->get_rid(split_pos),
                         move_n);
  node->set_size(split_pos);

  if (node->is_leaf_page()) {
    new_node->set_next_leaf(node->get_next_leaf());
    new_node->set_prev_leaf(node->get_page_no());

    IxNodeHandle* next = fetch_node(node->get_next_leaf());
    next->set_prev_leaf(new_node->get_page_no());
    buffer_pool_manager_->unpin_page(next->get_page_id(), true);
    delete next;

    node->set_next_leaf(new_node->get_page_no());

    if (node->get_page_no() == file_hdr_->last_leaf_) {
      file_hdr_->last_leaf_ = new_node->get_page_no();
    }
  } else {
    for (int i = 0; i < new_node->get_size(); i++) {
      maintain_child(new_node, i);
    }
  }

  return new_node;
}

void IxIndexHandle::insert_into_parent(IxNodeHandle* old_node, const char* key,
                                       IxNodeHandle* new_node,
                                       Transaction* transaction) {
  if (old_node->is_root_page()) {
    IxNodeHandle* new_root = create_node();
    new_root->page_hdr->is_leaf = false;
    new_root->page_hdr->parent = IX_NO_PAGE;
    new_root->page_hdr->num_key = 0;
    new_root->page_hdr->next_free_page_no = IX_NO_PAGE;
    new_root->page_hdr->prev_leaf = IX_NO_PAGE;
    new_root->page_hdr->next_leaf = IX_NO_PAGE;

    Rid r1 = {.page_no = old_node->get_page_no(), .slot_no = -1};
    Rid r2 = {.page_no = new_node->get_page_no(), .slot_no = -1};
    new_root->insert_pair(0, old_node->get_key(0), r1);
    new_root->insert_pair(1, key, r2);

    old_node->set_parent_page_no(new_root->get_page_no());
    new_node->set_parent_page_no(new_root->get_page_no());
    file_hdr_->root_page_ = new_root->get_page_no();

    buffer_pool_manager_->unpin_page(new_root->get_page_id(), true);
    delete new_root;
    return;
  }

  IxNodeHandle* parent = fetch_node(old_node->get_parent_page_no());
  int idx = parent->find_child(old_node);
  Rid r = {.page_no = new_node->get_page_no(), .slot_no = -1};
  parent->insert_pair(idx + 1, key, r);
  new_node->set_parent_page_no(parent->get_page_no());

  if (parent->get_size() >= parent->get_max_size()) {
    IxNodeHandle* new_parent = split(parent);
    insert_into_parent(parent, new_parent->get_key(0), new_parent, transaction);
    buffer_pool_manager_->unpin_page(new_parent->get_page_id(), true);
    delete new_parent;
  }

  buffer_pool_manager_->unpin_page(parent->get_page_id(), true);
  delete parent;
}

// 插入键值对；重复 key 返回 -1，成功返回所在叶子页号
page_id_t IxIndexHandle::insert_entry(const char* key, const Rid& value,
                                      Transaction* transaction) {
  std::scoped_lock lock{root_latch_};
  auto pair = find_leaf_page(key, Operation::INSERT, transaction);
  IxNodeHandle* leaf = pair.first;
  page_id_t leaf_page_no = leaf->get_page_no();

  int idx = leaf->lower_bound(key);
  if (idx < leaf->get_size() &&
      ix_compare(leaf->get_key(idx), key, file_hdr_->col_types_,
                 file_hdr_->col_lens_) == 0) {
    buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
    delete leaf;
    return -1;
  }
  leaf->insert_pair(idx, key, value);

  if (idx == 0) {
    maintain_parent(leaf);
  }

  if (leaf->get_size() >= leaf->get_max_size()) {
    IxNodeHandle* new_leaf = split(leaf);
    insert_into_parent(leaf, new_leaf->get_key(0), new_leaf, transaction);
    buffer_pool_manager_->unpin_page(new_leaf->get_page_id(), true);
    delete new_leaf;
  }

  buffer_pool_manager_->unpin_page(leaf->get_page_id(), true);
  delete leaf;
  return leaf_page_no;
}

// 简化实现：只从叶子里抹掉 key，不做合并/重分配
bool IxIndexHandle::delete_entry(const char* key, Transaction* transaction) {
  std::scoped_lock lock{root_latch_};
  auto pair = find_leaf_page(key, Operation::DELETE, transaction);
  IxNodeHandle* leaf = pair.first;
  int old_size = leaf->get_size();
  int new_size = leaf->remove(key);
  bool deleted = (new_size != old_size);
  if (deleted && new_size > 0) {
    maintain_parent(leaf);
  }
  buffer_pool_manager_->unpin_page(leaf->get_page_id(), true);
  delete leaf;
  return deleted;
}

bool IxIndexHandle::coalesce_or_redistribute(IxNodeHandle* node,
                                             Transaction* transaction,
                                             bool* root_is_latched) {
  return false;
}

bool IxIndexHandle::adjust_root(IxNodeHandle* old_root_node) { return false; }

void IxIndexHandle::redistribute(IxNodeHandle* neighbor_node,
                                 IxNodeHandle* node, IxNodeHandle* parent,
                                 int index) {}

bool IxIndexHandle::coalesce(IxNodeHandle** neighbor_node, IxNodeHandle** node,
                             IxNodeHandle** parent, int index,
                             Transaction* transaction, bool* root_is_latched) {
  return false;
}

Rid IxIndexHandle::get_rid(const Iid& iid) const {
  IxNodeHandle* node = fetch_node(iid.page_no);
  if (iid.slot_no >= node->get_size()) {
    buffer_pool_manager_->unpin_page(node->get_page_id(), false);
    delete node;
    throw IndexEntryNotFoundError();
  }
  Rid rid = *node->get_rid(iid.slot_no);
  buffer_pool_manager_->unpin_page(node->get_page_id(), false);
  delete node;
  return rid;
}

// 返回第一个 >= key 的位置；越界则跳到下一个叶子的开头或停在 last_leaf 的末尾
Iid IxIndexHandle::lower_bound(const char* key) {
  std::scoped_lock lock{root_latch_};
  auto pair = find_leaf_page(key, Operation::FIND, nullptr);
  IxNodeHandle* leaf = pair.first;
  int idx = leaf->lower_bound(key);
  Iid iid;
  if (idx == leaf->get_size() && leaf->get_page_no() != file_hdr_->last_leaf_) {
    iid = {.page_no = leaf->get_next_leaf(), .slot_no = 0};
  } else {
    iid = {.page_no = leaf->get_page_no(), .slot_no = idx};
  }
  buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
  delete leaf;
  return iid;
}

Iid IxIndexHandle::upper_bound(const char* key) {
  std::scoped_lock lock{root_latch_};
  auto pair = find_leaf_page(key, Operation::FIND, nullptr);
  IxNodeHandle* leaf = pair.first;
  int idx = leaf->upper_bound(key);
  Iid iid;
  if (idx == leaf->get_size() && leaf->get_page_no() != file_hdr_->last_leaf_) {
    iid = {.page_no = leaf->get_next_leaf(), .slot_no = 0};
  } else {
    iid = {.page_no = leaf->get_page_no(), .slot_no = idx};
  }
  buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
  delete leaf;
  return iid;
}

Iid IxIndexHandle::leaf_end() const {
  IxNodeHandle* node = fetch_node(file_hdr_->last_leaf_);
  Iid iid = {.page_no = file_hdr_->last_leaf_, .slot_no = node->get_size()};
  buffer_pool_manager_->unpin_page(node->get_page_id(), false);
  delete node;
  return iid;
}

Iid IxIndexHandle::leaf_begin() const {
  Iid iid = {.page_no = file_hdr_->first_leaf_, .slot_no = 0};
  return iid;
}

IxNodeHandle* IxIndexHandle::fetch_node(int page_no) const {
  Page* page = buffer_pool_manager_->fetch_page(PageId{fd_, page_no});
  return new IxNodeHandle(file_hdr_, page);
}

IxNodeHandle* IxIndexHandle::create_node() {
  file_hdr_->num_pages_++;
  PageId new_page_id = {.fd = fd_, .page_no = INVALID_PAGE_ID};
  Page* page = buffer_pool_manager_->new_page(&new_page_id);
  return new IxNodeHandle(file_hdr_, page);
}

// 叶子/内部节点首键变动后，沿父链向上同步祖先里指向自己的 key
void IxIndexHandle::maintain_parent(IxNodeHandle* node) {
  IxNodeHandle* curr = node;
  while (curr->get_parent_page_no() != IX_NO_PAGE) {
    IxNodeHandle* parent = fetch_node(curr->get_parent_page_no());
    int rank = parent->find_child(curr);
    char* parent_key = parent->get_key(rank);
    char* child_first_key = curr->get_key(0);
    bool changed =
        (memcmp(parent_key, child_first_key, file_hdr_->col_tot_len_) != 0);
    if (changed) {
      memcpy(parent_key, child_first_key, file_hdr_->col_tot_len_);
    }
    if (curr != node) {
      buffer_pool_manager_->unpin_page(curr->get_page_id(), true);
      delete curr;
    }
    // rank > 0 时，parent 自己的首键没变，不必继续向上
    if (!changed || rank > 0) {
      buffer_pool_manager_->unpin_page(parent->get_page_id(), changed);
      delete parent;
      return;
    }
    curr = parent;
  }
  if (curr != node) {
    buffer_pool_manager_->unpin_page(curr->get_page_id(), true);
    delete curr;
  }
}

void IxIndexHandle::erase_leaf(IxNodeHandle* leaf) {
  assert(leaf->is_leaf_page());

  IxNodeHandle* prev = fetch_node(leaf->get_prev_leaf());
  prev->set_next_leaf(leaf->get_next_leaf());
  buffer_pool_manager_->unpin_page(prev->get_page_id(), true);
  delete prev;

  IxNodeHandle* next = fetch_node(leaf->get_next_leaf());
  next->set_prev_leaf(leaf->get_prev_leaf());
  buffer_pool_manager_->unpin_page(next->get_page_id(), true);
  delete next;
}

void IxIndexHandle::release_node_handle(IxNodeHandle& node) {
  file_hdr_->num_pages_--;
}

void IxIndexHandle::maintain_child(IxNodeHandle* node, int child_idx) {
  if (!node->is_leaf_page()) {
    int child_page_no = node->value_at(child_idx);
    IxNodeHandle* child = fetch_node(child_page_no);
    child->set_parent_page_no(node->get_page_no());
    buffer_pool_manager_->unpin_page(child->get_page_id(), true);
    delete child;
  }
}
