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

#include <cerrno>
#include <cstring>
#include <string>
#include "optimizer/plan.h"
#include "execution/executor_abstract.h"
#include "execution/executor_aggregate.h"
#include "execution/executor_filter.h"
#include "execution/executor_index_join.h"
#include "execution/executor_limit.h"
#include "execution/executor_hash_join.h"
#include "execution/executor_nestedloop_join.h"
#include "execution/executor_projection.h"
#include "execution/executor_seq_scan.h"
#include "execution/executor_index_scan.h"
#include "execution/executor_update.h"
#include "execution/executor_insert.h"
#include "execution/executor_load.h"
#include "execution/executor_delete.h"
#include "execution/executor_union.h"
#include "execution/execution_sort.h"
#include "common/common.h"

typedef enum portalTag{
    PORTAL_Invalid_Query = 0,
    PORTAL_ONE_SELECT,
    PORTAL_DML_WITHOUT_SELECT,
    PORTAL_MULTI_QUERY,
    PORTAL_CMD_UTILITY,
    PORTAL_EXPLAIN_ANALYZE
} portalTag;


struct PortalStmt {
    portalTag tag;

    std::vector<TabCol> sel_cols;
    std::unique_ptr<AbstractExecutor> root;
    std::shared_ptr<Plan> plan;

    PortalStmt(portalTag tag_, std::vector<TabCol> sel_cols_, std::unique_ptr<AbstractExecutor> root_, std::shared_ptr<Plan> plan_) :
            tag(tag_), sel_cols(std::move(sel_cols_)), root(std::move(root_)), plan(std::move(plan_)) {}
};

class Portal
{
   private:
    SmManager *sm_manager_;


   public:
    Portal(SmManager *sm_manager) : sm_manager_(sm_manager){}
    ~Portal(){}

    // 将查询执行计划转换成对应的算子树
    std::shared_ptr<PortalStmt> start(std::shared_ptr<Plan> plan, Context *context)
    {
        // 这里可以将select进行拆分，例如：一个select，带有return的select等
        if (auto x = std::dynamic_pointer_cast<OtherPlan>(plan)) {
            return std::make_shared<PortalStmt>(PORTAL_CMD_UTILITY, std::vector<TabCol>(), std::unique_ptr<AbstractExecutor>(),plan);
        } else if(auto x = std::dynamic_pointer_cast<SetKnobPlan>(plan)) {
            return std::make_shared<PortalStmt>(PORTAL_CMD_UTILITY, std::vector<TabCol>(), std::unique_ptr<AbstractExecutor>(), plan);
        } else if(auto x = std::dynamic_pointer_cast<SetIsolationPlan>(plan)) {
            return std::make_shared<PortalStmt>(PORTAL_CMD_UTILITY, std::vector<TabCol>(), std::unique_ptr<AbstractExecutor>(), plan);
        } else if (auto x = std::dynamic_pointer_cast<LoadPlan>(plan)) {
            std::unique_ptr<AbstractExecutor> root =
                std::make_unique<LoadExecutor>(sm_manager_, x->tab_name_, x->file_path_, context);
            return std::make_shared<PortalStmt>(PORTAL_DML_WITHOUT_SELECT, std::vector<TabCol>(), std::move(root), plan);
        } else if (auto x = std::dynamic_pointer_cast<DDLPlan>(plan)) {
            return std::make_shared<PortalStmt>(PORTAL_MULTI_QUERY, std::vector<TabCol>(), std::unique_ptr<AbstractExecutor>(),plan);
        } else if (auto x = std::dynamic_pointer_cast<DMLPlan>(plan)) {
            switch(x->tag) {
                case T_select:
                {
                    std::shared_ptr<ProjectionPlan> p = std::dynamic_pointer_cast<ProjectionPlan>(x->subplan_);
                    std::unique_ptr<AbstractExecutor> root= convert_plan_executor(p, context);
                    portalTag ptag = x->is_explain_ ? PORTAL_EXPLAIN_ANALYZE : PORTAL_ONE_SELECT;
                    // Prepared plans are connection-local but reusable across many
                    // EXEC_BATCH operations. Keep the cached projection intact.
                    std::vector<TabCol> sel = p->sel_cols_;
                    return std::make_shared<PortalStmt>(ptag, std::move(sel), std::move(root), plan);
                }

                case T_Update:
                {
                    std::unique_ptr<AbstractExecutor> scan= convert_plan_executor(x->subplan_, context);
                    std::vector<Rid> rids;
                    for (scan->beginTuple(); !scan->is_end(); scan->nextTuple()) {
                        rids.push_back(scan->rid());
                    }
                    std::unique_ptr<AbstractExecutor> root =std::make_unique<UpdateExecutor>(sm_manager_,
                                                            x->tab_name_, x->set_clauses_, x->conds_, rids, context);
                    return std::make_shared<PortalStmt>(PORTAL_DML_WITHOUT_SELECT, std::vector<TabCol>(), std::move(root), plan);
                }
                case T_Delete:
                {
                    std::unique_ptr<AbstractExecutor> scan= convert_plan_executor(x->subplan_, context);
                    std::vector<Rid> rids;
                    for (scan->beginTuple(); !scan->is_end(); scan->nextTuple()) {
                        rids.push_back(scan->rid());
                    }

                    std::unique_ptr<AbstractExecutor> root =
                        std::make_unique<DeleteExecutor>(sm_manager_, x->tab_name_, x->conds_, rids, context);

                    return std::make_shared<PortalStmt>(PORTAL_DML_WITHOUT_SELECT, std::vector<TabCol>(), std::move(root), plan);
                }

                case T_Insert:
                {
                    std::unique_ptr<AbstractExecutor> root =
                            std::make_unique<InsertExecutor>(sm_manager_, x->tab_name_, x->values_, context);

                    return std::make_shared<PortalStmt>(PORTAL_DML_WITHOUT_SELECT, std::vector<TabCol>(), std::move(root), plan);
                }


                default:
                    throw InternalError("Unexpected field type");
                    break;
            }
        } else {
            throw InternalError("Unexpected field type");
        }
        return nullptr;
    }

    // 遍历算子树并执行算子生成执行结果
    void run(std::shared_ptr<PortalStmt> portal, QlManager* ql, txn_id_t *txn_id, Context *context){
        switch(portal->tag) {
            case PORTAL_ONE_SELECT:
            {
                ql->select_from(std::move(portal->root), std::move(portal->sel_cols), context);
                break;
            }

            case PORTAL_DML_WITHOUT_SELECT:
            {
                ql->run_dml(std::move(portal->root));
                break;
            }
            case PORTAL_MULTI_QUERY:
            {
                ql->run_mutli_query(portal->plan, context);
                break;
            }
            case PORTAL_CMD_UTILITY:
            {
                ql->run_cmd_utility(portal->plan, txn_id, context);
                break;
            }
            case PORTAL_EXPLAIN_ANALYZE:
            {
                ql->explain_analyze(std::move(portal->root), portal->plan, context);
                break;
            }
            default:
            {
                throw InternalError("Unexpected field type");
            }
        }
    }

    // 清空资源
    void drop(){}


    std::unique_ptr<AbstractExecutor> convert_plan_executor(std::shared_ptr<Plan> plan, Context *context)
    {
        if(auto x = std::dynamic_pointer_cast<ProjectionPlan>(plan)){
            return std::make_unique<ProjectionExecutor>(convert_plan_executor(x->subplan_, context),
                                                        x->sel_cols_);
        } else if(auto x = std::dynamic_pointer_cast<FilterPlan>(plan)) {
            return std::make_unique<FilterExecutor>(convert_plan_executor(x->subplan_, context),
                                                    x->conds_);
        } else if(auto x = std::dynamic_pointer_cast<ScanPlan>(plan)) {
            if(x->tag == T_SeqScan) {
                auto seq = std::make_unique<SeqScanExecutor>(sm_manager_, x->tab_name_, x->alias_, x->conds_, context);
                // SELECT 计划条件在上层 Filter 时，SSI 读跟踪仍需行级谓词
                seq->set_ssi_conds(x->ssi_conds_);
                return seq;
            }
            else {
                auto idx = std::make_unique<IndexScanExecutor>(sm_manager_, x->tab_name_, x->alias_, x->conds_, x->index_col_names_, context);
                idx->set_ssi_conds(x->ssi_conds_);
                idx->set_skip_scan_allowed(x->use_skip_scan_);
                return idx;
            }
        } else if(auto x = std::dynamic_pointer_cast<JoinPlan>(plan)) {
            // 走 INLJ：右侧穿过 ProjectionPlan 找到 is_join_probe_ 的 ScanPlan
            ScanPlan* probe_scan = nullptr;
            {
                Plan* p = x->right_.get();
                while (p != nullptr) {
                    if (auto s = dynamic_cast<ScanPlan*>(p)) {
                        if (s->is_join_probe_) probe_scan = s;
                        break;
                    }
                    if (auto pr = dynamic_cast<ProjectionPlan*>(p)) {
                        p = pr->subplan_.get();
                        continue;
                    }
                    break;
                }
            }
            std::unique_ptr<AbstractExecutor> left = convert_plan_executor(x->left_, context);
            std::unique_ptr<AbstractExecutor> right = convert_plan_executor(x->right_, context);
            if (probe_scan != nullptr) {
                return std::make_unique<IndexJoinExecutor>(
                    std::move(left), std::move(right), sm_manager_,
                    probe_scan->tab_name_, probe_scan->alias_,
                    probe_scan->index_col_names_, probe_scan->probe_bindings_,
                    probe_scan->probe_prefix_len_, probe_scan->conds_,
                    x->conds_, context);
            }
            if (x->use_hash_) {
                // 等值连接且内表大：改走哈希连接，避免 |outer|x|inner| 的无界重扫。
                // usable() 为假说明没有可哈希的等值列对（如只有 FLOAT 等值），
                // 此时原样退回嵌套循环，语义不变。
                auto hash_join = std::make_unique<HashJoinExecutor>(
                    std::move(left), std::move(right), x->conds_);
                if (hash_join->usable()) {
                    return hash_join;
                }
                // 哈希不可用（如唯一等值列是 FLOAT）时，planner 早已判定
                // 朴素代价过大，所以要退回分块嵌套循环而不是朴素循环。
                left = hash_join->release_left();
                right = hash_join->release_right();
                return std::make_unique<NestedLoopJoinExecutor>(
                    std::move(left), std::move(right), x->conds_, true);
            }
            return std::make_unique<NestedLoopJoinExecutor>(
                std::move(left), std::move(right), x->conds_,
                x->use_block_);
        } else if(auto x = std::dynamic_pointer_cast<SortPlan>(plan)) {
            return std::make_unique<SortExecutor>(convert_plan_executor(x->subplan_, context),
                                            x->sel_cols_, x->is_descs_);
        } else if(auto x = std::dynamic_pointer_cast<AggregatePlan>(plan)) {
            return std::make_unique<AggregateExecutor>(
                convert_plan_executor(x->subplan_, context),
                x->sel_cols_, x->group_by_cols_, x->having_conds_);
        } else if(auto x = std::dynamic_pointer_cast<LimitPlan>(plan)) {
            return std::make_unique<LimitExecutor>(
                convert_plan_executor(x->subplan_, context), x->limit_);
        } else if(auto x = std::dynamic_pointer_cast<UnionPlan>(plan)) {
            std::vector<std::unique_ptr<AbstractExecutor>> children;
            for (auto& bp : x->branches_) {
                children.push_back(convert_plan_executor(bp, context));
            }
            return std::make_unique<UnionExecutor>(std::move(children), x->out_cols_);
        }
        return nullptr;
    }

};
