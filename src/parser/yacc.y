%{
#include "ast.h"
#include "yacc.tab.h"
#include <iostream>
#include <memory>

// 解析器值栈在进入 yyparse 时整体默认构造、退出时整体析构 YYSTYPE yyvsa[YYINITDEPTH]。
// YYSTYPE(=ast::SemValue) 含约 25 个非平凡成员(string/vector/shared_ptr)，-O0 下每个
// 成员的构造/析构都是真实函数调用，故该固定开销 ∝ YYINITDEPTH，占灌库解析 CPU 大头。
// 本文法所有列表均为左递归(valueList/colList/whereClause/fromClause…)，真实栈深是与
// 输入规模无关的固定上界；实测最深合法语句(7 路 join+子查询 union)栈深 ≈ 20。
// 取 64 留 ~3× 余量(溢出仅干净返回失败、无 UB)，把固定开销从默认 200 降到 64。
#ifndef YYINITDEPTH
#define YYINITDEPTH 64
#endif

// 可重入扫描器签名(flex %option reentrant + bison-bridge + bison-locations)
int yylex(YYSTYPE *yylval, YYLTYPE *yylloc, yyscan_t scanner);

// yyerror 形参表 = locp + 全部 parse-param(scanner, parse_result) + 消息
void yyerror(YYLTYPE *locp, yyscan_t scanner,
             std::shared_ptr<ast::TreeNode> *parse_result, const char* s) {
    std::cerr << "Parser Error at line " << locp->first_line << " column " << locp->first_column << ": " << s << std::endl;
}

using namespace ast;
%}

// 生成头(yacc.tab.h)中 yyparse/yyerror 的形参用到 yyscan_t 与 shared_ptr<TreeNode>，
// 需在头里先可见(与 flex 生成的 yyscan_t typedef 用同一守卫宏避免重复定义)。
%code requires {
#include <memory>
namespace ast { struct TreeNode; }
#ifndef YY_TYPEDEF_YY_SCANNER_T
#define YY_TYPEDEF_YY_SCANNER_T
typedef void *yyscan_t;
#endif
}

// request a pure (reentrant) parser
%define api.pure full
// scanner 同时传给 yylex 与 yyparse；parse_result 仅作 yyparse 的输出参数,
// 取代原全局 ast::parse_tree(全局可变状态会让并发解析竞争)。
%param {yyscan_t scanner}
%parse-param {std::shared_ptr<ast::TreeNode> *parse_result}
// enable location in error handler
%locations
// enable verbose syntax error message
%define parse.error verbose

// keywords
%token SHOW TABLES CREATE TABLE DROP DESC INSERT INTO VALUES DELETE FROM ASC ORDER BY
WHERE UPDATE SET SELECT INT CHAR FLOAT DATETIME INDEX AND JOIN ON AS EXPLAIN ANALYZE EXIT HELP TXN_BEGIN TXN_COMMIT TXN_ABORT TXN_ROLLBACK ORDER_BY ENABLE_NESTLOOP ENABLE_SORTMERGE
GROUP HAVING LIMIT COUNT DISTINCT MAX_TOK MIN_TOK SUM_TOK AVG_TOK UNION
TRANSACTION ISOLATION LEVEL SNAPSHOT SERIALIZABLE LOAD
// non-keywords
%token LEQ NEQ GEQ T_EOF

// type-specific tokens
%token <sv_str> IDENTIFIER VALUE_STRING FILEPATH PARAMETER
%token <sv_int> VALUE_INT
%token <sv_float> VALUE_FLOAT
%token <sv_bool> VALUE_BOOL

// specify types for non-terminal symbol
%type <sv_node> stmt dbStmt ddl dml txnStmt setStmt explainStmt selectStmt
%type <sv_field> field
%type <sv_fields> fieldList
%type <sv_type_len> type
%type <sv_comp_op> op
%type <sv_expr> expr
%type <sv_val> value
%type <sv_vals> valueList
%type <sv_str> tbName colName aliasOpt
%type <sv_strs> colNameList
%type <sv_col> col selectItem havingExpr
%type <sv_cols> colList selector selectList opt_group_by
%type <sv_set_clause> setClause arithmeticSetClause
%type <sv_set_clauses> setClauses
%type <sv_cond> condition
%type <sv_conds> whereClause optWhereClause onClauseOpt havingClause opt_having
%type <sv_orderby>  order_clause opt_order_clause
%type <sv_orderby_item> order_item
%type <sv_orderby_items> order_item_list
%type <sv_orderby_dir> opt_asc_desc
%type <sv_setKnobType> set_knob_type
%type <sv_tab_ref> tableRef
%type <sv_from> fromClause
%type <sv_union> unionExpr
%type <sv_int> opt_limit

%%
start:
        stmt ';'
    {
        *parse_result = $1;
        YYACCEPT;
    }
    |   HELP
    {
        *parse_result = std::make_shared<Help>();
        YYACCEPT;
    }
    |   EXIT
    {
        *parse_result = nullptr;
        YYACCEPT;
    }
    |   T_EOF
    {
        *parse_result = nullptr;
        YYACCEPT;
    }
    ;

stmt:
        dbStmt
    |   ddl
    |   dml
    |   txnStmt
    |   setStmt
    |   explainStmt
    ;

txnStmt:
        TXN_BEGIN
    {
        $$ = std::make_shared<TxnBegin>();
    }
    |   TXN_COMMIT
    {
        $$ = std::make_shared<TxnCommit>();
    }
    |   TXN_ABORT
    {
        $$ = std::make_shared<TxnAbort>();
    }
    | TXN_ROLLBACK
    {
        $$ = std::make_shared<TxnRollback>();
    }
    ;

dbStmt:
        SHOW TABLES
    {
        $$ = std::make_shared<ShowTables>();
    }
    |   SHOW INDEX FROM tbName
    {
        $$ = std::make_shared<ShowIndex>($4);
    }
    ;

setStmt:
        SET set_knob_type '=' VALUE_BOOL
    {
        $$ = std::make_shared<SetStmt>($2, $4);
    }
    |   SET TRANSACTION ISOLATION LEVEL SNAPSHOT ISOLATION
    {
        $$ = std::make_shared<SetIsolationStmt>(SV_ISO_SNAPSHOT_ISOLATION);
    }
    |   SET TRANSACTION ISOLATION LEVEL SERIALIZABLE
    {
        $$ = std::make_shared<SetIsolationStmt>(SV_ISO_SERIALIZABLE);
    }
    ;

ddl:
        CREATE TABLE tbName '(' fieldList ')'
    {
        $$ = std::make_shared<CreateTable>($3, $5);
    }
    |   DROP TABLE tbName
    {
        $$ = std::make_shared<DropTable>($3);
    }
    |   DESC tbName
    {
        $$ = std::make_shared<DescTable>($2);
    }
    |   CREATE INDEX tbName '(' colNameList ')'
    {
        $$ = std::make_shared<CreateIndex>($3, $5);
    }
    |   DROP INDEX tbName '(' colNameList ')'
    {
        $$ = std::make_shared<DropIndex>($3, $5);
    }
    ;

dml:
        INSERT INTO tbName VALUES '(' valueList ')'
    {
        $$ = std::make_shared<InsertStmt>($3, $6);
    }
    |   LOAD FILEPATH INTO tbName
    {
        $$ = std::make_shared<LoadStmt>($2, $4);
    }
    |   DELETE FROM tbName optWhereClause
    {
        $$ = std::make_shared<DeleteStmt>($3, $4);
    }
    |   UPDATE tbName SET setClauses optWhereClause
    {
        $$ = std::make_shared<UpdateStmt>($2, $4, $5);
    }
    |   selectStmt
    {
        $$ = $1;
    }
    ;

selectStmt:
        SELECT selector FROM fromClause optWhereClause opt_group_by opt_having opt_order_clause opt_limit
    {
        auto sel = std::make_shared<SelectStmt>($2, $4->tabs, std::move($4->on_conds), $5, $8);
        sel->group_by_cols = $6;
        sel->having_conds = $7;
        sel->limit = $9;
        $$ = sel;
    }
    ;

explainStmt:
        EXPLAIN ANALYZE selectStmt
    {
        auto sel = std::dynamic_pointer_cast<SelectStmt>($3);
        $$ = std::make_shared<ExplainStmt>(sel);
    }
    ;

tableRef:
        tbName aliasOpt
    {
        $$ = std::make_shared<TableRef>($1, $2);
    }
    |   '(' unionExpr ')' aliasOpt
    {
        // 派生 UNION 表：用空 tab_name 作为占位，alias 走正常路径
        $$ = std::make_shared<TableRef>(std::string(), $4);
        $$->derived = $2;
    }
    ;

unionExpr:
        selectStmt UNION selectStmt
    {
        auto u = std::make_shared<UnionStmt>();
        auto s1 = std::dynamic_pointer_cast<SelectStmt>($1);
        auto s2 = std::dynamic_pointer_cast<SelectStmt>($3);
        u->branches.push_back(s1);
        u->branches.push_back(s2);
        $$ = u;
    }
    |   unionExpr UNION selectStmt
    {
        auto s = std::dynamic_pointer_cast<SelectStmt>($3);
        $1->branches.push_back(s);
        $$ = $1;
    }
    ;

aliasOpt:
        /* epsilon */ { $$ = std::string(); }
    |   IDENTIFIER { $$ = $1; }
    |   AS IDENTIFIER { $$ = $2; }
    ;

fromClause:
        tableRef
    {
        $$ = std::make_shared<FromClause>();
        $$->tabs.push_back($1);
    }
    |   fromClause ',' tableRef
    {
        $$ = $1;
        $$->tabs.push_back($3);
    }
    |   fromClause JOIN tableRef onClauseOpt
    {
        $$ = $1;
        $$->tabs.push_back($3);
        for (auto& c : $4) {
            $$->on_conds.push_back(c);
        }
    }
    ;

onClauseOpt:
        /* epsilon */ { /* ignore */ }
    |   ON whereClause
    {
        $$ = $2;
    }
    ;

fieldList:
        field
    {
        $$ = std::vector<std::shared_ptr<Field>>{$1};
    }
    |   fieldList ',' field
    {
        $$.push_back($3);
    }
    ;

colNameList:
        colName
    {
        $$ = std::vector<std::string>{$1};
    }
    | colNameList ',' colName
    {
        $$.push_back($3);
    }
    ;

field:
        colName type
    {
        $$ = std::make_shared<ColDef>($1, $2);
    }
    ;

type:
        INT
    {
        $$ = std::make_shared<TypeLen>(SV_TYPE_INT, sizeof(int));
    }
    |   CHAR '(' VALUE_INT ')'
    {
        $$ = std::make_shared<TypeLen>(SV_TYPE_STRING, $3);
    }
    |   FLOAT
    {
        $$ = std::make_shared<TypeLen>(SV_TYPE_FLOAT, sizeof(float));
    }
    |   DATETIME
    {
        $$ = std::make_shared<TypeLen>(SV_TYPE_STRING, 20);
    }
    ;

valueList:
        value
    {
        $$ = std::vector<std::shared_ptr<Value>>{$1};
    }
    |   valueList ',' value
    {
        $$.push_back($3);
    }
    ;

value:
        VALUE_INT
    {
        $$ = std::make_shared<IntLit>($1, std::to_string($1));
    }
    |   VALUE_FLOAT
    {
        $$ = std::make_shared<FloatLit>($1, $<sv_str>1);
    }
    |   VALUE_STRING
    {
        $$ = std::make_shared<StringLit>($1, std::string("'") + $1 + "'");
    }
    |   VALUE_BOOL
    {
        $$ = std::make_shared<BoolLit>($1);
    }
    |   PARAMETER
    {
        $$ = std::make_shared<ParamRef>($1);
    }
    ;

condition:
        havingExpr op expr
    {
        $$ = std::make_shared<BinaryExpr>($1, $2, $3);
    }
    ;

optWhereClause:
        /* epsilon */ { /* ignore*/ }
    |   WHERE whereClause
    {
        $$ = $2;
    }
    ;

whereClause:
        condition
    {
        $$ = std::vector<std::shared_ptr<BinaryExpr>>{$1};
    }
    |   whereClause AND condition
    {
        $$.push_back($3);
    }
    ;

col:
        tbName '.' colName
    {
        $$ = std::make_shared<Col>($1, $3);
    }
    |   colName
    {
        $$ = std::make_shared<Col>("", $1);
    }
    ;

colList:
        col
    {
        $$ = std::vector<std::shared_ptr<Col>>{$1};
    }
    |   colList ',' col
    {
        $$.push_back($3);
    }
    ;

op:
        '='
    {
        $$ = SV_OP_EQ;
    }
    |   '<'
    {
        $$ = SV_OP_LT;
    }
    |   '>'
    {
        $$ = SV_OP_GT;
    }
    |   NEQ
    {
        $$ = SV_OP_NE;
    }
    |   LEQ
    {
        $$ = SV_OP_LE;
    }
    |   GEQ
    {
        $$ = SV_OP_GE;
    }
    ;

expr:
        value
    {
        $$ = std::static_pointer_cast<Expr>($1);
    }
    |   col
    {
        $$ = std::static_pointer_cast<Expr>($1);
    }
    ;

setClauses:
        setClause
    {
        $$ = std::vector<std::shared_ptr<SetClause>>{$1};
    }
    |   setClauses ',' setClause
    {
        $$.push_back($3);
    }
    ;

setClause:
        colName '=' value
    {
        $$ = std::make_shared<SetClause>($1, $3);
    }
    |   colName '=' colName
    {
        $$ = std::make_shared<SetClause>($1, $3);
    }
    |   arithmeticSetClause
    {
        $$ = $1;
    }
    ;

arithmeticSetClause:
        colName '=' colName '+' value
    {
        $$ = std::make_shared<SetClause>($1, $3, '+', $5);
    }
    |   colName '=' colName '-' value
    {
        $$ = std::make_shared<SetClause>($1, $3, '-', $5);
    }
    |   colName '=' colName '*' value
    {
        $$ = std::make_shared<SetClause>($1, $3, '*', $5);
    }
    |   colName '=' colName VALUE_INT
    {
        // 词法把紧贴的 +N/-N 贪婪切成带符号整型字面量（{sign}?{digit}+），
        // 导致 set col=col2+1 / col=col2-1 走不到上面的 '+'/'-' 规则而报
        // syntax error。把带符号字面量落地为
        // col = col2 + (±N)，语义等价。
        if ($<sv_str>4.empty() ||
            ($<sv_str>4.front() != '+' && $<sv_str>4.front() != '-')) {
            yyerror(&@4, scanner, parse_result,
                    "missing arithmetic operator before integer literal");
            YYERROR;
        }
        $$ = std::make_shared<SetClause>($1, $3, '+',
                 std::make_shared<IntLit>($4, $<sv_str>4));
    }
    |   colName '=' colName VALUE_FLOAT
    {
        if ($<sv_str>4.empty() ||
            ($<sv_str>4.front() != '+' && $<sv_str>4.front() != '-')) {
            yyerror(&@4, scanner, parse_result,
                    "missing arithmetic operator before float literal");
            YYERROR;
        }
        $$ = std::make_shared<SetClause>($1, $3, '+',
                 std::make_shared<FloatLit>($4, $<sv_str>4));
    }
    |   arithmeticSetClause '+' value
    {
        $$ = $1;
        $$->append_term('+', $3);
    }
    |   arithmeticSetClause '-' value
    {
        $$ = $1;
        $$->append_term('-', $3);
    }
    |   arithmeticSetClause VALUE_INT
    {
        // 无空格的后续 +N/-N 同样会被词法器合并成带符号字面量。
        // 统一记录成“加上带符号值”，与显式 '+'/'-' 规则等价。
        if ($<sv_str>2.empty() ||
            ($<sv_str>2.front() != '+' && $<sv_str>2.front() != '-')) {
            yyerror(&@2, scanner, parse_result,
                    "missing arithmetic operator before integer literal");
            YYERROR;
        }
        $$ = $1;
        $$->append_term('+',
            std::make_shared<IntLit>($2, $<sv_str>2));
    }
    |   arithmeticSetClause VALUE_FLOAT
    {
        if ($<sv_str>2.empty() ||
            ($<sv_str>2.front() != '+' && $<sv_str>2.front() != '-')) {
            yyerror(&@2, scanner, parse_result,
                    "missing arithmetic operator before float literal");
            YYERROR;
        }
        $$ = $1;
        $$->append_term('+',
            std::make_shared<FloatLit>($2, $<sv_str>2));
    }
    ;

selector:
        '*'
    {
        $$ = {};
    }
    |   selectList
    ;

selectList:
        selectItem
    {
        $$ = std::vector<std::shared_ptr<Col>>{$1};
    }
    |   selectList ',' selectItem
    {
        $$.push_back($3);
    }
    ;

selectItem:
        col aliasOpt
    {
        $1->alias = $2;
        $$ = $1;
    }
    |   COUNT '(' '*' ')' aliasOpt
    {
        auto c = std::make_shared<Col>(std::string(), std::string("*"));
        c->agg_type = SV_AGG_COUNT;
        c->is_star = true;
        c->alias = $5;
        $$ = c;
    }
    |   COUNT '(' col ')' aliasOpt
    {
        $3->agg_type = SV_AGG_COUNT;
        $3->alias = $5;
        $$ = $3;
    }
    |   COUNT '(' DISTINCT col ')' aliasOpt
    {
        $4->agg_type = SV_AGG_COUNT;
        $4->is_distinct = true;
        $4->alias = $6;
        $$ = $4;
    }
    |   COUNT '(' DISTINCT '(' col ')' ')' aliasOpt
    {
        $5->agg_type = SV_AGG_COUNT;
        $5->is_distinct = true;
        $5->alias = $8;
        $$ = $5;
    }
    |   MAX_TOK '(' col ')' aliasOpt
    {
        $3->agg_type = SV_AGG_MAX;
        $3->alias = $5;
        $$ = $3;
    }
    |   MIN_TOK '(' col ')' aliasOpt
    {
        $3->agg_type = SV_AGG_MIN;
        $3->alias = $5;
        $$ = $3;
    }
    |   SUM_TOK '(' col ')' aliasOpt
    {
        $3->agg_type = SV_AGG_SUM;
        $3->alias = $5;
        $$ = $3;
    }
    |   AVG_TOK '(' col ')' aliasOpt
    {
        $3->agg_type = SV_AGG_AVG;
        $3->alias = $5;
        $$ = $3;
    }
    ;

opt_group_by:
        /* epsilon */ { $$ = std::vector<std::shared_ptr<Col>>(); }
    |   GROUP BY colList { $$ = $3; }
    ;

opt_having:
        /* epsilon */ { $$ = std::vector<std::shared_ptr<BinaryExpr>>(); }
    |   HAVING havingClause { $$ = $2; }
    ;

havingClause:
        condition
    {
        $$ = std::vector<std::shared_ptr<BinaryExpr>>{$1};
    }
    |   havingClause AND condition
    {
        $$.push_back($3);
    }
    ;

havingExpr:
        col { $$ = $1; }
    |   COUNT '(' '*' ')'
    {
        auto c = std::make_shared<Col>(std::string(), std::string("*"));
        c->agg_type = SV_AGG_COUNT;
        c->is_star = true;
        $$ = c;
    }
    |   COUNT '(' col ')'
    {
        $3->agg_type = SV_AGG_COUNT;
        $$ = $3;
    }
    |   COUNT '(' DISTINCT col ')'
    {
        $4->agg_type = SV_AGG_COUNT;
        $4->is_distinct = true;
        $$ = $4;
    }
    |   COUNT '(' DISTINCT '(' col ')' ')'
    {
        $5->agg_type = SV_AGG_COUNT;
        $5->is_distinct = true;
        $$ = $5;
    }
    |   MAX_TOK '(' col ')'
    {
        $3->agg_type = SV_AGG_MAX;
        $$ = $3;
    }
    |   MIN_TOK '(' col ')'
    {
        $3->agg_type = SV_AGG_MIN;
        $$ = $3;
    }
    |   SUM_TOK '(' col ')'
    {
        $3->agg_type = SV_AGG_SUM;
        $$ = $3;
    }
    |   AVG_TOK '(' col ')'
    {
        $3->agg_type = SV_AGG_AVG;
        $$ = $3;
    }
    ;

opt_limit:
        /* epsilon */ { $$ = -1; }
    |   LIMIT VALUE_INT { $$ = $2; }
    ;

opt_order_clause:
    ORDER BY order_clause
    {
        $$ = $3;
    }
    |   /* epsilon */ { /* ignore*/ }
    ;

order_clause:
        order_item_list
    {
        auto ob = std::make_shared<OrderBy>();
        ob->items = $1;
        $$ = ob;
    }
    ;

order_item_list:
        order_item
    {
        $$ = std::vector<std::shared_ptr<OrderByItem>>{$1};
    }
    |   order_item_list ',' order_item
    {
        $$.push_back($3);
    }
    ;

order_item:
        col opt_asc_desc
    {
        $$ = std::make_shared<OrderByItem>($1, $2);
    }
    |   COUNT '(' '*' ')' opt_asc_desc
    {
        auto c = std::make_shared<Col>(std::string(), std::string("*"));
        c->agg_type = SV_AGG_COUNT;
        c->is_star = true;
        $$ = std::make_shared<OrderByItem>(c, $5);
    }
    |   COUNT '(' col ')' opt_asc_desc
    {
        $3->agg_type = SV_AGG_COUNT;
        $$ = std::make_shared<OrderByItem>($3, $5);
    }
    |   COUNT '(' DISTINCT col ')' opt_asc_desc
    {
        $4->agg_type = SV_AGG_COUNT;
        $4->is_distinct = true;
        $$ = std::make_shared<OrderByItem>($4, $6);
    }
    |   COUNT '(' DISTINCT '(' col ')' ')' opt_asc_desc
    {
        $5->agg_type = SV_AGG_COUNT;
        $5->is_distinct = true;
        $$ = std::make_shared<OrderByItem>($5, $8);
    }
    |   MAX_TOK '(' col ')' opt_asc_desc
    {
        $3->agg_type = SV_AGG_MAX;
        $$ = std::make_shared<OrderByItem>($3, $5);
    }
    |   MIN_TOK '(' col ')' opt_asc_desc
    {
        $3->agg_type = SV_AGG_MIN;
        $$ = std::make_shared<OrderByItem>($3, $5);
    }
    |   SUM_TOK '(' col ')' opt_asc_desc
    {
        $3->agg_type = SV_AGG_SUM;
        $$ = std::make_shared<OrderByItem>($3, $5);
    }
    |   AVG_TOK '(' col ')' opt_asc_desc
    {
        $3->agg_type = SV_AGG_AVG;
        $$ = std::make_shared<OrderByItem>($3, $5);
    }
    ;

opt_asc_desc:
    ASC          { $$ = OrderBy_ASC;     }
    |  DESC      { $$ = OrderBy_DESC;    }
    |       { $$ = OrderBy_DEFAULT; }
    ;

set_knob_type:
    ENABLE_NESTLOOP { $$ = EnableNestLoop; }
    |   ENABLE_SORTMERGE { $$ = EnableSortMerge; }
    ;

tbName: IDENTIFIER;

colName: IDENTIFIER;
%%
