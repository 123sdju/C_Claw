/**
 * 学习导读：cclaw/adapters/src/storage/cc_inmem_memory_store.c
 *
 * 所属层次：适配器层。
 * 阅读重点：这里把端口接口落到具体后端，阅读时重点看协议转换、资源释放和失败降级。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/*
 * cc_inmem_memory_store.c — 纯内存记忆存储适配器（易失性）
 *
 * 模块说明：
 *   基于动态数组在进程堆内存中存储记忆条目。所有数据在进程退出后丢失，
 *   不依赖任何外部存储（无文件 I/O、无数据库）。
 *
 * 设计模式：Adapter（适配器）模式
 *   将内存数组操作适配为 cc_memory_store vtable 接口，
 *   与 JSON 文件存储和 SQLite 存储对外提供完全一致的 API。
 *
 * 实现接口：
 *   - cc_memory_store_vtable_t（7 个虚拟方法：set / get / search / list /
 *     delete_entry / delete_by_category / destroy）
 *
 * 核心数据结构：
 *   - entries 动态数组，初始容量 64，翻倍扩容
 *
 * 适用场景：
 *   - 单元测试（数据隔离、可重复）
 *   - 嵌入式环境（无文件系统支持）
 *   - 临时会话（不需要持久化）
 *
 * 线程安全性：
 *   - 所有读写操作均通过 cc_mutex_t 加锁保护
 *
 * 与 JSON 文件存储的关键差异：
 *   - 不进行文件 I/O，每次写操作不需要 save_to_file
 *   - 数据在进程退出后丢失（易失性存储）
 *   - 搜索使用 strstr 内存遍历（与 JSON 存储相同逻辑）
 */

#include "cc/ports/cc_memory_store.h"
#include "cc/ports/cc_thread.h"
#include <stdlib.h>
#include <string.h>

#define INITIAL_CAP 64

/*
 * cc_inmem_memory_store_t — 内存记忆存储的内部数据结构
 *
 * 字段说明：
 *   entries — 记忆条目动态数组
 *   count   — 当前记忆条目数量
 *   cap     — 数组当前容量（从 64 起步，每次翻倍）
 *   mutex   — 互斥锁，保护并发访问
 */
typedef struct {
    cc_memory_entry_t *entries;
    size_t count;
    size_t cap;
    cc_mutex_t mutex;
} cc_inmem_memory_store_t;

/*
 * inmem_set — vtable 方法：插入或更新一条记忆（覆盖写语义）
 *
 * 功能：先遍历查找是否存在同 key 的条目，
 *       - 存在 → 更新 value/category/updated_at（覆盖写）
 *       - 不存在 → 数组末尾追加，容量不足时翻倍扩容
 *
 * @param self      存储实例指针
 * @param key       记忆键名
 * @param value     记忆值
 * @param category  分类标签（可为 NULL）
 * @param session_id 所属会话 ID（可为 NULL）
 * @return cc_result_t
 */
static cc_result_t inmem_set(void *self, const char *key, const char *value,
                              const char *category, const char *session_id)
{
    cc_inmem_memory_store_t *s = (cc_inmem_memory_store_t *)self;
    cc_mutex_lock(s->mutex);

    for (size_t i = 0; i < s->count; i++) {
        if (strcmp(s->entries[i].key, key) == 0) {
            free(s->entries[i].value);
            s->entries[i].value = strdup(value);
            free(s->entries[i].category);
            s->entries[i].category = category ? strdup(category) : NULL;
            s->entries[i].updated_at = time(NULL);
            cc_mutex_unlock(s->mutex);
            return cc_result_ok();
        }
    }

    if (s->count >= s->cap) {
        s->cap *= 2;
        s->entries = realloc(s->entries, s->cap * sizeof(cc_memory_entry_t));
        if (!s->entries) { cc_mutex_unlock(s->mutex); return cc_result_error(CC_ERR_OUT_OF_MEMORY, "OOM"); }
    }

    cc_memory_entry_t *e = &s->entries[s->count++];
    e->key = strdup(key);
    e->value = strdup(value);
    e->category = category ? strdup(category) : NULL;
    e->session_id = session_id ? strdup(session_id) : NULL;
    e->created_at = time(NULL);
    e->updated_at = e->created_at;

    cc_mutex_unlock(s->mutex);
    return cc_result_ok();
}

/*
 * inmem_get — vtable 方法：按 key 精确查找一条记忆
 *
 * 功能：遍历 entries 数组，按 key 精确匹配（strcmp），
 *       找到后深拷贝所有字段到 out_entry。
 *
 * @param self      存储实例指针
 * @param key       要查找的记忆键名
 * @param out_entry 输出参数，找到时填充完整 entry（深拷贝）
 * @return cc_result_t，找到返回 OK，未找到返回 CC_ERR_NOT_FOUND
 */
static cc_result_t inmem_get(void *self, const char *key, cc_memory_entry_t *out_entry)
{
    cc_inmem_memory_store_t *s = (cc_inmem_memory_store_t *)self;
    cc_mutex_lock(s->mutex);

    for (size_t i = 0; i < s->count; i++) {
        if (strcmp(s->entries[i].key, key) == 0) {
            cc_memory_entry_t *src = &s->entries[i];
            out_entry->key = strdup(src->key);
            out_entry->value = strdup(src->value);
            out_entry->category = src->category ? strdup(src->category) : NULL;
            out_entry->session_id = src->session_id ? strdup(src->session_id) : NULL;
            out_entry->created_at = src->created_at;
            out_entry->updated_at = src->updated_at;
            cc_mutex_unlock(s->mutex);
            return cc_result_ok();
        }
    }

    cc_mutex_unlock(s->mutex);
    return cc_result_errf(CC_ERR_NOT_FOUND, "Memory key not found: %s", key);
}

/*
 * matches_query — 判断记忆条目是否匹配搜索关键词
 *
 * 功能：在条目的 key/value/category 三个字段中使用 strstr 进行 OR 匹配。
 *       - query 为 NULL 或空字符串 → 匹配所有条目（空查询返回全部）
 *       - 任一字段包含 query 子串 → 返回 1
 *
 * @param e     指向记忆条目的指针
 * @param query 搜索关键词
 * @return 匹配成功返回 1，否则返回 0
 */
static int matches_query(const cc_memory_entry_t *e, const char *query)
{
    if (!query || !query[0]) return 1;
    if (e->key && strstr(e->key, query)) return 1;
    if (e->value && strstr(e->value, query)) return 1;
    if (e->category && strstr(e->category, query)) return 1;
    return 0;
}

/*
 * inmem_search — vtable 方法：模糊搜索记忆
 *
 * 功能：遍历所有 entries，调用 matches_query 在每个条目的 key/value/category
 *       三个字段中进行 strstr 子串匹配（大小写敏感，OR 逻辑）。
 *
 * @param self         存储实例指针
 * @param query        搜索关键词（模糊匹配）
 * @param limit        最大返回条数（≤0 表示无限制）
 * @param out_entries  输出参数，结果数组
 * @param out_count    输出参数，实际条目数量
 * @return cc_result_t
 */
static cc_result_t inmem_search(void *self, const char *query, int limit,
                                 cc_memory_entry_t **out_entries, size_t *out_count)
{
    cc_inmem_memory_store_t *s = (cc_inmem_memory_store_t *)self;
    cc_mutex_lock(s->mutex);

    size_t cap = 16;
    cc_memory_entry_t *results = malloc(cap * sizeof(cc_memory_entry_t));
    if (!results) { cc_mutex_unlock(s->mutex); return cc_result_error(CC_ERR_OUT_OF_MEMORY, "OOM"); }

    size_t count = 0;
    for (size_t i = 0; i < s->count && (limit <= 0 || count < (size_t)limit); i++) {
        if (matches_query(&s->entries[i], query)) {
            if (count >= cap) { cap *= 2; results = realloc(results, cap * sizeof(cc_memory_entry_t)); if (!results) break; }
            cc_memory_entry_t *src = &s->entries[i];
            results[count].key = strdup(src->key);
            results[count].value = strdup(src->value);
            results[count].category = src->category ? strdup(src->category) : NULL;
            results[count].session_id = src->session_id ? strdup(src->session_id) : NULL;
            results[count].created_at = src->created_at;
            results[count].updated_at = src->updated_at;
            count++;
        }
    }

    cc_mutex_unlock(s->mutex);
    *out_entries = results;
    *out_count = count;
    return cc_result_ok();
}

/*
 * inmem_list — vtable 方法：列出所有记忆（可按 category 过滤）
 *
 * 功能：遍历所有条目，根据 category 过滤：
 *       - category 为 NULL/空 → 返回全部
 *       - category 非空 → 仅返回 category 精确匹配的条目
 *
 * @param self         存储实例指针
 * @param category     过滤分类
 * @param limit        最大返回条数（≤0 表示无限制）
 * @param out_entries  输出参数
 * @param out_count    输出参数
 * @return cc_result_t
 */
static cc_result_t inmem_list(void *self, const char *category, int limit,
                               cc_memory_entry_t **out_entries, size_t *out_count)
{
    cc_inmem_memory_store_t *s = (cc_inmem_memory_store_t *)self;
    cc_mutex_lock(s->mutex);

    size_t cap = 16;
    cc_memory_entry_t *results = malloc(cap * sizeof(cc_memory_entry_t));
    if (!results) { cc_mutex_unlock(s->mutex); return cc_result_error(CC_ERR_OUT_OF_MEMORY, "OOM"); }

    size_t count = 0;
    for (size_t i = 0; i < s->count && (limit <= 0 || count < (size_t)limit); i++) {
        if (!category || !category[0] ||
            (s->entries[i].category && strcmp(s->entries[i].category, category) == 0)) {
            if (count >= cap) { cap *= 2; results = realloc(results, cap * sizeof(cc_memory_entry_t)); if (!results) break; }
            cc_memory_entry_t *src = &s->entries[i];
            results[count].key = strdup(src->key);
            results[count].value = strdup(src->value);
            results[count].category = src->category ? strdup(src->category) : NULL;
            results[count].session_id = src->session_id ? strdup(src->session_id) : NULL;
            results[count].created_at = src->created_at;
            results[count].updated_at = src->updated_at;
            count++;
        }
    }

    cc_mutex_unlock(s->mutex);
    *out_entries = results;
    *out_count = count;
    return cc_result_ok();
}

/*
 * inmem_delete_entry — vtable 方法：按 key 删除一条记忆
 *
 * 功能：遍历查找目标 key，找到后释放条目资源，
 *       使用 memmove 将后续元素前移，紧凑化数组，count 减 1。
 *
 * @param self 存储实例指针
 * @param key  要删除的记忆键名
 * @return cc_result_t，找到并删除返回 OK，未找到返回 CC_ERR_NOT_FOUND
 */
static cc_result_t inmem_delete_entry(void *self, const char *key)
{
    cc_inmem_memory_store_t *s = (cc_inmem_memory_store_t *)self;
    cc_mutex_lock(s->mutex);

    for (size_t i = 0; i < s->count; i++) {
        if (strcmp(s->entries[i].key, key) == 0) {
            cc_memory_entry_free(&s->entries[i]);
            if (i < s->count - 1)
                memmove(&s->entries[i], &s->entries[i + 1], (s->count - i - 1) * sizeof(cc_memory_entry_t));
            s->count--;
            cc_mutex_unlock(s->mutex);
            return cc_result_ok();
        }
    }

    cc_mutex_unlock(s->mutex);
    return cc_result_error(CC_ERR_NOT_FOUND, "Memory key not found");
}

/*
 * inmem_delete_by_category — vtable 方法：按分类批量删除记忆
 *
 * 功能：使用双指针压缩法（读指针 i / 写指针 write）：
 *       - 匹配 category → 释放资源，不保留
 *       - 不匹配 category → 复制到 write++ 位置
 *       最后将 count 更新为 write。
 *
 * @param self     存储实例指针
 * @param category 要删除的分类标签
 * @return cc_result_t
 */
static cc_result_t inmem_delete_by_category(void *self, const char *category)
{
    cc_inmem_memory_store_t *s = (cc_inmem_memory_store_t *)self;
    cc_mutex_lock(s->mutex);

    size_t write = 0;
    for (size_t i = 0; i < s->count; i++) {
        if (s->entries[i].category && strcmp(s->entries[i].category, category) == 0) {
            cc_memory_entry_free(&s->entries[i]);
        } else {
            if (write != i) s->entries[write] = s->entries[i];
            write++;
        }
    }
    s->count = write;

    cc_mutex_unlock(s->mutex);
    return cc_result_ok();
}

/*
 * inmem_destroy — vtable 方法：销毁存储实例，释放所有资源
 *
 * 功能：
 *   1. 遍历所有 entries 释放字符串字段
 *   2. 释放 entries 数组内存
 *   3. 销毁互斥锁
 *   4. 释放结构体自身
 *
 * @param self 存储实例指针
 */
static void inmem_destroy(void *self)
{
    cc_inmem_memory_store_t *s = (cc_inmem_memory_store_t *)self;
    if (!s) return;
    for (size_t i = 0; i < s->count; i++) cc_memory_entry_free(&s->entries[i]);
    free(s->entries);
    cc_mutex_destroy(s->mutex);
    free(s);
}

/*
 * inmem_vtable — 内存记忆存储的虚函数表
 *
 * 说明：将全部 7 个 vtable 函数指针绑定到对应的纯内存实现。
 *       不涉及任何 I/O 操作，所有逻辑均为内存遍历。
 */
static cc_memory_store_vtable_t inmem_vtable = {
    inmem_set, inmem_get, inmem_search, inmem_list,
    inmem_delete_entry, inmem_delete_by_category, inmem_destroy
};

/*
 * cc_memory_store_create_inmem — 创建内存记忆存储实例（工厂函数）
 *
 * 执行流程：
 *   1. 分配 cc_inmem_memory_store_t 结构体
 *   2. 分配初始容量为 INITIAL_CAP(64) 的 entries 数组
 *   3. 创建互斥锁
 *   4. 绑定 vtable 并返回
 *
 * @param out_store 输出参数
 * @return cc_result_t
 */
cc_result_t cc_memory_store_create_inmem(cc_memory_store_t *out_store)
{
    cc_inmem_memory_store_t *s = calloc(1, sizeof(cc_inmem_memory_store_t));
    if (!s) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate inmem memory store");
    s->cap = INITIAL_CAP;
    s->entries = malloc(s->cap * sizeof(cc_memory_entry_t));
    if (!s->entries) { free(s); return cc_result_error(CC_ERR_OUT_OF_MEMORY, "OOM"); }
    cc_mutex_create(&s->mutex);

    out_store->self = s;
    out_store->vtable = &inmem_vtable;
    return cc_result_ok();
}