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

#include <memory>

#include "defs.h"

namespace ast { struct TreeNode; }

// 可重入 flex/bison 接口：每次解析独立的 yyscan_t，无全局词法/AST 状态，
// 调用方负责 yylex_init -> yy_scan_string -> yyparse -> yy_delete_buffer -> yylex_destroy。
// 与 flex 生成的 typedef 共用守卫宏，避免重复定义。
#ifndef YY_TYPEDEF_YY_SCANNER_T
#define YY_TYPEDEF_YY_SCANNER_T
typedef void *yyscan_t;
#endif

typedef struct yy_buffer_state *YY_BUFFER_STATE;

int yylex_init(yyscan_t *scanner);
int yylex_destroy(yyscan_t scanner);

// 解析结果经 parse_result 输出(取代原全局 ast::parse_tree)，返回 0 表示成功。
int yyparse(yyscan_t scanner, std::shared_ptr<ast::TreeNode> *parse_result);

YY_BUFFER_STATE yy_scan_string(const char *str, yyscan_t scanner);

void yy_delete_buffer(YY_BUFFER_STATE buffer, yyscan_t scanner);
