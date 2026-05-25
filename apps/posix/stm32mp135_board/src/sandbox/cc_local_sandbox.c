/**
 * 学习导读：apps/posix/cli/src/sandbox/cc_local_sandbox.c
 *
 * 所属层次：POSIX CLI 应用层。
 * 阅读重点：这里组装桌面 CLI、工具、插件和 sandbox，阅读时重点看 main 到 runtime builder 的组合流程。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * @file cc_local_sandbox.c
 * @brief 本地沙箱适配器——在宿主机环境中直接执行命令
 *
 * 通过对 cc_process（跨平台进程管理）的封装，在本地环境中执行沙箱命令。
 * 与 Docker 沙箱不同，本地沙箱没有资源隔离（CPU/内存限制）和网络隔离，
 * 但执行开销更低，适用于信任环境和开发调试场景。
 *
 * 实现 cc_sandbox_vtable 中的 2 个虚函数：
 *   run / destroy
 *
 * 安全注意：
 *   - 本地沙箱在宿主机中执行，命令可以访问所有文件和系统资源
 *   - 不应用于执行不可信的用户代码，应配合策略引擎进行前置审批
 *   - 超时机制由 cc_process 层的 timeout_ms 保证
 */

#include "cc/ports/cc_sandbox.h"
#include "cc/ports/cc_process.h"
#include <stdlib.h>
#include <string.h>

/**
 * @brief 本地沙箱的私有数据结构
 *
 * @field timeout_ms 命令执行的默认超时时间（毫秒），在创建时设定
 */
typedef struct {
    int timeout_ms;
} cc_local_sandbox_t;

/**
 * @brief vtable 函数：在本地环境中执行沙箱命令
 *
 * 将 cc_sandbox_command_t 转换为 cc_process_options_t，然后调用 cc_process_run()
 * 同步执行命令并捕获输出。
 *
 * 执行流程：
 *   1. 清零 out_result 结构体
 *   2. 构造 cc_process_options_t：
 *      - command：直接使用命令字符串
 *      - working_dir：继承请求的工作目录
 *      - env：继承请求的环境变量
 *      - timeout_ms：优先使用命令级超时，未设置则使用沙箱默认超时
 *      - capture_stdout / capture_stderr：均启用，捕获所有输出
 *   3. 调用 cc_process_run 同步执行
 *   4. 将进程结果映射到 cc_sandbox_result_t
 *
 * 安全注意：
 *   - 命令在宿主机进程空间中执行，无任何资源限制
 *   - 不适用于执行不可信代码
 *
 * @param self       沙箱实例指针
 * @param command    待执行的沙箱命令（command、working_dir、env、timeout_ms 等参数）
 * @param out_result 输出参数，执行结果（exit_code、stdout、stderr、timed_out）
 * @return cc_result_t 成功返回 OK，进程启动失败返回 cc_process_run 的错误码
 */
static cc_result_t local_sandbox_run(
    void *self,
    const cc_sandbox_command_t *command,
    cc_sandbox_result_t *out_result
)
{
    cc_local_sandbox_t *sandbox = (cc_local_sandbox_t *)self;

    memset(out_result, 0, sizeof(cc_sandbox_result_t));

    cc_process_options_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.command = command->command;
    opts.args = NULL;
    opts.working_dir = command->working_dir;
    opts.env = command->env;
    opts.timeout_ms = command->timeout_ms > 0 ? command->timeout_ms : sandbox->timeout_ms;
    opts.capture_stdout = 1;
    opts.capture_stderr = 1;

    cc_process_result_t prcc_result;
    cc_result_t rc = cc_process_run(&opts, &prcc_result);
    if (rc.code != CC_OK) return rc;

    out_result->exit_code = prcc_result.exit_code;
    out_result->stdout_text = prcc_result.stdout_text;
    out_result->stderr_text = prcc_result.stderr_text;
    out_result->timed_out = prcc_result.timed_out;

    return cc_result_ok();
}

/**
 * @brief vtable 函数：销毁本地沙箱实例
 *
 * 本地沙箱无特殊资源需要释放（仅 free 结构体自身）。
 *
 * @param self 沙箱实例指针
 */
static void local_sandbox_destroy(void *self)
{
    free(self);
}

/**
 * @brief 本地沙箱的虚函数表
 *
 * 绑定 run → local_sandbox_run, destroy → local_sandbox_destroy
 */
static cc_sandbox_vtable_t local_vtable = {
    local_sandbox_run,
    local_sandbox_destroy
};

/**
 * @brief 创建本地沙箱实例（公共工厂函数）
 *
 * 执行流程：
 *   1. 分配 cc_local_sandbox_t 结构体
 *   2. 设置默认超时时间 timeout_ms
 *   3. 填充 out_sandbox 的 self 和 vtable
 *
 * @param timeout_ms   命令执行的默认超时时间（毫秒），命令级超时可覆盖
 * @param out_sandbox  输出参数，填充创建好的沙箱实例
 * @return cc_result_t 成功返回 OK，calloc 失败返回 OUT_OF_MEMORY
 */
cc_result_t cc_local_sandbox_create(int timeout_ms, cc_sandbox_t *out_sandbox)
{
    cc_local_sandbox_t *self = calloc(1, sizeof(cc_local_sandbox_t));
    if (!self) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create local sandbox");

    self->timeout_ms = timeout_ms;
    out_sandbox->self = self;
    out_sandbox->vtable = &local_vtable;
    return cc_result_ok();
}

/**
 * @brief 释放沙箱执行结果中动态分配的内存
 *
 * 释放 stdout_text 和 stderr_text 字符串，然后清零整个结构体。
 * 调用者在获取 sandbox result 后必须调用此函数，否则造成内存泄漏。
 *
 * @param result 指向沙箱执行结果的指针，可为 NULL（此时函数直接返回）
 */
void cc_sandbox_result_free(cc_sandbox_result_t *result)
{
    if (!result) return;
    free(result->stdout_text);
    free(result->stderr_text);
    memset(result, 0, sizeof(cc_sandbox_result_t));
}