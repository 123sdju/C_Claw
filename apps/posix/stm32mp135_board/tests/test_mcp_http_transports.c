#include "cc/mcp/cc_mcp_manager.h"
#include "cc/ports/cc_process.h"
#include "cc/ports/cc_tool_registry.h"
#include "cc/util/cc_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int contains(const char *text, const char *needle)
{
    return text && needle && strstr(text, needle) != NULL;
}

static int start_server(int port, const char *mode, cc_process_pipe_t **out_pipe)
{
    char port_text[32];
    snprintf(port_text, sizeof(port_text), "%d", port);
    char *argv[] = {
        "python3",
        "apps/posix/cli/tests/mock_mcp_http_server.py",
        port_text,
        (char *)mode,
        NULL
    };
    if (cc_process_pipe_spawn("python3", argv, out_pipe).code != CC_OK) return 0;
    char *ready = NULL;
    cc_result_t rc = cc_process_pipe_read_line_timeout(*out_pipe, 5000, &ready);
    int ok = rc.code == CC_OK && ready && contains(ready, "READY");
    if (rc.code == CC_OK && ready && contains(ready, "SKIP")) ok = 2;
    free(ready);
    cc_result_free(&rc);
    return ok;
}

static int run_transport_case(
    int port,
    const char *server_mode,
    const char *transport_name,
    const char *text,
    const char *expected_extra
)
{
    cc_process_pipe_t *server = NULL;
    int started = start_server(port, server_mode, &server);
    if (started == 2) {
        fprintf(stderr, "skipping %s server: local sockets unavailable\n", server_mode);
        cc_process_pipe_destroy(server);
        return 1;
    }
    if (!started) {
        fprintf(stderr, "failed to start %s server on port %d\n", server_mode, port);
        return 0;
    }

    cc_config_t config;
    if (cc_config_load_default(&config).code != CC_OK) return 0;
    config.mcp.enabled = 1;
    config.mcp.session_idle_ttl_ms = 600000;
    config.mcp.server_count = 1;
    config.mcp.servers = calloc(1, sizeof(cc_config_mcp_server_t));
    if (!config.mcp.servers) return 0;
    char url[128];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/mcp", port);
    config.mcp.servers[0].name = strdup("mock");
    config.mcp.servers[0].transport = strdup(transport_name);
    config.mcp.servers[0].url = strdup(url);
    config.mcp.servers[0].connection_timeout_ms = 5000;

    cc_tool_registry_t *registry = NULL;
    cc_mcp_manager_t *manager = NULL;
    cc_result_t rc = cc_tool_registry_create(&registry);
    int ok = rc.code == CC_OK;
    if (!ok) fprintf(stderr, "%s registry: %s\n", transport_name, rc.message);
    cc_result_free(&rc);
    if (ok) {
        rc = cc_mcp_manager_create(&manager);
        ok = rc.code == CC_OK;
        if (!ok) fprintf(stderr, "%s manager: %s\n", transport_name, rc.message);
        cc_result_free(&rc);
    }
    if (ok) {
        cc_runtime_diagnostics_t diagnostics;
        cc_runtime_diagnostics_reset(&diagnostics);
        rc = cc_mcp_manager_load_tools(manager, &config, registry, &diagnostics);
        ok = rc.code == CC_OK;
        if (!ok) fprintf(stderr, "%s load: %s\n", transport_name, rc.message);
        cc_result_free(&rc);
    }

    cc_tool_t tool;
    if (ok) {
        rc = cc_tool_registry_find(registry, "mcp.mock.echo", &tool);
        ok = rc.code == CC_OK;
        if (!ok) fprintf(stderr, "%s find: %s\n", transport_name, rc.message);
        cc_result_free(&rc);
    }
    if (ok) {
        char args[128];
        snprintf(args, sizeof(args), "{\"text\":\"%s\"}", text);
        cc_tool_result_t result;
        memset(&result, 0, sizeof(result));
        cc_tool_context_t ctx;
        memset(&ctx, 0, sizeof(ctx));
        rc = tool.vtable->call(tool.self, args, &ctx, &result);
        ok = rc.code == CC_OK && result.ok && contains(result.content, text) &&
            (!expected_extra || contains(result.content, expected_extra));
        if (!ok) {
            fprintf(stderr, "%s call rc=%d result_ok=%d content=%s error=%s\n",
                transport_name,
                rc.code,
                result.ok,
                result.content ? result.content : "(null)",
                result.error ? result.error : "(null)");
        }
        cc_result_free(&rc);
        free(result.content);
        free(result.error);
        free(result.metadata_json);
    }

    cc_tool_registry_destroy(registry);
    cc_mcp_manager_destroy(manager);
    cc_config_destroy(&config);
    cc_process_pipe_destroy(server);
    return ok;
}

int main(void)
{
    int base_port = 21000 + ((int)getpid() % 10000);
    int ok = run_transport_case(base_port, "json", "http", "hello-json", NULL);
    ok = ok && run_transport_case(base_port + 1, "sse", "sse", "hello-sse", NULL);
    ok = ok && run_transport_case(
        base_port + 2,
        "streamable",
        "streamable_http",
        "hello-stream",
        "session:mock-session");
    return ok ? 0 : 1;
}
