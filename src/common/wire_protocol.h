/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */

#pragma once

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <unordered_set>
#include <vector>

#include <unistd.h>

#include "record/rm_defs.h"
#include "system/sm_meta.h"

namespace wire {

constexpr size_t kHandshakeSize = 8;
constexpr size_t kFrameHeaderSize = 8;
constexpr uint32_t kMaxPayload = 1U << 20;
constexpr uint32_t kMaxDiagnostic = 64U << 10;

constexpr uint8_t kExecStream = 0x20;
constexpr uint8_t kPrepareSet = 0x21;
constexpr uint8_t kExecBatch = 0x22;

constexpr uint8_t kMeta = 0x01;
constexpr uint8_t kRow = 0x02;
constexpr uint8_t kCommandOk = 0x10;
constexpr uint8_t kResultEnd = 0x11;
constexpr uint8_t kTransactionAbort = 0x12;
constexpr uint8_t kError = 0x13;
constexpr uint8_t kPrepareOk = 0x14;
constexpr uint8_t kBatchResult = 0x15;

constexpr uint8_t kBatchOk = 0;
constexpr uint8_t kBatchAbort = 1;
constexpr uint8_t kBatchError = 2;

constexpr uint8_t kInt32 = 0x01;
constexpr uint8_t kFloat32 = 0x02;
constexpr uint8_t kChar = 0x03;

constexpr uint8_t kHandshake[kHandshakeSize] = {'R', 'M', 'D', 'B',
                                                0x00, 0x03, 0x00, 0x00};

class ProtocolError : public std::runtime_error {
 public:
  explicit ProtocolError(const std::string& message)
      : std::runtime_error(message) {}
};

class IoError : public std::runtime_error {
 public:
  explicit IoError(const std::string& message) : std::runtime_error(message) {}
};

struct Frame {
  uint8_t tag = 0;
  uint8_t flags = 0;
  std::vector<uint8_t> payload;
};

struct Column {
  std::string name;
  uint8_t type = 0;
};

struct PrepareRequest {
  uint16_t statement_id = 0;
  uint8_t result_kind = 0;
  std::vector<uint8_t> parameter_types;
  std::string sql;
};

struct PreparedSchema {
  uint16_t statement_id = 0;
  std::vector<Column> columns;
};

struct BatchQueryResult {
  uint16_t operation_index = 0;
  std::vector<std::vector<uint8_t>> rows;
};

inline uint16_t load_u16_be(const uint8_t* data) {
  return (static_cast<uint16_t>(data[0]) << 8) |
         static_cast<uint16_t>(data[1]);
}

inline uint32_t load_u32_be(const uint8_t* data) {
  return (static_cast<uint32_t>(data[0]) << 24) |
         (static_cast<uint32_t>(data[1]) << 16) |
         (static_cast<uint32_t>(data[2]) << 8) |
         static_cast<uint32_t>(data[3]);
}

inline void append_u16(std::vector<uint8_t>& out, uint16_t value) {
  out.push_back(static_cast<uint8_t>(value >> 8));
  out.push_back(static_cast<uint8_t>(value));
}

inline void append_u32(std::vector<uint8_t>& out, uint32_t value) {
  out.push_back(static_cast<uint8_t>(value >> 24));
  out.push_back(static_cast<uint8_t>(value >> 16));
  out.push_back(static_cast<uint8_t>(value >> 8));
  out.push_back(static_cast<uint8_t>(value));
}

inline void append_u64(std::vector<uint8_t>& out, uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    out.push_back(static_cast<uint8_t>(value >> shift));
  }
}

// Returns false only for a clean EOF before reading any byte. A partial field
// is a protocol error because TCP fragmentation must not change semantics.
inline bool read_exact(int fd, void* destination, size_t length,
                       bool allow_clean_eof = false) {
  auto* out = static_cast<uint8_t*>(destination);
  size_t done = 0;
  while (done < length) {
    ssize_t count = ::read(fd, out + done, length - done);
    if (count > 0) {
      done += static_cast<size_t>(count);
      continue;
    }
    if (count == 0) {
      if (allow_clean_eof && done == 0) return false;
      throw ProtocolError("connection closed in the middle of a protocol field");
    }
    if (errno == EINTR) continue;
    throw IoError(std::string("socket read failed: ") + std::strerror(errno));
  }
  return true;
}

inline void write_all(int fd, const void* source, size_t length) {
  const auto* data = static_cast<const uint8_t*>(source);
  size_t done = 0;
  while (done < length) {
    ssize_t count = ::write(fd, data + done, length - done);
    if (count > 0) {
      done += static_cast<size_t>(count);
      continue;
    }
    if (count < 0 && errno == EINTR) continue;
    if (count == 0) throw IoError("socket write made no progress");
    throw IoError(std::string("socket write failed: ") + std::strerror(errno));
  }
}

inline bool valid_utf8(const uint8_t* data, size_t length) {
  size_t i = 0;
  while (i < length) {
    uint8_t c = data[i++];
    if (c <= 0x7f) continue;
    if (c >= 0xc2 && c <= 0xdf) {
      if (i >= length || (data[i] & 0xc0) != 0x80) return false;
      ++i;
      continue;
    }
    if (c == 0xe0) {
      if (i + 1 >= length || data[i] < 0xa0 || data[i] > 0xbf ||
          (data[i + 1] & 0xc0) != 0x80) return false;
      i += 2;
      continue;
    }
    if ((c >= 0xe1 && c <= 0xec) || (c >= 0xee && c <= 0xef)) {
      if (i + 1 >= length || (data[i] & 0xc0) != 0x80 ||
          (data[i + 1] & 0xc0) != 0x80) return false;
      i += 2;
      continue;
    }
    if (c == 0xed) {
      if (i + 1 >= length || data[i] < 0x80 || data[i] > 0x9f ||
          (data[i + 1] & 0xc0) != 0x80) return false;
      i += 2;
      continue;
    }
    if (c == 0xf0) {
      if (i + 2 >= length || data[i] < 0x90 || data[i] > 0xbf ||
          (data[i + 1] & 0xc0) != 0x80 ||
          (data[i + 2] & 0xc0) != 0x80) return false;
      i += 3;
      continue;
    }
    if (c >= 0xf1 && c <= 0xf3) {
      if (i + 2 >= length || (data[i] & 0xc0) != 0x80 ||
          (data[i + 1] & 0xc0) != 0x80 ||
          (data[i + 2] & 0xc0) != 0x80) return false;
      i += 3;
      continue;
    }
    if (c == 0xf4) {
      if (i + 2 >= length || data[i] < 0x80 || data[i] > 0x8f ||
          (data[i + 1] & 0xc0) != 0x80 ||
          (data[i + 2] & 0xc0) != 0x80) return false;
      i += 3;
      continue;
    }
    return false;
  }
  return true;
}

inline bool valid_utf8(const std::string& value) {
  return valid_utf8(reinterpret_cast<const uint8_t*>(value.data()),
                    value.size());
}

inline std::string diagnostic_text(std::string message) {
  if (!valid_utf8(message)) message = "internal error";
  if (message.size() <= kMaxDiagnostic) return message;
  message.resize(kMaxDiagnostic);
  while (!message.empty() && !valid_utf8(message)) message.pop_back();
  return message;
}

// Bounds-checked payload reader. Lengths are checked against bytes already
// present in the <=1 MiB frame before a variable-size object is allocated.
class PayloadReader {
 public:
  explicit PayloadReader(const std::vector<uint8_t>& payload)
      : payload_(payload) {}

  size_t remaining() const { return payload_.size() - offset_; }

  uint8_t u8(const char* field) {
    require(1, field);
    return payload_[offset_++];
  }

  uint16_t u16(const char* field) {
    require(2, field);
    uint16_t value = load_u16_be(payload_.data() + offset_);
    offset_ += 2;
    return value;
  }

  uint32_t u32(const char* field) {
    require(4, field);
    uint32_t value = load_u32_be(payload_.data() + offset_);
    offset_ += 4;
    return value;
  }

  const uint8_t* bytes(size_t count, const char* field) {
    require(count, field);
    const uint8_t* result = payload_.data() + offset_;
    offset_ += count;
    return result;
  }

  void finish(const char* structure) const {
    if (remaining() != 0) {
      throw ProtocolError(std::string(structure) +
                          " has unexplained trailing bytes");
    }
  }

 private:
  void require(size_t count, const char* field) const {
    if (count > remaining()) {
      throw ProtocolError(std::string("truncated ") + field);
    }
  }

  const std::vector<uint8_t>& payload_;
  size_t offset_ = 0;
};

inline std::vector<PrepareRequest> decode_prepare_set(
    const std::vector<uint8_t>& payload) {
  PayloadReader reader(payload);
  uint16_t statement_count = reader.u16("PREPARE_SET.statement_count");
  if (statement_count == 0 || statement_count > 256) {
    throw ProtocolError("PREPARE_SET statement_count must be in 1..256");
  }

  std::vector<PrepareRequest> requests;
  requests.reserve(statement_count);
  std::unordered_set<uint16_t> ids;
  for (uint16_t i = 0; i < statement_count; ++i) {
    PrepareRequest request;
    request.statement_id = reader.u16("PREPARE_SET.statement_id");
    if (request.statement_id == 0 ||
        !ids.insert(request.statement_id).second) {
      throw ProtocolError(
          "PREPARE_SET statement ids must be nonzero and unique");
    }
    request.result_kind = reader.u8("PREPARE_SET.result_kind");
    if (request.result_kind > 1) {
      throw ProtocolError("PREPARE_SET result_kind must be 0 or 1");
    }

    uint16_t parameter_count =
        reader.u16("PREPARE_SET.parameter_count");
    if (reader.remaining() < static_cast<size_t>(parameter_count) + 4U) {
      throw ProtocolError("truncated PREPARE_SET parameter types");
    }
    request.parameter_types.reserve(parameter_count);
    for (uint16_t p = 0; p < parameter_count; ++p) {
      uint8_t type = reader.u8("PREPARE_SET.parameter_sql_type");
      if (type != kInt32 && type != kFloat32 && type != kChar) {
        throw ProtocolError("PREPARE_SET has an unknown parameter SQL type");
      }
      request.parameter_types.push_back(type);
    }

    uint32_t sql_bytes = reader.u32("PREPARE_SET.sql_bytes");
    if (sql_bytes == 0) {
      throw ProtocolError("PREPARE_SET SQL must be non-empty");
    }
    const uint8_t* sql = reader.bytes(sql_bytes, "PREPARE_SET.sql");
    if (std::find(sql, sql + sql_bytes, 0) != sql + sql_bytes) {
      throw ProtocolError("PREPARE_SET SQL must not contain NUL");
    }
    if (!valid_utf8(sql, sql_bytes)) {
      throw ProtocolError("PREPARE_SET SQL is not valid UTF-8");
    }
    request.sql.assign(reinterpret_cast<const char*>(sql), sql_bytes);
    requests.push_back(std::move(request));
  }
  reader.finish("PREPARE_SET");
  return requests;
}

inline void append_column_definition(std::vector<uint8_t>& payload,
                                     const Column& column) {
  if (column.name.empty() ||
      column.name.size() > std::numeric_limits<uint16_t>::max() ||
      !valid_utf8(column.name)) {
    throw ProtocolError("invalid PREPARE_OK column name");
  }
  if (column.type != kInt32 && column.type != kFloat32 &&
      column.type != kChar) {
    throw ProtocolError("invalid PREPARE_OK column SQL type");
  }
  append_u16(payload, static_cast<uint16_t>(column.name.size()));
  payload.insert(payload.end(), column.name.begin(), column.name.end());
  payload.push_back(column.type);
}

inline std::vector<uint8_t> encode_prepare_ok(
    const std::vector<PreparedSchema>& schemas) {
  if (schemas.empty() || schemas.size() > 256) {
    throw ProtocolError("PREPARE_OK statement_count must be in 1..256");
  }
  std::vector<uint8_t> payload;
  append_u16(payload, static_cast<uint16_t>(schemas.size()));
  std::unordered_set<uint16_t> ids;
  for (const auto& schema : schemas) {
    if (schema.statement_id == 0 ||
        !ids.insert(schema.statement_id).second) {
      throw ProtocolError("PREPARE_OK statement ids must be nonzero and unique");
    }
    if (schema.columns.size() > std::numeric_limits<uint16_t>::max()) {
      throw ProtocolError("PREPARE_OK column_count exceeds u16");
    }
    append_u16(payload, schema.statement_id);
    append_u16(payload, static_cast<uint16_t>(schema.columns.size()));
    for (const auto& column : schema.columns) {
      append_column_definition(payload, column);
      if (payload.size() > kMaxPayload) {
        throw ProtocolError("PREPARE_OK payload exceeds 1 MiB");
      }
    }
  }
  return payload;
}

inline std::vector<uint8_t> encode_batch_result(
    uint16_t operation_count, uint16_t executed_operations, uint8_t status,
    uint16_t failed_operation, const std::string& diagnostic,
    const std::vector<BatchQueryResult>& results) {
  if (operation_count == 0 || operation_count > 256) {
    throw ProtocolError("BATCH_RESULT operation_count must be in 1..256");
  }
  if (status != kBatchOk && status != kBatchAbort && status != kBatchError) {
    throw ProtocolError("BATCH_RESULT has an unknown status");
  }

  std::string safe_diagnostic = diagnostic_text(diagnostic);
  if (status == kBatchOk) {
    if (executed_operations != operation_count) {
      throw ProtocolError("BATCH_RESULT OK executed count mismatch");
    }
    if (failed_operation != 0xffff || !safe_diagnostic.empty()) {
      throw ProtocolError("BATCH_RESULT OK has failure fields");
    }
  } else {
    if (executed_operations >= operation_count ||
        failed_operation != executed_operations || !results.empty()) {
      throw ProtocolError("BATCH_RESULT failure fields are inconsistent");
    }
  }
  if (results.size() > std::numeric_limits<uint16_t>::max()) {
    throw ProtocolError("BATCH_RESULT result_count exceeds u16");
  }

  std::vector<uint8_t> payload;
  append_u16(payload, executed_operations);
  payload.push_back(status);
  append_u16(payload, failed_operation);
  append_u32(payload, static_cast<uint32_t>(safe_diagnostic.size()));
  payload.insert(payload.end(), safe_diagnostic.begin(), safe_diagnostic.end());
  append_u16(payload, static_cast<uint16_t>(results.size()));

  int previous_index = -1;
  for (const auto& result : results) {
    if (result.operation_index >= operation_count ||
        static_cast<int>(result.operation_index) <= previous_index) {
      throw ProtocolError(
          "BATCH_RESULT operation indices must be strictly increasing");
    }
    previous_index = result.operation_index;
    if (result.rows.size() > std::numeric_limits<uint32_t>::max()) {
      throw ProtocolError("BATCH_RESULT row_count exceeds u32");
    }
    append_u16(payload, result.operation_index);
    append_u32(payload, static_cast<uint32_t>(result.rows.size()));
    for (const auto& row : result.rows) {
      if (row.empty()) {
        throw ProtocolError("BATCH_RESULT query row has no cells");
      }
      if (row.size() > kMaxPayload - std::min(payload.size(),
                                               static_cast<size_t>(kMaxPayload))) {
        throw ProtocolError("BATCH_RESULT payload exceeds 1 MiB");
      }
      payload.insert(payload.end(), row.begin(), row.end());
    }
    if (payload.size() > kMaxPayload) {
      throw ProtocolError("BATCH_RESULT payload exceeds 1 MiB");
    }
  }
  if (payload.size() > kMaxPayload) {
    throw ProtocolError("BATCH_RESULT payload exceeds 1 MiB");
  }
  return payload;
}

inline bool read_frame(int fd, Frame& frame) {
  uint8_t header[kFrameHeaderSize];
  if (!read_exact(fd, header, sizeof(header), true)) return false;

  uint32_t payload_bytes = load_u32_be(header);
  frame.tag = header[4];
  frame.flags = header[5];
  uint16_t reserved = load_u16_be(header + 6);
  if (payload_bytes > kMaxPayload) {
    throw ProtocolError("frame payload exceeds 1 MiB");
  }
  if (reserved != 0) throw ProtocolError("frame reserved field must be zero");
  switch (frame.tag) {
    case kExecStream:
    case kPrepareSet:
      if (frame.flags != 0) {
        throw ProtocolError("request frame uses an invalid flag");
      }
      break;
    case kExecBatch:
      if (frame.flags != 0x01) {
        throw ProtocolError("EXEC_BATCH requires AUTO_ABORT flag");
      }
      break;
    default:
      throw ProtocolError("unknown request frame tag");
  }
  frame.payload.resize(payload_bytes);
  if (payload_bytes != 0) {
    read_exact(fd, frame.payload.data(), frame.payload.size());
  }
  return true;
}

inline void write_frame(int fd, uint8_t tag,
                        const std::vector<uint8_t>& payload) {
  if (payload.size() > kMaxPayload) {
    throw ProtocolError("response frame payload exceeds 1 MiB");
  }
  uint8_t header[kFrameHeaderSize] = {};
  uint32_t size = static_cast<uint32_t>(payload.size());
  header[0] = static_cast<uint8_t>(size >> 24);
  header[1] = static_cast<uint8_t>(size >> 16);
  header[2] = static_cast<uint8_t>(size >> 8);
  header[3] = static_cast<uint8_t>(size);
  header[4] = tag;
  write_all(fd, header, sizeof(header));
  if (!payload.empty()) write_all(fd, payload.data(), payload.size());
}

inline uint8_t sql_type(ColType type) {
  switch (type) {
    case TYPE_INT:
      return kInt32;
    case TYPE_FLOAT:
      return kFloat32;
    case TYPE_STRING:
      return kChar;
  }
  throw ProtocolError("unsupported SQL result type");
}

class ResultWriter {
 public:
  ResultWriter() : fd_(-1), capture_only_(true) {}
  explicit ResultWriter(int fd) : fd_(fd) {}

  bool result_active() const { return result_active_; }
  uint64_t row_count() const { return row_count_; }
  const std::vector<Column>& columns() const { return columns_; }
  const std::vector<std::vector<uint8_t>>& captured_rows() const {
    return captured_rows_;
  }
  std::vector<std::vector<uint8_t>> take_captured_rows() {
    return std::move(captured_rows_);
  }

  void begin_result(std::vector<Column> columns) {
    if (result_active_) throw ProtocolError("a result stream is already active");
    if (columns.empty() || columns.size() > std::numeric_limits<uint16_t>::max()) {
      throw ProtocolError("META column count is outside 1..65535");
    }
    std::vector<uint8_t> payload;
    append_u16(payload, static_cast<uint16_t>(columns.size()));
    for (const auto& column : columns) {
      if (column.name.empty() ||
          column.name.size() > std::numeric_limits<uint16_t>::max() ||
          !valid_utf8(column.name)) {
        throw ProtocolError("invalid UTF-8 META column name");
      }
      if (column.type != kInt32 && column.type != kFloat32 &&
          column.type != kChar) {
        throw ProtocolError("invalid META SQL type");
      }
      append_u16(payload, static_cast<uint16_t>(column.name.size()));
      payload.insert(payload.end(), column.name.begin(), column.name.end());
      payload.push_back(column.type);
    }
    if (payload.size() > kMaxPayload) throw ProtocolError("META exceeds 1 MiB");
    columns_ = std::move(columns);
    row_count_ = 0;
    result_active_ = true;
    if (!capture_only_) write_frame(fd_, kMeta, payload);
  }

  void begin_record_result(const std::vector<std::string>& names,
                           const std::vector<ColMeta>& columns) {
    if (names.size() != columns.size()) {
      throw ProtocolError("result caption count does not match schema");
    }
    record_columns_ = columns;
    std::vector<Column> wire_columns;
    wire_columns.reserve(columns.size());
    for (size_t i = 0; i < columns.size(); ++i) {
      std::string name = names[i].empty() ? columns[i].name : names[i];
      wire_columns.push_back({std::move(name), sql_type(columns[i].type)});
    }
    begin_result(std::move(wire_columns));
  }

  void send_record(const RmRecord& record) {
    if (!result_active_ || record_columns_.size() != columns_.size()) {
      throw ProtocolError("ROW has no active record schema");
    }
    std::vector<uint8_t> payload;
    for (const auto& column : record_columns_) {
      if (column.offset < 0 || column.len < 0 ||
          static_cast<size_t>(column.offset) + static_cast<size_t>(column.len) >
              static_cast<size_t>(record.size)) {
        throw ProtocolError("result column lies outside tuple storage");
      }
      const char* value = record.data + column.offset;
      payload.push_back(1);  // This SQL engine has no NULL values.
      if (column.type == TYPE_INT) {
        int64_t integer = 0;
        if (column.len == static_cast<int>(sizeof(int32_t))) {
          int32_t value32;
          std::memcpy(&value32, value, sizeof(value32));
          integer = value32;
        } else if (column.len == static_cast<int>(sizeof(int64_t))) {
          std::memcpy(&integer, value, sizeof(integer));
        } else {
          throw ProtocolError("INT result is not 32-bit compatible");
        }
        if (integer < std::numeric_limits<int32_t>::min() ||
            integer > std::numeric_limits<int32_t>::max()) {
          throw ProtocolError("INT result is outside Wire INT32 range");
        }
        append_u32(payload, static_cast<uint32_t>(static_cast<int32_t>(integer)));
      } else if (column.type == TYPE_FLOAT) {
        if (column.len != static_cast<int>(sizeof(uint32_t))) {
          throw ProtocolError("FLOAT result is not binary32");
        }
        uint32_t bits;
        std::memcpy(&bits, value, sizeof(bits));
        append_u32(payload, bits);
      } else if (column.type == TYPE_STRING) {
        size_t logical = 0;
        while (logical < static_cast<size_t>(column.len) && value[logical] != '\0') {
          ++logical;
        }
        append_u32(payload, static_cast<uint32_t>(logical));
        payload.insert(payload.end(), value, value + logical);
      } else {
        throw ProtocolError("unsupported ROW SQL type");
      }
    }
    send_row_payload(payload);
  }

  void send_char_row(const std::vector<std::string>& values) {
    if (!result_active_ || values.size() != columns_.size()) {
      throw ProtocolError("CHAR ROW does not match active schema");
    }
    std::vector<uint8_t> payload;
    for (size_t i = 0; i < values.size(); ++i) {
      if (columns_[i].type != kChar) {
        throw ProtocolError("utility result row requires CHAR columns");
      }
      if (values[i].size() > std::numeric_limits<uint32_t>::max()) {
        throw ProtocolError("CHAR value exceeds u32 length");
      }
      payload.push_back(1);
      append_u32(payload, static_cast<uint32_t>(values[i].size()));
      payload.insert(payload.end(), values[i].begin(), values[i].end());
    }
    send_row_payload(payload);
  }

  void finish_result() {
    if (!result_active_) throw ProtocolError("RESULT_END has no active query");
    if (capture_only_) {
      result_active_ = false;
      return;
    }
    std::vector<uint8_t> payload;
    append_u64(payload, row_count_);
    write_frame(fd_, kResultEnd, payload);
    clear_result();
  }

  void command_ok() {
    if (result_active_) throw ProtocolError("COMMAND_OK cannot end a query");
    if (!capture_only_) write_frame(fd_, kCommandOk, {});
  }

  void error(const std::string& message) {
    terminal_diagnostic(kError, message);
  }

  void transaction_abort(const std::string& message) {
    terminal_diagnostic(kTransactionAbort, message);
  }

 private:
  void send_row_payload(const std::vector<uint8_t>& payload) {
    if (payload.size() > kMaxPayload) throw ProtocolError("ROW exceeds 1 MiB");
    if (capture_only_) {
      if (payload.size() > kMaxPayload - captured_bytes_) {
        throw ProtocolError("captured batch query rows exceed 1 MiB");
      }
      captured_bytes_ += payload.size();
      captured_rows_.push_back(payload);
    } else {
      write_frame(fd_, kRow, payload);
    }
    ++row_count_;
  }

  void terminal_diagnostic(uint8_t tag, const std::string& message) {
    if (capture_only_) {
      clear_result();
      throw ProtocolError("stream diagnostic cannot be captured as a batch row");
    }
    std::string safe = diagnostic_text(message);
    std::vector<uint8_t> payload(safe.begin(), safe.end());
    write_frame(fd_, tag, payload);
    clear_result();
  }

  void clear_result() {
    result_active_ = false;
    row_count_ = 0;
    columns_.clear();
    record_columns_.clear();
    captured_rows_.clear();
    captured_bytes_ = 0;
  }

  int fd_;
  bool capture_only_ = false;
  bool result_active_ = false;
  uint64_t row_count_ = 0;
  std::vector<Column> columns_;
  std::vector<ColMeta> record_columns_;
  std::vector<std::vector<uint8_t>> captured_rows_;
  size_t captured_bytes_ = 0;
};

}  // namespace wire
