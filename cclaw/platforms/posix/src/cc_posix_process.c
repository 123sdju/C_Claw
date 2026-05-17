/**
 * 学习导读：cclaw/platforms/posix/src/cc_posix_process.c
 *
 * 所属层次：平台层。
 * 阅读重点：这里隐藏 POSIX、Windows、ESP32 的系统 API 差异，阅读时重点看同名端口函数如何按平台实现。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * cc_posix_process.c — POSIX 进程管理与子进程执行
 *
 * 在整体架构中的角色和层次：
 *   本模块位于 Platform 层的 POSIX 平台实现子层。
 *   Platform 层是整个系统的最底层，负责封装操作系统差异。
 *   本文件是 cc_process.h 接口在 POSIX（Linux/macOS/BSD/Unix）平台的
 *   具体实现，向上层（如 Sandbox 沙箱模块、Agent 工具执行模块）提供
 *   外部进程的创建、执行、输出捕获和超时控制能力。
 *   调用者通过统一的 cc_process_options_t 和 cc_process_result_t 接口
 *   操作，无需关心底层是 fork/exec 还是其他平台机制。
 *
 * 核心流程（fork/exec 模式）：
 *   1. 父进程通过 pipe() 创建 stdout/stderr 管道
 *   2. fork() 创建子进程
 *   3. 子进程：dup2() 将管道写端重定向到 STDOUT_FILENO/STDERR_FILENO
 *   4. 子进程：execvp()/execlp() 执行目标程序，用 _exit(127) 兜底
 *   5. 父进程：关闭管道写端，WNOHANG 轮询 waitpid() 等待子进程结束
 *   6. 超时检测：超过 timeout_ms 后 kill(SIGKILL) 强制终止子进程
 *   7. 父进程：从管道读端收集子进程输出，组装 cc_process_result_t
 *
 * 关键特性：
 *   - fork/exec 模式启动子进程，与主进程内存空间完全隔离
 *   - 通过匿名管道异步捕获标准输出和标准错误
 *   - 可配置超时（毫秒级），超时后 SIGKILL 强制终止子进程
 *   - 支持设置工作目录（chdir）和环境变量（setenv）
 *   - 以轮询（WNOHANG + usleep 10ms）方式实现超时等待
 *
 * 设计决策：
 *   - 选择 WNOHANG 轮询而非信号驱动的 SIGCHLD：信号处理在复杂系统中
 *     容易出错（重入问题、信号丢失），轮询方式更简单可靠
 *   - 超时精度 ±10ms 是性能和精度的折中：1ms 轮询会带来过高 CPU 开销
 *   - 使用 SIGKILL（而非 SIGTERM）：确保子进程被强制终止，避免僵尸进程
 *   - 输出读取在子进程结束后进行：因为管道缓冲区有限（通常 64KB），
 *     如果子进程输出超过缓冲区且父进程未读取，子进程将被阻塞
 *   - args 为 NULL 时通过 /bin/sh -c 执行：支持 shell 管道、重定向等语法
 *
 * 平台依赖（POSIX 特有，不可移植到 Windows）：
 *   - fork() / execvp() / execlp() — 进程创建与替换
 *   - pipe() / dup2() — 匿名管道与文件描述符重定向
 *   - waitpid() / kill() — 进程等待与信号发送
 *   - usleep() — 微秒级休眠（超时轮询间隔）
 */

#include "cc/ports/cc_process.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sys/select.h>

typedef struct {
    char *data;
    size_t total;
    size_t cap;
} capture_buffer_t;

/* 学习注释：sleep_10ms 是本文件内部辅助函数。
 * 阅读时先看它服务哪个 public API，再看它如何处理边界条件和资源释放。 */
static void sleep_10ms(void)
{
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 10 * 1000 * 1000;
    nanosleep(&ts, NULL);
}

/* 学习注释：set_nonblocking 是本文件内部辅助函数。
 * 阅读时先看它服务哪个 public API，再看它如何处理边界条件和资源释放。 */
static void set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

/* 学习注释：capture_append 是本文件内部辅助函数。
 * 阅读时先看它服务哪个 public API，再看它如何处理边界条件和资源释放。 */
static int capture_append(capture_buffer_t *capture, const char *buf, size_t len)
{
    if (!capture || len == 0) return 1;

    if (capture->total + len + 1 > capture->cap) {
        size_t new_cap = capture->cap ? capture->cap : 4096;
        while (capture->total + len + 1 > new_cap) {
            new_cap *= 2;
        }

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

/* 学习注释：drain_fd 是本文件内部辅助函数。
 * 阅读时先看它服务哪个 public API，再看它如何处理边界条件和资源释放。 */
static void drain_fd(int fd, capture_buffer_t *capture)
{
    char buf[4096];
    while (1) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            if (!capture_append(capture, buf, (size_t)n)) return;
            continue;
        }
        if (n == 0) return;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        return;
    }
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
 */
void cc_process_result_free(cc_process_result_t *result)
{
    if (!result) return;
    free(result->stdout_text);
    free(result->stderr_text);
    memset(result, 0, sizeof(cc_process_result_t));
}

/*
 * cc_process_run — 执行外部进程并捕获输出
 *
 * 使用 POSIX fork/exec 创建子进程，可选捕获其标准输出和标准错误。
 * 支持超时控制：在 timeout_ms 内轮询等待子进程结束，超时后发送
 * SIGKILL 强制终止。
 *
 * 参数：
 *   options    — 进程执行选项，包含命令、参数、环境变量、超时等配置
 *     options->command       — 要执行的命令字符串
 *     options->args          — 参数数组（可选，为 NULL 时通过 sh -c 执行）
 *     options->working_dir   — 工作目录（可选，为 NULL 时继承父进程）
 *     options->env           — 环境变量数组（可选，格式 "KEY=VALUE"）
 *     options->capture_stdout — 是否捕获标准输出
 *     options->capture_stderr — 是否捕获标准错误
 *     options->timeout_ms    — 超时时间（毫秒），0 或负数表示无限等待
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
 *   1. 父进程创建管道后 fork，子进程将管道写端 dup2 到 STDOUT_FILENO/STDERR_FILENO
 *   2. 父进程关闭管道写端，通过轮询 waitpid(WNOHANG) 等待子进程结束
 *   3. 超时后发送 SIGKILL，确保子进程被强制终止
 *   4. 子进程结束后，从管道读端读取全部输出数据
 *
 * 平台注意事项：
 *   - fork() 在内存受限环境下可能失败（ENOMEM）
 *   - 管道缓冲区有限（通常 64KB），子进程输出超过缓冲区会阻塞等待父进程读取
 *   - SIGKILL 不可被捕获，子进程无清理机会
 *   - usleep(10000) 即 10ms 轮询间隔，超时精度约 ±10ms
 *   - 当 args 为 NULL 或空时，通过 /bin/sh -c 执行命令，支持 shell 语法
 */
cc_result_t cc_process_run(
    const cc_process_options_t *options,
    cc_process_result_t *out_result
)
{
    memset(out_result, 0, sizeof(cc_process_result_t));

    int stdout_pipe[2] = {-1, -1};
    int stderr_pipe[2] = {-1, -1};

    if (options->capture_stdout && pipe(stdout_pipe) != 0)
        return cc_result_error(CC_ERR_PLATFORM, "Failed to create stdout pipe");

    if (options->capture_stderr && pipe(stderr_pipe) != 0) {
        if (options->capture_stdout) { close(stdout_pipe[0]); close(stdout_pipe[1]); }
        return cc_result_error(CC_ERR_PLATFORM, "Failed to create stderr pipe");
    }

    pid_t pid = fork();
    if (pid < 0) {
        if (options->capture_stdout) { close(stdout_pipe[0]); close(stdout_pipe[1]); }
        if (options->capture_stderr) { close(stderr_pipe[0]); close(stderr_pipe[1]); }
        return cc_result_error(CC_ERR_PLATFORM, "Failed to fork");
    }

    if (pid == 0) {
        if (options->capture_stdout) {
            close(stdout_pipe[0]);
            dup2(stdout_pipe[1], STDOUT_FILENO);
            close(stdout_pipe[1]);
        }
        if (options->capture_stderr) {
            close(stderr_pipe[0]);
            dup2(stderr_pipe[1], STDERR_FILENO);
            close(stderr_pipe[1]);
        }

        if (options->working_dir)
            chdir(options->working_dir);

        if (options->env) {
            for (char **e = options->env; *e; e++) {
                char *eq = strchr(*e, '=');
                if (eq) {
                    *eq = '\0';
                    setenv(*e, eq + 1, 1);
                    *eq = '=';
                }
            }
        }

        if (options->args && options->args[0]) {
            execvp(options->command, options->args);
        } else {
            execlp("/bin/sh", "sh", "-c", options->command, NULL);
        }
        _exit(127);
    }

    if (options->capture_stdout) {
        close(stdout_pipe[1]);
        set_nonblocking(stdout_pipe[0]);
    }
    if (options->capture_stderr) {
        close(stderr_pipe[1]);
        set_nonblocking(stderr_pipe[0]);
    }

    capture_buffer_t stdout_capture = {0};
    capture_buffer_t stderr_capture = {0};

    int timed_out = 0;
    int status = 0;
    int waited = 0;
    int child_done = 0;

    while (!child_done) {
        if (options->capture_stdout) drain_fd(stdout_pipe[0], &stdout_capture);
        if (options->capture_stderr) drain_fd(stderr_pipe[0], &stderr_capture);

        pid_t result = waitpid(pid, &status, WNOHANG);
        if (result == pid) {
            if (WIFEXITED(status)) {
                out_result->exit_code = WEXITSTATUS(status);
            } else if (WIFSIGNALED(status)) {
                out_result->exit_code = -WTERMSIG(status);
            } else {
                out_result->exit_code = -1;
            }
            child_done = 1;
            break;
        }

        if (result < 0 && errno != EINTR) {
            out_result->exit_code = -1;
            child_done = 1;
            break;
        }

        if (options->timeout_ms > 0 && waited >= options->timeout_ms) {
            timed_out = 1;
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            out_result->exit_code = -1;
            child_done = 1;
            break;
        }

        sleep_10ms();
        waited += 10;
    }

    out_result->timed_out = timed_out;

    if (options->capture_stdout) {
        drain_fd(stdout_pipe[0], &stdout_capture);
        out_result->stdout_text = stdout_capture.data ? stdout_capture.data : strdup("");
        close(stdout_pipe[0]);
    }

    if (options->capture_stderr) {
        drain_fd(stderr_pipe[0], &stderr_capture);
        out_result->stderr_text = stderr_capture.data ? stderr_capture.data : strdup("");
        close(stderr_pipe[0]);
    }

    return cc_result_ok();
}

/* ───────────────────────────────────────────────────────────────────
 *  持久管道进程实现（从 cc_plugin_process.c 提取）
 * ─────────────────────────────────────────────────────────────────── */

struct cc_process_pipe {
    pid_t pid;
    int stdin_fd;
    int stdout_fd;
    int stderr_fd;
    FILE *to_child;
    FILE *from_child;
};

/* 学习注释：cc_process_pipe_spawn 是对外可见或跨模块调用的入口。
 * 阅读时重点确认参数校验、所有权转移、错误码和清理路径是否成对出现。 */
cc_result_t cc_process_pipe_spawn(
    const char *command,
    char *const argv[],
    cc_process_pipe_t **out_pipe
)
{
    signal(SIGPIPE, SIG_IGN);

    int to_child[2] = {-1, -1};
    int from_child[2] = {-1, -1};
    int err_child[2] = {-1, -1};

    if (pipe(to_child) < 0 || pipe(from_child) < 0 || pipe(err_child) < 0) {
        if (to_child[0] >= 0)  { close(to_child[0]); close(to_child[1]); }
        if (from_child[0] >= 0){ close(from_child[0]); close(from_child[1]); }
        if (err_child[0] >= 0) { close(err_child[0]); close(err_child[1]); }
        return cc_result_error(CC_ERR_PLATFORM, "Failed to create pipes for pipe process");
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(to_child[0]); close(to_child[1]);
        close(from_child[0]); close(from_child[1]);
        close(err_child[0]); close(err_child[1]);
        return cc_result_error(CC_ERR_PLATFORM, "Failed to fork pipe process");
    }

    if (pid == 0) {
        dup2(to_child[0], STDIN_FILENO);
        dup2(from_child[1], STDOUT_FILENO);
        dup2(err_child[1], STDERR_FILENO);
        close(to_child[0]); close(to_child[1]);
        close(from_child[0]); close(from_child[1]);
        close(err_child[0]); close(err_child[1]);
        execvp(command, argv);
        _exit(127);
    }

    close(to_child[0]);
    close(from_child[1]);
    close(err_child[1]);

    cc_process_pipe_t *p = calloc(1, sizeof(cc_process_pipe_t));
    if (!p) {
        close(to_child[1]);
        close(from_child[0]);
        close(err_child[0]);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate pipe process");
    }

    p->pid = pid;
    p->stdin_fd = to_child[1];
    p->stdout_fd = from_child[0];
    p->stderr_fd = err_child[0];
    set_nonblocking(p->stdout_fd);

    p->to_child = fdopen(to_child[1], "w");
    p->from_child = fdopen(from_child[0], "r");

    if (!p->to_child || !p->from_child) {
        if (p->to_child) fclose(p->to_child); else close(to_child[1]);
        if (p->from_child) fclose(p->from_child); else close(from_child[0]);
        close(err_child[0]);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        free(p);
        return cc_result_error(CC_ERR_PLATFORM, "Failed to fdopen pipe process pipes");
    }

    setvbuf(p->to_child, NULL, _IONBF, 0);

    *out_pipe = p;
    return cc_result_ok();
}

/* 学习注释：cc_process_pipe_write 是对外可见或跨模块调用的入口。
 * 阅读时重点确认参数校验、所有权转移、错误码和清理路径是否成对出现。 */
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

/* 学习注释：cc_process_pipe_read_line 是对外可见或跨模块调用的入口。
 * 阅读时重点确认参数校验、所有权转移、错误码和清理路径是否成对出现。 */
cc_result_t cc_process_pipe_read_line(cc_process_pipe_t *pipe, char **out_line)
{
    if (!pipe || pipe->stdout_fd < 0 || !out_line)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid pipe read arguments");

    size_t cap = 256;
    size_t len = 0;
    char *line = malloc(cap);
    if (!line) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate pipe line");

    while (1) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(pipe->stdout_fd, &readfds);

        struct timeval tv;
        tv.tv_sec = 30;
        tv.tv_usec = 0;

        int ready = select(pipe->stdout_fd + 1, &readfds, NULL, NULL, &tv);
        if (ready < 0) {
            if (errno == EINTR) continue;
            free(line);
            return cc_result_error(CC_ERR_PLATFORM, "Pipe process stdout select failed");
        }
        if (ready == 0) {
            free(line);
            return cc_result_error(CC_ERR_TIMEOUT, "Timed out waiting for pipe process line");
        }

        char ch;
        ssize_t nread = read(pipe->stdout_fd, &ch, 1);
        if (nread < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            free(line);
            return cc_result_error(CC_ERR_PLATFORM, "Pipe process stdout read failed");
        }
        if (nread == 0) {
            if (len == 0) {
                free(line);
                return cc_result_error(CC_ERR_PLATFORM, "Pipe process closed stdout");
            }
            break;
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

    if (len > 0 && line[len - 1] == '\r') {
        len--;
    }
    line[len] = '\0';

    *out_line = line;
    return cc_result_ok();
}

/* 学习注释：cc_process_pipe_stop 是对外可见或跨模块调用的入口。
 * 阅读时重点确认参数校验、所有权转移、错误码和清理路径是否成对出现。 */
void cc_process_pipe_stop(cc_process_pipe_t *pipe)
{
    if (!pipe) return;

    if (pipe->pid > 0) {
        kill(pipe->pid, SIGTERM);
        struct timespec ts = {0, 100000000};
        nanosleep(&ts, NULL);
        kill(pipe->pid, SIGKILL);
        waitpid(pipe->pid, NULL, 0);
        pipe->pid = 0;
    }

    if (pipe->to_child) {
        fclose(pipe->to_child);
        pipe->to_child = NULL;
    }
    if (pipe->from_child) {
        fclose(pipe->from_child);
        pipe->from_child = NULL;
    }
    if (pipe->stderr_fd >= 0) {
        close(pipe->stderr_fd);
        pipe->stderr_fd = -1;
    }
}

/* 学习注释：cc_process_pipe_destroy 是对外可见或跨模块调用的入口。
 * 阅读时重点确认参数校验、所有权转移、错误码和清理路径是否成对出现。 */
void cc_process_pipe_destroy(cc_process_pipe_t *pipe)
{
    if (!pipe) return;
    cc_process_pipe_stop(pipe);
    free(pipe);
}
