



#include "cc/ports/cc_memory_store.h"
#include "cc/util/cc_json.h"
#include "cc/ports/cc_thread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * JSON 文件 memory store 私有状态。
 *
 * 启动时把文件加载到 entries 数组，后续 set/delete 会把整个数组重新写回文件。mutex
 * 保护内存数组和文件写入临界区；该实现简单可读，适合学习和小规模持久化。
 */
typedef struct {
    char *file_path;
    cc_memory_entry_t *entries;
    size_t count;
    size_t cap;
    cc_mutex_t mutex;
} cc_json_memory_store_t;

/*
 * 将当前内存 entries 保存到 JSON 文件。
 *
 * 这是 best-effort helper：写文件失败不会把错误返回给上层，因为旧接口签名是 void。
 * 更严格的生产实现应把 I/O 错误向上返回，或采用临时文件 + rename 的原子写入策略。
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
 * 从 JSON 文件加载 entries。
 *
 * 文件不存在或解析失败时保持空 store；每条记录会深拷贝 key/value/category/session_id。
 * 这个函数只在创建阶段调用，因此没有单独加锁。
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
 * 写入或更新一条长期记忆。
 *
 * key 已存在则更新 value/category/updated_at 并保存文件；key 不存在则追加新 entry。
 * session_id 在首次创建时记录，用于后续按会话过滤或调试。
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
 * 按 key 读取一条长期记忆。
 *
 * 成功后 out_entry 中所有字符串为深拷贝，调用方用 cc_memory_entry_free 释放。
 * 未找到返回 CC_ERR_NOT_FOUND。
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
 * 简单子串匹配 helper。
 *
 * JSON 文件后端不做向量检索，只在 key/value/category 上做 strstr；needle 为空时不会匹配。
 */
static int match_query(const char *haystack, const char *needle)
{
    return haystack && strstr(haystack, needle);
}

/*
 * 旧版 search API：按 query 子串扫描 JSON memory entries。
 *
 * 返回数组由调用方释放。该实现保留为轻量 fallback；结构化 query/score 更完整的实现可以
 * 由其它 adapter 提供。
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
 * 按 category 列举 entries。
 *
 * category 为空表示全部，limit <= 0 表示不限。返回数据是深拷贝，调用方不持有内部数组。
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
 * 删除指定 key 的 entry。
 *
 * 删除后压缩数组并立即保存文件；未找到返回 CC_ERR_NOT_FOUND，便于 memory tool 给用户
 * 明确反馈。
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
 * 删除某个 category 下的所有 entries。
 *
 * 使用 write/read 下标原地压缩；匹配项先释放，不匹配项前移。最后保存整个文件。
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
 * 销毁 JSON memory store。
 *
 * 调用方需保证没有并发操作；函数释放 entries、file_path、mutex 和私有对象。
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

/* JSON 文件 memory store vtable；当前未实现结构化 query 扩展槽时由 core fallback 处理。 */
static cc_memory_store_vtable_t json_vtable = {
    json_set, json_get, json_search, json_list,
    json_delete_entry, json_delete_by_category, json_destroy
};

/*
 * 创建 JSON 文件 memory store。
 *
 * 成功后 out_store 获得 self/vtable；file_path 由调用方传入并被深拷贝。该函数加载已有
 * 文件内容，后续写操作会覆盖保存整个数组。
 */
cc_result_t cc_memory_store_create_json_file(cc_memory_store_t *out_store, const char *file_path)
{
    if (!out_store || !file_path)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid json file memory store arguments");
    memset(out_store, 0, sizeof(*out_store));

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
