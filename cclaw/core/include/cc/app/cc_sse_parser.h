/**
 * cc_sse_parser.h — Server-Sent Events 增量解析器。
 *
 * 所属层次：核心 SDK。
 *
 * SSE 是一种文本流协议，网络层可能把一条 event 拆成多个 body chunk。
 * 因此解析器必须持有跨 chunk 的半行和半个 event。这里不依赖 curl、
 * ESP-IDF 或任何平台 API，HTTP transport 只负责把 body chunk 喂进来。
 */

#ifndef CC_SSE_PARSER_H
#define CC_SSE_PARSER_H

#include "cc/core/cc_result.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cc_sse_parser cc_sse_parser_t;

/**
 * 当一个 SSE event 完成时调用。
 *
 * data 是已经合并好的 data 字段：多行 data: 会用 '\n' 拼接；heartbeat
 * comment 与其他字段被忽略。data 只在回调期间有效，调用方如果要保存必须复制。
 */
typedef cc_result_t (*cc_sse_event_fn)(
    const char *data,
    void *user_data
);

cc_result_t cc_sse_parser_create(cc_sse_parser_t **out_parser);

void cc_sse_parser_destroy(cc_sse_parser_t *parser);

/**
 * 增量输入一段 body chunk。
 *
 * 解析器会缓存未完成的行；只有遇到空行时才提交一个完整 event。
 */
cc_result_t cc_sse_parser_feed(
    cc_sse_parser_t *parser,
    const char *data,
    size_t len,
    cc_sse_event_fn on_event,
    void *user_data
);

/**
 * 输入结束时调用。若最后一个 event 没有空行结尾，finish 会按已接收内容提交。
 */
cc_result_t cc_sse_parser_finish(
    cc_sse_parser_t *parser,
    cc_sse_event_fn on_event,
    void *user_data
);

#ifdef __cplusplus
}
#endif

#endif
