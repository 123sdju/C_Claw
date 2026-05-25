/**
 * 学习导读：cclaw/core/src/app/cc_plugin_protocol.c
 *
 * 所属层次：核心层。
 * 阅读重点：JSON-RPC 2.0 请求/响应的构建与解析。本模块不做进程管理、不碰
 *          pipe/stdin/stdout，只把 C-Claw tool call 编成单行 JSON-RPC 2.0
 *          envelope，并把插件回包拆成 result/error。具体 transport 由
 *          应用层提供。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * cc_plugin_protocol.c — 插件 JSON-RPC 2.0 envelope 编解码模块
 *
 * 本模块在整体架构中的角色：
 * ─────────────────────────────
 * 位于 Core 层（与平台无关），是工具调用与外部插件进程之间通信协议的
 * 标准化封装。本模块只负责消息格式（JSON-RPC 2.0 envelope），不参与
 * 进程生命周期、pipe 读写或任何平台特定操作。
 *
 * 上游调用方：
 *   - 应用层 plugin runner —— 调用 build_request()
 *     构造请求 JSON，再自行写入子进程 stdin；从 stdout 读到一行后调
 *     parse_response() 解析为 result/error
 *
 * 下游依赖模块：
 *   - cc_json.c — JSON 对象构建、序列化与反序列化（cc_json_create_object、
 *     cc_json_stringify_unformatted、cc_json_parse 等）
 *
 * 关键设计决策：
 * ─────────────────────────────
 *   - 固定 id="1"：插件调用是一问一答串行模型，每个 worker 拥有独立
 *     stdin/stdout，不需要全局递增 id。固定 id 避免了 core 中引入
 *     全局可变计数器及其初始化竞态。
 *   - params 解析容错：若 params_json 解析失败，降级为空对象 {}，
 *     而非让整个请求失败——防止格式错误的参数导致调用完全无法发出。
 */

#include "cc/app/cc_plugin_protocol.h"
#include "cc/util/cc_json.h"

#include <stdlib.h>

/*
 * Plugin protocol 留在 core，因为 JSON-RPC envelope 与平台无关。应用层只负责把
 * request_json 写入 transport，并把 response line 交回这里解析。
 */
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
