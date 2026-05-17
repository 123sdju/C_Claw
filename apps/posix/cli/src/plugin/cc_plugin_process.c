/**
 * 学习导读：apps/posix/cli/src/plugin/cc_plugin_process.c
 *
 * 所属层次：POSIX CLI 应用层。
 * 阅读重点：这里组装桌面 CLI、工具、插件和 sandbox，阅读时重点看 main 到 runtime builder 的组合流程。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/******************************************************************************
 * cc_plugin_process.c — 插件进程管理模块（平台无关）
 *
 * 本模块管理外部插件子进程的完整生命周期。通过 cc_process_pipe 实现
 * 父进程与插件进程之间的 JSON-RPC 双向通信。
 *
 * ── 在插件通信栈中的位置 ──
 *
 *   cc_plugin_tool.c (适配层)
 *      调用 send() 发送方法调用, receive() 获取结果
 *      不关心底层是 pipe 还是 socket
 *      ▼
 *   本模块 (传输层)
 *      调用 cc_process_pipe_spawn/write/read_line/stop → platform 层
 *      ▼
 *   platform/posix/cc_posix_process.c  或  platform/windows/cc_windows_process.c
 *      fork+exec+pipe                  CreateProcess+CreatePipe
 *
 * ── 缓冲策略 ──
 *
 *   写端：_IONBF 无缓冲模式（platform 层保证）
 *     每次 fprintf + fflush，确保子进程立即收到完整的 JSON 行
 *
 *   读端：默认缓冲 + getline（platform 层实现）
 *     getline 按行读取 JSON-RPC 响应
 *****************************************************************************/

#include "cc/plugin/cc_plugin_process.h"
#include "cc/ports/cc_thread.h"
#include "cc/ports/cc_process.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct cc_plugin_process {
    cc_process_pipe_t *pipe;
    cc_mutex_t request_mutex;
} cc_plugin_process_t;

/* 学习注释：cc_plugin_process_start 是对外可见或跨模块调用的入口。
 * 阅读时重点确认参数校验、所有权转移、错误码和清理路径是否成对出现。 */
cc_result_t cc_plugin_process_start(
    const char *command,
    char *const argv[],
    cc_plugin_process_t **out_process
)
{
    cc_plugin_process_t *p = calloc(1, sizeof(cc_plugin_process_t));
    if (!p)
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate plugin process");

    cc_result_t rc = cc_mutex_create(&p->request_mutex);
    if (rc.code != CC_OK) {
        free(p);
        return rc;
    }

    rc = cc_process_pipe_spawn(command, argv, &p->pipe);
    if (rc.code != CC_OK) {
        cc_mutex_destroy(p->request_mutex);
        free(p);
        return rc;
    }

    *out_process = p;
    return cc_result_ok();
}

/* 学习注释：cc_plugin_process_call 是对外可见或跨模块调用的入口。
 * 阅读时重点确认参数校验、所有权转移、错误码和清理路径是否成对出现。 */
cc_result_t cc_plugin_process_call(
    cc_plugin_process_t *process,
    const char *request_json,
    char **out_json
)
{
    if (!process || !request_json || !out_json)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid plugin call argument");

    cc_mutex_lock(process->request_mutex);
    cc_result_t rc = cc_plugin_process_send(process, request_json);
    if (rc.code == CC_OK)
        rc = cc_plugin_process_receive(process, out_json);
    cc_mutex_unlock(process->request_mutex);
    return rc;
}

/* 学习注释：cc_plugin_process_send 是对外可见或跨模块调用的入口。
 * 阅读时重点确认参数校验、所有权转移、错误码和清理路径是否成对出现。 */
cc_result_t cc_plugin_process_send(
    cc_plugin_process_t *process,
    const char *json_str
)
{
    if (!process || !process->pipe)
        return cc_result_error(CC_ERR_PLATFORM, "Plugin process not running");
    return cc_process_pipe_write(process->pipe, json_str);
}

/* 学习注释：cc_plugin_process_receive 是对外可见或跨模块调用的入口。
 * 阅读时重点确认参数校验、所有权转移、错误码和清理路径是否成对出现。 */
cc_result_t cc_plugin_process_receive(
    cc_plugin_process_t *process,
    char **out_json
)
{
    if (!process || !process->pipe)
        return cc_result_error(CC_ERR_PLATFORM, "Plugin process not running");
    return cc_process_pipe_read_line(process->pipe, out_json);
}

/* 学习注释：cc_plugin_process_stop 是对外可见或跨模块调用的入口。
 * 阅读时重点确认参数校验、所有权转移、错误码和清理路径是否成对出现。 */
void cc_plugin_process_stop(cc_plugin_process_t *process)
{
    if (!process) return;
    cc_mutex_lock(process->request_mutex);
    if (process->pipe) {
        cc_process_pipe_stop(process->pipe);
        process->pipe = NULL;
    }
    cc_mutex_unlock(process->request_mutex);
}

/* 学习注释：cc_plugin_process_destroy 是对外可见或跨模块调用的入口。
 * 阅读时重点确认参数校验、所有权转移、错误码和清理路径是否成对出现。 */
void cc_plugin_process_destroy(cc_plugin_process_t *process)
{
    if (!process) return;
    cc_plugin_process_stop(process);
    cc_mutex_destroy(process->request_mutex);
    if (process->pipe)
        cc_process_pipe_destroy(process->pipe);
    free(process);
}