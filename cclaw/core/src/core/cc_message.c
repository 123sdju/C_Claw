/**
 * 学习导读：cclaw/core/src/core/cc_message.c
 *
 * 所属层次：核心层。
 * 阅读重点：这里定义 Agent 运行时的数据模型、主循环和通用工具，阅读时重点看所有权、错误返回和 ReAct 数据流。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * cc_message.c — 消息体实现模块
 *
 * 模块在整体架构中的角色：
 *   本模块是 c-claw 框架 Core 层中表示 LLM 对话消息的核心数据结构。
 *   cc_message_t 代表了会话中的一条消息，包含发送者角色、文本内容、以及
 *   工具调用相关的元数据。它是构建对话历史和与 LLM 交互的基本单元。
 *   在分层架构中处于最底层的数据模型层——被 session 聚合，被 adapter 消费，
 *   被 storage 持久化。
 *
 * 依赖的其他模块：
 *   - cc_result.h — 统一错误返回类型，create 函数返回 cc_result_t
 *   - 标准库 (stdlib.h, string.h) — calloc/free 用于内存管理
 *   - cc_memory.h — 可移植字符串复制
 *
 * 被哪些模块使用：
 *   - cc_session (核心层) — session 持有消息列表，管理对话上下文
 *   - LLM Adapter 层 (platform adapter) — 读取消息构造 API 请求 JSON
 *   - Storage 层 (storage adapter) — 将消息序列化/反序列化到持久化后端
 *   - Tool executor 层 — 将工具执行结果包装为 CC_ROLE_TOOL 消息回传 LLM
 *
 * 消息角色（cc_message_role_t）说明：
 *   四种角色对应于 LLM API 协议中的标准角色划分，每一类消息在对话中
 *   承担不同的语义角色：
 *
 *   CC_ROLE_SYSTEM    - 系统提示词（System Prompt）
 *     - 用于设定 AI 的行为边界、角色定位和对话规则
 *     - 通常是对话的第一条消息（大多数 LLM API 要求 system 消息位于开头）
 *     - 内容示例："你是一个专业的代码助手，擅长 C 语言和系统编程"
 *     - 为什么放在第一条：LLM 在处理对话时，system 消息构成了基准上下文，
 *       后续消息的语义解析都会以此为基础。中途插入 system 消息可能被 API 拒绝
 *       或被模型忽略。
 *     - 对应 LLM API role 字段值："system"
 *
 *   CC_ROLE_USER      - 用户输入的消息
 *     - 代表终端用户发送的对话内容
 *     - 内容示例："请帮我写一个快速排序函数"
 *     - 为什么需要区分 user 和 assistant：LLM 需要知道"谁说了什么"才能
 *       正确理解对话的上下文和意图。user 消息通常触发推理，assistant
 *       消息提供参考。
 *     - 对应 LLM API role 字段值："user"
 *
 *   CC_ROLE_ASSISTANT - AI 助手的回复
 *     - 代表 LLM 模型生成的回复内容
 *     - 可以包含纯文本（content 字段），也可以同时包含工具调用
 *       （通过关联的 cc_tool_call_t，但消息结构体本身不直接持有 tool_call 引用）
 *     - 内容示例："快速排序的实现如下：\n```c\nvoid quicksort..."
 *     - 为什么 content 可以为 NULL：某些 messages 仅包含 tool_calls 而不含文本
 *       （如 LLM 返回纯 function call 时）
 *     - 对应 LLM API role 字段值："assistant"
 *
 *   CC_ROLE_TOOL      - 工具执行后的结果反馈
 *     - 代表工具执行完成后返回给 LLM 的结果信息
 *     - 通过 tool_call_id 关联到具体的 cc_tool_call_t，
 *       LLM 以此判断哪个工具调用已完成，并将结果纳入后续推理
 *     - 内容示例："File read successfully: [file contents...]"
 *     - 为什么需要 tool_call_id：一次对话中 LLM 可能并行发起多个工具调用，
 *       tool_call_id 确保了"哪个调用对应哪个结果"的正确配对
 *     - 对应 LLM API role 字段值："tool"
 *
 * 消息生命周期（完整流程）：
 *   1. 创建：上层调用 cc_message_create(...) 分配消息体，填充 id/role/content
 *      等字段。此时 created_at 为 NULL（留待上层设置）。
 *   2. 归属：消息被添加到 session 的消息列表中，session 持有其指针。
 *      消息自身通过 session_id 记住所属会话（反向引用）。
 *   3. 序列化：LLM Adapter 遍历消息列表，通过 cc_message_role_string()
 *      获取 role 字符串，将消息转换为 API 请求的 JSON 格式。
 *   4. 持久化：Storage 层将消息及其字段写入文件/数据库。
 *   5. 销毁：上层调用 cc_message_destroy(...) 释放消息的所有堆分配资源。
 *      注意：销毁前需要先从 session 的消息列表中移除引用。
 *
 * 设计决策（为什么这样设计）：
 *   1. 所有字符串字段（id, session_id, content, tool_call_id, created_at）
 *      均通过 strdup 独立拷贝，确保消息对象的生命周期完全自管理。
 *      为什么：外部传入的字符串可能是栈上临时变量或会被释放的缓冲区，
 *      直接保存指针会导致 use-after-free 问题。这是 C 语言内存管理的
 *      基本原则——谁分配谁负责释放，但框架不信任调用者的生命周期管理。
 *      strdup 将所有权转移给消息对象，调用者可以随意释放自己的缓冲区。
 *   2. create 函数使用 calloc 确保未设置的可选字段安全为 NULL。
 *      为什么：C 语言中未初始化的指针为"野指针"，free 时会导致崩溃。
 *      calloc 的零初始化使得 destroy 函数可以安全地 free(NULL)。
 *      这与 malloc + memset 等价，但 calloc 可能利用 OS 的零页机制
 *      （CoW zero-page），在某些平台上比 malloc+memset 更高效。
 *   3. 消息结构体不持有对其他消息的引用（单向独立）。
 *      为什么：LLM 的上下文窗口大小有限，消息往往是按顺序处理的流水线，
 *      双向引用会增加内存开销且不带来实际收益。如果需要消息间关联，
 *      应通过 session 级别的列表索引实现。
 *   4. created_at 字段在 create 时不填充。
 *      为什么：创建时间应由上层 runtime 或存储后端根据实际发送/接收时机设置，
 *      而非结构体分配时间。消息可能在创建后数秒才实际发送到 LLM API——
 *      如果以 calloc 时间作为创建时间，会产生不准确的时序记录。
 *   5. cc_message_role_string 返回静态字符串指针。
 *      为什么：四种角色的 API 名称是固定标准（"system"/"user"/"assistant"/
 *      "tool"），永远不会改变。返回常量指针避免了每次序列化时的 strdup
 *      和 free 开销。在消息频繁序列化的场景下（每条消息都要取 role 字符串），
 *      这个优化节省了大量不必要的堆操作。
 */

#include "cc/core/cc_message.h"
#include "cc/util/cc_memory.h"
#include <stdlib.h>
#include <string.h>

/*
 * cc_message_create - 创建一条新的消息对象
 *
 * 功能：
 *   分配并初始化一个 cc_message_t 实例，所有传入的字符串参数均通过
 *   strdup 深拷贝，确保消息对象独立拥有所有数据。调用者传入的参数
 *   可以为 NULL，对应的字段也会被设置为 NULL。
 *
 *   这是消息的"构造函数"——所有消息的生命周期都从此函数开始。
 *   函数在堆上分配 cc_message_t（通过 calloc），并复制所有字符串参数。
 *
 * 典型使用场景：
 *   1. 用户输入消息：role=CC_ROLE_USER，content=用户输入文本
 *      示例：cc_message_create(uuid, sess_id, CC_ROLE_USER, "你好", NULL, &msg);
 *   2. AI 回复消息：role=CC_ROLE_ASSISTANT，content=LLM 生成的文本
 *      示例：cc_message_create(uuid, sess_id, CC_ROLE_ASSISTANT, llm_text, NULL, &msg);
 *   3. 工具结果消息：role=CC_ROLE_TOOL，content=工具输出，
 *      tool_call_id=对应 cc_tool_call_t 的 id
 *      示例：cc_message_create(uuid, sess_id, CC_ROLE_TOOL, result_text, call_id, &msg);
 *   4. 系统提示词：role=CC_ROLE_SYSTEM，content=系统提示词文本
 *      示例：cc_message_create(uuid, sess_id, CC_ROLE_SYSTEM, system_prompt, NULL, &msg);
 *      注意：system 消息通常没有 tool_call_id（它不关联工具调用）。
 *
 * 参数:
 *   @param id          - 消息的唯一标识符，通常由上层生成（唯一字符串），可以为 NULL。
 *                        为什么需要 id：支持通过 ID 查找/更新/删除特定消息，
 *                        以及在日志中精确追踪单条消息的流转。
 *   @param session_id  - 所属会话的唯一标识符，关联到 cc_session_t，可以为 NULL。
 *                        为什么需要反向引用：当消息独立于 session 被处理时
 *                        （如序列化到文件中），session_id 提供了溯源能力。
 *   @param role        - 消息发送者的角色（CC_ROLE_SYSTEM / CC_ROLE_USER /
 *                        CC_ROLE_ASSISTANT / CC_ROLE_TOOL）。
 *                        为什么 role 不是字符串而是枚举：类型安全——
 *                        编译器会检查角色值是否合法，不会出现"usre"这类拼写错误。
 *                        需要输出字符串时通过 cc_message_role_string() 转换。
 *   @param content     - 消息的文本内容，可以为 NULL（如仅发送 tool_call 时）。
 *                        为什么 content 可以为 NULL：LLM 的某些响应只包含
 *                        function call 而不含文本描述，此时 content 无内容。
 *   @param tool_call_id- 关联的工具调用 ID，仅当 role == CC_ROLE_TOOL 时有意义，
 *                        用于 LLM 将工具结果与请求配对，可以为 NULL。
 *                        为什么 tool_call_id 存在于消息而非独立的关联表中：
 *                        每个 tool 消息只关联一个 tool_call，一对一的简单关系
 *                        不需要额外的关联表。将 id 嵌入消息字段是最简洁的设计。
 *   @param out_message - [out] 输出参数，接收新创建的消息指针。
 *                        如果函数返回非 CC_OK，此参数的值未定义，不应使用。
 *                        为什么是双重指针（**）而非单指针返回值：
 *                        返回值 cc_result_t 已被错误信息占用，输出必须通过参数传递。
 *                        双重指针允许函数修改调用者栈上的指针变量。
 *
 * @return CC_OK 表示成功，*out_message 指向有效的新消息对象。
 * @return CC_ERR_OUT_OF_MEMORY 表示 calloc 分配失败，系统内存不足。
 *
 * 内存管理约定：
 *   调用者负责在使用完毕后调用 cc_message_destroy(*out_message) 释放。
 *   消息对象不依赖外部传入的指针，可以安全地在任何生命周期内使用。
 *   这是自管理（self-contained）数据结构的核心优势——所有权清楚，不会
 *   出现"谁该释放"的困惑。
 *
 * 为什么用 calloc 而非 malloc：
 *   calloc 保证所有字段初始化为零（指针为 NULL，整数为 0）。
 *   如果使用 malloc，tool_call_id 等未设置的字段将是随机值，
 *   后续 cc_message_destroy 中的 free(tool_call_id) 会触发未定义行为。
 *   对于结构体中包含指针的情况，calloc 是唯一安全的分配方式。
 *
 * strdup 的内部工作（为什么每个字段都需要独立拷贝）：
 *   strdup(str) 等价于 malloc(strlen(str)+1) + strcpy。
 *   每个字段获得独立的堆内存副本，消息对象的生命周期不再依赖调用者的栈内存。
 *   调用者可以在 cc_message_create 返回后立即释放或覆写自己的字符串缓冲区，
 *   消息对象仍能安全访问其内容。这种"所有权转移"模式是 C 语言中
 *   避免悬空引用的标准实践。
 */
cc_result_t cc_message_create(
    const char *id,
    const char *session_id,
    cc_message_role_t role,
    const char *content,
    const char *tool_call_id,
    cc_message_t **out_message
)
{
    cc_message_t *msg = calloc(1, sizeof(cc_message_t));
    if (!msg) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate message");

    /* 所有字符串字段通过 strdup 深拷贝。
       如果传入 NULL，对应字段也保持 NULL——这是安全的，因为 calloc 已将其置零。
       destroy 函数中的 free(NULL) 是安全的无操作。 */
    msg->id = id ? strdup(id) : NULL;
    msg->session_id = session_id ? strdup(session_id) : NULL;
    msg->role = role;
    msg->content = content ? strdup(content) : NULL;
    msg->tool_call_id = tool_call_id ? strdup(tool_call_id) : NULL;

    *out_message = msg;
    return cc_result_ok();
}

/*
 * cc_message_destroy - 销毁消息对象并释放所有关联资源
 *
 * 功能：
 *   释放 cc_message_t 及其持有的所有堆分配字符串，将消息对象占用的
 *   所有内存归还给系统。本函数实现了安全的空指针检查，允许传入 NULL。
 *
 * 参数:
 *   @param message - 要销毁的 cc_message_t 指针，可以为 NULL（安全无操作）。
 *                    调用后 message 指针变为悬空指针，调用者不应再使用。
 *
 * 行为细节（释放顺序）:
 *   依次释放 id、session_id、content、tool_call_id、created_at
 *   五个字符串字段，最后释放 cc_message_t 结构体本身。
 *   释放顺序无关紧要——五个字段在不同的堆内存区域，互不依赖。
 *   先释放字段再释放结构体是合理的：字段是结构体的一部分，在结构体
 *   被 free 后字段指针本身就不可访问了。
 *
 * 为什么需要释放 created_at：
 *   created_at 字段虽然在 cc_message_create 中未设置（保持 calloc 的 NULL），
 *   但上层 runtime 或存储后端可能在创建后通过 strdup 填充此字段。
 *   如果上层确实填充了 created_at，destroy 必须释放它；如果没有填充
 *   （仍是 NULL），free(NULL) 是安全的无操作。这是一种"防未来的"释放策略——
 *   即便当前代码不设置 created_at，destroy 的释放逻辑也不需要修改。
 *
 * 为什么采用"安全释放"模式（允许 NULL 指针）：
 *   在复杂控制流中，消息指针可能在多条路径上被部分初始化或已释放。
 *   例如：创建失败时 out_message 未定义，上层可能在清理路径上
 *   无条件调用 destroy。允许传入 NULL 使得清理代码可以无脑调用 destroy
 *   而不需要额外的 if 判断，显著减少内存泄漏和 double-free 的风险。
 *   这是 C 语言中资源管理的最佳实践——destructor 总是接受 NULL。
 *
 * 为什么不销毁关联的 session 或其他对象：
 *   本函数遵循"谁创建谁销毁"原则——消息对象只负责自己分配的资源。
 *   session 由调用者管理，消息只是 session 的"内容"，不反向持有所有权。
 *   级联销毁（destroy message → destroy session → destroy messages）
 *   会造成无限循环。保持单向所有权是防止循环引用的标准方式。
 *
 * free 函数的顺序不影响正确性但遵循"从叶子到根"的习惯：
 *   先释放结构体的字符串字段（叶子数据），最后释放结构体本身（根节点）。
 *   虽然 free 不要求任何顺序（所有内存块独立），但保持一致的释放顺序
 *   有助于代码审查和维护。
 */
void cc_message_destroy(cc_message_t *message)
{
    if (!message) return;
    cc_message_cleanup(message);
    free(message);
}

/* 学习注释：cc_message_cleanup 是对外可见或跨模块调用的入口。
 * 阅读时重点确认参数校验、所有权转移、错误码和清理路径是否成对出现。 */
void cc_message_cleanup(cc_message_t *message)
{
    if (!message) return;
    free(message->id);
    free(message->session_id);
    free(message->content);
    free(message->tool_calls_json);
    free(message->reasoning_content);
    free(message->tool_call_id);
    free(message->created_at);
    memset(message, 0, sizeof(*message));
}

/* 学习注释：cc_message_copy 是对外可见或跨模块调用的入口。
 * 阅读时重点确认参数校验、所有权转移、错误码和清理路径是否成对出现。 */
cc_result_t cc_message_copy(const cc_message_t *src, cc_message_t *dst)
{
    if (!src || !dst) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null message copy argument");
    }
    memset(dst, 0, sizeof(*dst));
    dst->id = cc_strdup(src->id);
    dst->session_id = cc_strdup(src->session_id);
    dst->role = src->role;
    dst->content = cc_strdup(src->content);
    dst->tool_calls_json = cc_strdup(src->tool_calls_json);
    dst->reasoning_content = cc_strdup(src->reasoning_content);
    dst->tool_call_id = cc_strdup(src->tool_call_id);
    dst->created_at = cc_strdup(src->created_at);
    if ((src->id && !dst->id) ||
        (src->session_id && !dst->session_id) ||
        (src->content && !dst->content) ||
        (src->tool_calls_json && !dst->tool_calls_json) ||
        (src->reasoning_content && !dst->reasoning_content) ||
        (src->tool_call_id && !dst->tool_call_id) ||
        (src->created_at && !dst->created_at)) {
        cc_message_cleanup(dst);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy message");
    }
    return cc_result_ok();
}

/* 学习注释：cc_message_set_tool_calls_json 是对外可见或跨模块调用的入口。
 * 阅读时重点确认参数校验、所有权转移、错误码和清理路径是否成对出现。 */
cc_result_t cc_message_set_tool_calls_json(cc_message_t *message, const char *tool_calls_json)
{
    if (!message) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null message");
    char *copy = cc_strdup(tool_calls_json);
    if (tool_calls_json && !copy) {
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy tool calls");
    }
    free(message->tool_calls_json);
    message->tool_calls_json = copy;
    return cc_result_ok();
}

/* 学习注释：cc_message_set_reasoning_content 是对外可见或跨模块调用的入口。
 * 阅读时重点确认参数校验、所有权转移、错误码和清理路径是否成对出现。 */
cc_result_t cc_message_set_reasoning_content(cc_message_t *message, const char *reasoning_content)
{
    if (!message) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null message");
    char *copy = cc_strdup(reasoning_content);
    if (reasoning_content && !copy) {
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy reasoning content");
    }
    free(message->reasoning_content);
    message->reasoning_content = copy;
    return cc_result_ok();
}

/*
 * cc_message_role_string - 将消息角色枚举转换为 API 协议字符串
 *
 * 功能：
 *   将内部的 cc_message_role_t 枚举值映射为 LLM API 协议要求的
 *   标准小写英文字符串。这是消息序列化的第一步——LLM adapter 层
 *   通过此函数获取 role 字段的值，嵌入到 HTTP 请求的 JSON body 中。
 *
 *   四种角色映射：
 *     CC_ROLE_SYSTEM    → "system"
 *     CC_ROLE_USER      → "user"
 *     CC_ROLE_ASSISTANT → "assistant"
 *     CC_ROLE_TOOL      → "tool"
 *
 *   为什么这些是标准名称：
 *   OpenAI Chat Completions API 和 Anthropic Messages API 都使用
 *   这些字符串作为 role 字段的值。Gemini API 虽然内部用不同的术语
 *   （"model" 代替 "assistant"），但 c-claw 框架在 adapter 层做了
 *   转换——本函数输出的始终是 OpenAI 标准的角色名称，adapter 层
 *   负责将此标准名称转换为目标 API 的具体格式。
 *
 * 参数:
 *   @param role - cc_message_role_t 枚举值
 *
 * @return 指向静态字符串常量（只读）的指针，为角色的英文小写名称。
 *         返回值不需要调用者释放，生命周期等同于程序运行期。
 *         如果传入无法识别的枚举值，返回 "unknown" 作为安全默认值。
 *         为什么返回 "unknown" 而非 NULL：LLM API 收到 "unknown" role
 *         会返回明确的错误信息，而收到 null/missing role 可能导致
 *         各种不确定行为（取决于具体的 API 实现）。
 *
 * 用途详解：
 *   1. LLM Adapter 序列化消息到 API 请求 JSON：
 *      fprintf(json, "\"role\":\"%s\"", cc_message_role_string(msg->role));
 *   2. 日志输出和调试——在日志中看到 "role=assistant" 比看到 "role=2"
 *      更容易理解。
 *   3. Storage 层序列化——将消息的 role 存储为字符串而非数字枚举值，
 *      使得持久化数据更易读且不会因枚举值变化而失去兼容性。
 *
 * 为什么用静态字符串而非 strdup 返回值：
 *   1. 角色名称是固定的标准字符串，永远不会改变——没有理由创建动态副本。
 *   2. 此函数可能在高频路径上被调用（每条消息的每次序列化都要调用一次），
 *      返回常量指针的延迟为零（只是返回编译时确定的地址），strdup 有
 *      malloc + memcpy 的开销（可能数十到数百纳秒）。
 *   3. 如果返回 strdup 的值，调用者必须记得 free——在序列化循环中
 *      增加 free 调用会显著降低代码可读性。
 *
 * 为什么用 switch 而非查找表：
 *   与 cc_error_string 相同的设计考虑——四个条目 switch 和数组查找
 *   性能无差异，但 switch 中每个 case 明确对应"哪个枚举 → 什么字符串"，
 *   一目了然。default 分支返回 "unknown" 提供了安全的降级行为。
 */
const char *cc_message_role_string(cc_message_role_t role)
{
    switch (role) {
    case CC_ROLE_SYSTEM:    return "system";
    case CC_ROLE_USER:      return "user";
    case CC_ROLE_ASSISTANT: return "assistant";
    case CC_ROLE_TOOL:      return "tool";
    default:                return "unknown";
    }
}
