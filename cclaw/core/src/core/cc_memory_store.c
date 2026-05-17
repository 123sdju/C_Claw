/**
 * 学习导读：cclaw/core/src/core/cc_memory_store.c
 *
 * 所属层次：核心层。
 * 阅读重点：这里定义 Agent 运行时的数据模型、主循环和通用工具，阅读时重点看所有权、错误返回和 ReAct 数据流。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * cc_memory_store.c — 记忆存储虚表封装模块
 *
 * 模块在整体架构中的角色：
 *   本模块是 c-claw 框架 Core 层中记忆（Memory）子系统的对外接口层。
 *   它不实现具体的存储逻辑，而是对 cc_memory_store_t 虚表（vtable）的
 *   薄封装——每个公开函数都执行统一的参数校验，然后委托给虚表中对应的
 *   函数指针。这是一种经典的"外观模式 + 策略模式"组合。
 *
 *   记忆子系统让 AI Agent 拥有"长期记忆"能力——Agent 可以在不同的会话
 *   之间记住用户偏好、历史上下文、关键信息片段，从而提供更个性化的体验。
 *   在 RAG（检索增强生成）架构中，记忆存储扮演了向量数据库的角色。
 *
 * 依赖的其他模块：
 *   - cc_memory_store.h  — 定义 cc_memory_store_t 结构体（含 vtable + self 指针）
 *   - cc_memory_entry.h  — 定义 cc_memory_entry_t（key/value/category/session_id）
 *   - cc_result.h        — 统一错误返回类型
 *   - 标准库 (stdlib.h)
 *
 * 被哪些模块使用：
 *   - Memory tool 和 context_builder — 通过本接口写入、检索并注入长期记忆
 *   - Tool 层     — memory 工具通过本接口执行 set/get/search/list/delete
 *   - Context 构建层 — 在构建 LLM 上下文时注入相关记忆
 *
 * 核心操作（CRUD + 搜索）：
 *   - set             — 写入/更新一条记忆（按 key 去重）
 *   - get             — 按 key 精确查找一条记忆
 *   - search          — 语义搜索（由后端实现具体的相似度匹配算法）
 *   - list            — 按 category（分类）列出记忆
 *   - delete          — 按 key 删除单条记忆
 *   - delete_category — 批量删除某个分类下的全部记忆
 *   - destroy         — 销毁记忆存储实例，释放所有资源
 *
 * 设计决策（为什么这样设计）：
 *   1. 使用虚表模式（vtable + self），而非直接实现函数。
 *      为什么：记忆存储的后端可能有多种实现——内存哈希表、SQLite 数据库、
 *      Redis 远程服务、向量数据库等。虚表使得上层代码无需知道具体后端，
 *      只需通过统一的 cc_memory_store_t 接口操作数据。新的存储后端可以
 *      在不修改任何调用方代码的情况下替换。
 *   2. 每个公开函数先做参数校验（NULL 检查），再委托给虚表。
 *      为什么：将公共的防御性检查集中到封装层，避免了每个后端实现
 *      都要重复编写相同的 NULL 检查代码。这遵循了 DRY 原则。
 *      如果校验失败，直接返回 CC_ERR_INVALID_ARGUMENT，不触发后端调用。
 *   3. 虚表方法不接受 cc_memory_store_t* 而是直接接收 store->self。
 *      为什么：store->self 是后端实例指针（void*），后端实现不需要知道
 *      cc_memory_store_t 的结构。这保持了"接口与实现分离"的纯粹性——
 *      后端代码不依赖任何框架头文件，可以独立编译和测试。
 *   4. destroy 函数将 self 和 vtable 置 NULL 后再返回。
 *      为什么：防止调用者在 destroy 后误用已释放的 store。虽然 store
 *      本身可能仍在栈上，但置 NULL 的 self/vtable 使得后续的任何调用
 *      都会因为 NULL 检查而安全返回，而非触发 use-after-free 崩溃。
 *
 * 与 cc_session_store_t 的区别：
 *   - cc_session_store_t — 会话级别的持久化（messages, sessions）
 *   - cc_memory_store_t  — 跨会话的长期记忆（key-value, 语义搜索）
 *   两者面向不同的使用场景和管理粒度，不应混用。
 */

#include "cc/ports/cc_memory_store.h"
#include <stdlib.h>

/*
 * cc_memory_store_set — 写入或更新一条记忆条目
 *
 * 功能：
 *   将一条 key-value 记忆写入存储后端。如果 key 已存在，则更新其 value
 *   和元数据（具体行为由后端虚表实现决定，但语义上应视为 upsert）。
 *
 *   这是一条记忆写入的标准入口——无论是用户手动输入"记住xxx"，
 *   还是 LLM 自动提取的重要信息，都通过此函数持久化。
 *
 * 参数：
 *   @param key        — 记忆的唯一键（必填，不可为 NULL）。
 *                       通常由调用者生成，建议使用稳定唯一键或语义哈希。
 *                       为什么需要 key：支持精确查找（get）和删除（delete），
 *                       比纯语义搜索更高效。
 *   @param value      — 记忆的内容文本（必填，不可为 NULL）。
 *                       可以是任意文本，包括结构化数据（JSON）或自然语言。
 *   @param category   — 记忆的分类标签（可选，可以为 NULL）。
 *                       用途：按分类批量检索或删除，如 "user_preference"、
 *                       "project_context"、"technical_knowledge"。
 *   @param session_id — 关联的会话 ID（可选，可以为 NULL）。
 *                       用途：标记该记忆来源于哪个对话，方便追溯上下文。
 *   @param store      — 记忆存储实例（必填）。
 *
 * @return CC_OK 表示写入成功。
 * @return CC_ERR_INVALID_ARGUMENT 表示 store/vtable/set 函数指针或
 *         key/value 任一为 NULL。
 * @return 其他错误码由后端实现返回（如 CC_ERR_STORAGE）。
 *
 * 典型使用场景：
 *   // LLM 说 "用户的名字是张三"
 *   cc_memory_store_set(store, "user_name_001", "张三", "user_info", session->id);
 *
 *   // 用户手动输入 "记住：项目的数据库地址是 localhost:5432"
 *   cc_memory_store_set(store, "db_config", "localhost:5432", "project_config", NULL);
 *
 * 安全性注意：
 *   本函数不检查 key 或 value 的内容合法性（如 SQL 注入防护）——
 *   这些由后端实现负责。封装层只检查指针非 NULL。
 */
cc_result_t cc_memory_store_set(
    cc_memory_store_t *store,
    const char *key, const char *value,
    const char *category, const char *session_id
)
{
    if (!store || !store->vtable || !store->vtable->set || !key || !value)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid memory store set arguments");
    return store->vtable->set(store->self, key, value, category, session_id);
}

/*
 * cc_memory_store_get — 按 key 精确检索一条记忆
 *
 * 功能：
 *   根据唯一键 key 从存储后端精确检索一条记忆条目。与 search 不同，
 *   get 是精确匹配，不涉及语义相似度计算，因此查询效率远高于 search。
 *
 *   返回的 cc_memory_entry_t 包含该记忆的完整信息（key/value/category/
 *   session_id/timestamp）。调用者通过检查返回值的 code 是否为 CC_OK
 *   来判断记忆是否存在。
 *
 * 参数：
 *   @param store     — 记忆存储实例（必填）
 *   @param key       — 要查找的记忆键（必填，不可为 NULL）
 *   @param out_entry — [out] 输出参数，由调用者分配栈空间。
 *                      后端实现将 key/value/category/session_id/timestamp
 *                      等字段填充到此结构体中。
 *                      如果记忆不存在，后端应返回 CC_ERR_NOT_FOUND，
 *                      此时 out_entry 的值未定义，调用者不应使用。
 *
 * @return CC_OK 表示找到，out_entry 已被填充。
 * @return CC_ERR_NOT_FOUND 表示 key 不存在。
 * @return CC_ERR_INVALID_ARGUMENT 表示 store/vtable/get/key/out_entry 有 NULL。
 *
 * 内存管理约定：
 *   后端实现填充的 entry 字段（key/value/category/session_id）是后端
 *   通过 strdup 分配的堆字符串。调用者在使用完毕后必须调用
 *   cc_memory_entry_free(out_entry) 释放这些字符串。
 *   注意：cc_memory_entry_free 不会释放 entry 结构体本身（它是栈分配的），
 *   只释放其中的字符串字段。
 *
 * 典型使用模式：
 *   cc_memory_entry_t entry;
 *   cc_memory_entry_init(&entry);  // 初始化为零
 *   cc_result_t rc = cc_memory_store_get(store, "user_name_001", &entry);
 *   if (rc.code == CC_OK) {
 *       printf("Found: %s = %s\n", entry.key, entry.value);
 *   } else {
 *       printf("Not found\n");
 *   }
 *   cc_memory_entry_free(&entry);  // 释放堆字符串
 *
 * 设计决策：
 *   为什么调用者分配 entry 而非后端分配？
 *   允许在栈上分配 entry，避免额外的堆分配。entry 结构体较小
 *   （约 32 字节），栈分配成本为零。这与 cc_llm_response_init
 *   的设计思路一致——高频操作中栈分配优于堆分配。
 */
cc_result_t cc_memory_store_get(
    cc_memory_store_t *store,
    const char *key,
    cc_memory_entry_t *out_entry
)
{
    if (!store || !store->vtable || !store->vtable->get || !key || !out_entry)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid memory store get arguments");
    return store->vtable->get(store->self, key, out_entry);
}

/*
 * cc_memory_store_search — 语义搜索记忆条目
 *
 * 功能：
 *   根据查询文本（query）在记忆存储中执行语义搜索，返回与查询内容
 *   最相关的前 limit 条记忆。这是 RAG 架构中"检索"步骤的核心接口——
 *   将用户查询向量化后，在记忆库中找到相关的上下文片段。
 *
 *   与 get 的精确匹配不同，search 使用语义相似度算法（如余弦相似度、
 *   向量距离等），具体算法由后端实现决定。
 *
 * 参数：
 *   @param store       — 记忆存储实例（必填）
 *   @param query       — 搜索查询文本（必填，不可为 NULL）。
 *                        可以是自然语言问题或关键词。
 *                        如 "数据库地址是什么"、"项目的技术栈"。
 *   @param limit       — 返回结果的最大数量。
 *                        传入 0 表示不限制（由后端决定合理上限）。
 *                        传入正数表示最多返回 limit 条。
 *                        为什么需要 limit：LLM 的上下文窗口有限，
 *                        注入过多记忆会挤占对话空间，通常取 3-10 条。
 *   @param out_entries — [out] 输出参数，指向后端分配的 cc_memory_entry_t 数组。
 *                        调用者负责释放：先遍历释放每个元素
 *                        （cc_memory_entry_free），再 free 数组本身。
 *                        或者直接调用 cc_memory_entry_free_array。
 *   @param out_count   — [out] 输出参数，指向实际返回的记忆条目数量。
 *                        调用者据此遍历 out_entries 数组。
 *
 * @return CC_OK 表示搜索完成（即使结果为空也是成功）。
 * @return CC_ERR_INVALID_ARGUMENT 表示 store/vtable/search/query/
 *         out_entries/out_count 任一为 NULL。
 *
 * 典型使用场景：
 *   // 用户问 "我的数据库地址是什么"
 *   cc_memory_entry_t *results = NULL;
 *   size_t count = 0;
 *   cc_memory_store_search(store, "数据库地址", 5, &results, &count);
 *   // 将 results 中的内容注入到 LLM 的 context 中
 *   for (size_t i = 0; i < count; i++) {
 *       inject_into_context(results[i].key, results[i].value);
 *   }
 *   cc_memory_entry_free_array(results, count);  // 释放结果
 *
 * 为什么后备分配数组而非由调用者分配：
 *   结果数量在调用前未知（取决于搜索命中数），调用者无法预分配合适大小。
 *   由后端分配动态数组是最自然的做法。调用者通过 free_array 统一释放。
 */
cc_result_t cc_memory_store_search(
    cc_memory_store_t *store,
    const char *query,
    int limit,
    cc_memory_entry_t **out_entries,
    size_t *out_count
)
{
    if (!store || !store->vtable || !store->vtable->search || !query || !out_entries || !out_count)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid memory store search arguments");
    return store->vtable->search(store->self, query, limit, out_entries, out_count);
}

/*
 * cc_memory_store_list — 按分类列出记忆条目
 *
 * 功能：
 *   列出指定分类（category）下的所有记忆条目，支持分页限制（limit）。
 *   与 search 不同，list 不涉及语义匹配——只是按分类标签过滤。
 *   当 category 为 NULL 时，列出所有分类的记忆（全量遍历）。
 *
 * 参数：
 *   @param store       — 记忆存储实例（必填）
 *   @param category    — 分类标签筛选（可选，可以为 NULL）。
 *                        NULL 表示列出所有分类。
 *                        "user_preference" 表示只列出用户偏好类记忆。
 *   @param limit       — 返回结果的最大数量。0 表示不限制。
 *   @param out_entries — [out] 输出参数，指向后端分配的条目数组。
 *   @param out_count   — [out] 输出参数，实际条目数。
 *
 * @return CC_OK 表示列出成功。
 * @return CC_ERR_INVALID_ARGUMENT 表示 store/vtable/list/
 *         out_entries/out_count 有 NULL。
 *
 * 典型使用场景：
 *   // 列出所有项目配置记忆
 *   cc_memory_store_list(store, "project_config", 0, &entries, &count);
 *
 *   // 列出全部记忆（管理/调试用途）
 *   cc_memory_store_list(store, NULL, 0, &entries, &count);
 */
cc_result_t cc_memory_store_list(
    cc_memory_store_t *store,
    const char *category,
    int limit,
    cc_memory_entry_t **out_entries,
    size_t *out_count
)
{
    if (!store || !store->vtable || !store->vtable->list || !out_entries || !out_count)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid memory store list arguments");
    return store->vtable->list(store->self, category, limit, out_entries, out_count);
}

/*
 * cc_memory_store_delete — 按 key 删除单条记忆
 *
 * 功能：
 *   从存储后端中精确删除指定 key 对应的记忆条目。
 *   如果 key 不存在，后端应返回 CC_ERR_NOT_FOUND。
 *
 * 参数：
 *   @param store — 记忆存储实例（必填）
 *   @param key   — 要删除的记忆键（必填，不可为 NULL）
 *
 * @return CC_OK 表示删除成功。
 * @return CC_ERR_NOT_FOUND 表示 key 不存在。
 * @return CC_ERR_INVALID_ARGUMENT 表示 store/vtable/delete_entry/key 有 NULL。
 *
 * 典型使用场景：
 *   // 用户说 "忘记我的数据库地址"
 *   cc_memory_store_delete(store, "db_config");
 *
 * 为什么命名为 delete 而非 remove 或 erase：
 *   "delete" 在 CRUD 术语中是标准操作名，与 get/set/search 形成
 *   一致的命名体系。delete_entry 是虚表中的函数名，以区别于
 *   delete_by_category（按分类删除）。
 */
cc_result_t cc_memory_store_delete(
    cc_memory_store_t *store,
    const char *key
)
{
    if (!store || !store->vtable || !store->vtable->delete_entry || !key)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid memory store delete arguments");
    return store->vtable->delete_entry(store->self, key);
}

/*
 * cc_memory_store_delete_category — 批量删除某个分类的记忆
 *
 * 功能：
 *   删除指定分类（category）标签下的所有记忆条目。这是批量操作——
 *   可能影响多条记录。
 *
 * 参数：
 *   @param store    — 记忆存储实例（必填）
 *   @param category — 分类标签（必填，不可为 NULL）。
 *                     只删除此分类下的记忆，不影响其他分类。
 *
 * @return CC_OK 表示删除成功（即使分类下没有条目也是成功）。
 * @return CC_ERR_INVALID_ARGUMENT 表示 store/vtable/
 *         delete_by_category/category 有 NULL。
 *
 * 典型使用场景：
 *   // 用户切换项目，清空旧项目的配置记忆
 *   cc_memory_store_delete_category(store, "project_config");
 *
 * 为什么需要批量删除：
 *   某些记忆分类（如临时上下文、阶段性项目信息）应该在特定条件
 *   下被整体清理。逐个 key 删除的效率低且容易遗漏。
 *   批量操作允许后端实现优化（如直接删除一个哈希桶而非逐个扫描）。
 */
cc_result_t cc_memory_store_delete_category(
    cc_memory_store_t *store,
    const char *category
)
{
    if (!store || !store->vtable || !store->vtable->delete_by_category || !category)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid memory store delete_category arguments");
    return store->vtable->delete_by_category(store->self, category);
}

/*
 * cc_memory_store_destroy — 销毁记忆存储实例
 *
 * 功能：
 *   通过虚表调用后端实现的 destroy 函数释放记忆存储的所有资源
 *   （数据库连接、内存索引、文件句柄等），然后将 store 的 self
 *   和 vtable 指针置 NULL 以防止后续误用。
 *
 * 参数：
 *   @param store — 要销毁的记忆存储实例（可以为 NULL，安全无操作）。
 *
 * 行为细节：
 *   1. 检查 store/vtable/destroy 均非 NULL，否则直接返回
 *   2. 调用 store->vtable->destroy(store->self) — 后端释放其资源
 *   3. store->self = NULL; store->vtable = NULL; — 防止误用
 *
 * 为什么 destroy 后置 NULL 而不自动 free store 本身：
 *   与 cc_result_t 的逻辑一致——store 可能由调用者在栈上分配，
 *   框架不应假设其内存来源。置 NULL 的 self 和 vtable 使得
 *   后续的 get/set/search 调用会因为 NULL 检查而安全返回。
 */
void cc_memory_store_destroy(cc_memory_store_t *store)
{
    if (!store || !store->vtable || !store->vtable->destroy) return;
    store->vtable->destroy(store->self);
    store->self = NULL;
    store->vtable = NULL;
}