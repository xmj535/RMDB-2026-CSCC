/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL
v2. You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */
#include "storage/disk_manager.h"

#include <assert.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "defs.h"

// 把每个 fd 对应的"下一个可分配 page_no"清零
DiskManager::DiskManager() { memset(fd2pageno_, 0, sizeof(fd2pageno_)); }

// 把 num_bytes 字节写到 fd 文件第 page_no 页对应的偏移处
void DiskManager::write_page(int fd, page_id_t page_no, const char* offset,
                             int num_bytes) {
  off_t pos = (off_t)page_no * PAGE_SIZE;
  ssize_t n = pwrite(fd, offset, num_bytes, pos);
  if (n != num_bytes) {
    perror("DiskManager::write_page pwrite");
    throw InternalError("DiskManager::write_page Error");
  }
}

// 从 fd 文件第 page_no 页读 num_bytes 字节到 offset
void DiskManager::read_page(int fd, page_id_t page_no, char* offset,
                            int num_bytes) {
  off_t pos = (off_t)page_no * PAGE_SIZE;
  ssize_t n = pread(fd, offset, num_bytes, pos);
  if (n != num_bytes) {
    perror("DiskManager::read_page pread");
    throw InternalError("DiskManager::read_page Error");
  }
}

// 给指定文件分配一个新的 page_no（简单自增，不复用空洞）
page_id_t DiskManager::allocate_page(int fd) {
  assert(fd >= 0 && fd < MAX_FD);
  return fd2pageno_[fd]++;
}

// 暂未实现真正的回收，留空让接口可调
void DiskManager::deallocate_page(__attribute__((unused)) page_id_t page_id) {}

// path 是否为已存在的目录
bool DiskManager::is_dir(const std::string& path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// 通过 shell 调用创建一个新目录
void DiskManager::create_dir(const std::string& path) {
  std::string cmd = "mkdir " + path;
  if (system(cmd.c_str()) < 0) {
    throw UnixError();
  }
}

// 递归删除目录及其内容
void DiskManager::destroy_dir(const std::string& path) {
  std::string cmd = "rm -r " + path;
  if (system(cmd.c_str()) < 0) {
    throw UnixError();
  }
}

// path 是否为已存在的普通文件
bool DiskManager::is_file(const std::string& path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

// 创建一个空文件；若同名文件已存在则抛 FileExistsError
void DiskManager::create_file(const std::string& path) {
  if (is_file(path)) {
    throw FileExistsError(path);
  }
  int fd = open(path.c_str(), O_CREAT | O_WRONLY, 0644);
  if (fd == -1) {
    throw InternalError("DiskManager::create_file open failed");
  }
  if (close(fd) == -1) {
    throw InternalError("DiskManager::create_file close failed");
  }
}

// 删除文件；必须先关闭、且文件须存在
void DiskManager::destroy_file(const std::string& path) {
  if (!is_file(path)) {
    throw FileNotFoundError(path);
  }
  // 删除前必须先关闭，否则 fd 表会悬空
  if (path2fd_.count(path)) {
    throw FileNotClosedError(path);
  }
  if (unlink(path.c_str()) == -1) {
    perror("DiskManager::destroy_file unlink");
    throw InternalError("DiskManager::destroy_file Error");
  }
}

// 以读写模式打开文件并登记到 path↔fd 双向表，返回 fd
int DiskManager::open_file(const std::string& path) {
  if (!is_file(path)) {
    throw FileNotFoundError(path);
  }
  if (path2fd_.count(path)) {
    throw FileNotClosedError(path);
  }
  int fd = open(path.c_str(), O_RDWR);
  if (fd == -1) {
    perror("DiskManager::open_file");
    throw InternalError("DiskManager::open_file Error");
  }
  path2fd_[path] = fd;
  fd2path_[fd] = path;
  return fd;
}

// 关闭已打开的 fd，并把它从 path↔fd 表里移除
void DiskManager::close_file(int fd) {
  auto it = fd2path_.find(fd);
  if (it == fd2path_.end()) {
    throw FileNotOpenError(fd);
  }
  if (close(fd) == -1) {
    perror("DiskManager::close_file");
    throw InternalError("DiskManager::close_file Error");
  }
  path2fd_.erase(it->second);
  fd2path_.erase(it);
}

// 取文件字节大小；文件不存在返回 -1
int64_t DiskManager::get_file_size(const std::string& file_name) {
  struct stat st;
  return stat(file_name.c_str(), &st) == 0 ? st.st_size : -1;
}

// 由 fd 反查文件名；未打开则抛错
std::string DiskManager::get_file_name(int fd) {
  auto it = fd2path_.find(fd);
  if (it == fd2path_.end()) {
    throw FileNotOpenError(fd);
  }
  return it->second;
}

// 取文件名对应的 fd；尚未打开就顺手打开
int DiskManager::get_file_fd(const std::string& file_name) {
  auto it = path2fd_.find(file_name);
  if (it != path2fd_.end()) {
    return it->second;
  }
  return open_file(file_name);
}

// 从 offset 开始读日志；offset 越界返回 -1，越读越少时只读剩余部分
int64_t DiskManager::read_log(char* log_data, int64_t size, int64_t offset) {
  if (log_fd_ == -1) {
    log_fd_ = open_file(LOG_FILE_NAME);
  }
  int64_t file_size = get_file_size(LOG_FILE_NAME);
  if (offset > file_size) {
    return -1;
  }
  size = std::min(size, file_size - offset);
  if (size == 0) {
    return 0;
  }
  lseek(log_fd_, offset, SEEK_SET);
  ssize_t bytes_read = read(log_fd_, log_data, size);
  assert(bytes_read == size);
  return bytes_read;
}

// 追加写日志到 db.log 末尾
void DiskManager::write_log(char* log_data, int size) {
  if (log_fd_ == -1) {
    log_fd_ = open_file(LOG_FILE_NAME);
  }
  if (lseek(log_fd_, 0, SEEK_END) == -1) {
    perror("DiskManager::write_log lseek");
    throw UnixError();
  }
  int bytes_written = 0;
  while (bytes_written < size) {
    ssize_t count =
        write(log_fd_, log_data + bytes_written, size - bytes_written);
    if (count > 0) {
      bytes_written += static_cast<int>(count);
      continue;
    }
    if (count < 0 && errno == EINTR) {
      continue;
    }
    perror("DiskManager::write_log write");
    throw UnixError();
  }
}

void DiskManager::sync_log() {
  if (log_fd_ == -1) {
    throw InternalError("DiskManager::sync_log without an open WAL");
  }
  while (fdatasync(log_fd_) == -1) {
    if (errno == EINTR) {
      continue;
    }
    perror("DiskManager::sync_log fdatasync");
    throw UnixError();
  }
}
