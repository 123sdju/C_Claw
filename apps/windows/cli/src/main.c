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
/**
 * cc_cli_gateway_run — 启动命令行 gateway，解析 CLI 参数并把交互或 ask 请求交给已构建的 runtime。
 *
 * 这里使用 extern 是为了让 main.c 只依赖 gateway 的入口签名，不暴露 Windows
 * 控制台交互细节。gateway 只借用 runtime 和 config，不负责销毁它们。
 *
 * @param argc 命令行参数数量。
 * @param argv 命令行参数数组；gateway 只借用，不保存。
 * @param builder 已由 runtime_builder 创建的组合根借用指针。
 * @param config 已加载的配置；gateway 可读取 CLI 行为参数。
 * @param config_path 配置文件路径；桌面 gateway 可用它实现热重载。
 * @return 进程退出码，0 表示 CLI 正常完成，非 0 表示命令处理失败。
 */
extern int cc_cli_gateway_run(
    int argc,
    char **argv,
    cc_runtime_builder_t *builder,
    cc_config_t *config,
    const char *config_path
);
#endif

#ifndef CC_DEFAULT_CONFIG_PATH
#define CC_DEFAULT_CONFIG_PATH ""
#endif

/**
 * main — Windows CLI 可执行文件入口。
 *
 * 启动流程按“解析 --config 覆盖项 → 加载配置 → 通过 feature set 构建 runtime
 * → 交给 gateway → cleanup”展开。资源释放集中在 cleanup 标签，确保初始化
 * 中途失败和正常退出走同一套释放路径。
 *
 * @param argc 命令行参数数量。
 * @param argv 命令行参数数组；本函数只借用，不保存。
 * @return 进程退出码：0 表示成功，非 0 表示初始化或 gateway 执行失败。
 */
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
        builder,
        &config,
        config_path
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
