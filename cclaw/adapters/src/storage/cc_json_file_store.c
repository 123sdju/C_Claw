/**
 * 学习导读：cclaw/adapters/src/storage/cc_json_file_store.c
 *
 * 所属层次：适配器层。
 * 阅读重点：这里把端口接口落到具体后端，阅读时重点看协议转换、资源释放和失败降级。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * @file cc_json_file_store.c
 * @brief JSON 文件会话存储适配器
 *
 * 基于单个 JSON 文件实现会话状态的持久化存储。所有数据以 JSON 对象组织，
 * 包含 sessions、messages、tool_calls、tool_results 四个顶层数组。
 *
 * 完整实现 cc_session_store_vtable 中的全部 7 个虚函数：
 *   create_session / append_message / load_messages / append_tool_call /
 *   append_tool_result / list_sessions / destroy
 *
 * 工作流程：
 *   - 每次写操作前从磁盘加载 JSON（load_store），操作后写回磁盘（save_store）
 *   - 文件不存在时自动创建带初始化结构的空 JSON 文件
 *   - append_tool_result 会写入 tool_results 数组，便于审计和恢复
 *
 * 适用场景：
 *   - 轻量级部署，不依赖数据库
 *   - 会话数据量较小（< 千条级别）
 *   - 需要直接查看/编辑存储内容的调试场景
 *
 * 安全注意：
 *   - JSON 解析/序列化由 cc_json 库处理，不受用户输入影响
 *   - 文件 I/O 路径由工厂函数固定，不接收外部拼接
 */

#include "cc/ports/cc_session_store.h"
#include "cc/ports/cc_thread.h"
#include "cc/util/cc_json.h"
#include "cc/util/cc_string_builder.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifndef CC_DEFAULT_WORKSPACE_PATH
#define CC_DEFAULT_WORKSPACE_PATH "runtime/workspace"
#endif

#ifndef CC_DEFAULT_STORAGE_PATH
#define CC_DEFAULT_STORAGE_PATH "runtime/data/sessions.json"
#endif

/**
 * @brief JSON 文件存储的私有数据结构
 *
 * @field file_path JSON 存储文件的完整路径
 * @field root      JSON 文档的根对象，包含 sessions、messages、tool_calls、tool_results 四个数组
 */
typedef struct {
    char *file_path;
    cc_json_value_t *root;
    cc_mutex_t mutex;
} cc_json_file_store_t;

/**
 * @brief 确保 JSON 存储文件存在
 *
 * 先尝试以只读模式打开文件，若成功则文件已存在，直接关闭返回 OK。
 * 若文件不存在，则创建新文件并写入初始化 JSON 结构：
 *   {"sessions":[],"messages":[],"tool_calls":[],"tool_results":[]}
 *
 * @param file_path JSON 文件路径
 * @return cc_result_t 成功返回 OK，文件创建失败返回 IO 错误
 */
static cc_result_t ensure_file_exists(const char *file_path)
{
    FILE *f = fopen(file_path, "r");
    if (f) {
        fclose(f);
        return cc_result_ok();
    }

    f = fopen(file_path, "w");
    if (!f) return cc_result_error(CC_ERR_IO, "Cannot create JSON store file");

    fputs("{\"sessions\":[],\"messages\":[],\"tool_calls\":[],\"tool_results\":[]}", f);
    fclose(f);
    return cc_result_ok();
}

/**
 * @brief 从磁盘加载 JSON 存储到内存
 *
 * 解析 JSON 文件并将根对象存入 store->root。如果根对象无效或缺失必要的数组字段，
 * 则自动创建新对象并初始化 sessions、messages、tool_calls、tool_results 四个空数组。
 * 调用前会自动销毁旧的 root 对象以防止内存泄漏。
 *
 * @param store JSON 文件存储实例
 * @return cc_result_t 成功返回 OK，解析失败返回对应错误码
 */
static cc_result_t load_store(cc_json_file_store_t *store)
{
    if (store->root) cc_json_destroy(store->root);

    cc_result_t rc = cc_json_parse_from_file(store->file_path, &store->root);
    if (rc.code != CC_OK) {
        return rc;
    }

    if (!store->root || !cc_json_is_object(store->root)) {
        if (store->root) cc_json_destroy(store->root);
        store->root = cc_json_create_object();
        cc_json_object_set(store->root, "sessions", cc_json_create_array());
        cc_json_object_set(store->root, "messages", cc_json_create_array());
        cc_json_object_set(store->root, "tool_calls", cc_json_create_array());
        cc_json_object_set(store->root, "tool_results", cc_json_create_array());
    }

    if (!cc_json_object_get(store->root, "sessions")) {
        cc_json_object_set(store->root, "sessions", cc_json_create_array());
    }
    if (!cc_json_object_get(store->root, "messages")) {
        cc_json_object_set(store->root, "messages", cc_json_create_array());
    }
    if (!cc_json_object_get(store->root, "tool_calls")) {
        cc_json_object_set(store->root, "tool_calls", cc_json_create_array());
    }
    if (!cc_json_object_get(store->root, "tool_results")) {
        cc_json_object_set(store->root, "tool_results", cc_json_create_array());
    }

    return cc_result_ok();
}

/**
 * @brief 将内存中的 JSON 存储写回磁盘
 *
 * 使用 cc_json_stringify() 将根对象序列化为 JSON 文本，然后以覆盖模式写入文件。
 * root 为 NULL 时视为空存储，直接返回 OK 不做任何操作。
 *
 * @param store JSON 文件存储实例
 * @return cc_result_t 成功返回 OK，序列化失败或 I/O 错误返回对应错误码
 */
static cc_result_t save_store(cc_json_file_store_t *store)
{
    if (!store->root) return cc_result_ok();

    char *json_text = cc_json_stringify(store->root);
    if (!json_text) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to serialize JSON store");

    FILE *f = fopen(store->file_path, "w");
    if (!f) {
        free(json_text);
        return cc_result_error(CC_ERR_IO, "Cannot write JSON store file");
    }

    fputs(json_text, f);
    fclose(f);
    free(json_text);
    return cc_result_ok();
}

/**
 * @brief vtable 函数：创建新会话
 *
 * 遍历 sessions 数组检查会话是否已存在（按 id 匹配），若已存在则直接返回 OK。
 * 否则创建包含 id、workspace_dir、status 三个字段的 JSON 对象并追加到 sessions 数组。
 * 操作流程：load_store → 查重/追加 → save_store
 *
 * @param self          存储实例指针
 * @param session_id    新会话的唯一标识符
 * @param workspace_dir 工作目录路径，NULL 时默认 profile workspace path
 * @return cc_result_t  成功返回 OK，I/O 或 JSON 操作失败返回相应错误码
 */
static cc_result_t json_file_create_session(
    void *self,
    const char *session_id,
    const char *workspace_dir
)
{
    cc_json_file_store_t *store = (cc_json_file_store_t *)self;

    cc_mutex_lock(store->mutex);
    cc_result_t rc = load_store(store);
    if (rc.code != CC_OK) {
        cc_mutex_unlock(store->mutex);
        return rc;
    }

    cc_json_value_t *sessions = cc_json_object_get(store->root, "sessions");
    if (!sessions || !cc_json_is_array(sessions)) {
        cc_mutex_unlock(store->mutex);
        return cc_result_error(CC_ERR_STORAGE, "Invalid sessions array");
    }

    int count = cc_json_array_size(sessions);
    for (int i = 0; i < count; i++) {
        cc_json_value_t *s = cc_json_array_get(sessions, i);
        cc_json_value_t *id = cc_json_object_get(s, "id");
        const char *id_str = cc_json_string_value(id);
        if (id_str && strcmp(id_str, session_id) == 0) {
            cc_mutex_unlock(store->mutex);
            return cc_result_ok();
        }
    }

    cc_json_value_t *session = cc_json_create_object();
    cc_json_object_set(session, "id", cc_json_create_string(session_id));
    cc_json_object_set(session, "workspace_dir",
        cc_json_create_string(workspace_dir ? workspace_dir : CC_DEFAULT_WORKSPACE_PATH));
    cc_json_object_set(session, "status", cc_json_create_string("active"));

    cc_json_array_append(sessions, session);

    rc = save_store(store);
    cc_mutex_unlock(store->mutex);
    return rc;
}

/**
 * @brief vtable 函数：追加消息到 messages 数组
 *
 * 将 cc_message_t 的字段映射为 JSON 对象：
 *   id → "id", session_id → "session_id", role → "role",
 *   content → "content", tool_call_id → "tool_call_id"（仅当非 NULL 时）
 *
 * 操作流程：load_store → 追加到 messages 数组 → save_store
 *
 * @param self    存储实例指针
 * @param message 要追加的消息对象
 * @return cc_result_t 成功返回 OK，失败返回相应错误码
 */
static cc_result_t json_file_append_message(
    void *self,
    const cc_message_t *message
)
{
    cc_json_file_store_t *store = (cc_json_file_store_t *)self;

    cc_mutex_lock(store->mutex);
    cc_result_t rc = load_store(store);
    if (rc.code != CC_OK) {
        cc_mutex_unlock(store->mutex);
        return rc;
    }

    cc_json_value_t *messages = cc_json_object_get(store->root, "messages");
    if (!messages || !cc_json_is_array(messages)) {
        cc_mutex_unlock(store->mutex);
        return cc_result_error(CC_ERR_STORAGE, "Invalid messages array");
    }

    cc_json_value_t *msg = cc_json_create_object();
    cc_json_object_set(msg, "id", cc_json_create_string(message->id ? message->id : ""));
    cc_json_object_set(msg, "session_id", cc_json_create_string(message->session_id ? message->session_id : ""));
    cc_json_object_set(msg, "role", cc_json_create_string(cc_message_role_string(message->role)));
    cc_json_object_set(msg, "content", cc_json_create_string(message->content ? message->content : ""));
    if (message->tool_calls_json) {
        cc_json_object_set(msg, "tool_calls_json",
            cc_json_create_string(message->tool_calls_json));
    }
    if (message->reasoning_content) {
        cc_json_object_set(msg, "reasoning_content",
            cc_json_create_string(message->reasoning_content));
    }
    if (message->tool_call_id) {
        cc_json_object_set(msg, "tool_call_id", cc_json_create_string(message->tool_call_id));
    }

    cc_json_array_append(messages, msg);

    rc = save_store(store);
    cc_mutex_unlock(store->mutex);
    return rc;
}

/**
 * @brief vtable 函数：加载指定会话的消息列表
 *
 * 遍历 messages 数组，筛选出 session_id 匹配的消息，并按加载顺序返回
 * （JSON 数组天然保持插入顺序，等价于 SQLite 版本的 ORDER BY created_at ASC）。
 * 受 limit 参数限制，最多返回 limit 条消息。
 *
 * 角色字符串（system/user/assistant/tool）将转换为 cc_message_role 枚举。
 * load_store 失败时不视为致命错误——返回空列表而非报错。
 *
 * @param self         存储实例指针
 * @param session_id   目标会话ID
 * @param limit        最大返回消息数
 * @param out_messages 输出参数，指向结果消息数组的指针
 * @param out_count    输出参数，实际返回的消息数量
 * @return cc_result_t 成功返回 OK
 */
static cc_result_t json_file_load_messages(
    void *self,
    const char *session_id,
    int limit,
    cc_message_t **out_messages,
    size_t *out_count
)
{
    cc_json_file_store_t *store = (cc_json_file_store_t *)self;

    cc_mutex_lock(store->mutex);
    cc_result_t rc = load_store(store);
    if (rc.code != CC_OK) {
        *out_messages = NULL;
        *out_count = 0;
        cc_mutex_unlock(store->mutex);
        return cc_result_ok();
    }

    cc_json_value_t *messages_arr = cc_json_object_get(store->root, "messages");
    if (!messages_arr || !cc_json_is_array(messages_arr)) {
        *out_messages = NULL;
        *out_count = 0;
        cc_mutex_unlock(store->mutex);
        return cc_result_ok();
    }

    size_t cap = 16;
    size_t count = 0;
    cc_message_t *messages = calloc(cap, sizeof(cc_message_t));
    if (!messages) {
        *out_messages = NULL;
        *out_count = 0;
        cc_mutex_unlock(store->mutex);
        return cc_result_ok();
    }

    int arr_size = cc_json_array_size(messages_arr);
    int loaded = 0;
    int max_loaded = limit > 0 ? limit : arr_size;
    for (int i = 0; i < arr_size && loaded < max_loaded; i++) {
        cc_json_value_t *msg = cc_json_array_get(messages_arr, i);

        cc_json_value_t *sid = cc_json_object_get(msg, "session_id");
        const char *sid_str = cc_json_string_value(sid);
        if (!sid_str || strcmp(sid_str, session_id) != 0) continue;

        if (count >= cap) {
            void *new_ptr = realloc(messages, cap * 2 * sizeof(cc_message_t));
            if (!new_ptr) break;
            messages = new_ptr;
            cap *= 2;
        }

        cc_message_t *m = &messages[count];
        memset(m, 0, sizeof(cc_message_t));

        cc_json_value_t *v = cc_json_object_get(msg, "id");
        m->id = strdup(cc_json_string_value(v) ? cc_json_string_value(v) : "");

        v = cc_json_object_get(msg, "content");
        m->content = strdup(cc_json_string_value(v) ? cc_json_string_value(v) : "");

        v = cc_json_object_get(msg, "tool_calls_json");
        const char *tcj = cc_json_string_value(v);
        m->tool_calls_json = tcj ? strdup(tcj) : NULL;

        v = cc_json_object_get(msg, "reasoning_content");
        const char *reasoning = cc_json_string_value(v);
        m->reasoning_content = reasoning ? strdup(reasoning) : NULL;

        v = cc_json_object_get(msg, "tool_call_id");
        const char *tci = cc_json_string_value(v);
        m->tool_call_id = tci ? strdup(tci) : NULL;

        v = cc_json_object_get(msg, "role");
        const char *role = cc_json_string_value(v);
        if (role) {
            if (strcmp(role, "system") == 0) m->role = CC_ROLE_SYSTEM;
            else if (strcmp(role, "user") == 0) m->role = CC_ROLE_USER;
            else if (strcmp(role, "assistant") == 0) m->role = CC_ROLE_ASSISTANT;
            else if (strcmp(role, "tool") == 0) m->role = CC_ROLE_TOOL;
        }

        m->session_id = sid_str ? strdup(sid_str) : NULL;
        count++;
        loaded++;
    }

    *out_messages = messages;
    *out_count = count;
    cc_mutex_unlock(store->mutex);
    return cc_result_ok();
}

/**
 * @brief vtable 函数：记录工具调用
 *
 * 将 cc_tool_call_t 字段映射为 JSON 对象并追加到 tool_calls 数组：
 *   id → "id", session_id → "session_id", name → "name",
 *   arguments_json → "arguments_json", status 固定为 "completed"
 *
 * 操作流程：load_store → 追加到 tool_calls 数组 → save_store
 *
 * @param self       存储实例指针
 * @param session_id 关联的会话ID
 * @param call       工具调用对象
 * @return cc_result_t 成功返回 OK，失败返回相应错误码
 */
static cc_result_t json_file_append_tool_call(
    void *self,
    const char *session_id,
    const cc_tool_call_t *call
)
{
    cc_json_file_store_t *store = (cc_json_file_store_t *)self;

    cc_mutex_lock(store->mutex);
    cc_result_t rc = load_store(store);
    if (rc.code != CC_OK) {
        cc_mutex_unlock(store->mutex);
        return rc;
    }

    cc_json_value_t *tool_calls = cc_json_object_get(store->root, "tool_calls");
    if (!tool_calls || !cc_json_is_array(tool_calls)) {
        cc_mutex_unlock(store->mutex);
        return cc_result_error(CC_ERR_STORAGE, "Invalid tool_calls array");
    }

    cc_json_value_t *tc = cc_json_create_object();
    cc_json_object_set(tc, "id", cc_json_create_string(call->id ? call->id : ""));
    cc_json_object_set(tc, "session_id", cc_json_create_string(session_id));
    cc_json_object_set(tc, "name", cc_json_create_string(call->name ? call->name : ""));
    cc_json_object_set(tc, "arguments_json", cc_json_create_string(call->arguments_json ? call->arguments_json : ""));
    cc_json_object_set(tc, "status", cc_json_create_string("completed"));

    cc_json_array_append(tool_calls, tc);

    rc = save_store(store);
    cc_mutex_unlock(store->mutex);
    return rc;
}

/**
 * @brief vtable 函数：记录工具调用结果
 *
 * 将 cc_tool_result_t 字段映射为 JSON 对象并追加到 tool_results 数组。
 * 结果与 tool_call_id 关联，同时保留 session_id 方便直接检索和审计。
 *
 * @param self         存储实例指针
 * @param session_id   关联的会话ID
 * @param tool_call_id 关联的工具调用ID
 * @param result       工具调用结果对象
 * @return cc_result_t 成功返回 OK，失败返回相应错误码
 */
static cc_result_t json_file_append_tool_result(
    void *self,
    const char *session_id,
    const char *tool_call_id,
    const cc_tool_result_t *result
)
{
    cc_json_file_store_t *store = (cc_json_file_store_t *)self;

    cc_mutex_lock(store->mutex);
    cc_result_t rc = load_store(store);
    if (rc.code != CC_OK) {
        cc_mutex_unlock(store->mutex);
        return rc;
    }

    cc_json_value_t *tool_results = cc_json_object_get(store->root, "tool_results");
    if (!tool_results || !cc_json_is_array(tool_results)) {
        cc_mutex_unlock(store->mutex);
        return cc_result_error(CC_ERR_STORAGE, "Invalid tool_results array");
    }

    cc_json_value_t *tr = cc_json_create_object();
    if (!tr) {
        cc_mutex_unlock(store->mutex);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate tool result record");
    }
    cc_json_object_set(tr, "id", cc_json_create_string(tool_call_id ? tool_call_id : ""));
    cc_json_object_set(tr, "session_id", cc_json_create_string(session_id ? session_id : ""));
    cc_json_object_set(tr, "tool_call_id", cc_json_create_string(tool_call_id ? tool_call_id : ""));
    cc_json_object_set(tr, "ok", cc_json_create_bool(result && result->ok));
    cc_json_object_set(tr, "content", cc_json_create_string(result && result->content ? result->content : ""));
    cc_json_object_set(tr, "error", cc_json_create_string(result && result->error ? result->error : ""));
    cc_json_object_set(tr, "metadata_json", cc_json_create_string(result && result->metadata_json ? result->metadata_json : ""));

    cc_json_array_append(tool_results, tr);

    rc = save_store(store);
    cc_mutex_unlock(store->mutex);
    return rc;
}

/**
 * @brief vtable 函数：列出所有会话
 *
 * 遍历 sessions 数组，提取每个会话的 id、workspace_dir、name 字段。
 * name 字段复用 workspace_dir 的值（JSON 文件存储格式中未显式存储 "name" 字段）。
 *
 * @param self          存储实例指针
 * @param out_sessions  输出参数，指向结果会话数组的指针
 * @param out_count     输出参数，实际返回的会话数量
 * @return cc_result_t  成功返回 OK，失败返回相应错误码
 */
static cc_result_t json_file_list_sessions(
    void *self,
    cc_session_t **out_sessions,
    size_t *out_count
)
{
    cc_json_file_store_t *store = (cc_json_file_store_t *)self;

    cc_mutex_lock(store->mutex);
    cc_result_t rc = load_store(store);
    if (rc.code != CC_OK) {
        *out_sessions = NULL;
        *out_count = 0;
        cc_mutex_unlock(store->mutex);
        return rc;
    }

    cc_json_value_t *sessions_arr = cc_json_object_get(store->root, "sessions");
    if (!sessions_arr || !cc_json_is_array(sessions_arr)) {
        *out_sessions = NULL;
        *out_count = 0;
        cc_mutex_unlock(store->mutex);
        return cc_result_ok();
    }

    int count = cc_json_array_size(sessions_arr);
    cc_session_t *sessions = calloc(count, sizeof(cc_session_t));
    if (!sessions) {
        cc_mutex_unlock(store->mutex);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate sessions");
    }

    for (int i = 0; i < count; i++) {
        cc_json_value_t *s = cc_json_array_get(sessions_arr, i);
        cc_session_t *out = &sessions[i];

        cc_json_value_t *v = cc_json_object_get(s, "id");
        out->id = strdup(cc_json_string_value(v) ? cc_json_string_value(v) : "");

        v = cc_json_object_get(s, "workspace_dir");
        out->workspace_dir = strdup(cc_json_string_value(v) ? cc_json_string_value(v) : "");

        v = cc_json_object_get(s, "workspace_dir");
        const char *name_str = cc_json_string_value(v);
        out->name = name_str ? strdup(name_str) : NULL;
    }

    *out_sessions = sessions;
    *out_count = (size_t)count;
    cc_mutex_unlock(store->mutex);
    return cc_result_ok();
}

/**
 * @brief vtable 函数：销毁存储实例
 *
 * 依次释放 JSON 根对象（递归销毁全部子节点）、文件路径字符串、存储结构体自身。
 *
 * @param self 存储实例指针
 */
static void json_file_destroy(void *self)
{
    cc_json_file_store_t *store = (cc_json_file_store_t *)self;
    cc_mutex_lock(store->mutex);
    cc_json_destroy(store->root);
    free(store->file_path);
    cc_mutex_unlock(store->mutex);
    cc_mutex_destroy(store->mutex);
    free(store);
}

/**
 * @brief JSON 文件存储的虚函数表
 *
 * 将全部 7 个 vtable 函数绑定到对应的 JSON 文件实现。
 */
static cc_session_store_vtable_t json_file_vtable = {
    json_file_create_session,
    json_file_append_message,
    json_file_load_messages,
    json_file_append_tool_call,
    json_file_append_tool_result,
    json_file_list_sessions,
    json_file_destroy
};

/**
 * @brief 创建 JSON 文件存储实例（公共工厂函数）
 *
 * 执行流程：
 *   1. 分配 cc_json_file_store_t 结构体
 *   2. 保存文件路径（用户指定则复制，否则默认 profile storage path）
 *   3. 调用 ensure_file_exists 确保文件存在
 *   4. 填充 out_store 的 self 和 vtable
 *
 * @param file_path  JSON 文件路径，可为 NULL 使用默认路径
 * @param out_store  输出参数，填充创建好的存储实例
 * @return cc_result_t 成功返回 OK，失败返回相应错误码
 */
cc_result_t cc_json_file_store_create(const char *file_path, cc_session_store_t *out_store)
{
    cc_json_file_store_t *self = calloc(1, sizeof(cc_json_file_store_t));
    if (!self) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create JSON file store");

    self->file_path = file_path ? strdup(file_path) : strdup(CC_DEFAULT_STORAGE_PATH);
    self->root = NULL;
    cc_result_t rc = cc_mutex_create(&self->mutex);
    if (rc.code != CC_OK) {
        free(self->file_path);
        free(self);
        return rc;
    }

    rc = ensure_file_exists(self->file_path);
    if (rc.code != CC_OK) {
        cc_mutex_destroy(self->mutex);
        free(self->file_path);
        free(self);
        return rc;
    }

    out_store->self = self;
    out_store->vtable = &json_file_vtable;
    return cc_result_ok();
}
