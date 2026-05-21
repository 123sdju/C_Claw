/**
 * 学习导读：cclaw/ports/include/cc/ports/cc_process.h
 *
 * 所属层次：端口层。
 * 阅读重点：这里定义可替换接口，阅读时重点看 struct + vtable + void *self 如何表达多态和依赖注入。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * cc_process.h — 进程执行抽象模块
 *
 * @file    cc/ports/cc_process.h
 * @brief   提供子进程的创建、执行和结果获取功能。
 *
 * 两个层次的 API：
 *   cc_process_run()     — 同步执行，阻塞等待直到进程结束，收集全部输出
 *   cc_process_pipe_*()  — 持久管道进程，适合实时双向通信（如 JSON-RPC 插件）
 */

#ifndef CC_PROCESS_H
#define CC_PROCESS_H

#include "cc/core/cc_result.h"

typedef struct cc_cancel_token cc_cancel_token_t;

/**
 * cc_process_options — 同步启动子进程时使用的参数集合。
 *
 * 该结构体不拥有字符串或数组；cc_process_run 只在调用期间读取这些字段。
 * args/env 遵循 NULL 结尾数组约定，便于 POSIX exec 和 Windows CreateProcess
 * 适配层转换。
 */
typedef struct cc_process_options {
    /** 可执行文件或命令路径；不可为 NULL。 */
    char *command;
    /** 参数数组，通常包含 argv[0]；以 NULL 结尾。 */
    char **args;
    /** 可选工作目录；NULL 表示继承当前进程目录。 */
    char *working_dir;
    /** 可选环境变量数组，格式为 KEY=VALUE，以 NULL 结尾。 */
    char **env;
    /** 超时时间毫秒数；0 表示平台默认或不主动超时。 */
    int timeout_ms;
    /** 非 0 表示捕获 stdout 到 result.stdout_text。 */
    int capture_stdout;
    /** 非 0 表示捕获 stderr 到 result.stderr_text。 */
    int capture_stderr;
} cc_process_options_t;

/**
 * cc_process_result — 结果数据结构，承载调用输出和错误信息；其中堆字符串需要由对应 free 函数释放。
 *
 * stdout_text/stderr_text 由平台实现分配，调用方用 cc_process_result_free
 * 释放；结构体本身通常由调用方按值持有。
 */
typedef struct cc_process_result {
    /** 子进程退出码；进程未正常退出时由平台层给出约定值。 */
    int exit_code;
    /** 捕获的 stdout 文本；可能为 NULL。 */
    char *stdout_text;
    /** 捕获的 stderr 文本；可能为 NULL。 */
    char *stderr_text;
    /** 非 0 表示因为 timeout_ms 到达而终止或放弃等待。 */
    int timed_out;
} cc_process_result_t;

/**
 * cc_process_run — 同步执行一个子进程并收集退出状态和可选输出。
 *
 * 平台实现负责命令启动、超时处理和输出捕获。options 中的字符串全部借用；
 * out_result 中的堆字符串由调用方用 cc_process_result_free 释放。
 *
 * @param options 借用的进程启动参数；不可为 NULL。
 * @param out_result 输出结果；成功时写入退出码、输出和超时标志。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
cc_result_t cc_process_run(
    const cc_process_options_t *options,
    cc_process_result_t *out_result
);

/**
 * cc_process_result_free — 释放结果结构体内部由平台层分配的缓冲区，并把大小/指针复位。
 *
 * 只释放 stdout_text/stderr_text，不释放 result 结构体本身；传入 NULL 安全。
 *
 * @param result 要清理的进程结果对象。
 */
void cc_process_result_free(cc_process_result_t *result);

/* ─────────────────────────────────────────────────────────────────────
 *  持久管道进程 API（用于插件等需要实时双向通信的场景）
 *
 *  使用流程：
 *    sp = cc_process_pipe_spawn(cmd, argv)
 *    cc_process_pipe_write(sp, json_request)
 *    resp = cc_process_pipe_read_line(sp)
 *    ... repeat ...
 *    cc_process_pipe_destroy(sp)
 * ───────────────────────────────────────────────────────────────────── */

/**
 * cc_process_pipe — 持久子进程管道的不透明句柄。
 *
 * 由 cc_process_pipe_spawn 创建，适合插件 JSON-RPC 这种需要多次写入/读取的
 * 长生命周期子进程。调用方最终必须 destroy。
 */
typedef struct cc_process_pipe cc_process_pipe_t;

/**
 * cc_process_pipe_spawn — 启动一个可双向通信的持久子进程。
 *
 * @param command 可执行文件路径；调用期间借用。
 * @param argv NULL 结尾参数数组；调用期间借用。
 * @param out_pipe 输出管道句柄；成功后调用方负责 destroy。
 * @return CC_OK 表示启动成功；失败返回平台错误。
 */
cc_result_t cc_process_pipe_spawn(
    const char *command,
    char *const argv[],
    cc_process_pipe_t **out_pipe
);

/**
 * cc_process_pipe_write — 向持久子进程 stdin 写入一段文本。
 *
 * data 通常是一行 JSON-RPC 请求；函数不自动补换行，调用方应传入协议需要的
 * 完整文本。
 *
 * @param pipe 借用的管道句柄；不可为 NULL。
 * @param data 借用的待写入文本；不可为 NULL。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
cc_result_t cc_process_pipe_write(
    cc_process_pipe_t *pipe,
    const char *data
);

/**
 * cc_process_pipe_read_line — 从持久子进程 stdout 读取一行文本。
 *
 * 返回的 out_line 为新分配字符串，包含读取到的一行内容；调用方负责 free。
 * 该接口用于插件 JSON-RPC 的“一请求一响应行”协议。
 *
 * @param pipe 借用的管道句柄；不可为 NULL。
 * @param out_line 输出字符串；成功时写入新分配的一行文本。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
cc_result_t cc_process_pipe_read_line(
    cc_process_pipe_t *pipe,
    char **out_line
);

/**
 * cc_process_pipe_read_line_timeout — 带超时读取一行文本。
 *
 * timeout_ms <= 0 时使用平台默认值。该接口让上层 plugin/MCP 可以把
 * config.json 中的 timeoutMs 落到真实管道等待上，而不是只能依赖平台默认。
 */
cc_result_t cc_process_pipe_read_line_timeout(
    cc_process_pipe_t *pipe,
    int timeout_ms,
    char **out_line
);

/**
 * cc_process_pipe_read_line_timeout_cancel — 带超时和取消令牌读取一行文本。
 *
 * cancel_token 为借用指针；平台实现只查询状态，不取得所有权。该接口用于
 * plugin/MCP stdio 这类长等待路径，让 interrupt/shutdown 能从 tool context
 * 传到实际 pipe 等待。NULL token 表示不启用取消。
 */
cc_result_t cc_process_pipe_read_line_timeout_cancel(
    cc_process_pipe_t *pipe,
    int timeout_ms,
    cc_cancel_token_t *cancel_token,
    char **out_line
);

/**
 * cc_process_pipe_stop — 释放、停止或复位该组件拥有的资源，防止失败路径泄漏。
 *
 * 请求子进程停止并关闭通信管道，但不释放 pipe 句柄本身；用于 destroy 前或
 * 需要提前终止插件进程的路径。传入 NULL 安全。
 *
 * @param pipe 要停止的管道句柄。
 */
void cc_process_pipe_stop(cc_process_pipe_t *pipe);

/**
 * cc_process_pipe_destroy — 释放、停止或复位该组件拥有的资源，防止失败路径泄漏。
 *
 * 若子进程仍在运行，destroy 会先停止它，再释放 pipe 句柄及平台私有资源。
 * 传入 NULL 安全。
 *
 * @param pipe 要销毁的管道句柄；函数取得并释放该句柄所有权。
 */
void cc_process_pipe_destroy(cc_process_pipe_t *pipe);

#endif
