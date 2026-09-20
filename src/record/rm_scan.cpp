/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL
v2. You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "rm_scan.h"

#include "rm_file_handle.h"

// run_3a: 取页。命中缓存(同页且已 pin)直接复用已 pin 的帧;否则先换出旧页,
// 再 fetch 新页并保持 pin。返回的 RmPageHandle 复用 cur_page_ 这一帧,不额外加 pin。
RmPageHandle RmScan::fetch_page_pinned(int page_no) {
  if (cur_page_ != nullptr && cur_page_no_ == page_no) {
    return RmPageHandle(&file_handle_->file_hdr_, cur_page_);
  }
  release_cur_page();
  RmPageHandle ph = file_handle_->fetch_page_handle(page_no);  // pin +1
  cur_page_ = ph.page;
  cur_page_no_ = page_no;
  return ph;
}

// run_3a: 释放当前 pin 住的页(幂等)。任何离开当前页/扫描结束/析构都必须经此,
// 保证 pin 严格守恒(见 rm_file_handle.h is_record 钉帧致 server 段错误的教训)。
void RmScan::release_cur_page() {
  if (cur_page_ != nullptr) {
    file_handle_->buffer_pool_manager_->unpin_page(cur_page_->get_page_id(),
                                                   false);
    cur_page_ = nullptr;
    cur_page_no_ = -1;
  }
}

RmScan::~RmScan() { release_cur_page(); }

// 构造时即定位到文件中第一条有效记录；空文件则 rid_ 直接落到 end 位置
RmScan::RmScan(const RmFileHandle* file_handle) : file_handle_(file_handle) {
  // 先把 rid_ 置成"末尾"，找到第一条有效记录再覆盖
  rid_.page_no = file_handle_->file_hdr_.num_pages;
  rid_.slot_no = 0;

  const int records_per_page = file_handle_->file_hdr_.num_records_per_page;
  for (int page_no = RM_FIRST_RECORD_PAGE;
       page_no < file_handle_->file_hdr_.num_pages; ++page_no) {
    RmPageHandle ph = fetch_page_pinned(page_no);
    int slot_no = Bitmap::first_bit(true, ph.bitmap, records_per_page);

    if (slot_no < records_per_page) {
      rid_ = Rid{page_no, slot_no};
      return;  // 保持 cur_page_ 的 pin,供后续逐槽 next()/get_record 复用
    }
  }
  release_cur_page();  // 空表/无记录:释放循环中可能 pin 的最后一页
}

// 移动到下一条有效记录；当前页找完就跨页继续，跨完所有页则进入 end 状态
void RmScan::next() {
  if (is_end()) {
    return;
  }

  const int records_per_page = file_handle_->file_hdr_.num_records_per_page;
  int page_no = rid_.page_no;
  int slot_no = rid_.slot_no;

  while (page_no < file_handle_->file_hdr_.num_pages) {
    RmPageHandle ph = fetch_page_pinned(page_no);
    int next_slot =
        Bitmap::next_bit(true, ph.bitmap, records_per_page, slot_no);

    if (next_slot < records_per_page) {
      rid_ = Rid{page_no, next_slot};
      return;  // 保持 cur_page_ 的 pin
    }
    // 当前页没有更多，从下一页的第一个槽位继续找
    ++page_no;
    slot_no = -1;
  }

  release_cur_page();  // 扫描结束:释放最后 pin 的页
  rid_.page_no = file_handle_->file_hdr_.num_pages;
  rid_.slot_no = 0;
}

// 是否已扫描完所有页
bool RmScan::is_end() const {
  return rid_.page_no >= file_handle_->file_hdr_.num_pages;
}

// 当前光标指向的 Rid
Rid RmScan::rid() const { return rid_; }

// 复用 cur_page_ 这一帧读出当前记录，避免 get_record 再走一次 fetch_page/unpin。
bool RmScan::copy_current_record(char* out) {
  if (is_end()) return false;
  RmPageHandle ph = fetch_page_pinned(rid_.page_no);
  if (!Bitmap::is_set(ph.bitmap, rid_.slot_no)) return false;
  memcpy(out, ph.get_slot(rid_.slot_no), file_handle_->file_hdr_.record_size);
  return true;
}
