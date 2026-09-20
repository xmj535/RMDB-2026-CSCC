/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */
#undef NDEBUG

#include <cassert>

#include "parser.h"

int main() {
    std::vector<std::string> sqls = {
        "show tables;",
        "desc tb;",
        "create table tb (a int, b float, c char(4));",
        "drop table tb;",
        "create index tb(a);",
        "create index tb(a, b, c);",
        "drop index tb(a, b, c);",
        "drop index tb(b);",
        "insert into tb values (1, 3.14, 'pi');",
        "insert into tb values ($1,$2,$3);",
        "delete from tb where a = 1;",
        "update tb set a = 1, b = 2.2, c = 'xyz' where x = 2 and y < 1.1 and z > 'abc';",
        "update tb set a=a where x=1;",
        "update tb set a=b, b=b+1, c=c where x=2;",
        "update tb set a=a - 1 + 91, b=b + 2 - 3 where x=2;",
        "update tb set a=a-1+91 where x=2;",
        "update tb set a=a+$1-$2 where x=$3;",
        "update tb set b=$1 where a=$2 and c='$3';",
        "select * from tb;",
        "select * from tb where x <> 2 and y != 3 and z >= 3. and b <= '123' and c < tb.a;",
        "select x.a, y.b from x, y where x.a = y.b and c = d;",
        "select x.a, y.b from x join y where x.a = y.b and c = d;",
        "select count(distinct a) as n from tb;",
        "select count(distinct (a)) as n from tb;",
        "select a, count(distinct b) as n from tb group by a having count(distinct b) > 1 order by count(distinct b) desc;",
        "exit;",
        "help;",
        "",
    };
    for (auto &sql : sqls) {
        std::cout << sql << std::endl;
        yyscan_t scanner;
        yylex_init(&scanner);
        std::shared_ptr<ast::TreeNode> parse_tree;
        YY_BUFFER_STATE buf = yy_scan_string(sql.c_str(), scanner);
        assert(yyparse(scanner, &parse_tree) == 0);
        yy_delete_buffer(buf, scanner);
        yylex_destroy(scanner);
        if (parse_tree != nullptr) {
            ast::TreePrinter::print(parse_tree);
            std::cout << std::endl;
        } else {
            std::cout << "exit/EOF" << std::endl;
        }
    }
    return 0;
}
