/**
 * 学习导读：apps/windows/cli/src/sandbox/cc_docker_sandbox.c
 *
 * 所属层次：Windows CLI 应用层。
 * 阅读重点：这里镜像桌面 CLI 能力但使用 Windows 平台实现，阅读时重点比较与 POSIX 版本的差异。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * @file cc_docker_sandbox.c
 * @brief Docker 容器沙箱适配器——通过 docker run 隔离执行命令
 *
 * 将用户命令包装在 Docker 容器中执行，提供以下安全隔离：
 *   - 网络隔离（--network=none）：容器无法访问任何网络
 *   - 内存限制（--memory=256m）：限制最大内存使用 256MB
 *   - CPU 限制（--cpus=0.5）：限制最多使用半个 CPU 核心
 *   - 自动清理（--rm）：容器退出后自动删除，不留残留
 *
 * 实现 cc_sandbox_vtable 中的 2 个虚函数：
 *   run / destroy
 *
 * 与本地沙箱的对比：
 *   - 本地沙箱：零开销但无隔离，适合信任环境
 *   - Docker 沙箱：有启动开销但提供完整隔离，适合不可信代码
 *
 * 安全注意：
 *   - Docker 命令字符串通过 snprintf 构建，确保缓冲区不溢出
 *   - 用户命令以单引号包裹在 sh -c 中执行，但仍需注意 shell 注入风险
 *   - 依赖宿主机安装 Docker 并配置正确的用户权限
 */

#include "cc/ports/cc_sandbox.h"
#include "cc/ports/cc_process.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief Docker 沙箱的私有数据结构
 *
 * @field timeout_ms 命令执行的默认超时时间（毫秒），在创建时设定
 */
typedef struct {
    int timeout_ms;
} cc_docker_sandbox_t;

/**
 * @brief vtable 函数：在 Docker 容器中隔离执行沙箱命令
 *
 * 构建 Docker argv 并交由 cc_process 执行。Docker 参数说明：
 *   docker run --rm --network=none --memory=256m --cpus=0.5
 *   -w <working_dir> ubuntu:22.04 /bin/sh -c '<user_command>'
 *
 * 执行流程：
 *   1. 清零 out_result 结构体
 *   2. 生成 -v 参数需要的 volume 字符串
 *   3. 通过 argv 数组传递 docker run 参数，避免宿主 shell 二次解析
 *   4. 将超时和捕获选项传递给 cc_process_run
 *   5. 将进程结果映射到 cc_sandbox_result_t
 *   6. 释放临时分配的 volume 字符串
 *
 * 安全注意：
 *   - Docker CLI 参数以 argv 形式传入，不经过宿主 shell 拼接
 *   - 用户命令仍由容器内 /bin/sh -c 解释，应按目标 shell 语义审查
 *   - --network=none 确保容器无网络访问，防止数据外泄
 *   - --memory 和 --cpus 限制防止资源耗尽攻击
 *
 * @param self       沙箱实例指针
 * @param command    待执行的沙箱命令
 * @param out_result 输出参数，执行结果（exit_code、stdout、stderr、timed_out）
 * @return cc_result_t 成功返回 OK，内存分配或进程启动失败返回对应错误码
 */
static cc_result_t docker_sandbox_run(
    void *self,
    const cc_sandbox_command_t *command,
    cc_sandbox_result_t *out_result
)
{
    cc_docker_sandbox_t *sandbox = (cc_docker_sandbox_t *)self;

    memset(out_result, 0, sizeof(cc_sandbox_result_t));
    if (!command || !command->command) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Docker sandbox command is required");
    }

    const char *workdir = command->working_dir ? command->working_dir : "/tmp";
    size_t volume_len = strlen(workdir) * 2 + 5;
    char *volume = malloc(volume_len);
    if (!volume) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to build docker volume");
    snprintf(volume, volume_len, "%s:%s:rw", workdir, workdir);

    char *argv[] = {
        "docker",
        "run",
        "--rm",
        "--network=none",
        "--memory=256m",
        "--cpus=0.5",
        "-v",
        volume,
        "-w",
        (char *)workdir,
        "ubuntu:22.04",
        "/bin/sh",
        "-c",
        command->command,
        NULL
    };

    cc_process_options_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.command = "docker";
    opts.args = argv;
    opts.timeout_ms = command->timeout_ms > 0 ? command->timeout_ms : sandbox->timeout_ms;
    opts.capture_stdout = 1;
    opts.capture_stderr = 1;

    cc_process_result_t prcc_result;
    cc_result_t rc = cc_process_run(&opts, &prcc_result);
    free(volume);

    if (rc.code != CC_OK) return rc;

    out_result->exit_code = prcc_result.exit_code;
    out_result->stdout_text = prcc_result.stdout_text;
    out_result->stderr_text = prcc_result.stderr_text;
    out_result->timed_out = prcc_result.timed_out;

    return cc_result_ok();
}

/**
 * @brief vtable 函数：销毁 Docker 沙箱实例
 *
 * Docker 沙箱的容器由 --rm 参数自动清理，因此 destroy 只需释放结构体内存。
 *
 * @param self 沙箱实例指针
 */
static void docker_sandbox_destroy(void *self)
{
    free(self);
}

/**
 * @brief Docker 沙箱的虚函数表
 *
 * 绑定 run → docker_sandbox_run, destroy → docker_sandbox_destroy
 */
static cc_sandbox_vtable_t docker_vtable = {
    docker_sandbox_run,
    docker_sandbox_destroy
};

/**
 * @brief 创建 Docker 沙箱实例（公共工厂函数）
 *
 * 执行流程：
 *   1. 分配 cc_docker_sandbox_t 结构体
 *   2. 设置默认超时时间 timeout_ms
 *   3. 填充 out_sandbox 的 self 和 vtable
 *
 * 依赖条件：宿主机需安装 Docker 并允许当前用户执行 docker 命令。
 *
 * @param timeout_ms   命令执行的默认超时时间（毫秒），命令级超时可覆盖
 * @param out_sandbox  输出参数，填充创建好的沙箱实例
 * @return cc_result_t 成功返回 OK，calloc 失败返回 OUT_OF_MEMORY
 */
cc_result_t cc_docker_sandbox_create(int timeout_ms, cc_sandbox_t *out_sandbox)
{
    cc_docker_sandbox_t *self = calloc(1, sizeof(cc_docker_sandbox_t));
    if (!self) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create docker sandbox");

    self->timeout_ms = timeout_ms;
    out_sandbox->self = self;
    out_sandbox->vtable = &docker_vtable;
    return cc_result_ok();
}
