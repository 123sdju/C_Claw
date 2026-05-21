/**
 * 学习导读：cclaw/adapters/src/tools/common/cc_http_request_tool.c
 *
 * 所属层次：适配器层。
 * 阅读重点：这里把端口接口落到具体后端，阅读时重点看协议转换、资源释放和失败降级。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * cc_http_request_tool.c — http.request 工具适配器
 *
 * 模块在整体架构中的角色：
 *   本文件为 "http.request" 工具提供适配器。该工具允许 LLM 发起 HTTP 请求，
 *   是 Agent 与外部 Web 服务交互的核心通道。
 *   该工具通过 cc_http_client_t 风格的端口函数执行请求。桌面平台默认
 *   使用 libcurl 适配器，设备平台可以替换为自己的 HTTP client 实现。
 *
 * 设计模式：Adapter（适配器）模式
 *   将 HTTP 请求能力适配为 cc_tool vtable 接口，
 *   使 LLM 可通过统一工具接口发起 HTTP 请求。
 *   与 file_read / file_write / shell_run 等工具保持相同的 vtable 契约。
 *
 * 实现接口：
 *   - cc_tool_vtable_t（5 个虚拟方法：name / description / schema_json / call / destroy）
 *
 * 接口契约：
 *   - name() 返回工具在注册表中的唯一标识 "http.request"
 *   - description() 返回 LLM 可读的自然语言描述
 *   - schema_json() 定义参数规范：url（必填）、method/body/timeout_ms（可选）
 *   - call() 通过 cc_http_client_perform 执行 HTTP 请求
 *   - destroy() 释放工具实例内存
 *
 * 依赖关系：
 *   - cc/ports/cc_tool.h：工具端口接口定义（cc_tool_vtable_t、cc_tool_t 等）
 *   - cc/ports/cc_http_client.h：平台可替换的 HTTP client 端口
 *
 * 当前边界：
 *   - body 存在时默认发送 Content-Type: application/json。
 *   - 返回值包含 status 和 body；响应 headers 暂不暴露给 LLM tool result。
 *   - 证书、代理、重定向等策略由底层 cc_http_client_t adapter 或 profile 决定。
 */

#include "cc/ports/cc_tool.h"
#include "cc/ports/cc_http_client.h"
#include "cc/util/cc_json.h"
#include <stdlib.h>
#include <string.h>

/**
 * cc_http_request_tool_t — http.request 工具的内部数据结构
 *
 * 字段说明：
 *   dummy — 占位字段，当前实现不需要保存状态；所有请求参数都从 args_json 读取。
 *
 * 设计决策：
 *   - 使用 typedef struct 而非匿名结构体，保持调试信息和类型名称稳定
 *   - 使用 calloc 零初始化，dummy 字段始终为 0
 */
typedef struct {
    int dummy;
} cc_http_request_tool_t;

/**
 * http_req_name — vtable 方法：返回工具名称
 *
 * 功能：返回该工具在工具注册表中的唯一标识名称。
 *       工具注册表使用此名称进行工具查找和路由。
 *
 * @param self 工具实例指针（当前实现中未使用，保留以保持 vtable 签名一致）
 * @return 工具名称字符串 "http.request"
 *         注意：其他工具使用下划线命名（如 "file_read"），此工具使用点号分隔命名，
 *         遵循 MCP（Model Context Protocol）风格的工具命名约定
 */
static const char *http_req_name(void *self) { (void)self; return "http.request"; }

/**
 * http_req_description — vtable 方法：返回工具描述
 *
 * 功能：返回工具的自然语言描述，供 LLM 理解工具用途。
 *       描述文本会出现在 system prompt 的工具列表部分。
 *
 * @param self 工具实例指针（当前实现中未使用）
 * @return 工具描述字符串 "Make HTTP requests"
 */
static const char *http_req_description(void *self) { (void)self; return "Make HTTP requests"; }

/**
 * http_req_schema_json — vtable 方法：返回工具参数的 JSON Schema
 *
 * 功能：定义工具调用时必须/可选的参数及其类型，符合 JSON Schema 规范。
 *       框架将此 Schema 发送给 LLM，使模型了解如何正确构造工具调用参数。
 *
 * @param self 工具实例指针（当前实现中未使用）
 * @return JSON Schema 字符串，定义了以下参数：
 *         - url（string 类型，必填）：目标请求 URL
 *         - method（string 类型，可选）：HTTP 方法，如 GET/POST/PUT/DELETE
 *         - body（string 类型，可选）：请求体；存在时默认 method 为 POST
 *         - timeout_ms（integer 类型，可选）：本次请求 timeout
 *
 * 设计决策：
 *   - url 为必填参数，因为 HTTP 请求必须有目标地址
 *   - method 为可选参数；未传且存在 body 时使用 POST，否则使用 GET
 *   - 当前工具不暴露自定义 headers，避免 LLM 直接构造 Authorization 等敏感字段
 */
static const char *http_req_schema_json(void *self)
{
    (void)self;
    return "{"
        "\"type\":\"object\","
        "\"properties\":{"
            "\"url\":{\"type\":\"string\",\"description\":\"URL to request\"},"
            "\"method\":{\"type\":\"string\",\"description\":\"HTTP method\"},"
            "\"body\":{\"type\":\"string\",\"description\":\"Optional request body\"},"
            "\"timeout_ms\":{\"type\":\"integer\",\"description\":\"Request timeout in milliseconds\"}"
        "},"
        "\"required\":[\"url\"]"
    "}";
}

/**
 * http_req_call — vtable 方法：执行 HTTP 请求
 *
 * @param self      工具实例指针（当前未使用）
 * @param args_json JSON 格式的调用参数
 * @param ctx       工具上下文；当前 HTTP tool 使用 args_json 自带 timeout_ms，
 *                  不读取 workspace_dir
 * @param out_result 输出结果结构体
 */
static cc_result_t http_req_call(
    void *self,
    const char *args_json,
    const cc_tool_context_t *ctx,
    cc_tool_result_t *out_result
)
{
    (void)self;
    (void)ctx;
    memset(out_result, 0, sizeof(cc_tool_result_t));

    cc_json_value_t *args = NULL;
    cc_result_t rc = cc_json_parse(args_json, &args);
    if (rc.code != CC_OK || !args) {
        out_result->ok = 0;
        out_result->error = strdup("Failed to parse arguments JSON");
        cc_result_free(&rc);
        return cc_result_ok();
    }

    const char *url = cc_json_string_value(cc_json_object_get(args, "url"));
    const char *method = cc_json_string_value(cc_json_object_get(args, "method"));
    const char *body = cc_json_string_value(cc_json_object_get(args, "body"));
    int timeout_ms = cc_json_int_value(cc_json_object_get(args, "timeout_ms"));

    if (!url || strlen(url) == 0) {
        out_result->ok = 0;
        out_result->error = strdup("Missing required parameter: url");
        cc_json_destroy(args);
        return cc_result_ok();
    }

    cc_http_header_t headers[1] = {
        {"Content-Type", "application/json"}
    };

    cc_http_request_t request;
    memset(&request, 0, sizeof(request));
    request.method = method ? method : (body ? "POST" : "GET");
    request.url = url;
    request.headers = body ? headers : NULL;
    request.header_count = body ? 1 : 0;
    request.body = body;
    request.timeout_ms = timeout_ms > 0 ? timeout_ms :
        (ctx && ctx->timeout_ms > 0 ? ctx->timeout_ms : 30000);
    request.cancel_token = ctx ? ctx->cancel_token : NULL;

    cc_http_response_t response;
    rc = cc_http_client_perform(&request, &response);
    cc_json_destroy(args);

    if (rc.code != CC_OK) {
        out_result->ok = 0;
        out_result->error = strdup(rc.message ? rc.message : "HTTP request failed");
        cc_result_free(&rc);
        return cc_result_ok();
    }

    cc_json_value_t *result = cc_json_create_object();
    cc_json_object_set(result, "status", cc_json_create_number((double)response.status_code));
    cc_json_object_set(result, "body", cc_json_create_string(response.body ? response.body : ""));
    out_result->content = cc_json_stringify_unformatted(result);
    cc_json_destroy(result);

    out_result->ok = (response.status_code >= 200 && response.status_code < 300) ? 1 : 0;
    if (!out_result->ok) {
        out_result->error = strdup("HTTP status outside 2xx");
    }
    cc_http_response_free(&response);
    return cc_result_ok();
}

/**
 * http_req_destroy — vtable 方法：销毁 http.request 工具实例
 *
 * 功能：释放工具实例占用的内存。
 *       当前实现仅需释放结构体自身。
 *
 * @param self 工具实例指针，由 cc_http_request_tool_create 分配
 */
static void http_req_destroy(void *self) { free(self); }

/**
 * http_req_vtable — http.request 工具的虚拟方法表
 *
 * 说明：
 *   - 将 5 个静态函数绑定为 cc_tool_vtable_t 接口的实现
 *   - 使用 Adapter 模式将 HTTP 请求功能适配为标准工具接口
 *   - 实现了 cc_tool_vtable_t 接口（5 个函数指针：name/description/schema_json/call/destroy）
 *
 * 函数绑定关系：
 *   - name         → http_req_name
 *   - description  → http_req_description
 *   - schema_json  → http_req_schema_json
 *   - call         → http_req_call
 *   - destroy      → http_req_destroy
 */
static cc_tool_vtable_t http_req_vtable = {
    http_req_name,
    http_req_description,
    http_req_schema_json,
    http_req_call,
    http_req_destroy
};

/**
 * cc_http_request_tool_create — 创建 http.request 工具实例（工厂函数）
 *
 * 功能：
 *   1. 分配并零初始化 cc_http_request_tool_t 结构体
 *   2. 填充 cc_tool_t 输出参数，设置 self 指针和 vtable
 *
 * 与同类工厂函数（cc_file_read_tool_create 等）的差异：
 *   - 本工厂函数不需要依赖注入参数（如 fs、sandbox），
 *     因为网络能力通过 cc_http_client 端口在编译期选择
 *
 * @param out_tool 输出参数，创建成功后包含：
 *                 - self：指向 cc_http_request_tool_t 的指针
 *                 - vtable：指向 http_req_vtable 的指针
 *
 * @return cc_result_t
 *   - CC_OK：创建成功
 *   - CC_ERR_OUT_OF_MEMORY：calloc 分配内存失败
 */
cc_result_t cc_http_request_tool_create(cc_tool_t *out_tool)
{
    cc_http_request_tool_t *self = calloc(1, sizeof(cc_http_request_tool_t));
    if (!self) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create http request tool");
    out_tool->self = self;
    out_tool->vtable = &http_req_vtable;
    return cc_result_ok();
}
