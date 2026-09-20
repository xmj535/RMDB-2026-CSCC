/* A Bison parser, made by GNU Bison 3.8.2.  */

/* Bison implementation for Yacc-like parsers in C

   Copyright (C) 1984, 1989-1990, 2000-2015, 2018-2021 Free Software Foundation,
   Inc.

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <https://www.gnu.org/licenses/>.  */

/* As a special exception, you may create a larger work that contains
   part or all of the Bison parser skeleton and distribute that work
   under terms of your choice, so long as that work isn't itself a
   parser generator using the skeleton or a modified version thereof
   as a parser skeleton.  Alternatively, if you modify or redistribute
   the parser skeleton itself, you may (at your option) remove this
   special exception, which will cause the skeleton and the resulting
   Bison output files to be licensed under the GNU General Public
   License without this special exception.

   This special exception was added by the Free Software Foundation in
   version 2.2 of Bison.  */

/* C LALR(1) parser skeleton written by Richard Stallman, by
   simplifying the original so-called "semantic" parser.  */

/* DO NOT RELY ON FEATURES THAT ARE NOT DOCUMENTED in the manual,
   especially those whose name start with YY_ or yy_.  They are
   private implementation details that can be changed or removed.  */

/* All symbols defined below should begin with yy or YY, to avoid
   infringing on user name space.  This should be done even for local
   variables, as they might otherwise be expanded by user macros.
   There are some unavoidable exceptions within include files to
   define necessary library symbols; they are noted "INFRINGES ON
   USER NAME SPACE" below.  */

/* Identify Bison output, and Bison version.  */
#define YYBISON 30802

/* Bison version string.  */
#define YYBISON_VERSION "3.8.2"

/* Skeleton name.  */
#define YYSKELETON_NAME "yacc.c"

/* Pure parsers.  */
#define YYPURE 2

/* Push parsers.  */
#define YYPUSH 0

/* Pull parsers.  */
#define YYPULL 1




/* First part of user prologue.  */
#line 1 "src/parser/yacc.y"

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

#line 99 "src/parser/yacc.tab.cpp"

# ifndef YY_CAST
#  ifdef __cplusplus
#   define YY_CAST(Type, Val) static_cast<Type> (Val)
#   define YY_REINTERPRET_CAST(Type, Val) reinterpret_cast<Type> (Val)
#  else
#   define YY_CAST(Type, Val) ((Type) (Val))
#   define YY_REINTERPRET_CAST(Type, Val) ((Type) (Val))
#  endif
# endif
# ifndef YY_NULLPTR
#  if defined __cplusplus
#   if 201103L <= __cplusplus
#    define YY_NULLPTR nullptr
#   else
#    define YY_NULLPTR 0
#   endif
#  else
#   define YY_NULLPTR ((void*)0)
#  endif
# endif

#include "yacc.tab.h"
/* Symbol kind.  */
enum yysymbol_kind_t
{
  YYSYMBOL_YYEMPTY = -2,
  YYSYMBOL_YYEOF = 0,                      /* "end of file"  */
  YYSYMBOL_YYerror = 1,                    /* error  */
  YYSYMBOL_YYUNDEF = 2,                    /* "invalid token"  */
  YYSYMBOL_SHOW = 3,                       /* SHOW  */
  YYSYMBOL_TABLES = 4,                     /* TABLES  */
  YYSYMBOL_CREATE = 5,                     /* CREATE  */
  YYSYMBOL_TABLE = 6,                      /* TABLE  */
  YYSYMBOL_DROP = 7,                       /* DROP  */
  YYSYMBOL_DESC = 8,                       /* DESC  */
  YYSYMBOL_INSERT = 9,                     /* INSERT  */
  YYSYMBOL_INTO = 10,                      /* INTO  */
  YYSYMBOL_VALUES = 11,                    /* VALUES  */
  YYSYMBOL_DELETE = 12,                    /* DELETE  */
  YYSYMBOL_FROM = 13,                      /* FROM  */
  YYSYMBOL_ASC = 14,                       /* ASC  */
  YYSYMBOL_ORDER = 15,                     /* ORDER  */
  YYSYMBOL_BY = 16,                        /* BY  */
  YYSYMBOL_WHERE = 17,                     /* WHERE  */
  YYSYMBOL_UPDATE = 18,                    /* UPDATE  */
  YYSYMBOL_SET = 19,                       /* SET  */
  YYSYMBOL_SELECT = 20,                    /* SELECT  */
  YYSYMBOL_INT = 21,                       /* INT  */
  YYSYMBOL_CHAR = 22,                      /* CHAR  */
  YYSYMBOL_FLOAT = 23,                     /* FLOAT  */
  YYSYMBOL_DATETIME = 24,                  /* DATETIME  */
  YYSYMBOL_INDEX = 25,                     /* INDEX  */
  YYSYMBOL_AND = 26,                       /* AND  */
  YYSYMBOL_JOIN = 27,                      /* JOIN  */
  YYSYMBOL_ON = 28,                        /* ON  */
  YYSYMBOL_AS = 29,                        /* AS  */
  YYSYMBOL_EXPLAIN = 30,                   /* EXPLAIN  */
  YYSYMBOL_ANALYZE = 31,                   /* ANALYZE  */
  YYSYMBOL_EXIT = 32,                      /* EXIT  */
  YYSYMBOL_HELP = 33,                      /* HELP  */
  YYSYMBOL_TXN_BEGIN = 34,                 /* TXN_BEGIN  */
  YYSYMBOL_TXN_COMMIT = 35,                /* TXN_COMMIT  */
  YYSYMBOL_TXN_ABORT = 36,                 /* TXN_ABORT  */
  YYSYMBOL_TXN_ROLLBACK = 37,              /* TXN_ROLLBACK  */
  YYSYMBOL_ORDER_BY = 38,                  /* ORDER_BY  */
  YYSYMBOL_ENABLE_NESTLOOP = 39,           /* ENABLE_NESTLOOP  */
  YYSYMBOL_ENABLE_SORTMERGE = 40,          /* ENABLE_SORTMERGE  */
  YYSYMBOL_GROUP = 41,                     /* GROUP  */
  YYSYMBOL_HAVING = 42,                    /* HAVING  */
  YYSYMBOL_LIMIT = 43,                     /* LIMIT  */
  YYSYMBOL_COUNT = 44,                     /* COUNT  */
  YYSYMBOL_DISTINCT = 45,                  /* DISTINCT  */
  YYSYMBOL_MAX_TOK = 46,                   /* MAX_TOK  */
  YYSYMBOL_MIN_TOK = 47,                   /* MIN_TOK  */
  YYSYMBOL_SUM_TOK = 48,                   /* SUM_TOK  */
  YYSYMBOL_AVG_TOK = 49,                   /* AVG_TOK  */
  YYSYMBOL_UNION = 50,                     /* UNION  */
  YYSYMBOL_TRANSACTION = 51,               /* TRANSACTION  */
  YYSYMBOL_ISOLATION = 52,                 /* ISOLATION  */
  YYSYMBOL_LEVEL = 53,                     /* LEVEL  */
  YYSYMBOL_SNAPSHOT = 54,                  /* SNAPSHOT  */
  YYSYMBOL_SERIALIZABLE = 55,              /* SERIALIZABLE  */
  YYSYMBOL_LOAD = 56,                      /* LOAD  */
  YYSYMBOL_LEQ = 57,                       /* LEQ  */
  YYSYMBOL_NEQ = 58,                       /* NEQ  */
  YYSYMBOL_GEQ = 59,                       /* GEQ  */
  YYSYMBOL_T_EOF = 60,                     /* T_EOF  */
  YYSYMBOL_IDENTIFIER = 61,                /* IDENTIFIER  */
  YYSYMBOL_VALUE_STRING = 62,              /* VALUE_STRING  */
  YYSYMBOL_FILEPATH = 63,                  /* FILEPATH  */
  YYSYMBOL_PARAMETER = 64,                 /* PARAMETER  */
  YYSYMBOL_VALUE_INT = 65,                 /* VALUE_INT  */
  YYSYMBOL_VALUE_FLOAT = 66,               /* VALUE_FLOAT  */
  YYSYMBOL_VALUE_BOOL = 67,                /* VALUE_BOOL  */
  YYSYMBOL_68_ = 68,                       /* ';'  */
  YYSYMBOL_69_ = 69,                       /* '='  */
  YYSYMBOL_70_ = 70,                       /* '('  */
  YYSYMBOL_71_ = 71,                       /* ')'  */
  YYSYMBOL_72_ = 72,                       /* ','  */
  YYSYMBOL_73_ = 73,                       /* '.'  */
  YYSYMBOL_74_ = 74,                       /* '<'  */
  YYSYMBOL_75_ = 75,                       /* '>'  */
  YYSYMBOL_76_ = 76,                       /* '+'  */
  YYSYMBOL_77_ = 77,                       /* '-'  */
  YYSYMBOL_78_ = 78,                       /* '*'  */
  YYSYMBOL_YYACCEPT = 79,                  /* $accept  */
  YYSYMBOL_start = 80,                     /* start  */
  YYSYMBOL_stmt = 81,                      /* stmt  */
  YYSYMBOL_txnStmt = 82,                   /* txnStmt  */
  YYSYMBOL_dbStmt = 83,                    /* dbStmt  */
  YYSYMBOL_setStmt = 84,                   /* setStmt  */
  YYSYMBOL_ddl = 85,                       /* ddl  */
  YYSYMBOL_dml = 86,                       /* dml  */
  YYSYMBOL_selectStmt = 87,                /* selectStmt  */
  YYSYMBOL_explainStmt = 88,               /* explainStmt  */
  YYSYMBOL_tableRef = 89,                  /* tableRef  */
  YYSYMBOL_unionExpr = 90,                 /* unionExpr  */
  YYSYMBOL_aliasOpt = 91,                  /* aliasOpt  */
  YYSYMBOL_fromClause = 92,                /* fromClause  */
  YYSYMBOL_onClauseOpt = 93,               /* onClauseOpt  */
  YYSYMBOL_fieldList = 94,                 /* fieldList  */
  YYSYMBOL_colNameList = 95,               /* colNameList  */
  YYSYMBOL_field = 96,                     /* field  */
  YYSYMBOL_type = 97,                      /* type  */
  YYSYMBOL_valueList = 98,                 /* valueList  */
  YYSYMBOL_value = 99,                     /* value  */
  YYSYMBOL_condition = 100,                /* condition  */
  YYSYMBOL_optWhereClause = 101,           /* optWhereClause  */
  YYSYMBOL_whereClause = 102,              /* whereClause  */
  YYSYMBOL_col = 103,                      /* col  */
  YYSYMBOL_colList = 104,                  /* colList  */
  YYSYMBOL_op = 105,                       /* op  */
  YYSYMBOL_expr = 106,                     /* expr  */
  YYSYMBOL_setClauses = 107,               /* setClauses  */
  YYSYMBOL_setClause = 108,                /* setClause  */
  YYSYMBOL_arithmeticSetClause = 109,      /* arithmeticSetClause  */
  YYSYMBOL_selector = 110,                 /* selector  */
  YYSYMBOL_selectList = 111,               /* selectList  */
  YYSYMBOL_selectItem = 112,               /* selectItem  */
  YYSYMBOL_opt_group_by = 113,             /* opt_group_by  */
  YYSYMBOL_opt_having = 114,               /* opt_having  */
  YYSYMBOL_havingClause = 115,             /* havingClause  */
  YYSYMBOL_havingExpr = 116,               /* havingExpr  */
  YYSYMBOL_opt_limit = 117,                /* opt_limit  */
  YYSYMBOL_opt_order_clause = 118,         /* opt_order_clause  */
  YYSYMBOL_order_clause = 119,             /* order_clause  */
  YYSYMBOL_order_item_list = 120,          /* order_item_list  */
  YYSYMBOL_order_item = 121,               /* order_item  */
  YYSYMBOL_opt_asc_desc = 122,             /* opt_asc_desc  */
  YYSYMBOL_set_knob_type = 123,            /* set_knob_type  */
  YYSYMBOL_tbName = 124,                   /* tbName  */
  YYSYMBOL_colName = 125                   /* colName  */
};
typedef enum yysymbol_kind_t yysymbol_kind_t;




#ifdef short
# undef short
#endif

/* On compilers that do not define __PTRDIFF_MAX__ etc., make sure
   <limits.h> and (if available) <stdint.h> are included
   so that the code can choose integer types of a good width.  */

#ifndef __PTRDIFF_MAX__
# include <limits.h> /* INFRINGES ON USER NAME SPACE */
# if defined __STDC_VERSION__ && 199901 <= __STDC_VERSION__
#  include <stdint.h> /* INFRINGES ON USER NAME SPACE */
#  define YY_STDINT_H
# endif
#endif

/* Narrow types that promote to a signed type and that can represent a
   signed or unsigned integer of at least N bits.  In tables they can
   save space and decrease cache pressure.  Promoting to a signed type
   helps avoid bugs in integer arithmetic.  */

#ifdef __INT_LEAST8_MAX__
typedef __INT_LEAST8_TYPE__ yytype_int8;
#elif defined YY_STDINT_H
typedef int_least8_t yytype_int8;
#else
typedef signed char yytype_int8;
#endif

#ifdef __INT_LEAST16_MAX__
typedef __INT_LEAST16_TYPE__ yytype_int16;
#elif defined YY_STDINT_H
typedef int_least16_t yytype_int16;
#else
typedef short yytype_int16;
#endif

/* Work around bug in HP-UX 11.23, which defines these macros
   incorrectly for preprocessor constants.  This workaround can likely
   be removed in 2023, as HPE has promised support for HP-UX 11.23
   (aka HP-UX 11i v2) only through the end of 2022; see Table 2 of
   <https://h20195.www2.hpe.com/V2/getpdf.aspx/4AA4-7673ENW.pdf>.  */
#ifdef __hpux
# undef UINT_LEAST8_MAX
# undef UINT_LEAST16_MAX
# define UINT_LEAST8_MAX 255
# define UINT_LEAST16_MAX 65535
#endif

#if defined __UINT_LEAST8_MAX__ && __UINT_LEAST8_MAX__ <= __INT_MAX__
typedef __UINT_LEAST8_TYPE__ yytype_uint8;
#elif (!defined __UINT_LEAST8_MAX__ && defined YY_STDINT_H \
       && UINT_LEAST8_MAX <= INT_MAX)
typedef uint_least8_t yytype_uint8;
#elif !defined __UINT_LEAST8_MAX__ && UCHAR_MAX <= INT_MAX
typedef unsigned char yytype_uint8;
#else
typedef short yytype_uint8;
#endif

#if defined __UINT_LEAST16_MAX__ && __UINT_LEAST16_MAX__ <= __INT_MAX__
typedef __UINT_LEAST16_TYPE__ yytype_uint16;
#elif (!defined __UINT_LEAST16_MAX__ && defined YY_STDINT_H \
       && UINT_LEAST16_MAX <= INT_MAX)
typedef uint_least16_t yytype_uint16;
#elif !defined __UINT_LEAST16_MAX__ && USHRT_MAX <= INT_MAX
typedef unsigned short yytype_uint16;
#else
typedef int yytype_uint16;
#endif

#ifndef YYPTRDIFF_T
# if defined __PTRDIFF_TYPE__ && defined __PTRDIFF_MAX__
#  define YYPTRDIFF_T __PTRDIFF_TYPE__
#  define YYPTRDIFF_MAXIMUM __PTRDIFF_MAX__
# elif defined PTRDIFF_MAX
#  ifndef ptrdiff_t
#   include <stddef.h> /* INFRINGES ON USER NAME SPACE */
#  endif
#  define YYPTRDIFF_T ptrdiff_t
#  define YYPTRDIFF_MAXIMUM PTRDIFF_MAX
# else
#  define YYPTRDIFF_T long
#  define YYPTRDIFF_MAXIMUM LONG_MAX
# endif
#endif

#ifndef YYSIZE_T
# ifdef __SIZE_TYPE__
#  define YYSIZE_T __SIZE_TYPE__
# elif defined size_t
#  define YYSIZE_T size_t
# elif defined __STDC_VERSION__ && 199901 <= __STDC_VERSION__
#  include <stddef.h> /* INFRINGES ON USER NAME SPACE */
#  define YYSIZE_T size_t
# else
#  define YYSIZE_T unsigned
# endif
#endif

#define YYSIZE_MAXIMUM                                  \
  YY_CAST (YYPTRDIFF_T,                                 \
           (YYPTRDIFF_MAXIMUM < YY_CAST (YYSIZE_T, -1)  \
            ? YYPTRDIFF_MAXIMUM                         \
            : YY_CAST (YYSIZE_T, -1)))

#define YYSIZEOF(X) YY_CAST (YYPTRDIFF_T, sizeof (X))


/* Stored state numbers (used for stacks). */
typedef yytype_int16 yy_state_t;

/* State numbers in computations.  */
typedef int yy_state_fast_t;

#ifndef YY_
# if defined YYENABLE_NLS && YYENABLE_NLS
#  if ENABLE_NLS
#   include <libintl.h> /* INFRINGES ON USER NAME SPACE */
#   define YY_(Msgid) dgettext ("bison-runtime", Msgid)
#  endif
# endif
# ifndef YY_
#  define YY_(Msgid) Msgid
# endif
#endif


#ifndef YY_ATTRIBUTE_PURE
# if defined __GNUC__ && 2 < __GNUC__ + (96 <= __GNUC_MINOR__)
#  define YY_ATTRIBUTE_PURE __attribute__ ((__pure__))
# else
#  define YY_ATTRIBUTE_PURE
# endif
#endif

#ifndef YY_ATTRIBUTE_UNUSED
# if defined __GNUC__ && 2 < __GNUC__ + (7 <= __GNUC_MINOR__)
#  define YY_ATTRIBUTE_UNUSED __attribute__ ((__unused__))
# else
#  define YY_ATTRIBUTE_UNUSED
# endif
#endif

/* Suppress unused-variable warnings by "using" E.  */
#if ! defined lint || defined __GNUC__
# define YY_USE(E) ((void) (E))
#else
# define YY_USE(E) /* empty */
#endif

/* Suppress an incorrect diagnostic about yylval being uninitialized.  */
#if defined __GNUC__ && ! defined __ICC && 406 <= __GNUC__ * 100 + __GNUC_MINOR__
# if __GNUC__ * 100 + __GNUC_MINOR__ < 407
#  define YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN                           \
    _Pragma ("GCC diagnostic push")                                     \
    _Pragma ("GCC diagnostic ignored \"-Wuninitialized\"")
# else
#  define YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN                           \
    _Pragma ("GCC diagnostic push")                                     \
    _Pragma ("GCC diagnostic ignored \"-Wuninitialized\"")              \
    _Pragma ("GCC diagnostic ignored \"-Wmaybe-uninitialized\"")
# endif
# define YY_IGNORE_MAYBE_UNINITIALIZED_END      \
    _Pragma ("GCC diagnostic pop")
#else
# define YY_INITIAL_VALUE(Value) Value
#endif
#ifndef YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
# define YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
# define YY_IGNORE_MAYBE_UNINITIALIZED_END
#endif
#ifndef YY_INITIAL_VALUE
# define YY_INITIAL_VALUE(Value) /* Nothing. */
#endif

#if defined __cplusplus && defined __GNUC__ && ! defined __ICC && 6 <= __GNUC__
# define YY_IGNORE_USELESS_CAST_BEGIN                          \
    _Pragma ("GCC diagnostic push")                            \
    _Pragma ("GCC diagnostic ignored \"-Wuseless-cast\"")
# define YY_IGNORE_USELESS_CAST_END            \
    _Pragma ("GCC diagnostic pop")
#endif
#ifndef YY_IGNORE_USELESS_CAST_BEGIN
# define YY_IGNORE_USELESS_CAST_BEGIN
# define YY_IGNORE_USELESS_CAST_END
#endif


#define YY_ASSERT(E) ((void) (0 && (E)))

#if 1

/* The parser invokes alloca or malloc; define the necessary symbols.  */

# ifdef YYSTACK_USE_ALLOCA
#  if YYSTACK_USE_ALLOCA
#   ifdef __GNUC__
#    define YYSTACK_ALLOC __builtin_alloca
#   elif defined __BUILTIN_VA_ARG_INCR
#    include <alloca.h> /* INFRINGES ON USER NAME SPACE */
#   elif defined _AIX
#    define YYSTACK_ALLOC __alloca
#   elif defined _MSC_VER
#    include <malloc.h> /* INFRINGES ON USER NAME SPACE */
#    define alloca _alloca
#   else
#    define YYSTACK_ALLOC alloca
#    if ! defined _ALLOCA_H && ! defined EXIT_SUCCESS
#     include <stdlib.h> /* INFRINGES ON USER NAME SPACE */
      /* Use EXIT_SUCCESS as a witness for stdlib.h.  */
#     ifndef EXIT_SUCCESS
#      define EXIT_SUCCESS 0
#     endif
#    endif
#   endif
#  endif
# endif

# ifdef YYSTACK_ALLOC
   /* Pacify GCC's 'empty if-body' warning.  */
#  define YYSTACK_FREE(Ptr) do { /* empty */; } while (0)
#  ifndef YYSTACK_ALLOC_MAXIMUM
    /* The OS might guarantee only one guard page at the bottom of the stack,
       and a page size can be as small as 4096 bytes.  So we cannot safely
       invoke alloca (N) if N exceeds 4096.  Use a slightly smaller number
       to allow for a few compiler-allocated temporary stack slots.  */
#   define YYSTACK_ALLOC_MAXIMUM 4032 /* reasonable circa 2006 */
#  endif
# else
#  define YYSTACK_ALLOC YYMALLOC
#  define YYSTACK_FREE YYFREE
#  ifndef YYSTACK_ALLOC_MAXIMUM
#   define YYSTACK_ALLOC_MAXIMUM YYSIZE_MAXIMUM
#  endif
#  if (defined __cplusplus && ! defined EXIT_SUCCESS \
       && ! ((defined YYMALLOC || defined malloc) \
             && (defined YYFREE || defined free)))
#   include <stdlib.h> /* INFRINGES ON USER NAME SPACE */
#   ifndef EXIT_SUCCESS
#    define EXIT_SUCCESS 0
#   endif
#  endif
#  ifndef YYMALLOC
#   define YYMALLOC malloc
#   if ! defined malloc && ! defined EXIT_SUCCESS
void *malloc (YYSIZE_T); /* INFRINGES ON USER NAME SPACE */
#   endif
#  endif
#  ifndef YYFREE
#   define YYFREE free
#   if ! defined free && ! defined EXIT_SUCCESS
void free (void *); /* INFRINGES ON USER NAME SPACE */
#   endif
#  endif
# endif
#endif /* 1 */

#if (! defined yyoverflow \
     && (! defined __cplusplus \
         || (defined YYLTYPE_IS_TRIVIAL && YYLTYPE_IS_TRIVIAL \
             && defined YYSTYPE_IS_TRIVIAL && YYSTYPE_IS_TRIVIAL)))

/* A type that is properly aligned for any stack member.  */
union yyalloc
{
  yy_state_t yyss_alloc;
  YYSTYPE yyvs_alloc;
  YYLTYPE yyls_alloc;
};

/* The size of the maximum gap between one aligned stack and the next.  */
# define YYSTACK_GAP_MAXIMUM (YYSIZEOF (union yyalloc) - 1)

/* The size of an array large to enough to hold all stacks, each with
   N elements.  */
# define YYSTACK_BYTES(N) \
     ((N) * (YYSIZEOF (yy_state_t) + YYSIZEOF (YYSTYPE) \
             + YYSIZEOF (YYLTYPE)) \
      + 2 * YYSTACK_GAP_MAXIMUM)

# define YYCOPY_NEEDED 1

/* Relocate STACK from its old location to the new one.  The
   local variables YYSIZE and YYSTACKSIZE give the old and new number of
   elements in the stack, and YYPTR gives the new location of the
   stack.  Advance YYPTR to a properly aligned location for the next
   stack.  */
# define YYSTACK_RELOCATE(Stack_alloc, Stack)                           \
    do                                                                  \
      {                                                                 \
        YYPTRDIFF_T yynewbytes;                                         \
        YYCOPY (&yyptr->Stack_alloc, Stack, yysize);                    \
        Stack = &yyptr->Stack_alloc;                                    \
        yynewbytes = yystacksize * YYSIZEOF (*Stack) + YYSTACK_GAP_MAXIMUM; \
        yyptr += yynewbytes / YYSIZEOF (*yyptr);                        \
      }                                                                 \
    while (0)

#endif

#if defined YYCOPY_NEEDED && YYCOPY_NEEDED
/* Copy COUNT objects from SRC to DST.  The source and destination do
   not overlap.  */
# ifndef YYCOPY
#  if defined __GNUC__ && 1 < __GNUC__
#   define YYCOPY(Dst, Src, Count) \
      __builtin_memcpy (Dst, Src, YY_CAST (YYSIZE_T, (Count)) * sizeof (*(Src)))
#  else
#   define YYCOPY(Dst, Src, Count)              \
      do                                        \
        {                                       \
          YYPTRDIFF_T yyi;                      \
          for (yyi = 0; yyi < (Count); yyi++)   \
            (Dst)[yyi] = (Src)[yyi];            \
        }                                       \
      while (0)
#  endif
# endif
#endif /* !YYCOPY_NEEDED */

/* YYFINAL -- State number of the termination state.  */
#define YYFINAL  58
/* YYLAST -- Last index in YYTABLE.  */
#define YYLAST   319

/* YYNTOKENS -- Number of terminals.  */
#define YYNTOKENS  79
/* YYNNTS -- Number of nonterminals.  */
#define YYNNTS  47
/* YYNRULES -- Number of rules.  */
#define YYNRULES  142
/* YYNSTATES -- Number of states.  */
#define YYNSTATES  314

/* YYMAXUTOK -- Last valid token kind.  */
#define YYMAXUTOK   322


/* YYTRANSLATE(TOKEN-NUM) -- Symbol number corresponding to TOKEN-NUM
   as returned by yylex, with out-of-bounds checking.  */
#define YYTRANSLATE(YYX)                                \
  (0 <= (YYX) && (YYX) <= YYMAXUTOK                     \
   ? YY_CAST (yysymbol_kind_t, yytranslate[YYX])        \
   : YYSYMBOL_YYUNDEF)

/* YYTRANSLATE[TOKEN-NUM] -- Symbol number corresponding to TOKEN-NUM
   as returned by yylex.  */
static const yytype_int8 yytranslate[] =
{
       0,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
      70,    71,    78,    76,    72,    77,    73,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,    68,
      74,    69,    75,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     1,     2,     3,     4,
       5,     6,     7,     8,     9,    10,    11,    12,    13,    14,
      15,    16,    17,    18,    19,    20,    21,    22,    23,    24,
      25,    26,    27,    28,    29,    30,    31,    32,    33,    34,
      35,    36,    37,    38,    39,    40,    41,    42,    43,    44,
      45,    46,    47,    48,    49,    50,    51,    52,    53,    54,
      55,    56,    57,    58,    59,    60,    61,    62,    63,    64,
      65,    66,    67
};

#if YYDEBUG
/* YYRLINE[YYN] -- Source line where rule number YYN was defined.  */
static const yytype_int16 yyrline[] =
{
       0,    94,    94,    99,   104,   109,   117,   118,   119,   120,
     121,   122,   126,   130,   134,   138,   145,   149,   156,   160,
     164,   171,   175,   179,   183,   187,   194,   198,   202,   206,
     210,   217,   228,   236,   240,   249,   258,   267,   268,   269,
     273,   278,   283,   294,   295,   302,   306,   313,   317,   324,
     331,   335,   339,   343,   350,   354,   361,   365,   369,   373,
     377,   384,   391,   392,   399,   403,   410,   414,   421,   425,
     432,   436,   440,   444,   448,   452,   459,   463,   470,   474,
     481,   485,   489,   496,   500,   504,   508,   523,   534,   539,
     544,   558,   573,   577,   581,   585,   592,   597,   605,   611,
     618,   625,   631,   637,   643,   652,   653,   657,   658,   662,
     666,   673,   674,   681,   686,   692,   698,   703,   708,   713,
     721,   722,   726,   730,   734,   743,   747,   754,   758,   765,
     770,   776,   782,   787,   792,   797,   805,   806,   807,   811,
     812,   815,   817
};
#endif

/** Accessing symbol of state STATE.  */
#define YY_ACCESSING_SYMBOL(State) YY_CAST (yysymbol_kind_t, yystos[State])

#if 1
/* The user-facing name of the symbol whose (internal) number is
   YYSYMBOL.  No bounds checking.  */
static const char *yysymbol_name (yysymbol_kind_t yysymbol) YY_ATTRIBUTE_UNUSED;

/* YYTNAME[SYMBOL-NUM] -- String name of the symbol SYMBOL-NUM.
   First, the terminals, then, starting at YYNTOKENS, nonterminals.  */
static const char *const yytname[] =
{
  "\"end of file\"", "error", "\"invalid token\"", "SHOW", "TABLES",
  "CREATE", "TABLE", "DROP", "DESC", "INSERT", "INTO", "VALUES", "DELETE",
  "FROM", "ASC", "ORDER", "BY", "WHERE", "UPDATE", "SET", "SELECT", "INT",
  "CHAR", "FLOAT", "DATETIME", "INDEX", "AND", "JOIN", "ON", "AS",
  "EXPLAIN", "ANALYZE", "EXIT", "HELP", "TXN_BEGIN", "TXN_COMMIT",
  "TXN_ABORT", "TXN_ROLLBACK", "ORDER_BY", "ENABLE_NESTLOOP",
  "ENABLE_SORTMERGE", "GROUP", "HAVING", "LIMIT", "COUNT", "DISTINCT",
  "MAX_TOK", "MIN_TOK", "SUM_TOK", "AVG_TOK", "UNION", "TRANSACTION",
  "ISOLATION", "LEVEL", "SNAPSHOT", "SERIALIZABLE", "LOAD", "LEQ", "NEQ",
  "GEQ", "T_EOF", "IDENTIFIER", "VALUE_STRING", "FILEPATH", "PARAMETER",
  "VALUE_INT", "VALUE_FLOAT", "VALUE_BOOL", "';'", "'='", "'('", "')'",
  "','", "'.'", "'<'", "'>'", "'+'", "'-'", "'*'", "$accept", "start",
  "stmt", "txnStmt", "dbStmt", "setStmt", "ddl", "dml", "selectStmt",
  "explainStmt", "tableRef", "unionExpr", "aliasOpt", "fromClause",
  "onClauseOpt", "fieldList", "colNameList", "field", "type", "valueList",
  "value", "condition", "optWhereClause", "whereClause", "col", "colList",
  "op", "expr", "setClauses", "setClause", "arithmeticSetClause",
  "selector", "selectList", "selectItem", "opt_group_by", "opt_having",
  "havingClause", "havingExpr", "opt_limit", "opt_order_clause",
  "order_clause", "order_item_list", "order_item", "opt_asc_desc",
  "set_knob_type", "tbName", "colName", YY_NULLPTR
};

static const char *
yysymbol_name (yysymbol_kind_t yysymbol)
{
  return yytname[yysymbol];
}
#endif

#define YYPACT_NINF (-161)

#define yypact_value_is_default(Yyn) \
  ((Yyn) == YYPACT_NINF)

#define YYTABLE_NINF (-142)

#define yytable_value_is_error(Yyn) \
  0

/* YYPACT[STATE-NUM] -- Index in YYTABLE of the portion describing
   STATE-NUM.  */
static const yytype_int16 yypact[] =
{
      97,    10,    62,    76,   -49,     7,     9,   -49,    38,    -2,
      21,  -161,  -161,  -161,  -161,  -161,  -161,   -43,  -161,    54,
     -12,  -161,  -161,  -161,  -161,  -161,  -161,  -161,  -161,    59,
     -49,   -49,   -49,   -49,  -161,  -161,   -49,   -49,    71,  -161,
    -161,    55,    45,    56,    66,    70,    78,    82,    51,  -161,
     -11,   133,    93,  -161,    85,  -161,   150,   168,  -161,  -161,
     -49,   110,   129,  -161,   134,   200,   212,   169,   178,   165,
     -35,   172,   172,   172,   172,   173,  -161,  -161,    -1,    64,
     169,  -161,   -49,  -161,   169,   169,   169,   166,    74,  -161,
    -161,   -14,  -161,   124,   170,    96,  -161,    22,   164,   167,
     171,   174,   175,   176,  -161,   150,  -161,    -6,   -11,  -161,
    -161,  -161,   125,  -161,   163,   145,  -161,   147,   158,   180,
     182,   183,   184,   185,  -161,   211,  -161,    80,   169,  -161,
    -161,  -161,   158,   158,   141,   188,  -161,   172,   177,   -11,
     -11,   -11,   -11,   -11,   -11,   191,     3,    -1,    -1,   202,
    -161,  -161,   169,  -161,   186,  -161,  -161,  -161,  -161,   169,
    -161,  -161,  -161,  -161,  -161,  -161,   156,  -161,   -30,   172,
     172,   172,   172,    74,  -161,  -161,  -161,  -161,  -161,  -161,
     148,  -161,  -161,  -161,  -161,   117,  -161,   187,   -11,  -161,
    -161,  -161,  -161,  -161,  -161,   150,   150,   -11,   216,  -161,
     241,   217,  -161,   195,  -161,  -161,   158,    23,   190,   192,
     193,   194,   196,   203,  -161,  -161,  -161,  -161,  -161,  -161,
     158,   158,   158,   204,  -161,  -161,  -161,  -161,    74,  -161,
     172,    74,   247,   207,  -161,   172,   208,  -161,  -161,  -161,
    -161,  -161,  -161,  -161,  -161,  -161,   -11,   211,  -161,   201,
    -161,   240,   264,   238,  -161,   213,  -161,  -161,   172,    74,
     120,   218,  -161,   215,  -161,  -161,   219,   220,   221,   222,
     223,    72,  -161,   210,  -161,  -161,  -161,   -29,   172,   172,
     172,   172,  -161,  -161,  -161,   120,    86,   224,   225,   226,
     227,   228,   229,  -161,   172,   230,    72,    72,    72,    72,
      72,    72,   231,    72,  -161,  -161,  -161,  -161,  -161,  -161,
     232,  -161,    72,  -161
};

/* YYDEFACT[STATE-NUM] -- Default reduction number in state STATE-NUM.
   Performed when YYTABLE does not specify something else to do.  Zero
   means the default is an error.  */
static const yytype_uint8 yydefact[] =
{
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     4,     3,    12,    13,    14,    15,     0,     5,     0,
       0,     9,     6,    10,     7,     8,    30,    11,    16,     0,
       0,     0,     0,     0,   141,    23,     0,     0,     0,   139,
     140,     0,     0,     0,     0,     0,     0,     0,   142,    92,
      37,     0,    93,    94,     0,    67,     0,     0,     1,     2,
       0,     0,     0,    22,     0,     0,    62,     0,     0,     0,
       0,     0,     0,     0,     0,     0,    38,    96,     0,     0,
       0,    32,     0,    17,     0,     0,     0,     0,     0,    28,
     142,    62,    78,    82,     0,     0,    18,     0,     0,     0,
       0,     0,     0,     0,    39,     0,    40,    62,    37,    95,
      66,    27,     0,    45,     0,     0,    47,     0,     0,     0,
       0,     0,     0,     0,    64,    63,   111,     0,     0,    29,
      90,    91,     0,     0,     0,     0,    20,     0,     0,    37,
      37,    37,    37,    37,    37,     0,     0,     0,     0,   105,
      33,    21,     0,    50,     0,    52,    53,    49,    24,     0,
      25,    58,    60,    56,    57,    59,     0,    54,     0,     0,
       0,     0,     0,     0,    74,    73,    75,    70,    71,    72,
       0,    79,    88,    89,    80,    81,    19,     0,    37,    97,
      98,   101,   102,   103,   104,     0,     0,    37,    43,    41,
       0,   107,    46,     0,    48,    26,     0,     0,     0,     0,
       0,     0,     0,     0,    65,    76,    77,    61,    86,    87,
       0,     0,     0,     0,    99,    35,    36,    34,     0,    42,
       0,     0,   123,     0,    55,     0,     0,   112,   113,   116,
     117,   118,   119,    83,    84,    85,    37,    44,    68,   106,
     109,   108,     0,   120,    51,     0,   114,   100,     0,     0,
       0,     0,    31,     0,    69,   110,     0,     0,     0,     0,
       0,   138,   122,   124,   125,   121,   115,     0,     0,     0,
       0,     0,   137,   136,   127,     0,     0,     0,     0,     0,
       0,     0,     0,   126,     0,     0,   138,   138,   138,   138,
     138,   138,     0,   138,   128,   129,   132,   133,   134,   135,
       0,   130,   138,   131
};

/* YYPGOTO[NTERM-NUM].  */
static const yytype_int16 yypgoto[] =
{
    -161,  -161,  -161,  -161,  -161,  -161,  -161,  -161,   -54,  -161,
      44,  -161,  -103,  -161,  -161,  -161,   233,   135,  -161,  -161,
    -125,  -160,    12,    60,    -9,  -161,  -161,  -161,  -161,   179,
    -161,  -161,  -161,   234,  -161,  -161,  -161,  -161,  -161,  -161,
    -161,  -161,    19,  -124,  -161,    -3,   -61
};

/* YYDEFGOTO[NTERM-NUM].  */
static const yytype_int16 yydefgoto[] =
{
       0,    19,    20,    21,    22,    23,    24,    25,    26,    27,
     106,   146,    77,   107,   229,   112,   115,   113,   157,   166,
     167,   124,    89,   125,   126,   249,   180,   217,    91,    92,
      93,    51,    52,    53,   201,   232,   251,   127,   262,   253,
     272,   273,   274,   284,    42,    54,    55
};

/* YYTABLE[YYPACT[STATE-NUM]] -- What to do in state STATE-NUM.  If
   positive, shift that token.  If negative, reduce the rule whose
   number is the opposite.  If YYTABLE_NINF, syntax error.  */
static const yytype_int16 yytable[] =
{
      50,    35,    81,    88,    38,   150,    94,   182,   183,   184,
      97,    88,    34,   214,    28,   207,   286,    36,    75,   110,
      57,   147,    37,   114,   116,   116,    48,    61,    62,    63,
      64,    48,    48,    65,    66,    29,   189,   190,   191,   192,
     193,   194,    43,    98,    44,    45,    46,    47,   208,   287,
      76,   145,    56,   196,    58,   215,    59,    83,   128,    48,
      34,    99,   100,   101,   102,   103,   148,    94,    30,   105,
      50,   250,    60,   185,   197,   108,    49,    39,    40,   111,
     282,   234,    32,    48,    48,   224,   283,    31,   138,    41,
      67,   114,   137,   235,   227,   243,   244,   245,   204,   265,
       1,    33,     2,   129,     3,     4,     5,    68,    43,     6,
      44,    45,    46,    47,    69,     7,     8,     9,   119,   149,
     120,   121,   122,   123,  -141,    48,    70,    10,   187,    11,
      12,    13,    14,    15,    16,    48,    71,   174,   175,   176,
      72,   225,   226,   257,   108,   108,    78,    48,    73,   177,
     135,   136,    74,    17,   178,   179,   294,    18,    80,   209,
     210,   211,   212,   213,   266,    79,   267,   268,   269,   270,
       9,   216,   304,   305,   306,   307,   308,   309,    82,   311,
      84,    48,   218,   219,   153,   154,   155,   156,   313,   130,
     131,   198,   199,   220,   221,   222,   151,   152,   236,    85,
     132,   133,    90,   161,    86,   162,   163,   164,   165,    48,
     161,    87,   162,   163,   164,   165,   158,   159,   160,   159,
     161,   248,   162,   163,   164,   165,   255,   205,   206,    88,
      90,    95,    96,    48,   104,   139,   118,   173,   140,   134,
     186,   195,   141,   200,   228,   142,   143,   144,   188,   264,
     168,   271,   169,   170,   171,   172,   203,   230,   223,   231,
     233,   237,   252,   238,   239,   240,   259,   241,   288,   289,
     290,   291,   292,   258,   242,   246,   271,   295,   254,   256,
     260,   261,   285,   275,   263,   302,   276,   202,   247,   277,
     278,   279,   280,   281,     0,   296,   297,   298,   299,   300,
     301,   303,   310,   312,   293,     0,     0,   181,     0,     0,
       0,     0,     0,   109,     0,     0,     0,     0,     0,   117
};

static const yytype_int16 yycheck[] =
{
       9,     4,    56,    17,     7,   108,    67,   132,   133,   134,
      45,    17,    61,   173,     4,    45,    45,    10,    29,    80,
      63,    27,    13,    84,    85,    86,    61,    30,    31,    32,
      33,    61,    61,    36,    37,    25,   139,   140,   141,   142,
     143,   144,    44,    78,    46,    47,    48,    49,    78,    78,
      61,   105,    31,    50,     0,   180,    68,    60,    72,    61,
      61,    70,    71,    72,    73,    74,    72,   128,     6,    70,
      79,   231,    13,   134,    71,    78,    78,    39,    40,    82,
       8,   206,     6,    61,    61,   188,    14,    25,    97,    51,
      19,   152,    70,    70,   197,   220,   221,   222,   159,   259,
       3,    25,     5,    91,     7,     8,     9,    52,    44,    12,
      46,    47,    48,    49,    69,    18,    19,    20,    44,   107,
      46,    47,    48,    49,    73,    61,    70,    30,   137,    32,
      33,    34,    35,    36,    37,    61,    70,    57,    58,    59,
      70,   195,   196,   246,   147,   148,    13,    61,    70,    69,
      54,    55,    70,    56,    74,    75,    70,    60,    73,   168,
     169,   170,   171,   172,    44,    72,    46,    47,    48,    49,
      20,   180,   296,   297,   298,   299,   300,   301,    10,   303,
      70,    61,    65,    66,    21,    22,    23,    24,   312,    65,
      66,   147,   148,    76,    77,    78,    71,    72,   207,    70,
      76,    77,    61,    62,    70,    64,    65,    66,    67,    61,
      62,    11,    64,    65,    66,    67,    71,    72,    71,    72,
      62,   230,    64,    65,    66,    67,   235,    71,    72,    17,
      61,    53,    67,    61,    61,    71,    70,    26,    71,    69,
      52,    50,    71,    41,    28,    71,    71,    71,    71,   258,
      70,   260,    70,    70,    70,    70,    70,    16,    71,    42,
      65,    71,    15,    71,    71,    71,    26,    71,   277,   278,
     279,   280,   281,    72,    71,    71,   285,   286,    71,    71,
      16,    43,    72,    65,    71,   294,    71,   152,   228,    70,
      70,    70,    70,    70,    -1,    71,    71,    71,    71,    71,
      71,    71,    71,    71,   285,    -1,    -1,   128,    -1,    -1,
      -1,    -1,    -1,    79,    -1,    -1,    -1,    -1,    -1,    86
};

/* YYSTOS[STATE-NUM] -- The symbol kind of the accessing symbol of
   state STATE-NUM.  */
static const yytype_int8 yystos[] =
{
       0,     3,     5,     7,     8,     9,    12,    18,    19,    20,
      30,    32,    33,    34,    35,    36,    37,    56,    60,    80,
      81,    82,    83,    84,    85,    86,    87,    88,     4,    25,
       6,    25,     6,    25,    61,   124,    10,    13,   124,    39,
      40,    51,   123,    44,    46,    47,    48,    49,    61,    78,
     103,   110,   111,   112,   124,   125,    31,    63,     0,    68,
      13,   124,   124,   124,   124,   124,   124,    19,    52,    69,
      70,    70,    70,    70,    70,    29,    61,    91,    13,    72,
      73,    87,    10,   124,    70,    70,    70,    11,    17,   101,
      61,   107,   108,   109,   125,    53,    67,    45,    78,   103,
     103,   103,   103,   103,    61,    70,    89,    92,   124,   112,
     125,   124,    94,    96,   125,    95,   125,    95,    70,    44,
      46,    47,    48,    49,   100,   102,   103,   116,    72,   101,
      65,    66,    76,    77,    69,    54,    55,    70,   103,    71,
      71,    71,    71,    71,    71,    87,    90,    27,    72,   101,
      91,    71,    72,    21,    22,    23,    24,    97,    71,    72,
      71,    62,    64,    65,    66,    67,    98,    99,    70,    70,
      70,    70,    70,    26,    57,    58,    59,    69,    74,    75,
     105,   108,    99,    99,    99,   125,    52,   103,    71,    91,
      91,    91,    91,    91,    91,    50,    50,    71,    89,    89,
      41,   113,    96,    70,   125,    71,    72,    45,    78,   103,
     103,   103,   103,   103,   100,    99,   103,   106,    65,    66,
      76,    77,    78,    71,    91,    87,    87,    91,    28,    93,
      16,    42,   114,    65,    99,    70,   103,    71,    71,    71,
      71,    71,    71,    99,    99,    99,    71,   102,   103,   104,
     100,   115,    15,   118,    71,   103,    71,    91,    72,    26,
      16,    43,   117,    71,   103,   100,    44,    46,    47,    48,
      49,   103,   119,   120,   121,    65,    71,    70,    70,    70,
      70,    70,     8,    14,   122,    72,    45,    78,   103,   103,
     103,   103,   103,   121,    70,   103,    71,    71,    71,    71,
      71,    71,   103,    71,   122,   122,   122,   122,   122,   122,
      71,   122,    71,   122
};

/* YYR1[RULE-NUM] -- Symbol kind of the left-hand side of rule RULE-NUM.  */
static const yytype_int8 yyr1[] =
{
       0,    79,    80,    80,    80,    80,    81,    81,    81,    81,
      81,    81,    82,    82,    82,    82,    83,    83,    84,    84,
      84,    85,    85,    85,    85,    85,    86,    86,    86,    86,
      86,    87,    88,    89,    89,    90,    90,    91,    91,    91,
      92,    92,    92,    93,    93,    94,    94,    95,    95,    96,
      97,    97,    97,    97,    98,    98,    99,    99,    99,    99,
      99,   100,   101,   101,   102,   102,   103,   103,   104,   104,
     105,   105,   105,   105,   105,   105,   106,   106,   107,   107,
     108,   108,   108,   109,   109,   109,   109,   109,   109,   109,
     109,   109,   110,   110,   111,   111,   112,   112,   112,   112,
     112,   112,   112,   112,   112,   113,   113,   114,   114,   115,
     115,   116,   116,   116,   116,   116,   116,   116,   116,   116,
     117,   117,   118,   118,   119,   120,   120,   121,   121,   121,
     121,   121,   121,   121,   121,   121,   122,   122,   122,   123,
     123,   124,   125
};

/* YYR2[RULE-NUM] -- Number of symbols on the right-hand side of rule RULE-NUM.  */
static const yytype_int8 yyr2[] =
{
       0,     2,     2,     1,     1,     1,     1,     1,     1,     1,
       1,     1,     1,     1,     1,     1,     2,     4,     4,     6,
       5,     6,     3,     2,     6,     6,     7,     4,     4,     5,
       1,     9,     3,     2,     4,     3,     3,     0,     1,     2,
       1,     3,     4,     0,     2,     1,     3,     1,     3,     2,
       1,     4,     1,     1,     1,     3,     1,     1,     1,     1,
       1,     3,     0,     2,     1,     3,     3,     1,     1,     3,
       1,     1,     1,     1,     1,     1,     1,     1,     1,     3,
       3,     3,     1,     5,     5,     5,     4,     4,     3,     3,
       2,     2,     1,     1,     1,     3,     2,     5,     5,     6,
       8,     5,     5,     5,     5,     0,     3,     0,     2,     1,
       3,     1,     4,     4,     5,     7,     4,     4,     4,     4,
       0,     2,     3,     0,     1,     1,     3,     2,     5,     5,
       6,     8,     5,     5,     5,     5,     1,     1,     0,     1,
       1,     1,     1
};


enum { YYENOMEM = -2 };

#define yyerrok         (yyerrstatus = 0)
#define yyclearin       (yychar = YYEMPTY)

#define YYACCEPT        goto yyacceptlab
#define YYABORT         goto yyabortlab
#define YYERROR         goto yyerrorlab
#define YYNOMEM         goto yyexhaustedlab


#define YYRECOVERING()  (!!yyerrstatus)

#define YYBACKUP(Token, Value)                                    \
  do                                                              \
    if (yychar == YYEMPTY)                                        \
      {                                                           \
        yychar = (Token);                                         \
        yylval = (Value);                                         \
        YYPOPSTACK (yylen);                                       \
        yystate = *yyssp;                                         \
        goto yybackup;                                            \
      }                                                           \
    else                                                          \
      {                                                           \
        yyerror (&yylloc, scanner, parse_result, YY_("syntax error: cannot back up")); \
        YYERROR;                                                  \
      }                                                           \
  while (0)

/* Backward compatibility with an undocumented macro.
   Use YYerror or YYUNDEF. */
#define YYERRCODE YYUNDEF

/* YYLLOC_DEFAULT -- Set CURRENT to span from RHS[1] to RHS[N].
   If N is 0, then set CURRENT to the empty location which ends
   the previous symbol: RHS[0] (always defined).  */

#ifndef YYLLOC_DEFAULT
# define YYLLOC_DEFAULT(Current, Rhs, N)                                \
    do                                                                  \
      if (N)                                                            \
        {                                                               \
          (Current).first_line   = YYRHSLOC (Rhs, 1).first_line;        \
          (Current).first_column = YYRHSLOC (Rhs, 1).first_column;      \
          (Current).last_line    = YYRHSLOC (Rhs, N).last_line;         \
          (Current).last_column  = YYRHSLOC (Rhs, N).last_column;       \
        }                                                               \
      else                                                              \
        {                                                               \
          (Current).first_line   = (Current).last_line   =              \
            YYRHSLOC (Rhs, 0).last_line;                                \
          (Current).first_column = (Current).last_column =              \
            YYRHSLOC (Rhs, 0).last_column;                              \
        }                                                               \
    while (0)
#endif

#define YYRHSLOC(Rhs, K) ((Rhs)[K])


/* Enable debugging if requested.  */
#if YYDEBUG

# ifndef YYFPRINTF
#  include <stdio.h> /* INFRINGES ON USER NAME SPACE */
#  define YYFPRINTF fprintf
# endif

# define YYDPRINTF(Args)                        \
do {                                            \
  if (yydebug)                                  \
    YYFPRINTF Args;                             \
} while (0)


/* YYLOCATION_PRINT -- Print the location on the stream.
   This macro was not mandated originally: define only if we know
   we won't break user code: when these are the locations we know.  */

# ifndef YYLOCATION_PRINT

#  if defined YY_LOCATION_PRINT

   /* Temporary convenience wrapper in case some people defined the
      undocumented and private YY_LOCATION_PRINT macros.  */
#   define YYLOCATION_PRINT(File, Loc)  YY_LOCATION_PRINT(File, *(Loc))

#  elif defined YYLTYPE_IS_TRIVIAL && YYLTYPE_IS_TRIVIAL

/* Print *YYLOCP on YYO.  Private, do not rely on its existence. */

YY_ATTRIBUTE_UNUSED
static int
yy_location_print_ (FILE *yyo, YYLTYPE const * const yylocp)
{
  int res = 0;
  int end_col = 0 != yylocp->last_column ? yylocp->last_column - 1 : 0;
  if (0 <= yylocp->first_line)
    {
      res += YYFPRINTF (yyo, "%d", yylocp->first_line);
      if (0 <= yylocp->first_column)
        res += YYFPRINTF (yyo, ".%d", yylocp->first_column);
    }
  if (0 <= yylocp->last_line)
    {
      if (yylocp->first_line < yylocp->last_line)
        {
          res += YYFPRINTF (yyo, "-%d", yylocp->last_line);
          if (0 <= end_col)
            res += YYFPRINTF (yyo, ".%d", end_col);
        }
      else if (0 <= end_col && yylocp->first_column < end_col)
        res += YYFPRINTF (yyo, "-%d", end_col);
    }
  return res;
}

#   define YYLOCATION_PRINT  yy_location_print_

    /* Temporary convenience wrapper in case some people defined the
       undocumented and private YY_LOCATION_PRINT macros.  */
#   define YY_LOCATION_PRINT(File, Loc)  YYLOCATION_PRINT(File, &(Loc))

#  else

#   define YYLOCATION_PRINT(File, Loc) ((void) 0)
    /* Temporary convenience wrapper in case some people defined the
       undocumented and private YY_LOCATION_PRINT macros.  */
#   define YY_LOCATION_PRINT  YYLOCATION_PRINT

#  endif
# endif /* !defined YYLOCATION_PRINT */


# define YY_SYMBOL_PRINT(Title, Kind, Value, Location)                    \
do {                                                                      \
  if (yydebug)                                                            \
    {                                                                     \
      YYFPRINTF (stderr, "%s ", Title);                                   \
      yy_symbol_print (stderr,                                            \
                  Kind, Value, Location, scanner, parse_result); \
      YYFPRINTF (stderr, "\n");                                           \
    }                                                                     \
} while (0)


/*-----------------------------------.
| Print this symbol's value on YYO.  |
`-----------------------------------*/

static void
yy_symbol_value_print (FILE *yyo,
                       yysymbol_kind_t yykind, YYSTYPE const * const yyvaluep, YYLTYPE const * const yylocationp, yyscan_t scanner, std::shared_ptr<ast::TreeNode> *parse_result)
{
  FILE *yyoutput = yyo;
  YY_USE (yyoutput);
  YY_USE (yylocationp);
  YY_USE (scanner);
  YY_USE (parse_result);
  if (!yyvaluep)
    return;
  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  YY_USE (yykind);
  YY_IGNORE_MAYBE_UNINITIALIZED_END
}


/*---------------------------.
| Print this symbol on YYO.  |
`---------------------------*/

static void
yy_symbol_print (FILE *yyo,
                 yysymbol_kind_t yykind, YYSTYPE const * const yyvaluep, YYLTYPE const * const yylocationp, yyscan_t scanner, std::shared_ptr<ast::TreeNode> *parse_result)
{
  YYFPRINTF (yyo, "%s %s (",
             yykind < YYNTOKENS ? "token" : "nterm", yysymbol_name (yykind));

  YYLOCATION_PRINT (yyo, yylocationp);
  YYFPRINTF (yyo, ": ");
  yy_symbol_value_print (yyo, yykind, yyvaluep, yylocationp, scanner, parse_result);
  YYFPRINTF (yyo, ")");
}

/*------------------------------------------------------------------.
| yy_stack_print -- Print the state stack from its BOTTOM up to its |
| TOP (included).                                                   |
`------------------------------------------------------------------*/

static void
yy_stack_print (yy_state_t *yybottom, yy_state_t *yytop)
{
  YYFPRINTF (stderr, "Stack now");
  for (; yybottom <= yytop; yybottom++)
    {
      int yybot = *yybottom;
      YYFPRINTF (stderr, " %d", yybot);
    }
  YYFPRINTF (stderr, "\n");
}

# define YY_STACK_PRINT(Bottom, Top)                            \
do {                                                            \
  if (yydebug)                                                  \
    yy_stack_print ((Bottom), (Top));                           \
} while (0)


/*------------------------------------------------.
| Report that the YYRULE is going to be reduced.  |
`------------------------------------------------*/

static void
yy_reduce_print (yy_state_t *yyssp, YYSTYPE *yyvsp, YYLTYPE *yylsp,
                 int yyrule, yyscan_t scanner, std::shared_ptr<ast::TreeNode> *parse_result)
{
  int yylno = yyrline[yyrule];
  int yynrhs = yyr2[yyrule];
  int yyi;
  YYFPRINTF (stderr, "Reducing stack by rule %d (line %d):\n",
             yyrule - 1, yylno);
  /* The symbols being reduced.  */
  for (yyi = 0; yyi < yynrhs; yyi++)
    {
      YYFPRINTF (stderr, "   $%d = ", yyi + 1);
      yy_symbol_print (stderr,
                       YY_ACCESSING_SYMBOL (+yyssp[yyi + 1 - yynrhs]),
                       &yyvsp[(yyi + 1) - (yynrhs)],
                       &(yylsp[(yyi + 1) - (yynrhs)]), scanner, parse_result);
      YYFPRINTF (stderr, "\n");
    }
}

# define YY_REDUCE_PRINT(Rule)          \
do {                                    \
  if (yydebug)                          \
    yy_reduce_print (yyssp, yyvsp, yylsp, Rule, scanner, parse_result); \
} while (0)

/* Nonzero means print parse trace.  It is left uninitialized so that
   multiple parsers can coexist.  */
int yydebug;
#else /* !YYDEBUG */
# define YYDPRINTF(Args) ((void) 0)
# define YY_SYMBOL_PRINT(Title, Kind, Value, Location)
# define YY_STACK_PRINT(Bottom, Top)
# define YY_REDUCE_PRINT(Rule)
#endif /* !YYDEBUG */


/* YYINITDEPTH -- initial size of the parser's stacks.  */
#ifndef YYINITDEPTH
# define YYINITDEPTH 200
#endif

/* YYMAXDEPTH -- maximum size the stacks can grow to (effective only
   if the built-in stack extension method is used).

   Do not make this value too large; the results are undefined if
   YYSTACK_ALLOC_MAXIMUM < YYSTACK_BYTES (YYMAXDEPTH)
   evaluated with infinite-precision integer arithmetic.  */

#ifndef YYMAXDEPTH
# define YYMAXDEPTH 10000
#endif


/* Context of a parse error.  */
typedef struct
{
  yy_state_t *yyssp;
  yysymbol_kind_t yytoken;
  YYLTYPE *yylloc;
} yypcontext_t;

/* Put in YYARG at most YYARGN of the expected tokens given the
   current YYCTX, and return the number of tokens stored in YYARG.  If
   YYARG is null, return the number of expected tokens (guaranteed to
   be less than YYNTOKENS).  Return YYENOMEM on memory exhaustion.
   Return 0 if there are more than YYARGN expected tokens, yet fill
   YYARG up to YYARGN. */
static int
yypcontext_expected_tokens (const yypcontext_t *yyctx,
                            yysymbol_kind_t yyarg[], int yyargn)
{
  /* Actual size of YYARG. */
  int yycount = 0;
  int yyn = yypact[+*yyctx->yyssp];
  if (!yypact_value_is_default (yyn))
    {
      /* Start YYX at -YYN if negative to avoid negative indexes in
         YYCHECK.  In other words, skip the first -YYN actions for
         this state because they are default actions.  */
      int yyxbegin = yyn < 0 ? -yyn : 0;
      /* Stay within bounds of both yycheck and yytname.  */
      int yychecklim = YYLAST - yyn + 1;
      int yyxend = yychecklim < YYNTOKENS ? yychecklim : YYNTOKENS;
      int yyx;
      for (yyx = yyxbegin; yyx < yyxend; ++yyx)
        if (yycheck[yyx + yyn] == yyx && yyx != YYSYMBOL_YYerror
            && !yytable_value_is_error (yytable[yyx + yyn]))
          {
            if (!yyarg)
              ++yycount;
            else if (yycount == yyargn)
              return 0;
            else
              yyarg[yycount++] = YY_CAST (yysymbol_kind_t, yyx);
          }
    }
  if (yyarg && yycount == 0 && 0 < yyargn)
    yyarg[0] = YYSYMBOL_YYEMPTY;
  return yycount;
}




#ifndef yystrlen
# if defined __GLIBC__ && defined _STRING_H
#  define yystrlen(S) (YY_CAST (YYPTRDIFF_T, strlen (S)))
# else
/* Return the length of YYSTR.  */
static YYPTRDIFF_T
yystrlen (const char *yystr)
{
  YYPTRDIFF_T yylen;
  for (yylen = 0; yystr[yylen]; yylen++)
    continue;
  return yylen;
}
# endif
#endif

#ifndef yystpcpy
# if defined __GLIBC__ && defined _STRING_H && defined _GNU_SOURCE
#  define yystpcpy stpcpy
# else
/* Copy YYSRC to YYDEST, returning the address of the terminating '\0' in
   YYDEST.  */
static char *
yystpcpy (char *yydest, const char *yysrc)
{
  char *yyd = yydest;
  const char *yys = yysrc;

  while ((*yyd++ = *yys++) != '\0')
    continue;

  return yyd - 1;
}
# endif
#endif

#ifndef yytnamerr
/* Copy to YYRES the contents of YYSTR after stripping away unnecessary
   quotes and backslashes, so that it's suitable for yyerror.  The
   heuristic is that double-quoting is unnecessary unless the string
   contains an apostrophe, a comma, or backslash (other than
   backslash-backslash).  YYSTR is taken from yytname.  If YYRES is
   null, do not copy; instead, return the length of what the result
   would have been.  */
static YYPTRDIFF_T
yytnamerr (char *yyres, const char *yystr)
{
  if (*yystr == '"')
    {
      YYPTRDIFF_T yyn = 0;
      char const *yyp = yystr;
      for (;;)
        switch (*++yyp)
          {
          case '\'':
          case ',':
            goto do_not_strip_quotes;

          case '\\':
            if (*++yyp != '\\')
              goto do_not_strip_quotes;
            else
              goto append;

          append:
          default:
            if (yyres)
              yyres[yyn] = *yyp;
            yyn++;
            break;

          case '"':
            if (yyres)
              yyres[yyn] = '\0';
            return yyn;
          }
    do_not_strip_quotes: ;
    }

  if (yyres)
    return yystpcpy (yyres, yystr) - yyres;
  else
    return yystrlen (yystr);
}
#endif


static int
yy_syntax_error_arguments (const yypcontext_t *yyctx,
                           yysymbol_kind_t yyarg[], int yyargn)
{
  /* Actual size of YYARG. */
  int yycount = 0;
  /* There are many possibilities here to consider:
     - If this state is a consistent state with a default action, then
       the only way this function was invoked is if the default action
       is an error action.  In that case, don't check for expected
       tokens because there are none.
     - The only way there can be no lookahead present (in yychar) is if
       this state is a consistent state with a default action.  Thus,
       detecting the absence of a lookahead is sufficient to determine
       that there is no unexpected or expected token to report.  In that
       case, just report a simple "syntax error".
     - Don't assume there isn't a lookahead just because this state is a
       consistent state with a default action.  There might have been a
       previous inconsistent state, consistent state with a non-default
       action, or user semantic action that manipulated yychar.
     - Of course, the expected token list depends on states to have
       correct lookahead information, and it depends on the parser not
       to perform extra reductions after fetching a lookahead from the
       scanner and before detecting a syntax error.  Thus, state merging
       (from LALR or IELR) and default reductions corrupt the expected
       token list.  However, the list is correct for canonical LR with
       one exception: it will still contain any token that will not be
       accepted due to an error action in a later state.
  */
  if (yyctx->yytoken != YYSYMBOL_YYEMPTY)
    {
      int yyn;
      if (yyarg)
        yyarg[yycount] = yyctx->yytoken;
      ++yycount;
      yyn = yypcontext_expected_tokens (yyctx,
                                        yyarg ? yyarg + 1 : yyarg, yyargn - 1);
      if (yyn == YYENOMEM)
        return YYENOMEM;
      else
        yycount += yyn;
    }
  return yycount;
}

/* Copy into *YYMSG, which is of size *YYMSG_ALLOC, an error message
   about the unexpected token YYTOKEN for the state stack whose top is
   YYSSP.

   Return 0 if *YYMSG was successfully written.  Return -1 if *YYMSG is
   not large enough to hold the message.  In that case, also set
   *YYMSG_ALLOC to the required number of bytes.  Return YYENOMEM if the
   required number of bytes is too large to store.  */
static int
yysyntax_error (YYPTRDIFF_T *yymsg_alloc, char **yymsg,
                const yypcontext_t *yyctx)
{
  enum { YYARGS_MAX = 5 };
  /* Internationalized format string. */
  const char *yyformat = YY_NULLPTR;
  /* Arguments of yyformat: reported tokens (one for the "unexpected",
     one per "expected"). */
  yysymbol_kind_t yyarg[YYARGS_MAX];
  /* Cumulated lengths of YYARG.  */
  YYPTRDIFF_T yysize = 0;

  /* Actual size of YYARG. */
  int yycount = yy_syntax_error_arguments (yyctx, yyarg, YYARGS_MAX);
  if (yycount == YYENOMEM)
    return YYENOMEM;

  switch (yycount)
    {
#define YYCASE_(N, S)                       \
      case N:                               \
        yyformat = S;                       \
        break
    default: /* Avoid compiler warnings. */
      YYCASE_(0, YY_("syntax error"));
      YYCASE_(1, YY_("syntax error, unexpected %s"));
      YYCASE_(2, YY_("syntax error, unexpected %s, expecting %s"));
      YYCASE_(3, YY_("syntax error, unexpected %s, expecting %s or %s"));
      YYCASE_(4, YY_("syntax error, unexpected %s, expecting %s or %s or %s"));
      YYCASE_(5, YY_("syntax error, unexpected %s, expecting %s or %s or %s or %s"));
#undef YYCASE_
    }

  /* Compute error message size.  Don't count the "%s"s, but reserve
     room for the terminator.  */
  yysize = yystrlen (yyformat) - 2 * yycount + 1;
  {
    int yyi;
    for (yyi = 0; yyi < yycount; ++yyi)
      {
        YYPTRDIFF_T yysize1
          = yysize + yytnamerr (YY_NULLPTR, yytname[yyarg[yyi]]);
        if (yysize <= yysize1 && yysize1 <= YYSTACK_ALLOC_MAXIMUM)
          yysize = yysize1;
        else
          return YYENOMEM;
      }
  }

  if (*yymsg_alloc < yysize)
    {
      *yymsg_alloc = 2 * yysize;
      if (! (yysize <= *yymsg_alloc
             && *yymsg_alloc <= YYSTACK_ALLOC_MAXIMUM))
        *yymsg_alloc = YYSTACK_ALLOC_MAXIMUM;
      return -1;
    }

  /* Avoid sprintf, as that infringes on the user's name space.
     Don't have undefined behavior even if the translation
     produced a string with the wrong number of "%s"s.  */
  {
    char *yyp = *yymsg;
    int yyi = 0;
    while ((*yyp = *yyformat) != '\0')
      if (*yyp == '%' && yyformat[1] == 's' && yyi < yycount)
        {
          yyp += yytnamerr (yyp, yytname[yyarg[yyi++]]);
          yyformat += 2;
        }
      else
        {
          ++yyp;
          ++yyformat;
        }
  }
  return 0;
}


/*-----------------------------------------------.
| Release the memory associated to this symbol.  |
`-----------------------------------------------*/

static void
yydestruct (const char *yymsg,
            yysymbol_kind_t yykind, YYSTYPE *yyvaluep, YYLTYPE *yylocationp, yyscan_t scanner, std::shared_ptr<ast::TreeNode> *parse_result)
{
  YY_USE (yyvaluep);
  YY_USE (yylocationp);
  YY_USE (scanner);
  YY_USE (parse_result);
  if (!yymsg)
    yymsg = "Deleting";
  YY_SYMBOL_PRINT (yymsg, yykind, yyvaluep, yylocationp);

  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  YY_USE (yykind);
  YY_IGNORE_MAYBE_UNINITIALIZED_END
}






/*----------.
| yyparse.  |
`----------*/

int
yyparse (yyscan_t scanner, std::shared_ptr<ast::TreeNode> *parse_result)
{
/* Lookahead token kind.  */
int yychar;


/* The semantic value of the lookahead symbol.  */
/* Default value used for initialization, for pacifying older GCCs
   or non-GCC compilers.  */
YY_INITIAL_VALUE (static YYSTYPE yyval_default;)
YYSTYPE yylval YY_INITIAL_VALUE (= yyval_default);

/* Location data for the lookahead symbol.  */
static YYLTYPE yyloc_default
# if defined YYLTYPE_IS_TRIVIAL && YYLTYPE_IS_TRIVIAL
  = { 1, 1, 1, 1 }
# endif
;
YYLTYPE yylloc = yyloc_default;

    /* Number of syntax errors so far.  */
    int yynerrs = 0;

    yy_state_fast_t yystate = 0;
    /* Number of tokens to shift before error messages enabled.  */
    int yyerrstatus = 0;

    /* Refer to the stacks through separate pointers, to allow yyoverflow
       to reallocate them elsewhere.  */

    /* Their size.  */
    YYPTRDIFF_T yystacksize = YYINITDEPTH;

    /* The state stack: array, bottom, top.  */
    yy_state_t yyssa[YYINITDEPTH];
    yy_state_t *yyss = yyssa;
    yy_state_t *yyssp = yyss;

    /* The semantic value stack: array, bottom, top.  */
    YYSTYPE yyvsa[YYINITDEPTH];
    YYSTYPE *yyvs = yyvsa;
    YYSTYPE *yyvsp = yyvs;

    /* The location stack: array, bottom, top.  */
    YYLTYPE yylsa[YYINITDEPTH];
    YYLTYPE *yyls = yylsa;
    YYLTYPE *yylsp = yyls;

  int yyn;
  /* The return value of yyparse.  */
  int yyresult;
  /* Lookahead symbol kind.  */
  yysymbol_kind_t yytoken = YYSYMBOL_YYEMPTY;
  /* The variables used to return semantic value and location from the
     action routines.  */
  YYSTYPE yyval;
  YYLTYPE yyloc;

  /* The locations where the error started and ended.  */
  YYLTYPE yyerror_range[3];

  /* Buffer for error messages, and its allocated size.  */
  char yymsgbuf[128];
  char *yymsg = yymsgbuf;
  YYPTRDIFF_T yymsg_alloc = sizeof yymsgbuf;

#define YYPOPSTACK(N)   (yyvsp -= (N), yyssp -= (N), yylsp -= (N))

  /* The number of symbols on the RHS of the reduced rule.
     Keep to zero when no symbol should be popped.  */
  int yylen = 0;

  YYDPRINTF ((stderr, "Starting parse\n"));

  yychar = YYEMPTY; /* Cause a token to be read.  */

  yylsp[0] = yylloc;
  goto yysetstate;


/*------------------------------------------------------------.
| yynewstate -- push a new state, which is found in yystate.  |
`------------------------------------------------------------*/
yynewstate:
  /* In all cases, when you get here, the value and location stacks
     have just been pushed.  So pushing a state here evens the stacks.  */
  yyssp++;


/*--------------------------------------------------------------------.
| yysetstate -- set current state (the top of the stack) to yystate.  |
`--------------------------------------------------------------------*/
yysetstate:
  YYDPRINTF ((stderr, "Entering state %d\n", yystate));
  YY_ASSERT (0 <= yystate && yystate < YYNSTATES);
  YY_IGNORE_USELESS_CAST_BEGIN
  *yyssp = YY_CAST (yy_state_t, yystate);
  YY_IGNORE_USELESS_CAST_END
  YY_STACK_PRINT (yyss, yyssp);

  if (yyss + yystacksize - 1 <= yyssp)
#if !defined yyoverflow && !defined YYSTACK_RELOCATE
    YYNOMEM;
#else
    {
      /* Get the current used size of the three stacks, in elements.  */
      YYPTRDIFF_T yysize = yyssp - yyss + 1;

# if defined yyoverflow
      {
        /* Give user a chance to reallocate the stack.  Use copies of
           these so that the &'s don't force the real ones into
           memory.  */
        yy_state_t *yyss1 = yyss;
        YYSTYPE *yyvs1 = yyvs;
        YYLTYPE *yyls1 = yyls;

        /* Each stack pointer address is followed by the size of the
           data in use in that stack, in bytes.  This used to be a
           conditional around just the two extra args, but that might
           be undefined if yyoverflow is a macro.  */
        yyoverflow (YY_("memory exhausted"),
                    &yyss1, yysize * YYSIZEOF (*yyssp),
                    &yyvs1, yysize * YYSIZEOF (*yyvsp),
                    &yyls1, yysize * YYSIZEOF (*yylsp),
                    &yystacksize);
        yyss = yyss1;
        yyvs = yyvs1;
        yyls = yyls1;
      }
# else /* defined YYSTACK_RELOCATE */
      /* Extend the stack our own way.  */
      if (YYMAXDEPTH <= yystacksize)
        YYNOMEM;
      yystacksize *= 2;
      if (YYMAXDEPTH < yystacksize)
        yystacksize = YYMAXDEPTH;

      {
        yy_state_t *yyss1 = yyss;
        union yyalloc *yyptr =
          YY_CAST (union yyalloc *,
                   YYSTACK_ALLOC (YY_CAST (YYSIZE_T, YYSTACK_BYTES (yystacksize))));
        if (! yyptr)
          YYNOMEM;
        YYSTACK_RELOCATE (yyss_alloc, yyss);
        YYSTACK_RELOCATE (yyvs_alloc, yyvs);
        YYSTACK_RELOCATE (yyls_alloc, yyls);
#  undef YYSTACK_RELOCATE
        if (yyss1 != yyssa)
          YYSTACK_FREE (yyss1);
      }
# endif

      yyssp = yyss + yysize - 1;
      yyvsp = yyvs + yysize - 1;
      yylsp = yyls + yysize - 1;

      YY_IGNORE_USELESS_CAST_BEGIN
      YYDPRINTF ((stderr, "Stack size increased to %ld\n",
                  YY_CAST (long, yystacksize)));
      YY_IGNORE_USELESS_CAST_END

      if (yyss + yystacksize - 1 <= yyssp)
        YYABORT;
    }
#endif /* !defined yyoverflow && !defined YYSTACK_RELOCATE */


  if (yystate == YYFINAL)
    YYACCEPT;

  goto yybackup;


/*-----------.
| yybackup.  |
`-----------*/
yybackup:
  /* Do appropriate processing given the current state.  Read a
     lookahead token if we need one and don't already have one.  */

  /* First try to decide what to do without reference to lookahead token.  */
  yyn = yypact[yystate];
  if (yypact_value_is_default (yyn))
    goto yydefault;

  /* Not known => get a lookahead token if don't already have one.  */

  /* YYCHAR is either empty, or end-of-input, or a valid lookahead.  */
  if (yychar == YYEMPTY)
    {
      YYDPRINTF ((stderr, "Reading a token\n"));
      yychar = yylex (&yylval, &yylloc, scanner);
    }

  if (yychar <= YYEOF)
    {
      yychar = YYEOF;
      yytoken = YYSYMBOL_YYEOF;
      YYDPRINTF ((stderr, "Now at end of input.\n"));
    }
  else if (yychar == YYerror)
    {
      /* The scanner already issued an error message, process directly
         to error recovery.  But do not keep the error token as
         lookahead, it is too special and may lead us to an endless
         loop in error recovery. */
      yychar = YYUNDEF;
      yytoken = YYSYMBOL_YYerror;
      yyerror_range[1] = yylloc;
      goto yyerrlab1;
    }
  else
    {
      yytoken = YYTRANSLATE (yychar);
      YY_SYMBOL_PRINT ("Next token is", yytoken, &yylval, &yylloc);
    }

  /* If the proper action on seeing token YYTOKEN is to reduce or to
     detect an error, take that action.  */
  yyn += yytoken;
  if (yyn < 0 || YYLAST < yyn || yycheck[yyn] != yytoken)
    goto yydefault;
  yyn = yytable[yyn];
  if (yyn <= 0)
    {
      if (yytable_value_is_error (yyn))
        goto yyerrlab;
      yyn = -yyn;
      goto yyreduce;
    }

  /* Count tokens shifted since error; after three, turn off error
     status.  */
  if (yyerrstatus)
    yyerrstatus--;

  /* Shift the lookahead token.  */
  YY_SYMBOL_PRINT ("Shifting", yytoken, &yylval, &yylloc);
  yystate = yyn;
  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  *++yyvsp = yylval;
  YY_IGNORE_MAYBE_UNINITIALIZED_END
  *++yylsp = yylloc;

  /* Discard the shifted token.  */
  yychar = YYEMPTY;
  goto yynewstate;


/*-----------------------------------------------------------.
| yydefault -- do the default action for the current state.  |
`-----------------------------------------------------------*/
yydefault:
  yyn = yydefact[yystate];
  if (yyn == 0)
    goto yyerrlab;
  goto yyreduce;


/*-----------------------------.
| yyreduce -- do a reduction.  |
`-----------------------------*/
yyreduce:
  /* yyn is the number of a rule to reduce with.  */
  yylen = yyr2[yyn];

  /* If YYLEN is nonzero, implement the default value of the action:
     '$$ = $1'.

     Otherwise, the following line sets YYVAL to garbage.
     This behavior is undocumented and Bison
     users should not rely upon it.  Assigning to YYVAL
     unconditionally makes the parser a bit smaller, and it avoids a
     GCC warning that YYVAL may be used uninitialized.  */
  yyval = yyvsp[1-yylen];

  /* Default location. */
  YYLLOC_DEFAULT (yyloc, (yylsp - yylen), yylen);
  yyerror_range[1] = yyloc;
  YY_REDUCE_PRINT (yyn);
  switch (yyn)
    {
  case 2: /* start: stmt ';'  */
#line 95 "src/parser/yacc.y"
    {
        *parse_result = (yyvsp[-1].sv_node);
        YYACCEPT;
    }
#line 1830 "src/parser/yacc.tab.cpp"
    break;

  case 3: /* start: HELP  */
#line 100 "src/parser/yacc.y"
    {
        *parse_result = std::make_shared<Help>();
        YYACCEPT;
    }
#line 1839 "src/parser/yacc.tab.cpp"
    break;

  case 4: /* start: EXIT  */
#line 105 "src/parser/yacc.y"
    {
        *parse_result = nullptr;
        YYACCEPT;
    }
#line 1848 "src/parser/yacc.tab.cpp"
    break;

  case 5: /* start: T_EOF  */
#line 110 "src/parser/yacc.y"
    {
        *parse_result = nullptr;
        YYACCEPT;
    }
#line 1857 "src/parser/yacc.tab.cpp"
    break;

  case 12: /* txnStmt: TXN_BEGIN  */
#line 127 "src/parser/yacc.y"
    {
        (yyval.sv_node) = std::make_shared<TxnBegin>();
    }
#line 1865 "src/parser/yacc.tab.cpp"
    break;

  case 13: /* txnStmt: TXN_COMMIT  */
#line 131 "src/parser/yacc.y"
    {
        (yyval.sv_node) = std::make_shared<TxnCommit>();
    }
#line 1873 "src/parser/yacc.tab.cpp"
    break;

  case 14: /* txnStmt: TXN_ABORT  */
#line 135 "src/parser/yacc.y"
    {
        (yyval.sv_node) = std::make_shared<TxnAbort>();
    }
#line 1881 "src/parser/yacc.tab.cpp"
    break;

  case 15: /* txnStmt: TXN_ROLLBACK  */
#line 139 "src/parser/yacc.y"
    {
        (yyval.sv_node) = std::make_shared<TxnRollback>();
    }
#line 1889 "src/parser/yacc.tab.cpp"
    break;

  case 16: /* dbStmt: SHOW TABLES  */
#line 146 "src/parser/yacc.y"
    {
        (yyval.sv_node) = std::make_shared<ShowTables>();
    }
#line 1897 "src/parser/yacc.tab.cpp"
    break;

  case 17: /* dbStmt: SHOW INDEX FROM tbName  */
#line 150 "src/parser/yacc.y"
    {
        (yyval.sv_node) = std::make_shared<ShowIndex>((yyvsp[0].sv_str));
    }
#line 1905 "src/parser/yacc.tab.cpp"
    break;

  case 18: /* setStmt: SET set_knob_type '=' VALUE_BOOL  */
#line 157 "src/parser/yacc.y"
    {
        (yyval.sv_node) = std::make_shared<SetStmt>((yyvsp[-2].sv_setKnobType), (yyvsp[0].sv_bool));
    }
#line 1913 "src/parser/yacc.tab.cpp"
    break;

  case 19: /* setStmt: SET TRANSACTION ISOLATION LEVEL SNAPSHOT ISOLATION  */
#line 161 "src/parser/yacc.y"
    {
        (yyval.sv_node) = std::make_shared<SetIsolationStmt>(SV_ISO_SNAPSHOT_ISOLATION);
    }
#line 1921 "src/parser/yacc.tab.cpp"
    break;

  case 20: /* setStmt: SET TRANSACTION ISOLATION LEVEL SERIALIZABLE  */
#line 165 "src/parser/yacc.y"
    {
        (yyval.sv_node) = std::make_shared<SetIsolationStmt>(SV_ISO_SERIALIZABLE);
    }
#line 1929 "src/parser/yacc.tab.cpp"
    break;

  case 21: /* ddl: CREATE TABLE tbName '(' fieldList ')'  */
#line 172 "src/parser/yacc.y"
    {
        (yyval.sv_node) = std::make_shared<CreateTable>((yyvsp[-3].sv_str), (yyvsp[-1].sv_fields));
    }
#line 1937 "src/parser/yacc.tab.cpp"
    break;

  case 22: /* ddl: DROP TABLE tbName  */
#line 176 "src/parser/yacc.y"
    {
        (yyval.sv_node) = std::make_shared<DropTable>((yyvsp[0].sv_str));
    }
#line 1945 "src/parser/yacc.tab.cpp"
    break;

  case 23: /* ddl: DESC tbName  */
#line 180 "src/parser/yacc.y"
    {
        (yyval.sv_node) = std::make_shared<DescTable>((yyvsp[0].sv_str));
    }
#line 1953 "src/parser/yacc.tab.cpp"
    break;

  case 24: /* ddl: CREATE INDEX tbName '(' colNameList ')'  */
#line 184 "src/parser/yacc.y"
    {
        (yyval.sv_node) = std::make_shared<CreateIndex>((yyvsp[-3].sv_str), (yyvsp[-1].sv_strs));
    }
#line 1961 "src/parser/yacc.tab.cpp"
    break;

  case 25: /* ddl: DROP INDEX tbName '(' colNameList ')'  */
#line 188 "src/parser/yacc.y"
    {
        (yyval.sv_node) = std::make_shared<DropIndex>((yyvsp[-3].sv_str), (yyvsp[-1].sv_strs));
    }
#line 1969 "src/parser/yacc.tab.cpp"
    break;

  case 26: /* dml: INSERT INTO tbName VALUES '(' valueList ')'  */
#line 195 "src/parser/yacc.y"
    {
        (yyval.sv_node) = std::make_shared<InsertStmt>((yyvsp[-4].sv_str), (yyvsp[-1].sv_vals));
    }
#line 1977 "src/parser/yacc.tab.cpp"
    break;

  case 27: /* dml: LOAD FILEPATH INTO tbName  */
#line 199 "src/parser/yacc.y"
    {
        (yyval.sv_node) = std::make_shared<LoadStmt>((yyvsp[-2].sv_str), (yyvsp[0].sv_str));
    }
#line 1985 "src/parser/yacc.tab.cpp"
    break;

  case 28: /* dml: DELETE FROM tbName optWhereClause  */
#line 203 "src/parser/yacc.y"
    {
        (yyval.sv_node) = std::make_shared<DeleteStmt>((yyvsp[-1].sv_str), (yyvsp[0].sv_conds));
    }
#line 1993 "src/parser/yacc.tab.cpp"
    break;

  case 29: /* dml: UPDATE tbName SET setClauses optWhereClause  */
#line 207 "src/parser/yacc.y"
    {
        (yyval.sv_node) = std::make_shared<UpdateStmt>((yyvsp[-3].sv_str), (yyvsp[-1].sv_set_clauses), (yyvsp[0].sv_conds));
    }
#line 2001 "src/parser/yacc.tab.cpp"
    break;

  case 30: /* dml: selectStmt  */
#line 211 "src/parser/yacc.y"
    {
        (yyval.sv_node) = (yyvsp[0].sv_node);
    }
#line 2009 "src/parser/yacc.tab.cpp"
    break;

  case 31: /* selectStmt: SELECT selector FROM fromClause optWhereClause opt_group_by opt_having opt_order_clause opt_limit  */
#line 218 "src/parser/yacc.y"
    {
        auto sel = std::make_shared<SelectStmt>((yyvsp[-7].sv_cols), (yyvsp[-5].sv_from)->tabs, std::move((yyvsp[-5].sv_from)->on_conds), (yyvsp[-4].sv_conds), (yyvsp[-1].sv_orderby));
        sel->group_by_cols = (yyvsp[-3].sv_cols);
        sel->having_conds = (yyvsp[-2].sv_conds);
        sel->limit = (yyvsp[0].sv_int);
        (yyval.sv_node) = sel;
    }
#line 2021 "src/parser/yacc.tab.cpp"
    break;

  case 32: /* explainStmt: EXPLAIN ANALYZE selectStmt  */
#line 229 "src/parser/yacc.y"
    {
        auto sel = std::dynamic_pointer_cast<SelectStmt>((yyvsp[0].sv_node));
        (yyval.sv_node) = std::make_shared<ExplainStmt>(sel);
    }
#line 2030 "src/parser/yacc.tab.cpp"
    break;

  case 33: /* tableRef: tbName aliasOpt  */
#line 237 "src/parser/yacc.y"
    {
        (yyval.sv_tab_ref) = std::make_shared<TableRef>((yyvsp[-1].sv_str), (yyvsp[0].sv_str));
    }
#line 2038 "src/parser/yacc.tab.cpp"
    break;

  case 34: /* tableRef: '(' unionExpr ')' aliasOpt  */
#line 241 "src/parser/yacc.y"
    {
        // 派生 UNION 表：用空 tab_name 作为占位，alias 走正常路径
        (yyval.sv_tab_ref) = std::make_shared<TableRef>(std::string(), (yyvsp[0].sv_str));
        (yyval.sv_tab_ref)->derived = (yyvsp[-2].sv_union);
    }
#line 2048 "src/parser/yacc.tab.cpp"
    break;

  case 35: /* unionExpr: selectStmt UNION selectStmt  */
#line 250 "src/parser/yacc.y"
    {
        auto u = std::make_shared<UnionStmt>();
        auto s1 = std::dynamic_pointer_cast<SelectStmt>((yyvsp[-2].sv_node));
        auto s2 = std::dynamic_pointer_cast<SelectStmt>((yyvsp[0].sv_node));
        u->branches.push_back(s1);
        u->branches.push_back(s2);
        (yyval.sv_union) = u;
    }
#line 2061 "src/parser/yacc.tab.cpp"
    break;

  case 36: /* unionExpr: unionExpr UNION selectStmt  */
#line 259 "src/parser/yacc.y"
    {
        auto s = std::dynamic_pointer_cast<SelectStmt>((yyvsp[0].sv_node));
        (yyvsp[-2].sv_union)->branches.push_back(s);
        (yyval.sv_union) = (yyvsp[-2].sv_union);
    }
#line 2071 "src/parser/yacc.tab.cpp"
    break;

  case 37: /* aliasOpt: %empty  */
#line 267 "src/parser/yacc.y"
                      { (yyval.sv_str) = std::string(); }
#line 2077 "src/parser/yacc.tab.cpp"
    break;

  case 38: /* aliasOpt: IDENTIFIER  */
#line 268 "src/parser/yacc.y"
                   { (yyval.sv_str) = (yyvsp[0].sv_str); }
#line 2083 "src/parser/yacc.tab.cpp"
    break;

  case 39: /* aliasOpt: AS IDENTIFIER  */
#line 269 "src/parser/yacc.y"
                      { (yyval.sv_str) = (yyvsp[0].sv_str); }
#line 2089 "src/parser/yacc.tab.cpp"
    break;

  case 40: /* fromClause: tableRef  */
#line 274 "src/parser/yacc.y"
    {
        (yyval.sv_from) = std::make_shared<FromClause>();
        (yyval.sv_from)->tabs.push_back((yyvsp[0].sv_tab_ref));
    }
#line 2098 "src/parser/yacc.tab.cpp"
    break;

  case 41: /* fromClause: fromClause ',' tableRef  */
#line 279 "src/parser/yacc.y"
    {
        (yyval.sv_from) = (yyvsp[-2].sv_from);
        (yyval.sv_from)->tabs.push_back((yyvsp[0].sv_tab_ref));
    }
#line 2107 "src/parser/yacc.tab.cpp"
    break;

  case 42: /* fromClause: fromClause JOIN tableRef onClauseOpt  */
#line 284 "src/parser/yacc.y"
    {
        (yyval.sv_from) = (yyvsp[-3].sv_from);
        (yyval.sv_from)->tabs.push_back((yyvsp[-1].sv_tab_ref));
        for (auto& c : (yyvsp[0].sv_conds)) {
            (yyval.sv_from)->on_conds.push_back(c);
        }
    }
#line 2119 "src/parser/yacc.tab.cpp"
    break;

  case 43: /* onClauseOpt: %empty  */
#line 294 "src/parser/yacc.y"
                      { /* ignore */ }
#line 2125 "src/parser/yacc.tab.cpp"
    break;

  case 44: /* onClauseOpt: ON whereClause  */
#line 296 "src/parser/yacc.y"
    {
        (yyval.sv_conds) = (yyvsp[0].sv_conds);
    }
#line 2133 "src/parser/yacc.tab.cpp"
    break;

  case 45: /* fieldList: field  */
#line 303 "src/parser/yacc.y"
    {
        (yyval.sv_fields) = std::vector<std::shared_ptr<Field>>{(yyvsp[0].sv_field)};
    }
#line 2141 "src/parser/yacc.tab.cpp"
    break;

  case 46: /* fieldList: fieldList ',' field  */
#line 307 "src/parser/yacc.y"
    {
        (yyval.sv_fields).push_back((yyvsp[0].sv_field));
    }
#line 2149 "src/parser/yacc.tab.cpp"
    break;

  case 47: /* colNameList: colName  */
#line 314 "src/parser/yacc.y"
    {
        (yyval.sv_strs) = std::vector<std::string>{(yyvsp[0].sv_str)};
    }
#line 2157 "src/parser/yacc.tab.cpp"
    break;

  case 48: /* colNameList: colNameList ',' colName  */
#line 318 "src/parser/yacc.y"
    {
        (yyval.sv_strs).push_back((yyvsp[0].sv_str));
    }
#line 2165 "src/parser/yacc.tab.cpp"
    break;

  case 49: /* field: colName type  */
#line 325 "src/parser/yacc.y"
    {
        (yyval.sv_field) = std::make_shared<ColDef>((yyvsp[-1].sv_str), (yyvsp[0].sv_type_len));
    }
#line 2173 "src/parser/yacc.tab.cpp"
    break;

  case 50: /* type: INT  */
#line 332 "src/parser/yacc.y"
    {
        (yyval.sv_type_len) = std::make_shared<TypeLen>(SV_TYPE_INT, sizeof(int));
    }
#line 2181 "src/parser/yacc.tab.cpp"
    break;

  case 51: /* type: CHAR '(' VALUE_INT ')'  */
#line 336 "src/parser/yacc.y"
    {
        (yyval.sv_type_len) = std::make_shared<TypeLen>(SV_TYPE_STRING, (yyvsp[-1].sv_int));
    }
#line 2189 "src/parser/yacc.tab.cpp"
    break;

  case 52: /* type: FLOAT  */
#line 340 "src/parser/yacc.y"
    {
        (yyval.sv_type_len) = std::make_shared<TypeLen>(SV_TYPE_FLOAT, sizeof(float));
    }
#line 2197 "src/parser/yacc.tab.cpp"
    break;

  case 53: /* type: DATETIME  */
#line 344 "src/parser/yacc.y"
    {
        (yyval.sv_type_len) = std::make_shared<TypeLen>(SV_TYPE_STRING, 20);
    }
#line 2205 "src/parser/yacc.tab.cpp"
    break;

  case 54: /* valueList: value  */
#line 351 "src/parser/yacc.y"
    {
        (yyval.sv_vals) = std::vector<std::shared_ptr<Value>>{(yyvsp[0].sv_val)};
    }
#line 2213 "src/parser/yacc.tab.cpp"
    break;

  case 55: /* valueList: valueList ',' value  */
#line 355 "src/parser/yacc.y"
    {
        (yyval.sv_vals).push_back((yyvsp[0].sv_val));
    }
#line 2221 "src/parser/yacc.tab.cpp"
    break;

  case 56: /* value: VALUE_INT  */
#line 362 "src/parser/yacc.y"
    {
        (yyval.sv_val) = std::make_shared<IntLit>((yyvsp[0].sv_int), std::to_string((yyvsp[0].sv_int)));
    }
#line 2229 "src/parser/yacc.tab.cpp"
    break;

  case 57: /* value: VALUE_FLOAT  */
#line 366 "src/parser/yacc.y"
    {
        (yyval.sv_val) = std::make_shared<FloatLit>((yyvsp[0].sv_float), (yyvsp[0].sv_str));
    }
#line 2237 "src/parser/yacc.tab.cpp"
    break;

  case 58: /* value: VALUE_STRING  */
#line 370 "src/parser/yacc.y"
    {
        (yyval.sv_val) = std::make_shared<StringLit>((yyvsp[0].sv_str), std::string("'") + (yyvsp[0].sv_str) + "'");
    }
#line 2245 "src/parser/yacc.tab.cpp"
    break;

  case 59: /* value: VALUE_BOOL  */
#line 374 "src/parser/yacc.y"
    {
        (yyval.sv_val) = std::make_shared<BoolLit>((yyvsp[0].sv_bool));
    }
#line 2253 "src/parser/yacc.tab.cpp"
    break;

  case 60: /* value: PARAMETER  */
#line 378 "src/parser/yacc.y"
    {
        (yyval.sv_val) = std::make_shared<ParamRef>((yyvsp[0].sv_str));
    }
#line 2261 "src/parser/yacc.tab.cpp"
    break;

  case 61: /* condition: havingExpr op expr  */
#line 385 "src/parser/yacc.y"
    {
        (yyval.sv_cond) = std::make_shared<BinaryExpr>((yyvsp[-2].sv_col), (yyvsp[-1].sv_comp_op), (yyvsp[0].sv_expr));
    }
#line 2269 "src/parser/yacc.tab.cpp"
    break;

  case 62: /* optWhereClause: %empty  */
#line 391 "src/parser/yacc.y"
                      { /* ignore*/ }
#line 2275 "src/parser/yacc.tab.cpp"
    break;

  case 63: /* optWhereClause: WHERE whereClause  */
#line 393 "src/parser/yacc.y"
    {
        (yyval.sv_conds) = (yyvsp[0].sv_conds);
    }
#line 2283 "src/parser/yacc.tab.cpp"
    break;

  case 64: /* whereClause: condition  */
#line 400 "src/parser/yacc.y"
    {
        (yyval.sv_conds) = std::vector<std::shared_ptr<BinaryExpr>>{(yyvsp[0].sv_cond)};
    }
#line 2291 "src/parser/yacc.tab.cpp"
    break;

  case 65: /* whereClause: whereClause AND condition  */
#line 404 "src/parser/yacc.y"
    {
        (yyval.sv_conds).push_back((yyvsp[0].sv_cond));
    }
#line 2299 "src/parser/yacc.tab.cpp"
    break;

  case 66: /* col: tbName '.' colName  */
#line 411 "src/parser/yacc.y"
    {
        (yyval.sv_col) = std::make_shared<Col>((yyvsp[-2].sv_str), (yyvsp[0].sv_str));
    }
#line 2307 "src/parser/yacc.tab.cpp"
    break;

  case 67: /* col: colName  */
#line 415 "src/parser/yacc.y"
    {
        (yyval.sv_col) = std::make_shared<Col>("", (yyvsp[0].sv_str));
    }
#line 2315 "src/parser/yacc.tab.cpp"
    break;

  case 68: /* colList: col  */
#line 422 "src/parser/yacc.y"
    {
        (yyval.sv_cols) = std::vector<std::shared_ptr<Col>>{(yyvsp[0].sv_col)};
    }
#line 2323 "src/parser/yacc.tab.cpp"
    break;

  case 69: /* colList: colList ',' col  */
#line 426 "src/parser/yacc.y"
    {
        (yyval.sv_cols).push_back((yyvsp[0].sv_col));
    }
#line 2331 "src/parser/yacc.tab.cpp"
    break;

  case 70: /* op: '='  */
#line 433 "src/parser/yacc.y"
    {
        (yyval.sv_comp_op) = SV_OP_EQ;
    }
#line 2339 "src/parser/yacc.tab.cpp"
    break;

  case 71: /* op: '<'  */
#line 437 "src/parser/yacc.y"
    {
        (yyval.sv_comp_op) = SV_OP_LT;
    }
#line 2347 "src/parser/yacc.tab.cpp"
    break;

  case 72: /* op: '>'  */
#line 441 "src/parser/yacc.y"
    {
        (yyval.sv_comp_op) = SV_OP_GT;
    }
#line 2355 "src/parser/yacc.tab.cpp"
    break;

  case 73: /* op: NEQ  */
#line 445 "src/parser/yacc.y"
    {
        (yyval.sv_comp_op) = SV_OP_NE;
    }
#line 2363 "src/parser/yacc.tab.cpp"
    break;

  case 74: /* op: LEQ  */
#line 449 "src/parser/yacc.y"
    {
        (yyval.sv_comp_op) = SV_OP_LE;
    }
#line 2371 "src/parser/yacc.tab.cpp"
    break;

  case 75: /* op: GEQ  */
#line 453 "src/parser/yacc.y"
    {
        (yyval.sv_comp_op) = SV_OP_GE;
    }
#line 2379 "src/parser/yacc.tab.cpp"
    break;

  case 76: /* expr: value  */
#line 460 "src/parser/yacc.y"
    {
        (yyval.sv_expr) = std::static_pointer_cast<Expr>((yyvsp[0].sv_val));
    }
#line 2387 "src/parser/yacc.tab.cpp"
    break;

  case 77: /* expr: col  */
#line 464 "src/parser/yacc.y"
    {
        (yyval.sv_expr) = std::static_pointer_cast<Expr>((yyvsp[0].sv_col));
    }
#line 2395 "src/parser/yacc.tab.cpp"
    break;

  case 78: /* setClauses: setClause  */
#line 471 "src/parser/yacc.y"
    {
        (yyval.sv_set_clauses) = std::vector<std::shared_ptr<SetClause>>{(yyvsp[0].sv_set_clause)};
    }
#line 2403 "src/parser/yacc.tab.cpp"
    break;

  case 79: /* setClauses: setClauses ',' setClause  */
#line 475 "src/parser/yacc.y"
    {
        (yyval.sv_set_clauses).push_back((yyvsp[0].sv_set_clause));
    }
#line 2411 "src/parser/yacc.tab.cpp"
    break;

  case 80: /* setClause: colName '=' value  */
#line 482 "src/parser/yacc.y"
    {
        (yyval.sv_set_clause) = std::make_shared<SetClause>((yyvsp[-2].sv_str), (yyvsp[0].sv_val));
    }
#line 2419 "src/parser/yacc.tab.cpp"
    break;

  case 81: /* setClause: colName '=' colName  */
#line 486 "src/parser/yacc.y"
    {
        (yyval.sv_set_clause) = std::make_shared<SetClause>((yyvsp[-2].sv_str), (yyvsp[0].sv_str));
    }
#line 2427 "src/parser/yacc.tab.cpp"
    break;

  case 82: /* setClause: arithmeticSetClause  */
#line 490 "src/parser/yacc.y"
    {
        (yyval.sv_set_clause) = (yyvsp[0].sv_set_clause);
    }
#line 2435 "src/parser/yacc.tab.cpp"
    break;

  case 83: /* arithmeticSetClause: colName '=' colName '+' value  */
#line 497 "src/parser/yacc.y"
    {
        (yyval.sv_set_clause) = std::make_shared<SetClause>((yyvsp[-4].sv_str), (yyvsp[-2].sv_str), '+', (yyvsp[0].sv_val));
    }
#line 2443 "src/parser/yacc.tab.cpp"
    break;

  case 84: /* arithmeticSetClause: colName '=' colName '-' value  */
#line 501 "src/parser/yacc.y"
    {
        (yyval.sv_set_clause) = std::make_shared<SetClause>((yyvsp[-4].sv_str), (yyvsp[-2].sv_str), '-', (yyvsp[0].sv_val));
    }
#line 2451 "src/parser/yacc.tab.cpp"
    break;

  case 85: /* arithmeticSetClause: colName '=' colName '*' value  */
#line 505 "src/parser/yacc.y"
    {
        (yyval.sv_set_clause) = std::make_shared<SetClause>((yyvsp[-4].sv_str), (yyvsp[-2].sv_str), '*', (yyvsp[0].sv_val));
    }
#line 2459 "src/parser/yacc.tab.cpp"
    break;

  case 86: /* arithmeticSetClause: colName '=' colName VALUE_INT  */
#line 509 "src/parser/yacc.y"
    {
        // 词法把紧贴的 +N/-N 贪婪切成带符号整型字面量（{sign}?{digit}+），
        // 导致 set col=col2+1 / col=col2-1 走不到上面的 '+'/'-' 规则而报
        // syntax error。把带符号字面量落地为
        // col = col2 + (±N)，语义等价。
        if ((yyvsp[0].sv_str).empty() ||
            ((yyvsp[0].sv_str).front() != '+' && (yyvsp[0].sv_str).front() != '-')) {
            yyerror(&(yylsp[0]), scanner, parse_result,
                    "missing arithmetic operator before integer literal");
            YYERROR;
        }
        (yyval.sv_set_clause) = std::make_shared<SetClause>((yyvsp[-3].sv_str), (yyvsp[-1].sv_str), '+',
                 std::make_shared<IntLit>((yyvsp[0].sv_int), (yyvsp[0].sv_str)));
    }
#line 2478 "src/parser/yacc.tab.cpp"
    break;

  case 87: /* arithmeticSetClause: colName '=' colName VALUE_FLOAT  */
#line 524 "src/parser/yacc.y"
    {
        if ((yyvsp[0].sv_str).empty() ||
            ((yyvsp[0].sv_str).front() != '+' && (yyvsp[0].sv_str).front() != '-')) {
            yyerror(&(yylsp[0]), scanner, parse_result,
                    "missing arithmetic operator before float literal");
            YYERROR;
        }
        (yyval.sv_set_clause) = std::make_shared<SetClause>((yyvsp[-3].sv_str), (yyvsp[-1].sv_str), '+',
                 std::make_shared<FloatLit>((yyvsp[0].sv_float), (yyvsp[0].sv_str)));
    }
#line 2493 "src/parser/yacc.tab.cpp"
    break;

  case 88: /* arithmeticSetClause: arithmeticSetClause '+' value  */
#line 535 "src/parser/yacc.y"
    {
        (yyval.sv_set_clause) = (yyvsp[-2].sv_set_clause);
        (yyval.sv_set_clause)->append_term('+', (yyvsp[0].sv_val));
    }
#line 2502 "src/parser/yacc.tab.cpp"
    break;

  case 89: /* arithmeticSetClause: arithmeticSetClause '-' value  */
#line 540 "src/parser/yacc.y"
    {
        (yyval.sv_set_clause) = (yyvsp[-2].sv_set_clause);
        (yyval.sv_set_clause)->append_term('-', (yyvsp[0].sv_val));
    }
#line 2511 "src/parser/yacc.tab.cpp"
    break;

  case 90: /* arithmeticSetClause: arithmeticSetClause VALUE_INT  */
#line 545 "src/parser/yacc.y"
    {
        // 无空格的后续 +N/-N 同样会被词法器合并成带符号字面量。
        // 统一记录成“加上带符号值”，与显式 '+'/'-' 规则等价。
        if ((yyvsp[0].sv_str).empty() ||
            ((yyvsp[0].sv_str).front() != '+' && (yyvsp[0].sv_str).front() != '-')) {
            yyerror(&(yylsp[0]), scanner, parse_result,
                    "missing arithmetic operator before integer literal");
            YYERROR;
        }
        (yyval.sv_set_clause) = (yyvsp[-1].sv_set_clause);
        (yyval.sv_set_clause)->append_term('+',
            std::make_shared<IntLit>((yyvsp[0].sv_int), (yyvsp[0].sv_str)));
    }
#line 2529 "src/parser/yacc.tab.cpp"
    break;

  case 91: /* arithmeticSetClause: arithmeticSetClause VALUE_FLOAT  */
#line 559 "src/parser/yacc.y"
    {
        if ((yyvsp[0].sv_str).empty() ||
            ((yyvsp[0].sv_str).front() != '+' && (yyvsp[0].sv_str).front() != '-')) {
            yyerror(&(yylsp[0]), scanner, parse_result,
                    "missing arithmetic operator before float literal");
            YYERROR;
        }
        (yyval.sv_set_clause) = (yyvsp[-1].sv_set_clause);
        (yyval.sv_set_clause)->append_term('+',
            std::make_shared<FloatLit>((yyvsp[0].sv_float), (yyvsp[0].sv_str)));
    }
#line 2545 "src/parser/yacc.tab.cpp"
    break;

  case 92: /* selector: '*'  */
#line 574 "src/parser/yacc.y"
    {
        (yyval.sv_cols) = {};
    }
#line 2553 "src/parser/yacc.tab.cpp"
    break;

  case 94: /* selectList: selectItem  */
#line 582 "src/parser/yacc.y"
    {
        (yyval.sv_cols) = std::vector<std::shared_ptr<Col>>{(yyvsp[0].sv_col)};
    }
#line 2561 "src/parser/yacc.tab.cpp"
    break;

  case 95: /* selectList: selectList ',' selectItem  */
#line 586 "src/parser/yacc.y"
    {
        (yyval.sv_cols).push_back((yyvsp[0].sv_col));
    }
#line 2569 "src/parser/yacc.tab.cpp"
    break;

  case 96: /* selectItem: col aliasOpt  */
#line 593 "src/parser/yacc.y"
    {
        (yyvsp[-1].sv_col)->alias = (yyvsp[0].sv_str);
        (yyval.sv_col) = (yyvsp[-1].sv_col);
    }
#line 2578 "src/parser/yacc.tab.cpp"
    break;

  case 97: /* selectItem: COUNT '(' '*' ')' aliasOpt  */
#line 598 "src/parser/yacc.y"
    {
        auto c = std::make_shared<Col>(std::string(), std::string("*"));
        c->agg_type = SV_AGG_COUNT;
        c->is_star = true;
        c->alias = (yyvsp[0].sv_str);
        (yyval.sv_col) = c;
    }
#line 2590 "src/parser/yacc.tab.cpp"
    break;

  case 98: /* selectItem: COUNT '(' col ')' aliasOpt  */
#line 606 "src/parser/yacc.y"
    {
        (yyvsp[-2].sv_col)->agg_type = SV_AGG_COUNT;
        (yyvsp[-2].sv_col)->alias = (yyvsp[0].sv_str);
        (yyval.sv_col) = (yyvsp[-2].sv_col);
    }
#line 2600 "src/parser/yacc.tab.cpp"
    break;

  case 99: /* selectItem: COUNT '(' DISTINCT col ')' aliasOpt  */
#line 612 "src/parser/yacc.y"
    {
        (yyvsp[-2].sv_col)->agg_type = SV_AGG_COUNT;
        (yyvsp[-2].sv_col)->is_distinct = true;
        (yyvsp[-2].sv_col)->alias = (yyvsp[0].sv_str);
        (yyval.sv_col) = (yyvsp[-2].sv_col);
    }
#line 2611 "src/parser/yacc.tab.cpp"
    break;

  case 100: /* selectItem: COUNT '(' DISTINCT '(' col ')' ')' aliasOpt  */
#line 619 "src/parser/yacc.y"
    {
        (yyvsp[-3].sv_col)->agg_type = SV_AGG_COUNT;
        (yyvsp[-3].sv_col)->is_distinct = true;
        (yyvsp[-3].sv_col)->alias = (yyvsp[0].sv_str);
        (yyval.sv_col) = (yyvsp[-3].sv_col);
    }
#line 2622 "src/parser/yacc.tab.cpp"
    break;

  case 101: /* selectItem: MAX_TOK '(' col ')' aliasOpt  */
#line 626 "src/parser/yacc.y"
    {
        (yyvsp[-2].sv_col)->agg_type = SV_AGG_MAX;
        (yyvsp[-2].sv_col)->alias = (yyvsp[0].sv_str);
        (yyval.sv_col) = (yyvsp[-2].sv_col);
    }
#line 2632 "src/parser/yacc.tab.cpp"
    break;

  case 102: /* selectItem: MIN_TOK '(' col ')' aliasOpt  */
#line 632 "src/parser/yacc.y"
    {
        (yyvsp[-2].sv_col)->agg_type = SV_AGG_MIN;
        (yyvsp[-2].sv_col)->alias = (yyvsp[0].sv_str);
        (yyval.sv_col) = (yyvsp[-2].sv_col);
    }
#line 2642 "src/parser/yacc.tab.cpp"
    break;

  case 103: /* selectItem: SUM_TOK '(' col ')' aliasOpt  */
#line 638 "src/parser/yacc.y"
    {
        (yyvsp[-2].sv_col)->agg_type = SV_AGG_SUM;
        (yyvsp[-2].sv_col)->alias = (yyvsp[0].sv_str);
        (yyval.sv_col) = (yyvsp[-2].sv_col);
    }
#line 2652 "src/parser/yacc.tab.cpp"
    break;

  case 104: /* selectItem: AVG_TOK '(' col ')' aliasOpt  */
#line 644 "src/parser/yacc.y"
    {
        (yyvsp[-2].sv_col)->agg_type = SV_AGG_AVG;
        (yyvsp[-2].sv_col)->alias = (yyvsp[0].sv_str);
        (yyval.sv_col) = (yyvsp[-2].sv_col);
    }
#line 2662 "src/parser/yacc.tab.cpp"
    break;

  case 105: /* opt_group_by: %empty  */
#line 652 "src/parser/yacc.y"
                      { (yyval.sv_cols) = std::vector<std::shared_ptr<Col>>(); }
#line 2668 "src/parser/yacc.tab.cpp"
    break;

  case 106: /* opt_group_by: GROUP BY colList  */
#line 653 "src/parser/yacc.y"
                         { (yyval.sv_cols) = (yyvsp[0].sv_cols); }
#line 2674 "src/parser/yacc.tab.cpp"
    break;

  case 107: /* opt_having: %empty  */
#line 657 "src/parser/yacc.y"
                      { (yyval.sv_conds) = std::vector<std::shared_ptr<BinaryExpr>>(); }
#line 2680 "src/parser/yacc.tab.cpp"
    break;

  case 108: /* opt_having: HAVING havingClause  */
#line 658 "src/parser/yacc.y"
                            { (yyval.sv_conds) = (yyvsp[0].sv_conds); }
#line 2686 "src/parser/yacc.tab.cpp"
    break;

  case 109: /* havingClause: condition  */
#line 663 "src/parser/yacc.y"
    {
        (yyval.sv_conds) = std::vector<std::shared_ptr<BinaryExpr>>{(yyvsp[0].sv_cond)};
    }
#line 2694 "src/parser/yacc.tab.cpp"
    break;

  case 110: /* havingClause: havingClause AND condition  */
#line 667 "src/parser/yacc.y"
    {
        (yyval.sv_conds).push_back((yyvsp[0].sv_cond));
    }
#line 2702 "src/parser/yacc.tab.cpp"
    break;

  case 111: /* havingExpr: col  */
#line 673 "src/parser/yacc.y"
            { (yyval.sv_col) = (yyvsp[0].sv_col); }
#line 2708 "src/parser/yacc.tab.cpp"
    break;

  case 112: /* havingExpr: COUNT '(' '*' ')'  */
#line 675 "src/parser/yacc.y"
    {
        auto c = std::make_shared<Col>(std::string(), std::string("*"));
        c->agg_type = SV_AGG_COUNT;
        c->is_star = true;
        (yyval.sv_col) = c;
    }
#line 2719 "src/parser/yacc.tab.cpp"
    break;

  case 113: /* havingExpr: COUNT '(' col ')'  */
#line 682 "src/parser/yacc.y"
    {
        (yyvsp[-1].sv_col)->agg_type = SV_AGG_COUNT;
        (yyval.sv_col) = (yyvsp[-1].sv_col);
    }
#line 2728 "src/parser/yacc.tab.cpp"
    break;

  case 114: /* havingExpr: COUNT '(' DISTINCT col ')'  */
#line 687 "src/parser/yacc.y"
    {
        (yyvsp[-1].sv_col)->agg_type = SV_AGG_COUNT;
        (yyvsp[-1].sv_col)->is_distinct = true;
        (yyval.sv_col) = (yyvsp[-1].sv_col);
    }
#line 2738 "src/parser/yacc.tab.cpp"
    break;

  case 115: /* havingExpr: COUNT '(' DISTINCT '(' col ')' ')'  */
#line 693 "src/parser/yacc.y"
    {
        (yyvsp[-2].sv_col)->agg_type = SV_AGG_COUNT;
        (yyvsp[-2].sv_col)->is_distinct = true;
        (yyval.sv_col) = (yyvsp[-2].sv_col);
    }
#line 2748 "src/parser/yacc.tab.cpp"
    break;

  case 116: /* havingExpr: MAX_TOK '(' col ')'  */
#line 699 "src/parser/yacc.y"
    {
        (yyvsp[-1].sv_col)->agg_type = SV_AGG_MAX;
        (yyval.sv_col) = (yyvsp[-1].sv_col);
    }
#line 2757 "src/parser/yacc.tab.cpp"
    break;

  case 117: /* havingExpr: MIN_TOK '(' col ')'  */
#line 704 "src/parser/yacc.y"
    {
        (yyvsp[-1].sv_col)->agg_type = SV_AGG_MIN;
        (yyval.sv_col) = (yyvsp[-1].sv_col);
    }
#line 2766 "src/parser/yacc.tab.cpp"
    break;

  case 118: /* havingExpr: SUM_TOK '(' col ')'  */
#line 709 "src/parser/yacc.y"
    {
        (yyvsp[-1].sv_col)->agg_type = SV_AGG_SUM;
        (yyval.sv_col) = (yyvsp[-1].sv_col);
    }
#line 2775 "src/parser/yacc.tab.cpp"
    break;

  case 119: /* havingExpr: AVG_TOK '(' col ')'  */
#line 714 "src/parser/yacc.y"
    {
        (yyvsp[-1].sv_col)->agg_type = SV_AGG_AVG;
        (yyval.sv_col) = (yyvsp[-1].sv_col);
    }
#line 2784 "src/parser/yacc.tab.cpp"
    break;

  case 120: /* opt_limit: %empty  */
#line 721 "src/parser/yacc.y"
                      { (yyval.sv_int) = -1; }
#line 2790 "src/parser/yacc.tab.cpp"
    break;

  case 121: /* opt_limit: LIMIT VALUE_INT  */
#line 722 "src/parser/yacc.y"
                        { (yyval.sv_int) = (yyvsp[0].sv_int); }
#line 2796 "src/parser/yacc.tab.cpp"
    break;

  case 122: /* opt_order_clause: ORDER BY order_clause  */
#line 727 "src/parser/yacc.y"
    {
        (yyval.sv_orderby) = (yyvsp[0].sv_orderby);
    }
#line 2804 "src/parser/yacc.tab.cpp"
    break;

  case 123: /* opt_order_clause: %empty  */
#line 730 "src/parser/yacc.y"
                      { /* ignore*/ }
#line 2810 "src/parser/yacc.tab.cpp"
    break;

  case 124: /* order_clause: order_item_list  */
#line 735 "src/parser/yacc.y"
    {
        auto ob = std::make_shared<OrderBy>();
        ob->items = (yyvsp[0].sv_orderby_items);
        (yyval.sv_orderby) = ob;
    }
#line 2820 "src/parser/yacc.tab.cpp"
    break;

  case 125: /* order_item_list: order_item  */
#line 744 "src/parser/yacc.y"
    {
        (yyval.sv_orderby_items) = std::vector<std::shared_ptr<OrderByItem>>{(yyvsp[0].sv_orderby_item)};
    }
#line 2828 "src/parser/yacc.tab.cpp"
    break;

  case 126: /* order_item_list: order_item_list ',' order_item  */
#line 748 "src/parser/yacc.y"
    {
        (yyval.sv_orderby_items).push_back((yyvsp[0].sv_orderby_item));
    }
#line 2836 "src/parser/yacc.tab.cpp"
    break;

  case 127: /* order_item: col opt_asc_desc  */
#line 755 "src/parser/yacc.y"
    {
        (yyval.sv_orderby_item) = std::make_shared<OrderByItem>((yyvsp[-1].sv_col), (yyvsp[0].sv_orderby_dir));
    }
#line 2844 "src/parser/yacc.tab.cpp"
    break;

  case 128: /* order_item: COUNT '(' '*' ')' opt_asc_desc  */
#line 759 "src/parser/yacc.y"
    {
        auto c = std::make_shared<Col>(std::string(), std::string("*"));
        c->agg_type = SV_AGG_COUNT;
        c->is_star = true;
        (yyval.sv_orderby_item) = std::make_shared<OrderByItem>(c, (yyvsp[0].sv_orderby_dir));
    }
#line 2855 "src/parser/yacc.tab.cpp"
    break;

  case 129: /* order_item: COUNT '(' col ')' opt_asc_desc  */
#line 766 "src/parser/yacc.y"
    {
        (yyvsp[-2].sv_col)->agg_type = SV_AGG_COUNT;
        (yyval.sv_orderby_item) = std::make_shared<OrderByItem>((yyvsp[-2].sv_col), (yyvsp[0].sv_orderby_dir));
    }
#line 2864 "src/parser/yacc.tab.cpp"
    break;

  case 130: /* order_item: COUNT '(' DISTINCT col ')' opt_asc_desc  */
#line 771 "src/parser/yacc.y"
    {
        (yyvsp[-2].sv_col)->agg_type = SV_AGG_COUNT;
        (yyvsp[-2].sv_col)->is_distinct = true;
        (yyval.sv_orderby_item) = std::make_shared<OrderByItem>((yyvsp[-2].sv_col), (yyvsp[0].sv_orderby_dir));
    }
#line 2874 "src/parser/yacc.tab.cpp"
    break;

  case 131: /* order_item: COUNT '(' DISTINCT '(' col ')' ')' opt_asc_desc  */
#line 777 "src/parser/yacc.y"
    {
        (yyvsp[-3].sv_col)->agg_type = SV_AGG_COUNT;
        (yyvsp[-3].sv_col)->is_distinct = true;
        (yyval.sv_orderby_item) = std::make_shared<OrderByItem>((yyvsp[-3].sv_col), (yyvsp[0].sv_orderby_dir));
    }
#line 2884 "src/parser/yacc.tab.cpp"
    break;

  case 132: /* order_item: MAX_TOK '(' col ')' opt_asc_desc  */
#line 783 "src/parser/yacc.y"
    {
        (yyvsp[-2].sv_col)->agg_type = SV_AGG_MAX;
        (yyval.sv_orderby_item) = std::make_shared<OrderByItem>((yyvsp[-2].sv_col), (yyvsp[0].sv_orderby_dir));
    }
#line 2893 "src/parser/yacc.tab.cpp"
    break;

  case 133: /* order_item: MIN_TOK '(' col ')' opt_asc_desc  */
#line 788 "src/parser/yacc.y"
    {
        (yyvsp[-2].sv_col)->agg_type = SV_AGG_MIN;
        (yyval.sv_orderby_item) = std::make_shared<OrderByItem>((yyvsp[-2].sv_col), (yyvsp[0].sv_orderby_dir));
    }
#line 2902 "src/parser/yacc.tab.cpp"
    break;

  case 134: /* order_item: SUM_TOK '(' col ')' opt_asc_desc  */
#line 793 "src/parser/yacc.y"
    {
        (yyvsp[-2].sv_col)->agg_type = SV_AGG_SUM;
        (yyval.sv_orderby_item) = std::make_shared<OrderByItem>((yyvsp[-2].sv_col), (yyvsp[0].sv_orderby_dir));
    }
#line 2911 "src/parser/yacc.tab.cpp"
    break;

  case 135: /* order_item: AVG_TOK '(' col ')' opt_asc_desc  */
#line 798 "src/parser/yacc.y"
    {
        (yyvsp[-2].sv_col)->agg_type = SV_AGG_AVG;
        (yyval.sv_orderby_item) = std::make_shared<OrderByItem>((yyvsp[-2].sv_col), (yyvsp[0].sv_orderby_dir));
    }
#line 2920 "src/parser/yacc.tab.cpp"
    break;

  case 136: /* opt_asc_desc: ASC  */
#line 805 "src/parser/yacc.y"
                 { (yyval.sv_orderby_dir) = OrderBy_ASC;     }
#line 2926 "src/parser/yacc.tab.cpp"
    break;

  case 137: /* opt_asc_desc: DESC  */
#line 806 "src/parser/yacc.y"
                 { (yyval.sv_orderby_dir) = OrderBy_DESC;    }
#line 2932 "src/parser/yacc.tab.cpp"
    break;

  case 138: /* opt_asc_desc: %empty  */
#line 807 "src/parser/yacc.y"
            { (yyval.sv_orderby_dir) = OrderBy_DEFAULT; }
#line 2938 "src/parser/yacc.tab.cpp"
    break;

  case 139: /* set_knob_type: ENABLE_NESTLOOP  */
#line 811 "src/parser/yacc.y"
                    { (yyval.sv_setKnobType) = EnableNestLoop; }
#line 2944 "src/parser/yacc.tab.cpp"
    break;

  case 140: /* set_knob_type: ENABLE_SORTMERGE  */
#line 812 "src/parser/yacc.y"
                         { (yyval.sv_setKnobType) = EnableSortMerge; }
#line 2950 "src/parser/yacc.tab.cpp"
    break;


#line 2954 "src/parser/yacc.tab.cpp"

      default: break;
    }
  /* User semantic actions sometimes alter yychar, and that requires
     that yytoken be updated with the new translation.  We take the
     approach of translating immediately before every use of yytoken.
     One alternative is translating here after every semantic action,
     but that translation would be missed if the semantic action invokes
     YYABORT, YYACCEPT, or YYERROR immediately after altering yychar or
     if it invokes YYBACKUP.  In the case of YYABORT or YYACCEPT, an
     incorrect destructor might then be invoked immediately.  In the
     case of YYERROR or YYBACKUP, subsequent parser actions might lead
     to an incorrect destructor call or verbose syntax error message
     before the lookahead is translated.  */
  YY_SYMBOL_PRINT ("-> $$ =", YY_CAST (yysymbol_kind_t, yyr1[yyn]), &yyval, &yyloc);

  YYPOPSTACK (yylen);
  yylen = 0;

  *++yyvsp = yyval;
  *++yylsp = yyloc;

  /* Now 'shift' the result of the reduction.  Determine what state
     that goes to, based on the state we popped back to and the rule
     number reduced by.  */
  {
    const int yylhs = yyr1[yyn] - YYNTOKENS;
    const int yyi = yypgoto[yylhs] + *yyssp;
    yystate = (0 <= yyi && yyi <= YYLAST && yycheck[yyi] == *yyssp
               ? yytable[yyi]
               : yydefgoto[yylhs]);
  }

  goto yynewstate;


/*--------------------------------------.
| yyerrlab -- here on detecting error.  |
`--------------------------------------*/
yyerrlab:
  /* Make sure we have latest lookahead translation.  See comments at
     user semantic actions for why this is necessary.  */
  yytoken = yychar == YYEMPTY ? YYSYMBOL_YYEMPTY : YYTRANSLATE (yychar);
  /* If not already recovering from an error, report this error.  */
  if (!yyerrstatus)
    {
      ++yynerrs;
      {
        yypcontext_t yyctx
          = {yyssp, yytoken, &yylloc};
        char const *yymsgp = YY_("syntax error");
        int yysyntax_error_status;
        yysyntax_error_status = yysyntax_error (&yymsg_alloc, &yymsg, &yyctx);
        if (yysyntax_error_status == 0)
          yymsgp = yymsg;
        else if (yysyntax_error_status == -1)
          {
            if (yymsg != yymsgbuf)
              YYSTACK_FREE (yymsg);
            yymsg = YY_CAST (char *,
                             YYSTACK_ALLOC (YY_CAST (YYSIZE_T, yymsg_alloc)));
            if (yymsg)
              {
                yysyntax_error_status
                  = yysyntax_error (&yymsg_alloc, &yymsg, &yyctx);
                yymsgp = yymsg;
              }
            else
              {
                yymsg = yymsgbuf;
                yymsg_alloc = sizeof yymsgbuf;
                yysyntax_error_status = YYENOMEM;
              }
          }
        yyerror (&yylloc, scanner, parse_result, yymsgp);
        if (yysyntax_error_status == YYENOMEM)
          YYNOMEM;
      }
    }

  yyerror_range[1] = yylloc;
  if (yyerrstatus == 3)
    {
      /* If just tried and failed to reuse lookahead token after an
         error, discard it.  */

      if (yychar <= YYEOF)
        {
          /* Return failure if at end of input.  */
          if (yychar == YYEOF)
            YYABORT;
        }
      else
        {
          yydestruct ("Error: discarding",
                      yytoken, &yylval, &yylloc, scanner, parse_result);
          yychar = YYEMPTY;
        }
    }

  /* Else will try to reuse lookahead token after shifting the error
     token.  */
  goto yyerrlab1;


/*---------------------------------------------------.
| yyerrorlab -- error raised explicitly by YYERROR.  |
`---------------------------------------------------*/
yyerrorlab:
  /* Pacify compilers when the user code never invokes YYERROR and the
     label yyerrorlab therefore never appears in user code.  */
  if (0)
    YYERROR;
  ++yynerrs;

  /* Do not reclaim the symbols of the rule whose action triggered
     this YYERROR.  */
  YYPOPSTACK (yylen);
  yylen = 0;
  YY_STACK_PRINT (yyss, yyssp);
  yystate = *yyssp;
  goto yyerrlab1;


/*-------------------------------------------------------------.
| yyerrlab1 -- common code for both syntax error and YYERROR.  |
`-------------------------------------------------------------*/
yyerrlab1:
  yyerrstatus = 3;      /* Each real token shifted decrements this.  */

  /* Pop stack until we find a state that shifts the error token.  */
  for (;;)
    {
      yyn = yypact[yystate];
      if (!yypact_value_is_default (yyn))
        {
          yyn += YYSYMBOL_YYerror;
          if (0 <= yyn && yyn <= YYLAST && yycheck[yyn] == YYSYMBOL_YYerror)
            {
              yyn = yytable[yyn];
              if (0 < yyn)
                break;
            }
        }

      /* Pop the current state because it cannot handle the error token.  */
      if (yyssp == yyss)
        YYABORT;

      yyerror_range[1] = *yylsp;
      yydestruct ("Error: popping",
                  YY_ACCESSING_SYMBOL (yystate), yyvsp, yylsp, scanner, parse_result);
      YYPOPSTACK (1);
      yystate = *yyssp;
      YY_STACK_PRINT (yyss, yyssp);
    }

  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  *++yyvsp = yylval;
  YY_IGNORE_MAYBE_UNINITIALIZED_END

  yyerror_range[2] = yylloc;
  ++yylsp;
  YYLLOC_DEFAULT (*yylsp, yyerror_range, 2);

  /* Shift the error token.  */
  YY_SYMBOL_PRINT ("Shifting", YY_ACCESSING_SYMBOL (yyn), yyvsp, yylsp);

  yystate = yyn;
  goto yynewstate;


/*-------------------------------------.
| yyacceptlab -- YYACCEPT comes here.  |
`-------------------------------------*/
yyacceptlab:
  yyresult = 0;
  goto yyreturnlab;


/*-----------------------------------.
| yyabortlab -- YYABORT comes here.  |
`-----------------------------------*/
yyabortlab:
  yyresult = 1;
  goto yyreturnlab;


/*-----------------------------------------------------------.
| yyexhaustedlab -- YYNOMEM (memory exhaustion) comes here.  |
`-----------------------------------------------------------*/
yyexhaustedlab:
  yyerror (&yylloc, scanner, parse_result, YY_("memory exhausted"));
  yyresult = 2;
  goto yyreturnlab;


/*----------------------------------------------------------.
| yyreturnlab -- parsing is finished, clean up and return.  |
`----------------------------------------------------------*/
yyreturnlab:
  if (yychar != YYEMPTY)
    {
      /* Make sure we have latest lookahead translation.  See comments at
         user semantic actions for why this is necessary.  */
      yytoken = YYTRANSLATE (yychar);
      yydestruct ("Cleanup: discarding lookahead",
                  yytoken, &yylval, &yylloc, scanner, parse_result);
    }
  /* Do not reclaim the symbols of the rule whose action triggered
     this YYABORT or YYACCEPT.  */
  YYPOPSTACK (yylen);
  YY_STACK_PRINT (yyss, yyssp);
  while (yyssp != yyss)
    {
      yydestruct ("Cleanup: popping",
                  YY_ACCESSING_SYMBOL (+*yyssp), yyvsp, yylsp, scanner, parse_result);
      YYPOPSTACK (1);
    }
#ifndef yyoverflow
  if (yyss != yyssa)
    YYSTACK_FREE (yyss);
#endif
  if (yymsg != yymsgbuf)
    YYSTACK_FREE (yymsg);
  return yyresult;
}

#line 818 "src/parser/yacc.y"

