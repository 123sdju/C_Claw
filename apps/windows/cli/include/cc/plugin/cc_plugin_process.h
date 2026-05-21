/**
 * 学习导读：apps/windows/cli/include/cc/plugin/cc_plugin_process.h
 *
 * 所属层次：Windows CLI 应用层。
 * 阅读重点：这里镜像桌面 CLI 能力但使用 Windows 平台实现，阅读时重点比较与 POSIX 版本的差异。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/******************************************************************************
 * cc_plugin_process.h — 插件进程管理接口
 *
 * 本头文件声明了外部插件子进程的完整生命周期管理 API。
 * 使用 opaque pointer 模式隐藏内部实现细节。
 *
 * ── 生命周期 ──
 *
 *   start(command, argv)    — 创建子进程（fork + exec + pipe）
 *   send(json_str)          — 通过 stdin 管道发送 JSON-RPC 请求
 *   receive(&json_str)      — 从 stdout 管道接收 JSON-RPC 响应
 *        ... (可多次 send/receive) ...
 *   stop()                  — 终止子进程（SIGTERM → SIGKILL → waitpid）
 *   destroy()               — 释放内存（先 stop 再 free）
 *
 * ── 通信特点 ──
 *
 *   - 同步阻塞：send 后必须 receive，不支持并发
 *   - 行协议：每行一个完整 JSON，以 '\n' 分隔
 *   - 单行 JSON：发送方必须使用紧凑格式（无缩进换行）
 *   - 无缓冲写：写端设为 _IONBF，数据立即到达子进程
 *
 * ── 依赖 ──
 *
 *   cc/core/cc_result.h — 统一错误处理类型 cc_result_t
 *****************************************************************************/

#ifndef CC_PLUGIN_PROCESS_H
#define CC_PLUGIN_PROCESS_H

#include "cc/app/cc_cancel_token.h"
#include "cc/core/cc_result.h"

/* opaque pointer — 实现细节隐藏在 .c 文件中 */
typedef struct cc_plugin_process cc_plugin_process_t;

/**
 * 启动插件子进程。
 *
 * fork + exec 创建子进程，建立三对管道（stdin/stdout/stderr）。
 * 子进程的 stdin/stdout 被重定向到管道，用于 JSON-RPC 通信。
 *
 * @param command     可执行文件路径或名称（支持 PATH 查找）
 * @param argv        NULL 结尾的参数数组，argv[0] 应为 command
 * @param out_process 输出：创建的进程对象
 * @return            CC_OK 成功，否则为管道/fork/exec 错误
 */
cc_result_t cc_plugin_process_start(
    const char *command,
    char *const argv[],
    cc_plugin_process_t **out_process
);

/**
 * 启动插件子进程并配置运行时策略。
 *
 * restart_on_crash 非 0 时，如果一次 JSON-RPC 调用遇到管道写入、读取或超时
 * 失败，进程层会销毁当前 worker、按原始 argv 重启并重试当前请求一次。
 * timeout_ms 直接传给平台 pipe read timeout，用于限制单次工具调用等待时间。
 */
cc_result_t cc_plugin_process_start_with_options(
    const char *command,
    char *const argv[],
    int restart_on_crash,
    int timeout_ms,
    cc_plugin_process_t **out_process
);

/**
 * 向插件子进程发送 JSON-RPC 请求。
 *
 * 将 JSON 字符串写入子进程的 stdin 管道。自动追加 '\n' 换行符
 * 并调用 fflush 确保数据立即到达子进程。
 *
 * @param process  目标插件进程
 * @param json_str 待发送的 JSON 字符串（不含换行符）
 * @return         CC_OK 成功，否则管道写入失败（子进程可能已崩溃）
 */
cc_result_t cc_plugin_process_send(
    cc_plugin_process_t *process,
    const char *json_str
);

/**
 * 从插件子进程接收 JSON-RPC 响应。
 *
 * 从子进程的 stdout 管道阻塞读取一行 JSON。自动去除尾部 '\n' 和 '\r'。
 * 返回的字符串在堆上分配，调用者负责 free()。
 *
 * @param process  目标插件进程
 * @param out_json 输出：JSON 响应字符串（调用者负责 free）
 * @return         CC_OK 成功，否则读取失败（子进程可能已退出）
 */
cc_result_t cc_plugin_process_receive(
    cc_plugin_process_t *process,
    char **out_json
);

/**
 * 原子执行一次 JSON-RPC 请求。
 *
 * 在同一插件进程上串行化 send + receive，避免多线程并发调用时请求和响应交错。
 * 返回的响应字符串在堆上分配，调用者负责 free()。
 *
 * @param process       目标插件进程
 * @param request_json  待发送的 JSON-RPC 请求（不含换行符）
 * @param out_json      输出：JSON 响应字符串
 * @return              CC_OK 成功，否则为写入/读取错误
 */
cc_result_t cc_plugin_process_call(
    cc_plugin_process_t *process,
    const char *request_json,
    char **out_json
);

/**
 * 原子执行一次 JSON-RPC 请求，并把 timeout/cancel 传到底层 pipe 读取。
 *
 * timeout_ms <= 0 时使用插件启动配置里的默认 timeout。cancel_token 为借用
 * 指针，函数只查询状态；取消后返回 CC_ERR_CANCELLED，不会把它伪装成普通
 * 插件错误。
 */
cc_result_t cc_plugin_process_call_with_options(
    cc_plugin_process_t *process,
    const char *request_json,
    int timeout_ms,
    cc_cancel_token_t *cancel_token,
    char **out_json
);

/**
 * 终止插件子进程并关闭管道。
 *
 * 两阶段终止：SIGTERM（优雅）→ 100ms 等待 → SIGKILL（强制）→ waitpid。
 * 幂等操作，可安全地多次调用。
 *
 * @param process  要终止的插件进程
 */
void cc_plugin_process_stop(cc_plugin_process_t *process);

/**
 * 销毁插件进程对象。
 *
 * 先调用 stop 终止子进程并关闭管道，再释放 struct 内存。
 * process 可以为 NULL。
 *
 * @param process  要销毁的插件进程
 */
void cc_plugin_process_destroy(cc_plugin_process_t *process);

#endif
