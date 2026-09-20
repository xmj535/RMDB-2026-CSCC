/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL
v2. You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "rm_file_handle.h"

#include <cstring>

#include "errors.h"

// page_no 范围合法：[RM_FIRST_RECORD_PAGE, num_pages)
static inline bool valid_page_no(int page_no, const RmFileHdr& hdr) {
  return page_no >= RM_FIRST_RECORD_PAGE && page_no < hdr.num_pages;
}

// 按 rid 读出一条记录的拷贝；槽位为空返回 nullptr，page 越界抛
// PageNotExistError
std::unique_ptr<RmRecord> RmFileHandle::get_record(const Rid& rid,
                                                   Context* context) const {
  (void)context;

  if (!valid_page_no(rid.page_no, file_hdr_)) {
    throw PageNotExistError(disk_manager_->get_file_name(fd_), rid.page_no);
  }
  if (rid.slot_no < 0 || rid.slot_no >= file_hdr_.num_records_per_page) {
    return nullptr;
  }

  RmPageHandle ph = fetch_page_handle(rid.page_no);
  if (!Bitmap::is_set(ph.bitmap, rid.slot_no)) {
    buffer_pool_manager_->unpin_page(ph.page->get_page_id(), false);
    return nullptr;
  }

  auto record = std::make_unique<RmRecord>(file_hdr_.record_size);
  std::memcpy(record->data, ph.get_slot(rid.slot_no), file_hdr_.record_size);
  buffer_pool_manager_->unpin_page(ph.page->get_page_id(), false);
  return record;
}

// 找一个有空槽的页插入 buf，返回新记录的 Rid；若该页插完变满则更新空闲链
Rid RmFileHandle::insert_record(char* buf, Context* context) {
  (void)context;
  std::lock_guard<std::mutex> guard(heap_latch_);

  RmPageHandle ph = create_page_handle();

  int slot_no =
      Bitmap::first_bit(false, ph.bitmap, file_hdr_.num_records_per_page);
  while (slot_no >= file_hdr_.num_records_per_page) {
    // 防御：空闲链头是满页（历史不一致状态遗留）。摘链后换页重试——
    // 绝不能返回 Rid{-1,-1}：上层会把它写进 WAL（恢复毒丸）且丢行。
    file_hdr_.first_free_page_no = ph.page_hdr->next_free_page_no;
    ph.page_hdr->next_free_page_no = RM_NO_PAGE;
    // 只改了空闲链头，不落盘：recover_free_list() 按页 bitmap 完全重算空闲链，
    // 而检查点路径下未被触及的表本就不会插入。陈旧链头由本函数上方的防御分支
    // 兜住（它正是为"链头是满页"设计的）。这次 pwrite 原本发生在 heap_latch_
    // 持有期间，是全局临界区里的一次系统调用。num_pages 的落盘仍然保留。
    buffer_pool_manager_->unpin_page(ph.page->get_page_id(), true);
    ph = create_page_handle();
    slot_no =
        Bitmap::first_bit(false, ph.bitmap, file_hdr_.num_records_per_page);
  }

  std::memcpy(ph.get_slot(slot_no), buf, file_hdr_.record_size);
  Bitmap::set(ph.bitmap, slot_no);
  ph.page_hdr->num_records++;

  Rid rid{ph.page->get_page_id().page_no, slot_no};

  // 这一页满了，把它从空闲链上摘掉
  if (ph.page_hdr->num_records == file_hdr_.num_records_per_page) {
    file_hdr_.first_free_page_no = ph.page_hdr->next_free_page_no;
    ph.page_hdr->next_free_page_no = RM_NO_PAGE;
    // 同上：空闲链头不落盘（恢复时重算）。这里原本每装满一页就在锁内 pwrite 一次。
  }

  buffer_pool_manager_->unpin_page(ph.page->get_page_id(), true);
  return rid;
}

// 在指定 rid 位置写入记录（恢复时用），原槽未占则也维护 bitmap 和空闲链
void RmFileHandle::insert_record(const Rid& rid, char* buf) {
  std::lock_guard<std::mutex> guard(heap_latch_);
  if (!valid_page_no(rid.page_no, file_hdr_)) {
    throw PageNotExistError(disk_manager_->get_file_name(fd_), rid.page_no);
  }
  if (rid.slot_no < 0 || rid.slot_no >= file_hdr_.num_records_per_page) {
    return;
  }

  RmPageHandle ph = fetch_page_handle(rid.page_no);

  bool was_set = Bitmap::is_set(ph.bitmap, rid.slot_no);
  std::memcpy(ph.get_slot(rid.slot_no), buf, file_hdr_.record_size);

  if (!was_set) {
    Bitmap::set(ph.bitmap, rid.slot_no);
    ph.page_hdr->num_records++;

    if (ph.page_hdr->num_records == file_hdr_.num_records_per_page) {
      file_hdr_.first_free_page_no = ph.page_hdr->next_free_page_no;
      ph.page_hdr->next_free_page_no = RM_NO_PAGE;
      disk_manager_->write_page(fd_, RM_FILE_HDR_PAGE,
                                reinterpret_cast<char*>(&file_hdr_),
                                sizeof(file_hdr_));
    }
  }

  buffer_pool_manager_->unpin_page(ph.page->get_page_id(), true);
}

// 删掉 rid 处记录：清 bitmap、num_records--；若页之前是满的则重新挂回空闲链
void RmFileHandle::delete_record(const Rid& rid, Context* context) {
  (void)context;
  std::lock_guard<std::mutex> guard(heap_latch_);

  if (!valid_page_no(rid.page_no, file_hdr_)) {
    throw PageNotExistError(disk_manager_->get_file_name(fd_), rid.page_no);
  }
  if (rid.slot_no < 0 || rid.slot_no >= file_hdr_.num_records_per_page) {
    return;
  }

  RmPageHandle ph = fetch_page_handle(rid.page_no);

  if (!Bitmap::is_set(ph.bitmap, rid.slot_no)) {
    buffer_pool_manager_->unpin_page(ph.page->get_page_id(), false);
    return;
  }

  const bool was_full =
      (ph.page_hdr->num_records == file_hdr_.num_records_per_page);

  Bitmap::reset(ph.bitmap, rid.slot_no);
  ph.page_hdr->num_records--;

  // 删除前页是满的，现在腾出空间了，重新挂回空闲链
  if (was_full) {
    release_page_handle(ph);
  }

  buffer_pool_manager_->unpin_page(ph.page->get_page_id(), true);
}

// 原地覆盖 rid 处记录内容；槽位本来是空的就直接跳过
void RmFileHandle::update_record(const Rid& rid, char* buf, Context* context) {
  (void)context;

  if (!valid_page_no(rid.page_no, file_hdr_)) {
    throw PageNotExistError(disk_manager_->get_file_name(fd_), rid.page_no);
  }
  if (rid.slot_no < 0 || rid.slot_no >= file_hdr_.num_records_per_page) {
    return;
  }

  RmPageHandle ph = fetch_page_handle(rid.page_no);
  if (!Bitmap::is_set(ph.bitmap, rid.slot_no)) {
    buffer_pool_manager_->unpin_page(ph.page->get_page_id(), false);
    return;
  }

  std::memcpy(ph.get_slot(rid.slot_no), buf, file_hdr_.record_size);
  buffer_pool_manager_->unpin_page(ph.page->get_page_id(), true);
}

// 从 buffer pool 取出某页并包装成 RmPageHandle（含 page_hdr / bitmap / slots
// 指针）
RmPageHandle RmFileHandle::fetch_page_handle(int page_no) const {
  if (!valid_page_no(page_no, file_hdr_)) {
    throw PageNotExistError(disk_manager_->get_file_name(fd_), page_no);
  }

  PageId pid{fd_, page_no};
  Page* page = buffer_pool_manager_->fetch_page(pid);
  if (page == nullptr) {
    throw PageNotExistError(disk_manager_->get_file_name(fd_), page_no);
  }
  return RmPageHandle(&file_hdr_, page);
}

// 给文件追加一个新页：初始化页头/bitmap，挂到空闲链表首，并把文件头写回磁盘
RmPageHandle RmFileHandle::create_new_page_handle() {
  PageId pid{fd_, file_hdr_.num_pages};
  Page* page = buffer_pool_manager_->new_page(&pid);
  if (page == nullptr) {
    throw RMDBError("Fail to create new page");
  }

  RmPageHandle ph(&file_hdr_, page);
  ph.page_hdr->next_free_page_no = file_hdr_.first_free_page_no;
  ph.page_hdr->num_records = 0;
  Bitmap::init(ph.bitmap, file_hdr_.bitmap_size);

  file_hdr_.first_free_page_no = pid.page_no;
  file_hdr_.num_pages++;

  disk_manager_->write_page(fd_, RM_FILE_HDR_PAGE,
                            reinterpret_cast<char*>(&file_hdr_),
                            sizeof(file_hdr_));
  return ph;
}

// 拿一个有空槽的页：空闲链非空就复用首部，否则新建一页
RmPageHandle RmFileHandle::create_page_handle() {
  if (file_hdr_.first_free_page_no == RM_NO_PAGE) {
    return create_new_page_handle();
  }
  return fetch_page_handle(file_hdr_.first_free_page_no);
}

// 把页插回空闲链头，让后续 insert 优先使用它；同步刷新文件头
void RmFileHandle::release_page_handle(RmPageHandle& page_handle) {
  page_handle.page_hdr->next_free_page_no = file_hdr_.first_free_page_no;
  file_hdr_.first_free_page_no = page_handle.page->get_page_id().page_no;
  // 空闲链头不落盘（恢复时由 recover_free_list 重算）。DELETE 每让一个满页重新
  // 可用就在 heap_latch_ 内 pwrite 一次，Delivery 的批量 delete 尤其密集。
}

// 故障恢复用：扫描所有数据页，按 bitmap 重算 num_records，并重建空闲页链表
void RmFileHandle::recover_free_list() {
  file_hdr_.first_free_page_no = RM_NO_PAGE;
  // 从后往前挂链，使最终链表按页号升序，行为接近正常运行时
  for (int page_no = file_hdr_.num_pages - 1; page_no >= RM_FIRST_RECORD_PAGE;
       --page_no) {
    RmPageHandle ph = fetch_page_handle(page_no);
    int cnt = 0;
    for (int slot = 0; slot < file_hdr_.num_records_per_page; ++slot) {
      if (Bitmap::is_set(ph.bitmap, slot)) ++cnt;
    }
    const int new_next = (cnt < file_hdr_.num_records_per_page)
                             ? file_hdr_.first_free_page_no
                             : RM_NO_PAGE;
    // 绝大多数页在崩溃时并未被改动，重算结果与盘上完全一致。原先无条件按脏页
    // unpin，等于把整库(本次 4.6 GiB / 约 110 万页)全部标脏并回写一遍；
    // 只有真正变化的页才需要落盘。
    const bool changed = (ph.page_hdr->num_records != cnt) ||
                         (ph.page_hdr->next_free_page_no != new_next);
    ph.page_hdr->num_records = cnt;
    ph.page_hdr->next_free_page_no = new_next;
    if (cnt < file_hdr_.num_records_per_page) {
      file_hdr_.first_free_page_no = page_no;
    }
    buffer_pool_manager_->unpin_page(ph.page->get_page_id(), changed);
  }
  disk_manager_->write_page(fd_, RM_FILE_HDR_PAGE,
                            reinterpret_cast<char*>(&file_hdr_),
                            sizeof(file_hdr_));
}
