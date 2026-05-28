



#include "cc/ports/cc_session_store.h"
#include "cc/ports/cc_thread.h"
#include <stdlib.h>
#include <string.h>

#ifndef CC_DEFAULT_WORKSPACE_PATH
#define CC_DEFAULT_WORKSPACE_PATH "runtime/workspace"
#endif

#define INITIAL_CAPACITY 64

/*
 * 内存版 tool call 记录。
 *
 * session store 不能直接保存调用方传入的 cc_tool_call_t 指针，因为 runtime 可能在返回后
 * 释放原对象；这里把需要持久化的字段全部深拷贝成独立字符串。
 */
typedef struct {
    char *id;
    char *session_id;
    char *name;
    char *arguments_json;
    char *status;
} cc_memory_tool_call_record_t;

/*
 * 内存版 tool result 记录。
 *
 * artifacts 被序列化为 JSON 字符串保存，避免内存 store 需要理解 artifact list 的内部
 * 生命周期。读取路径目前主要服务测试和调试，完整持久化 adapter 可以选择更结构化存储。
 */
typedef struct {
    char *id;
    char *session_id;
    char *tool_call_id;
    int ok;
    char *text;
    char *error;
    char *metadata;
    char *artifacts;
} cc_memory_tool_result_record_t;

/*
 * 内存 session store 的私有状态。
 *
 * 该 adapter 使用动态数组保存 session/message/tool 记录，并用一个 mutex 保护所有数组。
 * 这适合单进程测试或轻量嵌入式 Linux 场景；如果需要掉电保存，应替换为 JSON/SQLite
 * 等持久化 adapter。
 */
typedef struct {
    cc_message_t *messages;
    size_t message_count;
    size_t message_cap;

    cc_session_t *sessions;
    size_t session_count;
    size_t session_cap;

    cc_memory_tool_call_record_t *tool_calls;
    size_t tool_call_count;
    size_t tool_call_cap;

    cc_memory_tool_result_record_t *tool_results;
    size_t tool_result_count;
    size_t tool_result_cap;

    cc_mutex_t mutex;
} cc_memory_session_store_t;

/* 复制必填字符串字段；NULL 会落成空串，简化记录结构的释放逻辑。 */
static char *dup_required_string(const char *value)
{
    return strdup(value ? value : "");
}

/* 释放一条 tool call 记录里的所有深拷贝字段，并清零避免压缩数组时重复释放。 */
static void free_tool_call_record(cc_memory_tool_call_record_t *record)
{
    if (!record) return;
    free(record->id);
    free(record->session_id);
    free(record->name);
    free(record->arguments_json);
    free(record->status);
    memset(record, 0, sizeof(*record));
}

/* 释放一条 tool result 记录，包括文本、错误、metadata 和 artifact JSON 字符串。 */
static void free_tool_result_record(cc_memory_tool_result_record_t *record)
{
    if (!record) return;
    free(record->id);
    free(record->session_id);
    free(record->tool_call_id);
    free(record->text);
    free(record->error);
    free(record->metadata);
    free(record->artifacts);
    memset(record, 0, sizeof(*record));
}

/*
 * 确保 tool call 数组有可写空间。
 *
 * 调用方必须已经持有 store mutex；函数只负责扩容和初始化新槽位，不修改 count。
 */
static cc_result_t ensure_tool_call_capacity(cc_memory_session_store_t *store)
{
    if (store->tool_call_count < store->tool_call_cap) return cc_result_ok();
    size_t new_cap = store->tool_call_cap ? store->tool_call_cap * 2 : INITIAL_CAPACITY;
    cc_memory_tool_call_record_t *new_records =
        realloc(store->tool_calls, new_cap * sizeof(cc_memory_tool_call_record_t));
    if (!new_records) {
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to grow tool calls");
    }
    memset(new_records + store->tool_call_cap, 0,
        (new_cap - store->tool_call_cap) * sizeof(cc_memory_tool_call_record_t));
    store->tool_calls = new_records;
    store->tool_call_cap = new_cap;
    return cc_result_ok();
}

/*
 * 确保 tool result 数组有可写空间。
 *
 * 和 tool call 扩容一样，必须在持锁状态下调用，避免并发 append 看到未同步的数组指针。
 */
static cc_result_t ensure_tool_result_capacity(cc_memory_session_store_t *store)
{
    if (store->tool_result_count < store->tool_result_cap) return cc_result_ok();
    size_t new_cap = store->tool_result_cap ? store->tool_result_cap * 2 : INITIAL_CAPACITY;
    cc_memory_tool_result_record_t *new_records =
        realloc(store->tool_results, new_cap * sizeof(cc_memory_tool_result_record_t));
    if (!new_records) {
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to grow tool results");
    }
    memset(new_records + store->tool_result_cap, 0,
        (new_cap - store->tool_result_cap) * sizeof(cc_memory_tool_result_record_t));
    store->tool_results = new_records;
    store->tool_result_cap = new_cap;
    return cc_result_ok();
}

/*
 * 创建或确保一个 session 记录存在。
 *
 * 如果 session_id 已存在则直接成功返回；否则深拷贝 id/workspace 并追加到 sessions 数组。
 * workspace 会被后续文件工具作为安全边界读取，所以即使是内存 store 也要保存它。
 */
static cc_result_t memory_create_session(
    void *self,
    const char *session_id,
    const char *workspace_dir
)
{
    cc_memory_session_store_t *store = (cc_memory_session_store_t *)self;

    cc_mutex_lock(store->mutex);
    for (size_t i = 0; i < store->session_count; i++) {
        if (store->sessions[i].id && strcmp(store->sessions[i].id, session_id) == 0) {
            cc_mutex_unlock(store->mutex);
            return cc_result_ok();
        }
    }

    if (store->session_count >= store->session_cap) {
        size_t new_cap = store->session_cap * 2;
        cc_session_t *new_sessions = realloc(store->sessions, new_cap * sizeof(cc_session_t));
        if (!new_sessions) {
            cc_mutex_unlock(store->mutex);
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to grow sessions");
        }
        store->sessions = new_sessions;
        store->session_cap = new_cap;
    }

    cc_session_t *s = &store->sessions[store->session_count];
    memset(s, 0, sizeof(cc_session_t));
    s->id = strdup(session_id);
    s->workspace_dir = workspace_dir ? strdup(workspace_dir) : strdup(CC_DEFAULT_WORKSPACE_PATH);
    s->status = CC_SESSION_ACTIVE;
    store->session_count++;

    cc_mutex_unlock(store->mutex);
    return cc_result_ok();
}

/*
 * 追加一条消息。
 *
 * store 在持锁状态下扩容，并通过 cc_message_copy 深拷贝 message 的 content/tool calls，
 * 因而调用方可以在 append 返回后销毁自己的 cc_message_t。
 */
static cc_result_t memory_append_message(
    void *self,
    const cc_message_t *message
)
{
    cc_memory_session_store_t *store = (cc_memory_session_store_t *)self;

    cc_mutex_lock(store->mutex);
    if (store->message_count >= store->message_cap) {
        size_t new_cap = store->message_cap * 2;
        cc_message_t *new_msgs = realloc(store->messages, new_cap * sizeof(cc_message_t));
        if (!new_msgs) {
            cc_mutex_unlock(store->mutex);
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to grow messages");
        }
        store->messages = new_msgs;
        store->message_cap = new_cap;
    }

    cc_message_t *m = &store->messages[store->message_count];
    cc_result_t rc = cc_message_copy(message, m);
    if (rc.code != CC_OK) {
        cc_mutex_unlock(store->mutex);
        return rc;
    }
    store->message_count++;

    cc_mutex_unlock(store->mutex);
    return cc_result_ok();
}

/*
 * 加载指定 session 的历史消息。
 *
 * 返回数组由调用方拥有，需要逐项 cc_message_cleanup 后 free。limit <= 0 表示不限制。
 * 为了避免调用方读到内部数组，函数会重新分配并深拷贝每条消息。
 */
static cc_result_t memory_load_messages(
    void *self,
    const char *session_id,
    int limit,
    cc_message_t **out_messages,
    size_t *out_count
)
{
    cc_memory_session_store_t *store = (cc_memory_session_store_t *)self;

    cc_mutex_lock(store->mutex);
    size_t count = 0;
    size_t max_count = limit > 0 ? (size_t)limit : (size_t)-1;
    for (size_t i = 0; i < store->message_count && count < max_count; i++) {
        if (store->messages[i].session_id &&
            strcmp(store->messages[i].session_id, session_id) == 0) {
            count++;
        }
    }

    cc_message_t *result = calloc(count > 0 ? count : 1, sizeof(cc_message_t));
    if (!result) {
        *out_messages = NULL;
        *out_count = 0;
        cc_mutex_unlock(store->mutex);
        return cc_result_ok();
    }

    size_t idx = 0;
    for (size_t i = 0; i < store->message_count && idx < count; i++) {
        if (store->messages[i].session_id &&
            strcmp(store->messages[i].session_id, session_id) == 0) {
            cc_message_t *m = &result[idx];
            cc_message_copy(&store->messages[i], m);
            idx++;
        }
    }

    *out_messages = result;
    *out_count = idx;
    cc_mutex_unlock(store->mutex);
    return cc_result_ok();
}

/*
 * 追加工具调用记录。
 *
 * 工具调用和普通 assistant message 分开保存，方便调试 UI 或审计查看“模型请求了什么
 * 工具”。所有字段都深拷贝，append 失败时释放临时 record。
 */
static cc_result_t memory_append_tool_call(
    void *self,
    const char *session_id,
    const cc_tool_call_t *call
)
{
    cc_memory_session_store_t *store = (cc_memory_session_store_t *)self;

    cc_mutex_lock(store->mutex);
    cc_result_t rc = ensure_tool_call_capacity(store);
    if (rc.code != CC_OK) {
        cc_mutex_unlock(store->mutex);
        return rc;
    }

    cc_memory_tool_call_record_t record;
    memset(&record, 0, sizeof(record));
    record.id = dup_required_string(call && call->id ? call->id : "");
    record.session_id = dup_required_string(session_id);
    record.name = dup_required_string(call && call->name ? call->name : "");
    record.arguments_json = dup_required_string(call && call->arguments_json ? call->arguments_json : "");
    record.status = dup_required_string("completed");

    if (!record.id || !record.session_id || !record.name ||
        !record.arguments_json || !record.status) {
        free_tool_call_record(&record);
        cc_mutex_unlock(store->mutex);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy tool call record");
    }

    store->tool_calls[store->tool_call_count++] = record;
    cc_mutex_unlock(store->mutex);
    return cc_result_ok();
}

/*
 * 追加工具结果记录。
 *
 * tool result 可能包含 artifact 列表，内存 store 将其序列化为 JSON 字符串记录。函数
 * 持锁完成扩容和拷贝，返回后调用方仍然负责释放原 cc_tool_result_t。
 */
static cc_result_t memory_append_tool_result(
    void *self,
    const char *session_id,
    const char *tool_call_id,
    const cc_tool_result_t *result
)
{
    cc_memory_session_store_t *store = (cc_memory_session_store_t *)self;

    cc_mutex_lock(store->mutex);
    cc_result_t rc = ensure_tool_result_capacity(store);
    if (rc.code != CC_OK) {
        cc_mutex_unlock(store->mutex);
        return rc;
    }

    cc_memory_tool_result_record_t record;
    memset(&record, 0, sizeof(record));
    record.id = dup_required_string(tool_call_id);
    record.session_id = dup_required_string(session_id);
    record.tool_call_id = dup_required_string(tool_call_id);
    record.ok = result && result->ok;
    record.text = dup_required_string(result && result->text ? result->text : "");
    record.error = dup_required_string(result && result->error ? result->error : "");
    record.metadata = dup_required_string(result && result->metadata ? result->metadata : "");
    char *artifacts = NULL;
    if (result) {
        cc_media_artifact_list_to_json(&result->artifacts, &artifacts);
    }
    record.artifacts = dup_required_string(artifacts ? artifacts : "[]");
    free(artifacts);

    if (!record.id || !record.session_id || !record.tool_call_id ||
        !record.text || !record.error || !record.metadata ||
        !record.artifacts) {
        free_tool_result_record(&record);
        cc_mutex_unlock(store->mutex);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy tool result record");
    }

    store->tool_results[store->tool_result_count++] = record;
    cc_mutex_unlock(store->mutex);
    return cc_result_ok();
}

/*
 * 列举所有 session。
 *
 * 返回数组由调用方拥有，数组元素中的字符串也是深拷贝。该接口主要给 UI/测试使用；
 * 内部锁只保护拷贝过程，返回后调用方不会持有 store 内部指针。
 */
static cc_result_t memory_list_sessions(
    void *self,
    cc_session_t **out_sessions,
    size_t *out_count
)
{
    cc_memory_session_store_t *store = (cc_memory_session_store_t *)self;

    cc_mutex_lock(store->mutex);
    cc_session_t *result = calloc(store->session_count, sizeof(cc_session_t));
    if (!result) {
        cc_mutex_unlock(store->mutex);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate sessions");
    }

    for (size_t i = 0; i < store->session_count; i++) {
        result[i].id = store->sessions[i].id ? strdup(store->sessions[i].id) : NULL;
        result[i].name = store->sessions[i].name ? strdup(store->sessions[i].name) : NULL;
        result[i].workspace_dir = store->sessions[i].workspace_dir ? strdup(store->sessions[i].workspace_dir) : NULL;
        result[i].status = store->sessions[i].status;
    }

    *out_sessions = result;
    *out_count = store->session_count;
    cc_mutex_unlock(store->mutex);
    return cc_result_ok();
}

/*
 * 清空某个 session 的消息和工具记录。
 *
 * 函数使用读写下标原地压缩数组：匹配 session_id 的记录先释放，不匹配的记录前移。
 * 前移后尾部槽位清零，避免 destroy 阶段重复释放已经移动走的资源。
 */
static cc_result_t memory_clear_session(void *self, const char *session_id)
{
    if (!self || !session_id) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid session clear request");
    }
    cc_memory_session_store_t *store = (cc_memory_session_store_t *)self;
    cc_mutex_lock(store->mutex);

    size_t write = 0;
    for (size_t read = 0; read < store->message_count; read++) {
        cc_message_t *message = &store->messages[read];
        if (message->session_id && strcmp(message->session_id, session_id) == 0) {
            cc_message_cleanup(message);
            continue;
        }
        if (write != read) store->messages[write] = store->messages[read];
        write++;
    }
    for (size_t i = write; i < store->message_count; i++) {
        memset(&store->messages[i], 0, sizeof(store->messages[i]));
    }
    store->message_count = write;

    write = 0;
    for (size_t read = 0; read < store->tool_call_count; read++) {
        cc_memory_tool_call_record_t *record = &store->tool_calls[read];
        if (record->session_id && strcmp(record->session_id, session_id) == 0) {
            free_tool_call_record(record);
            continue;
        }
        if (write != read) store->tool_calls[write] = store->tool_calls[read];
        write++;
    }
    for (size_t i = write; i < store->tool_call_count; i++) {
        memset(&store->tool_calls[i], 0, sizeof(store->tool_calls[i]));
    }
    store->tool_call_count = write;

    write = 0;
    for (size_t read = 0; read < store->tool_result_count; read++) {
        cc_memory_tool_result_record_t *record = &store->tool_results[read];
        if (record->session_id && strcmp(record->session_id, session_id) == 0) {
            free_tool_result_record(record);
            continue;
        }
        if (write != read) store->tool_results[write] = store->tool_results[read];
        write++;
    }
    for (size_t i = write; i < store->tool_result_count; i++) {
        memset(&store->tool_results[i], 0, sizeof(store->tool_results[i]));
    }
    store->tool_result_count = write;

    cc_mutex_unlock(store->mutex);
    return cc_result_ok();
}

/*
 * 销毁内存 session store。
 *
 * destroy 时先持锁释放所有深拷贝对象，再销毁 mutex 和 store 自身。调用方必须保证没有
 * 其它线程还在使用这个 store；这是端口销毁的一般约束。
 */
static void memory_destroy(void *self)
{
    cc_memory_session_store_t *store = (cc_memory_session_store_t *)self;
    cc_mutex_lock(store->mutex);
    for (size_t i = 0; i < store->message_count; i++) {
        cc_message_cleanup(&store->messages[i]);
    }
    free(store->messages);
    for (size_t i = 0; i < store->session_count; i++) {
        free(store->sessions[i].id);
        free(store->sessions[i].name);
        free(store->sessions[i].workspace_dir);
        free(store->sessions[i].model);
        free(store->sessions[i].created_at);
        free(store->sessions[i].updated_at);
    }
    free(store->sessions);
    for (size_t i = 0; i < store->tool_call_count; i++) {
        free_tool_call_record(&store->tool_calls[i]);
    }
    free(store->tool_calls);
    for (size_t i = 0; i < store->tool_result_count; i++) {
        free_tool_result_record(&store->tool_results[i]);
    }
    free(store->tool_results);
    cc_mutex_unlock(store->mutex);
    cc_mutex_destroy(store->mutex);
    free(store);
}

/* 内存 session store 的 vtable，把本文件函数绑定到 cc_session_store_t 端口。 */
static cc_session_store_vtable_t memory_vtable = {
    memory_create_session,
    memory_append_message,
    memory_load_messages,
    memory_append_tool_call,
    memory_append_tool_result,
    memory_list_sessions,
    memory_clear_session,
    memory_destroy
};

/*
 * 创建内存 session store。
 *
 * 成功后 out_store 获得 self/vtable，销毁时通过 vtable->destroy 释放。这个 adapter 不做
 * 持久化，适合作为测试、最小 profile 或 MCU 原型；生产环境可用同一端口替换成文件/DB。
 */
cc_result_t cc_memory_session_store_create(cc_session_store_t *out_store)
{
    if (!out_store) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null memory session store output");
    }
    memset(out_store, 0, sizeof(*out_store));
    cc_memory_session_store_t *self = calloc(1, sizeof(cc_memory_session_store_t));
    if (!self) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create memory store");

    self->messages = calloc(INITIAL_CAPACITY, sizeof(cc_message_t));
    self->message_cap = INITIAL_CAPACITY;

    self->sessions = calloc(INITIAL_CAPACITY, sizeof(cc_session_t));
    self->session_cap = INITIAL_CAPACITY;
    self->tool_calls = calloc(INITIAL_CAPACITY, sizeof(cc_memory_tool_call_record_t));
    self->tool_call_cap = INITIAL_CAPACITY;
    self->tool_results = calloc(INITIAL_CAPACITY, sizeof(cc_memory_tool_result_record_t));
    self->tool_result_cap = INITIAL_CAPACITY;
    if (!self->messages || !self->sessions || !self->tool_calls || !self->tool_results) {
        free(self->messages);
        free(self->sessions);
        free(self->tool_calls);
        free(self->tool_results);
        free(self);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate memory session store arrays");
    }
    cc_result_t rc = cc_mutex_create(&self->mutex);
    if (rc.code != CC_OK) {
        free(self->messages);
        free(self->sessions);
        free(self->tool_calls);
        free(self->tool_results);
        free(self);
        return rc;
    }

    out_store->self = self;
    out_store->vtable = &memory_vtable;
    return cc_result_ok();
}
