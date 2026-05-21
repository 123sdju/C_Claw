#define _POSIX_C_SOURCE 200809L

#include "cc/mcp/cc_mcp_manager.h"
#include "cc/app/cc_cancel_token.h"
#include "cc/app/cc_mcp_runtime_manager.h"
#include "cc/app/cc_sse_parser.h"
#include "cc/ports/cc_http_client.h"
#include "cc/ports/cc_process.h"
#include "cc/ports/cc_thread.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

typedef struct cc_mcp_stdio_transport {
    char *command;
    char **argv;
    cc_process_pipe_t *pipe;
} cc_mcp_stdio_transport_t;

typedef struct cc_mcp_http_transport {
    char *url;
    char *transport_name;
    char *session_id;
    cc_mutex_t mutex;
} cc_mcp_http_transport_t;

struct cc_mcp_manager {
    cc_mcp_runtime_manager_t *core;
};

static int ascii_case_equal(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *b) {
        char ca = *a;
        char cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return 0;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static int ascii_contains_case(const char *text, const char *needle)
{
    if (!text || !needle || !*needle) return 0;
    size_t needle_len = strlen(needle);
    for (const char *p = text; *p; p++) {
        size_t i = 0;
        while (i < needle_len && p[i]) {
            char ca = p[i];
            char cb = needle[i];
            if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
            if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
            if (ca != cb) break;
            i++;
        }
        if (i == needle_len) return 1;
    }
    return 0;
}

static const char *response_header_value(const cc_http_response_t *response, const char *name)
{
    if (!response || !name) return NULL;
    for (size_t i = 0; i < response->header_count; i++) {
        if (ascii_case_equal(response->headers[i].name, name)) {
            return response->headers[i].value;
        }
    }
    return NULL;
}

static char **copy_argv(const cc_config_mcp_server_t *server)
{
    size_t argc = server->arg_count + 2;
    char **argv = calloc(argc, sizeof(char *));
    if (!argv) return NULL;
    argv[0] = strdup(server->command ? server->command : "");
    for (size_t i = 0; i < server->arg_count; i++) {
        argv[i + 1] = strdup(server->args[i] ? server->args[i] : "");
    }
    argv[argc - 1] = NULL;
    return argv;
}

static void free_argv(char **argv)
{
    if (!argv) return;
    for (size_t i = 0; argv[i]; i++) free(argv[i]);
    free(argv);
}

#if CC_ENABLE_MCP_STDIO
static cc_result_t stdio_start(cc_mcp_stdio_transport_t *transport)
{
    if (transport->pipe) return cc_result_ok();
    return cc_process_pipe_spawn(transport->command, transport->argv, &transport->pipe);
}

static cc_result_t stdio_send_json(
    void *self,
    const char *request_json,
    int timeout_ms,
    cc_cancel_token_t *cancel_token,
    char **out_response_json
)
{
    cc_mcp_stdio_transport_t *transport = (cc_mcp_stdio_transport_t *)self;
    *out_response_json = NULL;
    if (cc_cancel_token_is_cancelled(cancel_token)) {
        return cc_result_error(CC_ERR_CANCELLED, "MCP stdio request cancelled before send");
    }

    cc_result_t rc = stdio_start(transport);
    if (rc.code == CC_OK) rc = cc_process_pipe_write(transport->pipe, request_json);
    if (rc.code == CC_OK) {
        rc = cc_process_pipe_read_line_timeout(transport->pipe, timeout_ms, out_response_json);
    }
    if (cc_cancel_token_is_cancelled(cancel_token)) {
        free(*out_response_json);
        *out_response_json = NULL;
        cc_result_free(&rc);
        return cc_result_error(CC_ERR_CANCELLED, "MCP stdio request cancelled");
    }
    return rc;
}

static cc_result_t stdio_reset(void *self)
{
    cc_mcp_stdio_transport_t *transport = (cc_mcp_stdio_transport_t *)self;
    if (transport->pipe) {
        cc_process_pipe_destroy(transport->pipe);
        transport->pipe = NULL;
    }
    return cc_result_ok();
}

static int stdio_is_serial(void *self)
{
    (void)self;
    return 1;
}

static void stdio_destroy(void *self)
{
    cc_mcp_stdio_transport_t *transport = (cc_mcp_stdio_transport_t *)self;
    if (!transport) return;
    if (transport->pipe) cc_process_pipe_destroy(transport->pipe);
    free(transport->command);
    free_argv(transport->argv);
    free(transport);
}

static const cc_mcp_transport_vtable_t stdio_vtable = {
    stdio_send_json,
    stdio_reset,
    stdio_is_serial,
    stdio_destroy
};

static cc_result_t create_stdio_transport(
    const cc_config_mcp_server_t *server_config,
    cc_mcp_transport_t *out_transport
)
{
    if (!server_config->command) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "MCP stdio server requires command");
    }
    cc_mcp_stdio_transport_t *transport = calloc(1, sizeof(*transport));
    if (!transport) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create MCP stdio transport");
    transport->command = strdup(server_config->command);
    transport->argv = copy_argv(server_config);
    if (!transport->command || !transport->argv) {
        stdio_destroy(transport);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy MCP stdio config");
    }
    cc_result_t rc = stdio_start(transport);
    if (rc.code != CC_OK) {
        stdio_destroy(transport);
        return rc;
    }
    out_transport->self = transport;
    out_transport->vtable = &stdio_vtable;
    return cc_result_ok();
}
#endif

typedef struct mcp_sse_body_ctx {
    cc_sse_parser_t *parser;
    const char *request_json;
    cc_cancel_token_t *cancel_token;
    char *matched_response;
} mcp_sse_body_ctx_t;

static cc_result_t sse_event_for_response(const char *data, void *user_data)
{
    mcp_sse_body_ctx_t *ctx = (mcp_sse_body_ctx_t *)user_data;
    if (!data || strcmp(data, "[DONE]") == 0 || ctx->matched_response) {
        return cc_result_ok();
    }
    int matches = 0;
    cc_result_t rc = cc_mcp_jsonrpc_response_matches_request(ctx->request_json, data, &matches);
    if (rc.code != CC_OK) return rc;
    if (matches) {
        ctx->matched_response = strdup(data);
        if (!ctx->matched_response) {
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy MCP SSE response");
        }
    }
    return cc_result_ok();
}

static cc_result_t sse_body_callback(const char *data, size_t len, void *user_data)
{
    mcp_sse_body_ctx_t *ctx = (mcp_sse_body_ctx_t *)user_data;
    if (cc_cancel_token_is_cancelled(ctx->cancel_token)) {
        return cc_result_error(CC_ERR_CANCELLED, "MCP HTTP stream cancelled");
    }
    return cc_sse_parser_feed(ctx->parser, data, len, sse_event_for_response, ctx);
}

#if CC_ENABLE_MCP_HTTP
static char *http_copy_session_id(cc_mcp_http_transport_t *transport)
{
    char *copy = NULL;
    cc_mutex_lock(transport->mutex);
    if (transport->session_id) copy = strdup(transport->session_id);
    cc_mutex_unlock(transport->mutex);
    return copy;
}

static void http_update_session_id(cc_mcp_http_transport_t *transport, const char *session_id)
{
    if (!session_id || !session_id[0]) return;
    cc_mutex_lock(transport->mutex);
    char *copy = strdup(session_id);
    if (copy) {
        free(transport->session_id);
        transport->session_id = copy;
    }
    cc_mutex_unlock(transport->mutex);
}

static int http_is_streaming_mode(const char *transport_name)
{
    return strcmp(transport_name, "sse") == 0 ||
        strcmp(transport_name, "streamable_http") == 0;
}

static cc_result_t http_send_json(
    void *self,
    const char *request_json,
    int timeout_ms,
    cc_cancel_token_t *cancel_token,
    char **out_response_json
)
{
    cc_mcp_http_transport_t *transport = (cc_mcp_http_transport_t *)self;
    *out_response_json = NULL;
    if (cc_cancel_token_is_cancelled(cancel_token)) {
        return cc_result_error(CC_ERR_CANCELLED, "MCP HTTP request cancelled before send");
    }

    char *session_id = http_copy_session_id(transport);
    cc_http_header_t headers[3];
    size_t header_count = 0;
    headers[header_count++] = (cc_http_header_t){"Content-Type", "application/json"};
    headers[header_count++] = (cc_http_header_t){"Accept", "application/json, text/event-stream"};
    if (session_id) {
        headers[header_count++] = (cc_http_header_t){"Mcp-Session-Id", session_id};
    }

    cc_sse_parser_t *parser = NULL;
    mcp_sse_body_ctx_t sse_ctx;
    memset(&sse_ctx, 0, sizeof(sse_ctx));
    int streaming_mode = http_is_streaming_mode(transport->transport_name);
    if (streaming_mode) {
        cc_result_t prc = cc_sse_parser_create(&parser);
        if (prc.code != CC_OK) {
            free(session_id);
            return prc;
        }
        sse_ctx.parser = parser;
        sse_ctx.request_json = request_json;
        sse_ctx.cancel_token = cancel_token;
    }

    cc_http_request_t http_req;
    memset(&http_req, 0, sizeof(http_req));
    http_req.method = "POST";
    http_req.url = transport->url;
    http_req.headers = headers;
    http_req.header_count = header_count;
    http_req.body = request_json;
    http_req.timeout_ms = timeout_ms > 0 ? timeout_ms : 30000;
    http_req.max_response_bytes = streaming_mode ? 1024 * 1024 : 0;
    http_req.on_body = streaming_mode ? sse_body_callback : NULL;
    http_req.user_data = streaming_mode ? &sse_ctx : NULL;
    http_req.cancel_token = cancel_token;

    cc_http_response_t http_resp;
    memset(&http_resp, 0, sizeof(http_resp));
    cc_result_t rc = cc_http_client_perform(&http_req, &http_resp);
    free(session_id);
    if (rc.code != CC_OK) {
        free(sse_ctx.matched_response);
        cc_sse_parser_destroy(parser);
        return rc;
    }

    http_update_session_id(transport, response_header_value(&http_resp, "Mcp-Session-Id"));
    const char *content_type = response_header_value(&http_resp, "Content-Type");
    int is_event_stream = content_type && ascii_contains_case(content_type, "text/event-stream");

    if (streaming_mode && (is_event_stream || sse_ctx.matched_response ||
                           strcmp(transport->transport_name, "sse") == 0)) {
        if (!sse_ctx.matched_response) {
            rc = cc_sse_parser_finish(parser, sse_event_for_response, &sse_ctx);
        }
        if (rc.code == CC_OK && sse_ctx.matched_response) {
            *out_response_json = sse_ctx.matched_response;
            sse_ctx.matched_response = NULL;
        } else if (rc.code == CC_OK) {
            rc = cc_result_error(CC_ERR_JSON, "MCP SSE stream ended without matching JSON-RPC response");
        }
    } else {
        *out_response_json = strdup(http_resp.body ? http_resp.body : "{}");
        if (!*out_response_json) {
            rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy MCP HTTP response");
        }
    }

    free(sse_ctx.matched_response);
    cc_sse_parser_destroy(parser);
    cc_http_response_free(&http_resp);
    return rc;
}

static cc_result_t http_reset(void *self)
{
    cc_mcp_http_transport_t *transport = (cc_mcp_http_transport_t *)self;
    cc_mutex_lock(transport->mutex);
    free(transport->session_id);
    transport->session_id = NULL;
    cc_mutex_unlock(transport->mutex);
    return cc_result_ok();
}

static int http_is_serial(void *self)
{
    (void)self;
    return 0;
}

static void http_destroy(void *self)
{
    cc_mcp_http_transport_t *transport = (cc_mcp_http_transport_t *)self;
    if (!transport) return;
    if (transport->mutex) cc_mutex_destroy(transport->mutex);
    free(transport->url);
    free(transport->transport_name);
    free(transport->session_id);
    free(transport);
}

static const cc_mcp_transport_vtable_t http_vtable = {
    http_send_json,
    http_reset,
    http_is_serial,
    http_destroy
};

static cc_result_t create_http_transport(
    const cc_config_mcp_server_t *server_config,
    cc_mcp_transport_t *out_transport
)
{
    if (!server_config->url) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "MCP HTTP server requires url");
    }
    cc_mcp_http_transport_t *transport = calloc(1, sizeof(*transport));
    if (!transport) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create MCP HTTP transport");
    transport->url = strdup(server_config->url);
    transport->transport_name = strdup(server_config->transport ? server_config->transport : "http");
    cc_result_t rc = cc_mutex_create(&transport->mutex);
    if (rc.code == CC_OK && (!transport->url || !transport->transport_name)) {
        rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy MCP HTTP config");
    }
    if (rc.code != CC_OK) {
        http_destroy(transport);
        return rc;
    }
    out_transport->self = transport;
    out_transport->vtable = &http_vtable;
    return cc_result_ok();
}
#endif

static cc_result_t create_transport(
    const cc_config_mcp_server_t *server_config,
    cc_mcp_transport_t *out_transport,
    void *user_data
)
{
    (void)user_data;
    memset(out_transport, 0, sizeof(*out_transport));
    const char *transport = server_config->transport ? server_config->transport : "stdio";
    if (strcmp(transport, "stdio") == 0) {
#if CC_ENABLE_MCP_STDIO
        return create_stdio_transport(server_config, out_transport);
#else
        return cc_result_error(CC_ERR_PLATFORM, "MCP stdio transport is disabled in this build");
#endif
    }
    if (strcmp(transport, "http") == 0 ||
        strcmp(transport, "sse") == 0 ||
        strcmp(transport, "streamable_http") == 0) {
#if CC_ENABLE_MCP_HTTP
        return create_http_transport(server_config, out_transport);
#else
        return cc_result_error(CC_ERR_PLATFORM, "MCP HTTP transport is disabled in this build");
#endif
    }
    return cc_result_errf(CC_ERR_INVALID_ARGUMENT, "Unknown MCP transport: %s", transport);
}

cc_result_t cc_mcp_manager_create(cc_mcp_manager_t **out_manager)
{
    if (!out_manager) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null MCP manager output");
    cc_mcp_manager_t *manager = calloc(1, sizeof(*manager));
    if (!manager) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create MCP manager");
    cc_result_t rc = cc_mcp_runtime_manager_create(create_transport, NULL, &manager->core);
    if (rc.code != CC_OK) {
        free(manager);
        return rc;
    }
    *out_manager = manager;
    return cc_result_ok();
}

void cc_mcp_manager_destroy(cc_mcp_manager_t *manager)
{
    if (!manager) return;
    cc_mcp_runtime_manager_destroy(manager->core);
    free(manager);
}

cc_result_t cc_mcp_manager_load_tools(
    cc_mcp_manager_t *manager,
    const cc_config_t *config,
    cc_tool_registry_t *registry,
    cc_runtime_diagnostics_t *diagnostics
)
{
    if (!manager) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null MCP manager");
    return cc_mcp_runtime_manager_load_tools(manager->core, config, registry, diagnostics);
}
