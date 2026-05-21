/**
 * test_config_runtime_sections.c
 *
 * 测试目标：固定新的分段配置模型，避免后续把 queue/tools/plugins/skills/mcp
 * 又散落回硬编码或旧的独立配置文件中。
 */

#include "cc/util/cc_config.h"

#include <stdio.h>
#include <string.h>

static int streq(const char *a, const char *b)
{
    return a && b && strcmp(a, b) == 0;
}

static int load_invalid_config_fails(const char *json, const char *needle)
{
    const char *path = "__c_claw_invalid_config_test__.json";
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    fputs(json, f);
    fclose(f);

    cc_config_t config;
    cc_result_t rc = cc_config_load(path, &config);
    remove(path);
    int ok = rc.code == CC_ERR_INVALID_ARGUMENT &&
        rc.message &&
        strstr(rc.message, needle) != NULL;
    cc_result_free(&rc);
    cc_config_destroy(&config);
    return ok;
}

int main(void)
{
    const char *path = "__c_claw_runtime_sections_test__.json";
    FILE *f = fopen(path, "wb");
    if (!f) return 1;
    fputs(
        "{"
        "\"queue\":{\"lanes\":{\"main\":2,\"subagent\":3,\"plugin\":4,\"mcp\":5},"
        "\"perSessionConcurrency\":1,\"mode\":\"collect\",\"debounceMs\":125,"
        "\"maxPendingPerSession\":7},"
        "\"agents\":{\"defaults\":{\"model\":\"base-model\",\"workspace\":\"./ws\","
        "\"agentDir\":\".agents/default\",\"systemPromptFile\":\"system.md\","
        "\"skills\":[\"core\"]},\"list\":[{\"id\":\"reviewer\",\"model\":\"review-model\","
        "\"skills\":[\"code-review\"]}]},"
        "\"tools\":{\"enabled\":[\"file_read\",\"plugin.echo\"],\"defaultTimeoutMs\":1234,"
        "\"perTool\":{\"plugin.echo\":{\"concurrency\":2,\"timeoutMs\":4567}}},"
        "\"plugins\":{\"hotReload\":true,\"reloadDebounceMs\":88,\"entries\":{"
        "\"echo\":{\"command\":\"python3\",\"args\":[\"plugin.py\"],\"workers\":2,"
        "\"timeoutMs\":9000,\"maxInFlight\":3,\"restartOnCrash\":false,"
        "\"skills\":[\"plugins/echo/skills\"],\"tools\":[{\"name\":\"plugin.echo\","
        "\"description\":\"Echo input\",\"parameters\":{\"type\":\"object\"}}]}}},"
        "\"skills\":{\"load\":{\"watch\":true,\"watchDebounceMs\":77,"
        "\"extraDirs\":[\".agents/skills\",\"~/.cclaw/skills\"]}},"
        "\"mcp\":{\"enabled\":true,\"sessionIdleTtlMs\":60000,\"servers\":{"
        "\"fs\":{\"transport\":\"stdio\",\"command\":\"node\",\"args\":[\"server.js\"],"
        "\"cwd\":\"./mcp\",\"connectionTimeoutMs\":3210}}},"
        "\"memory\":{\"active\":{\"enabled\":true,\"writeSummary\":true,"
        "\"maxValueChars\":256,\"category\":\"turn_summary\"}}"
        "}", f);
    fclose(f);

    cc_config_t config;
    cc_result_t rc = cc_config_load(path, &config);
    remove(path);
    if (rc.code != CC_OK) {
        cc_result_free(&rc);
        return 1;
    }
    cc_result_free(&rc);

    int ok = 1;
    ok = ok && config.queue.main_concurrency == 2;
    ok = ok && config.queue.subagent_concurrency == 3;
    ok = ok && config.queue.plugin_concurrency == 4;
    ok = ok && config.queue.mcp_concurrency == 5;
    ok = ok && streq(config.queue.mode, "collect");
    ok = ok && config.queue.debounce_ms == 125;
    ok = ok && config.queue.max_pending_per_session == 7;

    ok = ok && streq(config.agents.defaults.model, "base-model");
    ok = ok && streq(config.agents.defaults.agent_dir, ".agents/default");
    ok = ok && config.agents.defaults.skills.count == 1;
    ok = ok && config.agents.profile_count == 1;
    ok = ok && streq(config.agents.profiles[0].id, "reviewer");

    ok = ok && config.tools.enabled.count == 2;
    ok = ok && config.enabled_tools_count == 2;
    ok = ok && config.tools.default_timeout_ms == 1234;
    ok = ok && config.tools.policy_count == 1;
    ok = ok && streq(config.tools.policies[0].name, "plugin.echo");
    ok = ok && config.tools.policies[0].concurrency == 2;
    ok = ok && config.tools.policies[0].timeout_ms == 4567;

    ok = ok && config.plugins.hot_reload == 1;
    ok = ok && config.plugins.reload_debounce_ms == 88;
    ok = ok && config.plugins.entry_count == 1;
    ok = ok && streq(config.plugins.entries[0].id, "echo");
    ok = ok && config.plugins.entries[0].workers == 2;
    ok = ok && config.plugins.entries[0].restart_on_crash == 0;
    ok = ok && config.plugins.entries[0].tool_count == 1;
    ok = ok && streq(config.plugins.entries[0].tools[0].name, "plugin.echo");

    ok = ok && config.skills.watch == 1;
    ok = ok && config.skills.watch_debounce_ms == 77;
    ok = ok && config.skills.extra_dirs.count == 2;

    ok = ok && config.mcp.enabled == 1;
    ok = ok && config.mcp.session_idle_ttl_ms == 60000;
    ok = ok && config.mcp.server_count == 1;
    ok = ok && streq(config.mcp.servers[0].name, "fs");
    ok = ok && streq(config.mcp.servers[0].transport, "stdio");
    ok = ok && config.mcp.servers[0].connection_timeout_ms == 3210;
    ok = ok && config.active_memory_enabled == 1;
    ok = ok && config.active_memory_write_summary == 1;
    ok = ok && config.active_memory_max_value_chars == 256;
    ok = ok && streq(config.active_memory_category, "turn_summary");

    cc_config_destroy(&config);
    ok = ok && load_invalid_config_fails(
        "{\"mcp\":{\"enabled\":true,\"servers\":{\"bad\":{\"transport\":\"streamable_http\"}}}}",
        "requires url");
    ok = ok && load_invalid_config_fails(
        "{\"mcp\":{\"enabled\":true,\"servers\":{\"bad\":{\"transport\":\"websocket\",\"url\":\"http://x\"}}}}",
        "unknown transport");
    ok = ok && load_invalid_config_fails(
        "{\"plugins\":{\"entries\":{\"bad\":{\"enabled\":true,\"workers\":0,\"command\":\"python3\"}}}}",
        "workers");
    return ok ? 0 : 1;
}
