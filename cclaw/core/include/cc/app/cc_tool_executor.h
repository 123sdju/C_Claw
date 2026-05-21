/**
 * 学习导读：cclaw/core/include/cc/app/cc_tool_executor.h
 *
 * 所属层次：核心层。
 * 阅读重点：这里定义 Agent 运行时的数据模型、主循环和通用工具，阅读时重点看所有权、错误返回和 ReAct 数据流。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * cc_tool_executor.h — 工具调用执行编排模块
 *
 * @file    cc/app/cc_tool_executor.h
 * @brief   编排工具调用的完整生命周期：查找工具 → 策略检查 → pool acquire → vtable->call → 事件发布。
 *
 * 当 LLM 返回一个 tool_call 请求时，本模块接管整个工具执行流程。
 * 它不关心具体工具是文件工具、plugin worker、MCP bridge 还是 shell adapter，
 * 而是作为编排器统一处理 registry lookup、policy 检查、tool executor pool
 * lane 并发、cancel token/timeout 注入和事件审计。
 *
 * ─── 接口契约 ─────────────────────────────────────────────────────────
 *
 *   - cc_tool_executor_execute() 是唯一对外接口
 *   - 传入 runtime（含 registry、policy、tool_pool、event_bus 等组件）、
 *     session_id 和 call（LLM 返回的工具调用请求）
 *   - 输出 cc_tool_result_t（包含 ok/content/error 信息）
 *
 * ─── 执行流程 ─────────────────────────────────────────────────────────
 *
 *   ┌─ Step 1: 工具查找 ──────────────────────────────────────────────┐
 *   │  通过 runtime->tool_registry 查找 cc_tool_call_t.name 对应的     │
 *   │  工具 vtable。找不到 → 返回 error result                         │
 *   └──────────────────────────────────────────────────────────────────┘
 *                                    │
 *                                    ▼
 *   ┌─ Step 2: 策略评估 ──────────────────────────────────────────────┐
 *   │  调用 policy.check_tool_call() 检查此工具调用是否被允许。        │
 *   │  策略可能检查：参数合法性、文件路径是否在 workspace 内、          │
 *   │  是否包含危险命令等。评估不通过 → 返回 out_result.ok=0。         │
 *   │  如果策略要求人工审批，executor 通过 services.approve_tool_call  │
 *   │  询问 app/gateway；没有审批通道时返回拒绝原因。                 │
 *   └──────────────────────────────────────────────────────────────────┘
 *                                    │
 *                                    ▼
 *   ┌─ Step 3: Pool 与上下文 ─────────────────────────────────────────┐
 *   │  根据工具名进入 tool.<name> / plugin.<id> / mcp.<server> lane。 │
 *   │  acquire 成功后构造 cc_tool_context_t，注入 services、           │
 *   │  cancel_token、timeout_ms、lane_name 和 generation。             │
 *   └──────────────────────────────────────────────────────────────────┘
 *                                    │
 *                                    ▼
 *   ┌─ Step 4: 工具调用 ──────────────────────────────────────────────┐
 *   │  调用 tool.vtable->call(self, args_json, &ctx, out_result)。     │
 *   │  shell 的隔离由 shell adapter/sandbox 完成；plugin/MCP 的进程、  │
 *   │  pipe、HTTP 取消由各自 app/platform adapter 处理。               │
 *   │  executor 只负责统一入口、策略、并发、timeout 和事件。           │
 *   └──────────────────────────────────────────────────────────────────┘
 *                                    │
 *                                    ▼
 *   返回 cc_tool_result_t { ok/content/error }
 *
 * ─── 依赖 ─────────────────────────────────────────────────────────────
 *
 *   依赖 cc/core/cc_result.h、cc/core/cc_tool_call.h、
 *        cc/app/cc_agent_runtime.h。
 *   内部访问 runtime->tool_registry、runtime->policy、
 *   runtime->tool_pool、runtime->services、runtime->event_bus 等组件。
 */

#ifndef CC_TOOL_EXECUTOR_H
#define CC_TOOL_EXECUTOR_H

#include "cc/core/cc_result.h"
#include "cc/core/cc_tool_call.h"
#include "cc/app/cc_agent_runtime.h"
#include "cc/app/cc_cancel_token.h"

typedef struct cc_tool_executor_options {
    cc_cancel_token_t *cancel_token;
} cc_tool_executor_options_t;

cc_result_t cc_tool_executor_execute_with_options(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const cc_tool_call_t *call,
    const cc_tool_executor_options_t *options,
    cc_tool_result_t *out_result
);

/**
 * cc_tool_executor_execute — 执行一次完整的工具调用
 *
 * 这是工具执行的统一入口。给定一个 LLM 返回的工具调用请求，
 * 自动完成：工具注册表查找 → 安全策略评估 → tool pool acquire →
 * 工具 vtable 调用 → 事件发布。调用方只需传入 runtime、session_id 和 call，
 * 即可获得标准化的工具执行结果。
 *
 * 各阶段细节：
 *   1. 工具查找：从 runtime->tool_registry 中按 call->name 查找工具 vtable
 *   2. 策略评估：调用 runtime->policy.check_tool_call() 检查执行权限
 *   3. Pool 策略：按 lane 获取并发令牌，并把 timeout/cancel 注入 ctx
 *   4. 工具调用：调用 tool.vtable->call()
 *   5. 事件发布：通过 runtime->event_bus 发布 started/finished 事件
 *
 * @param runtime     Agent 运行时实例（不可为 NULL），提供 registry/policy/tool_pool 等组件
 * @param session_id  当前会话 ID（不可为 NULL），用于事件和日志关联
 * @param call        LLM 返回的工具调用请求（不可为 NULL）
 * @param out_result  输出：工具执行结果（调用者负责 cc_tool_result_destroy）
 * @return            CC_OK 表示工具已执行（结果在 out_result 中，ok 字段标识成功/失败）
 */
cc_result_t cc_tool_executor_execute(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const cc_tool_call_t *call,
    cc_tool_result_t *out_result
);

#endif
