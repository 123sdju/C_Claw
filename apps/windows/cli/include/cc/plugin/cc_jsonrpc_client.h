/**
 * 学习导读：apps/windows/cli/include/cc/plugin/cc_jsonrpc_client.h
 *
 * 所属层次：Windows CLI 应用层。
 * 阅读重点：这里镜像桌面 CLI 能力但使用 Windows 平台实现，阅读时重点比较与 POSIX 版本的差异。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/******************************************************************************
 * cc_jsonrpc_client.h — JSON-RPC 2.0 客户端接口
 *
 * 本头文件声明了 JSON-RPC 2.0 协议的客户端编码/解码函数。
 * 用于 cc_plugin_tool 与外部插件子进程之间的结构化通信。
 *
 * ── 使用场景 ──
 *
 *   1. 构建请求：cc_jsonrpc_build_request(method, params_json)
 *      将方法调用信息编码为 JSON-RPC 请求字符串
 *
 *   2. 发送请求：cc_plugin_process_send(process, request_json)
 *      通过管道将请求发送给插件子进程
 *
 *   3. 接收响应：cc_plugin_process_receive(process, &response_json)
 *      从管道读取插件子进程的响应
 *
 *   4. 解析响应：cc_jsonrpc_parse_response(response_json, &result, &error)
 *      将响应 JSON 解码为 result（成功）和 error（失败）两部分
 *
 *   5. 释放结果：cc_jsonrpc_free_result(result, error)
 *      释放解析结果占用的内存
 *
 * ── 依赖 ──
 *
 *   cc/core/cc_result.h — 统一错误处理类型 cc_result_t
 *****************************************************************************/

#ifndef CC_JSONRPC_CLIENT_H
#define CC_JSONRPC_CLIENT_H

#include "cc/core/cc_result.h"

/**
 * 构建 JSON-RPC 2.0 请求报文。
 *
 * 生成紧凑单行 JSON（使用 cJSON_PrintUnformatted），
 * 适合通过管道行协议发送。
 *
 * @param method      方法名（对应插件工具名）
 * @param params_json 参数 JSON 字符串，可为 NULL（无参）
 * @return            堆上分配的请求 JSON 字符串，调用者负责 free()
 */
char *cc_jsonrpc_build_request(const char *method, const char *params_json);

/**
 * 解析 JSON-RPC 2.0 响应报文。
 *
 * 从响应 JSON 中提取 result（成功）和 error（失败）字段。
 * 二者互斥：成功时 out_result_json 非 NULL、out_error_json 为 NULL；
 * 失败时相反。
 *
 * @param response_json   插件返回的原始 JSON 响应
 * @param out_result_json 输出：result 字段的 JSON 字符串（调用者释放）
 * @param out_error_json  输出：error  字段的 JSON 字符串（调用者释放）
 * @return                CC_OK 解析完成（不代表调用成功），否则为 JSON 解析错误
 */
cc_result_t cc_jsonrpc_parse_response(
    const char *response_json,
    char **out_result_json,
    char **out_error_json
);

/**
 * 释放 cc_jsonrpc_parse_response 的输出。
 *
 * 两个参数都可以是 NULL（free(NULL) 安全），调用者无需检查。
 *
 * @param result_json result 字段的 JSON 字符串
 * @param error_json  error  字段的 JSON 字符串
 */
void cc_jsonrpc_free_result(char *result_json, char *error_json);

#endif