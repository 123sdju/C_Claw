/**
 * 学习导读：cclaw/adapters/src/storage/cc_json_file_memory_store.c
 *
 * 所属层次：适配器层。
 * 阅读重点：这里把端口接口落到具体后端，阅读时重点看协议转换、资源释放和失败降级。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/*
 * cc_json_file_memory_store.c — JSON 文件记忆存储适配器
 *
 * 模块说明：
 *   基于单个 JSON 文件实现 cc_memory_store_vtable 接口的持久化记忆存储。
 *   所有记忆条目以 JSON 数组形式存储在文件中，每次写操作都会全量序列化回写。
 *
 * 设计模式：Adapter（适配器）模式
 *   将 JSON 文件 I/O 适配为 cc_memory_store vtable 接口，
 *   上层 memory 工具无需关心底层是 JSON 文件、SQLite 还是内存存储。
 *
 * 实现接口：
 *   - cc_memory_store_vtable_t（7 个虚拟方法：set / get / search / list /
 *     delete_entry / delete_by_category / destroy）
 *
 * 数据格式：
 *   JSON 文件中存储一个 JSON 数组，每个元素包含以下字段：
 *     - key：记忆键名（string）
 *     - value：记忆值（string）
 *     - category：分类标签（string，可选）
 *     - session_id：所属会话 ID（string，可选）
 *     - created_at：创建时间戳（number，Unix epoch）
 *     - updated_at：更新时间戳（number，Unix epoch）
 *
 * 读写策略：
 *   - 启动时：load_from_file() 一次性从 JSON 文件加载全部数据到内存数组
 *   - 每次写操作：save_to_file() 全量序列化回写 JSON 文件
 *   - 适用数据量较小（< 万条级别）的场景
 *
 * 线程安全性：
 *   - 读/写操作均通过 cc_mutex_t（mutex）加锁保护
 *
 * 安全注意：
 *   - JSON 解析由 cc_json 库处理，不受用户输入影响
 *   - 文件路径在创建时固定，后续不受外部输入影响
 */

#include "cc/ports/cc_memory_store.h"
#include "cc/util/cc_json.h"
#include "cc/ports/cc_thread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * cc_json_memory_store_t — JSON 文件记忆存储的内部数据结构
 *
 * 字段说明：
 *   file_path — JSON 持久化文件的完整路径
 *   entries   — 内存中的记忆条目动态数组
 *   count     — 当前记忆条目数量
 *   cap       — entries 数组的当前容量（自动扩容）
 *   mutex     — 互斥锁，保护并发访问的线程安全
 */
typedef struct {
    char *file_path;
    cc_memory_entry_t *entries;
    size_t count;
    size_t cap;
    cc_mutex_t mutex;
} cc_json_memory_store_t;

/*
 * save_to_file — 将内存中的记忆数据全量序列化回写到 JSON 文件
 *
 * 功能：
 *   1. 创建 JSON 数组，遍历内存中所有 entries
 *   2. 将每个 entry 转换为 JSON 对象（key/value/category/session_id/时间戳）
 *   3. 序列化为 JSON 字符串，以覆盖模式写入文件
 *
 * 性能注意：
 *   - 全量回写策略适用于小数据量场景（< 万条），大量数据时应考虑增量更新
 *   - 写操作在 mutex 保护下进行（由调用方持有锁）
 *
 * @param s 存储实例指针
 */
static void save_to_file(cc_json_memory_store_t *s)
{
    cc_json_value_t *arr = cc_json_create_array();
    for (size_t i = 0; i < s->count; i++) {
        cc_json_value_t *obj = cc_json_create_object();
        cc_json_object_set(obj, "key", cc_json_create_string(s->entries[i].key));
        cc_json_object_set(obj, "value", cc_json_create_string(s->entries[i].value));
        if (s->entries[i].category)
            cc_json_object_set(obj, "category", cc_json_create_string(s->entries[i].category));
        if (s->entries[i].session_id)
            cc_json_object_set(obj, "session_id", cc_json_create_string(s->entries[i].session_id));
        cc_json_object_set(obj, "created_at", cc_json_create_number((double)s->entries[i].created_at));
        cc_json_object_set(obj, "updated_at", cc_json_create_number((double)s->entries[i].updated_at));
        cc_json_array_append(arr, obj);
    }
    char *json_str = cc_json_stringify(arr);
    cc_json_destroy(arr);
    if (json_str) {
        FILE *f = fopen(s->file_path, "w");
        if (f) { fputs(json_str, f); fclose(f); }
        free(json_str);
    }
}

/*
 * load_from_file — 从 JSON 文件加载记忆数据到内存数组
 *
 * 功能：
 *   1. 打开 JSON 文件，读取全部内容到字符串缓冲区
 *   2. 解析 JSON 顶层数组，遍历每个元素
 *   3. 将每个 JSON 对象转换为 cc_memory_entry_t，填充 entries 数组
 *   4. 创建时间戳缺失或为 0 时自动设为当前时间，保证新写回的数据完整
 *
 * 容错机制：
 *   - 文件不存在时直接返回（fopen 失败 → return），不报错
 *   - JSON 解析失败时释放临时缓冲区，返回空状态
 *   - 初始容量至少为 64，若数据多于 64 则分配 len+16 容量
 *
 * @param s 存储实例指针
 */
static void load_from_file(cc_json_memory_store_t *s)
{
    FILE *f = fopen(s->file_path, "r");
    if (!f) return;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *content = malloc(size + 1);
    if (!content) { fclose(f); return; }
    size_t n = fread(content, 1, size, f);
    fclose(f);
    content[n] = '\0';

    cc_json_value_t *arr = NULL;
    if (cc_json_parse(content, &arr).code != CC_OK || !arr) {
        free(content);
        return;
    }
    free(content);

    size_t len = (size_t)cc_json_array_size(arr);
    s->cap = len > 64 ? len + 16 : 64;
    s->entries = malloc(s->cap * sizeof(cc_memory_entry_t));
    if (!s->entries) { cc_json_destroy(arr); return; }

    for (size_t i = 0; i < len; i++) {
        cc_json_value_t *obj = cc_json_array_get(arr, i);
        if (!obj) continue;
        const char *key = cc_json_string_value(cc_json_object_get(obj, "key"));
        const char *value = cc_json_string_value(cc_json_object_get(obj, "value"));
        if (!key || !value) continue;
        cc_memory_entry_t *e = &s->entries[s->count];
        memset(e, 0, sizeof(*e));
        e->key = strdup(key);
        e->value = strdup(value);
        if (!e->key || !e->value) {
            free(e->key);
            free(e->value);
            memset(e, 0, sizeof(*e));
            continue;
        }
        const char *cat = cc_json_string_value(cc_json_object_get(obj, "category"));
        e->category = cat ? strdup(cat) : NULL;
        const char *sid = cc_json_string_value(cc_json_object_get(obj, "session_id"));
        e->session_id = sid ? strdup(sid) : NULL;
        e->created_at = (time_t)cc_json_int_value(cc_json_object_get(obj, "created_at"));
        e->updated_at = (time_t)cc_json_int_value(cc_json_object_get(obj, "updated_at"));
        if (e->created_at == 0) e->created_at = time(NULL);
        if (e->updated_at == 0) e->updated_at = e->created_at;
        s->count++;
    }
    cc_json_destroy(arr);
}

/*
 * json_set — vtable 方法：设置/更新一条记忆
 *
 * 功能：
 *   - 若 key 已存在，更新其 value/category/updated_at（实现覆盖写语义）
 *   - 若 key 不存在，在数组末尾追加新条目
 *   - 数组满时翻倍扩容（2x 策略）
 *
 * 流程：加锁 → 遍历查找 → 更新或新建 → save_to_file → 解锁
 *
 * @param self      存储实例指针
 * @param key       记忆键名
 * @param value     记忆值（不可为 NULL）
 * @param category  分类标签（可为 NULL）
 * @param session_id 所属会话 ID（可为 NULL）
 * @return cc_result_t
 */
static cc_result_t json_set(void *self, const char *key, const char *value,
                             const char *category, const char *session_id)
{
    cc_json_memory_store_t *s = (cc_json_memory_store_t *)self;
    cc_mutex_lock(s->mutex);

    for (size_t i = 0; i < s->count; i++) {
        if (strcmp(s->entries[i].key, key) == 0) {
            free(s->entries[i].value);
            s->entries[i].value = strdup(value);
            free(s->entries[i].category);
            s->entries[i].category = category ? strdup(category) : NULL;
            s->entries[i].updated_at = time(NULL);
            save_to_file(s);
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

    save_to_file(s);
    cc_mutex_unlock(s->mutex);
    return cc_result_ok();
}

/*
 * json_get — vtable 方法：按 key 精确查找一条记忆
 *
 * 功能：遍历 entries 数组，按 key 做字符串精确匹配（strcmp）。
 *       找到后深拷贝所有字段到 out_entry（调用者负责释放）。
 *
 * 流程：加锁 → 遍历匹配 → 深拷贝 → 解锁
 *
 * @param self      存储实例指针
 * @param key       要查找的记忆键名
 * @param out_entry 输出参数，找到时填充完整 entry（深拷贝）
 * @return cc_result_t，找到返回 OK，未找到返回 CC_ERR_NOT_FOUND
 */
static cc_result_t json_get(void *self, const char *key, cc_memory_entry_t *out_entry)
{
    cc_json_memory_store_t *s = (cc_json_memory_store_t *)self;
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
 * match_query — 子串匹配辅助函数
 *
 * 功能：在 haystack 中查找 needle 子串（使用 strstr 进行大小写敏感的模糊匹配）。
 *       haystack 为 NULL 时视为不匹配。
 *
 * @param haystack 被搜索的字符串（可为 NULL）
 * @param needle   搜索关键词
 * @return 匹配成功返回 1，否则返回 0
 */
static int match_query(const char *haystack, const char *needle)
{
    return haystack && strstr(haystack, needle);
}

/*
 * json_search — vtable 方法：模糊搜索记忆
 *
 * 功能：遍历所有 entries，在 key/value/category 三个字段中使用 strstr
 *       进行子串匹配（大小写敏感，OR 逻辑——任一字段匹配即命中）。
 *
 * 结果限制：最多返回 limit 条匹配结果，未设置（≤0）则返回全部。
 *
 * 流程：加锁 → 遍历匹配 → 深拷贝结果 → 解锁
 *
 * @param self         存储实例指针
 * @param query        搜索关键词（模糊匹配）
 * @param limit        最大返回条数（≤0 表示无限制）
 * @param out_entries  输出参数，匹配结果数组（调用者负责调用 cc_memory_entry_free_array 释放）
 * @param out_count    输出参数，实际匹配的条目数量
 * @return cc_result_t
 */
static cc_result_t json_search(void *self, const char *query, int limit,
                                cc_memory_entry_t **out_entries, size_t *out_count)
{
    cc_json_memory_store_t *s = (cc_json_memory_store_t *)self;
    cc_mutex_lock(s->mutex);

    size_t cap = 16;
    cc_memory_entry_t *results = malloc(cap * sizeof(cc_memory_entry_t));
    if (!results) { cc_mutex_unlock(s->mutex); return cc_result_error(CC_ERR_OUT_OF_MEMORY, "OOM"); }

    size_t count = 0;
    for (size_t i = 0; i < s->count && (limit <= 0 || count < (size_t)limit); i++) {
        cc_memory_entry_t *e = &s->entries[i];
        if (match_query(e->key, query) || match_query(e->value, query) || match_query(e->category, query)) {
            if (count >= cap) { cap *= 2; results = realloc(results, cap * sizeof(cc_memory_entry_t)); if (!results) break; }
            results[count].key = strdup(e->key);
            results[count].value = strdup(e->value);
            results[count].category = e->category ? strdup(e->category) : NULL;
            results[count].session_id = e->session_id ? strdup(e->session_id) : NULL;
            results[count].created_at = e->created_at;
            results[count].updated_at = e->updated_at;
            count++;
        }
    }

    cc_mutex_unlock(s->mutex);
    *out_entries = results;
    *out_count = count;
    return cc_result_ok();
}

/*
 * json_list — vtable 方法：列出所有记忆（可按 category 过滤）
 *
 * 功能：遍历所有 entries，根据 category 参数过滤：
 *       - category 为 NULL 或空字符串 → 返回全部条目
 *       - category 非空 → 仅返回 category 字段精确匹配的条目
 *
 * 结果限制：最多返回 limit 条结果，未设置（≤0）则返回全部。
 *
 * 流程：加锁 → 遍历筛选 → 深拷贝结果 → 解锁
 *
 * @param self         存储实例指针
 * @param category     过滤分类（可为 NULL 表示不过滤）
 * @param limit        最大返回条数（≤0 表示无限制）
 * @param out_entries  输出参数，结果数组
 * @param out_count    输出参数，实际条目数量
 * @return cc_result_t
 */
static cc_result_t json_list(void *self, const char *category, int limit,
                              cc_memory_entry_t **out_entries, size_t *out_count)
{
    cc_json_memory_store_t *s = (cc_json_memory_store_t *)self;
    cc_mutex_lock(s->mutex);

    size_t cap = 16;
    cc_memory_entry_t *results = malloc(cap * sizeof(cc_memory_entry_t));
    if (!results) { cc_mutex_unlock(s->mutex); return cc_result_error(CC_ERR_OUT_OF_MEMORY, "OOM"); }

    size_t count = 0;
    for (size_t i = 0; i < s->count && (limit <= 0 || count < (size_t)limit); i++) {
        cc_memory_entry_t *e = &s->entries[i];
        if (!category || !category[0] || (e->category && strcmp(e->category, category) == 0)) {
            if (count >= cap) { cap *= 2; results = realloc(results, cap * sizeof(cc_memory_entry_t)); if (!results) break; }
            results[count].key = strdup(e->key);
            results[count].value = strdup(e->value);
            results[count].category = e->category ? strdup(e->category) : NULL;
            results[count].session_id = e->session_id ? strdup(e->session_id) : NULL;
            results[count].created_at = e->created_at;
            results[count].updated_at = e->updated_at;
            count++;
        }
    }

    cc_mutex_unlock(s->mutex);
    *out_entries = results;
    *out_count = count;
    return cc_result_ok();
}

/*
 * json_delete_entry — vtable 方法：按 key 删除一条记忆
 *
 * 功能：遍历 entries 数组，按 key 精确匹配（strcmp）找到目标条目后：
 *       1. 释放该条目的内部字符串
 *       2. 将后续元素前移（memmove），紧凑化数组
 *       3. count 减 1
 *       4. 回写到 JSON 文件
 *
 * 流程：加锁 → 遍历匹配 → 删除 + memmove → save_to_file → 解锁
 *
 * @param self 存储实例指针
 * @param key  要删除的记忆键名
 * @return cc_result_t，找到并删除返回 OK，未找到返回 CC_ERR_NOT_FOUND
 */
static cc_result_t json_delete_entry(void *self, const char *key)
{
    cc_json_memory_store_t *s = (cc_json_memory_store_t *)self;
    cc_mutex_lock(s->mutex);

    for (size_t i = 0; i < s->count; i++) {
        if (strcmp(s->entries[i].key, key) == 0) {
            cc_memory_entry_free(&s->entries[i]);
            if (i < s->count - 1)
                memmove(&s->entries[i], &s->entries[i + 1], (s->count - i - 1) * sizeof(cc_memory_entry_t));
            s->count--;
            save_to_file(s);
            cc_mutex_unlock(s->mutex);
            return cc_result_ok();
        }
    }

    cc_mutex_unlock(s->mutex);
    return cc_result_error(CC_ERR_NOT_FOUND, "Memory key not found");
}

/*
 * json_delete_by_category — vtable 方法：按分类批量删除记忆
 *
 * 功能：遍历 entries 数组，使用双指针（读指针 i / 写指针 write）进行压缩：
 *       1. category 匹配的条目 → 释放资源（cc_memory_entry_free），跳过不复制
 *       2. category 不匹配的条目 → 保留，复制到 write 位置
 *       3. 最后将 count 更新为 write，回写到 JSON 文件
 *
 * 流程：加锁 → 遍历压缩 → save_to_file → 解锁
 *
 * @param self     存储实例指针
 * @param category 要删除的分类标签
 * @return cc_result_t
 */
static cc_result_t json_delete_by_category(void *self, const char *category)
{
    cc_json_memory_store_t *s = (cc_json_memory_store_t *)self;
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
    save_to_file(s);

    cc_mutex_unlock(s->mutex);
    return cc_result_ok();
}

/*
 * json_destroy — vtable 方法：销毁存储实例，释放所有资源
 *
 * 功能：
 *   1. 遍历并释放所有 entries 的字符串字段
 *   2. 释放 entries 数组和 file_path 字符串
 *   3. 销毁互斥锁，最后释放结构体自身
 *
 * @param self 存储实例指针
 */
static void json_destroy(void *self)
{
    cc_json_memory_store_t *s = (cc_json_memory_store_t *)self;
    if (!s) return;
    for (size_t i = 0; i < s->count; i++) cc_memory_entry_free(&s->entries[i]);
    free(s->entries);
    free(s->file_path);
    cc_mutex_destroy(s->mutex);
    free(s);
}

/*
 * json_vtable — JSON 文件记忆存储的虚函数表
 *
 * 说明：将全部 7 个 vtable 函数指针绑定到对应的 JSON 文件实现。
 *       上层 memory 工具通过此 vtable 调用，无需感知存储后端细节。
 */
static cc_memory_store_vtable_t json_vtable = {
    json_set, json_get, json_search, json_list,
    json_delete_entry, json_delete_by_category, json_destroy
};

/*
 * cc_memory_store_create_json_file — 创建 JSON 文件记忆存储实例（工厂函数）
 *
 * 执行流程：
 *   1. 校验参数（out_store 和 file_path 非空）
 *   2. 分配 cc_json_memory_store_t 结构体
 *   3. 保存文件路径，分配初始容量为 64 的 entries 数组
 *   4. 创建互斥锁
 *   5. 调用 load_from_file 从磁盘加载已有数据（文件不存在则跳过）
 *   6. 填充 cc_memory_store_t 输出参数（self + vtable）
 *
 * 参数：
 *   out_store — 输出参数，填充创建好的存储实例
 *   file_path — JSON 持久化文件路径
 * @return cc_result_t
 */
cc_result_t cc_memory_store_create_json_file(cc_memory_store_t *out_store, const char *file_path)
{
    if (!out_store || !file_path)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid json file memory store arguments");

    cc_json_memory_store_t *s = calloc(1, sizeof(cc_json_memory_store_t));
    if (!s) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate json memory store");

    s->file_path = strdup(file_path);
    s->cap = 64;
    s->entries = malloc(s->cap * sizeof(cc_memory_entry_t));
    if (!s->entries) { free(s->file_path); free(s); return cc_result_error(CC_ERR_OUT_OF_MEMORY, "OOM"); }

    cc_mutex_create(&s->mutex);
    load_from_file(s);

    out_store->self = s;
    out_store->vtable = &json_vtable;
    return cc_result_ok();
}
