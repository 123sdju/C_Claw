/**
 * 学习导读：cclaw/tests/core/test_config_missing_defaults.c
 *
 * 所属层次：测试层。
 * 阅读重点：这里用小型 Given/When/Then 场景固定行为，阅读时重点看每个断言防止哪类回归。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

#include "cc/util/cc_config.h"
#include "cc/ports/cc_platform.h"

#include <stdio.h>
#include <string.h>

static int double_eq(double a, double b)
{
    double d = a - b;
    if (d < 0.0) d = -d;
    return d < 0.000001;
}

/**
 * main — 执行本文件的 Given/When/Then 回归测试，失败时返回非零退出码。
 *
 * @return 0 表示断言全部通过，非 0 表示行为回归。
 */
int main(void)
{
    cc_config_t config;
    cc_result_t rc = cc_config_load("__c_claw_missing_config__.json", &config);

    if (rc.code == CC_OK) {
        cc_result_free(&rc);
        cc_config_destroy(&config);
        return 1;
    }
    cc_result_free(&rc);

    if (!config.provider || !config.model || !config.storage_type ||
        !config.data_dir || !config.storage_path || !config.workspace_path ||
        !config.sandbox_type || !config.memory_backend ||
        !config.memory_path) {
        cc_config_destroy(&config);
        return 1;
    }

    if (strlen(config.provider) == 0 || strlen(config.storage_type) == 0) {
        cc_config_destroy(&config);
        return 1;
    }

    if (config.stream_mode != 0 || config.debug_mode != 0) {
        cc_config_destroy(&config);
        return 1;
    }
    if (config.max_tokens != 4096 || !double_eq(config.temperature, 0.7) ||
        config.summary_max_tokens != 1024 ||
        !double_eq(config.summary_temperature, 0.3)) {
        cc_config_destroy(&config);
        return 1;
    }

#if CC_PLATFORM != CC_PLATFORM_ESP32
    const char root_workspace_path[] = "." "/" "workspace";
    const char root_data_dir[] = "." "/" "data";

    if (strcmp(config.workspace_path, root_workspace_path) == 0 ||
        strcmp(config.data_dir, root_data_dir) == 0 ||
        strstr(config.data_dir, "/runtime/") == NULL) {
        cc_config_destroy(&config);
        return 1;
    }
#endif

    cc_config_destroy(&config);

    const char *path = "__c_claw_config_ui_test__.json";
    FILE *f = fopen(path, "wb");
    if (!f) return 1;
    fputs("{\"model\":{\"stream_mode\":true,\"max_tokens\":2048,\"temperature\":0.2},"
          "\"system\":{\"summary_max_tokens\":512,\"summary_temperature\":0.1},"
          "\"cli\":{\"debug_mode\":true}}", f);
    fclose(f);

    memset(&config, 0, sizeof(config));
    rc = cc_config_load(path, &config);
    remove(path);
    if (rc.code != CC_OK) {
        cc_result_free(&rc);
        cc_config_destroy(&config);
        return 1;
    }
    cc_result_free(&rc);

    if (config.stream_mode != 1 || config.debug_mode != 1) {
        cc_config_destroy(&config);
        return 1;
    }
    if (config.max_tokens != 2048 || !double_eq(config.temperature, 0.2) ||
        config.summary_max_tokens != 512 ||
        !double_eq(config.summary_temperature, 0.1)) {
        cc_config_destroy(&config);
        return 1;
    }
    cc_config_destroy(&config);

    f = fopen(path, "wb");
    if (!f) return 1;
    fputs("{\"model\":{\"thinking_mode\":1,\"stream_mode\":0},\"cli\":{\"debug_mode\":false}}", f);
    fclose(f);

    memset(&config, 0, sizeof(config));
    rc = cc_config_load(path, &config);
    remove(path);
    if (rc.code != CC_OK) {
        cc_result_free(&rc);
        cc_config_destroy(&config);
        return 1;
    }
    cc_result_free(&rc);

    if (config.thinking_mode != 1 || config.stream_mode != 1) {
        cc_config_destroy(&config);
        return 1;
    }
    cc_config_destroy(&config);
    return 0;
}
