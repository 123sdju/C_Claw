/**
 * 学习导读：cclaw/core/src/app/cc_memory_context.c
 *
 * 所属层次：核心层。
 * 阅读重点：这里定义 Agent 运行时的数据模型、主循环和通用工具，阅读时重点看所有权、错误返回和 ReAct 数据流。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * ===========================================================================
 * cc_memory_context.c — 长期记忆上下文注入器
 * ===========================================================================
 *
 * 模块在整体架构中的角色：
 * ─────────────────────────────
 * 本模块是"长期记忆"（Long-Term Memory）子系统的上下文注入层。
 * 它负责从 memory store 中检索与当前会话相关的记忆条目，并将其格式化
 * 为可直接插入 LLM 系统提示词（system prompt）的记忆文本块。
 *
 * 与短期记忆（会话历史）的关键区别：
 * ────────────────────────────────
 *   短期记忆（Session Memory / Conversation History）：
 *     - 存储在 session store 中
 *     - 通过 cc_context_builder 从 storage 加载最近 100 条消息
 *     - 随会话的进行动态增长（每轮添加 user/assistant/tool 消息）
 *     - 生命周期：随会话创建而开始，会话结束即不再使用
 *
 *   长期记忆（Persistent Memory / Long-Term Memory）：
 *     - 存储在 memory store 中（独立于 session store）
 *     - 通过本模块（cc_memory_context）从 memory store 检索
 *     - 跨会话持久化，不会因为会话结束而丢失
 *     - 生命周期：永久保存直到被显式删除
 *     - 典型用途：记住用户偏好、过往重要事实、项目背景等
 *
 * 上游调用方：
 *   - cc_context_builder.c —— 在构建 messages 数组时，调用本模块获取
 *     记忆文本块，并将其作为一条额外的 system 消息注入到 messages 中
 *
 * 下游依赖模块：
 *   - cc_memory_store —— 长期记忆的持久化存储接口（虚接口）
 *   - cc_string_builder —— 用于高效拼接记忆条目的格式化文本
 *
 * 记忆注入的数据流：
 * ──────────────────
 *   cc_context_builder_build_messages()
 *     │
 *     ├─ 1. 构建 system_prompt 消息（role="system"）
 *     │
 *     ├─ 2. 调用 cc_memory_context_inject()  ← 本模块入口
 *     │     │
 *     │     ├─ 2a. 调用 cc_memory_store_search()
 *     │     │      在 memory store 中搜索与 system_prompt
 *     │     │      （或当前会话文本）语义相关的记忆条目
 *     │     │      最多返回 10 条最相关的记忆
 *     │     │
 *     │     ├─ 2b. 如果搜索结果为空（count == 0）
 *     │     │      → out_memory_block = NULL，直接返回 OK
 *     │     │      WHY：不注入空内容，避免浪费 LLM token
 *     │     │
 *     │     ├─ 2c. 将每条记忆格式化为 Markdown 列表项
 *     │     │      格式：- key: value (category: category_name)
 *     │     │      示例：- user_preferred_language: Chinese (category: preference)
 *     │     │
 *     │     └─ 2d. 在列表前添加引导标题
 *     │            "[Memory] The following are persistent facts from previous sessions:"
 *     │
 *     ├─ 3. 如果 mem_block 非空，将其包装为 role="system" 消息
 *     │     并加入 messages JSON 数组（紧接在 system_prompt 之后）
 *     │
 *     └─ 4. 释放 mem_block（已序列化到 JSON 中，不再需要）
 *
 * 记忆条目格式化为 system prompt 块的详细说明：
 * ──────────────────────────────────────────────
 *
 * 最终注入到 LLM 上下文中的记忆文本块格式如下：
 *
 * ┌─────────────────────────────────────────────────────────────┐
 * │ [Memory] The following are persistent facts from previous   │
 * │ sessions:                                                   │
 * │ - user_name: 张三 (category: identity)                      │
 * │ - preferred_language: Chinese (category: preference)        │
 * │ - project_root: /home/user/myapp (category: workspace)      │
 * │ - favorite_editor: vscode (category: preference)            │
 * └─────────────────────────────────────────────────────────────┘
 *
 * 设计决策——为什么用 Markdown 格式的行内列表：
 * ────────────────────────────────────────
 *   1. LLM 友好性：大多数 LLM 在训练数据中见过大量 Markdown 格式文本，
 *      对这种 "标题 + 列表项" 的结构有良好的理解能力。
 *
 *   2. Token 效率：相比于每条记忆都作为独立的 system 消息（会重复
 *      "role":"system" 等 JSON 开销），将多条记忆合并为一条消息能节省
 *      约 30-50% 的 token 开销。
 *
 *   3. 结构化与可读性平衡：使用 "- key: value" 格式而非 JSON 格式，
 *      让 LLM 在自然语言推理中更容易理解记忆条目的含义。
 *      JSON 格式虽然结构更严格，但在自然语言上下文中突兀且浪费 token。
 *
 *   4. category 字段的作用：
 *      category 为 LLM 提供了记忆的分类标签（如 "identity"、"preference"、
 *      "workspace"），帮助 LLM 在推理时判断何时引用哪条记忆。
 *      例如：当用户问 "我叫什么名字" 时，LLM 可以快速定位到
 *      category="identity" 的记忆条目。
 *
 * 设计决策——为什么搜索关键词是 system_prompt：
 * ──────────────────────────────────────────────
 *   cc_memory_context_inject 接收 session_text（即 system_prompt）作为
 *   搜索关键词。原因是：
 *     - system_prompt 通常包含了 Agent 的角色定义和当前任务上下文
 *     - 用 system_prompt 做语义搜索，能检索到与当前 Agent 角色相关的记忆
 *     - 如果用户刚进入会话还没发消息，此时没有 user 消息可用，
 *       system_prompt 是最佳的"当前上下文"代理
 *
 * 设计决策——为什么最多返回 10 条记忆：
 * ──────────────────────────────────────
 *   - 记忆条目过多会挤占 LLM 宝贵的上下文窗口
 *   - 10 条通常是够用的上限（大部分用户不会在一个 Agent 中积累超过
 *     10 条活跃的长期记忆）
 *   - 如果项目需要更多记忆，可以调整调用方的 limit 参数
 *
 * 错误处理策略：
 * ──────────────
 *   - 如果 memory store 接口未提供（store 或 vtable 为 NULL），
 *     返回 out_memory_block = NULL + CC_OK（优雅降级）
 *   - 如果搜索失败或返回空结果，同样返回 NULL + CC_OK
 *   - 记忆注入失败不应阻止 Agent 主循环的正常运行
 *   - 这是"软依赖"（soft dependency）模式：记忆是增强功能而非核心功能
 */

#include "cc/app/cc_memory_context.h"
#include "cc/util/cc_string_builder.h"
#include <stdlib.h>
#include <string.h>

/**
 * cc_memory_context_inject — 从 memory store 检索记忆并格式化为文本块
 *
 * 功能：
 *   根据给定的搜索文本（通常是 system_prompt），从长期记忆存储中检索
 *   最相关的记忆条目，将其格式化为一条可供 LLM 阅读的记忆上下文文本块。
 *   这个文本块会被 cc_context_builder 包装为 system 消息注入到
 *   LLM 的 messages 数组中。
 *
 * @param store           长期记忆存储接口（虚接口，可为 NULL）
 *                        如果 store 或 store->vtable 为 NULL，直接返回空结果
 * @param session_text    用于语义搜索的关键词文本（通常是 system_prompt）
 *                        memory store 根据此文本检索最相关的记忆条目
 * @param out_memory_block 输出参数，格式化后的记忆文本块
 *                         格式见上方"记忆注入的数据流"部分
 *                         调用方负责 free
 *                         如果无记忆或搜索失败，设为 NULL
 *
 * @return CC_OK —— 始终成功（即使无记忆也是正常状态）
 *         CC_ERR_INVALID_ARGUMENT —— out_memory_block 为 NULL
 *
 * 算法步骤（4 步）：
 * ──────────────────
 *   Step 1: 参数校验
 *     - 如果 store、store->vtable 或 out_memory_block 为 NULL，
 *       设置 out_memory_block = NULL 并返回错误或 OK
 *
 *   Step 2: 从 memory store 搜索相关记忆
 *     - 调用 cc_memory_store_search(store, session_text, 10, &entries, &count)
 *     - 最多返回 10 条最相关的记忆条目
 *     - 搜索结果存储在 entries 数组中，count 为实际条目数
 *     - 内存所有权：entries 由 memory store 分配，调用方负责释放
 *       （通过 cc_memory_entry_free_array 释放）
 *
 *   Step 3: 判断是否需要注入
 *     - 如果搜索失败（rc.code != CC_OK）或结果为空（count == 0）：
 *       释放 rc 内部资源（cc_result_free），设置 out_memory_block = NULL
 *       返回 CC_OK（这是正常路径，不是错误）
 *
 *   Step 4: 格式化记忆条目为文本块
 *     - 使用 cc_string_builder 构建格式化文本
 *     - 首先添加引导标题行：
 *       "[Memory] The following are persistent facts from previous sessions:"
 *     - 然后逐条遍历 entries 数组，格式化为：
 *       "- key: value (category: category_name)"
 *     - 最后通过 cc_string_builder_take() 转移缓冲区所有权到 out_memory_block
 *
 * 典型输出示例：
 *   [Memory] The following are persistent facts from previous sessions:
 *   - user_name: 张三 (category: identity)
 *   - project_language: C (category: workspace)
 *   - os: linux (category: environment)
 *
 * 调用方的处理方式（在 cc_context_builder.c 中）：
 *   char *mem_block = NULL;
 *   cc_memory_context_inject(runtime->memory_store, system_prompt, &mem_block);
 *   if (mem_block && strlen(mem_block) > 0) {
 *       // 将 mem_block 包装为 {"role":"system","content":mem_block}
 *       // 并追加入 messages JSON 数组
 *   }
 *   free(mem_block);
 */
cc_result_t cc_memory_context_inject(
    cc_memory_store_t *store,
    const char *session_text,
    char **out_memory_block
)
{
    /*
     * Step 1: 参数校验
     *
     * 如果 memory store 未配置或输出指针无效，直接返回。
     * 这是"优雅降级"策略——没有记忆功能不影响 Agent 正常工作。
     * out_memory_block 设为 NULL 让调用方知道没有记忆需要注入。
     */
    if (!store || !store->vtable || !out_memory_block) {
        if (out_memory_block) *out_memory_block = NULL;
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid memory context arguments");
    }

    cc_memory_entry_t *entries = NULL;
    size_t count = 0;

    /*
     * Step 2: 从 memory store 搜索相关记忆
     *
     * 最多返回 10 条最相关的记忆条目。
     * session_text（system_prompt）作为语义搜索的关键词。
     * 如果 session_text 为 NULL 或空字符串，传空字符串以搜索所有记忆。
     *
     * Memory store 的 search 实现通常是基于关键词匹配或向量相似度
     * （具体策略取决于 memory store 的后端实现，如 JSON 文件后端使用
     * 关键词匹配，未来可能扩展为向量数据库后端）。
     */
    cc_result_t rc = cc_memory_store_search(store,
        session_text ? session_text : "", 10, &entries, &count);

    /*
     * Step 3: 判断是否注入——搜索失败或结果为空
     *
     * 如果搜索返回错误或无匹配记忆：
     *   - 释放 rc 中可能持有的错误资源
     *   - 设置 out_memory_block = NULL
     *   - 返回 CC_OK（不是错误，只是没有可注入的记忆）
     *
     * WHY 返回 CC_OK 而非传播错误：
     *   记忆注入是增强功能，不应因为记忆搜索失败而中断上下文构建。
     *   即使没有长期记忆，LLM 仍能基于会话历史正常工作。
     */
    if (rc.code != CC_OK || count == 0) {
        cc_result_free(&rc);
        *out_memory_block = NULL;
        return cc_result_ok();
    }
    cc_result_free(&rc);

    /*
     * Step 4: 格式化记忆条目为文本块
     *
     * 使用 cc_string_builder 高效拼接多条记忆的格式化文本。
     * 输出格式：
     *   [Memory] The following are persistent facts from previous sessions:
     *   - key1: value1 (category: cat1)
     *   - key2: value2
     *
     * 每行的格式细节：
     *   - 以 "- " 开头（Markdown 无序列表格式）
     *   - "key: value" 作为主要内容
     *   - 如果条目有 category 字段，追加 " (category: xxx)" 提供分类信息
     *     category 为 LLM 提供了记忆的分类标签，帮助 LLM 判断何时引用该记忆
     *   - 每行以换行 "\n" 结尾
     *
     * cc_string_builder 的优势：
     *   - O(1) 均摊追加性能（倍增扩容策略）
     *   - 自动管理缓冲区，无需手动计算总长度
     *   - take() 语义实现零拷贝所有权转移
     */
    cc_string_builder_t sb;
    cc_string_builder_init(&sb);

    /*
     * 引导标题行：
     * [Memory] 前缀明确标识这是来自长期记忆的信息，
     * 让 LLM 能够区分短期会话历史（conversation history）
     * 和长期持久记忆（persistent facts）。
     */
    cc_string_builder_append(&sb, "[Memory] The following are persistent facts from previous sessions:");
    cc_string_builder_append(&sb, "\n");

    for (size_t i = 0; i < count; i++) {
        /*
         * 每条记忆格式化为：- key: value
         * 如果有 category，追加 (category: xxx)
         *
         * 示例输出：
         *   - user_name: 张三 (category: identity)
         *   - project_language: C
         */
        cc_string_builder_appendf(&sb, "- %s: %s", entries[i].key, entries[i].value);
        if (entries[i].category)
            cc_string_builder_appendf(&sb, " (category: %s)", entries[i].category);
        cc_string_builder_append(&sb, "\n");
    }

    /*
     * 释放 memory store 返回的条目数组
     *
     * cc_memory_entry_free_array 负责释放整个 entries 数组及其内部
     * 每个条目的 key/value/category 字符串。释放时机：在所有条目
     * 被格式化写入 sb 之后——因为格式化过程需要读取 entries[i].key 等字段，
     * 释放必须在格式化完成之后。
     */
    cc_memory_entry_free_array(entries, count);

    /*
     * 转移字符串构建器的所有权
     *
     * cc_string_builder_take 将 sb 内部的动态缓冲区指针返回给调用方，
     * 同时将 sb 重置为空状态（data=NULL, length=0, capacity=0）。
     * 调用方（cc_context_builder）负责最终 free 这个字符串。
     *
     * 零拷贝语义：不会发生额外的 strdup/memcpy，直接转移指针所有权。
     */
    *out_memory_block = cc_string_builder_take(&sb);
    return cc_result_ok();
}
