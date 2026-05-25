/**
 * 学习导读：apps/posix/cli/src/cc_cli_gateway.c
 *
 * 所属层次：POSIX CLI 应用层。
 * 阅读重点：这里组装桌面 CLI、工具、插件和 sandbox，阅读时重点看 main 到 runtime builder 的组合流程。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

#define _POSIX_C_SOURCE 200809L

/******************************************************************************
 * cc_cli_gateway.c — CLI 交互入口模块
 *
 * 本模块是 c-claw 的命令行交互界面（Gateway），负责接收用户输入并转发给
 * Agent Runtime 处理。支持两种运行模式：
 *   - 交互式聊天循环（默认模式）：持续接收用户输入，逐条处理并显示回复
 *   - 单次问答模式（'ask' 子命令）：接收一次查询，输出结果后退出
 *
 * 组件装配关系（依赖注入）：
 *   本模块作为 Gateway 适配器，由 main.c 在完成所有组件装配后调用。
 *   cc_agent_runtime_t（包含 LLM、工具注册表、存储、策略引擎、沙箱等全部
 *   依赖）通过参数注入，Gateway 本身不持有任何组件的所有权。
 *****************************************************************************/

#include "cc/app/cc_agent_runtime.h"
#include "cc/app/cc_agent_manager.h"
#include "cc/app/cc_runtime_builder.h"
#include "cc/app/cc_session_manager.h"
#include "cc/util/cc_config.h"
#include "cc/util/cc_json.h"
#include "cc/ports/cc_logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <termios.h>
#include <unistd.h>
#include <sys/stat.h>

/* 当前会话 ID，基于时间戳生成，全局生命周期内唯一 */
static char *g_session_id = NULL;

#define HISTORY_MAX 100
#define CLI_PROMPT "You> "
static char *history[HISTORY_MAX];
static int history_count = 0;
static int g_stream_mode = 0;

typedef struct cc_cli_reload_watcher {
    const char *config_path;
    time_t last_mtime;
    long last_reload_ms;
    int initialized;
} cc_cli_reload_watcher_t;

static const char *slash_commands[] = {
    "/exit",
    "/quit",
    "/tools",
    "/reload",
    "/agents",
    "/agent",
    "/skills",
    "/mcp",
    "/interrupt",
    "/thinking",
    "/thinking on",
    "/thinking off",
    "/stream",
    "/stream on",
    "/stream off",
    "/debug",
    "/debug on",
    "/debug off",
    "/help",
    NULL
};

static long cli_now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return (long)time(NULL) * 1000L;
    return (long)ts.tv_sec * 1000L + (long)(ts.tv_nsec / 1000000L);
}

static int config_file_mtime(const char *path, time_t *out_mtime)
{
    if (!path || !path[0] || !out_mtime) return 0;
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    *out_mtime = st.st_mtime;
    return 1;
}

static int config_auto_reload_enabled(const cc_config_t *config)
{
    if (!config) return 0;
    return config->plugins.hot_reload || config->skills.watch;
}

static int config_reload_debounce_ms(const cc_config_t *config)
{
    int plugin_ms = config ? config->plugins.reload_debounce_ms : 300;
    int skill_ms = config ? config->skills.watch_debounce_ms : 250;
    int debounce = plugin_ms > skill_ms ? plugin_ms : skill_ms;
    return debounce > 0 ? debounce : 250;
}

static void print_runtime_diagnostics(
    const char *prefix,
    const cc_runtime_diagnostics_t *diagnostics
)
{
    if (!diagnostics || diagnostics->count == 0) return;
    printf("%s\n", prefix ? prefix : "[diagnostics] 外部工具加载问题:");
    for (size_t i = 0; i < diagnostics->count; i++) {
        const cc_runtime_diagnostic_t *item = &diagnostics->items[i];
        printf("  - [%s] %s: %s\n",
            item->kind[0] ? item->kind : "tool",
            item->id[0] ? item->id : "(unknown)",
            item->message[0] ? item->message : "unavailable");
    }
    if (diagnostics->truncated) {
        printf("  - ... 还有更多诊断被截断\n");
    }
    printf("\n");
}

static cc_result_t reload_runtime_from_path(
    cc_runtime_builder_t *builder,
    cc_config_t *config,
    const char *config_path,
    cc_runtime_reload_report_t *out_report
)
{
    cc_config_t next_config;
    memset(&next_config, 0, sizeof(next_config));
    cc_result_t rc = cc_config_load(config_path, &next_config);
    if (rc.code != CC_OK) {
        cc_config_destroy(&next_config);
        return rc;
    }

    rc = cc_runtime_builder_reload_with_report(builder, &next_config, out_report);
    if (rc.code != CC_OK) {
        cc_config_destroy(&next_config);
        return rc;
    }

    cc_config_destroy(config);
    *config = next_config;
    memset(&next_config, 0, sizeof(next_config));
    g_stream_mode = config->stream_mode ? 1 : 0;
    if (config->debug_mode) setenv("CCLAW_DEBUG", "1", 1);
    return cc_result_ok();
}

static void reload_watcher_init(
    cc_cli_reload_watcher_t *watcher,
    const char *config_path
)
{
    if (!watcher) return;
    memset(watcher, 0, sizeof(*watcher));
    watcher->config_path = config_path;
    watcher->last_reload_ms = cli_now_ms();
    if (config_file_mtime(config_path, &watcher->last_mtime)) {
        watcher->initialized = 1;
    }
}

static void reload_watcher_note_current(
    cc_cli_reload_watcher_t *watcher
)
{
    if (!watcher) return;
    time_t mtime = 0;
    if (config_file_mtime(watcher->config_path, &mtime)) {
        watcher->last_mtime = mtime;
        watcher->initialized = 1;
    }
    watcher->last_reload_ms = cli_now_ms();
}

static void reload_watcher_poll(
    cc_cli_reload_watcher_t *watcher,
    cc_runtime_builder_t *builder,
    cc_config_t *config
)
{
    if (!watcher || !builder || !config || !config_auto_reload_enabled(config)) return;
    time_t mtime = 0;
    if (!config_file_mtime(watcher->config_path, &mtime)) return;
    if (!watcher->initialized) {
        watcher->last_mtime = mtime;
        watcher->initialized = 1;
        return;
    }
    if (mtime == watcher->last_mtime) return;
    long now = cli_now_ms();
    if (now - watcher->last_reload_ms < config_reload_debounce_ms(config)) return;

    cc_runtime_reload_report_t report;
    cc_result_t rc = reload_runtime_from_path(builder, config, watcher->config_path, &report);
    if (rc.code == CC_OK) {
        watcher->last_mtime = mtime;
        watcher->last_reload_ms = now;
        printf("\n[reload] config.json changed; runtime generation updated\n\n");
        print_runtime_diagnostics("[reload] 部分外部工具不可用:", &report.diagnostics);
    } else {
        watcher->last_reload_ms = now;
        printf("\n[reload] 自动热重载失败: %s\n\n", rc.message ? rc.message : "unknown");
        cc_result_free(&rc);
    }
}

/**
 * history_add — 把用户输入追加到 CLI 历史数组，并在容量满时移除最旧记录。
 *
 * @param line 借用的只读字符串；函数不会释放该指针。
 */
static void history_add(const char *line)
{
    if (!line || !*line) return;
    if (history_count > 0 && strcmp(history[history_count - 1], line) == 0) return;
    if (history_count >= HISTORY_MAX) {
        free(history[0]);
        memmove(history, history + 1, (HISTORY_MAX - 1) * sizeof(char *));
        history_count = HISTORY_MAX - 1;
    }
    history[history_count++] = strdup(line);
}

/**
 * utf8_prev_index — 按 UTF-8 字节序列移动光标下标，避免在多字节字符中间删除或移动。
 *
 * @param buf 借用的只读字符串；函数不会释放该指针。
 * @param cursor 按值传入，用于控制本次操作。
 * @return 返回字节数、元素数或下标；不会转移任何指针所有权。
 */
static size_t utf8_prev_index(const char *buf, size_t cursor)
{
    if (cursor == 0) return 0;
    cursor--;
    while (cursor > 0 && ((unsigned char)buf[cursor] & 0xC0) == 0x80) {
        cursor--;
    }
    return cursor;
}

/**
 * utf8_next_index — 按 UTF-8 字节序列移动光标下标，避免在多字节字符中间删除或移动。
 *
 * @param buf 借用的只读字符串；函数不会释放该指针。
 * @param cursor 按值传入，用于控制本次操作。
 * @param len 按值传入，用于控制本次操作。
 * @return 返回字节数、元素数或下标；不会转移任何指针所有权。
 */
static size_t utf8_next_index(const char *buf, size_t cursor, size_t len)
{
    if (cursor >= len) return len;
    cursor++;
    while (cursor < len && ((unsigned char)buf[cursor] & 0xC0) == 0x80) {
        cursor++;
    }
    return cursor;
}

/**
 * utf8_decode_one — 从缓冲区当前位置解码一个 UTF-8 codepoint，并返回实际消耗的字节数。
 *
 * @param s 借用的只读字符串；函数不会释放该指针。
 * @param len 按值传入，用于控制本次操作。
 */
static unsigned int utf8_decode_one(const unsigned char *s, size_t len, size_t *consumed)
{
    if (len == 0) {
        *consumed = 0;
        return 0;
    }
    if (s[0] < 0x80) {
        *consumed = 1;
        return s[0];
    }
    if ((s[0] & 0xE0) == 0xC0 && len >= 2) {
        *consumed = 2;
        return ((unsigned int)(s[0] & 0x1F) << 6) |
               (unsigned int)(s[1] & 0x3F);
    }
    if ((s[0] & 0xF0) == 0xE0 && len >= 3) {
        *consumed = 3;
        return ((unsigned int)(s[0] & 0x0F) << 12) |
               ((unsigned int)(s[1] & 0x3F) << 6) |
               (unsigned int)(s[2] & 0x3F);
    }
    if ((s[0] & 0xF8) == 0xF0 && len >= 4) {
        *consumed = 4;
        return ((unsigned int)(s[0] & 0x07) << 18) |
               ((unsigned int)(s[1] & 0x3F) << 12) |
               ((unsigned int)(s[2] & 0x3F) << 6) |
               (unsigned int)(s[3] & 0x3F);
    }
    *consumed = 1;
    return s[0];
}

/**
 * codepoint_display_width — 估算 Unicode codepoint 在终端中的显示宽度，用于重绘光标位置。
 *
 * @param cp 按值传入，用于控制本次操作。
 */
static int codepoint_display_width(unsigned int cp)
{
    if (cp == 0) return 0;
    if (cp < 32 || (cp >= 0x7F && cp < 0xA0)) return 0;
    if ((cp >= 0x0300 && cp <= 0x036F) ||
        (cp >= 0x1AB0 && cp <= 0x1AFF) ||
        (cp >= 0x1DC0 && cp <= 0x1DFF) ||
        (cp >= 0x20D0 && cp <= 0x20FF) ||
        (cp >= 0xFE20 && cp <= 0xFE2F)) {
        return 0;
    }
    if ((cp >= 0x1100 && cp <= 0x115F) ||
        (cp >= 0x2E80 && cp <= 0xA4CF) ||
        (cp >= 0xAC00 && cp <= 0xD7A3) ||
        (cp >= 0xF900 && cp <= 0xFAFF) ||
        (cp >= 0xFE10 && cp <= 0xFE19) ||
        (cp >= 0xFE30 && cp <= 0xFE6F) ||
        (cp >= 0xFF00 && cp <= 0xFF60) ||
        (cp >= 0xFFE0 && cp <= 0xFFE6)) {
        return 2;
    }
    return 1;
}

/**
 * display_width_prefix — 计算字符串前缀在终端中的显示宽度，支撑多字节输入的行重绘。
 *
 * @param buf 借用的只读字符串；函数不会释放该指针。
 * @param bytes 按值传入，用于控制本次操作。
 * @return 返回字节数、元素数或下标；不会转移任何指针所有权。
 */
static size_t display_width_prefix(const char *buf, size_t bytes)
{
    size_t i = 0;
    size_t width = 0;
    while (i < bytes && buf[i]) {
        size_t consumed = 0;
        unsigned int cp = utf8_decode_one((const unsigned char *)buf + i, bytes - i, &consumed);
        if (consumed == 0) break;
        width += (size_t)codepoint_display_width(cp);
        i += consumed;
    }
    return width;
}

/**
 * redraw_line — 根据当前输入缓冲区和光标位置重绘 CLI 提示行。
 *
 * @param buf 借用的只读字符串；函数不会释放该指针。
 * @param cursor 按值传入，用于控制本次操作。
 */
static void redraw_line(const char *buf, size_t cursor)
{
    printf("\r\033[K%s%s", CLI_PROMPT, buf);
    if (buf[0] == '/') {
        printf("  [Tab补全: /exit /quit /tools /reload /agents /agent /skills /mcp /interrupt /thinking /stream /debug /help]");
    }
    printf("\r%s", CLI_PROMPT);
    size_t columns = display_width_prefix(buf, cursor);
    if (columns > 0) {
        printf("\033[%zuC", columns);
    }
    fflush(stdout);
}

/**
 * tab_complete — 根据当前缓冲区内容执行内置命令或路径补全。
 *
 * @param buf 可写缓冲区或字符串指针；函数可能就地修改内容但不释放缓冲区本身。
 * @param size 按值传入，用于控制本次操作。
 */
static int tab_complete(char *buf, size_t *len, size_t *cursor, size_t size)
{
    if (*cursor != *len) return 0;
    if (buf[0] != '/') return 0;

    const char *matches[16];
    int match_count = 0;
    for (int i = 0; slash_commands[i]; i++) {
        if (strncmp(slash_commands[i], buf, *len) == 0) {
            matches[match_count++] = slash_commands[i];
            if (match_count >= 16) break;
        }
    }

    if (match_count == 0) return 0;

    if (match_count == 1) {
        size_t cmd_len = strlen(matches[0]);
        if (cmd_len < size) {
            strcpy(buf, matches[0]);
            *len = cmd_len;
            *cursor = cmd_len;
        }
        return 1;
    }

    printf("\n");
    for (int i = 0; i < match_count; i++) {
        printf("  %s\n", matches[i]);
    }
    printf("%s%s", CLI_PROMPT, buf);
    fflush(stdout);
    return 0;
}

/**
 * delete_before_cursor — 按 UTF-8 字符边界删除输入缓冲区中的一个字符并更新长度/光标。
 *
 * @param buf 可写缓冲区或字符串指针；函数可能就地修改内容但不释放缓冲区本身。
 */
static void delete_before_cursor(char *buf, size_t *len, size_t *cursor)
{
    if (*cursor == 0) return;
    size_t prev = utf8_prev_index(buf, *cursor);
    memmove(buf + prev, buf + *cursor, *len - *cursor + 1);
    *len -= (*cursor - prev);
    *cursor = prev;
}

/**
 * delete_at_cursor — 按 UTF-8 字符边界删除输入缓冲区中的一个字符并更新长度/光标。
 *
 * @param buf 可写缓冲区或字符串指针；函数可能就地修改内容但不释放缓冲区本身。
 * @param cursor 按值传入，用于控制本次操作。
 */
static void delete_at_cursor(char *buf, size_t *len, size_t cursor)
{
    if (cursor >= *len) return;
    size_t next = utf8_next_index(buf, cursor, *len);
    memmove(buf + cursor, buf + next, *len - next + 1);
    *len -= (next - cursor);
}

/**
 * generate_session_id — 生成 CLI 临时会话 ID 字符串，调用方负责释放。
 *
 * @return 新分配字符串；返回 NULL 表示分配或输入校验失败，调用方负责 free。
 */
static char *generate_session_id(void)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "ses_%ld", (long)time(NULL));
    return strdup(buf);
}

/**
 * is_yes_answer — 解析交互式审批输入。
 *
 * CLI 只接受 `y`/`yes`，并允许后面跟空白或换行。其他输入都按拒绝处理。
 * 函数只借用 line，不修改、不保存，也不负责提示文本；调用方负责把结果映射到
 * policy decision。
 */
static int is_yes_answer(const char *line)
{
    if (!line) return 0;
    while (*line == ' ' || *line == '\t') line++;
    if ((line[0] == 'y' || line[0] == 'Y') &&
        (line[1] == '\0' || line[1] == '\n' || line[1] == '\r' ||
         line[1] == ' ' || line[1] == '\t')) {
        return 1;
    }
    if ((line[0] == 'y' || line[0] == 'Y') &&
        (line[1] == 'e' || line[1] == 'E') &&
        (line[2] == 's' || line[2] == 'S') &&
        (line[3] == '\0' || line[3] == '\n' || line[3] == '\r' ||
         line[3] == ' ' || line[3] == '\t')) {
        return 1;
    }
    return 0;
}

/**
 * cli_tool_approval — 参与工具注册、工具调用或工具结果写回流程。
 *
 * @param tool_name 借用的只读字符串；函数不会释放该指针。
 * @param arguments_json 借用的只读字符串；函数不会释放该指针。
 * @param reason 借用的只读字符串；函数不会释放该指针。
 * @param user_data 回调上下文；函数只透传或临时读取，不取得所有权。
 */
static int cli_tool_approval(
    const char *tool_name,
    const char *arguments_json,
    const char *reason,
    void *user_data
)
{
    (void)user_data;
    char answer[32];
    printf("\n[Tool Approval]\n");
    printf("Tool: %s\n", tool_name ? tool_name : "(unknown)");
    if (reason && reason[0]) {
        printf("Reason: %s\n", reason);
    }
    printf("Arguments: %s\n", arguments_json ? arguments_json : "{}");
    printf("Approve tool call? [y/N]: ");
    fflush(stdout);

    if (!fgets(answer, sizeof(answer), stdin)) {
        printf("\n");
        return 0;
    }
    return is_yes_answer(answer);
}

/**
 * cli_readline — 从终端读取一行用户输入，处理历史、补全和基本编辑快捷键。
 *
 * @param buf 可写缓冲区或字符串指针；函数可能就地修改内容但不释放缓冲区本身。
 * @param size 按值传入，用于控制本次操作。
 */
static int cli_readline(char *buf, size_t size)
{
    if (size == 0) return -1;

    struct termios old_term, new_term;
    if (!isatty(STDIN_FILENO) || tcgetattr(STDIN_FILENO, &old_term) != 0) {
        if (!fgets(buf, size, stdin)) return -1;
        size_t len = strlen(buf);
        while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
            buf[--len] = '\0';
        }
        return 0;
    }

    new_term = old_term;
    new_term.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &new_term);

    size_t len = 0;
    size_t cursor = 0;
    memset(buf, 0, size);

    int hist_idx = history_count;
    char saved_buf[4096] = {0};

    printf("%s", CLI_PROMPT);
    fflush(stdout);

    int result = 0;
    while (1) {
        int c = getchar();
        if (c == EOF) { result = -1; break; }
        if (c == '\n' || c == '\r') { putchar('\n'); break; }

        if (c == '\t') {
            if (tab_complete(buf, &len, &cursor, size)) {
                hist_idx = history_count;
                redraw_line(buf, cursor);
            }
            continue;
        }

        if (c == 0x1B) {
            int c2 = getchar();
            if (c2 == '[') {
                int c3 = getchar();
                if (c3 == 'A') {
                    if (history_count == 0) continue;
                    if (hist_idx == history_count) {
                        strncpy(saved_buf, buf, sizeof(saved_buf) - 1);
                    }
                    if (hist_idx > 0) {
                        hist_idx--;
                        size_t hlen = strlen(history[hist_idx]);
                        if (hlen < size) {
                            strcpy(buf, history[hist_idx]);
                            len = hlen;
                            cursor = len;
                        }
                        redraw_line(buf, cursor);
                    }
                    continue;
                }
                if (c3 == 'B') {
                    if (history_count == 0) continue;
                    if (hist_idx < history_count) {
                        hist_idx++;
                        if (hist_idx == history_count) {
                            size_t slen = strlen(saved_buf);
                            if (slen < size) {
                                strcpy(buf, saved_buf);
                                len = slen;
                                cursor = len;
                            }
                            saved_buf[0] = '\0';
                        } else {
                            size_t hlen = strlen(history[hist_idx]);
                            if (hlen < size) {
                                strcpy(buf, history[hist_idx]);
                                len = hlen;
                                cursor = len;
                            }
                        }
                        redraw_line(buf, cursor);
                    }
                    continue;
                }
                if (c3 == 'C') {
                    cursor = utf8_next_index(buf, cursor, len);
                    redraw_line(buf, cursor);
                    continue;
                }
                if (c3 == 'D') {
                    cursor = utf8_prev_index(buf, cursor);
                    redraw_line(buf, cursor);
                    continue;
                }
                if (c3 == 'H') {
                    cursor = 0;
                    redraw_line(buf, cursor);
                    continue;
                }
                if (c3 == 'F') {
                    cursor = len;
                    redraw_line(buf, cursor);
                    continue;
                }
                if (c3 >= '0' && c3 <= '9') {
                    int c4 = getchar();
                    if (c4 == '~') {
                        if (c3 == '1' || c3 == '7') {
                            cursor = 0;
                            redraw_line(buf, cursor);
                        } else if (c3 == '4' || c3 == '8') {
                            cursor = len;
                            redraw_line(buf, cursor);
                        } else if (c3 == '3') {
                            delete_at_cursor(buf, &len, cursor);
                            hist_idx = history_count;
                            saved_buf[0] = '\0';
                            redraw_line(buf, cursor);
                        }
                    }
                    continue;
                }
            }
            continue;
        }

        if (c == 0x7F || c == '\b') {
            if (cursor > 0) {
                delete_before_cursor(buf, &len, &cursor);
                hist_idx = history_count;
                saved_buf[0] = '\0';
                redraw_line(buf, cursor);
            }
            continue;
        }
        if (c == 1) {
            cursor = 0;
            redraw_line(buf, cursor);
            continue;
        }
        if (c == 5) {
            cursor = len;
            redraw_line(buf, cursor);
            continue;
        }
        if (c < 32) continue;

        if (len + 1 >= size) continue;
        memmove(buf + cursor + 1, buf + cursor, len - cursor + 1);
        buf[cursor++] = (char)c;
        len++;
        hist_idx = history_count;
        saved_buf[0] = '\0';
        redraw_line(buf, cursor);
    }

    tcsetattr(STDIN_FILENO, TCSANOW, &old_term);
    return result;
}

/**
 * run_chat_loop — 交互式聊天循环
 *
 * 进入无限循环，逐行读取用户输入并发送给 Agent Runtime 处理。
 * 支持以 "/" 开头的斜杠命令：
 *   /exit, /quit     — 退出对话
 *   /tools           — 列出所有注册的工具
 *   /thinking on|off — 运行时切换思考模式
 *   /help            — 显示帮助
 * 所有非 "/" 开头的输入将直接发送给 LLM 处理。
 *
 * @param runtime  已完成装配的 Agent Runtime 实例（包含 LLM、工具注册表等全部依赖）
 */
static void show_help(void)
{
    printf("\n");
    printf("  ========== C_Claw CLI Help ==========\n");
    printf("\n");
    printf("  Usage:\n");
    printf("    c-claw                 进入交互式对话\n");
    printf("    c-claw ask \"问题\"      单次问答后退出\n");
    printf("    c-claw --help          显示帮助\n");
    printf("\n");
    printf("  Slash Commands:\n");
    printf("    /exit, /quit      退出对话\n");
    printf("    /tools            列出所有可用工具\n");
    printf("    /reload           重新加载可热替换的运行时资源\n");
    printf("    /agents           列出当前 manager 中的 Agent\n");
    printf("    /agent <id>       切换当前 Agent\n");
    printf("    /skills           显示 skills 配置摘要\n");
    printf("    /mcp              显示 MCP server 配置摘要\n");
    printf("    /interrupt        标记当前 session 中断\n");
    printf("    /thinking on      开启思考模式 (DeepSeek reasoning)\n");
    printf("    /thinking off     关闭思考模式\n");
    printf("    /stream on        开启流式输出模式\n");
    printf("    /stream off       关闭流式输出模式\n");
    printf("    /debug on         开启内部调试输出\n");
    printf("    /debug off        关闭内部调试输出\n");
    printf("    /help             显示本帮助\n");
    printf("\n");
    printf("  直接输入文本即可与 AI 对话。\n");
    printf("\n");
}

static int g_stream_seen_thinking = 0;
static int g_stream_seen_text = 0;
static int g_stream_seen_tool = 0;

/**
 * stream_render_reset — 重置 CLI 流式渲染状态，准备显示下一次模型输出。
 *
 */
static void stream_render_reset(void)
{
    g_stream_seen_thinking = 0;
    g_stream_seen_text = 0;
    g_stream_seen_tool = 0;
}

/*
 * stream_event_handler — 流式事件回调（CLI 展示用）
 *
 * 功能：
 *   订阅事件总线中的流式事件，实时打印到控制台。
 *   仅在 g_stream_mode 为 1 时输出。
 *
 * 参数：
 *   event     — 事件名称
 *   payload   — 事件负载 JSON 字符串
 *   user_data — 透传的用户数据（此处未使用）
 */
static void stream_event_handler(const char *event, const char *payload, void *user_data)
{
    (void)user_data;
    if (!g_stream_mode) return;

    if (strcmp(event, CC_EVENT_STREAM_THINKING) == 0) {
        if (!g_stream_seen_thinking) {
            printf("\n\033[90m[Thinking]\033[0m\n");
            g_stream_seen_thinking = 1;
        }
        printf("\033[90m%s\033[0m", payload ? payload : "");
        fflush(stdout);
    } else if (strcmp(event, CC_EVENT_STREAM_TEXT) == 0) {
        if (!g_stream_seen_text) {
            if (g_stream_seen_thinking || g_stream_seen_tool) {
                printf("\n");
            }
            printf("\033[36m[Agent]\033[0m\n");
            g_stream_seen_text = 1;
        }
        printf("%s", payload ? payload : "");
        fflush(stdout);
    } else if (strcmp(event, CC_EVENT_STREAM_TOOL_START) == 0) {
        if (!g_stream_seen_tool) {
            if (g_stream_seen_thinking || g_stream_seen_text) {
                printf("\n");
            }
            g_stream_seen_tool = 1;
        }
        printf("\033[33m[Tool Call]\033[0m");
        if (payload) {
            cc_json_value_t *json = NULL;
            cc_result_t rc = cc_json_parse(payload, &json);
            if (rc.code == CC_OK && json) {
                cc_json_value_t *name = cc_json_object_get(json, "tool_name");
                cc_json_value_t *id = cc_json_object_get(json, "tool_id");
                const char *tool_name = cc_json_string_value(name);
                const char *tool_id = cc_json_string_value(id);
                printf(" %s", tool_name ? tool_name : "");
                if (tool_id && tool_id[0]) {
                    printf(" (%s)", tool_id);
                }
                cc_json_destroy(json);
            } else {
                printf(" %s", payload);
            }
            cc_result_free(&rc);
        }
        printf("\n");
        fflush(stdout);
    } else if (strcmp(event, CC_EVENT_STREAM_TOOL_END) == 0) {
        printf("\033[32m[Tool Output]\033[0m\n");
        if (payload) {
            cc_json_value_t *json = NULL;
            cc_result_t rc = cc_json_parse(payload, &json);
            if (rc.code == CC_OK && json) {
                cc_json_value_t *result = cc_json_object_get(json, "result");
                cc_json_value_t *error = cc_json_object_get(json, "error");
                const char *text = result ? cc_json_string_value(result) : cc_json_string_value(error);
                if (text && text[0]) {
                    printf("%s", text);
                }
                cc_json_destroy(json);
            }
            cc_result_free(&rc);
        }
        printf("\n\033[32m[Tool Done]\033[0m\n");
        fflush(stdout);
    } else if (strcmp(event, CC_EVENT_STREAM_FINISHED) == 0) {
        printf("\n");
        fflush(stdout);
    }
}

/**
 * run_chat_loop — 驱动 CLI 交互循环，读取用户输入并调用 runtime 获取同步或流式回答。
 *
 * @param runtime 借用的对象；函数不释放该对象本身。
 */
static void print_configured_skills(const cc_config_t *config)
{
    printf("\nSkills:\n");
    printf("  watch: %s\n", config && config->skills.watch ? "on" : "off");
    printf("  extraDirs (%zu):\n", config ? config->skills.extra_dirs.count : 0);
    if (config) {
        for (size_t i = 0; i < config->skills.extra_dirs.count; i++) {
            printf("    - %s\n", config->skills.extra_dirs.items[i]);
        }
    }
    printf("\n");
}

static void print_configured_mcp(const cc_config_t *config)
{
    printf("\nMCP servers:\n");
    printf("  enabled: %s\n", config && config->mcp.enabled ? "yes" : "no");
    printf("  idle TTL: %d ms\n", config ? config->mcp.session_idle_ttl_ms : 0);
    printf("  entries (%zu):\n", config ? config->mcp.server_count : 0);
    if (config) {
        for (size_t i = 0; i < config->mcp.server_count; i++) {
            const cc_config_mcp_server_t *server = &config->mcp.servers[i];
            printf("    - %s [%s]\n",
                server->name ? server->name : "(unnamed)",
                server->transport ? server->transport : "stdio");
        }
    }
    printf("\n");
}

static void run_chat_loop(
    cc_runtime_builder_t *builder,
    cc_agent_runtime_t *runtime,
    cc_agent_manager_t *manager,
    cc_config_t *config,
    const char *config_path
)
{
    /*
     * 订阅流式事件（一次性，调用时通过 g_stream_mode 控制是否输出）
     */
    cc_event_bus_t *event_bus = cc_agent_runtime_event_bus(runtime);
    if (event_bus) {
        cc_event_bus_subscribe(event_bus, CC_EVENT_STREAM_THINKING,
            stream_event_handler, NULL);
        cc_event_bus_subscribe(event_bus, CC_EVENT_STREAM_TEXT,
            stream_event_handler, NULL);
        cc_event_bus_subscribe(event_bus, CC_EVENT_STREAM_TOOL_START,
            stream_event_handler, NULL);
        cc_event_bus_subscribe(event_bus, CC_EVENT_STREAM_TOOL_DELTA,
            stream_event_handler, NULL);
        cc_event_bus_subscribe(event_bus, CC_EVENT_STREAM_TOOL_END,
            stream_event_handler, NULL);
        cc_event_bus_subscribe(event_bus, CC_EVENT_STREAM_FINISHED,
            stream_event_handler, NULL);
    }

    printf("c-claw CLI Chat (输入 /help 查看命令, /exit 退出)\n");
    printf("上下键历史记录 | Tab 补全斜杠命令\n");
    printf("Session: %s", g_session_id);
    if (cc_agent_runtime_get_thinking_mode(runtime)) {
        printf("  [思考模式: ON]");
    }
    if (g_stream_mode) {
        printf("  [流式输出: ON]");
    }
    printf("\n\n");

    cc_cli_reload_watcher_t reload_watcher;
    reload_watcher_init(&reload_watcher, config_path);

    char line[4096];
    while (1) {
        if (cli_readline(line, sizeof(line)) != 0) break;
        reload_watcher_poll(&reload_watcher, builder, config);

        if (line[0] == '\0') continue;

        history_add(line);

        if (line[0] == '/') {
            if (strcmp(line, "/exit") == 0 || strcmp(line, "/quit") == 0) {
                printf("Goodbye!\n");
                break;
            }

            if (strcmp(line, "/help") == 0) {
                show_help();
                continue;
            }

            if (strcmp(line, "/tools") == 0) {
                char **names = NULL;
                size_t count = 0;
                cc_tool_registry_t *registry = cc_agent_runtime_tool_registry(runtime);
                cc_tool_registry_list_names(registry, &names, &count);
                printf("\nAvailable tools (%zu):\n", count);
                for (size_t i = 0; i < count; i++) {
                    printf("  - %s\n", names[i]);
                    free(names[i]);
                }
                free(names);
                printf("\n");
                continue;
            }

            if (strcmp(line, "/reload") == 0) {
                cc_runtime_reload_report_t report;
                cc_result_t rc = reload_runtime_from_path(builder, config, config_path, &report);
                if (rc.code == CC_OK) {
                    reload_watcher_note_current(&reload_watcher);
                    printf("[reload] 已重新读取 config.json，后续 run 会使用可用的新快照\n\n");
                    print_runtime_diagnostics("[reload] 部分外部工具不可用:", &report.diagnostics);
                } else {
                    printf("[reload] 失败: %s\n\n", rc.message ? rc.message : "unknown");
                    cc_result_free(&rc);
                }
                continue;
            }

            if (strcmp(line, "/agents") == 0) {
                char **ids = NULL;
                size_t count = 0;
                cc_result_t rc = manager ?
                    cc_agent_manager_list_agents(manager, &ids, &count) :
                    cc_result_error(CC_ERR_PLATFORM, "Agent manager is disabled");
                if (rc.code != CC_OK) {
                    printf("[agents] %s\n\n", rc.message ? rc.message : "unavailable");
                    cc_result_free(&rc);
                } else {
                    const char *current = cc_agent_manager_current_agent(manager);
                    printf("\nAgents (%zu):\n", count);
                    for (size_t i = 0; i < count; i++) {
                        printf("  %s %s\n",
                            current && strcmp(current, ids[i]) == 0 ? "*" : "-",
                            ids[i]);
                        free(ids[i]);
                    }
                    free(ids);
                    printf("\n");
                }
                continue;
            }

            if (strncmp(line, "/agent ", 7) == 0) {
                const char *agent_id = line + 7;
                cc_result_t rc = manager ?
                    cc_agent_manager_set_current_agent(manager, agent_id) :
                    cc_result_error(CC_ERR_PLATFORM, "Agent manager is disabled");
                if (rc.code == CC_OK) {
                    printf("[agent] 当前 Agent: %s\n\n", agent_id);
                } else {
                    printf("[agent] 切换失败: %s\n\n", rc.message ? rc.message : "unknown");
                    cc_result_free(&rc);
                }
                continue;
            }

            if (strcmp(line, "/skills") == 0) {
                print_configured_skills(config);
                continue;
            }

            if (strcmp(line, "/mcp") == 0) {
                print_configured_mcp(config);
                continue;
            }

            if (strcmp(line, "/interrupt") == 0) {
                cc_result_t rc = manager ?
                    cc_agent_manager_interrupt(manager, NULL, g_session_id) :
                    cc_result_error(CC_ERR_PLATFORM, "Agent manager is disabled");
                if (rc.code == CC_OK) {
                    printf("[interrupt] 当前 session 已标记中断\n\n");
                } else {
                    printf("[interrupt] 失败: %s\n\n", rc.message ? rc.message : "unknown");
                    cc_result_free(&rc);
                }
                continue;
            }

            if (strcmp(line, "/thinking") == 0 ||
                strcmp(line, "/thinking on") == 0) {
                cc_agent_runtime_set_thinking_mode(runtime, 1);
                printf("[思考模式] 已开启\n\n");
                continue;
            }

            if (strcmp(line, "/thinking off") == 0) {
                cc_agent_runtime_set_thinking_mode(runtime, 0);
                printf("[思考模式] 已关闭\n\n");
                continue;
            }

            if (strcmp(line, "/stream") == 0 ||
                strcmp(line, "/stream on") == 0) {
                g_stream_mode = 1;
                printf("[流式输出] 已开启（思考内容、工具调用将实时展示）\n\n");
                continue;
            }

            if (strcmp(line, "/stream off") == 0) {
                g_stream_mode = 0;
                printf("[流式输出] 已关闭\n\n");
                continue;
            }

            if (strcmp(line, "/debug") == 0 ||
                strcmp(line, "/debug on") == 0) {
                setenv("CCLAW_DEBUG", "1", 1);
                printf("[调试输出] 已开启\n\n");
                continue;
            }

            if (strcmp(line, "/debug off") == 0) {
                unsetenv("CCLAW_DEBUG");
                printf("[调试输出] 已关闭\n\n");
                continue;
            }

            printf("未知命令: %s\n", line);
            printf("可用命令: /exit /quit /tools /reload /agents /agent <id> /skills /mcp /interrupt /thinking on|off /stream on|off /debug on|off /help\n\n");
            continue;
        }

        char *response = NULL;
        cc_result_t rc;
        if (g_stream_mode && cc_agent_runtime_supports_stream(runtime)) {
            stream_render_reset();
            rc = cc_agent_runtime_handle_message_stream(
                runtime, g_session_id, line, &response);
        } else {
            rc = manager ?
                cc_agent_manager_handle_message(manager, NULL, g_session_id, line, &response) :
                cc_agent_runtime_handle_message(runtime, g_session_id, line, &response);
        }

        if (rc.code != CC_OK) {
            printf("\nError: %s\n", rc.message ? rc.message : "Unknown error");
            cc_result_free(&rc);
        } else if (response) {
            if (!g_stream_mode) {
                printf("\nAgent> %s\n\n", response);
            } else {
                if (!g_stream_seen_text && strlen(response) > 0) {
                    printf("\n\033[36m[Agent]\033[0m\n%s\n\n", response);
                }
            }
            free(response);
        }
    }
}

/**
 * run_single_ask — 单次问答模式
 *
 * 发送一次查询给 Agent Runtime，获得回复后立即返回。
 * 不会进入交互循环，适用于脚本调用或单次查询场景。
 *
 * @param runtime  已完成装配的 Agent Runtime 实例
 * @param query    用户输入的查询字符串
 */
static void run_single_ask(
    cc_agent_runtime_t *runtime,
    cc_agent_manager_t *manager,
    const char *query
)
{
    char *response = NULL;
    cc_result_t rc = manager ?
        cc_agent_manager_handle_message(manager, NULL, g_session_id, query, &response) :
        cc_agent_runtime_handle_message(runtime, g_session_id, query, &response);

    if (rc.code != CC_OK) {
        fprintf(stderr, "Error: %s\n", rc.message ? rc.message : "Unknown error");
        cc_result_free(&rc);
    } else if (response) {
        printf("%s\n", response);
        free(response);
    }
}

/**
 * cc_cli_gateway_run — CLI Gateway 主入口
 *
 * 这是由 main() 调用的 Gateway 适配器入口。执行流程如下：
 *   1. 生成会话 ID
 *   2. 在存储中创建对应会话记录（如果存储后端支持）
 *   3. 根据命令行参数决定运行模式：
 *      - 'ask <query>' → 单次问答模式
 *      - 无参数或其它 → 交互式聊天循环
 *   4. 清理会话 ID
 *
 * @param argc     命令行参数个数（透传自 main）
 * @param argv     命令行参数数组（透传自 main）
 * @param builder     已完成依赖装配的组合根，`/reload` 通过它发布新 generation。
 * @param config      当前系统配置；reload 成功后会原地替换为新配置对象。
 * @param config_path 配置文件路径；reload 失败时旧 config 和旧 generation 都保留。
 * @return         退出码，0 表示正常退出，1 表示用法错误
 */
int cc_cli_gateway_run(
    int argc,
    char **argv,
    cc_runtime_builder_t *builder,
    cc_config_t *config,
    const char *config_path
)
{
    cc_agent_runtime_t *runtime = cc_runtime_builder_runtime(builder);
    cc_agent_manager_t *manager = cc_runtime_builder_agent_manager(builder);

    if (argc >= 2 &&
        (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0 ||
         strcmp(argv[1], "help") == 0)) {
        show_help();
        return 0;
    }

    cc_agent_runtime_set_tool_approval(runtime, cli_tool_approval, NULL);
    g_stream_mode = config && config->stream_mode ? 1 : 0;
    if (config && config->debug_mode) {
        setenv("CCLAW_DEBUG", "1", 1);
    }
    print_runtime_diagnostics(
        "[startup] 部分外部工具不可用:",
        cc_runtime_builder_diagnostics(builder));

    g_session_id = generate_session_id();

    cc_agent_runtime_create_session(runtime, g_session_id, config->workspace_path);

    if (argc >= 2 && strcmp(argv[1], "ask") == 0) {
        if (argc >= 3) {
            run_single_ask(runtime, manager, argv[2]);
        } else {
            fprintf(stderr, "Usage: c-claw ask \"your question\"\n");
            free(g_session_id);
            g_session_id = NULL;
            return 1;
        }
    } else {
        run_chat_loop(builder, runtime, manager, config, config_path);
    }

    free(g_session_id);
    g_session_id = NULL;
    return 0;
}
