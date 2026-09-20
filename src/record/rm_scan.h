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

#include "rm_defs.h"

class RmFileHandle;
struct RmPageHandle;

class RmScan : public RecScan {
    const RmFileHandle *file_handle_;
    Rid rid_;
    // run_3a: 当前 pin 住的页(对应 rid_.page_no)。同页逐槽推进时复用这一帧,避免
    // 每行重复 fetch_page(全局 BPM latch + page_table 查找);仅跨页时换出旧页、
    // pin 新页。析构/扫描结束必 unpin,严格守恒(见 rm_file_handle.h is_record 钉帧教训)。
    Page *cur_page_ = nullptr;
    int cur_page_no_ = -1;
public:
    RmScan(const RmFileHandle *file_handle);
    ~RmScan() override;

    void next() override;

    bool is_end() const override;

    Rid rid() const override;

    // 从当前已 pin 的页直接把记录拷进 out(至少 record_size 字节)。
    // 与 RmFileHandle::get_record(rid) 等价，但复用扫描已持有的帧，省掉逐行
    // 的 fetch_page/unpin 和一次 RmRecord 堆分配——全表重建索引时每行都付。
    // 槽位未占用(并发删除)返回 false。
    bool copy_current_record(char *out);

private:
    // 命中缓存(同页且已 pin)直接复用;否则换出旧页、fetch 新页并保持 pin。
    RmPageHandle fetch_page_pinned(int page_no);
    void release_cur_page();
};
