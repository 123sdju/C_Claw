#include "cc/mcp/cc_mcp_manager.h"
#include "cc/ports/cc_tool_registry.h"
#include "cc/util/cc_config.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int contains(const char *text, const char *needle)
{
    return text && needle && strstr(text, needle) != NULL;
}

static int extract_pid(const char *text, char *out, size_t out_size)
{
    const char *pos = text ? strstr(text, "pid:") : NULL;
    if (!pos || out_size == 0) return 0;
    pos += 4;
    size_t n = 0;
    while (pos[n] >= '0' && pos[n] <= '9' && n + 1 < out_size) n++;
    if (n == 0) return 0;
    memcpy(out, pos, n);
    out[n] = '\0';
    return 1;
}

int main(void)
{
    cc_config_t config;
    if (cc_config_load_default(&config).code != CC_OK) return 1;

    config.mcp.enabled = 1;
    config.mcp.session_idle_ttl_ms = 1000;
    config.mcp.server_count = 1;
    config.mcp.servers = calloc(1, sizeof(cc_config_mcp_server_t));
    if (!config.mcp.servers) return 1;
    config.mcp.servers[0].name = strdup("mock");
    config.mcp.servers[0].transport = strdup("stdio");
    config.mcp.servers[0].command = strdup("python3");
    config.mcp.servers[0].arg_count = 1;
    config.mcp.servers[0].args = calloc(1, sizeof(char *));
    config.mcp.servers[0].args[0] = strdup("apps/posix/cli/tests/mock_mcp_server.py");

    cc_tool_registry_t *registry = NULL;
    cc_mcp_manager_t *manager = NULL;
    if (cc_tool_registry_create(&registry).code != CC_OK) return 1;
    if (cc_mcp_manager_create(&manager).code != CC_OK) return 1;
    cc_runtime_diagnostics_t diagnostics;
    cc_runtime_diagnostics_reset(&diagnostics);
    if (cc_mcp_manager_load_tools(manager, &config, registry, &diagnostics).code != CC_OK) return 1;

    cc_tool_t tool;
    if (cc_tool_registry_find(registry, "mcp.mock.echo", &tool).code != CC_OK) return 1;
    cc_tool_result_t result;
    memset(&result, 0, sizeof(result));
    cc_tool_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    if (tool.vtable->call(tool.self, "{\"text\":\"hello-mcp\"}", &ctx, &result).code != CC_OK) return 1;
    char first_pid[32];
    int ok = result.ok && contains(result.content, "hello-mcp") &&
        extract_pid(result.content, first_pid, sizeof(first_pid));

    free(result.content);
    free(result.error);
    free(result.metadata_json);
    sleep(2);

    memset(&result, 0, sizeof(result));
    if (tool.vtable->call(tool.self, "{\"text\":\"hello-again\"}", &ctx, &result).code != CC_OK) return 1;
    char second_pid[32];
    ok = ok && result.ok && contains(result.content, "hello-again") &&
        extract_pid(result.content, second_pid, sizeof(second_pid)) &&
        strcmp(first_pid, second_pid) != 0;

    free(result.content);
    free(result.error);
    free(result.metadata_json);
    cc_tool_registry_destroy(registry);
    cc_mcp_manager_destroy(manager);
    cc_config_destroy(&config);
    return ok ? 0 : 1;
}
