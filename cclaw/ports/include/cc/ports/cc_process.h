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

typedef struct cc_process_options {
    char *command;
    char **args;
    char *working_dir;
    char **env;
    int timeout_ms;
    int capture_stdout;
    int capture_stderr;
} cc_process_options_t;

typedef struct cc_process_result {
    int exit_code;
    char *stdout_text;
    char *stderr_text;
    int timed_out;
} cc_process_result_t;

cc_result_t cc_process_run(
    const cc_process_options_t *options,
    cc_process_result_t *out_result
);

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

typedef struct cc_process_pipe cc_process_pipe_t;

cc_result_t cc_process_pipe_spawn(
    const char *command,
    char *const argv[],
    cc_process_pipe_t **out_pipe
);

cc_result_t cc_process_pipe_write(
    cc_process_pipe_t *pipe,
    const char *data
);

cc_result_t cc_process_pipe_read_line(
    cc_process_pipe_t *pipe,
    char **out_line
);

void cc_process_pipe_stop(cc_process_pipe_t *pipe);

void cc_process_pipe_destroy(cc_process_pipe_t *pipe);

#endif