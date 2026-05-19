/**
 * 学习导读：cclaw/platforms/windows/src/cc_windows_process.c
 *
 * 所属层次：平台层。
 * 阅读重点：这里隐藏 POSIX、Windows、ESP32 的系统 API 差异，阅读时重点看同名端口函数如何按平台实现。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * cc_windows_process.c — Windows 进程管理与子进程执行
 *
 * 在整体架构中的角色和层次：
 *   本模块位于 Platform 层的 Windows 平台实现子层。
 *   Platform 层是整个系统的最底层，负责封装操作系统差异。
 *   本文件是 cc_process.h 接口在 Windows（Win32）平台的具体实现，
 *   向上层（如 Sandbox 沙箱模块、Agent 工具执行模块）提供外部进程的
 *   创建、执行、输出捕获、超时控制和持久管道通信能力。
 *   调用者通过统一的 cc_process_options_t / cc_process_result_t 和
 *   cc_process_pipe_t 接口操作，无需关心底层是 CreateProcess 还是 POSIX fork/exec。
 *
 * 核心流程（CreateProcess + 匿名管道模式）：
 *   1. 通过 CreatePipe() 创建 stdout/stderr 匿名管道
 *   2. 通过 SetHandleInformation() 设置管道的继承属性（读端不可继承）
 *   3. 构造 Windows 命令行字符串（cmd_line），将所有参数用双引号包裹拼接
 *   4. 填充 STARTUPINFOA 结构体，将管道写端 / 标准句柄绑定到子进程的 I/O
 *   5. CreateProcessA() 创建子进程（CREATE_NO_WINDOW 防止弹出控制台窗口）
 *   6. 关闭管道写端（父进程不需要写入子进程标准输入），释放子线程句柄
 *   7. 通过 WaitForSingleObject() 等待子进程结束，支持超时（timeout_ms）
 *   8. 超时后通过 TerminateProcess() 强制终止子进程
 *   9. 从管道读端通过 drain_pipe()（PeekNamedPipe + ReadFile）收集输出
 *   10. 通过 GetExitCodeProcess() 获取退出码，组装 cc_process_result_t
 *
 * 与 POSIX fork/exec 的关键区别：
 *   - Windows 没有 fork() 语义，每个子进程通过 CreateProcess() 独立创建
 *   - Windows 子进程不与父进程共享内存空间（天然隔离，但启动成本更高）
 *   - Windows 使用 HANDLE + WaitForSingleObject 等待，而非 waitpid() + WNOHANG 轮询
 *   - Windows 通过 TerminateProcess() 强制终止，等价于 POSIX SIGKILL（进程无清理机会）
 *   - Windows 命令行是单个字符串（cmd_line），需要手动拼接和引号转义
 *   - Windows 管道使用 HANDLE + ReadFile/WriteFile，而非 POSIX fd + read/write
 *   - Windows 环境变量继承：子进程默认继承父进程环境块，无需显式传递
 *
 * 输出捕获设计（capture_buffer_t）：
 *   - 使用动态扩容缓冲区（初始 4KB，满后翻倍），而非固定大小数组
 *   - drain_pipe() 通过 PeekNamedPipe() 查询可用数据量，避免阻塞读取
 *   - 在子进程结束后一次性抽取管道中剩余数据，而非实时异步读取
 *   - 此设计与 POSIX 版本使用 read() + O_NONBLOCK 的策略对应
 *
 * 超时机制：
 *   - WaitForSingleObject() 原生支持超时参数，无需轮询（优于 POSIX 版本的 10ms 轮询）
 *   - 超时后通过 TerminateProcess() 强制终止，进程退出码设为 -1
 *   - TerminateProcess() 类似于 SIGKILL，子进程无法拦截或执行清理逻辑
 *
 * 持久管道进程（cc_process_pipe）：
 *   - 为插件/长期运行的子进程提供双向通信管道
 *   - stdin 管道用于向子进程发送数据（cc_process_pipe_write）
 *   - stdout 管道用于从子进程读取数据（cc_process_pipe_read_line，按行读取）
 *   - 使用 _open_osfhandle + _fdopen 将 Windows 句柄转换为 C FILE* 流
 *   - 停止时通过 TerminateProcess + WaitForSingleObject 等待进程退出
 *
 * 设计决策：
 *   - 选择 CreateProcessA（ANSI 版本）而非 CreateProcessW：保持与项目其他部分
 *     的编码一致性，简化字符串处理
 *   - 使用 CREATE_NO_WINDOW 标志：子进程为控制台程序时不显示窗口
 *   - args 为空时通过 cmd.exe /c 执行：支持内置命令（dir, echo 等）
 *   - 管道读端不可继承（SetHandleInformation + HANDLE_FLAG_INHERIT=0）：
 *     防止子进程意外持有读端句柄导致管道无法正常关闭
 *   - to_child 流使用无缓冲模式（_IONBF）：确保写入数据立即到达子进程
 *
 * 平台依赖（Windows 特有，不可移植到 POSIX）：
 *   - CreatePipe / CreateProcessA — 进程创建与管道创建
 *   - SetHandleInformation — 句柄继承属性控制
 *   - WaitForSingleObject / GetExitCodeProcess — 进程等待与退出码获取
 *   - TerminateProcess — 进程强制终止
 *   - PeekNamedPipe / ReadFile — 管道数据读取
 *   - CloseHandle — 句柄资源释放
 *   - _open_osfhandle / _fdopen — 句柄到 C 运行库文件描述符转换
 */

#include "cc/ports/cc_process.h"

#ifdef _WIN32
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>

/*
 * capture_buffer_t — 动态扩容的输出捕获缓冲区
 *
 * 用于在子进程执行期间收集标准输出和标准错误数据。
 * 采用动态扩容策略：初始容量为 0（延迟分配），首次写入时分配 4KB，
 * 之后每次空间不足时将容量翻倍。
 *
 * 字段：
 *   data  — 指向动态分配的缓冲区（NUL 结尾）
 *   total — 当前已存储的有效数据字节数（不含结尾 NUL）
 *   cap   — 缓冲区当前分配的容量（含结尾 NUL 所需的一个字节）
 */
typedef struct {
    char *data;
    size_t total;
    size_t cap;
} capture_buffer_t;

/*
 * capture_append — 向捕获缓冲区追加数据
 *
 * 将指定长度的数据追加到动态扩容缓冲区的末尾，自动处理扩容逻辑。
 * 缓冲区的数据始终保持 NUL 结尾（即 total 位置总是 '\0'）。
 *
 * 参数：
 *   capture — 指向捕获缓冲区（为 NULL 时无操作返回 1）
 *   buf     — 要追加的数据
 *   len     — 数据长度（为 0 时无操作返回 1）
 *
 * 返回值：
 *   1 表示成功，0 表示内存分配失败
 *
 * 扩容策略：
 *   首次写入时分配 4096 字节，后续每次翻倍（2x），直到容纳新数据。
 */
static int capture_append(capture_buffer_t *capture, const char *buf, size_t len)
{
    if (!capture || len == 0) return 1;
    if (capture->total + len + 1 > capture->cap) {
        size_t new_cap = capture->cap ? capture->cap : 4096;
        while (capture->total + len + 1 > new_cap) new_cap *= 2;
        char *new_data = realloc(capture->data, new_cap);
        if (!new_data) return 0;
        capture->data = new_data;
        capture->cap = new_cap;
    }
    memcpy(capture->data + capture->total, buf, len);
    capture->total += len;
    capture->data[capture->total] = '\0';
    return 1;
}

/*
 * drain_pipe — 从管道读端抽取全部可用数据
 *
 * 循环调用 PeekNamedPipe() 检测管道中是否有待读取的数据，
 * 然后通过 ReadFile() 读取并追加到捕获缓冲区中。
 * 当管道中无可用数据或读取出错时停止。
 *
 * 参数：
 *   h       — 管道的读端句柄（必须是有效的 HANDLE）
 *   capture — 目标捕获缓冲区
 *
 * 与 POSIX 版本的区别：
 *   POSIX 使用 read() + O_NONBLOCK，Windows 使用 PeekNamedPipe + ReadFile。
 *   Windows 的管道本质上是命名管道，PeekNamedPipe 可以检查是否有数据
 *   而不会阻塞，实现在子进程结束后安全抽取所有输出的目的。
 */
static void drain_pipe(HANDLE h, capture_buffer_t *capture)
{
    char buf[4096];
    DWORD available = 0;
    while (PeekNamedPipe(h, NULL, 0, NULL, &available, NULL) && available > 0) {
        DWORD nread = 0;
        if (ReadFile(h, buf, sizeof(buf) < available ? sizeof(buf) : available, &nread, NULL) && nread > 0) {
            if (!capture_append(capture, buf, nread)) return;
        } else {
            break;
        }
    }
}

/**
 * append_cmd_char — 向动态数组、字符串缓冲或结果集合追加内容，必要时扩容。
 *
 * 位置：Windows 平台层。注释重点说明当前函数的输入输出、资源边界和错误传播。
 *
 * @param buf 可写缓冲区或字符串指针；函数可能就地修改内容但不释放缓冲区本身。
 * @param cap 按值传入，用于控制本次操作。
 * @param pos 借用的指针参数；若需要长期保存内容，函数会复制。
 * @param ch 按值传入，用于控制本次操作。
 * @return 返回整数状态、计数或断言结果，供当前调用链判断下一步。
 */
static int append_cmd_char(char *buf, size_t cap, size_t *pos, char ch)
{
    if (*pos + 1 >= cap) return 0;
    buf[(*pos)++] = ch;
    buf[*pos] = '\0';
    return 1;
}

/**
 * append_cmd_space — 向动态数组、字符串缓冲或结果集合追加内容，必要时扩容。
 *
 * 位置：Windows 平台层。注释重点说明当前函数的输入输出、资源边界和错误传播。
 *
 * @param buf 可写缓冲区或字符串指针；函数可能就地修改内容但不释放缓冲区本身。
 * @param cap 按值传入，用于控制本次操作。
 * @param pos 借用的指针参数；若需要长期保存内容，函数会复制。
 * @return 返回整数状态、计数或断言结果，供当前调用链判断下一步。
 */
static int append_cmd_space(char *buf, size_t cap, size_t *pos)
{
    if (*pos == 0) return 1;
    return append_cmd_char(buf, cap, pos, ' ');
}

/**
 * append_windows_quoted_arg — 向动态数组、字符串缓冲或结果集合追加内容，必要时扩容。
 *
 * 位置：Windows 平台层。注释重点说明当前函数的输入输出、资源边界和错误传播。
 *
 * @param buf 可写缓冲区或字符串指针；函数可能就地修改内容但不释放缓冲区本身。
 * @param cap 按值传入，用于控制本次操作。
 * @param pos 借用的指针参数；若需要长期保存内容，函数会复制。
 * @param arg 回调上下文；函数只透传或临时读取，不取得所有权。
 * @return 返回整数状态、计数或断言结果，供当前调用链判断下一步。
 */
static int append_windows_quoted_arg(char *buf, size_t cap, size_t *pos, const char *arg)
{
    if (!arg) arg = "";
    if (!append_cmd_space(buf, cap, pos)) return 0;
    if (!append_cmd_char(buf, cap, pos, '"')) return 0;

    size_t backslashes = 0;
    for (const char *p = arg; *p; p++) {
        if (*p == '\\') {
            backslashes++;
            continue;
        }
        if (*p == '"') {
            for (size_t i = 0; i < backslashes * 2 + 1; i++) {
                if (!append_cmd_char(buf, cap, pos, '\\')) return 0;
            }
            if (!append_cmd_char(buf, cap, pos, '"')) return 0;
            backslashes = 0;
            continue;
        }
        for (size_t i = 0; i < backslashes; i++) {
            if (!append_cmd_char(buf, cap, pos, '\\')) return 0;
        }
        backslashes = 0;
        if (!append_cmd_char(buf, cap, pos, *p)) return 0;
    }

    for (size_t i = 0; i < backslashes * 2; i++) {
        if (!append_cmd_char(buf, cap, pos, '\\')) return 0;
    }
    return append_cmd_char(buf, cap, pos, '"');
}

/**
 * build_windows_env_block — 把 NULL 结尾的 KEY=VALUE 数组转换为 Windows 需要的双 NUL 环境块。
 *
 * 位置：Windows 平台层。注释重点说明当前函数的输入输出、资源边界和错误传播。
 *
 * @param env 输出参数；成功时写入有效结果，失败时保持为 NULL 或未定义状态。
 * @return 新分配字符串；返回 NULL 表示分配或输入校验失败，调用方负责 free。
 */
static char *build_windows_env_block(char **env)
{
    if (!env) return NULL;

    size_t total = 1;
    for (char **e = env; *e; e++) total += strlen(*e) + 1;

    char *block = calloc(total, 1);
    if (!block) return NULL;

    char *p = block;
    for (char **e = env; *e; e++) {
        size_t len = strlen(*e);
        memcpy(p, *e, len);
        p += len + 1;
    }
    *p = '\0';
    return block;
}

/*
 * cc_process_result_free — 释放进程执行结果中的动态内存
 *
 * 释放 cc_process_result_t 结构体中 stdout_text 和 stderr_text
 * 指向的堆内存，并将整个结构体清零。
 *
 * 参数：
 *   result — 指向进程执行结果的指针（可为 NULL，此时函数无操作）
 *
 * 注意事项：
 *   - 此函数不释放 result 结构体本身，只释放其内部动态分配的字符串
 *   - 调用后 result 的所有字段被清零，可安全复用
 *   - Windows 版本的 stdout_text/stderr_text 通过 _strdup 或 malloc 分配，
 *     统一使用 free() 释放
 */
void cc_process_result_free(cc_process_result_t *result)
{
    if (!result) return;
    free(result->stdout_text);
    free(result->stderr_text);
    memset(result, 0, sizeof(cc_process_result_t));
}

/*
 * cc_process_run — 执行外部进程并捕获输出（Windows 实现）
 *
 * 使用 Windows CreateProcessA API 创建子进程，可选捕获其标准输出和
 * 标准错误。支持超时控制：通过 WaitForSingleObject 等待子进程，
 * 超时后通过 TerminateProcess 强制终止。
 *
 * 参数：
 *   options    — 进程执行选项，包含命令、参数、超时等配置
 *     options->command        — 要执行的命令字符串
 *     options->args           — 参数数组（可选，为 NULL 时通过 cmd.exe /c 执行）
 *     options->working_dir    — 工作目录（可选，为 NULL 时继承父进程）
 *     options->capture_stdout — 是否捕获标准输出
 *     options->capture_stderr — 是否捕获标准错误
 *     options->timeout_ms     — 超时时间（毫秒），0 或负数表示无限等待
 *   out_result — 输出参数，进程执行结果
 *     out_result->exit_code   — 退出码（正常退出）或 -1（异常/超时）
 *     out_result->timed_out   — 是否因超时被终止
 *     out_result->stdout_text — 标准输出内容（由调用者通过 cc_process_result_free 释放）
 *     out_result->stderr_text — 标准错误内容（由调用者通过 cc_process_result_free 释放）
 *
 * 返回值：
 *   成功返回 cc_result_ok()，失败返回 CC_ERR_PLATFORM
 *
 * 实现要点：
 *   1. 创建管道后，将读端设置为不可继承（HANDLE_FLAG_INHERIT=0）
 *   2. 构造命令行字符串时，命令本身和每个参数都用双引号包裹
 *   3. args 为空时通过 "cmd.exe /c <command>" 执行，支持 Windows 内置命令
 *   4. STARTF_USESTDHANDLES 标志启用标准句柄重定向
 *   5. CREATE_NO_WINDOW 标志防止子进程弹出控制台窗口
 *   6. 子进程创建后立即关闭 hThread（主线程句柄）和管道写端
 *   7. WaitForSingleObject 原生支持超时，无需轮询
 *   8. 超时后 TerminateProcess(..., -1) 强制终止，退出码设为 -1
 *   9. 管道数据在子进程结束后一次性抽取
 *
 * 平台注意事项：
 *   - cmd_line 缓冲区为 8192 字节，超长命令行会被截断
 *   - CreateProcessA 第 1 个参数为 NULL 时由系统解析 cmd_line 查找可执行文件
 *   - 子进程默认继承父进程的环境变量，与 POSIX 需显式设置 setenv 不同
 *   - TerminateProcess 不给予子进程清理机会（类似 POSIX SIGKILL）
 *   - stdout_text/stderr_text 为空时返回 _strdup("")，保证非 NULL
 */
cc_result_t cc_process_run(
    const cc_process_options_t *options,
    cc_process_result_t *out_result
)
{
    memset(out_result, 0, sizeof(cc_process_result_t));

    HANDLE stdout_read = NULL, stdout_write = NULL;
    HANDLE stderr_read = NULL, stderr_write = NULL;
    SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };

    if (options->capture_stdout) {
        if (!CreatePipe(&stdout_read, &stdout_write, &sa, 0))
            return cc_result_error(CC_ERR_PLATFORM, "Failed to create stdout pipe");
        SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);
    }

    if (options->capture_stderr) {
        if (!CreatePipe(&stderr_read, &stderr_write, &sa, 0)) {
            if (stdout_read) { CloseHandle(stdout_read); CloseHandle(stdout_write); }
            return cc_result_error(CC_ERR_PLATFORM, "Failed to create stderr pipe");
        }
        SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0);
    }

    char cmd_line[8192] = {0};
    size_t cmd_pos = 0;
    int cmd_ok = 1;
    if (options->args && options->args[0]) {
        cmd_ok = append_windows_quoted_arg(cmd_line, sizeof(cmd_line), &cmd_pos, options->command);
        for (int i = 1; cmd_ok && options->args[i]; i++) {
            cmd_ok = append_windows_quoted_arg(cmd_line, sizeof(cmd_line), &cmd_pos, options->args[i]);
        }
    } else {
        cmd_ok = append_windows_quoted_arg(cmd_line, sizeof(cmd_line), &cmd_pos, "cmd.exe") &&
                 append_windows_quoted_arg(cmd_line, sizeof(cmd_line), &cmd_pos, "/c") &&
                 append_windows_quoted_arg(cmd_line, sizeof(cmd_line), &cmd_pos, options->command);
    }
    if (!cmd_ok) {
        if (stdout_read) { CloseHandle(stdout_read); CloseHandle(stdout_write); }
        if (stderr_read) { CloseHandle(stderr_read); CloseHandle(stderr_write); }
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Command line too long");
    }

    STARTUPINFOA si = { sizeof(STARTUPINFOA) };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = options->capture_stdout ? stdout_write : GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = options->capture_stderr ? stderr_write : GetStdHandle(STD_ERROR_HANDLE);

    PROCESS_INFORMATION pi = {0};
    char *env_block = build_windows_env_block(options->env);
    if (options->env && !env_block) {
        if (stdout_read) { CloseHandle(stdout_read); CloseHandle(stdout_write); }
        if (stderr_read) { CloseHandle(stderr_read); CloseHandle(stderr_write); }
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate environment block");
    }

    if (!CreateProcessA(NULL, cmd_line, NULL, NULL, TRUE,
                        CREATE_NO_WINDOW, env_block,
                        options->working_dir, &si, &pi)) {
        free(env_block);
        if (stdout_read) { CloseHandle(stdout_read); CloseHandle(stdout_write); }
        if (stderr_read) { CloseHandle(stderr_read); CloseHandle(stderr_write); }
        return cc_result_error(CC_ERR_PLATFORM, "Failed to create process");
    }
    free(env_block);

    CloseHandle(pi.hThread);
    if (stdout_write) CloseHandle(stdout_write);
    if (stderr_write) CloseHandle(stderr_write);

    capture_buffer_t stdout_capture = {0};
    capture_buffer_t stderr_capture = {0};

    DWORD elapsed = 0;
    DWORD wait_result = WAIT_TIMEOUT;
    DWORD timeout = options->timeout_ms > 0 ? (DWORD)options->timeout_ms : INFINITE;

    while (1) {
        if (options->capture_stdout) drain_pipe(stdout_read, &stdout_capture);
        if (options->capture_stderr) drain_pipe(stderr_read, &stderr_capture);

        wait_result = WaitForSingleObject(pi.hProcess, 10);
        if (wait_result == WAIT_OBJECT_0) break;
        if (wait_result == WAIT_FAILED) break;

        if (timeout != INFINITE) {
            elapsed += 10;
            if (elapsed >= timeout) break;
        }
    }

    out_result->timed_out = (timeout != INFINITE && elapsed >= timeout && wait_result != WAIT_OBJECT_0) ? 1 : 0;
    if (out_result->timed_out) {
        TerminateProcess(pi.hProcess, (UINT)-1);
        WaitForSingleObject(pi.hProcess, 5000);
    }

    if (options->capture_stdout) drain_pipe(stdout_read, &stdout_capture);
    if (options->capture_stderr) drain_pipe(stderr_read, &stderr_capture);

    DWORD exit_code = 0;
    if (GetExitCodeProcess(pi.hProcess, &exit_code)) {
        out_result->exit_code = out_result->timed_out ? -1 : (int)exit_code;
    } else {
        out_result->exit_code = -1;
    }

    CloseHandle(pi.hProcess);
    if (stdout_read) CloseHandle(stdout_read);
    if (stderr_read) CloseHandle(stderr_read);

    out_result->stdout_text = stdout_capture.data ? stdout_capture.data : _strdup("");
    out_result->stderr_text = stderr_capture.data ? stderr_capture.data : _strdup("");

    return cc_result_ok();
}

/* ───────────────────────────────────────────────────────────────────
 *  持久管道进程实现（Windows CreatePipe + CreateProcess 双向通信）
 *
 *  为需要长期运行和双向通信的子进程（如插件进程）提供管道通信机制。
 *  POSIX 版本使用 fork/exec + POSIX pipe，Windows 版本使用
 *  CreatePipe + CreateProcessA 配合 HANDLE 管理。
 *
 *  cc_process_pipe 结构体封装了：
 *    - 子进程句柄（hProcess, hThread）用于生命周期管理
 *    - stdin 写端（hStdinWrite / to_child）用于向子进程发送数据
 *    - stdout 读端（hStdoutRead / from_child）用于从子进程读取数据
 *    - 通过 _open_osfhandle 和 _fdopen 将原生 HANDLE 转换为 C FILE* 流
 *
 *  生命周期：
 *    spawn → write/read_line（循环）→ stop → destroy
 * ─────────────────────────────────────────────────────────────────── */

/*
 * cc_process_pipe_t — 持久管道进程句柄
 *
 * 封装与子进程进行双向通信所需的所有 HANDLE 和 FILE* 资源。
 * 通过 cc_process_pipe_spawn 创建，cc_process_pipe_destroy 释放。
 *
 * 字段：
 *   hProcess     — 子进程句柄，用于等待退出和强制终止
 *   hThread      — 子进程主线程句柄，创建后不再使用，仅在销毁时关闭
 *   hStdinWrite  — 子进程 stdin 管道的写端，用于向子进程发送数据
 *   hStdoutRead  — 子进程 stdout 管道的读端，用于从子进程读取数据
 *   to_child     — 封装 hStdinWrite 的 FILE* 流（写入子进程 stdin）
 *   from_child   — 封装 hStdoutRead 的 FILE* 流（读取子进程 stdout）
 *
 * 设计说明：
 *   to_child 使用无缓冲模式（_IONBF），确保每次 fflush 后数据立即到达子进程。
 *   from_child 使用默认缓冲模式，按行读取时效率更高。
 *   不单独创建 stderr 管道，子进程的 stderr 重定向到 stdout。
 */
struct cc_process_pipe {
    HANDLE hProcess;
    HANDLE hThread;
    HANDLE hStdinWrite;
    HANDLE hStdoutRead;
    FILE *to_child;
    FILE *from_child;
};

/*
 * cc_process_pipe_spawn — 启动持久管道子进程
 *
 * 创建 stdin/stdout 管道并通过 CreateProcessA 启动子进程，
 * 建立双向通信通道。子进程的 stderr 合并到 stdout。
 *
 * 参数：
 *   command  — 要执行的可执行文件路径
 *   argv     — 参数数组（argv[0] 通常与 command 相同，后续为参数）
 *   out_pipe — 输出参数，指向新创建的管道进程句柄
 *
 * 返回值：
 *   成功返回 cc_result_ok()，失败返回 CC_ERR_PLATFORM 或 CC_ERR_OUT_OF_MEMORY
 *
 * 实现要点：
 *   1. 创建两个匿名管道：stdin（父写子读）和 stdout（子写父读）
 *   2. 将父进程持有的"不需要"端设为不可继承（防止句柄泄露到子进程）
 *   3. 构造命令行字符串（与 cc_process_run 相同的格式）
 *   4. 子进程的 stderr 重定向到 stdout（hStdError = hChildStdoutWrite）
 *   5. 创建子进程后关闭父进程端的"子进程侧"句柄（hChildStdinRead, hChildStdoutWrite）
 *   6. 通过 _open_osfhandle 将 HANDLE 转为 C 运行时 fd
 *   7. 通过 _fdopen 将 fd 转为 FILE* 流，to_child 设为无缓冲模式
 *
 * 平台注意事项：
 *   - _open_osfhandle 会"接管" HANDLE 的所有权，FILE* 关闭时会自动关闭 HANDLE
 *   - 管道缓冲区大小由系统默认（通常 4KB），满后写入端阻塞
 *   - 如果子进程意外退出，管道写入会失败（ferror 返回非零）
 */
cc_result_t cc_process_pipe_spawn(
    const char *command,
    char *const argv[],
    cc_process_pipe_t **out_pipe
)
{
    SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };

    HANDLE hChildStdinRead = NULL, hChildStdinWrite = NULL;
    HANDLE hChildStdoutRead = NULL, hChildStdoutWrite = NULL;

    if (!CreatePipe(&hChildStdinRead, &hChildStdinWrite, &sa, 0))
        return cc_result_error(CC_ERR_PLATFORM, "Failed to create stdin pipe");
    if (!CreatePipe(&hChildStdoutRead, &hChildStdoutWrite, &sa, 0)) {
        CloseHandle(hChildStdinRead); CloseHandle(hChildStdinWrite);
        return cc_result_error(CC_ERR_PLATFORM, "Failed to create stdout pipe");
    }

    SetHandleInformation(hChildStdinWrite, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(hChildStdoutRead, HANDLE_FLAG_INHERIT, 0);

    char cmd_line[8192] = {0};
    size_t pos = 0;
    int cmd_ok = append_windows_quoted_arg(cmd_line, sizeof(cmd_line), &pos, command);
    if (argv) {
        for (int i = 1; cmd_ok && argv[i]; i++) {
            cmd_ok = append_windows_quoted_arg(cmd_line, sizeof(cmd_line), &pos, argv[i]);
        }
    }
    if (!cmd_ok) {
        CloseHandle(hChildStdinRead); CloseHandle(hChildStdinWrite);
        CloseHandle(hChildStdoutRead); CloseHandle(hChildStdoutWrite);
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Command line too long");
    }

    STARTUPINFOA si = { sizeof(STARTUPINFOA) };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = hChildStdinRead;
    si.hStdOutput = hChildStdoutWrite;
    si.hStdError = hChildStdoutWrite;

    PROCESS_INFORMATION pi = {0};

    if (!CreateProcessA(NULL, cmd_line, NULL, NULL, TRUE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(hChildStdinRead); CloseHandle(hChildStdinWrite);
        CloseHandle(hChildStdoutRead); CloseHandle(hChildStdoutWrite);
        return cc_result_error(CC_ERR_PLATFORM, "Failed to create pipe process");
    }

    CloseHandle(hChildStdinRead);
    CloseHandle(hChildStdoutWrite);

    cc_process_pipe_t *p = calloc(1, sizeof(cc_process_pipe_t));
    if (!p) {
        CloseHandle(hChildStdinWrite);
        CloseHandle(hChildStdoutRead);
        TerminateProcess(pi.hProcess, (UINT)-1);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate pipe process");
    }

    p->hProcess = pi.hProcess;
    p->hThread = pi.hThread;
    p->hStdinWrite = hChildStdinWrite;
    p->hStdoutRead = hChildStdoutRead;

    int fd_write = _open_osfhandle((intptr_t)hChildStdinWrite, 0);
    int fd_read = _open_osfhandle((intptr_t)hChildStdoutRead, 0);

    p->to_child = (fd_write >= 0) ? _fdopen(fd_write, "w") : NULL;
    p->from_child = (fd_read >= 0) ? _fdopen(fd_read, "r") : NULL;

    if (!p->to_child || !p->from_child) {
        if (p->to_child) fclose(p->to_child);
        if (p->from_child) fclose(p->from_child);
        TerminateProcess(pi.hProcess, (UINT)-1);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        free(p);
        return cc_result_error(CC_ERR_PLATFORM, "Failed to fdopen pipe process handles");
    }

    setvbuf(p->to_child, NULL, _IONBF, 0);

    *out_pipe = p;
    return cc_result_ok();
}

/*
 * cc_process_pipe_write — 向管道子进程发送一行数据
 *
 * 通过 to_child 流（子进程的 stdin）写入一行数据，
 * 自动追加换行符并立即刷新缓冲区（无缓冲模式下刷新生效）。
 *
 * 参数：
 *   pipe — 管道进程句柄
 *   data — 要发送的字符串（不含换行符，函数会自动追加 '\n'）
 *
 * 返回值：
 *   成功返回 cc_result_ok()，失败返回 CC_ERR_INVALID_ARGUMENT 或 CC_ERR_PLATFORM
 *
 * 注意事项：
 *   - 每次写入后检查 ferror 状态，如果子进程已退出或管道已关闭则返回错误
 *   - clearerr 用于清除错误标志（错误已上报，重置以允许后续调用）
 */
cc_result_t cc_process_pipe_write(cc_process_pipe_t *pipe, const char *data)
{
    if (!pipe || !pipe->to_child || !data)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid pipe write arguments");

    fprintf(pipe->to_child, "%s\n", data);
    fflush(pipe->to_child);

    if (ferror(pipe->to_child)) {
        clearerr(pipe->to_child);
        return cc_result_error(CC_ERR_PLATFORM, "Failed to write to pipe process");
    }

    return cc_result_ok();
}

/*
 * cc_process_pipe_read_line — 从管道子进程读取一行数据
 *
 * 通过 from_child 流（子进程的 stdout）阻塞读取一行数据，
 * 自动去除行尾的换行符（'\n' 和 '\r\n' 都正确处理）。
 *
 * 参数：
 *   pipe     — 管道进程句柄
 *   out_line — 输出参数，指向读取到的行内容（由调用者 free 释放）
 *
 * 返回值：
 *   成功返回 cc_result_ok()，失败返回 CC_ERR_INVALID_ARGUMENT 或 CC_ERR_PLATFORM
 *
 * 平台注意事项：
 *   - Windows 平台使用 getline() 读取（通过 MinGW/msvcrt 的兼容实现）
 *   - 自动处理 CRLF（'\r\n'）到 LF（'\n'）的转换
 *   - 读取失败（子进程关闭 stdout 或管道出错）时返回 CC_ERR_PLATFORM
 */
cc_result_t cc_process_pipe_read_line(cc_process_pipe_t *pipe, char **out_line)
{
    if (!pipe || !pipe->hStdoutRead || !out_line)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid pipe read arguments");

    size_t cap = 256;
    size_t len = 0;
    char *line = malloc(cap);
    if (!line) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate pipe line");

    DWORD elapsed = 0;
    while (1) {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe->hStdoutRead, NULL, 0, NULL, &available, NULL)) {
            free(line);
            return cc_result_error(CC_ERR_PLATFORM, "Pipe process stdout peek failed");
        }

        if (available == 0) {
            DWORD child_status = WaitForSingleObject(pipe->hProcess, 0);
            if (child_status == WAIT_OBJECT_0 && len == 0) {
                free(line);
                return cc_result_error(CC_ERR_PLATFORM, "Pipe process closed stdout");
            }
            if (elapsed >= 30000) {
                free(line);
                return cc_result_error(CC_ERR_TIMEOUT, "Timed out waiting for pipe process line");
            }
            Sleep(10);
            elapsed += 10;
            continue;
        }

        char ch;
        DWORD nread = 0;
        if (!ReadFile(pipe->hStdoutRead, &ch, 1, &nread, NULL) || nread == 0) {
            free(line);
            return cc_result_error(CC_ERR_PLATFORM, "Pipe process stdout read failed");
        }
        if (ch == '\n') break;
        if (len + 1 >= cap) {
            cap *= 2;
            char *next = realloc(line, cap);
            if (!next) {
                free(line);
                return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to grow pipe line");
            }
            line = next;
        }
        line[len++] = ch;
    }

    if (len > 0 && line[len - 1] == '\r') len--;
    line[len] = '\0';
    *out_line = line;
    return cc_result_ok();
}

/*
 * cc_process_pipe_stop — 停止管道子进程并关闭所有管道
 *
 * 先通过 TerminateProcess 强制终止子进程（类似 SIGKILL），
 * 等待最多 5 秒确认进程退出，然后关闭所有句柄和流。
 *
 * 参数：
 *   pipe — 管道进程句柄（可为 NULL，此时函数无操作）
 *
 * 与 POSIX 版本的区别：
 *   POSIX 版本先发送 SIGTERM 等待 100ms，再发送 SIGKILL。
 *   Windows 版本直接 TerminateProcess（等价于 SIGKILL），
 *   因为 Windows 没有优雅终止信号的机制，TerminateProcess 是唯一选择。
 */
void cc_process_pipe_stop(cc_process_pipe_t *pipe)
{
    if (!pipe) return;

    if (pipe->hProcess) {
        TerminateProcess(pipe->hProcess, (UINT)-1);
        WaitForSingleObject(pipe->hProcess, 5000);
        CloseHandle(pipe->hProcess);
        pipe->hProcess = NULL;
    }

    if (pipe->hThread) {
        CloseHandle(pipe->hThread);
        pipe->hThread = NULL;
    }

    if (pipe->to_child) {
        fclose(pipe->to_child);
        pipe->to_child = NULL;
    }
    if (pipe->from_child) {
        fclose(pipe->from_child);
        pipe->from_child = NULL;
    }
}

/*
 * cc_process_pipe_destroy — 销毁管道进程句柄
 *
 * 停止子进程（调用 cc_process_pipe_stop）并释放句柄结构体内存。
 * 对 NULL 指针安全（无操作）。
 *
 * 参数：
 *   pipe — 管道进程句柄（可为 NULL）
 */
void cc_process_pipe_destroy(cc_process_pipe_t *pipe)
{
    if (!pipe) return;
    cc_process_pipe_stop(pipe);
    free(pipe);
}

#endif
