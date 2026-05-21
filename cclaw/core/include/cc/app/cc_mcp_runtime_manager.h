/**
 * cc_mcp_runtime_manager.h — MCP client runtime and tool bridge.
 *
 * 所属层次：核心 SDK。
 *
 * 本模块负责 MCP 协议状态机、session cache、TTL、工具注册和 JSON-RPC
 * request/response 语义。它不启动进程、不创建 socket、不依赖 curl/Win32；
 * 具体 stdio、HTTP、SSE、streamable HTTP 由 app/platform 提供 transport。
 */

#ifndef CC_MCP_RUNTIME_MANAGER_H
#define CC_MCP_RUNTIME_MANAGER_H

#include "cc/app/cc_cancel_token.h"
#include "cc/app/cc_runtime_features.h"
#include "cc/core/cc_result.h"
#include "cc/ports/cc_tool_registry.h"
#include "cc/util/cc_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cc_mcp_runtime_manager cc_mcp_runtime_manager_t;

typedef struct cc_mcp_transport {
    void *self;
    const struct cc_mcp_transport_vtable *vtable;
} cc_mcp_transport_t;

typedef struct cc_mcp_transport_vtable {
    /**
     * 发送一条完整 JSON-RPC request，并返回匹配该 request id 的 JSON-RPC response。
     *
     * out_response_json 由 transport 分配，core manager 在解析后 free。transport
     * 可以是串行的 stdio，也可以是允许并发的 HTTP；并发语义通过 is_serial 告诉
     * manager，由 manager 决定是否在 send 期间持有 server mutex。
     */
    cc_result_t (*send_json)(
        void *self,
        const char *request_json,
        int timeout_ms,
        cc_cancel_token_t *cancel_token,
        char **out_response_json
    );
    /** 复位连接/session。TTL、reload dispose 或 session reset 都走同一语义。 */
    cc_result_t (*reset)(void *self);
    /** 非 0 表示 transport 内部只能串行执行请求，例如单 stdio worker。 */
    int (*is_serial)(void *self);
    /** 销毁 transport 私有资源；manager 取得 transport 所有权后负责调用。 */
    void (*destroy)(void *self);
} cc_mcp_transport_vtable_t;

typedef cc_result_t (*cc_mcp_transport_factory_fn)(
    const cc_config_mcp_server_t *server_config,
    cc_mcp_transport_t *out_transport,
    void *user_data
);

cc_result_t cc_mcp_runtime_manager_create(
    cc_mcp_transport_factory_fn factory,
    void *factory_user_data,
    cc_mcp_runtime_manager_t **out_manager
);

void cc_mcp_runtime_manager_destroy(cc_mcp_runtime_manager_t *manager);

cc_result_t cc_mcp_runtime_manager_load_tools(
    cc_mcp_runtime_manager_t *manager,
    const cc_config_t *config,
    cc_tool_registry_t *registry,
    cc_runtime_diagnostics_t *diagnostics
);

/**
 * 判断 response_json 是否是 request_json 对应 id 的 JSON-RPC response。
 *
 * HTTP/SSE transport 用它从 event stream 中挑出当前请求的响应。这样 request id
 * 比较规则留在 SDK 协议层，而不是散落在各个平台 transport 里。
 */
cc_result_t cc_mcp_jsonrpc_response_matches_request(
    const char *request_json,
    const char *response_json,
    int *out_matches
);

#ifdef __cplusplus
}
#endif

#endif
