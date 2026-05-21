/**
 * cc_plugin_protocol.h — plugin JSON-RPC envelope 编解码。
 *
 * 所属层次：核心 SDK。
 *
 * 这里不启动进程、不读写 pipe，也不关心 POSIX/Windows。SDK 只负责把
 * C-Claw tool call 编成单行 JSON-RPC 2.0 请求，并把插件返回的响应拆成
 * result/error。具体 transport 由 app/platform 提供。
 */

#ifndef CC_PLUGIN_PROTOCOL_H
#define CC_PLUGIN_PROTOCOL_H

#include "cc/core/cc_result.h"

#ifdef __cplusplus
extern "C" {
#endif

char *cc_plugin_protocol_build_request(
    const char *method,
    const char *params_json
);

cc_result_t cc_plugin_protocol_parse_response(
    const char *response_json,
    char **out_result_json,
    char **out_error_json
);

void cc_plugin_protocol_free_response(
    char *result_json,
    char *error_json
);

#ifdef __cplusplus
}
#endif

#endif
