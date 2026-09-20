    /* keywords are case insensitive */
%option caseless
    /* we don't need yywrap() function */
%option noyywrap
    /* we don't need yyunput() function */
%option nounput
    /* we don't need input() function */
%option noinput
    /* enable location */
%option bison-bridge
%option bison-locations
    /* 可重入扫描器：每次解析持有独立 yyscan_t，无任何全局词法状态，
       从而去掉 rmdb.cpp 中包裹整段 scan+parse+analyze 的 buffer_mutex，
       使多连接灌库的解析+语义分析可并发(原全局 flex 缓冲是串行化根因)。 */
%option reentrant

%{
#include "ast.h"
#include "yacc.tab.h"
#include <iostream>

// automatically update location
#define YY_USER_ACTION \
    yylloc->first_line = yylloc->last_line; \
    yylloc->first_column = yylloc->last_column; \
    for (int i = 0; yytext[i] != '\0'; i++) { \
        if(yytext[i] == '\n') { \
            yylloc->last_line++; \
            yylloc->last_column = 1; \
        } else { \
            yylloc->last_column++; \
        } \
    }

%}

alpha [a-zA-Z]
digit [0-9]
white_space [ \t]+
new_line "\r"|"\n"|"\r\n"
sign "+"|"-"
identifier {alpha}(_|{alpha}|{digit})*
value_int {sign}?{digit}+
value_float {sign}?{digit}+\.({digit}+)?
value_string '[^']*'
parameter "\$"{digit}+
single_op ";"|"("|")"|","|"*"|"="|">"|"<"|"."|"+"|"-"

%x STATE_COMMENT
    /* 读 load 命令的未引号文件路径(含 / . - 等普通规则无法识别的字符) */
%x STATE_LOAD_PATH

%%
    /* block comment */
"/*" { BEGIN(STATE_COMMENT); }
<STATE_COMMENT>"*/" { BEGIN(INITIAL); }
<STATE_COMMENT>[^*] { /* ignore the text of the comment */ }
<STATE_COMMENT>\* { /* ignore *'s that aren't part of */ }
    /* load 文件路径:跳过前导空白,捕获到下一个空白/分号为止,然后回 INITIAL */
<STATE_LOAD_PATH>[ \t]+ { /* skip leading whitespace before path */ }
<STATE_LOAD_PATH>[^ \t\r\n;]+ {
    yylval->sv_str = std::string(yytext);
    BEGIN(INITIAL);
    return FILEPATH;
}
    /* single line comment */
"--".* { /* ignore single line comment */ }
    /* white space and new line */
{white_space} { /* ignore white space */ }
{new_line} { /* ignore new line */ }
    /* keywords */
"SHOW" { return SHOW; }
"BEGIN" { return TXN_BEGIN; }
"COMMIT" { return TXN_COMMIT; }
"ABORT" { return TXN_ABORT; }
"ROLLBACK" { return TXN_ROLLBACK; }
"TABLES" { return TABLES; }
"CREATE" { return CREATE; }
"TABLE" { return TABLE; }
"DROP" { return DROP; }
"DESC" { return DESC; }
"INSERT" { return INSERT; }
"LOAD" { BEGIN(STATE_LOAD_PATH); return LOAD; }
"INTO" { return INTO; }
"VALUES" { return VALUES; }
"DELETE" { return DELETE; }
"FROM" { return FROM; }
"WHERE" { return WHERE; }
"UPDATE" { return UPDATE; }
"SET" { return SET; }
"SELECT" { return SELECT; }
"INT" { return INT; }
"CHAR" { return CHAR; }
"FLOAT" { return FLOAT; }
"DATETIME" { return DATETIME; }
"INDEX" { return INDEX; }
"AND" { return AND; }
"JOIN" {return JOIN;}
"EXIT" { return EXIT; }
"HELP" { return HELP; }
"ORDER" { return ORDER; }
"BY" {  return BY;  }
"ASC" { return ASC; }
"AS" { return AS; }
"ON" { return ON; }
"EXPLAIN" { return EXPLAIN; }
"ANALYZE" { return ANALYZE; }
"ENABLE_NESTLOOP" { return ENABLE_NESTLOOP; }
"ENABLE_SORTMERGE" { return ENABLE_SORTMERGE; }
"TRANSACTION" { return TRANSACTION; }
"ISOLATION" { return ISOLATION; }
"LEVEL" { return LEVEL; }
"SNAPSHOT" { return SNAPSHOT; }
"SERIALIZABLE" { return SERIALIZABLE; }
"GROUP" { return GROUP; }
"HAVING" { return HAVING; }
"LIMIT" { return LIMIT; }
"UNION" { return UNION; }
"COUNT" { return COUNT; }
"DISTINCT" { return DISTINCT; }
"MAX" { return MAX_TOK; }
"MIN" { return MIN_TOK; }
"SUM" { return SUM_TOK; }
"AVG" { return AVG_TOK; }
"TRUE" { 
    yylval->sv_bool = true;
    return VALUE_BOOL; 
}
"FALSE" {
    yylval->sv_bool = false;
    return VALUE_BOOL;
}
    /* operators */
">=" { return GEQ; }
"<=" { return LEQ; }
"<>" { return NEQ; }
"!=" { return NEQ; }
{single_op} { return yytext[0]; }
    /* Keep marker digits as text so semantic analysis can reject overflow. */
{parameter} {
    yylval->sv_str = std::string(yytext + 1, yyleng - 1);
    return PARAMETER;
}
    /* identifiers */
{identifier} {
    yylval->sv_str = yytext;
    return IDENTIFIER;
}
    /* literals */
{value_int} {
    yylval->sv_int = atoi(yytext);
    yylval->sv_str = yytext;
    return VALUE_INT;
}
{value_float} {
    yylval->sv_float = atof(yytext);
    yylval->sv_str = yytext;
    return VALUE_FLOAT;
}
{value_string} {
    yylval->sv_str = std::string(yytext + 1, strlen(yytext) - 2);
    return VALUE_STRING;
}
    /* EOF */
<<EOF>> { return T_EOF; }
    /* Never discard an unknown character and continue parsing: doing so can
       silently change SQL semantics (for example, `!=` used to become `=`). */
. {
    std::cerr << "Lexer Error: unexpected character '" << yytext[0]
              << "' at line " << yylloc->last_line << std::endl;
    return YYUNDEF;
}
%%
