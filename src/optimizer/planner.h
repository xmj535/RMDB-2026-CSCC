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
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "execution/execution_defs.h"
#include "execution/execution_manager.h"
#include "record/rm.h"
#include "system/sm.h"
#include "common/context.h"
#include "plan.h"
#include "parser/parser.h"
#include "common/common.h"
#include "analyze/analyze.h"

class Planner {
   private:
    SmManager *sm_manager_;

    bool enable_nestedloop_join = true;
    bool enable_sortmerge_join = false;

   public:
    Planner(SmManager *sm_manager) : sm_manager_(sm_manager) {}


    std::shared_ptr<Plan> do_planner(std::shared_ptr<Query> query, Context *context);

    void set_enable_nestedloop_join(bool set_val) { enable_nestedloop_join = set_val; }
    
    void set_enable_sortmerge_join(bool set_val) { enable_sortmerge_join = set_val; }
    
   private:
    std::shared_ptr<Query> logical_optimization(std::shared_ptr<Query> query, Context *context);
    std::shared_ptr<Plan> physical_optimization(std::shared_ptr<Query> query, Context *context);

    std::shared_ptr<Plan> make_one_rel(std::shared_ptr<Query> query, Context *context);

    std::shared_ptr<Plan> generate_sort_plan(std::shared_ptr<Query> query, std::shared_ptr<Plan> plan);
    
    std::shared_ptr<Plan> generate_select_plan(std::shared_ptr<Query> query, Context *context);


    // int get_indexNo(std::string tab_name, std::vector<Condition> curr_conds);
    // disp 是查询里该表的显示名（无别名时等于 tab_name）：索引元数据按真实表名取，
    // 而条件列的 tab_name 记的是显示名，两者必须分开匹配，否则带别名的查询恒不命中索引。
    // allow_skip_scan：允许在最左前缀匹配不上时退而选用「首列未绑定 + 其后连续
    // 等值列」的跳跃扫描索引。**只有单表扫描才可以打开**。多表时打开会让本来
    // 没有固定前缀的表突然报告有前缀，进而改变左深树的初始外侧选择与
    // try_upgrade_inlj 的判定：实测 NewOrder 的 `customer ⋈ warehouse` 会从
    // 「warehouse 点查驱动 customer 探测」翻成「customer 跳跃扫 50 个仓库驱动
    // warehouse 全表扫」，整体吞吐掉约 40%。
    bool get_index_cols(const std::string& tab_name, const std::string& disp,
                        const std::vector<Condition>& curr_conds,
                        std::vector<std::string>& index_col_names,
                        bool allow_skip_scan = false);

    // 按文件头估算表行数上界，供连接算法选择使用
    int64_t estimated_rows(const std::string& tab_name) const;

    ColType interp_sv_type(ast::SvType sv_type) {
        std::map<ast::SvType, ColType> m = {
            {ast::SV_TYPE_INT, TYPE_INT}, {ast::SV_TYPE_FLOAT, TYPE_FLOAT}, {ast::SV_TYPE_STRING, TYPE_STRING}};
        return m.at(sv_type);
    }
};
