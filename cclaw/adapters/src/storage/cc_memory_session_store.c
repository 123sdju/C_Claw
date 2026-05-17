/**
 * 学习导读：cclaw/adapters/src/storage/cc_memory_session_store.c
 *
 * 所属层次：适配器层。
 * 阅读重点：这里把端口接口落到具体后端，阅读时重点看协议转换、资源释放和失败降级。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * @file cc_memory_session_store.c
 * @brief 纯内存会话存储适配器（易失性）
 *
 * 基于动态数组在进程堆内存中存储会话和消息数据。所有数据在进程退出后丢失，
 * 不依赖任何外部存储（无文件 I/O、无数据库）。
 *
 * 完整实现 cc_session_store_vtable 中的全部 7 个虚函数：
 *   create_session / append_message / load_messages / append_tool_call /
 *   append_tool_result / list_sessions / destroy
 *
 * 核心数据结构：
 *   - messages 动态数组，初始容量 64，翻倍扩容
 *   - sessions 动态数组，初始容量 64，翻倍扩容
 *   - tool_calls/tool_results 动态数组，用于保留工具调用审计信息
 *
 * 适用场景：
 *   - 单元测试和集成测试（数据隔离、可重复）
 *   - 嵌入式/受限环境（无文件系统或数据库支持）
 *   - 临时会话（不需要持久化）
 *
 * 注意事项：
 *   - 所有数据仅保存在进程内存中，退出后丢失
 *   - 大量数据场景需关注内存占用（无上限保护）
 */

#include "cc/ports/cc_session_store.h"
#include "cc/ports/cc_thread.h"
#include <stdlib.h>
#include <string.h>

#ifndef CC_DEFAULT_WORKSPACE_PATH
#define CC_DEFAULT_WORKSPACE_PATH "runtime/workspace"
#endif

#define INITIAL_CAPACITY 64

/**
 * @brief 内存会话存储的私有数据结构
 *
 * @field messages      消息动态数组，元素类型为 cc_message_t
 * @field message_count 当前消息数量
 * @field message_cap   消息数组当前容量
 * @field sessions      会话动态数组，元素类型为 cc_session_t
 * @field session_count 当前会话数量
 * @field session_cap   会话数组当前容量
 */
typedef struct {
    char *id;
    char *session_id;
    char *name;
    char *arguments_json;
    char *status;
} cc_memory_tool_call_record_t;

typedef struct {
    char *id;
    char *session_id;
    char *tool_call_id;
    int ok;
    char *content;
    char *error;
    char *metadata_json;
} cc_memory_tool_result_record_t;

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

/* 学习注释：dup_required_string 是本文件内部辅助函数。
 * 阅读时先看它服务哪个 public API，再看它如何处理边界条件和资源释放。 */
static char *dup_required_string(const char *value)
{
    return strdup(value ? value : "");
}

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

/* 学习注释：free_tool_result_record 是本文件内部辅助函数。
 * 阅读时先看它服务哪个 public API，再看它如何处理边界条件和资源释放。 */
static void free_tool_result_record(cc_memory_tool_result_record_t *record)
{
    if (!record) return;
    free(record->id);
    free(record->session_id);
    free(record->tool_call_id);
    free(record->content);
    free(record->error);
    free(record->metadata_json);
    memset(record, 0, sizeof(*record));
}

/* 学习注释：ensure_tool_call_capacity 是本文件内部辅助函数。
 * 阅读时先看它服务哪个 public API，再看它如何处理边界条件和资源释放。 */
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

/* 学习注释：ensure_tool_result_capacity 是本文件内部辅助函数。
 * 阅读时先看它服务哪个 public API，再看它如何处理边界条件和资源释放。 */
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

/**
 * @brief vtable 函数：创建新会话（内存版本）
 *
 * 先在 sessions 数组中遍历检查是否已存在同 ID 会话，若存在则直接返回 OK。
 * 不存在时，若数组已满则翻倍扩容（2x），然后在末尾追加新会话记录。
 * 新会话默认 status=CC_SESSION_ACTIVE，workspace_dir 为 NULL 时默认 profile workspace path。
 *
 * @param self          存储实例指针
 * @param session_id    新会话的唯一标识符
 * @param workspace_dir 工作目录路径，NULL 时使用默认值
 * @return cc_result_t  成功返回 OK，realloc 失败返回 OUT_OF_MEMORY
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

/**
 * @brief vtable 函数：追加消息（内存版本）
 *
 * 若消息数组已满则翻倍扩容（2x），然后将 message 的各个字段深拷贝到数组末尾。
 * 深拷贝的字段包括：id、session_id、content、tool_call_id，使用 strdup 分配。
 * role 字段为枚举值，直接值拷贝。
 *
 * @param self    存储实例指针
 * @param message 要追加的消息对象
 * @return cc_result_t 成功返回 OK，realloc 失败返回 OUT_OF_MEMORY
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

/**
 * @brief vtable 函数：加载指定会话的消息列表（内存版本）
 *
 * 采用两遍扫描策略：
 *   1. 第一遍：遍历全部消息，统计 session_id 匹配的消息数量（受 limit 约束）
 *   2. 第二遍：分配结果数组并深拷贝匹配的消息
 *
 * 由于内存数组天然保持插入顺序，返回的消息即为时间序。
 * 深拷贝避免调用者修改结果数组时影响内部存储状态。
 *
 * @param self         存储实例指针
 * @param session_id   目标会话ID
 * @param limit        最大返回消息数
 * @param out_messages 输出参数，指向结果消息数组的指针
 * @param out_count    输出参数，实际返回的消息数量
 * @return cc_result_t 成功返回 OK
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

/**
 * @brief vtable 函数：记录工具调用
 *
 * 将工具调用请求深拷贝到内存审计数组中。虽然内存后端不跨进程持久化，
 * 但在运行期保持与 SQLite/JSON 后端一致的记录语义。
 *
 * @param self       存储实例指针
 * @param session_id 关联的会话ID
 * @param call       工具调用对象
 * @return cc_result_t 成功返回 OK，失败返回 OUT_OF_MEMORY
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

/**
 * @brief vtable 函数：记录工具调用结果
 *
 * 将工具调用结果深拷贝到内存审计数组中，保留 ok/content/error/metadata_json。
 *
 * @param self         存储实例指针
 * @param session_id   关联的会话ID
 * @param tool_call_id 关联的工具调用ID
 * @param result       工具调用结果对象
 * @return cc_result_t 成功返回 OK，失败返回 OUT_OF_MEMORY
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
    record.content = dup_required_string(result && result->content ? result->content : "");
    record.error = dup_required_string(result && result->error ? result->error : "");
    record.metadata_json = dup_required_string(result && result->metadata_json ? result->metadata_json : "");

    if (!record.id || !record.session_id || !record.tool_call_id ||
        !record.content || !record.error || !record.metadata_json) {
        free_tool_result_record(&record);
        cc_mutex_unlock(store->mutex);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy tool result record");
    }

    store->tool_results[store->tool_result_count++] = record;
    cc_mutex_unlock(store->mutex);
    return cc_result_ok();
}

/**
 * @brief vtable 函数：列出所有会话（内存版本）
 *
 * 遍历 sessions 数组，将每个会话的 id、name、workspace_dir、status 深拷贝到结果数组。
 * 深拷贝使用 strdup，避免调用者修改返回数据影响内存存储的内部状态。
 *
 * @param self          存储实例指针
 * @param out_sessions  输出参数，指向结果会话数组的指针
 * @param out_count     输出参数，实际返回的会话数量
 * @return cc_result_t  成功返回 OK，calloc 失败返回 OUT_OF_MEMORY
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

/**
 * @brief vtable 函数：销毁内存存储，释放所有资源
 *
 * 依次遍历 messages 和 sessions 数组，对每个元素调用 cc_message_destroy / cc_session_destroy
 * 释放内部字符串，然后释放数组内存，最后释放存储结构体自身。
 *
 * 安全注意：必须在进程退出前调用，否则造成内存泄漏。
 *
 * @param self 存储实例指针
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

/**
 * @brief 内存会话存储的虚函数表
 *
 * 将全部 7 个 vtable 函数绑定到对应的内存实现。
 */
static cc_session_store_vtable_t memory_vtable = {
    memory_create_session,
    memory_append_message,
    memory_load_messages,
    memory_append_tool_call,
    memory_append_tool_result,
    memory_list_sessions,
    memory_destroy
};

/**
 * @brief 创建内存会话存储实例（公共工厂函数）
 *
 * 执行流程：
 *   1. 分配 cc_memory_session_store_t 结构体（calloc 零初始化）
 *   2. 分配初始容量为 INITIAL_CAPACITY(64) 的消息数组
 *   3. 分配初始容量为 INITIAL_CAPACITY(64) 的会话数组
 *   4. 填充 out_store 的 self 和 vtable
 *
 * @param out_store 输出参数，填充创建好的存储实例
 * @return cc_result_t 成功返回 OK，分配失败返回 OUT_OF_MEMORY
 */
cc_result_t cc_memory_session_store_create(cc_session_store_t *out_store)
{
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
