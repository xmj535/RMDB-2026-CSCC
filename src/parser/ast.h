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

enum JoinType { INNER_JOIN, LEFT_JOIN, RIGHT_JOIN, FULL_JOIN };
namespace ast {

enum SvType { SV_TYPE_INT, SV_TYPE_FLOAT, SV_TYPE_STRING, SV_TYPE_BOOL };

enum SvCompOp { SV_OP_EQ, SV_OP_NE, SV_OP_LT, SV_OP_GT, SV_OP_LE, SV_OP_GE };

enum OrderByDir { OrderBy_DEFAULT, OrderBy_ASC, OrderBy_DESC };

enum SetKnobType { EnableNestLoop, EnableSortMerge };

// 聚合函数类型；SV_AGG_NONE 表示该 Col 仍是普通列引用
enum SvAggType {
  SV_AGG_NONE,
  SV_AGG_COUNT,
  SV_AGG_MAX,
  SV_AGG_MIN,
  SV_AGG_SUM,
  SV_AGG_AVG
};

// Base class for tree nodes
struct TreeNode {
  virtual ~TreeNode() = default;  // enable polymorphism
};

struct Help : public TreeNode {};

struct ShowTables : public TreeNode {};

struct ShowIndex : public TreeNode {
  std::string tab_name;

  ShowIndex(std::string tab_name_) : tab_name(std::move(tab_name_)) {}
};

struct TxnBegin : public TreeNode {};

struct TxnCommit : public TreeNode {};

struct TxnAbort : public TreeNode {};

struct TxnRollback : public TreeNode {};

// SET TRANSACTION ISOLATION LEVEL <SNAPSHOT ISOLATION | SERIALIZABLE>
enum SvIsolationLevel { SV_ISO_SNAPSHOT_ISOLATION, SV_ISO_SERIALIZABLE };
struct SetIsolationStmt : public TreeNode {
  SvIsolationLevel level;
  SetIsolationStmt(SvIsolationLevel level_) : level(level_) {}
};

struct TypeLen : public TreeNode {
  SvType type;
  int len;

  TypeLen(SvType type_, int len_) : type(type_), len(len_) {}
};

struct Field : public TreeNode {};

struct ColDef : public Field {
  std::string col_name;
  std::shared_ptr<TypeLen> type_len;

  ColDef(std::string col_name_, std::shared_ptr<TypeLen> type_len_)
      : col_name(std::move(col_name_)), type_len(std::move(type_len_)) {}
};

struct CreateTable : public TreeNode {
  std::string tab_name;
  std::vector<std::shared_ptr<Field>> fields;

  CreateTable(std::string tab_name_,
              std::vector<std::shared_ptr<Field>> fields_)
      : tab_name(std::move(tab_name_)), fields(std::move(fields_)) {}
};

struct DropTable : public TreeNode {
  std::string tab_name;

  DropTable(std::string tab_name_) : tab_name(std::move(tab_name_)) {}
};

struct DescTable : public TreeNode {
  std::string tab_name;

  DescTable(std::string tab_name_) : tab_name(std::move(tab_name_)) {}
};

struct CreateIndex : public TreeNode {
  std::string tab_name;
  std::vector<std::string> col_names;

  CreateIndex(std::string tab_name_, std::vector<std::string> col_names_)
      : tab_name(std::move(tab_name_)), col_names(std::move(col_names_)) {}
};

struct DropIndex : public TreeNode {
  std::string tab_name;
  std::vector<std::string> col_names;

  DropIndex(std::string tab_name_, std::vector<std::string> col_names_)
      : tab_name(std::move(tab_name_)), col_names(std::move(col_names_)) {}
};

struct Expr : public TreeNode {};

// 字面量基类：raw_str 保留 SQL 源文本，供 EXPLAIN ANALYZE 还原原始书写形式
struct Value : public Expr {
  std::string raw_str;
};

struct IntLit : public Value {
  int val;

  IntLit(int val_) : val(val_) {}
  IntLit(int val_, std::string raw) : val(val_) { raw_str = std::move(raw); }
};

struct FloatLit : public Value {
  float val;

  FloatLit(float val_) : val(val_) {}
  FloatLit(float val_, std::string raw) : val(val_) {
    raw_str = std::move(raw);
  }
};

struct StringLit : public Value {
  std::string val;

  StringLit(std::string val_) : val(std::move(val_)) {}
  StringLit(std::string val_, std::string raw) : val(std::move(val_)) {
    raw_str = std::move(raw);
  }
};

// Wire v3 PREPARE_SET typed parameter marker. The ordinal remains source text
// until semantic analysis validates it against the connection-local type
// vector and creates an internal Value slot. EXEC_STREAM supplies no vector
// and therefore rejects ParamRef.
struct ParamRef : public Value {
  std::string ordinal;
  explicit ParamRef(std::string value) : ordinal(std::move(value)) {
    raw_str = "$" + ordinal;
  }
};
struct BoolLit : public Value {
  bool val;

  BoolLit(bool val_) : val(val_) {}
};

struct Col : public Expr {
  std::string tab_name;
  std::string col_name;
  // 聚合包装：SV_AGG_NONE 表示该 Col 仍为普通列引用
  SvAggType agg_type = SV_AGG_NONE;
  // COUNT(*) 时为 true，此时 col_name 取 "*"
  bool is_star = false;
  // COUNT(DISTINCT col) 时为 true；与普通 COUNT(col) 保持独立语义
  bool is_distinct = false;
  // SELECT 列表中的 AS 别名；HAVING / WHERE 条件中的列不使用
  std::string alias;

  Col(std::string tab_name_, std::string col_name_)
      : tab_name(std::move(tab_name_)), col_name(std::move(col_name_)) {}
};

struct SetClause : public TreeNode {
  struct ArithmeticTerm {
    char op;
    std::shared_ptr<Value> val;
  };

  std::string col_name;
  std::shared_ptr<Value> val;
  // 直接列赋值：set lhs = rhs_col。该标志与算术更新分开，确保自赋值
  // 仍进入完整 UPDATE 写路径，而不是被当成字面量或无操作跳过。
  bool rhs_is_col = false;
  // 算术更新的首项：set col = rhs_col op val（op 为 '+'/'-'/'*'）。
  // op == 0 且 rhs_is_col=false 表示字面量赋值；rhs_is_col=true 表示
  // 直接列赋值，二者均与算术更新区分。
  // trailing_terms 保存后续 +/- value，并按出现顺序左结合。
  // 典型场景：set balance = balance - 100 + 20。
  std::string rhs_col;
  char op = 0;
  std::vector<ArithmeticTerm> trailing_terms;

  SetClause(std::string col_name_, std::shared_ptr<Value> val_)
      : col_name(std::move(col_name_)), val(std::move(val_)) {}
  SetClause(std::string col_name_, std::string rhs_col_)
      : col_name(std::move(col_name_)),
        rhs_is_col(true),
        rhs_col(std::move(rhs_col_)) {}
  SetClause(std::string col_name_, std::string rhs_col_, char op_,
            std::shared_ptr<Value> val_)
      : col_name(std::move(col_name_)),
        val(std::move(val_)),
        rhs_col(std::move(rhs_col_)),
        op(op_) {}

  void append_term(char term_op, std::shared_ptr<Value> term_val) {
    trailing_terms.push_back({term_op, std::move(term_val)});
  }
};

struct BinaryExpr : public TreeNode {
  std::shared_ptr<Col> lhs;
  SvCompOp op;
  std::shared_ptr<Expr> rhs;

  BinaryExpr(std::shared_ptr<Col> lhs_, SvCompOp op_,
             std::shared_ptr<Expr> rhs_)
      : lhs(std::move(lhs_)), op(op_), rhs(std::move(rhs_)) {}
};

// ORDER BY 单项：列引用（可能含聚合）+ 排序方向
struct OrderByItem : public TreeNode {
  std::shared_ptr<Col> col;
  OrderByDir dir;
  OrderByItem(std::shared_ptr<Col> col_, OrderByDir dir_)
      : col(std::move(col_)), dir(dir_) {}
};

struct OrderBy : public TreeNode {
  std::vector<std::shared_ptr<OrderByItem>> items;
  OrderBy() = default;
  // 兼容旧的单列构造方式：保留下来供旧代码调用，新代码请直接使用 items
  OrderBy(std::shared_ptr<Col> cols_, OrderByDir orderby_dir_) {
    items.push_back(std::make_shared<OrderByItem>(std::move(cols_), orderby_dir_));
  }
};

struct InsertStmt : public TreeNode {
  std::string tab_name;
  std::vector<std::shared_ptr<Value>> vals;

  InsertStmt(std::string tab_name_, std::vector<std::shared_ptr<Value>> vals_)
      : tab_name(std::move(tab_name_)), vals(std::move(vals_)) {}
};

// load file_name into table_name; —— 批量 CSV 导入(性能测试 Phase 2 加载方式)
struct LoadStmt : public TreeNode {
  std::string file_path;
  std::string tab_name;

  LoadStmt(std::string file_path_, std::string tab_name_)
      : file_path(std::move(file_path_)), tab_name(std::move(tab_name_)) {}
};

struct DeleteStmt : public TreeNode {
  std::string tab_name;
  std::vector<std::shared_ptr<BinaryExpr>> conds;

  DeleteStmt(std::string tab_name_,
             std::vector<std::shared_ptr<BinaryExpr>> conds_)
      : tab_name(std::move(tab_name_)), conds(std::move(conds_)) {}
};

struct UpdateStmt : public TreeNode {
  std::string tab_name;
  std::vector<std::shared_ptr<SetClause>> set_clauses;
  std::vector<std::shared_ptr<BinaryExpr>> conds;

  UpdateStmt(std::string tab_name_,
             std::vector<std::shared_ptr<SetClause>> set_clauses_,
             std::vector<std::shared_ptr<BinaryExpr>> conds_)
      : tab_name(std::move(tab_name_)),
        set_clauses(std::move(set_clauses_)),
        conds(std::move(conds_)) {}
};

struct JoinExpr : public TreeNode {
  std::string left;
  std::string right;
  std::vector<std::shared_ptr<BinaryExpr>> conds;
  JoinType type;

  JoinExpr(std::string left_, std::string right_,
           std::vector<std::shared_ptr<BinaryExpr>> conds_, JoinType type_)
      : left(std::move(left_)),
        right(std::move(right_)),
        conds(std::move(conds_)),
        type(type_) {}
};

// 表引用：原始表名 + 可选别名（无别名时 alias 为空字符串）
struct TableRef : public TreeNode {
  std::string tab_name;
  std::string alias;
  // 派生表：若非空，则该 ref 实际上是括号包裹的 UNION 子查询
  std::shared_ptr<struct UnionStmt> derived;

  TableRef(std::string tab_name_, std::string alias_)
      : tab_name(std::move(tab_name_)), alias(std::move(alias_)) {}
};

// FROM 子句中转结构：携带表引用列表和 JOIN...ON 条件
struct FromClause {
  std::vector<std::shared_ptr<TableRef>> tabs;
  std::vector<std::shared_ptr<BinaryExpr>> on_conds;
};

struct SelectStmt : public TreeNode {
  std::vector<std::shared_ptr<Col>> cols;
  std::vector<std::string> tabs;
  std::vector<std::string> aliases;
  // 与 tabs/aliases 平行：若该位置是派生 UNION 表，此处保存其 AST；否则为空
  std::vector<std::shared_ptr<UnionStmt>> derived_tabs;
  std::vector<std::shared_ptr<BinaryExpr>> conds;
  std::vector<std::shared_ptr<BinaryExpr>> join_conds;
  std::vector<std::shared_ptr<JoinExpr>> jointree;

  // GROUP BY 列；空表示无分组
  std::vector<std::shared_ptr<Col>> group_by_cols;
  // HAVING 条件；左值允许是聚合 Col（lhs->agg_type != SV_AGG_NONE）
  std::vector<std::shared_ptr<BinaryExpr>> having_conds;
  // LIMIT N，< 0 表示无 LIMIT
  int limit = -1;

  bool has_sort;
  std::shared_ptr<OrderBy> order;

  SelectStmt(std::vector<std::shared_ptr<Col>> cols_,
             std::vector<std::shared_ptr<TableRef>> tab_refs_,
             std::vector<std::shared_ptr<BinaryExpr>> join_conds_,
             std::vector<std::shared_ptr<BinaryExpr>> conds_,
             std::shared_ptr<OrderBy> order_)
      : cols(std::move(cols_)),
        conds(std::move(conds_)),
        join_conds(std::move(join_conds_)),
        order(std::move(order_)) {
    has_sort = (bool)order;
    for (auto& ref : tab_refs_) {
      tabs.push_back(ref->tab_name);
      aliases.push_back(ref->alias);
      derived_tabs.push_back(ref->derived);
    }
  }
};

// EXPLAIN ANALYZE <select>; 仅包装 SELECT，要求执行后输出计划树和运行时统计
struct ExplainStmt : public TreeNode {
  std::shared_ptr<SelectStmt> select;

  ExplainStmt(std::shared_ptr<SelectStmt> select_)
      : select(std::move(select_)) {}
};

// 派生 UNION 子查询：作为 tableRef 出现时的 '(' SELECT UNION SELECT [...] ')'
// 仅承载分支列表，外层 SELECT 的 alias / ORDER BY 由 TableRef / SelectStmt 持有
struct UnionStmt : public TreeNode {
  std::vector<std::shared_ptr<SelectStmt>> branches;
};

// set enable_nestloop
struct SetStmt : public TreeNode {
  SetKnobType set_knob_type_;
  bool bool_val_;

  SetStmt(SetKnobType& type, bool bool_value)
      : set_knob_type_(type), bool_val_(bool_value) {}
};

// Semantic value
struct SemValue {
  int sv_int;
  float sv_float;
  std::string sv_str;
  bool sv_bool;
  OrderByDir sv_orderby_dir;
  std::vector<std::string> sv_strs;

  std::shared_ptr<TreeNode> sv_node;

  SvCompOp sv_comp_op;

  std::shared_ptr<TypeLen> sv_type_len;

  std::shared_ptr<Field> sv_field;
  std::vector<std::shared_ptr<Field>> sv_fields;

  std::shared_ptr<Expr> sv_expr;

  std::shared_ptr<Value> sv_val;
  std::vector<std::shared_ptr<Value>> sv_vals;

  std::shared_ptr<Col> sv_col;
  std::vector<std::shared_ptr<Col>> sv_cols;

  std::shared_ptr<SetClause> sv_set_clause;
  std::vector<std::shared_ptr<SetClause>> sv_set_clauses;

  std::shared_ptr<BinaryExpr> sv_cond;
  std::vector<std::shared_ptr<BinaryExpr>> sv_conds;

  std::shared_ptr<OrderBy> sv_orderby;
  std::shared_ptr<OrderByItem> sv_orderby_item;
  std::vector<std::shared_ptr<OrderByItem>> sv_orderby_items;

  SetKnobType sv_setKnobType;

  std::shared_ptr<TableRef> sv_tab_ref;
  std::vector<std::shared_ptr<TableRef>> sv_tab_refs;
  std::shared_ptr<FromClause> sv_from;
  std::shared_ptr<UnionStmt> sv_union;
  std::shared_ptr<SelectStmt> sv_select;
};

// 注意：解析结果已不再使用全局变量，改由 yyparse(scanner, &result) 的输出参数返回，
// 以支持多连接并发解析(去除 rmdb.cpp 的 buffer_mutex)。

}  // namespace ast

#define YYSTYPE ast::SemValue
