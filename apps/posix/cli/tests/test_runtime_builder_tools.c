/**
 * 学习导读：apps/posix/cli/tests/test_runtime_builder_tools.c
 *
 * 所属层次：POSIX CLI 应用层。
 * 阅读重点：这里组装桌面 CLI、工具、插件和 sandbox，阅读时重点看 main 到 runtime builder 的组合流程。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

#include "cc/app/cc_runtime_builder.h"
#include "cc/app/cc_app_features.h"
#include "cc/app/cc_agent_runtime.h"
#include "cc/ports/cc_tool_registry.h"

#include <stdlib.h>
#include <string.h>

/**
 * main — 执行本文件的 Given/When/Then 回归测试，失败时以非零退出码暴露问题。
 *
 * 位置：工具适配层。注释重点说明当前函数的输入输出、资源边界和错误传播。
 *
 * @return 0 通常表示成功完成，非 0 表示失败或应向进程层传播的状态。
 */
int main(void)
{
#if !(CC_LLM_OPENAI || CC_LLM_OLLAMA)
    return 0;
#else
    char *enabled[] = { "read" };
    cc_config_t config;
    memset(&config, 0, sizeof(config));
    config.provider = "ollama";
    config.model = "fake";
    config.base_url = "http://localhost:11434";
    config.storage_type = "memory";
    config.workspace_path = "/tmp/c_claw_builder_workspace";
    config.sandbox_type = "none";
    config.sandbox_timeout_ms = 1000;
    config.max_steps = 1;
    config.memory_backend = "noop";
    config.system_prompt = "system";
    config.enabled_tools = enabled;
    config.enabled_tools_count = 1;

    cc_runtime_builder_t *builder = NULL;
    cc_result_t rc = cc_runtime_builder_create(
        &config, cc_app_default_features(), &builder);
    if (rc.code != CC_OK) {
        cc_result_free(&rc);
        return 1;
    }

    cc_tool_registry_t *registry =
        cc_agent_runtime_tool_registry(cc_runtime_builder_runtime(builder));
    size_t count = cc_tool_registry_count(registry);
    char **names = NULL;
    size_t name_count = 0;
    cc_tool_registry_list_names(registry, &names, &name_count);

    int failed = 0;
    int found_read = 0;
    for (size_t i = 0; i < name_count; i++) {
        if (names[i] && strcmp(names[i], "file_read") == 0) found_read = 1;
    }
    if (count < 1 || name_count < 1 || !found_read) failed = 1;

    for (size_t i = 0; i < name_count; i++) free(names[i]);
    free(names);
    cc_runtime_builder_destroy(builder);
    return failed ? 1 : 0;
#endif
}
