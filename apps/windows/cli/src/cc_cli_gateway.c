/**
 * 学习导读：apps/windows/cli/src/cc_cli_gateway.c
 *
 * 所属层次：Windows CLI 应用层。
 * 阅读重点：这里镜像桌面 CLI 能力但使用 Windows 平台实现，阅读时重点比较与 POSIX 版本的差异。
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

/* 当前会话 ID，基于时间戳生成，全局生命周期内唯一 */
static char *g_session_id = NULL;

#define HISTORY_MAX 100
static char *history[HISTORY_MAX];
static int history_count = 0;

static const char *slash_commands[] = {
    "/exit",
    "/quit",
    "/tools",
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

/* 学习注释：history_add 是本文件内部辅助函数。
 * 阅读时先看它服务哪个 public API，再看它如何处理边界条件和资源释放。 */
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

/* 学习注释：tab_complete 是本文件内部辅助函数。
 * 阅读时先看它服务哪个 public API，再看它如何处理边界条件和资源释放。 */
static int tab_complete(char *buf, size_t *pos, size_t size)
{
    if (buf[0] != '/') return 0;

    const char *matches[16];
    int match_count = 0;
    for (int i = 0; slash_commands[i]; i++) {
        if (strncmp(slash_commands[i], buf, *pos) == 0) {
            matches[match_count++] = slash_commands[i];
            if (match_count >= 16) break;
        }
    }

    if (match_count == 0) return 0;

    if (match_count == 1) {
        size_t cmd_len = strlen(matches[0]);
        if (cmd_len < size) {
            strcpy(buf, matches[0]);
            *pos = cmd_len;
        }
        return 1;
    }

    printf("\n");
    for (int i = 0; i < match_count; i++) {
        printf("  %s\n", matches[i]);
    }
    printf("You> %s", buf);
    fflush(stdout);
    return 0;
}

/* 学习注释：__redraw_line 是本文件内部辅助函数。
 * 阅读时先看它服务哪个 public API，再看它如何处理边界条件和资源释放。 */
static void __redraw_line(const char *buf)
{
    printf("\r\033[KYou> %s", buf);
    if (buf[0] == '/') {
        printf("  [Tab补全: /exit /quit /tools /thinking /stream /debug /help]");
    }
    fflush(stdout);
}

/* 学习注释：generate_session_id 是本文件内部辅助函数。
 * 阅读时先看它服务哪个 public API，再看它如何处理边界条件和资源释放。 */
static char *generate_session_id(void)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "ses_%ld", (long)time(NULL));
    return strdup(buf);
}

/* 学习注释：cli_readline 是本文件内部辅助函数。
 * 阅读时先看它服务哪个 public API，再看它如何处理边界条件和资源释放。 */
static int cli_readline(char *buf, size_t size)
{
    if (size == 0) return -1;

    struct termios old_term, new_term;
    tcgetattr(STDIN_FILENO, &old_term);
    new_term = old_term;
    new_term.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &new_term);

    size_t pos = 0;
    memset(buf, 0, size);

    int hist_idx = history_count;
    char saved_buf[4096] = {0};

    printf("You> ");
    fflush(stdout);

    int result = 0;
    while (pos + 1 < size) {
        int c = getchar();
        if (c == EOF) { result = -1; break; }
        if (c == '\n' || c == '\r') { putchar('\n'); break; }

        if (c == '\t') {
            if (tab_complete(buf, &pos, size)) {
                hist_idx = history_count;
                __redraw_line(buf);
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
                            pos = hlen;
                        }
                        __redraw_line(buf);
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
                                pos = slen;
                            }
                            saved_buf[0] = '\0';
                        } else {
                            size_t hlen = strlen(history[hist_idx]);
                            if (hlen < size) {
                                strcpy(buf, history[hist_idx]);
                                pos = hlen;
                            }
                        }
                        __redraw_line(buf);
                    }
                    continue;
                }
            }
            continue;
        }

        if (c == 0x7F || c == '\b') {
            if (pos > 0) {
                pos--;
                while (pos > 0 && (buf[pos] & 0xC0) == 0x80) pos--;
                buf[pos] = '\0';
                hist_idx = history_count;
                saved_buf[0] = '\0';
                __redraw_line(buf);
            }
            continue;
        }
        if (c < 32) continue;

        buf[pos++] = (char)c;
        buf[pos] = '\0';
        hist_idx = history_count;
        saved_buf[0] = '\0';
        __redraw_line(buf);
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

static int g_stream_mode = 0;
static int g_stream_seen_thinking = 0;
static int g_stream_seen_text = 0;
static int g_stream_seen_tool = 0;

/* 学习注释：stream_render_reset 是本文件内部辅助函数。
 * 阅读时先看它服务哪个 public API，再看它如何处理边界条件和资源释放。 */
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

/* 学习注释：run_chat_loop 是本文件内部辅助函数。
 * 阅读时先看它服务哪个 public API，再看它如何处理边界条件和资源释放。 */
static void run_chat_loop(cc_agent_runtime_t *runtime)
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

    char line[4096];
    while (1) {
        if (cli_readline(line, sizeof(line)) != 0) break;

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
            printf("可用命令: /exit /quit /tools /thinking on|off /stream on|off /debug on|off /help\n\n");
            continue;
        }

        char *response = NULL;
        cc_result_t rc;
        if (g_stream_mode && cc_agent_runtime_supports_stream(runtime)) {
            stream_render_reset();
            rc = cc_agent_runtime_handle_message_stream(
                runtime, g_session_id, line, &response);
        } else {
            rc = cc_agent_runtime_handle_message(
                runtime, g_session_id, line, &response);
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
static void run_single_ask(cc_agent_runtime_t *runtime, const char *query)
{
    char *response = NULL;
    cc_result_t rc = cc_agent_runtime_handle_message(
        runtime, g_session_id, query, &response);

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
 * @param runtime  已完成依赖装配的 Agent Runtime 实例指针
 * @param config   系统配置实例
 * @return         退出码，0 表示正常退出，1 表示用法错误
 */
int cc_cli_gateway_run(int argc, char **argv, cc_agent_runtime_t *runtime, cc_config_t *config)
{
    if (argc >= 2 &&
        (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0 ||
         strcmp(argv[1], "help") == 0)) {
        show_help();
        return 0;
    }

    g_stream_mode = config && config->stream_mode ? 1 : 0;
    if (config && config->debug_mode) {
        setenv("CCLAW_DEBUG", "1", 1);
    }

    g_session_id = generate_session_id();

    cc_agent_runtime_create_session(runtime, g_session_id, config->workspace_path);

    if (argc >= 2 && strcmp(argv[1], "ask") == 0) {
        if (argc >= 3) {
            run_single_ask(runtime, argv[2]);
        } else {
            fprintf(stderr, "Usage: c-claw ask \"your question\"\n");
            free(g_session_id);
            g_session_id = NULL;
            return 1;
        }
    } else {
        run_chat_loop(runtime);
    }

    free(g_session_id);
    g_session_id = NULL;
    return 0;
}
