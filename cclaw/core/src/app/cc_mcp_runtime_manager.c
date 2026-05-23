/**
 * 学习导读：cclaw/core/src/app/cc_mcp_runtime_manager.c
 *
 * 所属层次：核心层。
 * 阅读重点：MCP 协议 JSON-RPC 状态机，重点看 initialize→notifications→
 *           tools/list→tools/call 的调用链、transport vtable 的串行/并发
 *           适配逻辑、tool bridge 命名规则以及 TTL/idle 管理。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * cc_mcp_runtime_manager.c — MCP 协议客户端运行时与工具桥接模块
 *
 * 本模块在整体架构中的角色：
 * ─────────────────────────────
 * 位于 App 层（业务逻辑层），是 SDK 侧 MCP 协议的完整状态机实现。它不启动
 * 进程、不创建 socket、不依赖 curl/Win32，只通过 transport vtable 发送
 * JSON-RPC 请求。具体 stdio、HTTP、SSE、streamable HTTP 等传输方式由
 * app/platform 通过 transport factory 注入。
 *
 * 上游调用方：
 *   - cc_agent_runtime.c —— 在运行时初始化阶段调用 load_tools 注册 MCP 工具
 *   - cc_tool_executor.c —— 通过 tool vtable 间接调用 mcp_tool_call
 *
 * 下游依赖模块：
 *   - cc_json.c —— JSON-RPC 请求构造与响应解析
 *   - cc_tool_registry.c —— 将 MCP tool 注册为 C-Claw 工具
 *   - transport vtable（外部注入）—— send_json / reset / is_serial / destroy
 *   - cc_cancel_token.c —— 工具调用时的取消传播
 *
 * ─── 内部数据结构 ───────────────────────────────────────────────────
 *
 *   cc_mcp_server_runtime_t：
 *     单个 MCP server 的运行时状态。持有 server 名称、transport 名称、
 *     transport 实例、互斥锁、自增 request id、last_used_ms 时间戳、
 *     idle_ttl_ms、connection_timeout_ms 和 needs_initialize 标记。
 *
 *   cc_mcp_tool_t：
 *     单个 MCP 工具的桥接对象。持有 tool_name（MCP 原始名）、display_name
 *     （mcp.<server>.<tool> 格式）、description、schema_json 和指向所属
 *     server 的指针。通过 cc_tool_vtable 暴露给上层工具执行框架。
 *
 *   cc_mcp_runtime_manager（主结构体）：
 *     持有 transport factory 函数指针、factory_user_data 和 servers 动态数组。
 *
 * ─── MCP 协议状态机 ─────────────────────────────────────────────────
 *
 *   每次 mcp_call_raw 调用经历的流程：
 *
 *   1. 检查 cancel token，已取消则立即返回
 *   2. 持锁检查 idle TTL：
 *      - 若距上次使用超过 idle_ttl_ms，调用 transport->reset 复位连接，
 *        并标记 needs_initialize=1
 *   3. 若非 initialize 请求，调用 ensure_initialized_locked：
 *      - 若 needs_initialize==1，发送 initialize 请求（持锁，保证握手原子性）
 *      - 成功后置 needs_initialize=0，记录 last_used_ms
 *   4. 分配 request id（++next_id），更新 last_used_ms
 *   5. 构建 JSON-RPC 请求（jsonrpc_request）
 *   6. 调用 transport_send_locked_or_parallel：
 *      - 串行 transport（is_serial 返回真）：在 server mutex 内发送，
 *        保证同一时刻只有一个请求在 IO 路径上
 *      - 并发 transport（is_serial 返回假）：释放 mutex 后发送，
 *        允许多个请求同时在不同线程中执行
 *   7. 验证响应 id 匹配（cc_mcp_jsonrpc_response_matches_request）
 *   8. 解析响应 JSON，检查 error 字段
 *
 * ─── Tool Bridge 命名规则 ────────────────────────────────────────────
 *
 *   MCP 工具在 C-Claw 中以 "mcp.<server>.<tool>" 格式暴露，例如
 *   "mcp.filesystem.read_file"。display_name 在 register_mcp_tool 中
 *   由 snprintf 构造，确保名称全局唯一且可追溯到来源 server。
 *
 * ─── TTL 与重置管理 ─────────────────────────────────────────────────
 *
 *   idle_ttl_ms：从 config->mcp.session_idle_ttl_ms 传入。当 server
 *   上次使用时间距今超过此值，在下次调用前自动调用 transport->reset
 *   复位连接并重新 initialize。idle_ttl_ms <= 0 表示永不过期。
 *
 *   needs_initialize：标记 server 是否需要重新握手。创建时初始为 1，
 *   每次 TTL 过期 reset 后也置为 1。
 *
 * ─── 设计决策 ───────────────────────────────────────────────────────
 *
 *   为什么 initialize 握手必须在 mutex 内完成？
 *     initialize 是连接级握手，即使 HTTP transport 可以并发，握手也
 *     必须在 mutex 保护下进行，避免两个线程同时发现 needs_initialize
 *     并重复发送初始化请求，导致状态混乱。
 *
 *   为什么 mcp_call_raw 区分 transport 串行/并发语义？
 *     串行 transport（如 stdio）只有一个 IO 线程，若不在 mutex 内发送，
 *     多个请求会交错写入管道导致协议错乱。并发 transport（如 HTTP）可以
 *     并行发送，持锁只保护 request id 分配和 TTL 状态。is_serial vtable
 *     方法让 transport 自己声明其并发能力，manager 据此决定锁策略。
 *
 *   为什么 tools/list 失败被记录到 diagnostics 而非中断整个加载？
 *     load_tools 遍历所有 MCP server，逐个 initialize + list_tools +
 *     register。单个 server 失败时记录 diagnostics 后 continue，
 *     不影响其他 server 的加载。这保证部分 MCP server 不可用不会导致
 *     整个 agent 初始化失败。
 *
 *   为什么 mcp_tool_call 将 JSON-RPC error 转换为 cc_tool_result_t.error？
 *     MCP 协议中 error 字段表示调用层面的错误（如工具不存在、参数非法），
 *     不是传输层错误。tool bridge 将其映射为 result.ok=0 的 tool_result，
 *     让上层 agent 可以像处理普通工具错误一样处理 MCP 错误，无需区分。
 */

#define _POSIX_C_SOURCE 200809L

#include "cc/app/cc_mcp_runtime_manager.h"
#include "cc/ports/cc_thread.h"
#include "cc/util/cc_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*
 * MCP runtime manager 是 core SDK 的协议状态机：
 *   - server_runtime 持有 transport、初始化状态、TTL 和 request id。
 *   - tool bridge 把 MCP tool 暴露成 C-Claw tool 名称 mcp.<server>.<tool>。
 *   - transport 只通过 vtable 调用，core 不知道它是 stdio、HTTP、SSE 还是 ESP HTTP。
 *
 * 锁约定：串行 transport 在 send 期间持有 server mutex；并发 HTTP transport
 * 只在分配 request id、检查 TTL、更新 last_used 时持锁，实际 IO 在锁外执行。
 */
typedef struct cc_mcp_server_runtime {
    char *name;
    char *transport_name;
    cc_mcp_transport_t transport;
    cc_mutex_t mutex;
    unsigned long next_id;
    long last_used_ms;
    int idle_ttl_ms;
    int connection_timeout_ms;
    int needs_initialize;
} cc_mcp_server_runtime_t;

typedef struct cc_mcp_tool {
    char *tool_name;
    char *display_name;
    char *description;
    char *schema_json;
    cc_mcp_server_runtime_t *server;
} cc_mcp_tool_t;

struct cc_mcp_runtime_manager {
    cc_mcp_transport_factory_fn factory;
    void *factory_user_data;
    cc_mcp_server_runtime_t **servers;
    size_t server_count;
};

static long wall_time_ms(void)
{
    return (long)time(NULL) * 1000L;
}

static int server_transport_is_serial(cc_mcp_server_runtime_t *server)
{
    if (!server || !server->transport.vtable || !server->transport.vtable->is_serial) return 1;
    return server->transport.vtable->is_serial(server->transport.self);
}

static int server_idle_expired(cc_mcp_server_runtime_t *server, long now_ms)
{
    if (!server || server->idle_ttl_ms <= 0 || server->last_used_ms <= 0) return 0;
    return now_ms - server->last_used_ms >= server->idle_ttl_ms;
}

static cc_json_value_t *build_initialize_params(void)
{
    /*
     * initialize 参数保持最小集合：声明 protocolVersion、空 capabilities 和 clientInfo。
     * 这样 mock server、stdio server 和 streamable HTTP server 都能用同一条路径。
     */
    cc_json_value_t *params = cc_json_create_object();
    cc_json_object_set(params, "protocolVersion", cc_json_create_string("2024-11-05"));
    cc_json_object_set(params, "capabilities", cc_json_create_object());
    cc_json_value_t *client = cc_json_create_object();
    cc_json_object_set(client, "name", cc_json_create_string("c-claw"));
    cc_json_object_set(client, "version", cc_json_create_string("1.0"));
    cc_json_object_set(params, "clientInfo", client);
    return params;
}

static char *jsonrpc_request(unsigned long id, const char *method, cc_json_value_t *params)
{
    cc_json_value_t *root = cc_json_create_object();
    if (!root) {
        cc_json_destroy(params);
        return NULL;
    }
    cc_json_object_set(root, "jsonrpc", cc_json_create_string("2.0"));
    cc_json_object_set(root, "id", cc_json_create_number((double)id));
    cc_json_object_set(root, "method", cc_json_create_string(method));
    if (params) cc_json_object_set(root, "params", params);
    char *json = cc_json_stringify_unformatted(root);
    cc_json_destroy(root);
    return json;
}

static cc_result_t jsonrpc_id_string(const char *json, char **out_id)
{
    *out_id = NULL;
    cc_json_value_t *root = NULL;
    cc_result_t rc = cc_json_parse(json ? json : "", &root);
    if (rc.code != CC_OK) return rc;
    cc_json_value_t *id = cc_json_object_get(root, "id");
    if (id) *out_id = cc_json_stringify_unformatted(id);
    cc_json_destroy(root);
    if (id && !*out_id) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy JSON-RPC id");
    return cc_result_ok();
}

cc_result_t cc_mcp_jsonrpc_response_matches_request(
    const char *request_json,
    const char *response_json,
    int *out_matches
)
{
    if (!request_json || !response_json || !out_matches) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid JSON-RPC match arguments");
    }
    *out_matches = 0;
    char *request_id = NULL;
    char *response_id = NULL;
    cc_result_t rc = jsonrpc_id_string(request_json, &request_id);
    if (rc.code == CC_OK) rc = jsonrpc_id_string(response_json, &response_id);
    if (rc.code == CC_OK && request_id && response_id) {
        *out_matches = strcmp(request_id, response_id) == 0;
    }
    free(request_id);
    free(response_id);
    return rc;
}

static cc_result_t transport_send_locked_or_parallel(
    cc_mcp_server_runtime_t *server,
    char *request,
    int timeout_ms,
    cc_cancel_token_t *cancel_token,
    char **out_response_json,
    int lock_is_held
)
{
    if (!server->transport.vtable || !server->transport.vtable->send_json) {
        if (lock_is_held) cc_mutex_unlock(server->mutex);
        return cc_result_error(CC_ERR_PLATFORM, "MCP transport has no send_json implementation");
    }

    int serial = server_transport_is_serial(server);
    if (!serial && lock_is_held) cc_mutex_unlock(server->mutex);
    cc_result_t rc = server->transport.vtable->send_json(
        server->transport.self,
        request,
        timeout_ms > 0 ? timeout_ms :
            (server->connection_timeout_ms > 0 ? server->connection_timeout_ms : 30000),
        cancel_token,
        out_response_json);
    if (serial && lock_is_held) cc_mutex_unlock(server->mutex);
    return rc;
}

static cc_result_t send_request_and_parse(
    cc_mcp_server_runtime_t *server,
    char *request,
    int timeout_ms,
    cc_cancel_token_t *cancel_token,
    cc_json_value_t **out_response,
    int lock_is_held
)
{
    char *response_json = NULL;
    cc_result_t rc = transport_send_locked_or_parallel(
        server, request, timeout_ms, cancel_token, &response_json, lock_is_held);
    if (rc.code != CC_OK) {
        free(request);
        return rc;
    }

    int matches = 0;
    rc = cc_mcp_jsonrpc_response_matches_request(request, response_json, &matches);
    free(request);
    if (rc.code != CC_OK) {
        free(response_json);
        return rc;
    }
    if (!matches) {
        free(response_json);
        return cc_result_error(CC_ERR_JSON, "MCP transport returned a mismatched JSON-RPC response id");
    }

    rc = cc_json_parse(response_json, out_response);
    free(response_json);
    return rc;
}

static cc_result_t send_request_and_parse_while_locked(
    cc_mcp_server_runtime_t *server,
    char *request,
    cc_json_value_t **out_response
)
{
    if (!server->transport.vtable || !server->transport.vtable->send_json) {
        free(request);
        return cc_result_error(CC_ERR_PLATFORM, "MCP transport has no send_json implementation");
    }
    char *response_json = NULL;
    cc_result_t rc = server->transport.vtable->send_json(
        server->transport.self,
        request,
        server->connection_timeout_ms > 0 ? server->connection_timeout_ms : 30000,
        NULL,
        &response_json);
    if (rc.code != CC_OK) {
        free(request);
        return rc;
    }
    int matches = 0;
    rc = cc_mcp_jsonrpc_response_matches_request(request, response_json, &matches);
    free(request);
    if (rc.code != CC_OK) {
        free(response_json);
        return rc;
    }
    if (!matches) {
        free(response_json);
        return cc_result_error(CC_ERR_JSON, "MCP transport returned a mismatched JSON-RPC response id");
    }
    rc = cc_json_parse(response_json, out_response);
    free(response_json);
    return rc;
}

static cc_result_t ensure_initialized_locked(cc_mcp_server_runtime_t *server)
{
    if (!server->needs_initialize) {
        cc_mutex_unlock(server->mutex);
        return cc_result_ok();
    }

    unsigned long id = ++server->next_id;
    char *request = jsonrpc_request(id, "initialize", build_initialize_params());
    if (!request) {
        cc_mutex_unlock(server->mutex);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to build MCP initialize request");
    }

    cc_json_value_t *response = NULL;
    /*
     * initialize 是连接级握手。即使 HTTP transport 可以并发，握手也必须在
     * server mutex 内完成，避免两个线程同时发现 needs_initialize 并重复握手。
     */
    cc_result_t rc = send_request_and_parse_while_locked(server, request, &response);
    cc_json_destroy(response);
    if (rc.code == CC_OK) {
        server->needs_initialize = 0;
        server->last_used_ms = wall_time_ms();
    }
    cc_mutex_unlock(server->mutex);
    return rc;
}

static cc_result_t mcp_call_raw(
    cc_mcp_server_runtime_t *server,
    const char *method,
    cc_json_value_t *params,
    int timeout_ms,
    cc_cancel_token_t *cancel_token,
    cc_json_value_t **out_response
)
{
    if (!server || !method || !out_response) {
        cc_json_destroy(params);
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid MCP call");
    }
    *out_response = NULL;
    if (cc_cancel_token_is_cancelled(cancel_token)) {
        cc_json_destroy(params);
        return cc_result_error(CC_ERR_CANCELLED, "MCP call cancelled before send");
    }

    cc_mutex_lock(server->mutex);
    long now_ms = wall_time_ms();
    if (server_idle_expired(server, now_ms)) {
        if (server->transport.vtable && server->transport.vtable->reset) {
            cc_result_t reset_rc = server->transport.vtable->reset(server->transport.self);
            if (reset_rc.code != CC_OK) {
                cc_json_destroy(params);
                cc_mutex_unlock(server->mutex);
                return reset_rc;
            }
        }
        server->needs_initialize = 1;
    }

    if (strcmp(method, "initialize") != 0) {
        cc_result_t init_rc = ensure_initialized_locked(server);
        if (init_rc.code != CC_OK) {
            cc_json_destroy(params);
            return init_rc;
        }
        /*
         * ensure_initialized_locked 总是在返回前释放 server mutex。这里重新加锁，
         * 是为了保护 request id 分配、TTL 状态更新和 serial transport 的 send。
         */
        cc_mutex_lock(server->mutex);
    }

    unsigned long id = ++server->next_id;
    server->last_used_ms = wall_time_ms();
    char *request = jsonrpc_request(id, method, params);
    if (!request) {
        cc_mutex_unlock(server->mutex);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to build MCP request");
    }

    cc_result_t rc = send_request_and_parse(
        server, request, timeout_ms, cancel_token, out_response, 1);
    if (rc.code == CC_OK && strcmp(method, "initialize") == 0) {
        cc_mutex_lock(server->mutex);
        server->needs_initialize = 0;
        server->last_used_ms = wall_time_ms();
        cc_mutex_unlock(server->mutex);
    }
    return rc;
}

static void server_runtime_destroy(cc_mcp_server_runtime_t *server)
{
    if (!server) return;
    if (server->transport.vtable && server->transport.vtable->destroy) {
        server->transport.vtable->destroy(server->transport.self);
    }
    if (server->mutex) cc_mutex_destroy(server->mutex);
    free(server->name);
    free(server->transport_name);
    free(server);
}

cc_result_t cc_mcp_runtime_manager_create(
    cc_mcp_transport_factory_fn factory,
    void *factory_user_data,
    cc_mcp_runtime_manager_t **out_manager
)
{
    if (!factory || !out_manager) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid MCP runtime manager arguments");
    }
    cc_mcp_runtime_manager_t *manager = calloc(1, sizeof(*manager));
    if (!manager) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create MCP runtime manager");
    manager->factory = factory;
    manager->factory_user_data = factory_user_data;
    *out_manager = manager;
    return cc_result_ok();
}

void cc_mcp_runtime_manager_destroy(cc_mcp_runtime_manager_t *manager)
{
    if (!manager) return;
    for (size_t i = 0; i < manager->server_count; i++) {
        server_runtime_destroy(manager->servers[i]);
    }
    free(manager->servers);
    free(manager);
}

static const char *mcp_tool_name(void *self)
{
    return ((cc_mcp_tool_t *)self)->display_name;
}

static const char *mcp_tool_description(void *self)
{
    return ((cc_mcp_tool_t *)self)->description;
}

static const char *mcp_tool_schema_json(void *self)
{
    return ((cc_mcp_tool_t *)self)->schema_json;
}

static cc_result_t mcp_tool_call(
    void *self,
    const char *args_json,
    const cc_tool_context_t *ctx,
    cc_tool_result_t *out_result
)
{
    cc_mcp_tool_t *tool = (cc_mcp_tool_t *)self;
    memset(out_result, 0, sizeof(*out_result));

    cc_json_value_t *params = cc_json_create_object();
    cc_json_object_set(params, "name", cc_json_create_string(tool->tool_name));
    cc_json_value_t *arguments = NULL;
    cc_result_t parse_rc = cc_json_parse(args_json ? args_json : "{}", &arguments);
    if (parse_rc.code != CC_OK || !arguments) {
        cc_result_free(&parse_rc);
        arguments = cc_json_create_object();
    }
    cc_json_object_set(params, "arguments", arguments);

    cc_json_value_t *response = NULL;
    cc_cancel_token_t *token = ctx ? ctx->cancel_token : NULL;
    int timeout_ms = ctx && ctx->timeout_ms > 0 ? ctx->timeout_ms : 0;
    cc_result_t rc = mcp_call_raw(
        tool->server, "tools/call", params, timeout_ms, token, &response);
    if (rc.code != CC_OK) {
        out_result->ok = 0;
        out_result->error = strdup(rc.message ? rc.message : "MCP tool call failed");
        cc_result_free(&rc);
        return cc_result_ok();
    }

    cc_json_value_t *error = cc_json_object_get(response, "error");
    if (error) {
        char *err = cc_json_stringify_unformatted(error);
        out_result->ok = 0;
        out_result->error = err ? err : strdup("MCP server returned an error");
        cc_json_destroy(response);
        return cc_result_ok();
    }

    cc_json_value_t *result = cc_json_object_get(response, "result");
    out_result->ok = 1;
    out_result->content = result ? cc_json_stringify_unformatted(result) : strdup("{}");
    cc_json_destroy(response);
    return cc_result_ok();
}

static void mcp_tool_destroy(void *self)
{
    cc_mcp_tool_t *tool = (cc_mcp_tool_t *)self;
    if (!tool) return;
    free(tool->tool_name);
    free(tool->display_name);
    free(tool->description);
    free(tool->schema_json);
    free(tool);
}

static const cc_tool_vtable_t mcp_tool_vtable = {
    mcp_tool_name,
    mcp_tool_description,
    mcp_tool_schema_json,
    mcp_tool_call,
    mcp_tool_destroy
};

static cc_result_t register_mcp_tool(
    cc_tool_registry_t *registry,
    cc_mcp_server_runtime_t *server,
    cc_json_value_t *tool_json
)
{
    const char *name = cc_json_string_value(cc_json_object_get(tool_json, "name"));
    if (!name || !name[0]) return cc_result_ok();
    const char *description = cc_json_string_value(cc_json_object_get(tool_json, "description"));
    cc_json_value_t *schema = cc_json_object_get(tool_json, "inputSchema");
    if (!schema) schema = cc_json_object_get(tool_json, "parameters");

    cc_mcp_tool_t *tool_self = calloc(1, sizeof(*tool_self));
    if (!tool_self) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create MCP tool");
    tool_self->tool_name = strdup(name);
    tool_self->description = strdup(description ? description : "MCP tool");
    tool_self->schema_json = schema ? cc_json_stringify_unformatted(schema) :
        strdup("{\"type\":\"object\",\"properties\":{}}");
    size_t display_len = strlen("mcp..") + strlen(server->name) + strlen(name) + 1;
    tool_self->display_name = malloc(display_len);
    if (tool_self->display_name) {
        snprintf(tool_self->display_name, display_len, "mcp.%s.%s", server->name, name);
    }
    tool_self->server = server;

    if (!tool_self->tool_name || !tool_self->display_name ||
        !tool_self->description || !tool_self->schema_json) {
        mcp_tool_destroy(tool_self);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy MCP tool metadata");
    }

    cc_tool_t tool = {0};
    tool.self = tool_self;
    tool.vtable = &mcp_tool_vtable;
    cc_result_t rc = cc_tool_registry_add(registry, tool);
    if (rc.code != CC_OK) {
        mcp_tool_destroy(tool_self);
    }
    return rc;
}

static cc_result_t list_server_tools(
    cc_mcp_server_runtime_t *server,
    cc_tool_registry_t *registry
)
{
    cc_json_value_t *response = NULL;
    cc_result_t rc = mcp_call_raw(server, "tools/list", cc_json_create_object(), 0, NULL, &response);
    if (rc.code != CC_OK) return rc;
    cc_json_value_t *error = cc_json_object_get(response, "error");
    if (error) {
        char *err = cc_json_stringify_unformatted(error);
        rc = cc_result_error(CC_ERR_TOOL, err ? err : "MCP tools/list returned an error");
        free(err);
        cc_json_destroy(response);
        return rc;
    }
    cc_json_value_t *result = cc_json_object_get(response, "result");
    cc_json_value_t *tools = result ? cc_json_object_get(result, "tools") : NULL;
    if (!tools || !cc_json_is_array(tools)) {
        cc_json_destroy(response);
        return cc_result_ok();
    }
    int count = cc_json_array_size(tools);
    for (int i = 0; i < count; i++) {
        cc_result_t trc = register_mcp_tool(registry, server, cc_json_array_get(tools, i));
        if (trc.code != CC_OK) {
            cc_json_destroy(response);
            return trc;
        }
    }
    cc_json_destroy(response);
    return cc_result_ok();
}

static cc_result_t manager_add_server(
    cc_mcp_runtime_manager_t *manager,
    cc_mcp_server_runtime_t *server
)
{
    cc_mcp_server_runtime_t **next = realloc(
        manager->servers, (manager->server_count + 1) * sizeof(*next));
    if (!next) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to grow MCP server table");
    manager->servers = next;
    manager->servers[manager->server_count++] = server;
    return cc_result_ok();
}

static cc_result_t server_runtime_create(
    cc_mcp_runtime_manager_t *manager,
    const cc_config_mcp_server_t *config,
    int idle_ttl_ms,
    cc_mcp_server_runtime_t **out_server
)
{
    *out_server = NULL;
    if (!config->name || !config->name[0]) return cc_result_ok();
    cc_mcp_transport_t transport;
    memset(&transport, 0, sizeof(transport));
    cc_result_t rc = manager->factory(config, &transport, manager->factory_user_data);
    if (rc.code != CC_OK) return rc;
    if (!transport.vtable) return cc_result_ok();

    cc_mcp_server_runtime_t *server = calloc(1, sizeof(*server));
    if (!server) {
        if (transport.vtable->destroy) transport.vtable->destroy(transport.self);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create MCP server runtime");
    }
    server->name = strdup(config->name);
    server->transport_name = strdup(config->transport ? config->transport : "stdio");
    server->transport = transport;
    server->idle_ttl_ms = idle_ttl_ms;
    server->connection_timeout_ms = config->connection_timeout_ms;
    server->needs_initialize = 1;
    rc = cc_mutex_create(&server->mutex);
    if (rc.code == CC_OK && (!server->name || !server->transport_name)) {
        rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy MCP server config");
    }
    if (rc.code != CC_OK) {
        server_runtime_destroy(server);
        return rc;
    }
    *out_server = server;
    return cc_result_ok();
}

cc_result_t cc_mcp_runtime_manager_load_tools(
    cc_mcp_runtime_manager_t *manager,
    const cc_config_t *config,
    cc_tool_registry_t *registry,
    cc_runtime_diagnostics_t *diagnostics
)
{
    if (!manager || !config || !registry || !config->mcp.enabled) return cc_result_ok();
    for (size_t i = 0; i < config->mcp.server_count; i++) {
        const cc_config_mcp_server_t *server_cfg = &config->mcp.servers[i];
        cc_mcp_server_runtime_t *server = NULL;
        cc_result_t rc = server_runtime_create(
            manager, server_cfg, config->mcp.session_idle_ttl_ms, &server);
        if (rc.code != CC_OK || !server) {
            char message[192];
            snprintf(message, sizeof(message), "failed to create transport: %s",
                rc.message ? rc.message : "unsupported or disabled transport");
            cc_runtime_diagnostics_add(diagnostics, "mcp",
                server_cfg->name ? server_cfg->name : "(unnamed)",
                message);
            cc_result_free(&rc);
            continue;
        }

        cc_json_value_t *init_response = NULL;
        rc = mcp_call_raw(server, "initialize", build_initialize_params(), 0, NULL, &init_response);
        cc_json_destroy(init_response);
        if (rc.code != CC_OK) {
            cc_runtime_diagnostics_add(diagnostics, "mcp",
                server->name ? server->name : "(unnamed)",
                rc.message ? rc.message : "failed to initialize MCP server");
            cc_result_free(&rc);
            server_runtime_destroy(server);
            continue;
        }

        rc = manager_add_server(manager, server);
        if (rc.code != CC_OK) {
            server_runtime_destroy(server);
            return rc;
        }

        rc = list_server_tools(server, registry);
        if (rc.code != CC_OK) {
            cc_runtime_diagnostics_add(diagnostics, "mcp",
                server->name ? server->name : "(unnamed)",
                rc.message ? rc.message : "failed to list/register MCP tools");
            cc_result_free(&rc);
        }
    }
    return cc_result_ok();
}
