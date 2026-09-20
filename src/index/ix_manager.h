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

#include <memory>
#include <string>
#include <vector>

#include "ix_defs.h"
#include "ix_index_handle.h"
#include "system/sm_meta.h"

class IxManager {
 private:
  DiskManager* disk_manager_;
  BufferPoolManager* buffer_pool_manager_;

 public:
  IxManager(DiskManager* disk_manager, BufferPoolManager* buffer_pool_manager)
      : disk_manager_(disk_manager),
        buffer_pool_manager_(buffer_pool_manager) {}

  std::string get_index_name(const std::string& filename,
                             const std::vector<std::string>& index_cols) {
    std::string index_name = filename;
    for (size_t i = 0; i < index_cols.size(); ++i)
      index_name += "_" + index_cols[i];
    index_name += ".idx";

    return index_name;
  }

  std::string get_index_name(const std::string& filename,
                             const std::vector<ColMeta>& index_cols) {
    std::string index_name = filename;
    for (size_t i = 0; i < index_cols.size(); ++i)
      index_name += "_" + index_cols[i].name;
    index_name += ".idx";

    return index_name;
  }

  bool exists(const std::string& filename,
              const std::vector<ColMeta>& index_cols) {
    auto ix_name = get_index_name(filename, index_cols);
    return disk_manager_->is_file(ix_name);
  }

  bool exists(const std::string& filename,
              const std::vector<std::string>& index_cols) {
    auto ix_name = get_index_name(filename, index_cols);
    return disk_manager_->is_file(ix_name);
  }

  void create_index(const std::string& filename,
                    const std::vector<ColMeta>& index_cols) {
    std::string ix_name = get_index_name(filename, index_cols);
    // Create index file
    disk_manager_->create_file(ix_name);
    // Open index file
    int fd = disk_manager_->open_file(ix_name);

    // Create file header and write to file
    // Theoretically we have: |page_hdr| + (|attr| + |rid|) * n <= PAGE_SIZE
    // but we reserve one slot for convenient inserting and deleting, i.e.
    // |page_hdr| + (|attr| + |rid|) * (n + 1) <= PAGE_SIZE
    int col_tot_len = 0;
    int col_num = index_cols.size();
    for (auto& col : index_cols) {
      col_tot_len += col.len;
    }
    if (col_tot_len > IX_MAX_COL_LEN) {
      throw InvalidColLengthError(col_tot_len);
    }
    // 根据 |page_hdr| + (|attr| + |rid|) * (n + 1) <= PAGE_SIZE
    // 求得n的最大值btree_order 即 n <=
    // btree_order，那么btree_order就是每个结点最多可插入的键值对数量（实际还多留了一个空位，但其不可插入）
    int btree_order = static_cast<int>(
        (PAGE_SIZE - sizeof(IxPageHdr)) / (col_tot_len + sizeof(Rid)) - 1);
    assert(btree_order > 2);

    // Create file header and write to file
    IxFileHdr* fhdr =
        new IxFileHdr(IX_NO_PAGE, IX_INIT_NUM_PAGES, IX_INIT_ROOT_PAGE, col_num,
                      col_tot_len, btree_order, (btree_order + 1) * col_tot_len,
                      IX_INIT_ROOT_PAGE, IX_INIT_ROOT_PAGE);
    for (int i = 0; i < col_num; ++i) {
      fhdr->col_types_.push_back(index_cols[i].type);
      fhdr->col_lens_.push_back(index_cols[i].len);
    }
    fhdr->update_tot_len();

    char* data = new char[fhdr->tot_len_];
    fhdr->serialize(data);

    disk_manager_->write_page(fd, IX_FILE_HDR_PAGE, data, fhdr->tot_len_);

    char page_buf
        [PAGE_SIZE];  // 在内存中初始化page_buf中的内容，然后将其写入磁盘
    memset(page_buf, 0, PAGE_SIZE);
    // 注意leaf header页号为1，也标记为叶子结点，其前一个/后一个叶子均指向root
    // node Create leaf list header page and write to file
    {
      memset(page_buf, 0, PAGE_SIZE);
      auto phdr = reinterpret_cast<IxPageHdr*>(page_buf);
      *phdr = {
          .next_free_page_no = IX_NO_PAGE,
          .parent = IX_NO_PAGE,
          .num_key = 0,
          .is_leaf = true,
          .prev_leaf = IX_INIT_ROOT_PAGE,
          .next_leaf = IX_INIT_ROOT_PAGE,
      };
      disk_manager_->write_page(fd, IX_LEAF_HEADER_PAGE, page_buf, PAGE_SIZE);
    }
    // 注意root node页号为2，也标记为叶子结点，其前一个/后一个叶子均指向leaf
    // header Create root node and write to file
    {
      memset(page_buf, 0, PAGE_SIZE);
      auto phdr = reinterpret_cast<IxPageHdr*>(page_buf);
      *phdr = {
          .next_free_page_no = IX_NO_PAGE,
          .parent = IX_NO_PAGE,
          .num_key = 0,
          .is_leaf = true,
          .prev_leaf = IX_LEAF_HEADER_PAGE,
          .next_leaf = IX_LEAF_HEADER_PAGE,
      };
      // Must write PAGE_SIZE here in case of future fetch_node()
      disk_manager_->write_page(fd, IX_INIT_ROOT_PAGE, page_buf, PAGE_SIZE);
    }

    disk_manager_->set_fd2pageno(fd, IX_INIT_NUM_PAGES - 1);  // DEBUG

    // Close index file
    disk_manager_->close_file(fd);
  }

  void destroy_index(const std::string& filename,
                     const std::vector<ColMeta>& index_cols) {
    std::string ix_name = get_index_name(filename, index_cols);
    disk_manager_->destroy_file(ix_name);
  }

  void destroy_index(const std::string& filename,
                     const std::vector<std::string>& index_cols) {
    std::string ix_name = get_index_name(filename, index_cols);
    disk_manager_->destroy_file(ix_name);
  }

  // 注意这里打开文件，创建并返回了index file handle的指针
  std::unique_ptr<IxIndexHandle> open_index(
      const std::string& filename, const std::vector<ColMeta>& index_cols) {
    std::string ix_name = get_index_name(filename, index_cols);
    int fd = disk_manager_->open_file(ix_name);
    return std::make_unique<IxIndexHandle>(disk_manager_, buffer_pool_manager_,
                                           fd);
  }

  std::unique_ptr<IxIndexHandle> open_index(
      const std::string& filename, const std::vector<std::string>& index_cols) {
    std::string ix_name = get_index_name(filename, index_cols);
    int fd = disk_manager_->open_file(ix_name);
    return std::make_unique<IxIndexHandle>(disk_manager_, buffer_pool_manager_,
                                           fd);
  }

  // 静态检查点用：把索引的 file header 与全部脏页落盘，但**不关闭**索引。
  //
  // file_hdr_（其中的 root_page_）不在缓冲池里——它由 disk_manager_->write_page
  // 直写，所以 flush_all_pages 碰不到它，而此前**只有 close_index() 会写它**
  // （close_db / drop index / drop table）。检查点因此只刷了数据页、没刷头：
  // 崩溃后索引句柄从磁盘读回一个陈旧 root_page_，整棵树只剩根那一小片。
  // 无检查点时恢复会 destroy+create 全量重建索引，陈旧头无害；一旦检查点让
  // 恢复去**信任**磁盘上的索引（未被触及的表不重建），这棵树就废了。
  // 实测：零负载下发一次检查点再 SIGKILL，order_line 索引从 15,002,806 条
  // 掉到 84 条。
  void flush_index(const IxIndexHandle* ih) {
    std::vector<char> data(ih->file_hdr_->tot_len_);
    ih->file_hdr_->serialize(data.data());
    disk_manager_->write_page(ih->fd_, IX_FILE_HDR_PAGE, data.data(),
                              ih->file_hdr_->tot_len_);
    buffer_pool_manager_->flush_all_pages(ih->fd_);
  }

  void close_index(const IxIndexHandle* ih) {
    char* data = new char[ih->file_hdr_->tot_len_];
    ih->file_hdr_->serialize(data);
    disk_manager_->write_page(ih->fd_, IX_FILE_HDR_PAGE, data,
                              ih->file_hdr_->tot_len_);
    // 缓冲区的所有页刷到磁盘，注意这句话必须写在close_file前面
    buffer_pool_manager_->flush_all_pages(ih->fd_);
    // 把该 fd 在 BPM 中的页驱逐，避免 fd 复用后读到旧索引的数据
    buffer_pool_manager_->delete_all_pages(ih->fd_);
    disk_manager_->close_file(ih->fd_);
    delete[] data;
  }
};