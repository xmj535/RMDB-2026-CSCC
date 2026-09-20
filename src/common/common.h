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

#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include "defs.h"
#include "record/rm_defs.h"


// 聚合函数类型；AGG_NONE 表示该列为普通列引用
enum AggType { AGG_NONE, AGG_COUNT, AGG_MAX, AGG_MIN, AGG_SUM, AGG_AVG };

struct TabCol {
    std::string tab_name;
    std::string col_name;
    // 以下字段仅在 SELECT 列表 / HAVING 条件中有效；
    // 普通列引用（WHERE / ORDER BY / JOIN）保留默认值
    AggType agg = AGG_NONE;
    bool is_star = false;     // COUNT(*) 时为 true
    bool is_distinct = false; // COUNT(DISTINCT col) 时为 true
    std::string alias;        // SELECT 列表中 AS 指定的显示名

    friend bool operator<(const TabCol &x, const TabCol &y) {
        return std::make_pair(x.tab_name, x.col_name) < std::make_pair(y.tab_name, y.col_name);
    }
};

struct Value {
    ColType type;  // type of value
    union {
        int int_val;      // int value
        float float_val;  // float value
    };
    std::string str_val;  // string value

    std::shared_ptr<RmRecord> raw;  // raw record buffer

    // 原始 SQL 文本表示，用于 EXPLAIN ANALYZE 复原书写形式（含字符串引号）
    std::string raw_str;

    // Wire v3 prepared-plan slot, 1-based. Zero means an ordinary literal.
    // The dictionary retains the declared wire type; `type` records normal
    // SQL coercion for this particular plan value.
    uint16_t parameter_index = 0;
    void set_int(int int_val_) {
        type = TYPE_INT;
        int_val = int_val_;
    }

    void set_float(float float_val_) {
        type = TYPE_FLOAT;
        float_val = float_val_;
    }

    void set_str(std::string str_val_) {
        type = TYPE_STRING;
        str_val = std::move(str_val_);
    }

    void init_raw(int len) {
        assert(raw == nullptr);
        raw = std::make_shared<RmRecord>(len);
        if (type == TYPE_INT) {
            assert(len == sizeof(int));
            *(int *)(raw->data) = int_val;
        } else if (type == TYPE_FLOAT) {
            assert(len == sizeof(float));
            *(float *)(raw->data) = float_val;
        } else if (type == TYPE_STRING) {
            if (len < (int)str_val.size()) {
                throw StringOverflowError();
            }
            memset(raw->data, 0, len);
            memcpy(raw->data, str_val.c_str(), str_val.size());
        }
    }
};

enum CompOp { OP_EQ, OP_NE, OP_LT, OP_GT, OP_LE, OP_GE };

struct Condition {
    TabCol lhs_col;   // left-hand side column
    CompOp op;        // comparison operator
    bool is_rhs_val;  // true if right-hand side is a value (not a column)
    TabCol rhs_col;   // right-hand side column
    Value rhs_val;    // right-hand side value
    // 右值的原始 SQL 文本（用于 EXPLAIN 输出），rhs 是列时为空
    std::string rhs_raw;
};

struct SetClauseArithmeticTerm {
    char op = 0;
    Value rhs;
};

struct SetClause {
    TabCol lhs;
    Value rhs;
    // true 表示直接列赋值 lhs = rhs_col；即使两列相同也必须保留写语义。
    bool rhs_is_col = false;
    // 算术更新首项：op != 0 时语义为 lhs = rhs_col op rhs。
    // op == 0 时由 rhs_is_col 区分字面量赋值和直接列赋值。
    // trailing_terms 按 SQL 出现顺序保存后续 +/- 值，执行时左结合。
    // 典型算术更新：set balance = balance - 100 + 20。
    char op = 0;
    TabCol rhs_col;
    std::vector<SetClauseArithmeticTerm> trailing_terms;
};

// HAVING 条件：左值是聚合或分组列，右值必须是字面量
struct HavingCond {
    TabCol lhs_col;       // 含 agg / is_star 信息
    CompOp op;
    Value rhs_val;
    std::string rhs_raw;  // EXPLAIN / 调试输出用
};
