/**
 * 学习导读：cclaw/core/src/app/cc_sse_parser.c
 *
 * 所属层次：核心层。
 * 阅读重点：SSE（Server-Sent Events）文本流协议的增量有状态解析器。核心是
 *          "逐字符进，按行切，空行触发 event 回调"的循环。理解 line 缓冲区与
 *          event_data 累积缓冲区的独立管理，以及 flush_event 的内存复用策略。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * cc_sse_parser.c — SSE（Server-Sent Events）增量文本解析器
 *
 * 本模块在整体架构中的角色：
 * ─────────────────────────────
 * 位于 Core 层（与平台无关），是一个纯文本增量状态机。HTTP client 的 body
 * callback 可能把一行、一个 event、甚至一个 UTF-8 字节序列拆成多个 chunk；
 * parser 因此分别保存"当前行"和"当前 event 数据"。解析器不依赖 curl、
 * ESP-IDF 或任何平台 API——HTTP transport 只负责把 body chunk 喂进来。
 *
 * 上游调用方：
 *   - MCP transport（如 cc_mcp_sse_transport）—— 在 HTTP 响应 body
 *     callback 中逐 chunk 调用 cc_sse_parser_feed()，流结束时调 finish()
 *
 * 下游依赖模块：
 *   - 无业务模块依赖；仅依赖标准 C 库（stdlib.h、string.h）做内存管理
 *
 * ─── SSE 协议子集 ──────────────────────────────────────────────────────
 *
 * 本 parser 只消费 MCP 实际需要的语义：
 *   - 以 '\n' 为行分隔符
 *   - "data:<value>" 行：累积 value 到 event_data，多行用 '\n' 拼接
 *   - 空行：触发事件回调，将累积的 event_data 提交给调用方
 *   - 以 ':' 开头的行（comment/heartbeat）：忽略
 *   - event/id/retry 字段：忽略
 *   - 尾部 '\r'：在行处理前剥离
 *
 * ─── 状态机设计 ────────────────────────────────────────────────────────
 *
 *  parser 持有两块独立缓冲区：
 *    - line：当前未完成的行片段（逐字符累积，遇到 '\n' 时提交处理）
 *    - event_data：当前 event 的 data 字段累积（跨多行 data: 直到空行）
 *
 *  feed() 逐字节循环：
 *    非 '\n' → append_char 到 line 缓冲区
 *    '\n'   → process_line(line) → 清空 line → 继续
 *
 *  process_line() 分发：
 *    空行（去除 '\r' 后 len==0）→ flush_event()
 *    ':' 开头 → 忽略（comment）
 *    "data:..." → append 到 event_data
 *
 *  flush_event()：
 *    调用 on_event 回调提交 event_data，然后 event_len 归零、缓冲区复用
 *    （不释放，降低长流式响应中的分配次数）
 *
 * ─── 关键设计决策 ──────────────────────────────────────────────────────
 *
 *   - 缓冲区起始容量 64 字节，按 2x 扩容——适合 SSE 中每个 data 行通常不
 *     超过几 KB 的实际情况
 *   - 回调期间 event_data 缓冲区稳定存在，回调返回后 Parser 复用同一块
 *     内存——调用方如需保存数据必须在回调内复制
 *   - finish() 处理两个边界：最后一个 event 没有空行结尾 → 按已接收的
 *     line 提交；最后一个 event_data 尚未 flush → 调用 flush_event
 */

#include "cc/app/cc_sse_parser.h"

#include <stdlib.h>
#include <string.h>

/*
 * SSE parser 是纯文本增量状态机。HTTP client 的 body callback 可能把一行、
 * 一个 event，甚至一个 UTF-8 字节序列拆成多个 chunk；parser 因此分别保存
 * 当前 line 和当前 event_data。这里只消费 MCP 需要的 data 字段，忽略 event/id/retry。
 */
struct cc_sse_parser {
    char *line;
    size_t line_len;
    size_t line_cap;
    char *event_data;
    size_t event_len;
    size_t event_cap;
};

static cc_result_t append_bytes(char **buffer, size_t *len, size_t *cap, const char *data, size_t n)
{
    if (n == 0) return cc_result_ok();
    if (*len + n + 1 > *cap) {
        size_t next_cap = *cap ? *cap : 64;
        while (next_cap < *len + n + 1) next_cap *= 2;
        char *next = realloc(*buffer, next_cap);
        if (!next) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to grow SSE parser buffer");
        *buffer = next;
        *cap = next_cap;
    }
    memcpy(*buffer + *len, data, n);
    *len += n;
    (*buffer)[*len] = '\0';
    return cc_result_ok();
}

static cc_result_t append_char(char **buffer, size_t *len, size_t *cap, char ch)
{
    return append_bytes(buffer, len, cap, &ch, 1);
}

static cc_result_t flush_event(
    cc_sse_parser_t *parser,
    cc_sse_event_fn on_event,
    void *user_data
)
{
    if (parser->event_len == 0) return cc_result_ok();
    if (!on_event) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "SSE event callback is required");

    /*
     * event_data 是 parser 持有的临时缓冲。回调期间它稳定存在；回调返回后
     * parser 会复用同一块内存来降低长流式响应中的分配次数。
     */
    cc_result_t rc = on_event(parser->event_data, user_data);
    parser->event_len = 0;
    if (parser->event_data) parser->event_data[0] = '\0';
    return rc;
}

static cc_result_t process_line(
    cc_sse_parser_t *parser,
    const char *line,
    size_t len,
    cc_sse_event_fn on_event,
    void *user_data
)
{
    while (len > 0 && line[len - 1] == '\r') len--;
    if (len == 0) {
        return flush_event(parser, on_event, user_data);
    }
    if (line[0] == ':') {
        return cc_result_ok();
    }
    if (len >= 5 && strncmp(line, "data:", 5) == 0) {
        const char *value = line + 5;
        size_t value_len = len - 5;
        if (value_len > 0 && *value == ' ') {
            value++;
            value_len--;
        }
        if (parser->event_len > 0) {
            cc_result_t rc = append_char(
                &parser->event_data, &parser->event_len, &parser->event_cap, '\n');
            if (rc.code != CC_OK) return rc;
        }
        return append_bytes(&parser->event_data, &parser->event_len, &parser->event_cap,
                            value, value_len);
    }
    return cc_result_ok();
}

cc_result_t cc_sse_parser_create(cc_sse_parser_t **out_parser)
{
    if (!out_parser) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null SSE parser output");
    cc_sse_parser_t *parser = calloc(1, sizeof(*parser));
    if (!parser) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create SSE parser");
    *out_parser = parser;
    return cc_result_ok();
}

void cc_sse_parser_destroy(cc_sse_parser_t *parser)
{
    if (!parser) return;
    free(parser->line);
    free(parser->event_data);
    free(parser);
}

cc_result_t cc_sse_parser_feed(
    cc_sse_parser_t *parser,
    const char *data,
    size_t len,
    cc_sse_event_fn on_event,
    void *user_data
)
{
    if (!parser || (!data && len > 0)) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid SSE parser feed");
    }

    for (size_t i = 0; i < len; i++) {
        if (data[i] == '\n') {
            cc_result_t rc = process_line(
                parser, parser->line ? parser->line : "", parser->line_len,
                on_event, user_data);
            parser->line_len = 0;
            if (parser->line) parser->line[0] = '\0';
            if (rc.code != CC_OK) return rc;
        } else {
            cc_result_t rc = append_char(&parser->line, &parser->line_len, &parser->line_cap, data[i]);
            if (rc.code != CC_OK) return rc;
        }
    }
    return cc_result_ok();
}

cc_result_t cc_sse_parser_finish(
    cc_sse_parser_t *parser,
    cc_sse_event_fn on_event,
    void *user_data
)
{
    if (!parser) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null SSE parser");
    if (parser->line_len > 0) {
        cc_result_t rc = process_line(
            parser, parser->line, parser->line_len, on_event, user_data);
        parser->line_len = 0;
        if (parser->line) parser->line[0] = '\0';
        if (rc.code != CC_OK) return rc;
    }
    return flush_event(parser, on_event, user_data);
}
