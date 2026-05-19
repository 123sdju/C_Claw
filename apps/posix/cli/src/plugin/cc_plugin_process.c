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

/**
 * cc_plugin_process — 插件子进程句柄，保存进程管道和启动配置，生命周期由 plugin manager 管理。
 *
 * 资源约定：动态缓冲区由该结构拥有；借用指针只在所属调用链有效，count/capacity 字段必须同步维护。
 */
typedef struct cc_plugin_process {
    cc_process_pipe_t *pipe;
    cc_mutex_t request_mutex;
} cc_plugin_process_t;

/**
 * cc_plugin_process_start — 创建、启动或加载组件资源，并把错误统一传播给调用方。
 *
 * 位置：插件/JSON-RPC 子系统。注释重点说明当前函数的输入输出、资源边界和错误传播。
 *
 * @param command 借用的只读字符串；函数不会释放该指针。
 * @param argv 命令行参数数组；只在本次调用中借用。
 * @param out_process 输出参数；成功时写入有效结果，失败时保持为 NULL 或未定义状态。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
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

/**
 * cc_plugin_process_call — 按 JSON-RPC 一问一答模式写入请求并读取插件进程响应。
 *
 * 位置：插件/JSON-RPC 子系统。注释重点说明当前函数的输入输出、资源边界和错误传播。
 *
 * @param process 借用的指针参数；若需要长期保存内容，函数会复制。
 * @param request_json 借用的只读字符串；函数不会释放该指针。
 * @param out_json 输出参数；成功时写入有效结果，失败时保持为 NULL 或未定义状态。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
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

/**
 * cc_plugin_process_send — 向插件子进程 stdin 写入一行 JSON-RPC 文本。
 *
 * 位置：插件/JSON-RPC 子系统。注释重点说明当前函数的输入输出、资源边界和错误传播。
 *
 * @param process 借用的指针参数；若需要长期保存内容，函数会复制。
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
 * 位置：插件/JSON-RPC 子系统。注释重点说明当前函数的输入输出、资源边界和错误传播。
 *
 * @param process 借用的指针参数；若需要长期保存内容，函数会复制。
 * @param out_json 输出参数；成功时写入有效结果，失败时保持为 NULL 或未定义状态。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
cc_result_t cc_plugin_process_receive(
    cc_plugin_process_t *process,
    char **out_json
)
{
    if (!process || !process->pipe)
        return cc_result_error(CC_ERR_PLATFORM, "Plugin process not running");
    return cc_process_pipe_read_line(process->pipe, out_json);
}

/**
 * cc_plugin_process_stop — 释放、停止或复位该组件拥有的资源，防止失败路径泄漏。
 *
 * 位置：插件/JSON-RPC 子系统。注释重点说明当前函数的输入输出、资源边界和错误传播。
 *
 * @param process 借用的指针参数；若需要长期保存内容，函数会复制。
 * 无返回值；副作用体现在对象状态、输出缓冲区或资源释放上。
 */
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

/**
 * cc_plugin_process_destroy — 释放、停止或复位该组件拥有的资源，防止失败路径泄漏。
 *
 * 位置：插件/JSON-RPC 子系统。注释重点说明当前函数的输入输出、资源边界和错误传播。
 *
 * @param process 借用的指针参数；若需要长期保存内容，函数会复制。
 * 无返回值；副作用体现在对象状态、输出缓冲区或资源释放上。
 */
void cc_plugin_process_destroy(cc_plugin_process_t *process)
{
    if (!process) return;
    cc_plugin_process_stop(process);
    cc_mutex_destroy(process->request_mutex);
    if (process->pipe)
        cc_process_pipe_destroy(process->pipe);
    free(process);
}