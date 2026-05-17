/**
 * 学习导读：cclaw/adapters/src/storage/cc_sqlite_memory_store.c
 *
 * 所属层次：适配器层。
 * 阅读重点：这里把端口接口落到具体后端，阅读时重点看协议转换、资源释放和失败降级。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/*
 * cc_sqlite_memory_store.c — SQLite 记忆存储适配器
 *
 * 模块说明：
 *   基于 SQLite3 数据库实现 cc_memory_store_vtable 接口的持久化记忆存储。
 *   使用参数化查询（sqlite3_prepare_v2 + sqlite3_bind_*）防止 SQL 注入。
 *
 * 设计模式：Adapter（适配器）模式
 *   将 SQLite 数据库操作适配为 cc_memory_store vtable 接口，
 *   上层 memory 工具无需关心底层是 JSON 文件、SQLite 还是内存存储。
 *
 * 实现接口：
 *   - cc_memory_store_vtable_t（7 个虚拟方法：set / get / search / list /
 *     delete_entry / delete_by_category / destroy）
 *
 * 数据库表结构：
 *   memory 表包含以下字段：
 *     - key（TEXT PRIMARY KEY）：唯一键名，作为主键
 *     - value（TEXT NOT NULL）：记忆值
 *     - category（TEXT）：分类标签
 *     - session_id（TEXT）：所属会话 ID
 *     - created_at（INTEGER）：创建时间戳（Unix epoch）
 *     - updated_at（INTEGER）：更新时间戳（Unix epoch）
 *   同时创建了 category 和 session_id 上的索引以加速查询。
 *
 * 与 JSON 文件存储的对比：
 *   - SQLite 支持增量写入和事务，适合大数据量场景
 *   - JSON 文件存储每次写操作需全量序列化，适合小数据量
 *   - SQLite 支持 LIKE 搜索，JSON 文件使用 strstr 内存遍历
 *
 * 线程安全性：
 *   - 所有读/写操作均通过 cc_mutex_t 加锁保护
 *
 * 安全注意：
 *   - 所有 SQL 参数使用 sqlite3_bind_* 绑定，从不拼接用户输入到 SQL 字符串
 */

#include "cc/ports/cc_memory_store.h"
#include "cc/ports/cc_thread.h"
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/*
 * cc_sqlite_memory_store_t — SQLite 记忆存储的内部数据结构
 *
 * 字段说明：
 *   db    — SQLite3 数据库连接句柄
 *   mutex — 互斥锁，保护并发访问的线程安全
 */
typedef struct {
    sqlite3 *db;
    cc_mutex_t mutex;
} cc_sqlite_memory_store_t;

/*
 * init_db — 初始化数据库表结构
 *
 * 功能：执行 CREATE TABLE IF NOT EXISTS 创建 memory 表：
 *       - key 为主键（PRIMARY KEY），确保键名唯一
 *       - value 为 NOT NULL，保证数据完整性
 *       - 创建 category 和 session_id 上的索引以加速查询
 *
 * @param db SQLite3 数据库连接
 * @return SQLITE_OK 表示成功，其他值表示建表失败
 */
static int init_db(sqlite3 *db)
{
    const char *sql =
        "CREATE TABLE IF NOT EXISTS memory ("
        "  key TEXT PRIMARY KEY,"
        "  value TEXT NOT NULL,"
        "  category TEXT,"
        "  session_id TEXT,"
        "  created_at INTEGER NOT NULL,"
        "  updated_at INTEGER NOT NULL"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_memory_category ON memory(category);"
        "CREATE INDEX IF NOT EXISTS idx_memory_session ON memory(session_id);";
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        if (err) sqlite3_free(err);
        return rc;
    }
    return SQLITE_OK;
}

/*
 * sqlite_set — vtable 方法：插入或更新一条记忆（UPSERT 语义）
 *
 * 功能：使用 INSERT OR REPLACE 实现覆盖写入：
 *       - 若 key 已存在 → 更新 value/category/session_id/updated_at，
 *         保持 created_at 不变（通过 COALESCE + 子查询实现）
 *       - 若 key 不存在 → 插入新行，created_at 和 updated_at 设为当前时间
 *
 * SQL 语句：
 *   INSERT OR REPLACE INTO memory (key, value, category, session_id, created_at, updated_at)
 *   VALUES (?, ?, ?, ?, COALESCE((SELECT created_at FROM memory WHERE key=?), ?), ?);
 *
 * 安全注意：所有值通过 sqlite3_bind_* 参数化绑定，防止 SQL 注入。
 *
 * @param self      存储实例指针
 * @param key       记忆键名
 * @param value     记忆值
 * @param category  分类标签（可为 NULL）
 * @param session_id 所属会话 ID（可为 NULL）
 * @return cc_result_t
 */
static cc_result_t sqlite_set(void *self, const char *key, const char *value,
                               const char *category, const char *session_id)
{
    cc_sqlite_memory_store_t *s = (cc_sqlite_memory_store_t *)self;
    cc_mutex_lock(s->mutex);

    time_t now = time(NULL);
    const char *sql = "INSERT OR REPLACE INTO memory (key, value, category, session_id, created_at, updated_at) "
                      "VALUES (?, ?, ?, ?, COALESCE((SELECT created_at FROM memory WHERE key=?), ?), ?);";
    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(s->db, sql, -1, &stmt, NULL);
    if (!stmt) { cc_mutex_unlock(s->mutex); return cc_result_error(CC_ERR_STORAGE, "SQL prepare failed"); }

    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, value, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, category, category ? -1 : 0, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, session_id, session_id ? -1 : 0, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, key, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 6, (sqlite3_int64)now);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    cc_mutex_unlock(s->mutex);
    if (rc != SQLITE_DONE)
        return cc_result_errf(CC_ERR_STORAGE, "SQLite set failed: %s", sqlite3_errmsg(s->db));
    return cc_result_ok();
}

/*
 * sqlite_get — vtable 方法：按 key 精确查找一条记忆
 *
 * 功能：使用 SELECT + WHERE key=? 查询单条记录。
 *       找到后从行数据中填充 cc_memory_entry_t 的各个字段（深拷贝字符串）。
 *
 * @param self      存储实例指针
 * @param key       要查找的记忆键名
 * @param out_entry 输出参数，找到时填充完整 entry
 * @return cc_result_t，找到返回 OK，未找到返回 CC_ERR_NOT_FOUND
 */
static cc_result_t sqlite_get(void *self, const char *key, cc_memory_entry_t *out_entry)
{
    cc_sqlite_memory_store_t *s = (cc_sqlite_memory_store_t *)self;
    cc_mutex_lock(s->mutex);

    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(s->db, "SELECT key, value, category, session_id, created_at, updated_at FROM memory WHERE key=?",
                       -1, &stmt, NULL);
    if (!stmt) { cc_mutex_unlock(s->mutex); return cc_result_error(CC_ERR_STORAGE, "SQL prepare failed"); }

    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
    cc_result_t result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        out_entry->key = strdup((const char *)sqlite3_column_text(stmt, 0));
        out_entry->value = strdup((const char *)sqlite3_column_text(stmt, 1));
        const char *cat = (const char *)sqlite3_column_text(stmt, 2);
        out_entry->category = cat ? strdup(cat) : NULL;
        const char *sid = (const char *)sqlite3_column_text(stmt, 3);
        out_entry->session_id = sid ? strdup(sid) : NULL;
        out_entry->created_at = (time_t)sqlite3_column_int64(stmt, 4);
        out_entry->updated_at = (time_t)sqlite3_column_int64(stmt, 5);
        result = cc_result_ok();
    } else {
        result = cc_result_errf(CC_ERR_NOT_FOUND, "Memory key not found: %s", key);
    }

    sqlite3_finalize(stmt);
    cc_mutex_unlock(s->mutex);
    return result;
}

/*
 * bind_search_query — 绑定搜索查询参数
 *
 * 功能：构造 LIKE 匹配模式（%query%），绑定到预编译的搜索 statment 中。
 *       三个 LIKE 分别匹配 key、value、category 字段，limit 限制返回行数。
 *
 * @param stmt  预编译的 SQL statment
 * @param query 搜索关键词
 * @param limit 最大返回行数
 */
static void bind_search_query(sqlite3_stmt *stmt, const char *query, int limit)
{
    char like[1024];
    snprintf(like, sizeof(like), "%%%s%%", query);
    sqlite3_bind_text(stmt, 1, like, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, like, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, like, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, limit);
}

/*
 * sqlite_search — vtable 方法：模糊搜索记忆
 *
 * 功能：使用 SELECT + WHERE key LIKE ? OR value LIKE ? OR category LIKE ? LIMIT ?
 *       进行 LIKE 模糊匹配搜索。三个字段 OR 逻辑，任一匹配即命中。
 *
 * SQL 使用参数化查询，LIKE 模式通过 snprintf 构造（%query%），
 * 但由于 query 本身通过 sqlite3_bind_text 绑定，不会产生 SQL 注入。
 *
 * @param self         存储实例指针
 * @param query        搜索关键词
 * @param limit        最大返回条数（≤0 时默认 100）
 * @param out_entries  输出参数，匹配结果数组
 * @param out_count    输出参数，实际条目数量
 * @return cc_result_t
 */
static cc_result_t sqlite_search(void *self, const char *query, int limit,
                                  cc_memory_entry_t **out_entries, size_t *out_count)
{
    cc_sqlite_memory_store_t *s = (cc_sqlite_memory_store_t *)self;
    cc_mutex_lock(s->mutex);

    const char *sql = "SELECT key, value, category, session_id, created_at, updated_at FROM memory "
                      "WHERE key LIKE ? OR value LIKE ? OR category LIKE ? LIMIT ?";
    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(s->db, sql, -1, &stmt, NULL);
    if (!stmt) { cc_mutex_unlock(s->mutex); return cc_result_error(CC_ERR_STORAGE, "SQL prepare failed"); }

    bind_search_query(stmt, query, limit > 0 ? limit : 100);

    size_t cap = 16, count = 0;
    cc_memory_entry_t *results = malloc(cap * sizeof(cc_memory_entry_t));
    if (!results) { sqlite3_finalize(stmt); cc_mutex_unlock(s->mutex); return cc_result_error(CC_ERR_OUT_OF_MEMORY, "OOM"); }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= cap) { cap *= 2; results = realloc(results, cap * sizeof(cc_memory_entry_t)); if (!results) break; }
        results[count].key = strdup((const char *)sqlite3_column_text(stmt, 0));
        results[count].value = strdup((const char *)sqlite3_column_text(stmt, 1));
        const char *cat = (const char *)sqlite3_column_text(stmt, 2);
        results[count].category = cat ? strdup(cat) : NULL;
        const char *sid = (const char *)sqlite3_column_text(stmt, 3);
        results[count].session_id = sid ? strdup(sid) : NULL;
        results[count].created_at = (time_t)sqlite3_column_int64(stmt, 4);
        results[count].updated_at = (time_t)sqlite3_column_int64(stmt, 5);
        count++;
    }

    sqlite3_finalize(stmt);
    cc_mutex_unlock(s->mutex);
    *out_entries = results;
    *out_count = count;
    return cc_result_ok();
}

/*
 * sqlite_list — vtable 方法：列出所有记忆（可按 category 过滤）
 *
 * 功能：
 *   - category 非空时：SELECT WHERE category=? 精确过滤
 *   - category 为 NULL/空时：SELECT 全表扫描，返回所有条目
 *   受 limit 参数约束，limit ≤ 0 时返回全部。
 *
 * @param self         存储实例指针
 * @param category     过滤分类（可为 NULL 表示不过滤）
 * @param limit        最大返回条数
 * @param out_entries  输出参数
 * @param out_count    输出参数
 * @return cc_result_t
 */
static cc_result_t sqlite_list(void *self, const char *category, int limit,
                                cc_memory_entry_t **out_entries, size_t *out_count)
{
    cc_sqlite_memory_store_t *s = (cc_sqlite_memory_store_t *)self;
    cc_mutex_lock(s->mutex);

    const char *sql;
    if (category && category[0])
        sql = "SELECT key, value, category, session_id, created_at, updated_at FROM memory WHERE category=?";
    else
        sql = "SELECT key, value, category, session_id, created_at, updated_at FROM memory";

    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(s->db, sql, -1, &stmt, NULL);
    if (!stmt) { cc_mutex_unlock(s->mutex); return cc_result_error(CC_ERR_STORAGE, "SQL prepare failed"); }

    if (category && category[0])
        sqlite3_bind_text(stmt, 1, category, -1, SQLITE_TRANSIENT);

    size_t cap = 16, count = 0;
    cc_memory_entry_t *results = malloc(cap * sizeof(cc_memory_entry_t));
    if (!results) { sqlite3_finalize(stmt); cc_mutex_unlock(s->mutex); return cc_result_error(CC_ERR_OUT_OF_MEMORY, "OOM"); }

    while (sqlite3_step(stmt) == SQLITE_ROW && (limit <= 0 || count < (size_t)limit)) {
        if (count >= cap) { cap *= 2; results = realloc(results, cap * sizeof(cc_memory_entry_t)); if (!results) break; }
        results[count].key = strdup((const char *)sqlite3_column_text(stmt, 0));
        results[count].value = strdup((const char *)sqlite3_column_text(stmt, 1));
        const char *cat = (const char *)sqlite3_column_text(stmt, 2);
        results[count].category = cat ? strdup(cat) : NULL;
        const char *sid = (const char *)sqlite3_column_text(stmt, 3);
        results[count].session_id = sid ? strdup(sid) : NULL;
        results[count].created_at = (time_t)sqlite3_column_int64(stmt, 4);
        results[count].updated_at = (time_t)sqlite3_column_int64(stmt, 5);
        count++;
    }

    sqlite3_finalize(stmt);
    cc_mutex_unlock(s->mutex);
    *out_entries = results;
    *out_count = count;
    return cc_result_ok();
}

/*
 * sqlite_delete_entry — vtable 方法：按 key 删除一条记忆
 *
 * SQL：DELETE FROM memory WHERE key=?
 * 即使 key 不存在也返回 OK（幂等操作，不报错）。
 *
 * @param self 存储实例指针
 * @param key  要删除的记忆键名
 * @return cc_result_t
 */
static cc_result_t sqlite_delete_entry(void *self, const char *key)
{
    cc_sqlite_memory_store_t *s = (cc_sqlite_memory_store_t *)self;
    cc_mutex_lock(s->mutex);

    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(s->db, "DELETE FROM memory WHERE key=?", -1, &stmt, NULL);
    if (!stmt) { cc_mutex_unlock(s->mutex); return cc_result_error(CC_ERR_STORAGE, "SQL prepare failed"); }

    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    cc_mutex_unlock(s->mutex);
    return cc_result_ok();
}

/*
 * sqlite_delete_by_category — vtable 方法：按分类批量删除记忆
 *
 * SQL：DELETE FROM memory WHERE category=?
 *
 * @param self     存储实例指针
 * @param category 要删除的分类标签
 * @return cc_result_t
 */
static cc_result_t sqlite_delete_by_category(void *self, const char *category)
{
    cc_sqlite_memory_store_t *s = (cc_sqlite_memory_store_t *)self;
    cc_mutex_lock(s->mutex);

    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(s->db, "DELETE FROM memory WHERE category=?", -1, &stmt, NULL);
    if (!stmt) { cc_mutex_unlock(s->mutex); return cc_result_error(CC_ERR_STORAGE, "SQL prepare failed"); }

    sqlite3_bind_text(stmt, 1, category, -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    cc_mutex_unlock(s->mutex);
    return cc_result_ok();
}

/*
 * sqlite_destroy — vtable 方法：销毁存储实例，释放所有资源
 *
 * 功能：
 *   1. 加锁后调用 sqlite3_close 关闭数据库连接
 *   2. 销毁互斥锁
 *   3. 释放结构体自身内存
 *
 * @param self 存储实例指针
 */
static void sqlite_destroy(void *self)
{
    cc_sqlite_memory_store_t *s = (cc_sqlite_memory_store_t *)self;
    if (!s) return;
    cc_mutex_lock(s->mutex);
    if (s->db) sqlite3_close(s->db);
    cc_mutex_unlock(s->mutex);
    cc_mutex_destroy(s->mutex);
    free(s);
}

/*
 * sqlite_vtable — SQLite 记忆存储的虚函数表
 *
 * 说明：将全部 7 个 vtable 函数指针绑定到对应的 SQLite 参数化查询实现。
 */
static cc_memory_store_vtable_t sqlite_vtable = {
    sqlite_set, sqlite_get, sqlite_search, sqlite_list,
    sqlite_delete_entry, sqlite_delete_by_category, sqlite_destroy
};

/*
 * cc_memory_store_create_sqlite — 创建 SQLite 记忆存储实例（工厂函数）
 *
 * 执行流程：
 *   1. 校验参数（out_store 和 db_path 非空）
 *   2. 分配 cc_sqlite_memory_store_t 结构体
 *   3. 调用 sqlite3_open 打开数据库文件
 *   4. 创建互斥锁
 *   5. 调用 init_db 创建表结构和索引
 *   6. 填充 cc_memory_store_t 输出参数
 *
 * 参数：
 *   out_store — 输出参数
 *   db_path   — SQLite 数据库文件路径
 * @return cc_result_t
 */
cc_result_t cc_memory_store_create_sqlite(cc_memory_store_t *out_store, const char *db_path)
{
    if (!out_store || !db_path)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid sqlite memory store arguments");

    cc_sqlite_memory_store_t *s = calloc(1, sizeof(cc_sqlite_memory_store_t));
    if (!s) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate sqlite memory store");

    int rc = sqlite3_open(db_path, &s->db);
    if (rc != SQLITE_OK) {
        const char *msg = sqlite3_errmsg(s->db);
        sqlite3_close(s->db);
        free(s);
        return cc_result_errf(CC_ERR_STORAGE, "SQLite open failed: %s", msg);
    }

    cc_mutex_create(&s->mutex);
    init_db(s->db);

    out_store->self = s;
    out_store->vtable = &sqlite_vtable;
    return cc_result_ok();
}