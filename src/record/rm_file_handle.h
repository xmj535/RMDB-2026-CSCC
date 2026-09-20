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

#include <assert.h>

#include <memory>
#include <mutex>

#include "bitmap.h"
#include "common/context.h"
#include "rm_defs.h"

class RmManager;

/* 对表数据文件中的页面进行封装 */
struct RmPageHandle {
    const RmFileHdr *file_hdr;  // 当前页面所在文件的文件头指针
    Page *page;                 // 页面的实际数据，包括页面存储的数据、元信息等
    RmPageHdr *page_hdr;        // page->data的第一部分，存储页面元信息，指针指向首地址，长度为sizeof(RmPageHdr)
    char *bitmap;               // page->data的第二部分，存储页面的bitmap，指针指向首地址，长度为file_hdr->bitmap_size
    char *slots;                // page->data的第三部分，存储表的记录，指针指向首地址，每个slot的长度为file_hdr->record_size

    RmPageHandle(const RmFileHdr *fhdr_, Page *page_) : file_hdr(fhdr_), page(page_) {
        page_hdr = reinterpret_cast<RmPageHdr *>(page->get_data() + page->OFFSET_PAGE_HDR);
        bitmap = page->get_data() + sizeof(RmPageHdr) + page->OFFSET_PAGE_HDR;
        slots = bitmap + file_hdr->bitmap_size;
    }

    // 返回指定slot_no的slot存储收地址
    char* get_slot(int slot_no) const {
        return slots + slot_no * file_hdr->record_size;  // slots的首地址 + slot个数 * 每个slot的大小(每个record的大小)
    }
};

/* 每个RmFileHandle对应一个表的数据文件，里面有多个page，每个page的数据封装在RmPageHandle中 */
class RmFileHandle {      
    friend class RmScan;    
    friend class RmManager;

   private:
    DiskManager *disk_manager_;
    BufferPoolManager *buffer_pool_manager_;
    // 守护 bitmap/num_records/空闲链 的结构性修改：insert_record 从空闲链取页、
    // 占槽、满页摘链与 delete_record 腾槽、挂链之间存在并发竞态——两个线程拿到
    // 同一"空闲"页时，后者会发现页满而返回 Rid{-1,-1}，上层把它写进 WAL 形成
    // 毒丸（恢复时 fetch_page(-1) 直接杀死 server）且该行悄悄丢失。
    std::mutex heap_latch_;
    int fd_;        // 打开文件后产生的文件句柄
    RmFileHdr file_hdr_;    // 文件头，维护当前表文件的元数据

   public:
    RmFileHandle(DiskManager *disk_manager, BufferPoolManager *buffer_pool_manager, int fd)
        : disk_manager_(disk_manager), buffer_pool_manager_(buffer_pool_manager), fd_(fd) {
        // 注意：这里从磁盘中读出文件描述符为fd的文件的file_hdr，读到内存中
        // 这里实际就是初始化file_hdr，只不过是从磁盘中读出进行初始化
        // init file_hdr_
        disk_manager_->read_page(fd, RM_FILE_HDR_PAGE, (char *)&file_hdr_, sizeof(file_hdr_));
        // disk_manager管理的fd对应的文件中，设置从file_hdr_.num_pages开始分配page_no
        disk_manager_->set_fd2pageno(fd, file_hdr_.num_pages);
    }

    RmFileHdr get_file_hdr() { return file_hdr_; }
    int GetFd() { return fd_; }

    /* 判断指定位置上是否已经存在一条记录，通过Bitmap来判断 */
    bool is_record(const Rid &rid) const {
        RmPageHandle page_handle = fetch_page_handle(rid.page_no);
        bool set = Bitmap::is_set(page_handle.bitmap, rid.slot_no);  // page的slot_no位置上是否有record
        // 必须释放引用：此前漏 unpin，使每次 is_record 永久占一帧引用——
        // 全表扫描每行触发(2次/行)，表超过约半个 buffer pool(~128MB)即把
        // 65536 帧全部钉死，后续 new_page 返回空指针 → server 段错误。
        // 大表扫描时若不及时 unpin，会把全部 Buffer Pool 帧固定住，
        // 后续 new_page 无法获得可用帧并可能导致服务异常。
        buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), false);
        return set;
    }

    std::unique_ptr<RmRecord> get_record(const Rid &rid, Context *context) const;

    Rid insert_record(char *buf, Context *context);

    void insert_record(const Rid &rid, char *buf);

    void delete_record(const Rid &rid, Context *context);

    void update_record(const Rid &rid, char *buf, Context *context);

    RmPageHandle create_new_page_handle();

    RmPageHandle fetch_page_handle(int page_no) const;

    // 故障恢复用：根据各页 bitmap 重算每页记录数并重建空闲页链，
    // 使恢复后的新插入能正确找到有空槽的页。
    void recover_free_list();

    // 一趟遍历同时重建空闲链并回调每条存活记录。
    //
    // 恢复原先是两趟全表：recover_free_list 走一遍全部页，随后 RmScan 再走一遍。
    // 4.45 GiB 的库配 1 GiB 缓冲池时，第一趟读进来的页在第二趟开始前就被淘汰，
    // 等于把整库读了两次——实测冷缓存下这第二趟占恢复耗时的 65%（34.9s→12.3s）。
    // 合并成一趟后，只剩"非满页"的收尾遍历用于串空闲链；装载后的表几乎全是满页，
    // 该收尾通常每表只有一页。
    //
    // on_record(rid, data)：data 指向仍被 pin 的页内槽位，仅在回调期间有效，
    // 调用方需立即取用（例如就地拼索引键），不得留存指针。
    // 逐页遍历而非 RmScan：全空页也必须访问，否则其 num_records 不会被重算、
    // 也不会挂回空闲链。
    template <typename Fn>
    void rebuild_free_list_and_scan(Fn &&on_record) {
        std::vector<int> partial;  // 非满页，升序
        const int max_recs = file_hdr_.num_records_per_page;
        for (int page_no = RM_FIRST_RECORD_PAGE; page_no < file_hdr_.num_pages;
             ++page_no) {
            RmPageHandle ph = fetch_page_handle(page_no);
            int cnt = 0;
            for (int slot = 0; slot < max_recs; ++slot) {
                if (Bitmap::is_set(ph.bitmap, slot)) {
                    ++cnt;
                    on_record(Rid{page_no, slot}, ph.get_slot(slot));
                }
            }
            const bool full = (cnt == max_recs);
            bool changed = (ph.page_hdr->num_records != cnt);
            ph.page_hdr->num_records = cnt;
            if (full) {
                if (ph.page_hdr->next_free_page_no != RM_NO_PAGE) {
                    ph.page_hdr->next_free_page_no = RM_NO_PAGE;
                    changed = true;
                }
            } else {
                partial.push_back(page_no);
            }
            buffer_pool_manager_->unpin_page(ph.page->get_page_id(), changed);
        }
        // 只回访非满页。倒序串链使最终链表按页号升序，与 recover_free_list 同序。
        file_hdr_.first_free_page_no = RM_NO_PAGE;
        for (auto it = partial.rbegin(); it != partial.rend(); ++it) {
            RmPageHandle ph = fetch_page_handle(*it);
            const bool changed =
                (ph.page_hdr->next_free_page_no != file_hdr_.first_free_page_no);
            ph.page_hdr->next_free_page_no = file_hdr_.first_free_page_no;
            file_hdr_.first_free_page_no = *it;
            buffer_pool_manager_->unpin_page(ph.page->get_page_id(), changed);
        }
        disk_manager_->write_page(fd_, RM_FILE_HDR_PAGE,
                                  reinterpret_cast<char *>(&file_hdr_),
                                  sizeof(file_hdr_));
    }

    // 静态检查点用：把堆文件的 file header 落盘。
    //
    // file_hdr_ 由 disk_manager_->write_page 直写，不经缓冲池，所以检查点的
    // flush_all_pages 碰不到它。检查点路径下未被触及的表既不重建索引也不重算
    // 空闲链、直接信任磁盘态，此时磁盘上的 num_pages / first_free_page_no 必须
    // 是检查点那一刻的值。R031 去掉了插入热路径上的空闲链头同步写之后，这里
    // 就成了它唯一的持久化点。
    void flush_file_hdr() {
        disk_manager_->write_page(fd_, RM_FILE_HDR_PAGE,
                                  reinterpret_cast<char *>(&file_hdr_),
                                  sizeof(file_hdr_));
    }

   private:
    RmPageHandle create_page_handle();

    void release_page_handle(RmPageHandle &page_handle);
};