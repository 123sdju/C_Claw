/**
 * 学习导读：cclaw/core/src/core/cc_tool_call.c
 *
 * 所属层次：核心层。
 * 阅读重点：这里定义 Agent 运行时的数据模型、主循环和通用工具，阅读时重点看所有权、错误返回和 ReAct 数据流。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * cc_tool_call.c — 工具调用/结果/LLM响应结构体实现模块
 *
 * 模块在整体架构中的角色：
 *   本模块是 c-claw 框架 Core 层中处理工具调用（Function Calling）的
 *   核心数据结构集合。它定义了三个紧密关联的结构体，构成了从 LLM 发出
 *   工具调用请求到工具执行完成并回传结果的完整数据流：
 *     1. cc_tool_call_t    - LLM 请求调用某个工具的信息（函数名 + JSON 参数）
 *     2. cc_tool_result_t  - 工具执行后的返回结果（成功/失败 + 输出内容）
 *     3. cc_llm_response_t - LLM 的完整流式响应（可能是文本或工具调用）
 *
 *   这是 AI Agent 的"双手"——通过工具调用，Agent 可以超越纯文本对话，
 *   实际执行文件操作、网络请求、数据库查询等副作用动作。
 *
 * 依赖的其他模块：
 *   - cc_result.h — 统一错误返回类型
 *   - 标准库 (stdlib.h, string.h)
 *
 * 被哪些模块使用：
 *   - LLM Adapter 层 — 将 HTTP API 响应解析为 cc_llm_response_t
 *   - Tool Executor 层 — 接收 cc_tool_call_t 执行实际工具，返回 cc_tool_result_t
 *   - Platform/Session 层 — 将 cc_tool_result_t 包装为 CC_ROLE_TOOL 消息
 *   - Storage 层 — 序列化工具调用历史用于持久化和审计
 *
 * 三者关系（完整的工具调用生命周期）：
 *
 *   第1步：LLM 决定调用工具
 *     LLM 推理过程中判断需要外部能力，返回 cc_llm_response_t：
 *     { has_tool_call = true, tool_call = {name: "file_read", arguments_json: ...} }
 *     → 框架从 response 中提取 cc_tool_call_t
 *
 *   第2步：框架分发工具调用
 *     根据 tool_call.name 在 Tool Registry 中查找对应的工具实现
 *     → cc_tool_registry_find(registry, tool_call.name, &tool)
 *     → tool executor 构造 cc_tool_context_t，再调用 tool.vtable->call(...)
 *
 *   第3步：工具执行并生成结果
 *     工具执行完毕，生成 cc_tool_result_t：
 *     { ok = 1, content = "文件内容...", error = NULL }
 *     → 框架将其包装为 CC_ROLE_TOOL 消息
 *     → cc_message_create(..., CC_ROLE_TOOL, result.content, tool_call.id, &msg)
 *
 *   第4步：工具结果反馈给 LLM
 *     工具结果消息作为输入追加到对话历史，发送回 LLM
 *     → LLM 基于工具结果继续推理
 *     → 可能产出文本回复（循环结束），或新一轮工具调用（回到第1步）
 *
 *   注意：以上循环可能多次迭代——LLM 可能在一次用户请求中调用多个工具
 *   （串行或并行）。框架需要支持多轮 tool call → tool result → tool call
 *   的循环直到 LLM 产生最终的文本回复。
 *
 * 设计决策（为什么这样设计）：
 *   1. cc_tool_call_t 的参数以 JSON 字符串形式存储（arguments_json），而非
 *      解析为 C 结构体。
 *      为什么：不同工具的参数量纲完全不同（一个工具可能接受 2 个参数，
 *      另一个可能接受 20 个，参数类型也各不相同）。如果尝试用 C union
 *      或 void* 表示，会失去类型安全且无法序列化。JSON 字符串是 LLM API
 *      的原生格式（OpenAI/Anthropic/Gemini 都使用 JSON 参数），保持此格式
 *      避免了"JSON→C结构体→JSON"的不必要来回转换。
 *      工具执行者按需解析 arguments_json（通常使用 cJSON 或 jansson），
 *      框架本身不关心参数的具体结构。
 *   2. cc_tool_result_t 使用 ok (int) 而非 cc_result_t 作为成功标志。
 *      为什么：tool result 是跨进程/跨语言传输的简化表示，它不需要完整的
 *      错误链（error code + message + context）。ok 为简单的布尔语义（0=失败，
 *      非0=成功），在序列化/反序列化时更简洁，也更容易映射到 JSON 的布尔类型。
 *      此外，工具执行的"成功"概念不同于框架的"成功"概念——工具可能"成功执行
 *      但返回了意料之外的结果"，此时 ok=1 但上层仍需判断内容是否满足需求。
 *   3. cc_llm_response_t 可以同时包含文本和工具调用（has_text 和 has_tool_call
 *      是两个独立的标志，而非互斥的枚举）。
 *      为什么：某些 LLM 在流式响应中可能先返回文本描述（如"让我来帮你搜索一下"），
 *      再返回工具调用。两个独立标志允许 adapter 层灵活处理这种混合响应——
 *      先展示文本，再执行工具。如果使用互斥枚举（TEXT_ONLY | TOOL_CALL_ONLY），
 *      则无法表达这种过渡状态。
 *   4. cc_llm_response_init 接受已分配的结构体指针（栈分配友好）。
 *      为什么：在流式处理循环中，每轮迭代都创建/销毁堆对象会产生大量内存碎片。
 *      接受外部指针允许调用者在栈上分配 cc_llm_response_t：
 *        cc_llm_response_t resp;
 *        while (streaming) { init→parse→consume→free→... }
 *      栈分配 + memset 的开销可以忽略不计，对高频率流式事件（每秒数十次）
 *      的性能至关重要。这是低延迟 AI Agent 的关键性能优化。
 *   5. reasoning_content 字段单独存储（不在 text 中附加）。
 *      为什么：某些 LLM（如 DeepSeek-R1、OpenAI o1 系列）会在最终回答之前
 *      产出"思考过程"（chain-of-thought / reasoning）。这个中间内容
 *      不同于最终回复文本——它可能需要隐藏（不展示给用户）、压缩（只保留
 *      关键推理步骤）或单独记录（用于审计和调试）。独立字段使这些操作
 *      不需要解析 text 内容来区分"思考"与"回答"。
 *      在 API 层面，OpenAI 的 reasoning_tokens 和 DeepSeek 的
 *      reasoning_content 都是与 content 分开返回的独立字段。
 */

#include "cc/core/cc_tool_call.h"
#include <stdlib.h>
#include <string.h>

/*
 * cc_tool_call_create - 创建一个工具调用对象
 *
 * 功能：
 *   分配并初始化一个 cc_tool_call_t 实例，用于表示 LLM 发起的单次
 *   工具调用请求。所有字符串字段通过 strdup 深拷贝，确保对象
 *   独立拥有数据。
 *
 *   这是工具调用生命周期的起点——当 LLM Adapter 从 API 响应中解析到
 *   function call 时，立即调用此函数将 API 数据转换为框架内部的标准化表示。
 *
 * 典型使用场景：
 *   LLM Adapter 从 API 响应中解析到 function call 时：
 *   // JSON 响应中有: {"function": {"name": "file_read", "arguments": "{\"path\":\"/tmp/test.txt\"}"}}
 *   cc_tool_call_create(call_id, "file_read", arguments_str, &call);
 *   // 然后将 call 传递给 Tool Executor 执行
 *
 * 参数:
 *   @param id             - 工具调用的唯一标识符（由 LLM API 返回，如
 *                           "call_abc123"），可以为 NULL。
 *                           为什么 id 很重要：一次 LLM 响应可能包含多个
 *                           并行工具调用，每个调用有独立的 id。工具结果
 *                           通过此 id 关联回对应的调用。
 *   @param name           - 要调用的工具名称（如 "file_read"、"http.request"），
 *                           可以为 NULL。此名称用于在 Tool Registry 中查找
 *                           对应的工具实现（cc_tool_registry_find）。
 *   @param arguments_json - 工具参数的 JSON 字符串（如 "{\"path\":\"/tmp/test.txt\"}"），
 *                           可以为 NULL。保持 JSON 字符串格式而非解析为 C 结构体，
 *                           因为不同工具的参数 schema 完全不同，框架不应假设
 *                           任何参数结构。
 *   @param out_call       - [out] 输出参数，接收新创建的工具调用指针。
 *                           如果函数返回非 CC_OK，此参数的值未定义。
 *
 * @return CC_OK 表示成功。
 * @return CC_ERR_OUT_OF_MEMORY 表示内存分配失败（系统内存不足）。
 *
 * 生命周期：
 *   调用者负责在使用完毕后调用 cc_tool_call_destroy(*out_call) 释放。
 *   一个 cc_tool_call_t 对应 LLM 的一次 function call 指令，
 *   生命周期通常跨越：adapter 解析 → tool executor 执行 → 结果生成。
 *   工具调用对象在结果生成后就可以销毁——执行结果已保存在
 *   cc_tool_result_t 中，不再需要原始的 tool_call 对象。
 */
cc_result_t cc_tool_call_create(
    const char *id,
    const char *name,
    const char *arguments_json,
    cc_tool_call_t **out_call
)
{
    cc_tool_call_t *call = calloc(1, sizeof(cc_tool_call_t));
    if (!call) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate tool call");

    call->id = id ? strdup(id) : NULL;
    call->name = name ? strdup(name) : NULL;
    call->arguments_json = arguments_json ? strdup(arguments_json) : NULL;

    *out_call = call;
    return cc_result_ok();
}

/*
 * cc_tool_call_destroy - 销毁工具调用对象并释放关联资源
 *
 * 功能：
 *   释放 cc_tool_call_t 及其持有的 id、name、arguments_json 三个字符串字段，
 *   最后释放结构体本身。与 cc_message_destroy 遵循相同的"安全释放"模式。
 *
 * 参数:
 *   @param call - 要销毁的 cc_tool_call_t 指针，可以为 NULL（安全无操作）。
 *                 调用后 call 指针变为悬空，调用者不应再使用。
 *
 * 注意事项：
 *   - 不会释放调用者的其他资源（如关联的 tool_result 或 message 等）。
 *     每个结构体独立管理自己的资源，这是保持所有权清晰的基本原则。
 *   - 在工具执行完成后、结果已生成时即可调用此函数销毁 tool_call 对象。
 *     不需要等到整个对话结束。
 */
void cc_tool_call_destroy(cc_tool_call_t *call)
{
    if (!call) return;
    free(call->id);
    free(call->name);
    free(call->arguments_json);
    free(call);
}

/*
 * cc_tool_result_create - 创建工具执行结果对象
 *
 * 功能：
 *   分配并初始化一个 cc_tool_result_t 实例，用于记录一次工具调用的执行结果。
 *   同时持有成功标志（ok）、输出内容（content）和错误描述（error），
 *   允许调用者一次判断和三向消费。
 *
 *   这是工具执行完成后的"出口"——Tool Executor 执行完工具后，调用此函数
 *   将执行结果封装为统一的 cc_tool_result_t，然后由 tool executor/runtime 包装为
 *   CC_ROLE_TOOL 消息返回给 LLM。
 *
 * 典型使用场景：
 *   // 工具执行成功
 *   cc_tool_result_t *result;
 *   cc_tool_result_create(1, "File content here...", NULL, NULL, &result);
 *
 *   // 工具执行失败
 *   cc_tool_result_create(0, NULL, "Permission denied: cannot read /etc/shadow", NULL, &result);
 *
 *   // 工具部分成功（有输出但也有错误）
 *   cc_tool_result_create(0, "Partial output...", "Connection lost after 50%", NULL, &result);
 *
 * 参数:
 *   @param ok            - 工具执行是否成功。0 表示失败，非 0 表示成功。
 *                          使用 int 而非 bool（C89 无 bool 类型），且 int
 *                          在序列化（JSON 布尔字段）时更通用和可移植。
 *   @param content       - 工具执行的标准输出内容（文本），可以为 NULL。
 *                          例如 file_read 工具返回文件内容，
 *                          http.request 或插件工具返回搜索结果摘要。
 *                          为什么 content 是字符串而非二进制：
 *                          LLM 只能处理文本，二进制数据需要先编码（如 base64）
 *                          才能传递给 LLM。这个编码步骤由工具实现负责。
 *   @param error         - 失败时的错误描述字符串（如 "Permission denied"），
 *                          成功时为 NULL。可以与 content 同时有值——
 *                          某些工具即使失败也可能产生部分输出（如编译命令
 *                          返回错误但仍有 stderr 输出），同时提供 content
 *                          和 error 可以让 LLM 获得完整的诊断信息。
 *   @param metadata_json - 附带的元数据 JSON（如执行耗时、内存用量、缓存命中等），
 *                          可以为 NULL。用于调试和性能监控，不影响业务逻辑。
 *                          为什么用 JSON 字符串而非结构体：元数据类型因工具而异，
 *                          固定的 C 结构体无法覆盖所有工具的元数据需求。
 *   @param out_result    - [out] 输出参数，接收新创建的结果指针。
 *
 * @return CC_OK 表示成功。
 * @return CC_ERR_OUT_OF_MEMORY 表示内存分配失败。
 *
 * 设计决策:
 *   ok 和 error 分离存储（而非轮换使用 content/error 字段）：
 *   如果 content 在成功时存输出、失败时存错误信息（语义重载），
 *   调用者需要先判断 ok 才知道 content 的含义，容易混淆。
 *   三个独立字段（ok/content/error）语义清晰，每个字段承担单一职责。
 *   LLM 收到 content 和 error 都可以作为推理输入——失败的原因（error）
 *   对于 LLM 调整后续的工具调用策略同样有价值。
 */
cc_result_t cc_tool_result_create(
    int ok,
    const char *content,
    const char *error,
    const char *metadata_json,
    cc_tool_result_t **out_result
)
{
    cc_tool_result_t *result = calloc(1, sizeof(cc_tool_result_t));
    if (!result) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate tool result");

    result->ok = ok;
    result->content = content ? strdup(content) : NULL;
    result->error = error ? strdup(error) : NULL;
    result->metadata_json = metadata_json ? strdup(metadata_json) : NULL;

    *out_result = result;
    return cc_result_ok();
}

/*
 * cc_tool_result_destroy - 销毁工具结果对象并释放关联资源
 *
 * 功能：
 *   释放 cc_tool_result_t 及其持有的 content、error、metadata_json
 *   三个字符串字段，最后释放结构体本身。
 *
 * 参数:
 *   @param result - 要销毁的 cc_tool_result_t 指针，可以为 NULL（安全无操作）。
 *                   调用后指针变为悬空，不应再使用。
 *
 * 生命周期注意：
 *   工具结果被包装为 CC_ROLE_TOOL 消息后（content 被拷贝到 message.content），
 *   tool_result 对象的使命就完成了。此时可以安全销毁 tool_result 对象，
 *   message.content 持有独立的 strdup 副本，不受影响。
 *   如果调用者需要保留 tool_result 供后续分析（如性能统计），可以推迟销毁。
 */
void cc_tool_result_destroy(cc_tool_result_t *result)
{
    if (!result) return;
    free(result->content);
    free(result->error);
    free(result->metadata_json);
    free(result);
}

/*
 * cc_llm_response_init - 初始化 LLM 响应结构体
 *
 * 功能：
 *   将 cc_llm_response_t 的所有字段重置为零，为新一轮流式响应解析做准备。
 *   使用 memset 整体清零——等同于 has_text = 0, has_tool_call = 0,
 *   text = NULL, tool_call = {0}, reasoning_content = NULL。
 *
 *   这是流式响应解析循环的"重置"步骤——每收到一个新的 SSE chunk 或
 *   完整响应片段时，先 init 清空旧数据，再填充新数据。
 *
 * 参数:
 *   @param response - 指向已分配（栈或堆）的 cc_llm_response_t 的指针，
 *                     不可为 NULL。调用者负责提供内存，可以在栈上或堆上分配。
 *
 * @return 始终返回 CC_OK（本函数不会失败，因为只有栈操作 memset，无堆分配）。
 *
 * 典型使用模式（流式循环——最关键的性能优化场景）：
 *   cc_llm_response_t resp;  // 栈上分配，零开销
 *   while (streaming) {
 *       cc_llm_response_init(&resp);     // 重置状态（O(1) memset）
 *       adapter_parse_chunk(&resp, ...); // 解析新的响应块并填充字段
 *       if (resp.has_text) {
 *           process_text(resp.text);     // 消费文本片段
 *       }
 *       if (resp.has_tool_call) {
 *           execute_tool(&resp.tool_call); // 执行工具调用
 *       }
 *       if (resp.reasoning_content) {
 *           log_reasoning(resp.reasoning_content); // 记录思考过程
 *       }
 *       cc_llm_response_free(&resp);     // 释放本轮堆分配
 *   }
 *   // 循环结束后 resp 在栈上，编译器自动回收，无需额外清理
 *
 * 为什么接受外部指针（而非内部分配）：
 *   在流式处理循环中，每轮迭代都 malloc/free 会产生两个问题：
 *     1. 内存碎片：频繁的 malloc/free 导致堆碎片化，长期运行后性能下降。
 *     2. 分配延迟：malloc 的延迟不可预测（可能需要系统调用申请新页），
 *        在低延迟流式场景中这点延迟会被放大（每秒数十次 * 每次数十微秒）。
 *   栈分配 + memset 的开销可以忽略不计（~几十纳秒），且无碎片问题。
 *
 * 为什么不是 memset 之后再逐个字段赋值：
 *   memset 一次清零所有字段已经是最简实现。逐个字段置 NULL/0/0 需要
 *   多条语句且容易遗漏新增字段。memset 的方式"天然支持未来新增字段"——
 *   新字段会自动被清零，不需要修改 init 函数。
 */
cc_result_t cc_llm_response_init(cc_llm_response_t *response)
{
    memset(response, 0, sizeof(cc_llm_response_t));
    return cc_result_ok();
}

/*
 * cc_llm_response_free - 释放 LLM 响应结构体的内部堆分配资源
 *
 * 功能：
 *   释放 cc_llm_response_t 中所有堆分配的字符串字段：
 *   text、tool_call.id、tool_call.name、tool_call.arguments_json、
 *   reasoning_content。释放后将各指针置为 NULL 以便后续复用。
 *   注意：不释放 cc_llm_response_t 结构体本身（通常由调用者在栈上分配）。
 *
 *   本函数释放的是 adapter 层在解析过程中通过 strdup 分配的内容——
 *   adapter 填充 response 字段时创建堆副本，free 函数负责清理这些副本。
 *
 * 参数:
 *   @param response - 要释放内部资源的 cc_llm_response_t 指针，
 *                     可以为 NULL（安全无操作）。
 *
 * 释放哪些字段及其来源：
 *   - response->text                   → adapter 从 API 响应中提取的文本内容
 *   - response->tool_call.id           → LLM API 返回的工具调用唯一 ID
 *   - response->tool_call.name         → LLM API 返回的工具名称
 *   - response->tool_call.arguments_json → LLM API 返回的工具参数 JSON
 *   - response->reasoning_content      → LLM 的链式推理（思考过程）内容
 *
 * 与 init 的配对使用（init-free-init 循环模式）：
 *   典型模式为 init → 解析 → 消费 → free → init → ...
 *   free 后可以安全地再次调用 init 进行复用，因为所有指针已置为 NULL，
 *   而 init 中的 memset(0) 再次清零后 free(NULL) 是无操作的。
 *   这种"使用后清理，清理后可复用"的模式保证了循环的正确性和资源安全。
 *
 * 为什么每个指针独立 free + 置 NULL（而非循环遍历）：
 *   总共只有 6 个指针字段，逐个处理比构建指针数组再循环更清晰直接。
 *   每个 free 行对应一个明确的字段，维护者一眼就能看出哪些字段被释放。
 *   如果用循环遍历（把所有指针放入数组再遍历），编译器优化会更复杂，
 *   且代码可读性下降（需要额外的数组定义）。
 *
 * 为什么 reasoning_content 单独存储并单独释放：
 *   某些 LLM（如 DeepSeek-R1）会在最终回答之前产出"思考过程"
 *   （chain-of-thought / reasoning）。这个中间内容不同于最终回复文本：
 *     - 它可能需要隐藏（不展示给用户）或压缩（只保留关键推理步骤）
 *     - 它可能需要单独记录（用于审计、调试或知识蒸馏）
 *     - 它在 API 层面是独立字段（如 OpenAI 的 reasoning_tokens 等价物、
 *       DeepSeek 的 reasoning_content）
 *   将 reasoning_content 与 text 分开存储使得上层可以分别处理这两个
 *   语义不同的内容——text 展示给用户，reasoning 记录到日志但不展示。
 *   分离存储也避免了适配层需要解析 text 来区分"思考"和"回答"内容。
 */
void cc_llm_response_free(cc_llm_response_t *response)
{
    if (!response) return;
    free(response->text);
    response->text = NULL;
    free(response->tool_call.id);
    response->tool_call.id = NULL;
    free(response->tool_call.name);
    response->tool_call.name = NULL;
    free(response->tool_call.arguments_json);
    response->tool_call.arguments_json = NULL;
    free(response->reasoning_content);
    response->reasoning_content = NULL;
}
