/**
 * 学习导读：apps/windows/cli/src/main.c
 *
 * 所属层次：Windows CLI 应用层。
 * 阅读重点：这里镜像桌面 CLI 能力但使用 Windows 平台实现，阅读时重点比较与 POSIX 版本的差异。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

#include "cc/app/cc_runtime_builder.h"
#include "cc/app/cc_app_features.h"
#include "cc/app/cc_agent_runtime.h"
#include "cc/util/cc_config.h"
#include "cc/ports/cc_logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if CC_GATEWAY_CLI
extern int cc_cli_gateway_run(
    int argc,
    char **argv,
    cc_agent_runtime_t *runtime,
    cc_config_t *config
);
#endif

#ifndef CC_DEFAULT_CONFIG_PATH
#define CC_DEFAULT_CONFIG_PATH ""
#endif

/* 学习注释：main 是 CLI 进程入口。
 * 阅读时按“解析参数 → 加载配置 → 构建 runtime → 交给 gateway → cleanup”的顺序看资源生命周期。 */
int main(int argc, char **argv)
{
    int exit_code = 1;
    cc_config_t config;
    cc_runtime_builder_t *builder = NULL;
    memset(&config, 0, sizeof(config));

    const char *config_path = CC_DEFAULT_CONFIG_PATH;
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--config") == 0) {
            config_path = argv[i + 1];
            break;
        }
    }

    cc_result_t rc = cc_config_load(config_path, &config);
    if (rc.code != CC_OK) {
        if (rc.code == CC_ERR_OUT_OF_MEMORY) {
            fprintf(stderr, "Failed to load config defaults: %s\n",
                rc.message ? rc.message : "out of memory");
            cc_result_free(&rc);
            goto cleanup;
        }
        fprintf(stderr, "Warning: %s; using defaults where needed\n",
            rc.message ? rc.message : "could not load config file");
        cc_result_free(&rc);
    }

    rc = cc_runtime_builder_create(&config, cc_app_default_features(), &builder);
    if (rc.code != CC_OK) {
        fprintf(stderr, "Failed to initialize c-claw runtime: %s\n",
            rc.message ? rc.message : "unknown");
        cc_result_free(&rc);
        goto cleanup;
    }

#if CC_GATEWAY_CLI
    exit_code = cc_cli_gateway_run(
        argc,
        argv,
        cc_runtime_builder_runtime(builder),
        &config
    );
#else
    (void)argc;
    (void)argv;
    fprintf(stderr, "CLI gateway disabled in this build\n");
#endif

cleanup:
    cc_runtime_builder_destroy(builder);
    cc_config_destroy(&config);
    return exit_code;
}
