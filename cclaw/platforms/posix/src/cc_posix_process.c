



#include "cc/app/cc_cancel_token.h"
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

/*
 * 子进程输出捕获缓冲。
 *
 * stdout/stderr 通过非阻塞 pipe 分块读入这里，data 成功后直接转交给 out_result，由调用方
 * 使用 cc_process_result_free 释放。
 */
typedef struct {
    char *data;
    size_t total;
    size_t cap;
} capture_buffer_t;

/* 简单 10ms 轮询等待，用于 waitpid(WNOHANG) 与 pipe drain 的循环节拍。 */
static void sleep_10ms(void)
{
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 10 * 1000 * 1000;
    nanosleep(&ts, NULL);
}

/* 将 fd 设置为非阻塞，避免读 pipe 时因为子进程尚未输出而卡住整个 runtime。 */
static void set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

/*
 * 追加捕获数据。
 *
 * 缓冲按 2 倍扩容，并始终保持 NUL 结尾，方便上层把输出当 C 字符串处理。
 */
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

/*
 * 尽可能读取一个非阻塞 fd 中的所有当前数据。
 *
 * EAGAIN/EWOULDBLOCK 表示暂时没有更多数据，不是错误；EINTR 继续重试。
 */
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

/* 释放进程结果里的 stdout/stderr 文本，并清零结构。 */
void cc_process_result_free(cc_process_result_t *result)
{
    if (!result) return;
    free(result->stdout_text);
    free(result->stderr_text);
    memset(result, 0, sizeof(cc_process_result_t));
}

/*
 * 执行一次性子进程命令。
 *
 * 该实现使用 fork/exec，按选项捕获 stdout/stderr，timeout 后 SIGKILL 子进程。返回 OK
 * 表示平台调用完成，命令业务失败通过 out_result->exit_code 表达。
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



/*
 * 长生命周期管道进程状态。
 *
 * stdin/stdout 用 FILE* 便于按行写读，stderr 只保留 fd 方便未来扩展。pid 用于 stop/destroy
 * 时终止子进程。
 */
struct cc_process_pipe {
    pid_t pid;
    int stdin_fd;
    int stdout_fd;
    int stderr_fd;
    FILE *to_child;
    FILE *from_child;
};

/*
 * 启动可交互的管道进程。
 *
 * 父进程保留子进程 stdin/stdout/stderr 的管道端，stdout 设置为非阻塞；适合 MCP/plugin
 * JSON-RPC 这类一行一帧的长期进程通信。
 */
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

/*
 * 向管道进程写一行。
 *
 * data 后自动追加换行并刷新，匹配 JSONL/RPC line protocol。写失败返回平台错误。
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
 * 带 timeout 和取消 token 读取一行 stdout。
 *
 * select 每 50ms 最多等待一次，以便及时检查 cancel_token；返回的 out_line 由调用方 free。
 * 遇到 EOF 且已有部分数据时返回该行，完全 EOF 则返回平台错误。
 */
cc_result_t cc_process_pipe_read_line_timeout_cancel(
    cc_process_pipe_t *pipe,
    int timeout_ms,
    cc_cancel_token_t *cancel_token,
    char **out_line
)
{
    if (!pipe || pipe->stdout_fd < 0 || !out_line)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid pipe read arguments");
    if (timeout_ms <= 0) timeout_ms = 30000;

    size_t cap = 256;
    size_t len = 0;
    char *line = malloc(cap);
    if (!line) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate pipe line");

    int waited_ms = 0;
    while (1) {
        if (cc_cancel_token_is_cancelled(cancel_token)) {
            free(line);
            return cc_result_error(CC_ERR_CANCELLED, "Pipe process read cancelled");
        }

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(pipe->stdout_fd, &readfds);

        int wait_ms = timeout_ms - waited_ms;
        if (wait_ms > 50) wait_ms = 50;
        if (wait_ms <= 0) {
            free(line);
            return cc_result_error(CC_ERR_TIMEOUT, "Timed out waiting for pipe process line");
        }

        struct timeval tv;
        tv.tv_sec = wait_ms / 1000;
        tv.tv_usec = (wait_ms % 1000) * 1000;

        int ready = select(pipe->stdout_fd + 1, &readfds, NULL, NULL, &tv);
        if (ready < 0) {
            if (errno == EINTR) continue;
            free(line);
            return cc_result_error(CC_ERR_PLATFORM, "Pipe process stdout select failed");
        }
        if (ready == 0) {
            waited_ms += wait_ms;
            continue;
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

/* 不带取消 token 的 timeout 读行 wrapper。 */
cc_result_t cc_process_pipe_read_line_timeout(
    cc_process_pipe_t *pipe,
    int timeout_ms,
    char **out_line
)
{
    return cc_process_pipe_read_line_timeout_cancel(pipe, timeout_ms, NULL, out_line);
}

/* 默认 30 秒超时读行 wrapper。 */
cc_result_t cc_process_pipe_read_line(cc_process_pipe_t *pipe, char **out_line)
{
    return cc_process_pipe_read_line_timeout(pipe, 30000, out_line);
}

/*
 * 停止管道进程并关闭管道端。
 *
 * 先 SIGTERM 给子进程一点退出机会，再 SIGKILL 兜底；随后关闭 FILE 指针和 fd，函数可重复调用。
 */
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

/* 停止并释放管道进程对象。 */
void cc_process_pipe_destroy(cc_process_pipe_t *pipe)
{
    if (!pipe) return;
    cc_process_pipe_stop(pipe);
    free(pipe);
}
