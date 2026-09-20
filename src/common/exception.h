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

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>


/** ExceptionType 是我们系统中可能抛出的所有异常类型。 */
enum class ExceptionType {
  /** 无效的异常类型。*/
  INVALID = 0,
  /** 值超出范围。 */
  OUT_OF_RANGE = 1,
  /** 转换/类型转换错误。 */
  CONVERSION = 2,
  /** 类型子系统中的未知类型。 */
  UNKNOWN_TYPE = 3,
  /** 与小数相关的错误。 */
  DECIMAL = 4,
  /** 类型不匹配。 */
  MISMATCH_TYPE = 5,
  /** 被 0 除。 */
  DIVIDE_BY_ZERO = 6,
  /** 不兼容的类型。 */
  INCOMPATIBLE_TYPE = 8,
  /** 内存不足错误。 */
  OUT_OF_MEMORY = 9,
  /** 方法未实现。 */
  NOT_IMPLEMENTED = 11,
  /** 执行异常。 */
  EXECUTION = 12,
};

extern std::atomic<bool> global_disable_execution_exception_print;

class Exception : public std::runtime_error {
 public:
  /**
   * 构造一个新的 Exception 实例。
   * @param message 异常消息
   */
  explicit Exception(const std::string &message, bool print = true)
      : std::runtime_error(message), type_(ExceptionType::INVALID) {
#ifndef NDEBUG
    if (print) {
      std::string exception_message = "Message :: " + message + "\n";
      std::cerr << exception_message;
    }
#endif
  }

  /**
   * 构造一个具有指定类型的新 Exception 实例。
   * @param exception_type 异常类型
   * @param message 异常消息
   */
  Exception(ExceptionType exception_type, const std::string &message, bool print = true)
      : std::runtime_error(message), type_(exception_type) {
#ifndef NDEBUG
    if (print && !global_disable_execution_exception_print.load()) {
      std::string exception_message =
          "\nException Type :: " + ExceptionTypeToString(type_) + ", Message :: " + message + "\n\n";
      std::cerr << exception_message;
    }
#endif
  }

  /** @return 异常的类型 */
  auto GetType() const -> ExceptionType { return type_; }

  /** @return 指定异常类型的可读字符串 */
  static auto ExceptionTypeToString(ExceptionType type) -> std::string {
    switch (type) {
      case ExceptionType::INVALID:
        return "Invalid";
      case ExceptionType::OUT_OF_RANGE:
        return "Out of Range";
      case ExceptionType::CONVERSION:
        return "Conversion";
      case ExceptionType::UNKNOWN_TYPE:
        return "Unknown Type";
      case ExceptionType::DECIMAL:
        return "Decimal";
      case ExceptionType::MISMATCH_TYPE:
        return "Mismatch Type";
      case ExceptionType::DIVIDE_BY_ZERO:
        return "Divide by Zero";
      case ExceptionType::INCOMPATIBLE_TYPE:
        return "Incompatible type";
      case ExceptionType::OUT_OF_MEMORY:
        return "Out of Memory";
      case ExceptionType::NOT_IMPLEMENTED:
        return "Not implemented";
      case ExceptionType::EXECUTION:
        return "Execution";
      default:
        return "Unknown";
    }
  }

 private:
  ExceptionType type_;
};

class NotImplementedException : public Exception {
 public:
  NotImplementedException() = delete;
  explicit NotImplementedException(const std::string &msg) : Exception(ExceptionType::NOT_IMPLEMENTED, msg) {}
};

class ExecutionException : public Exception {
 public:
  ExecutionException() = delete;
  explicit ExecutionException(const std::string &msg) : Exception(ExceptionType::EXECUTION, msg, true) {}
};