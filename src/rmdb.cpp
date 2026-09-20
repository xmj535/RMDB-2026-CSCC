/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <readline/history.h>
#include <readline/readline.h>
#include <setjmp.h>
#include <malloc.h>
#include <signal.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "errors.h"
#include "optimizer/optimizer.h"
#include "recovery/log_recovery.h"
#include "optimizer/plan.h"
#include "optimizer/planner.h"
#include "portal.h"
#include "analyze/analyze.h"
#include "common/wire_protocol.h"

#include <cstdarg>

#define SOCK_PORT 8765

static bool should_exit = false;

// 构建全局所需的管理器对象
auto disk_manager = std::make_unique<DiskManager>();
auto buffer_pool_manager = std::make_unique<BufferPoolManager>(BUFFER_POOL_SIZE, disk_manager.get());
auto rm_manager = std::make_unique<RmManager>(disk_manager.get(), buffer_pool_manager.get());
auto ix_manager = std::make_unique<IxManager>(disk_manager.get(), buffer_pool_manager.get());
auto sm_manager = std::make_unique<SmManager>(disk_manager.get(), buffer_pool_manager.get(), rm_manager.get(), ix_manager.get());
auto lock_manager = std::make_unique<LockManager>();
auto txn_manager = std::make_unique<TransactionManager>(lock_manager.get(), sm_manager.get());
auto planner = std::make_unique<Planner>(sm_manager.get());
auto optimizer = std::make_unique<Optimizer>(sm_manager.get(), planner.get());
auto ql_manager = std::make_unique<QlManager>(sm_manager.get(), txn_manager.get(), nullptr);
auto log_manager = std::make_unique<LogManager>(disk_manager.get());
auto recovery = std::make_unique<RecoveryManager>(disk_manager.get(), buffer_pool_manager.get(), sm_manager.get(), txn_manager.get(), log_manager.get());
auto portal = std::make_unique<Portal>(sm_manager.get());
auto analyze = std::make_unique<Analyze>(sm_manager.get());
// buffer_mutex 已移除：解析器改为可重入(每连接独立 yyscan_t)，无全局词法/AST 状态。

static jmp_buf jmpbuf;
void sigint_handler(int signo) {
    should_exit = true;
    log_manager->flush_log_to_disk();
    std::cout << "The Server receive Crtl+C, will been closed\n";
    longjmp(jmpbuf, 1);
}

// 创建静态检查点时全局互斥，避免与并发写事务交叉（静态检查点要求静止）
static std::mutex checkpoint_mutex;

// ===== TEMPORARY DIAGNOSTIC (exec-batch-trace) =====
// 定位 post-crash EXEC_BATCH 卡死点用。每条记录立即 fflush，进程被 SIGKILL
// 也不丢最后一行，从而能看出"哪个操作开始了但没结束"。
// 还原：删除本段与所有 EXEC_TRACE(...) 调用。
static std::mutex exec_trace_mutex;
static FILE* exec_trace_file = nullptr;

// 未设置 RMDB_EXEC_TRACE 时完全关闭：正式运行零开销（不建文件、不加锁写盘）
static const bool exec_trace_enabled = (getenv("RMDB_EXEC_TRACE") != nullptr);

static void exec_trace(const char* fmt, ...) {
    if (!exec_trace_enabled) return;
    std::lock_guard<std::mutex> guard(exec_trace_mutex);
    if (exec_trace_file == nullptr) {
        exec_trace_file = fopen(getenv("RMDB_EXEC_TRACE"), "a");
        if (exec_trace_file == nullptr) return;
        setvbuf(exec_trace_file, nullptr, _IONBF, 0);  // unbuffered
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    fprintf(exec_trace_file, "%lld.%06ld tid=%lu ",
            static_cast<long long>(ts.tv_sec), ts.tv_nsec / 1000,
            static_cast<unsigned long>(pthread_self()));
    va_list args;
    va_start(args, fmt);
    vfprintf(exec_trace_file, fmt, args);
    va_end(args);
    fputc('\n', exec_trace_file);
    fflush(exec_trace_file);
}

#define EXEC_TRACE(...) exec_trace(__VA_ARGS__)
// ===== END TEMPORARY DIAGNOSTIC =====

// 创建静态检查点：静止事务 → 刷日志缓冲 → 写检查点记录 → 刷脏页 → 把检查点地址写入重启文件
void create_static_checkpoint() {
    std::lock_guard<std::mutex> guard(checkpoint_mutex);
    // (1) 停止接收新事务 + 等待正在运行的事务结束（静止化）。
    //     这样下面 flush 期间不会有并发写改动缓冲页/目录结构，既避免并发数据竞争
    //     导致的崩溃，又保证不会把未提交数据落盘（落盘后无法在检查点之后被撤销）。
    txn_manager->BeginCheckpointQuiesce();
    try {
        // (2) 把只存在于版本链里的已提交逻辑删除兑现成堆上的物理删除。
        //     MVCC 的 DELETE 不动堆，物理删除只发生在恢复 redo 里；而检查点路径
        //     的 redo 从检查点偏移起扫，检查点之前的 DELETE 再也不会被重放。
        //     必须在静止化之内做（此刻无活跃事务，没有读者还需要这些旧版本）。
        txn_manager->ApplyLogicalDeletesToHeap();
        // (3) 把日志缓冲区内容刷到磁盘
        log_manager->flush_log_to_disk();
        // (3) 在日志文件中写入检查点记录（其地址即当前日志文件尾）
        long long ckpt_offset = disk_manager->get_file_size(LOG_FILE_NAME);
        StaticCheckpointLogRecord ckpt_rec;
        log_manager->add_log_to_buffer(&ckpt_rec);
        log_manager->flush_log_to_disk();
        // (4) 把数据库缓冲区中的脏页（堆 + 索引）写到磁盘。
        //
        // **连同两类 file header 一起落盘**。堆的 RmFileHdr 与索引的 IxFileHdr
        // 都由 disk_manager->write_page 直写、不经缓冲池，flush_all_pages 碰不到
        // 它们；而检查点路径的恢复对"未被触及的表"既不重建索引也不重算空闲链，
        // 直接信任磁盘态。头没落盘 ⇒ 崩溃后索引句柄读回陈旧的 root_page_，
        // 整棵树只剩根那一小片。实测（零负载、发一次检查点再 SIGKILL）：
        // order_line 索引 15,002,806 → 84 条，stock 5,000,000 → 127 条。
        // 此前从未暴露，是因为无检查点时恢复总会从堆全量重建所有索引。
        for (auto& entry : sm_manager->fhs_) {
            entry.second->flush_file_hdr();
            buffer_pool_manager->flush_all_pages(entry.second->GetFd());
        }
        for (auto& entry : sm_manager->ihs_) {
            sm_manager->get_ix_manager()->flush_index(entry.second.get());
        }
        // (5) 把检查点记录地址写到重启文件
        std::ofstream rf(RecoveryManager::RESTART_FILE_NAME,
                         std::ios::binary | std::ios::trunc);
        rf.write(reinterpret_cast<const char*>(&ckpt_offset), sizeof(ckpt_offset));
        rf.flush();
        rf.close();
    } catch (...) {
        // 无论成功失败都必须恢复事务接收，否则后续所有 begin 会被永久阻塞
        txn_manager->EndCheckpointQuiesce();
        throw;
    }
    txn_manager->EndCheckpointQuiesce();
}

// 判断一条请求是否为 create static_checkpoint 命令（容忍空白与结尾分号、大小写）
static bool is_static_checkpoint_cmd(const char* sql) {
    std::string s(sql);
    // 去掉首尾空白
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return false;
    size_t e = s.find_last_not_of(" \t\r\n;");
    std::string core = s.substr(b, e - b + 1);
    // 折叠内部连续空白并转小写
    std::string norm;
    bool prev_space = false;
    for (char c : core) {
        if (c == ' ' || c == '\t') {
            if (!prev_space && !norm.empty()) norm.push_back(' ');
            prev_space = true;
        } else {
            norm.push_back(static_cast<char>(::tolower(c)));
            prev_space = false;
        }
    }
    return norm == "create static_checkpoint";
}

// Wire v3 不使用历史 output.txt 输出通道，也不实现 SET OUTPUT_FILE ON/OFF。
std::atomic<bool> g_output_enabled{false};

static std::shared_ptr<ast::TreeNode> parse_one_sql(const std::string& sql) {
    yyscan_t scanner = nullptr;
    if (yylex_init(&scanner) != 0) {
        throw std::runtime_error("failed to initialize SQL scanner");
    }
    YY_BUFFER_STATE buffer = nullptr;
    try {
        std::shared_ptr<ast::TreeNode> parse_tree;
        buffer = yy_scan_string(sql.c_str(), scanner);
        int parse_rc = yyparse(scanner, &parse_tree);
        yy_delete_buffer(buffer, scanner);
        buffer = nullptr;
        yylex_destroy(scanner);
        scanner = nullptr;
        if (parse_rc != 0 || parse_tree == nullptr) {
            throw RMDBError("syntax error");
        }
        return parse_tree;
    } catch (...) {
        if (buffer != nullptr) yy_delete_buffer(buffer, scanner);
        if (scanner != nullptr) yylex_destroy(scanner);
        throw;
    }
}

// PREPARE_SET carries exactly one SQL statement. Ranking templates are not
// required to carry the interactive client's trailing delimiter, so add only
// that fixed delimiter (never parameter data) when it is absent.
static std::string prepared_parser_input(const std::string& sql) {
    size_t end = sql.find_last_not_of(" \t\r\n");
    if (end != std::string::npos && sql[end] == ';') return sql;
    return sql + "\n;";
}

// Validate the public marker contract independently of parser placement.
// Markers in SQL strings and comments do not create parameter slots.
static void validate_parameter_markers(const std::string& sql,
                                       size_t parameter_count) {
    enum class State { NORMAL, STRING, LINE_COMMENT, BLOCK_COMMENT };
    State state = State::NORMAL;
    std::vector<bool> seen(parameter_count + 1, false);

    for (size_t i = 0; i < sql.size();) {
        char c = sql[i];
        if (state == State::STRING) {
            if (c == '\'' && i + 1 < sql.size() && sql[i + 1] == '\'') {
                i += 2;
            } else {
                if (c == '\'') state = State::NORMAL;
                ++i;
            }
            continue;
        }
        if (state == State::LINE_COMMENT) {
            if (c == '\r' || c == '\n') state = State::NORMAL;
            ++i;
            continue;
        }
        if (state == State::BLOCK_COMMENT) {
            if (c == '*' && i + 1 < sql.size() && sql[i + 1] == '/') {
                state = State::NORMAL;
                i += 2;
            } else {
                ++i;
            }
            continue;
        }

        if (c == '\'') {
            state = State::STRING;
            ++i;
            continue;
        }
        if (c == '-' && i + 1 < sql.size() && sql[i + 1] == '-') {
            state = State::LINE_COMMENT;
            i += 2;
            continue;
        }
        if (c == '/' && i + 1 < sql.size() && sql[i + 1] == '*') {
            state = State::BLOCK_COMMENT;
            i += 2;
            continue;
        }
        if (c != '$') {
            ++i;
            continue;
        }
        if (i + 1 >= sql.size() || sql[i + 1] < '0' || sql[i + 1] > '9') {
            throw RMDBError("invalid PREPARE_SET parameter marker");
        }
        size_t j = i + 1;
        if (sql[j] == '0') {
            throw RMDBError("parameter ordinal must start at 1");
        }
        uint32_t ordinal = 0;
        while (j < sql.size() && sql[j] >= '0' && sql[j] <= '9') {
            uint32_t digit = static_cast<uint32_t>(sql[j] - '0');
            if (ordinal >
                (std::numeric_limits<uint16_t>::max() - digit) / 10U) {
                throw RMDBError("parameter ordinal is outside u16");
            }
            ordinal = ordinal * 10U + digit;
            ++j;
        }
        if (ordinal == 0 || ordinal > parameter_count) {
            throw RMDBError("parameter ordinal exceeds parameter_count");
        }
        seen[ordinal] = true;
        i = j;
    }

    for (size_t ordinal = 1; ordinal <= parameter_count; ++ordinal) {
        if (!seen[ordinal]) {
            throw RMDBError(
                "parameter ordinals must form dense set 1..parameter_count");
        }
    }
}

static ColType prepared_col_type(uint8_t wire_type) {
    switch (wire_type) {
        case wire::kInt32:
            return TYPE_INT;
        case wire::kFloat32:
            return TYPE_FLOAT;
        case wire::kChar:
            return TYPE_STRING;
        default:
            throw wire::ProtocolError("unknown prepared parameter type");
    }
}

struct PreparedEntry {
    wire::PrepareRequest request;
    std::vector<ColType> parameter_types;
    std::shared_ptr<Query> query;
    std::shared_ptr<Plan> plan;
    std::vector<wire::Column> columns;
};

using PreparedDictionary = std::unordered_map<uint16_t, PreparedEntry>;

struct BatchParameter {
    uint8_t wire_type = 0;
    bool present = false;
    int32_t int_value = 0;
    uint32_t float_bits = 0;
    std::string char_value;
};

struct BatchOperation {
    uint16_t statement_id = 0;
    std::vector<BatchParameter> parameters;
};

static std::vector<BatchOperation> decode_exec_batch(
    const std::vector<uint8_t>& payload,
    const PreparedDictionary& dictionary) {
    wire::PayloadReader reader(payload);
    uint16_t operation_count = reader.u16("EXEC_BATCH.operation_count");
    if (operation_count == 0 || operation_count > 256) {
        throw wire::ProtocolError(
            "EXEC_BATCH operation_count must be in 1..256");
    }

    std::vector<BatchOperation> operations;
    operations.reserve(operation_count);
    for (uint16_t operation_index = 0; operation_index < operation_count;
         ++operation_index) {
        BatchOperation operation;
        operation.statement_id =
            reader.u16("EXEC_BATCH.operation.statement_id");
        auto prepared = dictionary.find(operation.statement_id);
        if (prepared == dictionary.end()) {
            throw wire::ProtocolError(
                "EXEC_BATCH statement id is not prepared on this connection");
        }

        const auto& parameter_types = prepared->second.request.parameter_types;
        operation.parameters.reserve(parameter_types.size());
        for (uint8_t wire_type : parameter_types) {
            BatchParameter parameter;
            parameter.wire_type = wire_type;
            uint8_t present = reader.u8("EXEC_BATCH.cell.present");
            if (present > 1) {
                throw wire::ProtocolError(
                    "EXEC_BATCH cell.present must be 0 or 1");
            }
            parameter.present = present == 1;
            if (parameter.present) {
                if (wire_type == wire::kInt32) {
                    uint32_t bits = reader.u32("EXEC_BATCH.INT32");
                    std::memcpy(&parameter.int_value, &bits, sizeof(bits));
                } else if (wire_type == wire::kFloat32) {
                    parameter.float_bits = reader.u32("EXEC_BATCH.FLOAT32");
                } else if (wire_type == wire::kChar) {
                    uint32_t byte_count =
                        reader.u32("EXEC_BATCH.CHAR.byte_count");
                    const uint8_t* bytes =
                        reader.bytes(byte_count, "EXEC_BATCH.CHAR.value");
                    parameter.char_value.assign(
                        reinterpret_cast<const char*>(bytes), byte_count);
                } else {
                    throw wire::ProtocolError(
                        "prepared statement contains an unknown parameter type");
                }
            }
            operation.parameters.push_back(std::move(parameter));
        }
        operations.push_back(std::move(operation));
    }
    reader.finish("EXEC_BATCH");
    return operations;
}

static void bind_value(Value& value,
                       const std::vector<BatchParameter>& parameters) {
    if (value.parameter_index == 0) return;
    size_t index = static_cast<size_t>(value.parameter_index - 1);
    if (index >= parameters.size()) {
        throw wire::ProtocolError("prepared parameter ordinal is out of range");
    }
    const BatchParameter& parameter = parameters[index];
    if (!parameter.present) {
        throw RMDBError("SQL NULL parameters are not supported");
    }

    int raw_length = value.raw == nullptr ? 0 : value.raw->size;
    switch (parameter.wire_type) {
        case wire::kInt32:
            if (value.type == TYPE_INT) {
                value.set_int(parameter.int_value);
            } else if (value.type == TYPE_FLOAT) {
                value.set_float(static_cast<float>(parameter.int_value));
            } else {
                throw IncompatibleTypeError(coltype2str(value.type), "INT");
            }
            break;
        case wire::kFloat32: {
            float float_value;
            std::memcpy(&float_value, &parameter.float_bits,
                        sizeof(float_value));
            if (!std::isfinite(float_value)) {
                throw RMDBError("FLOAT32 parameter must be finite");
            }
            if (value.type == TYPE_FLOAT) {
                value.set_float(float_value);
            } else if (value.type == TYPE_INT) {
                double widened = static_cast<double>(float_value);
                if (widened < std::numeric_limits<int32_t>::min() ||
                    widened > std::numeric_limits<int32_t>::max()) {
                    throw RMDBError("FLOAT32 parameter is outside INT range");
                }
                value.set_int(static_cast<int32_t>(float_value));
            } else {
                throw IncompatibleTypeError(coltype2str(value.type), "FLOAT");
            }
            break;
        }
        case wire::kChar:
            if (value.type != TYPE_STRING) {
                throw IncompatibleTypeError(coltype2str(value.type), "STRING");
            }
            value.set_str(parameter.char_value);
            break;
        default:
            throw wire::ProtocolError("unknown prepared parameter type");
    }

    if (raw_length > 0) {
        value.raw.reset();
        value.init_raw(raw_length);
    }
}

static void bind_conditions(std::vector<Condition>& conditions,
                            const std::vector<BatchParameter>& parameters) {
    for (auto& condition : conditions) {
        if (condition.is_rhs_val) bind_value(condition.rhs_val, parameters);
    }
}

static void bind_set_clauses(std::vector<SetClause>& clauses,
                             const std::vector<BatchParameter>& parameters) {
    for (auto& clause : clauses) {
        // 直接列赋值没有参数 Value；其右值在执行时从当前行读取。
        if (!clause.rhs_is_col) bind_value(clause.rhs, parameters);
        for (auto& term : clause.trailing_terms) {
            bind_value(term.rhs, parameters);
        }
    }
}

static void bind_prepared_plan(const std::shared_ptr<Plan>& plan,
                               const std::vector<BatchParameter>& parameters) {
    if (plan == nullptr) return;
    if (auto dml = std::dynamic_pointer_cast<DMLPlan>(plan)) {
        for (auto& value : dml->values_) bind_value(value, parameters);
        bind_conditions(dml->conds_, parameters);
        bind_set_clauses(dml->set_clauses_, parameters);
        bind_prepared_plan(dml->subplan_, parameters);
    } else if (auto scan = std::dynamic_pointer_cast<ScanPlan>(plan)) {
        bind_conditions(scan->conds_, parameters);
        bind_conditions(scan->fed_conds_, parameters);
        bind_conditions(scan->ssi_conds_, parameters);
    } else if (auto filter = std::dynamic_pointer_cast<FilterPlan>(plan)) {
        bind_conditions(filter->conds_, parameters);
        bind_prepared_plan(filter->subplan_, parameters);
    } else if (auto join = std::dynamic_pointer_cast<JoinPlan>(plan)) {
        bind_conditions(join->conds_, parameters);
        bind_prepared_plan(join->left_, parameters);
        bind_prepared_plan(join->right_, parameters);
    } else if (auto projection =
                   std::dynamic_pointer_cast<ProjectionPlan>(plan)) {
        bind_prepared_plan(projection->subplan_, parameters);
    } else if (auto sort = std::dynamic_pointer_cast<SortPlan>(plan)) {
        bind_prepared_plan(sort->subplan_, parameters);
    } else if (auto aggregate =
                   std::dynamic_pointer_cast<AggregatePlan>(plan)) {
        for (auto& condition : aggregate->having_conds_) {
            bind_value(condition.rhs_val, parameters);
        }
        bind_prepared_plan(aggregate->subplan_, parameters);
    } else if (auto limit = std::dynamic_pointer_cast<LimitPlan>(plan)) {
        bind_prepared_plan(limit->subplan_, parameters);
    } else if (auto union_plan = std::dynamic_pointer_cast<UnionPlan>(plan)) {
        for (auto& branch : union_plan->branches_) {
            bind_prepared_plan(branch, parameters);
        }
    }
}

// INSERT's legacy analyzer defers value-count/type checks to execution. A
// prepared plan must complete binding during installation, without executing
// or changing any row, so perform the same checks here and retain parameter
// slot identity through numeric coercion.
static void validate_prepared_insert(const std::shared_ptr<Query>& query) {
    auto insert = std::dynamic_pointer_cast<ast::InsertStmt>(query->parse);
    if (insert == nullptr) return;
    TabMeta& table = sm_manager->db_.get_table(insert->tab_name);
    if (query->values.size() != table.cols.size()) {
        throw InvalidValueCountError();
    }
    for (size_t i = 0; i < query->values.size(); ++i) {
        Value& value = query->values[i];
        const ColMeta& column = table.cols[i];
        if (value.type != column.type) {
            if (column.type == TYPE_FLOAT && value.type == TYPE_INT) {
                value.set_float(static_cast<float>(value.int_val));
            } else if (column.type == TYPE_INT && value.type == TYPE_FLOAT) {
                value.set_int(static_cast<int>(value.float_val));
            } else {
                throw IncompatibleTypeError(coltype2str(column.type),
                                            coltype2str(value.type));
            }
        }
        if (value.type == TYPE_STRING && value.parameter_index == 0 &&
            value.str_val.size() > static_cast<size_t>(column.len)) {
            throw StringOverflowError();
        }
    }
}

static std::vector<wire::Column> prepared_query_columns(
    const std::shared_ptr<Plan>& plan, Context* context) {
    if (auto other = std::dynamic_pointer_cast<OtherPlan>(plan)) {
        switch (other->tag) {
            case T_Help:
                return {{"Help", wire::kChar}};
            case T_ShowTable:
                return {{"Tables", wire::kChar}};
            case T_ShowIndex:
                (void)sm_manager->db_.get_table(other->tab_name_);
                return {{"Table", wire::kChar}, {"Unique", wire::kChar},
                        {"Columns", wire::kChar}};
            case T_DescTable:
                (void)sm_manager->db_.get_table(other->tab_name_);
                return {{"Field", wire::kChar}, {"Type", wire::kChar},
                        {"Index", wire::kChar}};
            default:
                return {};
        }
    }

    auto dml = std::dynamic_pointer_cast<DMLPlan>(plan);
    if (dml == nullptr || dml->tag != T_select) return {};
    if (dml->is_explain_) return {{"QUERY PLAN", wire::kChar}};

    auto projection = std::dynamic_pointer_cast<ProjectionPlan>(dml->subplan_);
    if (projection == nullptr) {
        throw InternalError("prepared SELECT has no projection plan");
    }
    std::unique_ptr<AbstractExecutor> executor =
        portal->convert_plan_executor(dml->subplan_, context);
    if (executor == nullptr || executor->cols().empty()) {
        throw InternalError("prepared SELECT has no result schema");
    }

    std::vector<wire::Column> columns;
    const auto& result_cols = executor->cols();
    columns.reserve(result_cols.size());
    for (size_t i = 0; i < result_cols.size(); ++i) {
        std::string name = result_cols[i].name;
        if (i < projection->sel_cols_.size()) {
            const TabCol& selected = projection->sel_cols_[i];
            name = selected.alias.empty() ? selected.col_name : selected.alias;
        }
        columns.push_back({std::move(name),
                           wire::sql_type(result_cols[i].type)});
    }
    return columns;
}

struct PreparedCandidate {
    PreparedDictionary dictionary;
    std::vector<wire::PreparedSchema> schemas;
};

static PreparedCandidate prepare_dictionary_candidate(
    std::vector<wire::PrepareRequest> requests,
    IsolationLevel* session_iso) {
    PreparedCandidate candidate;
    candidate.dictionary.reserve(requests.size());
    candidate.schemas.reserve(requests.size());

    Context context(lock_manager.get(), log_manager.get(), nullptr);
    context.session_iso_ = session_iso;
    context.txn_mgr_ = txn_manager.get();

    for (auto& request : requests) {
        validate_parameter_markers(request.sql, request.parameter_types.size());
        std::vector<ColType> parameter_types;
        parameter_types.reserve(request.parameter_types.size());
        for (uint8_t type : request.parameter_types) {
            parameter_types.push_back(prepared_col_type(type));
        }

        auto parse_tree = parse_one_sql(prepared_parser_input(request.sql));
        auto query = analyze->do_analyze(parse_tree, &parameter_types);
        validate_prepared_insert(query);
        auto plan = optimizer->plan_query(query, &context);
        auto columns = prepared_query_columns(plan, &context);
        bool actual_query = !columns.empty();
        if ((request.result_kind == 1) != actual_query) {
            throw RMDBError(
                "PREPARE_SET result_kind does not match SQL result schema");
        }

        uint16_t statement_id = request.statement_id;
        candidate.schemas.push_back({statement_id, columns});
        PreparedEntry entry{std::move(request), std::move(parameter_types),
                            std::move(query), std::move(plan),
                            std::move(columns)};
        candidate.dictionary.emplace(statement_id, std::move(entry));
    }
    return candidate;
}

// 判断当前正在执行的是显式事务还是单条SQL语句的事务，并更新事务ID
void SetTransaction(txn_id_t *txn_id, Context *context) {
    Transaction* prev = txn_manager->get_transaction(*txn_id);
    if(prev == nullptr || prev->get_state() == TransactionState::COMMITTED ||
        prev->get_state() == TransactionState::ABORTED) {
        IsolationLevel iso = context->session_iso_ != nullptr
                                 ? *(context->session_iso_)
                                 : IsolationLevel::SNAPSHOT_ISOLATION;
        context->txn_ = txn_manager->begin(nullptr, context->log_mgr_, iso);
        *txn_id = context->txn_->get_transaction_id();
        context->txn_->set_txn_mode(false);
        // 回收上一条语句已结束的事务对象，防止 txn_map 无界堆积导致 normal 阶段 OOM。
        // 此刻已是同连接的下一条语句，prev 不会再被本线程解引用，安全 delete。
        if (prev != nullptr) txn_manager->reap_transaction(prev);
    } else {
        context->txn_ = prev;
    }
}

static void abort_open_transaction(txn_id_t* txn_id) {
    Transaction* txn = txn_manager->lookup_txn_any_thread(*txn_id);
    if (txn != nullptr && txn->get_state() == TransactionState::DEFAULT) {
        txn_manager->abort(txn, log_manager.get());
    }
    *txn_id = INVALID_TXN_ID;
}

static bool same_columns(const std::vector<wire::Column>& expected,
                         const std::vector<wire::Column>& actual) {
    if (expected.size() != actual.size()) return false;
    for (size_t i = 0; i < expected.size(); ++i) {
        if (expected[i].name != actual[i].name ||
            expected[i].type != actual[i].type) {
            return false;
        }
    }
    return true;
}

static wire::BatchQueryResult execute_prepared_operation(
    uint16_t operation_index, PreparedEntry* entry,
    const std::vector<BatchParameter>& parameters, txn_id_t* txn_id,
    IsolationLevel* session_iso) {
    if (entry == nullptr || parameters.size() != entry->parameter_types.size()) {
        throw wire::ProtocolError("EXEC_BATCH parameter count mismatch");
    }
    bind_prepared_plan(entry->plan, parameters);

    wire::ResultWriter capture;
    Context context(lock_manager.get(), log_manager.get(), nullptr);
    context.session_iso_ = session_iso;
    context.txn_mgr_ = txn_manager.get();
    context.wire_writer_ = &capture;
    SetTransaction(txn_id, &context);

    std::shared_ptr<PortalStmt> portal_stmt = portal->start(entry->plan, &context);
    portal->run(portal_stmt, ql_manager.get(), txn_id, &context);
    portal->drop();

    // A standalone prepared operation keeps EXEC_STREAM's implicit-transaction
    // semantics. Ranking batches use explicit BEGIN/COMMIT, so dependency-stage
    // batches leave that explicit transaction open between requests.
    if (context.txn_ != nullptr && !context.txn_->get_txn_mode() &&
        context.txn_->get_state() == TransactionState::DEFAULT) {
        txn_manager->commit(context.txn_, context.log_mgr_);
    }

    const bool expected_query = !entry->columns.empty();
    if (expected_query) {
        if (!capture.result_active() ||
            !same_columns(entry->columns, capture.columns())) {
            throw InternalError(
                "prepared query result does not match PREPARE_OK schema");
        }
        return {operation_index, capture.take_captured_rows()};
    }
    if (capture.result_active() || !capture.captured_rows().empty()) {
        throw InternalError("prepared command unexpectedly produced rows");
    }
    return {};
}

static void send_batch_failure(uint16_t operation_count,
                               uint16_t failed_operation, uint8_t status,
                               const std::string& diagnostic, int fd) {
    std::vector<uint8_t> payload = wire::encode_batch_result(
        operation_count, failed_operation, status, failed_operation,
        diagnostic, {});
    wire::write_frame(fd, wire::kBatchResult, payload);
}

static void ensure_batch_results_fit(
    const std::vector<wire::BatchQueryResult>& results) {
    // Fixed BATCH_RESULT header through result_count.
    size_t payload_bytes = 2 + 1 + 2 + 4 + 2;
    for (const auto& result : results) {
        if (payload_bytes > wire::kMaxPayload - 6) {
            throw wire::ProtocolError("BATCH_RESULT payload exceeds 1 MiB");
        }
        payload_bytes += 6;  // operation_index + row_count
        for (const auto& row : result.rows) {
            if (row.size() > wire::kMaxPayload - payload_bytes) {
                throw wire::ProtocolError("BATCH_RESULT payload exceeds 1 MiB");
            }
            payload_bytes += row.size();
        }
    }
}

static void execute_batch(std::vector<BatchOperation> operations,
                          PreparedDictionary* dictionary, txn_id_t* txn_id,
                          IsolationLevel* session_iso, int fd) {
    const uint16_t operation_count =
        static_cast<uint16_t>(operations.size());
    std::vector<wire::BatchQueryResult> query_results;
    query_results.reserve(operation_count);

    static thread_local uint64_t batch_ordinal = 0;
    ++batch_ordinal;
    EXEC_TRACE("BATCH_BEGIN conn=%d batch=%llu txn=%d ops=%u", fd,
               static_cast<unsigned long long>(batch_ordinal),
               static_cast<int>(*txn_id), static_cast<unsigned>(operation_count));

    for (uint16_t operation_index = 0; operation_index < operation_count;
         ++operation_index) {
        auto prepared = dictionary->find(operations[operation_index].statement_id);
        if (prepared == dictionary->end()) {
            // The full payload was decoded against this same dictionary before
            // execution, so this can only indicate an internal state error.
            abort_open_transaction(txn_id);
            send_batch_failure(operation_count, operation_index,
                               wire::kBatchError,
                               "prepared statement disappeared", fd);
            return;
        }
        try {
            EXEC_TRACE("  OP_BEGIN conn=%d batch=%llu op=%u stmt=%u txn=%d sql=%.160s",
                       fd, static_cast<unsigned long long>(batch_ordinal),
                       static_cast<unsigned>(operation_index),
                       static_cast<unsigned>(operations[operation_index].statement_id),
                       static_cast<int>(*txn_id),
                       prepared->second.request.sql.c_str());
            wire::BatchQueryResult result = execute_prepared_operation(
                operation_index, &prepared->second,
                operations[operation_index].parameters, txn_id, session_iso);
            EXEC_TRACE("  OP_END   conn=%d batch=%llu op=%u stmt=%u txn=%d rows=%zu",
                       fd, static_cast<unsigned long long>(batch_ordinal),
                       static_cast<unsigned>(operation_index),
                       static_cast<unsigned>(operations[operation_index].statement_id),
                       static_cast<int>(*txn_id), result.rows.size());
            if (!prepared->second.columns.empty()) {
                query_results.push_back(std::move(result));
                // Reject an oversized buffered result while the explicit
                // transaction is still active, before a later COMMIT can run.
                ensure_batch_results_fit(query_results);
            }
        } catch (wire::IoError&) {
            abort_open_transaction(txn_id);
            throw;
        } catch (TransactionAbortException& error) {
            abort_open_transaction(txn_id);
            send_batch_failure(operation_count, operation_index,
                               wire::kBatchAbort, error.GetInfo(), fd);
            return;
        } catch (const std::exception& error) {
            abort_open_transaction(txn_id);
            send_batch_failure(operation_count, operation_index,
                               wire::kBatchError, error.what(), fd);
            return;
        } catch (...) {
            abort_open_transaction(txn_id);
            send_batch_failure(operation_count, operation_index,
                               wire::kBatchError, "internal error", fd);
            return;
        }
    }

    EXEC_TRACE("  SERIALIZE_BEGIN conn=%d batch=%llu results=%zu", fd,
               static_cast<unsigned long long>(batch_ordinal),
               query_results.size());
    std::vector<uint8_t> payload = wire::encode_batch_result(
        operation_count, operation_count, wire::kBatchOk, 0xffff, "",
        query_results);
    EXEC_TRACE("  SERIALIZE_END   conn=%d batch=%llu bytes=%zu (about to write_frame)",
               fd, static_cast<unsigned long long>(batch_ordinal), payload.size());
    wire::write_frame(fd, wire::kBatchResult, payload);
    EXEC_TRACE("BATCH_END conn=%d batch=%llu WRITE_COMPLETE", fd,
               static_cast<unsigned long long>(batch_ordinal));
}

static void execute_stream_sql(const std::string& sql, txn_id_t* txn_id,
                               IsolationLevel* session_iso,
                               wire::ResultWriter* writer) {
    Context context(lock_manager.get(), log_manager.get(), nullptr);
    context.session_iso_ = session_iso;
    context.txn_mgr_ = txn_manager.get();
    context.wire_writer_ = writer;
    SetTransaction(txn_id, &context);

    try {
        std::shared_ptr<ast::TreeNode> parse_tree = parse_one_sql(sql);
        std::shared_ptr<Query> query = analyze->do_analyze(parse_tree);
        std::shared_ptr<Plan> plan = optimizer->plan_query(query, &context);
        std::shared_ptr<PortalStmt> portal_stmt = portal->start(plan, &context);
        portal->run(portal_stmt, ql_manager.get(), txn_id, &context);
        portal->drop();

        // ACK/RESULT_END 必须在隐式事务的 COMMIT（包括 WAL 稳定化）之后。
        if (context.txn_->get_txn_mode() == false &&
            context.txn_->get_state() == TransactionState::DEFAULT) {
            txn_manager->commit(context.txn_, context.log_mgr_);
        }
        if (writer->result_active()) {
            writer->finish_result();
        } else {
            writer->command_ok();
        }
    } catch (wire::IoError&) {
        if (context.txn_ != nullptr && !context.txn_->get_txn_mode()) {
            abort_open_transaction(txn_id);
        }
        throw;
    } catch (TransactionAbortException& error) {
        abort_open_transaction(txn_id);
        writer->transaction_abort(error.GetInfo());
    } catch (RMDBError& error) {
        if (context.txn_ != nullptr && !context.txn_->get_txn_mode()) {
            abort_open_transaction(txn_id);
        }
        writer->error(error.what());
    } catch (const std::exception& error) {
        if (context.txn_ != nullptr && !context.txn_->get_txn_mode()) {
            abort_open_transaction(txn_id);
        }
        std::cerr << "EXEC_STREAM failed: " << error.what() << std::endl;
        writer->error(error.what());
    } catch (...) {
        if (context.txn_ != nullptr && !context.txn_->get_txn_mode()) {
            abort_open_transaction(txn_id);
        }
        std::cerr << "EXEC_STREAM failed with a non-standard exception"
                  << std::endl;
        writer->error("internal error");
    }
}

static void execute_static_checkpoint(txn_id_t* txn_id,
                                      wire::ResultWriter* writer) {
    try {
        Transaction* current = txn_manager->get_transaction(*txn_id);
        if (current != nullptr &&
            current->get_state() == TransactionState::DEFAULT) {
            txn_manager->commit(current, log_manager.get());
        }
        *txn_id = INVALID_TXN_ID;
        create_static_checkpoint();
        writer->command_ok();
    } catch (TransactionAbortException& error) {
        abort_open_transaction(txn_id);
        writer->transaction_abort(error.GetInfo());
    } catch (const std::exception& error) {
        writer->error(error.what());
    } catch (...) {
        writer->error("checkpoint failed");
    }
}

void *client_handler(void *socket_argument) {
    std::unique_ptr<int> owned_fd(static_cast<int*>(socket_argument));
    int fd = *owned_fd;
    txn_id_t txn_id = INVALID_TXN_ID;
    IsolationLevel session_iso = IsolationLevel::SNAPSHOT_ISOLATION;
    PreparedDictionary prepared_statements;
    wire::ResultWriter writer(fd);
    bool handshake_complete = false;

    std::cout << "establish client connection, sockfd: " << fd << "\n";
    try {
        uint8_t handshake[wire::kHandshakeSize];
        if (!wire::read_exact(fd, handshake, sizeof(handshake), true)) {
            close(fd);
            return nullptr;
        }
        if (std::memcmp(handshake, wire::kHandshake, sizeof(handshake)) != 0) {
            // 不支持的握手按附件 A 直接关闭，不能交给 SQL parser。
            close(fd);
            return nullptr;
        }
        wire::write_all(fd, handshake, sizeof(handshake));
        handshake_complete = true;

        wire::Frame frame;
        while (wire::read_frame(fd, frame)) {
            if (frame.tag == wire::kPrepareSet) {
                try {
                    auto requests = wire::decode_prepare_set(frame.payload);
                    PreparedCandidate candidate = prepare_dictionary_candidate(
                        std::move(requests), &session_iso);
                    // Encoding is part of full validation. Only after it
                    // succeeds does the no-throw swap atomically replace this
                    // connection's old dictionary.
                    std::vector<uint8_t> response =
                        wire::encode_prepare_ok(candidate.schemas);
                    prepared_statements.swap(candidate.dictionary);
                    wire::write_frame(fd, wire::kPrepareOk, response);
                } catch (wire::IoError&) {
                    throw;
                } catch (const std::exception& error) {
                    writer.error(error.what());
                } catch (...) {
                    writer.error("PREPARE_SET failed");
                }
                continue;
            }
            if (frame.tag == wire::kExecBatch) {
                try {
                    // Decode the complete request before attempting operation 0.
                    // A structural failure may use top-level ERROR, but
                    // AUTO_ABORT still requires cleanup before that response.
                    std::vector<BatchOperation> operations =
                        decode_exec_batch(frame.payload, prepared_statements);
                    execute_batch(std::move(operations), &prepared_statements,
                                  &txn_id, &session_iso, fd);
                } catch (wire::IoError&) {
                    throw;
                } catch (const std::exception& error) {
                    abort_open_transaction(&txn_id);
                    writer.error(error.what());
                } catch (...) {
                    abort_open_transaction(&txn_id);
                    writer.error("EXEC_BATCH decode failed");
                }
                continue;
            }

            if (frame.payload.empty()) {
                writer.error("EXEC_STREAM SQL must be non-empty");
                continue;
            }
            if (std::find(frame.payload.begin(), frame.payload.end(), 0) !=
                frame.payload.end()) {
                writer.error("EXEC_STREAM SQL must not contain NUL");
                continue;
            }
            if (!wire::valid_utf8(frame.payload.data(), frame.payload.size())) {
                writer.error("EXEC_STREAM SQL is not valid UTF-8");
                continue;
            }
            std::string sql(frame.payload.begin(), frame.payload.end());
            if (is_static_checkpoint_cmd(sql.c_str())) {
                execute_static_checkpoint(&txn_id, &writer);
            } else {
                execute_stream_sql(sql, &txn_id, &session_iso, &writer);
            }
        }
    } catch (wire::ProtocolError& error) {
        if (handshake_complete) {
            try {
                // A structural error ends this connection. Roll back before
                // exposing the diagnostic so a valid AUTO_ABORT batch header
                // can never observe an ACK-before-rollback ordering.
                abort_open_transaction(&txn_id);
                writer.error(error.what());
            } catch (...) {
            }
        }
    } catch (wire::IoError& error) {
        std::cerr << "client socket ended: " << error.what() << std::endl;
    } catch (const std::exception& error) {
        std::cerr << "client handler failed: " << error.what() << std::endl;
        try {
            writer.error("internal error");
        } catch (...) {
        }
    } catch (...) {
        std::cerr << "client handler failed with a non-standard exception"
                  << std::endl;
    }

    try {
        abort_open_transaction(&txn_id);
    } catch (const std::exception& error) {
        std::cerr << "connection cleanup failed: " << error.what() << std::endl;
    } catch (...) {
        std::cerr << "connection cleanup failed" << std::endl;
    }
    close(fd);
    return nullptr;
}

void start_server() {
    int sockfd_server;
    int fd_temp;
    struct sockaddr_in s_addr_in {};

    // 初始化连接
    sockfd_server = socket(AF_INET, SOCK_STREAM, 0);  // ipv4,TCP
    assert(sockfd_server != -1);
    int val = 1;
    setsockopt(sockfd_server, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

    // before bind(), set the attr of structure sockaddr.
    memset(&s_addr_in, 0, sizeof(s_addr_in));
    s_addr_in.sin_family = AF_INET;
    s_addr_in.sin_addr.s_addr = htonl(INADDR_ANY);
    s_addr_in.sin_port = htons(SOCK_PORT);
    fd_temp = bind(sockfd_server, (struct sockaddr *)(&s_addr_in), sizeof(s_addr_in));
    if (fd_temp == -1) {
        std::cout << "Bind error!" << std::endl;
        exit(1);
    }

    fd_temp = listen(sockfd_server, SOMAXCONN);
    if (fd_temp == -1) {
        std::cout << "Listen error!" << std::endl;
        exit(1);
    }

    while (!should_exit) {
        std::cout << "Waiting for new connection..." << std::endl;
        pthread_t thread_id;
        struct sockaddr_in s_addr_client {};
        socklen_t client_length = sizeof(s_addr_client);

        if (setjmp(jmpbuf)) {
            std::cout << "Break from Server Listen Loop\n";
            break;
        }

        // Block here. Until server accepts a new connection.
        int sockfd = accept(sockfd_server, (struct sockaddr *)(&s_addr_client), (socklen_t *)(&client_length));
        if (sockfd == -1) {
            std::cout << "Accept error!" << std::endl;
            continue;  // ignore current socket ,continue while loop.
        }

        // Wire frames contain small headers and payloads.  Disable Nagle on
        // each accepted connection so a partial frame does not wait for a
        // delayed acknowledgement before the handler can send the rest.
        int tcp_nodelay = 1;
        if (setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &tcp_nodelay,
                       sizeof(tcp_nodelay)) == -1) {
            std::cerr << "Failed to set TCP_NODELAY: " << strerror(errno)
                      << std::endl;
        }
        
        // 和客户端建立连接，并开启一个线程负责处理客户端请求
        // 每个线程独占一个堆上 fd，避免把 accept 循环的栈地址交给并发线程。
        auto* thread_fd = new int(sockfd);
        if (pthread_create(&thread_id, nullptr, &client_handler,
                           static_cast<void*>(thread_fd)) != 0) {
            delete thread_fd;
            close(sockfd);
            std::cout << "Create thread fail!" << std::endl;
            break;  // break while loop
        }
        // 不 join：detach 让已结束线程立即回收栈等资源（joinable 僵尸线程会
        // 一直保留栈，反复短连接下无界累积）
        pthread_detach(thread_id);

    }

    // Clear
    std::cout << " Try to close all client-connection.\n";
    int ret = shutdown(sockfd_server, SHUT_WR);  // shut down the all or part of a full-duplex connection.
    if(ret == -1) { printf("%s\n", strerror(errno)); }
//    assert(ret != -1);
    close(sockfd_server);
    sm_manager->close_db();
    std::cout << " DB has been closed.\n";
    std::cout << "Server shuts down." << std::endl;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        // 需要指定数据库名称
        std::cerr << "Usage: " << argv[0] << " <database>" << std::endl;
        exit(1);
    }

    signal(SIGINT, sigint_handler);
    // 忽略 SIGPIPE：客户端超时或断开后，服务端向已关闭 socket 写入时，
    // 默认信号行为会终止整个进程。忽略后 write 返回 -1/EPIPE，由
    // client_handler 的错误分支关闭当前连接，而服务端继续运行。
    signal(SIGPIPE, SIG_IGN);
    // 单 malloc arena：多线程各开 arena 会让 malloc_trim 难以归还碎片页，
    // 海量自动提交灌库的 RSS 以 ~90B/事务爬升；语句执行本就被解析互斥锁
    // 大体串行化，单 arena 的锁开销可忽略。
    mallopt(M_ARENA_MAX, 1);
    try {
        std::cout << "\n"
                     "  _____  __  __ _____  ____  \n"
                     " |  __ \\|  \\/  |  __ \\|  _ \\ \n"
                     " | |__) | \\  / | |  | | |_) |\n"
                     " |  _  /| |\\/| | |  | |  _ < \n"
                     " | | \\ \\| |  | | |__| | |_) |\n"
                     " |_|  \\_\\_|  |_|_____/|____/ \n"
                     "\n"
                     "Welcome to RMDB!\n"
                     "Type 'help;' for help.\n"
                     "\n";
        // Database name is passed by args
        std::string db_name = argv[1];
        if (!sm_manager->is_dir(db_name)) {
            // Database not found, create a new one
            sm_manager->create_db(db_name);
        }
        // Open database
        sm_manager->open_db(db_name);

        // WAL：注入"写脏页前先刷日志"回调
        buffer_pool_manager->set_log_flush_callback(
            [] { log_manager->flush_log_to_disk(); });

        // recovery database（恢复期间不产生新日志）
        enable_logging.store(false);
        recovery->analyze();
        recovery->redo();
        recovery->undo();

        // 恢复完成，开启 WAL 日志
        enable_logging.store(true);

        // 开启服务端，开始接受客户端连接
        start_server();
    } catch (RMDBError &e) {
        std::cerr << e.what() << std::endl;
        exit(1);
    }
    return 0;
}
