/**
 * 学习导读：apps/windows/cli/src/tools/cc_plugin_tool.c
 *
 * 所属层次：Windows CLI 应用层。
 * 阅读重点：这里镜像桌面 CLI 能力但使用 Windows 平台实现，阅读时重点比较与 POSIX 版本的差异。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/******************************************************************************
 * cc_plugin_tool.c — 插件工具适配器
 *
 * 本模块将外部插件进程包装为符合 cc_tool_vtable_t 接口的标准工具。
 * 当一个工具被 LLM 调用时，本适配器通过 JSON-RPC 协议与插件子进程通信：
 *
 *   LLM tool_call
 *     → plugin_tool_call()
 *       → cc_jsonrpc_build_request(tool_name, args_json)
 *       → cc_plugin_process_send(request_json)
 *       → cc_plugin_process_receive(&response_json)
 *       → cc_jsonrpc_parse_response(response_json, &result, &error)
 *       → 填充 cc_tool_result_t 返回
 *
 * 多个工具可以共享同一个插件进程，因为它们使用不同的 method 名称
 * 来区分调用目标。这允许一个插件脚本暴露多个工具。
 *
 * 设计决策：
 *   - 每个工具独立持有 process 指针（共享、不拥有），进程生命周期由
 *     cc_plugin_manager 管理
 *   - JSON-RPC 是同步阻塞调用，适配当前单线程 Agent 循环
 *   - 插件进程崩溃时，工具调用返回错误，不会导致主进程崩溃
 *****************************************************************************/

/*
 *   - cc_tool_vtable_t（5 个虚拟方法：name / description / schema_json / call / destroy）
 *
 * 安全注意：
 *   - 插件进程崩溃时，工具调用返回错误，不会导致主进程崩溃
 *   - process 指针由 cc_plugin_manager 统一管理，cc_plugin_tool 不拥有其所有权
 *   - JSON-RPC 是同步阻塞调用，适配当前单线程 Agent 循环
 */

#include "cc/ports/cc_tool.h"
#include "cc/plugin/cc_plugin_process.h"
#include "cc/plugin/cc_jsonrpc_client.h"
#include <stdlib.h>
#include <string.h>

/*
 * cc_plugin_tool_t — 插件工具的内部数据结构
 *
 * 字段说明：
 *   plugin_name       — 所属插件的名称（用于日志和调试）
 *   tool_name         — 工具的唯一名称，同时作为 JSON-RPC 的 method 名称
 *   tool_description  — 工具的自然语言描述，供 LLM 理解工具用途
 *   tool_schema_json  — 工具参数的 JSON Schema，定义输入参数规范
 *   process           — 关联的插件进程句柄（共享指针，不拥有所有权），
 *                        用于通过 JSON-RPC 协议与子进程通信
 */
typedef struct {
    char *plugin_name;
    char *tool_name;
    char *tool_description;
    char *tool_schema_json;
    cc_plugin_process_t *process;
} cc_plugin_tool_t;

/*
 * plugin_tool_name — vtable 方法：返回工具名称
 *
 * 功能：返回该工具的唯一标识名称，同时也是 JSON-RPC 通信时的 method 名称。
 *       名称从创建时传入的 tool_name 字段获取。
 *
 * @param self 工具实例指针
 * @return 工具名称字符串（如 "weather"、"calculator" 等）
 */
static const char *plugin_tool_name(void *self)
{
    cc_plugin_tool_t *tool = (cc_plugin_tool_t *)self;
    return tool->tool_name;
}

/*
 * plugin_tool_description — vtable 方法：返回工具描述
 *
 * 功能：返回工具的自然语言描述，供 LLM 理解工具功能。
 *       描述由插件在注册时提供，存储在 tool_description 字符串中。
 *
 * @param self 工具实例指针
 * @return 工具描述字符串
 */
static const char *plugin_tool_description(void *self)
{
    cc_plugin_tool_t *tool = (cc_plugin_tool_t *)self;
    return tool->tool_description;
}

/*
 * plugin_tool_schema_json — vtable 方法：返回工具参数的 JSON Schema
 *
 * 功能：返回工具调用参数的 JSON Schema 定义，供 LLM 了解如何构造参数。
 *       Schema 由插件在注册时提供。
 *
 * @param self 工具实例指针
 * @return JSON Schema 字符串
 */
static const char *plugin_tool_schema_json(void *self)
{
    cc_plugin_tool_t *tool = (cc_plugin_tool_t *)self;
    return tool->tool_schema_json;
}

/*
 * plugin_tool_call — vtable 方法：通过 JSON-RPC 桥接调用插件工具
 *
 * 功能：这是 vtable 与插件进程之间的 JSON-RPC 桥接层。执行流程：
 *   1. 构造 JSON-RPC 请求：cc_jsonrpc_build_request(tool_name, args_json)
 *      - 将工具名称作为 JSON-RPC method 名称
 *      - 将 LLM 传入的 args_json 作为 JSON-RPC params
 *   2. 发送到插件进程：cc_plugin_process_call(process, request_json, &response_json)
 *      - 同步阻塞调用，等待插件进程返回结果
 *      - process 为 NULL 时（如简化创建路径）直接返回错误
 *   3. 解析 JSON-RPC 响应：cc_jsonrpc_parse_response(response_json, &result, &error)
 *      - 提取 result 字段作为成功输出
 *      - 提取 error 字段作为错误输出
 *   4. 填充 cc_tool_result_t：将解析结果映射到标准工具返回结构体
 *
 * @param self      工具实例指针
 * @param args_json LLM 传入的 JSON 格式调用参数（可含任意插件定义的结构）
 * @param ctx       工具上下文（当前插件工具未使用此参数）
 * @param out_result 输出结果结构体：
 *                   - ok=1 表示调用成功，content 为插件返回的 result 字段
 *                   - ok=0 表示通信或执行错误，error 包含错误描述
 *
 * @return cc_result_t 始终返回 OK（插件执行错误通过 out_result->ok=0 表达）
 *
 * 容错机制：
 *   - 进程通信失败 → out_result->ok=0, error="Plugin communication error"
 *   - JSON-RPC 响应解析失败 → out_result->ok=0, error="Failed to parse..."
 *   - 插件返回 error → out_result->ok=0, error 为插件端返回的错误内容
 */
/**
 * plugin_tool_call — 把工具调用参数封装成 JSON-RPC 请求并交给插件进程执行。
 *
 * @param self 插件工具私有状态。
 * @param args_json 借用的工具参数 JSON；NULL 时使用空对象。
 * @param ctx 工具上下文；当前实现不直接使用。
 * @param out_result 输出工具结果；插件错误通过 ok/error 表达。
 * @return CC_OK 表示通信流程已转换成工具结果。
 */
static cc_result_t plugin_tool_call(
    void *self,
    const char *args_json,
    const cc_tool_context_t *ctx,
    cc_tool_result_t *out_result
)
{
    (void)ctx;
    cc_plugin_tool_t *tool = (cc_plugin_tool_t *)self;

    memset(out_result, 0, sizeof(cc_tool_result_t));

    char *request_json = cc_jsonrpc_build_request(
        tool->tool_name, args_json ? args_json : "{}");

    char *response_json = NULL;
    cc_result_t rc = cc_plugin_process_call(tool->process, request_json, &response_json);
    free(request_json);

    if (rc.code != CC_OK) {
        out_result->ok = 0;
        out_result->error = strdup(rc.message ? rc.message : "Plugin communication error");
        cc_result_free(&rc);
        return cc_result_ok();
    }

    char *result_json = NULL;
    char *error_json = NULL;
    rc = cc_jsonrpc_parse_response(response_json, &result_json, &error_json);
    free(response_json);

    if (rc.code != CC_OK) {
        out_result->ok = 0;
        out_result->error = strdup("Failed to parse plugin JSON-RPC response");
        cc_result_free(&rc);
        return cc_result_ok();
    }

    if (error_json) {
        out_result->ok = 0;
        out_result->error = error_json;
        free(result_json);
        return cc_result_ok();
    }

    out_result->ok = 1;
    out_result->content = result_json ? result_json : strdup("{}");
    return cc_result_ok();
}

/*
 * plugin_tool_destroy — vtable 方法：销毁插件工具实例
 *
 * 功能：释放工具实例持有的所有字符串内存（plugin_name、tool_name 等）。
 *       注意：不释放 process 指针（由 cc_plugin_manager 管理生命周期）。
 *
 * @param self 工具实例指针
 */
static void plugin_tool_destroy(void *self)
{
    cc_plugin_tool_t *tool = (cc_plugin_tool_t *)self;
    if (!tool) return;
    free(tool->plugin_name);
    free(tool->tool_name);
    free(tool->tool_description);
    free(tool->tool_schema_json);
    free(tool);
}

/*
 * plugin_tool_vtable — 插件工具的虚函数表
 *
 * 说明：将 5 个静态函数绑定为 cc_tool_vtable_t 接口的实现。
 *       插件工具的 name/description/schema 均来自插件注册信息，
 *       call 通过 JSON-RPC 桥接同步调用插件进程。
 */
static cc_tool_vtable_t plugin_tool_vtable = {
    plugin_tool_name,
    plugin_tool_description,
    plugin_tool_schema_json,
    plugin_tool_call,
    plugin_tool_destroy
};

/*
 * cc_plugin_tool_create_full — 创建插件工具实例（完整工厂函数）
 *
 * 功能：
 *   1. 分配并零初始化 cc_plugin_tool_t 结构体
 *   2. 将插件名称、工具名称、描述、Schema 等信息深拷贝到实例中
 *   3. 绑定插件进程句柄（共享所有权模式）
 *   4. 填充 cc_tool_t 输出参数
 *
 * 参数：
 *   plugin_name       — 所属插件名称（如 "weather"，用于调试和日志）
 *   tool_name         — 工具唯一名称，同时也是 JSON-RPC method 名称
 *   tool_description  — 工具的自然语言描述，供 LLM 理解
 *   tool_schema_json  — 工具参数的 JSON Schema 定义
 *   process           — 插件进程句柄（共享指针，可被多个工具共享），
 *                        通过它进行 JSON-RPC 通信
 *   out_tool          — 输出参数，创建成功后包含工具 self 指针和 vtable
 *
 * 返回值：cc_result_t，成功返回 CC_OK，内存不足返回 CC_ERR_OUT_OF_MEMORY
 *
 * 设计决策：
 *   - process 指针为空时仍可成功创建，但在调用时将返回通信错误
 *   - 所有字符串均使用 strdup 深拷贝，确保工具实例独立于外部数据
 */
/**
 * cc_plugin_tool_create_full — 创建一个绑定到插件进程的工具实例并深拷贝元数据。
 *
 * @param plugin_name 借用插件名。
 * @param tool_name 借用工具名。
 * @param tool_description 借用工具描述。
 * @param tool_schema_json 借用工具 schema JSON。
 * @param process 借用插件进程句柄；工具调用时使用。
 * @param out_tool 输出工具端口；成功后由 registry 接管。
 * @return CC_OK 表示创建成功；失败返回内存错误。
 */
cc_result_t cc_plugin_tool_create_full(
    const char *plugin_name,
    const char *tool_name,
    const char *tool_description,
    const char *tool_schema_json,
    cc_plugin_process_t *process,
    cc_tool_t *out_tool
)
{
    cc_plugin_tool_t *self = calloc(1, sizeof(cc_plugin_tool_t));
    if (!self) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create plugin tool");

    self->plugin_name = plugin_name ? strdup(plugin_name) : strdup("unknown");
    self->tool_name = tool_name ? strdup(tool_name) : strdup("unknown");
    self->tool_description = tool_description ? strdup(tool_description) : strdup("No description");
    self->tool_schema_json = tool_schema_json ? strdup(tool_schema_json) : strdup("{}");
    self->process = process;

    out_tool->self = self;
    out_tool->vtable = &plugin_tool_vtable;
    return cc_result_ok();
}

/*
 * cc_plugin_tool_create — 创建插件工具实例（简化工厂函数）
 *
 * 功能：使用默认参数创建插件工具，process 为 NULL（不可通过 JSON-RPC 调用）。
 *       该路径通常用于占位或测试目的。
 *
 * 参数：
 *   name     — 工具名称
 *   out_tool — 输出参数
 *
 * 返回值：cc_result_t
 */
cc_result_t cc_plugin_tool_create(const char *name, cc_tool_t *out_tool)
{
    return cc_plugin_tool_create_full("plugin", name,
        "External plugin tool", "{\"type\":\"object\",\"properties\":{}}", NULL, out_tool);
}
