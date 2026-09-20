/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL
v2. You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "analyze.h"

#include <limits>

// 把 ast::SvAggType 映射成内部 AggType
static AggType convert_agg(ast::SvAggType t) {
  switch (t) {
    case ast::SV_AGG_NONE: return AGG_NONE;
    case ast::SV_AGG_COUNT: return AGG_COUNT;
    case ast::SV_AGG_MAX: return AGG_MAX;
    case ast::SV_AGG_MIN: return AGG_MIN;
    case ast::SV_AGG_SUM: return AGG_SUM;
    case ast::SV_AGG_AVG: return AGG_AVG;
  }
  return AGG_NONE;
}

// 拼出聚合表达式的默认显示名，例如 MAX(score)、COUNT(*)
static std::string default_agg_label(AggType agg, const std::string& tab_name,
                                     const std::string& col_name,
                                     bool is_star, bool is_distinct) {
  static const std::map<AggType, std::string> m = {
      {AGG_COUNT, "COUNT"}, {AGG_MAX, "MAX"}, {AGG_MIN, "MIN"},
      {AGG_SUM, "SUM"},     {AGG_AVG, "AVG"},
  };
  std::string body;
  if (is_star) {
    body = "*";
  } else if (!tab_name.empty()) {
    body = tab_name + "." + col_name;
  } else {
    body = col_name;
  }
  if (is_distinct) body = "DISTINCT " + body;
  auto it = m.find(agg);
  if (it == m.end()) return body;
  return it->second + "(" + body + ")";
}

// 把 parser 出来的 AST 翻成绑定好类型和列归属的 Query
std::shared_ptr<Query> Analyze::do_analyze(
    std::shared_ptr<ast::TreeNode> parse,
    const std::vector<ColType>* parameter_types) {
  std::shared_ptr<Query> query = std::make_shared<Query>();

  // 解开 EXPLAIN ANALYZE 包装，按 SELECT 流程分析
  bool is_explain = false;
  if (auto x = std::dynamic_pointer_cast<ast::ExplainStmt>(parse)) {
    is_explain = true;
    parse = x->select;
  }

  if (auto x = std::dynamic_pointer_cast<ast::SelectStmt>(parse)) {
    // 派生 UNION 表：FROM 子句中含括号包裹的 UNION 子查询时走 union 分支
    bool has_derived = false;
    for (auto& d : x->derived_tabs) {
      if (d) {
        has_derived = true;
        break;
      }
    }
    if (has_derived) {
      if (x->tabs.size() != 1 || !x->derived_tabs[0]) {
        throw RMDBError("derived UNION must be the only FROM table");
      }
      if (!x->conds.empty() || !x->join_conds.empty() ||
          !x->group_by_cols.empty() || !x->having_conds.empty()) {
        throw RMDBError(
            "WHERE/GROUP BY/HAVING not supported over derived UNION table");
      }
      std::string alias = x->aliases[0];
      if (alias.empty()) {
        throw RMDBError("derived UNION table must have an alias");
      }
      auto union_stmt = x->derived_tabs[0];
      if (union_stmt->branches.size() < 2) {
        throw RMDBError("UNION must have at least two branches");
      }

      // 分别分析每个分支：每个分支是普通 SelectStmt
      std::vector<std::shared_ptr<Query>> branch_queries;
      for (auto& b : union_stmt->branches) {
        branch_queries.push_back(do_analyze(b, parameter_types));
      }

      // 列数一致性校验
      size_t ncol = branch_queries[0]->cols.size();
      for (auto& bq : branch_queries) {
        if (bq->cols.size() != ncol) {
          throw RMDBError("UNION branches have different column counts");
        }
      }

      // 取每个分支真实列的类型 / 长度
      std::vector<std::vector<ColMeta>> branch_all_cols(branch_queries.size());
      for (size_t i = 0; i < branch_queries.size(); ++i) {
        get_all_cols(branch_queries[i]->tables, branch_queries[i]->aliases,
                     branch_all_cols[i]);
      }
      auto lookup_col = [&](const std::vector<ColMeta>& all_cols,
                            const TabCol& tc) -> ColMeta {
        for (auto& cm : all_cols) {
          if (cm.tab_name == tc.tab_name && cm.name == tc.col_name) return cm;
        }
        throw ColumnNotFoundError(tc.tab_name + "." + tc.col_name);
      };

      // 计算公共超类型并构造输出列
      std::vector<ColMeta> out_cols;
      size_t off = 0;
      for (size_t ci = 0; ci < ncol; ++ci) {
        ColType pt = TYPE_INT;
        int pl = 0;
        bool first = true;
        for (size_t bi = 0; bi < branch_queries.size(); ++bi) {
          auto& tc = branch_queries[bi]->cols[ci];
          ColMeta cm = lookup_col(branch_all_cols[bi], tc);
          if (first) {
            pt = cm.type;
            pl = cm.len;
            first = false;
          } else if (pt == cm.type) {
            if (pt == TYPE_STRING) pl = std::max(pl, cm.len);
          } else if ((pt == TYPE_INT && cm.type == TYPE_FLOAT) ||
                     (pt == TYPE_FLOAT && cm.type == TYPE_INT)) {
            pt = TYPE_FLOAT;
            pl = sizeof(float);
          } else {
            throw IncompatibleTypeError(coltype2str(pt), coltype2str(cm.type));
          }
        }
        ColMeta out;
        out.tab_name = alias;
        // 输出列名继承自第一个分支
        out.name = branch_queries[0]->cols[ci].col_name;
        out.type = pt;
        out.len = pl;
        out.offset = (int)off;
        out.index = false;
        off += pl;
        out_cols.push_back(out);
      }

      query->tables = {alias};
      query->aliases = {alias};
      query->union_branches = std::move(branch_queries);
      query->union_out_cols = out_cols;

      // 外层 SELECT 列表：默认沿用 union 输出
      query->select_all = x->cols.empty();
      if (query->select_all) {
        for (auto& oc : out_cols) {
          TabCol tc;
          tc.tab_name = oc.tab_name;
          tc.col_name = oc.name;
          query->cols.push_back(tc);
        }
      } else {
        for (auto& sv : x->cols) {
          if (sv->agg_type != ast::SV_AGG_NONE || sv->is_star) {
            throw RMDBError(
                "aggregate not supported in outer projection over UNION");
          }
          if (!sv->tab_name.empty() && sv->tab_name != alias) {
            throw ColumnNotFoundError(sv->tab_name + "." + sv->col_name);
          }
          bool found = false;
          for (auto& oc : out_cols) {
            if (oc.name == sv->col_name) {
              TabCol tc;
              tc.tab_name = alias;
              tc.col_name = oc.name;
              tc.alias = sv->alias;
              query->cols.push_back(tc);
              found = true;
              break;
            }
          }
          if (!found) throw ColumnNotFoundError(sv->col_name);
        }
      }

      // 外层 ORDER BY：列名必须出现在 union 输出列中
      if (x->has_sort && x->order) {
        for (auto& item : x->order->items) {
          if (!item || !item->col) continue;
          auto& col = item->col;
          if (col->agg_type != ast::SV_AGG_NONE) {
            throw RMDBError("aggregate not allowed in ORDER BY over UNION");
          }
          if (!col->tab_name.empty() && col->tab_name != alias) {
            throw ColumnNotFoundError(col->tab_name + "." + col->col_name);
          }
          bool found = false;
          for (auto& oc : out_cols) {
            if (oc.name == col->col_name) {
              col->tab_name = alias;
              found = true;
              break;
            }
          }
          if (!found) throw ColumnNotFoundError(col->col_name);
        }
      }

      query->limit = x->limit;
      query->is_explain = is_explain;
      query->parse = std::move(parse);
      return query;
    }

    query->tables = x->tabs;
    query->aliases = x->aliases;
    if (query->aliases.size() < query->tables.size()) {
      query->aliases.resize(query->tables.size(), std::string());
    }
    for (auto& tab_name : query->tables) {
      if (!sm_manager_->db_.is_table(tab_name)) {
        throw TableNotFoundError(tab_name);
      }
    }
    // 别名/表名作为显示名不可重复
    std::map<std::string, int> seen;
    for (size_t i = 0; i < query->tables.size(); ++i) {
      const std::string& disp =
          query->aliases[i].empty() ? query->tables[i] : query->aliases[i];
      if (++seen[disp] > 1) {
        throw AmbiguousColumnError(disp);
      }
    }

    query->select_all = x->cols.empty();

    std::vector<ColMeta> all_cols;
    get_all_cols(query->tables, query->aliases, all_cols);

    // SELECT 列表：保留聚合 / 别名信息
    for (auto& sv_col : x->cols) {
      TabCol tc;
      tc.tab_name = sv_col->tab_name;
      tc.col_name = sv_col->col_name;
      tc.agg = convert_agg(sv_col->agg_type);
      tc.is_star = sv_col->is_star;
      tc.is_distinct = sv_col->is_distinct;
      tc.alias = sv_col->alias;
      query->cols.push_back(tc);
    }
    if (query->select_all) {
      for (auto& col : all_cols) {
        TabCol tc;
        tc.tab_name = col.tab_name;
        tc.col_name = col.name;
        query->cols.push_back(tc);
      }
    } else {
      for (auto& sel_col : query->cols) {
        if (sel_col.is_distinct &&
            (sel_col.agg != AGG_COUNT || sel_col.is_star)) {
          throw RMDBError("DISTINCT is only supported by COUNT(column)");
        }
        if (sel_col.is_star) {
          // COUNT(*)：不绑列，但限制只能和 COUNT 一起出现
          if (sel_col.agg != AGG_COUNT) {
            throw RMDBError("* can only appear inside COUNT");
          }
          continue;
        }
        TabCol bound = check_column(
            all_cols, TabCol{sel_col.tab_name, sel_col.col_name});
        sel_col.tab_name = bound.tab_name;
        sel_col.col_name = bound.col_name;
      }
    }

    // 校验聚合函数与列类型兼容
    auto find_col_type = [&](const TabCol& tc) -> ColType {
      for (auto& c : all_cols) {
        if (c.tab_name == tc.tab_name && c.name == tc.col_name) {
          return c.type;
        }
      }
      throw ColumnNotFoundError(tc.tab_name + "." + tc.col_name);
    };
    // allow_string_minmax: SELECT 列表的 MIN/MAX 现已支持字符串(字典序)；
    // HAVING 下游求值是数值路径(eval_having_lhs→double)，字符串聚合仍不支持。
    auto check_agg_type = [&](AggType agg, const TabCol& tc,
                              bool allow_string_minmax) {
      if (agg == AGG_COUNT) return;  // COUNT 支持 int/float/char
      ColType t = find_col_type(tc);
      if (t != TYPE_STRING) return;
      if (allow_string_minmax && (agg == AGG_MAX || agg == AGG_MIN)) return;
      throw RMDBError("aggregate not supported on string column: " +
                      tc.tab_name + "." + tc.col_name);
    };

    bool has_agg_in_select = false;
    for (auto& sel : query->cols) {
      if (sel.agg != AGG_NONE) {
        has_agg_in_select = true;
        if (!sel.is_star) check_agg_type(sel.agg, sel, /*allow_string_minmax=*/true);
        // 默认显示名（无 alias 时）
        if (sel.alias.empty()) {
          sel.alias = default_agg_label(sel.agg, sel.tab_name, sel.col_name,
                                        sel.is_star, sel.is_distinct);
        }
      }
    }

    // GROUP BY 列：只能是普通列；类型必须可分组
    for (auto& sv_gc : x->group_by_cols) {
      if (sv_gc->agg_type != ast::SV_AGG_NONE || sv_gc->is_star) {
        throw RMDBError("GROUP BY does not accept aggregate expression");
      }
      TabCol gc{sv_gc->tab_name, sv_gc->col_name};
      gc = check_column(all_cols, gc);
      query->group_by_cols.push_back(gc);
    }

    // HAVING / ORDER BY 中允许直接写 SELECT 项的别名
    // 仅在“无表名 + 非聚合 + 非 *”的裸列引用上尝试匹配，避免与真实列冲突
    auto try_resolve_alias = [&](TabCol& tc) -> bool {
      if (!tc.tab_name.empty() || tc.agg != AGG_NONE || tc.is_star) return false;
      for (auto& sel : query->cols) {
        if (!sel.alias.empty() && sel.alias == tc.col_name) {
          tc = sel;
          return true;
        }
      }
      return false;
    };

    // HAVING 条件：左值可以是聚合 / 普通列 / SELECT 项别名，右值仅支持字面量
    for (auto& sv_hc : x->having_conds) {
      HavingCond hc;
      hc.lhs_col.tab_name = sv_hc->lhs->tab_name;
      hc.lhs_col.col_name = sv_hc->lhs->col_name;
      hc.lhs_col.agg = convert_agg(sv_hc->lhs->agg_type);
      hc.lhs_col.is_star = sv_hc->lhs->is_star;
      hc.lhs_col.is_distinct = sv_hc->lhs->is_distinct;
      if (hc.lhs_col.is_distinct &&
          (hc.lhs_col.agg != AGG_COUNT || hc.lhs_col.is_star)) {
        throw RMDBError("DISTINCT is only supported by COUNT(column)");
      }
      if (!hc.lhs_col.is_star) {
        // 先按真实列名解析；解析不到再尝试 SELECT 别名
        bool resolved = false;
        try {
          TabCol bound = check_column(
              all_cols, TabCol{hc.lhs_col.tab_name, hc.lhs_col.col_name});
          hc.lhs_col.tab_name = bound.tab_name;
          hc.lhs_col.col_name = bound.col_name;
          resolved = true;
        } catch (RMDBError&) {
          // 列名不存在则尝试别名
        }
        if (!resolved && !try_resolve_alias(hc.lhs_col)) {
          throw ColumnNotFoundError(hc.lhs_col.tab_name + "." +
                                    hc.lhs_col.col_name);
        }
      }
      if (hc.lhs_col.agg != AGG_NONE && !hc.lhs_col.is_star) {
        check_agg_type(hc.lhs_col.agg, hc.lhs_col, /*allow_string_minmax=*/false);
      }
      hc.op = convert_sv_comp_op(sv_hc->op);
      auto rhs_val = std::dynamic_pointer_cast<ast::Value>(sv_hc->rhs);
      if (!rhs_val) {
        throw RMDBError("HAVING rhs must be a literal");
      }
      hc.rhs_val = convert_sv_value(rhs_val, parameter_types);
      hc.rhs_raw = rhs_val->raw_str;
      query->having_conds.push_back(hc);
    }

    // 合并 JOIN ... ON 条件与 WHERE 条件，统一交给 planner 做谓词下推
    // 同时检查 WHERE / JOIN ON 中不允许出现聚合函数
    auto check_no_agg = [](const std::vector<std::shared_ptr<ast::BinaryExpr>>&
                               sv_conds) {
      for (auto& e : sv_conds) {
        if (e->lhs->agg_type != ast::SV_AGG_NONE || e->lhs->is_star) {
          throw RMDBError("aggregate not allowed in WHERE clause");
        }
        if (auto col = std::dynamic_pointer_cast<ast::Col>(e->rhs)) {
          if (col->agg_type != ast::SV_AGG_NONE || col->is_star) {
            throw RMDBError("aggregate not allowed in WHERE clause");
          }
        }
      }
    };
    check_no_agg(x->join_conds);
    check_no_agg(x->conds);
    std::vector<std::shared_ptr<ast::BinaryExpr>> all_conds;
    for (auto& c : x->join_conds) all_conds.push_back(c);
    for (auto& c : x->conds) all_conds.push_back(c);
    get_clause(all_conds, query->conds, parameter_types);
    check_clause(query->tables, query->aliases, query->conds);

    // 设置聚合标志：SELECT 含聚合、GROUP BY 非空、或 HAVING 非空，任一即启用
    query->has_agg = has_agg_in_select || !query->group_by_cols.empty() ||
                     !query->having_conds.empty();

    // 聚合查询：SELECT 列表中的非聚合列必须出现在 GROUP BY 中
    if (query->has_agg) {
      auto in_group_by = [&](const TabCol& tc) {
        for (auto& g : query->group_by_cols) {
          if (g.tab_name == tc.tab_name && g.col_name == tc.col_name) {
            return true;
          }
        }
        return false;
      };
      for (auto& sel : query->cols) {
        if (sel.agg == AGG_NONE && !in_group_by(sel)) {
          throw RMDBError("non-aggregated column " + sel.tab_name + "." +
                          sel.col_name + " not in GROUP BY");
        }
      }
      // HAVING 中的非聚合列也必须出现在 GROUP BY 中
      for (auto& hc : query->having_conds) {
        if (hc.lhs_col.agg == AGG_NONE && !hc.lhs_col.is_star &&
            !in_group_by(hc.lhs_col)) {
          throw RMDBError("HAVING references column " + hc.lhs_col.tab_name +
                          "." + hc.lhs_col.col_name + " not in GROUP BY");
        }
      }
      // ORDER BY 项：每项独立解析。
      // 三种合法形式：
      //   1) 出现在 SELECT 列表的别名（裸列 / 聚合别名）
      //   2) GROUP BY 中的列（直接按真实列绑定）
      //   3) 聚合表达式：必须能匹配上 SELECT 中的同名聚合（rewrite 成对应 alias）
      if (x->has_sort && x->order) {
        for (auto& item : x->order->items) {
          if (!item || !item->col) continue;
          auto& col = item->col;
          if (col->agg_type != ast::SV_AGG_NONE) {
            // 聚合形式：与 SELECT 中的聚合项匹配（agg + 列 + is_star）
            AggType agg = convert_agg(col->agg_type);
            bool matched = false;
            for (auto& sel : query->cols) {
              if (sel.agg != agg) continue;
              if (sel.is_star != col->is_star) continue;
              if (sel.is_distinct != col->is_distinct) continue;
              if (!col->is_star) {
                // 容许 col 没写表名时通过解析路径补全
                TabCol probe{col->tab_name, col->col_name};
                try {
                  probe = check_column(all_cols, probe);
                } catch (RMDBError&) {
                  continue;
                }
                if (probe.tab_name != sel.tab_name ||
                    probe.col_name != sel.col_name) {
                  continue;
                }
              }
              col->tab_name = "";
              col->col_name = sel.alias;
              matched = true;
              break;
            }
            if (!matched) {
              throw RMDBError("ORDER BY aggregate not present in SELECT list");
            }
            continue;
          }
          // 非聚合形式
          TabCol oc{col->tab_name, col->col_name};
          bool alias_resolved = false;
          TabCol bound;
          try {
            bound = check_column(all_cols, oc);
          } catch (RMDBError&) {
            if (try_resolve_alias(oc)) {
              bound = oc;
              alias_resolved = true;
            } else {
              throw;
            }
          }
          if (!alias_resolved && !in_group_by(bound)) {
            throw RMDBError("ORDER BY references column " + bound.tab_name +
                            "." + bound.col_name + " not in GROUP BY");
          }
          if (alias_resolved && bound.agg != AGG_NONE) {
            col->tab_name = "";
            col->col_name = bound.alias;
          } else {
            col->tab_name = bound.tab_name;
            col->col_name = bound.col_name;
          }
        }
      }
    }

    query->limit = x->limit;
  } else if (auto x = std::dynamic_pointer_cast<ast::UpdateStmt>(parse)) {
    if (!sm_manager_->db_.is_table(x->tab_name)) {
      throw TableNotFoundError(x->tab_name);
    }
    query->tables = {x->tab_name};
    query->aliases = {std::string()};

    // WHERE / SET 中不允许出现聚合表达式
    for (auto& e : x->conds) {
      if (e->lhs->agg_type != ast::SV_AGG_NONE || e->lhs->is_star) {
        throw RMDBError("aggregate not allowed in WHERE clause");
      }
    }

    TabMeta& tab = sm_manager_->db_.get_table(x->tab_name);
    for (auto& sv_set : x->set_clauses) {
      auto col = tab.get_col(sv_set->col_name);
      SetClause set;
      set.lhs = {.tab_name = x->tab_name, .col_name = sv_set->col_name};
      set.rhs_is_col = sv_set->rhs_is_col;
      // 算术更新 set col = rhs_col op val：记录操作符与操作数列。
      // 操作数列必须存在且与目标列同类型（数值列），否则语义非法。
      set.op = sv_set->op;
      if (set.rhs_is_col || set.op != 0) {
        auto rcol = tab.get_col(sv_set->rhs_col);  // 不存在会抛 ColumnNotFound
        set.rhs_col = {.tab_name = x->tab_name, .col_name = sv_set->rhs_col};
        if (rcol->type != col->type &&
            !((col->type == TYPE_FLOAT && rcol->type == TYPE_INT) ||
              (set.rhs_is_col && col->type == TYPE_INT &&
               rcol->type == TYPE_FLOAT))) {
          throw IncompatibleTypeError(coltype2str(col->type),
                                      coltype2str(rcol->type));
        }
        if (set.rhs_is_col && col->type == TYPE_STRING &&
            col->len != rcol->len) {
          throw IncompatibleTypeError(coltype2str(col->type),
                                      coltype2str(rcol->type));
        }
      }
      if (!set.rhs_is_col) {
        auto bind_set_value = [&](const std::shared_ptr<ast::Value>& sv_value) {
          Value value = convert_sv_value(sv_value, parameter_types);
          // 数值列跨 INT/FLOAT 赋值：按目标列类型隐式转换
          // （INT 列收窄、FLOAT 列提升）。连续表达式的每个字面量或参数
          // 都独立执行同一转换，避免首项与后续项规则不一致。
          if (value.type != col->type) {
            if (col->type == TYPE_FLOAT && value.type == TYPE_INT) {
              value.set_float(static_cast<float>(value.int_val));
            } else if (col->type == TYPE_INT && value.type == TYPE_FLOAT) {
              value.set_int(static_cast<int>(value.float_val));
            } else {
              throw IncompatibleTypeError(coltype2str(col->type),
                                          coltype2str(value.type));
            }
          }
          value.init_raw(col->len);
          return value;
        };

        set.rhs = bind_set_value(sv_set->val);
        set.trailing_terms.reserve(sv_set->trailing_terms.size());
        for (const auto& sv_term : sv_set->trailing_terms) {
          set.trailing_terms.push_back(
              {.op = sv_term.op, .rhs = bind_set_value(sv_term.val)});
        }
      }
      query->set_clauses.push_back(set);
    }

    get_clause(x->conds, query->conds, parameter_types);
    check_clause(query->tables, query->aliases, query->conds);
  } else if (auto x = std::dynamic_pointer_cast<ast::DeleteStmt>(parse)) {
    if (!sm_manager_->db_.is_table(x->tab_name)) {
      throw TableNotFoundError(x->tab_name);
    }
    query->tables = {x->tab_name};
    query->aliases = {std::string()};
    for (auto& e : x->conds) {
      if (e->lhs->agg_type != ast::SV_AGG_NONE || e->lhs->is_star) {
        throw RMDBError("aggregate not allowed in WHERE clause");
      }
    }
    get_clause(x->conds, query->conds, parameter_types);
    check_clause(query->tables, query->aliases, query->conds);
  } else if (auto x = std::dynamic_pointer_cast<ast::InsertStmt>(parse)) {
    if (!sm_manager_->db_.is_table(x->tab_name)) {
      throw TableNotFoundError(x->tab_name);
    }
    for (auto& sv_val : x->vals) {
      query->values.push_back(convert_sv_value(sv_val, parameter_types));
    }
  } else if (auto x = std::dynamic_pointer_cast<ast::LoadStmt>(parse)) {
    if (!sm_manager_->db_.is_table(x->tab_name)) {
      throw TableNotFoundError(x->tab_name);
    }
  }
  query->is_explain = is_explain;
  query->parse = std::move(parse);
  return query;
}

// 把没指定表名的列绑到唯一的表上，指定了就核实其确实属于扫描集合
TabCol Analyze::check_column(const std::vector<ColMeta>& all_cols,
                             TabCol target) {
  if (target.tab_name.empty()) {
    std::string tab_name;
    for (auto& col : all_cols) {
      if (col.name == target.col_name) {
        if (!tab_name.empty()) {
          throw AmbiguousColumnError(target.col_name);
        }
        tab_name = col.tab_name;
      }
    }
    if (tab_name.empty()) {
      throw ColumnNotFoundError(target.col_name);
    }
    target.tab_name = tab_name;
  } else {
    for (auto& col : all_cols) {
      if (col.tab_name == target.tab_name && col.name == target.col_name) {
        return target;
      }
    }
    throw ColumnNotFoundError(target.tab_name + "." + target.col_name);
  }
  return target;
}

// 把 tab_names+aliases 涉及的所有列拼成扁平列表，ColMeta.tab_name 改写为显示名
void Analyze::get_all_cols(const std::vector<std::string>& tab_names,
                           const std::vector<std::string>& aliases,
                           std::vector<ColMeta>& all_cols) {
  for (size_t i = 0; i < tab_names.size(); ++i) {
    const std::string& real = tab_names[i];
    const std::string& disp =
        (i < aliases.size() && !aliases[i].empty()) ? aliases[i] : real;
    auto cols = sm_manager_->db_.get_table(real).cols;
    for (auto& c : cols) c.tab_name = disp;
    all_cols.insert(all_cols.end(), cols.begin(), cols.end());
  }
}

// AST 的二元条件转成 Condition 结构；同时保留 raw_str 以便 EXPLAIN 还原原文
void Analyze::get_clause(
    const std::vector<std::shared_ptr<ast::BinaryExpr>>& sv_conds,
    std::vector<Condition>& conds,
    const std::vector<ColType>* parameter_types) {
  conds.clear();
  for (auto& expr : sv_conds) {
    Condition cond;
    cond.lhs_col = {.tab_name = expr->lhs->tab_name,
                    .col_name = expr->lhs->col_name};
    cond.op = convert_sv_comp_op(expr->op);
    if (auto rhs_val = std::dynamic_pointer_cast<ast::Value>(expr->rhs)) {
      cond.is_rhs_val = true;
      cond.rhs_val = convert_sv_value(rhs_val, parameter_types);
      cond.rhs_raw = rhs_val->raw_str;
    } else if (auto rhs_col = std::dynamic_pointer_cast<ast::Col>(expr->rhs)) {
      cond.is_rhs_val = false;
      cond.rhs_col = {.tab_name = rhs_col->tab_name,
                      .col_name = rhs_col->col_name};
    }
    conds.push_back(cond);
  }
}

// 给每个条件绑列、做类型检查并把字面量序列化成 raw
void Analyze::check_clause(const std::vector<std::string>& tab_names,
                           const std::vector<std::string>& aliases,
                           std::vector<Condition>& conds) {
  std::vector<ColMeta> all_cols;
  get_all_cols(tab_names, aliases, all_cols);

  // alias -> 真实表名 的映射；列查找需要拿到底层列的 type/len
  std::map<std::string, std::string> disp_to_real;
  for (size_t i = 0; i < tab_names.size(); ++i) {
    const std::string& real = tab_names[i];
    const std::string& disp =
        (i < aliases.size() && !aliases[i].empty()) ? aliases[i] : real;
    disp_to_real[disp] = real;
  }
  auto resolve_real = [&](const std::string& disp) -> const std::string& {
    auto it = disp_to_real.find(disp);
    if (it == disp_to_real.end()) {
      throw TableNotFoundError(disp);
    }
    return it->second;
  };

  for (auto& cond : conds) {
    cond.lhs_col = check_column(all_cols, cond.lhs_col);
    if (!cond.is_rhs_val) {
      cond.rhs_col = check_column(all_cols, cond.rhs_col);
    }
    TabMeta& lhs_tab =
        sm_manager_->db_.get_table(resolve_real(cond.lhs_col.tab_name));
    auto lhs_col = lhs_tab.get_col(cond.lhs_col.col_name);
    ColType lhs_type = lhs_col->type;
    ColType rhs_type;
    if (cond.is_rhs_val) {
      // FLOAT 列与 INT 字面量比较：把 rhs 提到 FLOAT
      if (lhs_type == TYPE_FLOAT && cond.rhs_val.type == TYPE_INT) {
        cond.rhs_val.set_float(static_cast<float>(cond.rhs_val.int_val));
      }
      cond.rhs_val.init_raw(lhs_col->len);
      rhs_type = cond.rhs_val.type;
    } else {
      TabMeta& rhs_tab =
          sm_manager_->db_.get_table(resolve_real(cond.rhs_col.tab_name));
      rhs_type = rhs_tab.get_col(cond.rhs_col.col_name)->type;
    }
    if (lhs_type != rhs_type) {
      throw IncompatibleTypeError(coltype2str(lhs_type), coltype2str(rhs_type));
    }
  }
}

// AST 字面量节点转 Value
Value Analyze::convert_sv_value(
    const std::shared_ptr<ast::Value>& sv_val,
    const std::vector<ColType>* parameter_types) {
  Value val;
  if (auto v = std::dynamic_pointer_cast<ast::IntLit>(sv_val)) {
    val.set_int(v->val);
    val.raw_str = v->raw_str;
  } else if (auto v = std::dynamic_pointer_cast<ast::FloatLit>(sv_val)) {
    val.set_float(v->val);
    val.raw_str = v->raw_str;
  } else if (auto v = std::dynamic_pointer_cast<ast::StringLit>(sv_val)) {
    val.set_str(v->val);
    val.raw_str = v->raw_str;
  } else if (auto v = std::dynamic_pointer_cast<ast::ParamRef>(sv_val)) {
    if (parameter_types == nullptr) {
      throw RMDBError("parameter marker is only valid in PREPARE_SET");
    }
    if (v->ordinal.empty() || v->ordinal.front() == '0') {
      throw RMDBError("parameter ordinal must start at 1");
    }
    uint32_t ordinal = 0;
    for (char digit : v->ordinal) {
      if (digit < '0' || digit > '9' ||
          ordinal > (std::numeric_limits<uint16_t>::max() -
                     static_cast<uint32_t>(digit - '0')) /
                        10U) {
        throw RMDBError("parameter ordinal is outside u16");
      }
      ordinal = ordinal * 10U + static_cast<uint32_t>(digit - '0');
    }
    if (ordinal == 0 || ordinal > parameter_types->size()) {
      throw RMDBError("parameter ordinal exceeds parameter_count");
    }
    switch ((*parameter_types)[ordinal - 1]) {
      case TYPE_INT:
        val.set_int(0);
        break;
      case TYPE_FLOAT:
        val.set_float(0.0F);
        break;
      case TYPE_STRING:
        val.set_str(std::string());
        break;
      default:
        throw RMDBError("unsupported prepared parameter type");
    }
    val.parameter_index = static_cast<uint16_t>(ordinal);
    val.raw_str = v->raw_str;
  } else {
    throw InternalError("Unexpected sv value type");
  }
  return val;
}

CompOp Analyze::convert_sv_comp_op(ast::SvCompOp op) {
  static const std::map<ast::SvCompOp, CompOp> m = {
      {ast::SV_OP_EQ, OP_EQ}, {ast::SV_OP_NE, OP_NE}, {ast::SV_OP_LT, OP_LT},
      {ast::SV_OP_GT, OP_GT}, {ast::SV_OP_LE, OP_LE}, {ast::SV_OP_GE, OP_GE},
  };
  return m.at(op);
}
