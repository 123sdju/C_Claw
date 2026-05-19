/**
 * 学习导读：apps/posix/cli/src/plugin/cc_jsonrpc_client.c
 *
 * 所属层次：POSIX CLI 应用层。
 * 阅读重点：这里组装桌面 CLI、工具、插件和 sandbox，阅读时重点看 main 到 runtime builder 的组合流程。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/******************************************************************************
 * cc_jsonrpc_client.c — JSON-RPC 客户端模块
 *
 * 本模块实现了 JSON-RPC 2.0 协议的客户端（调用方）功能，用于与外部插件
 * 子进程进行结构化通信。它是插件系统通信协议栈的"编解码层"。
 *
 * ── 在整体插件通信栈中的位置 ──
 *
 *   cc_plugin_tool.c (工具 vtable 适配层)
 *     │  调用 cc_plugin_process_send() / cc_plugin_process_receive()
 *     │  即通过管道发送和接收原始字符串
 *     ▼
 *   本模块 (编解码层)                          ← 你现在看的文件
 *     │  build_request: 将 method + params 编码为 {"jsonrpc":..., ...}
 *     │  parse_response: 将响应 JSON 解码为 result / error 两部分
 *     ▼
 *   cc_plugin_process.c (传输层)
 *     │  fork+exec 子进程, pipe(fd) 双向通信
 *     │  fprintf + fflush 写入, getline 读取
 *     ▼
 *   插件子进程 (任意语言实现)
 *     stdin  ← JSON-RPC 请求 (每行一个完整 JSON)
 *     stdout → JSON-RPC 响应 (每行一个完整 JSON)
 *
 * ── 为什么必须用单行 JSON（cc_json_stringify_unformatted）──
 *
 *   这是本模块最容易踩的坑。插件通信协议是**行协议**（line protocol）：
 *   每一条 JSON-RPC 消息独占一行，以换行符 '\n' 分隔。这意味着：
 *     - 请求体不能包含任何未转义的换行符
 *     - cJSON_Print() 输出的格式化 JSON 会在花括号、逗号后插入 '\n'
 *     - 子进程按行读取 stdin，读到不完整的半行 JSON（如单独一个 '{'）
 *       会立刻解析失败
 *
 *   因此本模块的 build_request 强制使用 cc_json_stringify_unformatted()，
 *   确保输出的 JSON 字符串是**紧凑单行**格式。
 *
 * ── JSON-RPC 2.0 协议要点 ──
 *
 *   请求必须包含四个字段：
 *     jsonrpc — 固定为 "2.0"
 *     id      — 请求标识符（字符串），用于关联请求和响应
 *     method  — 要调用的方法名，对应插件暴露的工具名
 *     params  — 方法参数，JSON 对象（不能省略，无参时传 {}）
 *
 *   响应分为两种互斥形式：
 *     成功：{"jsonrpc":"2.0","id":"...","result":<任意JSON值>}
 *     失败：{"jsonrpc":"2.0","id":"...","error":{"code":N,"message":"..."}}
 *
 *   result 和 error 不会同时出现。有 result 就是成功，有 error 就是失败。
 *
 * ── 依赖 ──
 *
 *   cc/util/cc_json.h — JSON 构造/解析能力，封装 cJSON
 *   stdlib / string    — malloc, strdup, snprintf 等
 *****************************************************************************/

#include "cc/util/cc_json.h"
#include "cc/util/cc_string_builder.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/*
 * 全局请求 ID 计数器。
 *
 * 每次调用 cc_jsonrpc_build_request 时自增，确保同一个插件进程内的
 * 每个 JSON-RPC 请求都有唯一的 id。虽然当前 Agent 循环是单线程同步的
 * （发一个请求 → 等响应 → 发下一个），不需要 id 来匹配顺序，但遵循
 * JSON-RPC 2.0 规范要求，同时为将来可能的异步/并发调用预留基础。
 */
static int next_id = 1;

/**
 * cc_jsonrpc_build_request — 构建 JSON-RPC 2.0 请求报文
 *
 * 将方法调用信息编码为 JSON-RPC 2.0 请求字符串。
 *
 * 构建流程：
 *   1. 创建空的 JSON 对象 req = {}
 *   2. 填入固定字段：jsonrpc="2.0", id=<自增>, method=<方法名>
 *   3. 处理 params 字段（三种情况见下）
 *   4. 使用 cc_json_stringify_unformatted() 序列化为单行 JSON
 *   5. 销毁临时 JSON 树，返回 C 字符串
 *
 * params 字段的三种处理路径：
 *
 *   (a) params_json 非 NULL 且是合法 JSON
 *       → 解析为 JSON 对象，嵌入 req 的 "params" 字段
 *       → 例如 params_json='{"city":"Beijing"}' → params={"city":"Beijing"}
 *
 *   (b) params_json 非 NULL 但解析失败（格式错误）
 *       → 作为容错处理，使用空对象 {} 作为 params
 *       → 这样即使上层传入了格式有误的参数，插件也能收到有效请求
 *       → 释放 cc_result_t 避免错误信息泄漏
 *
 *   (c) params_json 为 NULL
 *       → 使用空对象 {} 作为 params（JSON-RPC 要求 params 不能省略）
 *
 * ── 关键实现细节 ──
 *
 * 【为什么用 unformatted】行协议要求请求必须是单行 JSON。如果使用
 * cJSON_Print（即 cc_json_stringify），输出会包含换行和缩进，导致
 * 子进程 stdin 一行内只读到部分 JSON 而解析失败。这是之前 "Parse error"
 * bug 的根因。
 *
 * 【为什么 params 不能省】JSON-RPC 2.0 规范允许省略 params，但某些插件
 * 实现（如 Python 的 json.loads + request.get("params", {})）依赖
 * params 键存在。显式传入 {} 更健壮。
 *
 * @param method       要调用的远程方法名称（对应插件的工具名）
 * @param params_json  参数的 JSON 字符串表示，可为 NULL 表示无参
 * @return             堆上分配的 JSON 请求字符串，调用者负责 free()
 */
/**
 * cc_jsonrpc_build_request — 构造单行 JSON-RPC 2.0 请求字符串，调用方负责 free。
 *
 * @param method 借用的远程方法名。
 * @param params_json 借用的参数 JSON；NULL 时使用空对象。
 * @return 新分配请求字符串；失败返回 NULL。
 */
char *cc_jsonrpc_build_request(const char *method, const char *params_json)
{
    cc_json_value_t *req = cc_json_create_object();

    /* 填入 JSON-RPC 2.0 协议固定字段 */
    cc_json_object_set(req, "jsonrpc", cc_json_create_string("2.0"));

    char id_buf[32];
    snprintf(id_buf, sizeof(id_buf), "%d", next_id++);
    cc_json_object_set(req, "id", cc_json_create_string(id_buf));

    cc_json_object_set(req, "method", cc_json_create_string(method));

    /*
     * 处理 params 字段 — 三种情况：
     *   (a) 有效 params_json → 解析并嵌入
     *   (b) 无效 params_json → 容错，使用 {}
     *   (c) params_json 为 NULL → 使用 {}
     */
    if (params_json) {
        cc_json_value_t *params = NULL;
        cc_result_t rc = cc_json_parse(params_json, &params);
        if (rc.code == CC_OK && params) {
            /* (a) 解析成功，params 所有权转移给 req */
            cc_json_object_set(req, "params", params);
        } else {
            /* (b) 解析失败，容错给空对象 */
            cc_json_object_set(req, "params", cc_json_create_object());
            cc_result_free(&rc);
        }
    } else {
        /* (c) 无参数，显式传入空对象 */
        cc_json_object_set(req, "params", cc_json_create_object());
    }

    /*
     * 关键：必须使用 unformatted 版本！
     * cJSON_Print（cc_json_stringify）产生多行缩进 JSON，会破坏行协议。
     * cJSON_PrintUnformatted（cc_json_stringify_unformatted）产生紧凑单行。
     */
    char *result = cc_json_stringify_unformatted(req);
    cc_json_destroy(req);
    return result;
}

/**
 * cc_jsonrpc_parse_response — 解析 JSON-RPC 2.0 响应报文
 *
 * 将插件子进程返回的 JSON 字符串解析为两部分：result（成功结果）和
 * error（错误信息）。二者互斥，永远只有一个非 NULL。
 *
 * 解析流程：
 *   1. 将原始 JSON 字符串解析为 JSON 值树
 *   2. 检查并提取 "result" 字段 → 序列化为字符串 → 存入 out_result_json
 *   3. 检查并提取 "error" 字段  → 序列化为字符串 → 存入 out_error_json
 *   4. 销毁 JSON 值树
 *
 * 为什么用 cc_json_stringify 而不是 cc_json_stringify_unformatted？
 *   这里序列化的是响应内容，不需要再经过管道发送，所以用格式化版本
 *   也没关系。result/error 字段的内容会显示给 LLM，多行 JSON 不影响。
 *
 * 典型调用示例：
 *   char *result = NULL, *error = NULL;
 *   cc_result_t rc = cc_jsonrpc_parse_response(response_json, &result, &error);
 *   if (rc.code == CC_OK) {
 *       if (result) printf("成功: %s\n", result);
 *       if (error)  printf("失败: %s\n", error);
 *   }
 *   cc_jsonrpc_free_result(result, error);
 *
 * @param response_json   插件返回的原始 JSON 响应字符串
 * @param out_result_json 输出：result 字段的 JSON 字符串（调用者释放）
 * @param out_error_json  输出：error  字段的 JSON 字符串（调用者释放）
 * @return                CC_OK — 解析过程正常（不代表调用成功，要检查 error）
 *                        其他 — JSON 解析本身错误（响应不是合法 JSON）
 */
/**
 * cc_jsonrpc_parse_response — 解析插件返回的 JSON-RPC 响应并拆出 result/error 字段。
 *
 * @param response_json 借用的响应 JSON 文本。
 * @param out_result_json 输出 result JSON 字符串；调用方负责 free。
 * @param out_error_json 输出 error JSON 字符串；调用方负责 free。
 * @return CC_OK 表示响应 JSON 可解析；调用是否失败需检查 out_error_json。
 */
cc_result_t cc_jsonrpc_parse_response(
    const char *response_json,
    char **out_result_json,
    char **out_error_json
)
{
    /* 先初始化为 NULL，防止调用者读到垃圾值 */
    *out_result_json = NULL;
    *out_error_json = NULL;

    cc_json_value_t *resp = NULL;
    cc_result_t rc = cc_json_parse(response_json, &resp);
    if (rc.code != CC_OK) return rc;

    /* 提取 result（成功）字段 — 借用引用，不拥有所有权 */
    cc_json_value_t *result = cc_json_object_get(resp, "result");
    if (result) {
        *out_result_json = cc_json_stringify(result);
    }

    /* 提取 error（失败）字段 — result 和 error 互斥 */
    cc_json_value_t *error = cc_json_object_get(resp, "error");
    if (error) {
        *out_error_json = cc_json_stringify(error);
    }

    cc_json_destroy(resp);
    return cc_result_ok();
}

/**
 * cc_jsonrpc_free_result — 释放响应解析结果
 *
 * 与 cc_jsonrpc_parse_response 配对使用的清理函数。
 * 两个参数都可以是 NULL（free(NULL) 是安全的空操作），
 * 因此调用者不需要做空指针检查。
 *
 * @param result_json  result 字段的 JSON 字符串（可为 NULL）
 * @param error_json   error  字段的 JSON 字符串（可为 NULL）
 */
void cc_jsonrpc_free_result(char *result_json, char *error_json)
{
    free(result_json);
    free(error_json);
}