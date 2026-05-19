/**
 * 学习导读：cclaw/core/src/app/cc_tool_executor.c
 *
 * 所属层次：核心层。
 * 阅读重点：这里定义 Agent 运行时的数据模型、主循环和通用工具，阅读时重点看所有权、错误返回和 ReAct 数据流。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * ===========================================================================
 * cc_tool_executor.c — 工具执行调度器
 * ===========================================================================
 *
 * 模块在整体架构中的角色：
 * ─────────────────────────────
 * 本模块是 Agent 工具调用链的"执行调度引擎"——当 LLM 决定调用某个工具时，
 * 主循环将控制权交给本模块，由它统一完成工具的查找、安全审查、执行和事件发布。
 * 它是 cc_tool_executor.h 的唯一实现。
 *
 * 本模块在工具调用的完整链路上处于"中间人"位置：
 *   LLM (tool_call) → agent_runtime → tool_executor → tool_registry → tool → 结果
 *                                                       ↑
 *                                                   policy_engine（安全审查）
 *                                                       ↑
 *                                                   event_bus（事件发布）
 *
 * 上游调用方：
 *   - cc_agent_runtime.c 的 handle_message 函数 —— 在 ReAct 循环的分支 A
 *     （response.has_tool_call == true）中调用 cc_tool_executor_execute
 *
 * 下游依赖模块：
 *   - cc_tool_registry —— 按名称查找工具（cc_tool_registry_find）
 *   - cc_policy_engine —— 工具调用前的安全策略检查（check_tool_call）
 *   - cc_event_bus     —— 发布 "tool.call.started" / "tool.call.finished" 事件
 *   - cc_tool           —— 通过 vtable 调用工具的具体实现
 *   - cc_string_builder —— 构建事件 JSON 字符串
 *   - cc_json           —— JSON 解析/序列化
 *
 * 与其他模块的交互协议：
 * ──────────────────────
 * 本模块与 agent_runtime 之间存在一个"宽容错误"契约：
 *   - 无论工具执行成功还是失败（包括找不到工具或被策略拒绝），
 *     本函数始终返回 cc_result_ok()，通过 out_result->ok 字段来区分结果。
 *   - 这意味着 agent_runtime 的主循环永远不会因为工具层面的失败而中断，
 *     工具错误会被转换为对话上下文的一部分（role="tool" + content=error），
 *     让 LLM 有机会从错误中学习并调整后续行为。
 *
 * 为什么采用"宽容错误"而非传播错误：
 *   - LLM 的 tool_call 参数可能不正确（如参数类型错误），工具执行失败
 *     是系统预期的正常情况，不应视为系统级错误。
 *   - 如果传播错误导致主循环中断，整个 Agent 会话将终止，
 *     用户需要重新开始——这对于用户体验是不可接受的。
 *   - ReAct 模式的设计哲学之一就是"容错"：LLM 看到错误结果后
 *     可以调整参数重试，或换用其他方案。
 *
 * 安全模型（五层防护）：
 * ──────────────────────
 *   1. 工具注册层：只能调用已注册的工具（cc_tool_registry_find）
 *   2. 策略引擎：policy.check_tool_call 进行运行时安全审查
 *   3. 参数验证：由各工具实现的 call 函数自行负责
 *   4. 沙箱隔离：(预留) sandbox 接口可用于限制工具的运行时环境
 *   5. 事件审计：通过 event_bus 发布所有工具调用的开始/结束事件
 */

#include "cc/app/cc_tool_executor.h"
#include "cc_agent_runtime_internal.h"
#include "cc/util/cc_json.h"
#include "cc/util/cc_string_builder.h"
#include "cc/ports/cc_event_bus.h"
#include <stdlib.h>
#include <string.h>

/**
 * publish_json_event — 在结构体与 JSON/文本之间转换，并负责字段校验和临时内存。
 *
 * 位置：工具适配层。注释重点说明当前函数的输入输出、资源边界和错误传播。
 *
 * @param bus 借用的指针参数；若需要长期保存内容，函数会复制。
 * @param event_type 借用的只读字符串；函数不会释放该指针。
 * @param payload 借用的指针参数；若需要长期保存内容，函数会复制。
 * 无返回值；副作用体现在对象状态、输出缓冲区或资源释放上。
 */
static void publish_json_event(
    cc_event_bus_t *bus,
    const char *event_type,
    cc_json_value_t *payload
)
{
    if (!payload) return;
    if (bus) {
        char *event_json = cc_json_stringify_unformatted(payload);
        if (event_json) {
            cc_event_bus_publish(bus, event_type, event_json);
            free(event_json);
        }
    }
    cc_json_destroy(payload);
}

/**
 * set_policy_error_result — 更新对象内部字段或输出结构，同时维护旧值释放规则。
 *
 * 位置：工具适配层。注释重点说明当前函数的输入输出、资源边界和错误传播。
 *
 * @param out_result 输出参数；成功时写入有效结果，失败时保持为 NULL 或未定义状态。
 * @param message 借用的对象；函数不释放该对象本身。
 * 无返回值；副作用体现在对象状态、输出缓冲区或资源释放上。
 */
static void set_policy_error_result(
    cc_tool_result_t *out_result,
    const char *message
)
{
    memset(out_result, 0, sizeof(cc_tool_result_t));
    out_result->ok = 0;
    out_result->error = strdup(message ? message : "Tool call denied by policy");
}

/**
 * cc_tool_executor_execute — 查找、审查并执行一个工具调用
 *
 * 功能：
 *   将 LLM 请求的工具调用（tool_call）分派给对应的工具实现执行。
 *   在执行前后进行工具查找、安全策略检查、事件发布等横切关注点处理。
 *
 * 这是整个系统中"工具调用"的唯一入口——所有 LLM 发起的工具调用
 * 都必须经过此函数。这确保了安全策略和事件审计的一致性。
 *
 * @param runtime    Agent 运行时实例，提供以下子组件：
 *                     - tool_registry: 工具注册表（查找工具）
 *                     - policy: 策略引擎接口（安全审查）
 *                     - event_bus: 事件总线（发布事件）
 *                     - config.workspace_dir: 工作空间目录（传给工具上下文）
 * @param session_id 当前会话 ID，传递给工具的上下文信息
 * @param call       LLM 返回的工具调用信息结构体：
 *                     - call->id: 工具调用唯一标识（由 LLM 生成）
 *                     - call->name: 工具名称（如 "get_weather"）
 *                     - call->arguments_json: JSON 格式的参数字符串
 * @param out_result 输出参数，工具执行结果：
 *                     - ok=1 → content 包含工具的 JSON 输出字符串
 *                     - ok=0 → error 包含错误描述字符串
 *
 * @return 始终返回 CC_OK。实际执行成功与否通过 out_result->ok 判断。
 *         这是"宽容错误"策略的体现——工具层面的失败不传播为系统错误。
 *
 * 算法步骤（7 步流水线）：
 * ────────────────────────
 *
 *   Step 1: 工具查找
 *     ● 在 tool_registry 中按 call->name 查找工具
 *     ● 找不到 → 填充 "Tool not found: {name}" 错误，返回 CC_OK
 *     WHY 不返回错误：让 LLM 看到 "Tool not found" 信息后可以尝试其他工具名。
 *
 *   Step 2: 构造工具上下文
 *     ● 组装 cc_tool_context_t，包含 session_id、workspace_dir、user_data
 *     ● user_data 指向 runtime 本身（工具可以通过 ctx.user_data 访问全局状态）
 *     WHY 传 runtime 而非仅传 workspace_dir：未来工具可能需要访问 event_bus、
 *     logger 等更多 runtime 资源。
 *
 *   Step 3: 策略引擎安全检查
 *     ● 如果 runtime->policy 存在，调用 policy.check_tool_call 进行安全审查
     *     ● 策略检查可能返回：
     *       - allowed=1 且 require_approval=0: 允许执行，继续后续步骤
     *       - allowed=0: 拒绝执行，返回拒绝原因
     *       - require_approval=1: 缺少交互审批通道时停止执行并返回审批原因
 *     WHY 策略检查在工具查找之后、实际执行之前：只有知道具体工具是什么，
 *     才能判断其风险等级。例如 "file_read" 安全，"run_command" 危险。
 *
 *   Step 4: 策略拒绝处理
 *     ● 如果策略拒绝（decision.allowed == false），
 *       将拒绝原因填充到 out_result->error，返回 CC_OK
 *     WHY 将拒绝原因放入 error 而非抛出异常：LLM 看到拒绝原因后
 *     可以调整策略（如换用更安全的工具）或向用户解释原因。
 *
 *   Step 5: 发布 "tool.call.started" 事件
 *     ● 事件 JSON 格式：
 *       {"session_id":"ses_xxx", "tool":"get_weather", "args":"{\"city\":\"北京\"}"}
 *     WHY 事件在策略检查通过后、执行前发布：确保"开始"事件只对
 *     实际执行的操作发布，被拒绝的操作不会产生 "started" 事件。
 *
 *   Step 6: 实际调用工具
 *     ● 通过 tool.vtable->call(tool.self, args_json, &ctx, &out_result)
 *     ● 这是多态调用——不同的工具有不同的 call 实现
 *     ● 如果工具返回错误（rc.code != CC_OK），将错误信息填入 out_result->error
 *     WHY 工具返回的错误也填入 out_result 而非传播：同"宽容错误"策略。
 *
 *   Step 7: 发布 "tool.call.finished" 事件
 *     ● 事件 JSON 格式：
 *       {"session_id":"ses_xxx", "tool":"get_weather", "ok":1}
 *     WHY 事件中包含 ok 字段：让事件监听方（UI/监控）能区分成功和失败。
 *
 * 典型调用流程示例：
 *
 *   // LLM 想要查天气，返回 tool_call
 *   cc_tool_call_t call = {
 *       .id = "call_abc123",
 *       .name = "get_weather",
 *       .arguments_json = "{\"city\":\"北京\"}"
 *   };
 *
 *   cc_tool_result_t result;
 *   cc_tool_executor_execute(runtime, "ses_xxx", &call, &result);
 *
 *   // 检查结果
 *   if (result.ok) {
 *       printf("工具执行成功: %s\n", result.content);  // {"temp":25,"weather":"晴"}
 *   } else {
 *       printf("工具执行失败: %s\n", result.error);   // "City not found"
 *   }
 *
 *   // 释放结果字符串
 *   free(result.content);
 *   free(result.error);
 *   free(result.metadata_json);
 *
 * 错误场景覆盖：
 *   - 工具未注册 → "Tool not found: xxx"
 *   - 策略拒绝   → "Tool call denied by policy" 或策略返回的自定义原因
 *   - 工具执行异常 → 工具返回的错误消息或 "Tool execution failed"
 *   - 工具返回成功但内容为空 → out_result.ok=1, content="" 或为 NULL
 */
cc_result_t cc_tool_executor_execute(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const cc_tool_call_t *call,
    cc_tool_result_t *out_result
)
{
    cc_tool_t tool;

    /*
     * Step 1: 在 tool_registry 中按名称查找工具
     *
     * cc_tool_registry_find 通过名称查找已注册的工具。
     * 如果找不到（如 LLM 给出了不存在的工具名），
     * 返回 CC_ERR_NOT_FOUND 错误码。
     *
     * 为什么先查注册表：
     *   这是安全第一层——只允许调用显式注册的工具。
     *   防止 LLM 幻觉出一个不存在的工具名，也防止调用未授权的工具。
     */
    cc_result_t rc = cc_tool_registry_find(
        runtime->tool_registry,
        call->name,
        &tool
    );

    /*
     * 工具未注册：填充错误结果并返回 CC_OK（宽容策略）
     *
     * 使用 cc_string_builder 构建格式化的错误信息，
     * 让 LLM 可以看到具体的工具名称，知道是哪个工具不存在。
     * cc_string_builder_take 将 sb 内部缓冲区转移给调用方，
     * 避免额外的 strdup 操作。
     */
    if (rc.code != CC_OK) {
        memset(out_result, 0, sizeof(cc_tool_result_t));
        cc_string_builder_t sb;
        cc_string_builder_init(&sb);
        cc_string_builder_appendf(&sb, "Tool not found: %s", call->name);
        out_result->ok = 0;
        out_result->error = cc_string_builder_take(&sb);
        return cc_result_ok();
    }

    /*
     * Step 2: 构造工具上下文，传递给工具实现
     *
     * cc_tool_context_t 是传递给每个工具调用实现的上下文信息。
     * 各个字段的用途：
     *   - session_id: 工具可能需要知道当前会话（如状态管理）
     *   - workspace_dir: 工具的文件操作根目录（如 file_read/file_write）
     *   - user_id: 当前未使用，预留给多用户场景
     *   - user_data: 指向 runtime 的指针，允许工具访问全局资源
     *     （如 event_bus、logger、其他工具等）
     *
     * WHY user_data 指向 runtime 而非 null：
     *   某些高级工具可能需要与其他系统组件交互（如发布事件、查询策略），
     *   通过 runtime 可以访问所有注册的组件。
     */
    cc_tool_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.session_id = session_id;
    ctx.workspace_dir = runtime->config.workspace_dir;
    ctx.user_id = NULL;
    ctx.services = &runtime->services;

    /*
     * Step 3: 策略引擎安全检查
     *
     * 策略引擎是可选的（runtime->policy.vtable 可能为 NULL）。
     * 如果存在，则在工具实际执行前进行安全审查。
     *
     * 策略引擎的 check_tool_call 函数接收：
     *   - call: 完整的工具调用信息（名称+参数）
     *   - ctx: 工具上下文（会话+工作目录）
     *   然后返回：
     *   - decision.allowed: 是否允许执行
     *   - decision.require_approval: 是否需要人工审批
     *   - decision.reason: 拒绝原因（allowed=0 时有意义）
     *
     * 检查 vtable 和 vtable->check_tool_call 都存在再调用：
     *   双重检查防止空指针。即使 vtable 不为 NULL，
     *   check_tool_call 函数指针也可能为空（该策略后端未提供检查逻辑）。
     */
    if (runtime->policy.vtable && runtime->policy.vtable->check_tool_call) {
        cc_policy_decision_t decision;
        rc = runtime->policy.vtable->check_tool_call(
            runtime->policy.self,
            call,
            &ctx,
            &decision
        );

        if (rc.code == CC_OK) {
            /*
             * Step 4: 策略拒绝则返回拒绝原因
             *
             * 将策略引擎返回的拒绝原因（decision.reason）填入
             * out_result->error，让 LLM 能看到为什么调用被拒绝。
             *
             * 使用 strdup 深拷贝原因字符串，因为 decision 即将被 free。
             *
             * 为什么需要一个默认错误消息：
             *   策略引擎可能返回 allowed=0 但 reason 为 NULL。
             *   此时使用预设的 "Tool call denied by policy" 作为 fallback。
             */
            if (!decision.allowed) {
                set_policy_error_result(out_result, decision.reason);
                cc_policy_decision_free(&decision);
                return cc_result_ok();
            }

            if (decision.require_approval) {
                const char *reason = decision.reason ?
                    decision.reason : "Tool call requires user approval";
                if (!ctx.services || !ctx.services->approve_tool_call) {
                    set_policy_error_result(
                        out_result,
                        "Tool call requires user approval, but no approval handler is registered"
                    );
                    cc_policy_decision_free(&decision);
                    return cc_result_ok();
                }

                if (!ctx.services->approve_tool_call(
                        call->name,
                        call->arguments_json,
                        reason,
                        ctx.services->approval_user_data)) {
                    set_policy_error_result(out_result, "Tool call denied by user");
                    cc_policy_decision_free(&decision);
                    return cc_result_ok();
                }
            }
        }
        cc_policy_decision_free(&decision);
    }

    /*
     * Step 5: 发布 "tool.call.started" 工具调用开始事件
     *
     * 事件的 JSON payload 包含：
     *   - session_id: 关联的会话
     *   - tool: 工具名称
     *   - args: 工具参数 JSON
     *
     * WHY 发布此事件：
     *   - UI 可以实时显示 "正在调用工具 get_weather..."
     *   - 监控系统可以追踪工具调用的频率和耗时
     *   - 调试时可以查看完整的调用链路
     *
     * WHY 包含 args 字段：
     *   参数信息对于调试和安全审计很关键。
     *   注意：如果参数包含敏感信息（如密码），需要在发布前脱敏。
     *   当前版本不做脱敏处理，留待未来增强。
     *
     * WHY 在策略检查通过后才发布：
     *   如果策略拒绝了调用，实际上没有执行"开始"，
     *   发布 "started" 事件会产生误导。
     */
    if (runtime->event_bus) {
        cc_json_value_t *payload = cc_json_create_object();
        if (payload) {
            cc_json_object_set(payload, "session_id", cc_json_create_string(session_id ? session_id : ""));
            cc_json_object_set(payload, "tool", cc_json_create_string(call->name ? call->name : ""));
            cc_json_object_set(payload, "args", cc_json_create_string(call->arguments_json ? call->arguments_json : ""));
        }
        publish_json_event(runtime->event_bus, "tool.call.started", payload);
    }

    /*
     * Step 6: 通过虚函数表调用工具的具体实现
     *
     * 这是多态调用的核心：tool.vtable->call 在不同工具中有不同的实现。
     * 例如 file_read 工具的 call 函数读取文件内容并返回 JSON，
     * run_command 工具的 call 函数执行命令并返回 stdout/stderr。
     *
     * 参数传递：
     *   - tool.self: 工具实例的数据指针（如文件系统句柄）
     *   - call->arguments_json: LLM 传递的参数字符串（JSON 格式）
     *   - &ctx: 上下文信息（session_id、workspace_dir 等）
     *   - out_result: 输出参数，工具将结果写入此结构体
     *
     * 注意：此调用是同步的。如果某个工具需要很长时间执行
     * （如大文件下载），会阻塞整个 Agent 循环。未来可考虑
     * 支持异步工具调用（通过回调或 Promise 模式）。
     */
    rc = tool.vtable->call(
        tool.self,
        call->arguments_json,
        &ctx,
        out_result
    );

    /*
     * 工具执行失败处理
     *
     * 工具通过虚函数表返回的 rc.code != CC_OK 表示执行异常
     * （如网络超时、文件不存在、权限不足等）。
     *
     * 处理方式：
     *   - 清除 out_result 的旧内容（memset）
     *   - 设置 ok=0 标记
     *   - 将 rc.message（工具提供的错误信息）填入 error
     *   - 如果有 rc 的内部资源，释放之（cc_result_free）
     *   - 返回 CC_OK（不传播错误）
     *
     * WHY 覆盖 out_result 而非保留：
     *   确保 out_result 的格式一致——调用方只需检查 ok 字段，
     *   不需要同时检查 rc 和 out_result。
     *
     * WHY 用 rc.message 作为 error：
     *   工具开发者通过 rc.message 提供了人类可读的错误描述。
     *   这个信息对 LLM 理解错误并做出调整很有价值。
     */
    if (rc.code != CC_OK) {
        memset(out_result, 0, sizeof(cc_tool_result_t));
        out_result->ok = 0;
        out_result->error = strdup(rc.message ? rc.message : "Tool execution failed");
        cc_result_free(&rc);
        return cc_result_ok();
    }

    /*
     * Step 7: 发布 "tool.call.finished" 工具调用结束事件
     *
     * 事件的 JSON payload 包含：
     *   - session_id: 关联的会话
     *   - tool: 工具名称
     *   - ok: 执行成功标记（1=成功，0=失败）
     *
     * WHY 包含 ok 字段：
     *   事件监听方需要区分工具执行成功还是失败。
     *   失败的调用可能需要 UI 展示警告或触发后续处理。
     *
     * WHY 不包含完整的返回结果（content/error）：
     *   工具返回的结果可能很大（如文件内容），塞入事件会导致
     *   事件消息膨胀。监听方如需详细结果，可以通过 storage 查询。
     *
     * WHY 在 "started" 和 "finished" 之间发布：
     *   形成配对事件，监听方可以通过 start/finish 事件计算工具执行耗时。
     */
    if (runtime->event_bus) {
        cc_json_value_t *payload = cc_json_create_object();
        if (payload) {
            cc_json_object_set(payload, "session_id", cc_json_create_string(session_id ? session_id : ""));
            cc_json_object_set(payload, "tool", cc_json_create_string(call->name ? call->name : ""));
            cc_json_object_set(payload, "ok", cc_json_create_bool(out_result->ok));
        }
        publish_json_event(runtime->event_bus, "tool.call.finished", payload);
    }

    return cc_result_ok();
}
