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
