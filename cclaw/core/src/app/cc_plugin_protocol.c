#include "cc/app/cc_plugin_protocol.h"
#include "cc/util/cc_json.h"

#include <stdlib.h>

char *cc_plugin_protocol_build_request(
    const char *method,
    const char *params_json
)
{
    cc_json_value_t *req = cc_json_create_object();
    if (!req) return NULL;

    cc_json_object_set(req, "jsonrpc", cc_json_create_string("2.0"));

    /*
     * 插件调用当前是一问一答模型：process 层负责串行化单个 worker 的
     * stdin/stdout，多 worker 彼此独立，因此 SDK 不需要全局递增 id。
     * 固定 id 避免在 core 中引入全局可变计数器及其初始化竞态。
     */
    cc_json_object_set(req, "id", cc_json_create_string("1"));
    cc_json_object_set(req, "method", cc_json_create_string(method ? method : ""));

    if (params_json) {
        cc_json_value_t *params = NULL;
        cc_result_t rc = cc_json_parse(params_json, &params);
        if (rc.code == CC_OK && params) {
            cc_json_object_set(req, "params", params);
        } else {
            cc_json_object_set(req, "params", cc_json_create_object());
            cc_result_free(&rc);
        }
    } else {
        cc_json_object_set(req, "params", cc_json_create_object());
    }

    char *result = cc_json_stringify_unformatted(req);
    cc_json_destroy(req);
    return result;
}

cc_result_t cc_plugin_protocol_parse_response(
    const char *response_json,
    char **out_result_json,
    char **out_error_json
)
{
    if (!response_json || !out_result_json || !out_error_json) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid plugin response parse request");
    }
    *out_result_json = NULL;
    *out_error_json = NULL;

    cc_json_value_t *resp = NULL;
    cc_result_t rc = cc_json_parse(response_json, &resp);
    if (rc.code != CC_OK) return rc;

    cc_json_value_t *result = cc_json_object_get(resp, "result");
    if (result) {
        *out_result_json = cc_json_stringify(result);
    }

    cc_json_value_t *error = cc_json_object_get(resp, "error");
    if (error) {
        *out_error_json = cc_json_stringify(error);
    }

    cc_json_destroy(resp);
    return cc_result_ok();
}

void cc_plugin_protocol_free_response(char *result_json, char *error_json)
{
    free(result_json);
    free(error_json);
}
