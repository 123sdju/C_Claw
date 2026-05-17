/**
 * 学习导读：cclaw/adapters/src/storage/cc_sqlite_session_store.c
 *
 * 所属层次：适配器层。
 * 阅读重点：这里把端口接口落到具体后端，阅读时重点看协议转换、资源释放和失败降级。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * @file cc_sqlite_session_store.c
 * @brief SQLite 会话存储适配器
 *
 * 基于 SQLite3 实现会话状态持久化存储。使用参数化查询防止 SQL 注入，
 * 默认启用 WAL（Write-Ahead Logging）模式以提高并发读写性能。
 *
 * 数据库包含四张表：
 *   - sessions：会话元信息（ID、工作目录、模型、状态等）
 *   - messages：对话消息记录（关联会话、角色、内容、工具调用ID）
 *   - tool_calls：工具调用记录（名称、参数JSON、状态、时间戳）
 *   - tool_results：工具调用结果（成功标志、内容、错误信息、元数据JSON）
 *
 * 完整实现 cc_session_store_vtable 中的全部 7 个虚函数：
 *   create_session / append_message / load_messages / append_tool_call /
 *   append_tool_result / list_sessions / destroy
 *
 * 安全注意：
 *   - 所有 SQL 操作均使用 sqlite3_prepare_v2 + sqlite3_bind_* 参数化查询
 *   - 从不拼接用户输入到 SQL 字符串中
 *   - 数据库文件路径由 build_db_path() 统一构建
 */

#include "cc/ports/cc_session_store.h"
#include "cc/ports/cc_thread.h"
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#ifndef CC_DEFAULT_STORAGE_PATH
#define CC_DEFAULT_STORAGE_PATH "runtime/data/c-claw.db"
#endif

#ifndef CC_DEFAULT_WORKSPACE_PATH
#define CC_DEFAULT_WORKSPACE_PATH "runtime/workspace"
#endif

/**
 * @brief SQLite 会话存储的私有数据结构
 *
 * @field db        打开的 SQLite3 数据库句柄，由 sqlite3_open_v2 创建
 * @field use_wal   是否启用 WAL 日志模式（1=WAL, 0=传统回滚日志）
 */
typedef struct {
    sqlite3 *db;
    int use_wal;
    cc_mutex_t mutex;
} cc_sqlite_session_store_t;

/**
 * @brief 获取当前 UTC 时间的 ISO 8601 格式字符串
 *
 * 内部使用 localtime_r（POSIX）或 localtime_s（Windows）获取线程安全的本地时间，
 * 并以 "%Y-%m-%dT%H:%M:%S" 格式输出。结果通过 strdup 分配，调用者负责释放。
 *
 * @return 堆上新分配的 ISO 8601 时间字符串，调用者须用 free() 释放；失败返回 NULL
 */
static char *now_string(void)
{
    time_t t = time(NULL);
    char buf[64];
#ifdef _WIN32
    struct tm tm_info;
    localtime_s(&tm_info, &t);
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm_info);
#else
    struct tm tm_info;
    localtime_r(&t, &tm_info);
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm_info);
#endif
    return strdup(buf);
}

/**
 * @brief 构建 SQLite 数据库文件的完整路径
 *
 * 如果 db_path 为 NULL，则使用当前工作目录下的 "data/c-claw.db" 作为默认路径。
 * 跨平台处理 Windows（反斜杠分隔符）和 Unix/Linux（正斜杠分隔符）路径。
 *
 * @param db_path 用户指定的数据库路径，可为 NULL 以使用默认路径
 * @return 堆上新分配的路径字符串，调用者须用 free() 释放
 */
static char *build_db_path(const char *db_path)
{
    return strdup(db_path ? db_path : CC_DEFAULT_STORAGE_PATH);
}

/**
 * @brief 设置 SQLite 数据库性能与安全相关的编译指示（PRAGMA）
 *
 * 依次设置以下编译指示：
 *   - journal_mode=WAL：默认启用 Write-Ahead Logging，提高并发读写性能
 *   - foreign_keys=ON：启用外键约束检查，确保数据引用完整性
 *   - busy_timeout=5000：当数据库被锁时等待 5 秒再报错，而非立即失败
 *   - synchronous=NORMAL：在 WAL 模式下平衡安全性与写入性能
 *   - cache_size=-8000：分配约 8MB 的页面缓存（负值表示 KB 为单位）
 *
 * 如果 use_wal 为 0（调用者要求不使用 WAL），则在末尾将 journal_mode 回退为 DELETE。
 *
 * 安全注意：PRAGMA 命令不接收用户输入，均为硬编码的安全配置值。
 *
 * @param db      已打开的 SQLite3 数据库句柄
 * @param use_wal 是否启用 WAL 模式（非零启用，零禁用）
 */
static void setup_pragmas(sqlite3 *db, int use_wal)
{
    sqlite3_exec(db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA foreign_keys=ON", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA busy_timeout=5000", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA synchronous=NORMAL", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA cache_size=-8000", NULL, NULL, NULL);
    if (!use_wal) {
        sqlite3_exec(db, "PRAGMA journal_mode=DELETE", NULL, NULL, NULL);
    }
}

/**
 * @brief 数据库初始化 SQL 脚本
 *
 * 创建四张核心表和三条索引：
 *   - sessions：会话表，以 id 为主键，存储名称、工作目录、模型、状态和时间戳
 *   - messages：消息表，关联 session_id 外键，存储角色、内容、工具调用ID和时间戳
 *   - tool_calls：工具调用表，关联 session_id 外键，存储名称、参数JSON、状态和时间戳
 *   - tool_results：工具结果表，关联 tool_call_id 外键，存储成功标志、内容、错误和元数据
 *
 * 索引：
 *   - idx_messages_session：按会话+时间排序加速消息查询
 *   - idx_tool_calls_session：按会话+时间排序加速工具调用查询
 *   - idx_tool_results_tool_call：按工具调用ID加速结果查找
 */
static const char *init_sql =
    "CREATE TABLE IF NOT EXISTS sessions ("
    "  id TEXT PRIMARY KEY,"
    "  name TEXT,"
    "  workspace_dir TEXT,"
    "  model TEXT,"
    "  status TEXT,"
    "  created_at TEXT,"
    "  updated_at TEXT"
    ");"
    "CREATE TABLE IF NOT EXISTS messages ("
    "  id TEXT PRIMARY KEY,"
    "  session_id TEXT NOT NULL,"
    "  role TEXT NOT NULL,"
    "  content TEXT,"
    "  tool_calls_json TEXT,"
    "  reasoning_content TEXT,"
    "  tool_call_id TEXT,"
    "  created_at TEXT,"
    "  FOREIGN KEY(session_id) REFERENCES sessions(id)"
    ");"
    "CREATE TABLE IF NOT EXISTS tool_calls ("
    "  id TEXT PRIMARY KEY,"
    "  session_id TEXT NOT NULL,"
    "  name TEXT NOT NULL,"
    "  arguments_json TEXT,"
    "  status TEXT,"
    "  created_at TEXT,"
    "  finished_at TEXT,"
    "  FOREIGN KEY(session_id) REFERENCES sessions(id)"
    ");"
    "CREATE TABLE IF NOT EXISTS tool_results ("
    "  id TEXT PRIMARY KEY,"
    "  tool_call_id TEXT NOT NULL,"
    "  ok INTEGER,"
    "  content TEXT,"
    "  error TEXT,"
    "  metadata_json TEXT,"
    "  created_at TEXT,"
    "  FOREIGN KEY(tool_call_id) REFERENCES tool_calls(id)"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_messages_session ON messages(session_id, created_at);"
    "CREATE INDEX IF NOT EXISTS idx_tool_calls_session ON tool_calls(session_id, created_at);"
    "CREATE INDEX IF NOT EXISTS idx_tool_results_tool_call ON tool_results(tool_call_id);";

/**
 * @brief vtable 函数：创建新会话
 *
 * 使用 INSERT OR IGNORE 语义——如果 session_id 已存在则静默跳过，不会报错。
 * SQL 语句：
 *   INSERT OR IGNORE INTO sessions (id, workspace_dir, status, created_at, updated_at)
 *   VALUES (?1, ?2, 'active', ?3, ?3)
 * 新会话默认 status='active'，created_at 和 updated_at 均设为当前时间。
 *
 * 安全注意：全部使用 sqlite3_bind_text 参数绑定，不存在 SQL 注入风险。
 *
 * @param self          存储实例指针
 * @param session_id    新会话的唯一标识符
 * @param workspace_dir 工作目录路径，如果为 NULL 则使用 profile workspace path
 * @return cc_result_t  成功返回 OK，失败返回含错误信息的 STORAGE 错误
 */
static cc_result_t sqlite_create_session(
    void *self,
    const char *session_id,
    const char *workspace_dir
)
{
    cc_sqlite_session_store_t *store = (cc_sqlite_session_store_t *)self;
    char *now = now_string();

    cc_mutex_lock(store->mutex);
    sqlite3_stmt *stmt = NULL;
    const char *sql = "INSERT OR IGNORE INTO sessions (id, workspace_dir, status, created_at, updated_at) "
                      "VALUES (?1, ?2, 'active', ?3, ?3)";
    int rc = sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        cc_result_t result = cc_result_error(CC_ERR_STORAGE, sqlite3_errmsg(store->db));
        free(now);
        cc_mutex_unlock(store->mutex);
        return result;
    }

    sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2,
        workspace_dir ? workspace_dir : CC_DEFAULT_WORKSPACE_PATH,
        -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, now, -1, SQLITE_STATIC);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    free(now);

    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        cc_result_t result = cc_result_error(CC_ERR_STORAGE, sqlite3_errmsg(store->db));
        cc_mutex_unlock(store->mutex);
        return result;
    }
    cc_mutex_unlock(store->mutex);
    return cc_result_ok();
}

/**
 * @brief vtable 函数：追加消息到指定会话
 *
 * SQL 语句：
 *   INSERT INTO messages (id, session_id, role, content, tool_call_id, created_at)
 *   VALUES (?1, ?2, ?3, ?4, ?5, ?6)
 * 消息角色通过 cc_message_role_string() 转换为字符串存储，
 * tool_call_id 为可选字段（例如普通 user/assistant 消息可为空字符串）。
 *
 * 安全注意：所有字段均使用参数绑定；strdup 分配的 now 字符串在函数结束时释放。
 *
 * @param self    存储实例指针
 * @param message 要追加的消息对象，包含 id、session_id、role、content、tool_call_id
 * @return cc_result_t 成功返回 OK，失败返回 STORAGE 错误
 */
static cc_result_t sqlite_append_message(
    void *self,
    const cc_message_t *message
)
{
    cc_sqlite_session_store_t *store = (cc_sqlite_session_store_t *)self;
    char *now = now_string();
    const char *role_str = cc_message_role_string(message->role);

    cc_mutex_lock(store->mutex);
    sqlite3_stmt *stmt = NULL;
    const char *sql = "INSERT INTO messages (id, session_id, role, content, tool_calls_json, reasoning_content, tool_call_id, created_at) "
                      "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)";
    int rc = sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        cc_result_t result = cc_result_error(CC_ERR_STORAGE, sqlite3_errmsg(store->db));
        free(now);
        cc_mutex_unlock(store->mutex);
        return result;
    }

    sqlite3_bind_text(stmt, 1, message->id ? message->id : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, message->session_id ? message->session_id : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, role_str, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, message->content ? message->content : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 5, message->tool_calls_json ? message->tool_calls_json : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 6, message->reasoning_content ? message->reasoning_content : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 7, message->tool_call_id ? message->tool_call_id : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 8, now, -1, SQLITE_STATIC);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    free(now);

    if (rc != SQLITE_DONE) {
        cc_result_t result = cc_result_error(CC_ERR_STORAGE, sqlite3_errmsg(store->db));
        cc_mutex_unlock(store->mutex);
        return result;
    }
    cc_mutex_unlock(store->mutex);
    return cc_result_ok();
}

/**
 * @brief 安全的 realloc 封装
 *
 * 当 realloc 失败返回 NULL 时不会丢失原始指针，而是保持原指针不变。
 * 调用者需要通过检查返回值是否等于原指针来判断扩容是否失败。
 *
 * @param ptr      原始内存指针
 * @param new_size 新的内存块大小（字节）
 * @return 新分配的内存指针（成功），或原指针（扩容失败时保持原状）
 */
static void *safe_realloc(void *ptr, size_t new_size)
{
    void *new_ptr = realloc(ptr, new_size);
    return new_ptr ? new_ptr : ptr;
}

/**
 * @brief vtable 函数：加载指定会话的消息列表
 *
 * SQL 语句：
 *   SELECT id, session_id, role, content, tool_call_id, created_at
 *   FROM messages WHERE session_id = ?1
 *   ORDER BY created_at ASC LIMIT ?2
 * 按创建时间升序排列，保证消息的时间顺序正确。limit 参数控制最大返回条数。
 *
 * 角色字符串（system/user/assistant/tool）将转换为 cc_message_role 枚举。
 * 使用动态数组扩容策略：初始容量 16，每次翻倍，直到读完所有结果或达到 limit。
 *
 * @param self         存储实例指针
 * @param session_id   目标会话ID
 * @param limit        最大返回消息数
 * @param out_messages 输出参数，指向结果消息数组的指针
 * @param out_count    输出参数，实际返回的消息数量
 * @return cc_result_t 成功返回 OK，失败（数据库错误/内存不足）返回相应错误码
 */
static cc_result_t sqlite_load_messages(
    void *self,
    const char *session_id,
    int limit,
    cc_message_t **out_messages,
    size_t *out_count
)
{
    cc_sqlite_session_store_t *store = (cc_sqlite_session_store_t *)self;

    cc_mutex_lock(store->mutex);
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT id, session_id, role, content, tool_calls_json, reasoning_content, tool_call_id, created_at "
                      "FROM messages WHERE session_id = ?1 "
                      "ORDER BY created_at ASC LIMIT ?2";
    int rc = sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        cc_result_t result = cc_result_error(CC_ERR_STORAGE, sqlite3_errmsg(store->db));
        cc_mutex_unlock(store->mutex);
        return result;
    }

    sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, limit);

    size_t cap = 16;
    size_t count = 0;
    cc_message_t *messages = calloc(cap, sizeof(cc_message_t));
    if (!messages) {
        sqlite3_finalize(stmt);
        cc_mutex_unlock(store->mutex);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate messages");
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= cap) {
            void *new_ptr = safe_realloc(messages, cap * 2 * sizeof(cc_message_t));
            if (new_ptr == messages && cap * 2 > cap) {
                sqlite3_finalize(stmt);
                for (size_t i = 0; i < count; i++) {
                    cc_message_cleanup(&messages[i]);
                }
                free(messages);
                cc_mutex_unlock(store->mutex);
                return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to grow messages");
            }
            messages = new_ptr;
            cap *= 2;
        }

        cc_message_t *msg = &messages[count];
        memset(msg, 0, sizeof(cc_message_t));

        const char *id = (const char *)sqlite3_column_text(stmt, 0);
        const char *sid = (const char *)sqlite3_column_text(stmt, 1);
        const char *role_str = (const char *)sqlite3_column_text(stmt, 2);
        const char *content = (const char *)sqlite3_column_text(stmt, 3);
        const char *tcj = (const char *)sqlite3_column_text(stmt, 4);
        const char *reasoning = (const char *)sqlite3_column_text(stmt, 5);
        const char *tci = (const char *)sqlite3_column_text(stmt, 6);

        msg->id = id ? strdup(id) : NULL;
        msg->session_id = sid ? strdup(sid) : NULL;
        msg->content = content ? strdup(content) : NULL;
        msg->tool_calls_json = (tcj && tcj[0]) ? strdup(tcj) : NULL;
        msg->reasoning_content = (reasoning && reasoning[0]) ? strdup(reasoning) : NULL;
        msg->tool_call_id = (tci && tci[0]) ? strdup(tci) : NULL;

        if (role_str) {
            if (strcmp(role_str, "system") == 0) msg->role = CC_ROLE_SYSTEM;
            else if (strcmp(role_str, "user") == 0) msg->role = CC_ROLE_USER;
            else if (strcmp(role_str, "assistant") == 0) msg->role = CC_ROLE_ASSISTANT;
            else if (strcmp(role_str, "tool") == 0) msg->role = CC_ROLE_TOOL;
        }

        count++;
    }
    sqlite3_finalize(stmt);

    *out_messages = messages;
    *out_count = count;
    cc_mutex_unlock(store->mutex);
    return cc_result_ok();
}

/**
 * @brief vtable 函数：记录工具调用
 *
 * SQL 语句：
 *   INSERT INTO tool_calls (id, session_id, name, arguments_json, status, created_at)
 *   VALUES (?1, ?2, ?3, ?4, 'completed', ?5)
 * 工具调用默认 status='completed'，arguments_json 存储工具参数的 JSON 字符串。
 *
 * 安全注意：arguments_json 虽然来源于 AI 模型的输出，但通过参数绑定存储，
 * 不会参与 SQL 解析，不存在二阶注入风险。
 *
 * @param self       存储实例指针
 * @param session_id 关联的会话ID
 * @param call       工具调用对象，包含 id、name、arguments_json
 * @return cc_result_t 成功返回 OK，失败返回 STORAGE 错误
 */
static cc_result_t sqlite_append_tool_call(
    void *self,
    const char *session_id,
    const cc_tool_call_t *call
)
{
    cc_sqlite_session_store_t *store = (cc_sqlite_session_store_t *)self;
    char *now = now_string();

    cc_mutex_lock(store->mutex);
    sqlite3_stmt *stmt = NULL;
    const char *sql = "INSERT INTO tool_calls (id, session_id, name, arguments_json, status, created_at) "
                      "VALUES (?1, ?2, ?3, ?4, 'completed', ?5)";
    int rc = sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        cc_result_t result = cc_result_error(CC_ERR_STORAGE, sqlite3_errmsg(store->db));
        free(now);
        cc_mutex_unlock(store->mutex);
        return result;
    }

    sqlite3_bind_text(stmt, 1, call->id ? call->id : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, session_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, call->name ? call->name : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, call->arguments_json ? call->arguments_json : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 5, now, -1, SQLITE_STATIC);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    free(now);

    if (rc != SQLITE_DONE) {
        cc_result_t result = cc_result_error(CC_ERR_STORAGE, sqlite3_errmsg(store->db));
        cc_mutex_unlock(store->mutex);
        return result;
    }
    cc_mutex_unlock(store->mutex);
    return cc_result_ok();
}

/**
 * @brief vtable 函数：记录工具调用结果
 *
 * SQL 语句：
 *   INSERT INTO tool_results (id, tool_call_id, ok, content, error, metadata_json, created_at)
 *   VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)
 * ok 字段为布尔值（1/0），表示工具调用是否成功。
 * content 存储成功时的输出内容，error 存储失败时的错误信息，两者互斥。
 * metadata_json 存储附加的元数据（如执行耗时、资源使用等）。
 *
 * 安全注意：id 直接使用 tool_call_id 作为主键，content/error/metadata_json 全部参数绑定。
 * session_id 参数在 SQLite 实现中未使用（通过 tool_call_id 关联即可溯源会话）。
 *
 * @param self         存储实例指针
 * @param session_id   关联的会话ID（当前实现中未直接使用）
 * @param tool_call_id 关联的工具调用ID，同时作为结果的主键
 * @param result       工具调用结果对象，包含 ok、content、error、metadata_json
 * @return cc_result_t 成功返回 OK，失败返回 STORAGE 错误
 */
static cc_result_t sqlite_append_tool_result(
    void *self,
    const char *session_id,
    const char *tool_call_id,
    const cc_tool_result_t *result
)
{
    cc_sqlite_session_store_t *store = (cc_sqlite_session_store_t *)self;
    char *now = now_string();
    (void)session_id;

    cc_mutex_lock(store->mutex);
    sqlite3_stmt *stmt = NULL;
    const char *sql = "INSERT INTO tool_results (id, tool_call_id, ok, content, error, metadata_json, created_at) "
                      "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)";
    int rc = sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        cc_result_t result = cc_result_error(CC_ERR_STORAGE, sqlite3_errmsg(store->db));
        free(now);
        cc_mutex_unlock(store->mutex);
        return result;
    }

    sqlite3_bind_text(stmt, 1, tool_call_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, tool_call_id, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 3, result->ok ? 1 : 0);
    sqlite3_bind_text(stmt, 4, result->content ? result->content : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 5, result->error ? result->error : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 6, result->metadata_json ? result->metadata_json : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 7, now, -1, SQLITE_STATIC);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    free(now);

    if (rc != SQLITE_DONE) {
        cc_result_t result = cc_result_error(CC_ERR_STORAGE, sqlite3_errmsg(store->db));
        cc_mutex_unlock(store->mutex);
        return result;
    }
    cc_mutex_unlock(store->mutex);
    return cc_result_ok();
}

/**
 * @brief vtable 函数：列出所有会话
 *
 * SQL 语句：
 *   SELECT id, name, workspace_dir, model, status, created_at, updated_at
 *   FROM sessions ORDER BY updated_at DESC
 * 按更新时间降序排列，最近活跃的会话排在最前面。
 *
 * 返回值仅包含 id、name、workspace_dir、model 四个字段，
 * status/created_at/updated_at 等时间戳字段在结果中未使用（调用者可通过其他接口获取）。
 *
 * @param self          存储实例指针
 * @param out_sessions  输出参数，指向结果会话数组的指针
 * @param out_count     输出参数，实际返回的会话数量
 * @return cc_result_t  成功返回 OK，失败返回相应错误码
 */
static cc_result_t sqlite_list_sessions(
    void *self,
    cc_session_t **out_sessions,
    size_t *out_count
)
{
    cc_sqlite_session_store_t *store = (cc_sqlite_session_store_t *)self;

    cc_mutex_lock(store->mutex);
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT id, name, workspace_dir, model, status, created_at, updated_at "
                      "FROM sessions ORDER BY updated_at DESC";
    int rc = sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        cc_result_t result = cc_result_error(CC_ERR_STORAGE, sqlite3_errmsg(store->db));
        cc_mutex_unlock(store->mutex);
        return result;
    }

    size_t cap = 16;
    size_t count = 0;
    cc_session_t *sessions = calloc(cap, sizeof(cc_session_t));
    if (!sessions) {
        sqlite3_finalize(stmt);
        cc_mutex_unlock(store->mutex);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate sessions");
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= cap) {
            void *new_ptr = safe_realloc(sessions, cap * 2 * sizeof(cc_session_t));
            if (new_ptr == sessions && cap * 2 > cap) {
                sqlite3_finalize(stmt);
                for (size_t i = 0; i < count; i++) {
                    free(sessions[i].id);
                    free(sessions[i].name);
                    free(sessions[i].workspace_dir);
                    free(sessions[i].model);
                    free(sessions[i].created_at);
                    free(sessions[i].updated_at);
                }
                free(sessions);
                cc_mutex_unlock(store->mutex);
                return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to grow sessions");
            }
            sessions = new_ptr;
            cap *= 2;
        }

        cc_session_t *s = &sessions[count];
        memset(s, 0, sizeof(cc_session_t));

        const char *id = (const char *)sqlite3_column_text(stmt, 0);
        const char *name = (const char *)sqlite3_column_text(stmt, 1);
        const char *ws = (const char *)sqlite3_column_text(stmt, 2);
        const char *model = (const char *)sqlite3_column_text(stmt, 3);

        s->id = id ? strdup(id) : NULL;
        s->name = name ? strdup(name) : NULL;
        s->workspace_dir = ws ? strdup(ws) : NULL;
        s->model = model ? strdup(model) : NULL;
        count++;
    }
    sqlite3_finalize(stmt);

    *out_sessions = sessions;
    *out_count = count;
    cc_mutex_unlock(store->mutex);
    return cc_result_ok();
}

/**
 * @brief vtable 函数：销毁存储实例，释放所有资源
 *
 * 在关闭数据库连接前，先执行 PRAGMA optimize 以优化数据库文件布局，
 * 然后调用 sqlite3_close() 安全关闭连接。最后释放存储结构体自身。
 *
 * @param self 存储实例指针（将调用 free 释放）
 */
static void sqlite_destroy(void *self)
{
    cc_sqlite_session_store_t *store = (cc_sqlite_session_store_t *)self;
    cc_mutex_lock(store->mutex);
    if (store->db) {
        sqlite3_exec(store->db, "PRAGMA analysis_limit=0", NULL, NULL, NULL);
        sqlite3_exec(store->db, "PRAGMA optimize", NULL, NULL, NULL);
        sqlite3_close(store->db);
    }
    cc_mutex_unlock(store->mutex);
    cc_mutex_destroy(store->mutex);
    free(store);
}

/**
 * @brief SQLite 会话存储的虚函数表
 *
 * 将全部 7 个 vtable 函数绑定到对应的 SQLite 实现：
 *   create_session → sqlite_create_session
 *   append_message → sqlite_append_message
 *   load_messages  → sqlite_load_messages
 *   append_tool_call → sqlite_append_tool_call
 *   append_tool_result → sqlite_append_tool_result
 *   list_sessions  → sqlite_list_sessions
 *   destroy        → sqlite_destroy
 */
static cc_session_store_vtable_t sqlite_vtable = {
    sqlite_create_session,
    sqlite_append_message,
    sqlite_load_messages,
    sqlite_append_tool_call,
    sqlite_append_tool_result,
    sqlite_list_sessions,
    sqlite_destroy
};

/**
 * @brief 创建 SQLite 会话存储实例（公共工厂函数）
 *
 * 执行流程：
 *   1. 分配 cc_sqlite_session_store_t 结构体
 *   2. 解析数据库文件路径（调用 build_db_path）
 *   3. 以读写+创建+完整互斥模式打开数据库（SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX）
 *   4. 设置 PRAGMA（WAL、外键、超时、同步模式、缓存）
 *   5. 执行 init_sql 建表脚本
 *   6. 填充输出参数 out_store 的 self 和 vtable
 *
 * 安全注意：
 *   - 使用 SQLITE_OPEN_FULLMUTEX 确保多线程安全
 *   - 任何步骤失败都会正确清理已分配的资源（close db + free self）
 *
 * @param db_path   数据库文件路径，可为 NULL 使用默认路径
 * @param out_store 输出参数，填充创建好的存储实例
 * @return cc_result_t 成功返回 OK，失败返回相应错误码
 */
cc_result_t cc_sqlite_session_store_create(const char *db_path, cc_session_store_t *out_store)
{
    cc_sqlite_session_store_t *self = calloc(1, sizeof(cc_sqlite_session_store_t));
    if (!self) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create SQLite store");

    cc_result_t mutex_rc = cc_mutex_create(&self->mutex);
    if (mutex_rc.code != CC_OK) {
        free(self);
        return mutex_rc;
    }

    char *resolved_path = build_db_path(db_path);

    int rc = sqlite3_open_v2(resolved_path, &self->db,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        NULL);
    free(resolved_path);

    if (rc != SQLITE_OK) {
        const char *err = sqlite3_errmsg(self->db);
        cc_result_t result = cc_result_error(CC_ERR_STORAGE, err ? err : "Cannot open database");
        if (self->db) sqlite3_close(self->db);
        cc_mutex_destroy(self->mutex);
        free(self);
        return result;
    }

    setup_pragmas(self->db, 1);

    char *err = NULL;
    rc = sqlite3_exec(self->db, init_sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        cc_result_t result = cc_result_error(CC_ERR_STORAGE, err ? err : "Failed to init tables");
        sqlite3_free(err);
        sqlite3_close(self->db);
        cc_mutex_destroy(self->mutex);
        free(self);
        return result;
    }

    out_store->self = self;
    out_store->vtable = &sqlite_vtable;
    return cc_result_ok();
}
