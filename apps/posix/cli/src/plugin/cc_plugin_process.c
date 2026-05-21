/**
 * 学习导读：apps/posix/cli/src/plugin/cc_plugin_process.c
 *
 * 所属层次：POSIX CLI 应用层。
 * 阅读重点：这里封装外部 plugin worker 的进程和 pipe 生命周期，重点看单 worker
 *           串行、timeout/cancel 传播和崩溃后复位。
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

/**
 * cc_plugin_process — 单个 plugin worker。
 *
 * request_mutex 保证一个 worker 的 stdin/stdout 协议严格串行。多 worker 并发由
 * cc_plugin_tool 的 round-robin 和 core tool pool 控制；这里不尝试在同一 pipe 上
 * 复用多个 in-flight JSON-RPC 请求。
 */
typedef struct cc_plugin_process {
    cc_process_pipe_t *pipe;
    cc_mutex_t request_mutex;
    char *command;
    char **argv;
    int restart_on_crash;
    int timeout_ms;
} cc_plugin_process_t;

static void free_argv_copy(char **argv)
{
    if (!argv) return;
    for (size_t i = 0; argv[i]; i++) free(argv[i]);
    free(argv);
}

static char **copy_argv(char *const argv[])
{
    if (!argv) return NULL;
    size_t count = 0;
    while (argv[count]) count++;
    char **copy = calloc(count + 1, sizeof(char *));
    if (!copy) return NULL;
    for (size_t i = 0; i < count; i++) {
        copy[i] = strdup(argv[i] ? argv[i] : "");
        if (!copy[i]) {
            free_argv_copy(copy);
            return NULL;
        }
    }
    return copy;
}

static cc_result_t spawn_locked(cc_plugin_process_t *process)
{
    if (!process || !process->command || !process->argv) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid plugin process spawn state");
    }
    return cc_process_pipe_spawn(process->command, process->argv, &process->pipe);
}

static cc_result_t restart_locked(cc_plugin_process_t *process)
{
    if (!process || !process->restart_on_crash) {
        return cc_result_error(CC_ERR_PLATFORM, "Plugin process restart is disabled");
    }
    if (process->pipe) {
        cc_process_pipe_destroy(process->pipe);
        process->pipe = NULL;
    }
    return spawn_locked(process);
}

/**
 * cc_plugin_process_start — 启动一个 JSON-RPC 插件子进程。
 *
 * command/argv 会被深拷贝到 process 内部，后续 restartOnCrash 可以使用同一份启动
 * 参数重建 worker。成功后 out_process 由 plugin manager 拥有并最终 destroy。
 */
cc_result_t cc_plugin_process_start(
    const char *command,
    char *const argv[],
    cc_plugin_process_t **out_process
)
{
    return cc_plugin_process_start_with_options(command, argv, 0, 30000, out_process);
}

cc_result_t cc_plugin_process_start_with_options(
    const char *command,
    char *const argv[],
    int restart_on_crash,
    int timeout_ms,
    cc_plugin_process_t **out_process
)
{
    cc_plugin_process_t *p = calloc(1, sizeof(cc_plugin_process_t));
    if (!p)
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate plugin process");
    p->command = strdup(command ? command : "");
    p->argv = copy_argv(argv);
    p->restart_on_crash = restart_on_crash ? 1 : 0;
    p->timeout_ms = timeout_ms > 0 ? timeout_ms : 30000;
    if (!p->command || !p->argv) {
        free(p->command);
        free_argv_copy(p->argv);
        free(p);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy plugin process config");
    }

    cc_result_t rc = cc_mutex_create(&p->request_mutex);
    if (rc.code != CC_OK) {
        free(p->command);
        free_argv_copy(p->argv);
        free(p);
        return rc;
    }

    rc = spawn_locked(p);
    if (rc.code != CC_OK) {
        cc_mutex_destroy(p->request_mutex);
        free(p->command);
        free_argv_copy(p->argv);
        free(p);
        return rc;
    }

    *out_process = p;
    return cc_result_ok();
}

/**
 * cc_plugin_process_call — 按 JSON-RPC 一问一答模式写入请求并读取插件进程响应。
 *
 * @param request_json 借用的只读字符串；函数不会释放该指针。
 * @param out_json 输出参数；调用方传入有效指针，成功后接收结果。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
cc_result_t cc_plugin_process_call(
    cc_plugin_process_t *process,
    const char *request_json,
    char **out_json
)
{
    return cc_plugin_process_call_with_options(process, request_json, 0, NULL, out_json);
}

cc_result_t cc_plugin_process_call_with_options(
    cc_plugin_process_t *process,
    const char *request_json,
    int timeout_ms,
    cc_cancel_token_t *cancel_token,
    char **out_json
)
{
    if (!process || !request_json || !out_json)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid plugin call argument");
    if (cc_cancel_token_is_cancelled(cancel_token))
        return cc_result_error(CC_ERR_CANCELLED, "Plugin call cancelled before send");

    cc_mutex_lock(process->request_mutex);
    if (cc_cancel_token_is_cancelled(cancel_token)) {
        cc_mutex_unlock(process->request_mutex);
        return cc_result_error(CC_ERR_CANCELLED, "Plugin call cancelled before send");
    }

    int effective_timeout_ms = timeout_ms > 0 ? timeout_ms : process->timeout_ms;
    int request_sent = 0;
    cc_result_t rc = cc_plugin_process_send(process, request_json);
    if (rc.code == CC_OK)
    {
        request_sent = 1;
        rc = cc_process_pipe_read_line_timeout_cancel(
            process->pipe, effective_timeout_ms, cancel_token, out_json);
    }
    if (rc.code == CC_ERR_CANCELLED && request_sent) {
        /*
         * 一旦请求已经写给子进程，取消读取会让 stdout 中可能残留迟到响应。
         * 为避免下一次调用读到旧响应造成 JSON-RPC 串线，这里把该 worker
         * 复位；默认配置 restart_on_crash=1，会立即启动一个干净进程。
         */
        if (process->restart_on_crash) {
            cc_result_t restart_rc = restart_locked(process);
            cc_result_free(&restart_rc);
        } else if (process->pipe) {
            cc_process_pipe_destroy(process->pipe);
            process->pipe = NULL;
        }
    }
    if (rc.code != CC_OK && rc.code != CC_ERR_CANCELLED && process->restart_on_crash) {
        cc_result_t first = rc;
        cc_result_t restart_rc = restart_locked(process);
        if (restart_rc.code == CC_OK) {
            cc_result_free(&first);
            rc = cc_plugin_process_send(process, request_json);
            if (rc.code == CC_OK)
                rc = cc_process_pipe_read_line_timeout_cancel(
                    process->pipe, effective_timeout_ms, cancel_token, out_json);
        } else {
            cc_result_free(&first);
            rc = restart_rc;
        }
    }
    cc_mutex_unlock(process->request_mutex);
    return rc;
}

/**
 * cc_plugin_process_send — 向插件子进程 stdin 写入一行 JSON-RPC 文本。
 *
 * @param json_str 借用的只读字符串；函数不会释放该指针。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
cc_result_t cc_plugin_process_send(
    cc_plugin_process_t *process,
    const char *json_str
)
{
    if (!process || !process->pipe)
        return cc_result_error(CC_ERR_PLATFORM, "Plugin process not running");
    return cc_process_pipe_write(process->pipe, json_str);
}

/**
 * cc_plugin_process_receive — 从插件子进程 stdout 读取一行 JSON-RPC 响应文本。
 *
 * @param out_json 输出参数；调用方传入有效指针，成功后接收结果。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
cc_result_t cc_plugin_process_receive(
    cc_plugin_process_t *process,
    char **out_json
)
{
    if (!process || !process->pipe)
        return cc_result_error(CC_ERR_PLATFORM, "Plugin process not running");
    return cc_process_pipe_read_line_timeout(process->pipe, process->timeout_ms, out_json);
}

/**
 * cc_plugin_process_stop — 释放、停止或复位该组件拥有的资源，防止失败路径泄漏。
 *
 */
void cc_plugin_process_stop(cc_plugin_process_t *process)
{
    if (!process) return;
    cc_mutex_lock(process->request_mutex);
    if (process->pipe) {
        cc_process_pipe_destroy(process->pipe);
        process->pipe = NULL;
    }
    cc_mutex_unlock(process->request_mutex);
}

/**
 * cc_plugin_process_destroy — 释放、停止或复位该组件拥有的资源，防止失败路径泄漏。
 *
 */
void cc_plugin_process_destroy(cc_plugin_process_t *process)
{
    if (!process) return;
    cc_plugin_process_stop(process);
    cc_mutex_destroy(process->request_mutex);
    free(process->command);
    free_argv_copy(process->argv);
    free(process);
}
