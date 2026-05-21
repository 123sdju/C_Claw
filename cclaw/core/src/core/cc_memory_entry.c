/**
 * 学习导读：cclaw/core/src/core/cc_memory_entry.c
 *
 * 所属层次：核心层。
 * 阅读重点：这里实现长期记忆条目值对象，重点看 key/value/category/session
 *           字符串的深拷贝和清理规则。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * cc_memory_entry.c — 记忆条目的生命周期管理模块
 *
 * 模块在整体架构中的角色：
 *   本模块是 c-claw 框架 Core 层中记忆子系统的基础数据单元。
 *   cc_memory_entry_t 代表一条独立的长时记忆记录，包含键值对、
 *   分类标签、会话来源和时间戳等元信息。一条 entry 是记忆存储
 *   的最小操作单元——search/list/get 都以 entry 为单位返回结果。
 *
 *   如果说 cc_memory_store_t 是记忆的"仓库"，那么 cc_memory_entry_t
 *   就是仓库中的每个"箱子"。工厂管理仓库（store），工厂不关心箱子里
 *   装什么；本模块定义箱子的格式和生命周期。
 *
 * 依赖的其他模块：
 *   - cc_memory_entry.h  — 定义 cc_memory_entry_t 结构体字段
 *   - 标准库 (stdlib.h, string.h) — memset/free 用于内存管理
 *
 * 被哪些模块使用：
 *   - cc_memory_store 系列模块 — search/list/get 返回 entry
 *   - Memory tool 和 context_builder — 消费后端返回的 entry
 *   - Context 构建层 — 读取 entry 内容注入 LLM 上下文
 *   - 序列化/反序列化层 — 将 entry 转换为 JSON 或二进制格式
 *
 * cc_memory_entry_t 结构体字段说明：
 *   - key        — 记忆的唯一键（如稳定唯一键或语义哈希字符串）
 *   - value      — 记忆的内容文本（任意字符串，通常为自然语言）
 *   - category   — 分类标签（如 "user_preference"、"project_context"）
 *                   用于分类检索和批量删除
 *   - session_id — 来源会话 ID，标记该记忆是在哪个对话中产生的
 *   - created_at — 创建时间戳（Unix time_t 格式）
 *   - updated_at — 最后更新时间戳（Unix time_t 格式）
 *
 * 内存管理约定（与 cc_message、cc_session 一致）：
 *   - entry 结构体通常由调用者在栈上分配
 *   - 字符串字段（key/value/category/session_id/timestamp）
 *     由数据生产者通过 strdup 分配堆内存
 *   - 调用者在使用完毕后调用 cc_memory_entry_free 释放字符串字段
 *   - cc_memory_entry_free_array 用于批量释放搜索/列表结果
 *
 * 三个函数的分工：
 *   - cc_memory_entry_init       — "使用前初始化"，将栈结构体清零
 *   - cc_memory_entry_free       — "使用后清理"，释放单个 entry 的堆字符串
 *   - cc_memory_entry_free_array — "批量清理"，释放搜索结果的整个数组
 *   三者形成完整的生命周期管理链：init → 填充 → 消费 → free → 结束
 *
 * 设计决策（为什么这样设计）：
 *   1. init 和 free 分离——init 不分配堆内存，free 释放堆内存，结构体本身留在栈上。
 *      为什么：与 cc_llm_response_init/free 的设计一致。结构体由调用者在
 *      栈上分配以消除堆分配开销，框架只负责管理其字符串字段的堆内存。
 *   2. free_array 遍历调用 free 后释放整个数组。
 *      为什么：search/list 返回的是后端分配的数组，需要整体释放。
 *      提供专用函数避免了调用者写出双重释放或遗漏释放的错误代码。
 *   3. free 后使用 memset 清零。
 *      为什么：防止 double-free——如果调用者误对同一个 entry 调用了两次 free，
 *      第二次 free(NULL) 是安全的无操作。同时保留了结构体可被再次 init 使用的能力。
 *   4. 不提供 create/destroy 配对（只有 init/free）。
 *      为什么：entry 总是作为"搜索/列表结果"或"输出参数"由后端填充，
 *      调用者几乎不需要主动分配堆上的 entry。init/free 的模式更适合这种
 *      "栈分配 + 框架填充"的使用场景。
 */

#include "cc/core/cc_memory_entry.h"
#include <stdlib.h>
#include <string.h>

/*
 * cc_memory_entry_init — 初始化一个记忆条目结构体
 *
 * 功能：
 *   使用 memset 将 cc_memory_entry_t 的所有字段置为零（所有指针为 NULL，
 *   数值字段为 0）。这是使用 entry 之前必须调用的初始化步骤——确保结构体
 *   的所有字段处于明确的初始状态，而非栈上残留的随机值。
 *
 *   初始化后的 entry 可以安全地传递给 cc_memory_store_get 作为输出参数，
 *   后端实现会填充 key/value/category/session_id/timestamp 字段。
 *
 * 参数：
 *   @param entry — 指向调用者在栈上分配的 cc_memory_entry_t，可以为 NULL。
 *                  NULL 时函数安全返回，不做任何操作。
 *
 * 典型使用模式（三阶段）：
 *   // 阶段1：初始化
 *   cc_memory_entry_t entry;
 *   cc_memory_entry_init(&entry);
 *
 *   // 阶段2：由后端填充
 *   cc_memory_store_get(store, "some_key", &entry);
 *
 *   // 阶段3：消费后释放
 *   printf("value: %s\n", entry.value);
 *   cc_memory_entry_free(&entry);
 *
 * 为什么需要显式 init 而非依赖 calloc：
 *   entry 通常在栈上分配，栈内存是未初始化的（包含随机值）。
 *   如果调用者忘记 init 就直接传给 store->get，后端实现通过 strdup
 *   写入了新的字符串指针，但在 free 时如果原始指针是随机野值，
 *   free 会导致未定义行为（通常是崩溃）。
 *   显式 init 将所有的"危险"操作（free 野指针）变为"安全"操作（free(NULL)）。
 *
 * 为什么 memset 而非逐字段赋值：
 *   memset(ptr, 0, sizeof(...)) 是 C 语言中"零初始化"的习惯用法，
 *   一次调用清零所有字段，比逐个写 NULL/0/0 更简洁且不易遗漏新增字段。
 *   GCC/Clang 会将这种常量大小的 memset 优化为几条 mov 指令，性能无影响。
 */
void cc_memory_entry_init(cc_memory_entry_t *entry)
{
    if (!entry) return;
    memset(entry, 0, sizeof(cc_memory_entry_t));
}

/*
 * cc_memory_entry_free — 释放单个记忆条目的堆分配字段
 *
 * 功能：
 *   释放 cc_memory_entry_t 中所有堆分配的字符串字段（key/value/category/
 *   session_id/timestamp），然后将整个结构体用 memset 清零。
 *   不会释放 entry 结构体本身——因为它通常在调用者的栈上分配。
 *
 * 参数：
 *   @param entry — 指向要清理的 entry 的指针，可以为 NULL（安全无操作）。
 *
 * 行为细节：
 *   1. entry 为 NULL → 直接返回
 *   2. 依次 free(entry->key)、free(entry->value)、free(entry->category)、
 *      free(entry->session_id)
 *      ——四个字段的释放顺序互不依赖，任何顺序都是安全的
 *   3. memset(entry, 0, sizeof(cc_memory_entry_t))
 *      ——将所有字段置零，包括指针和数值字段
 *
 * 为什么 free 后 memset 清零而非单独置 NULL：
 *   memset 是一次操作清零所有字段，效率更高且不会遗漏新增字段。
 *   清零后整个结构体处于与 init 后相同的状态，可以被再次复用
 *   （例如循环中读取多个 entry 时复用同一个栈变量）。
 *
 * 典型使用（循环复用 entry）：
 *   cc_memory_entry_t entry;
 *   cc_memory_entry_init(&entry);
 *   for (int i = 0; i < n; i++) {
 *       cc_memory_store_get(store, keys[i], &entry);
 *       process_entry(&entry);
 *       cc_memory_entry_free(&entry);  // 释放堆字符串，结构体清零可复用
 *   }
 *   // 循环结束后无需再次 free，最后一次迭代已清理
 */
void cc_memory_entry_free(cc_memory_entry_t *entry)
{
    if (!entry) return;
    free(entry->key);
    free(entry->value);
    free(entry->category);
    free(entry->session_id);
    memset(entry, 0, sizeof(cc_memory_entry_t));
}

/*
 * cc_memory_entry_free_array — 批量释放记忆条目数组
 *
 * 功能：
 *   遍历数组中的每个 cc_memory_entry_t，调用 cc_memory_entry_free
 *   释放各自的堆字符串字段，最后释放数组本身（free(entries)）。
 *   这是 cc_memory_store_search 和 cc_memory_store_list 返回结果
 *   的标准释放方式。
 *
 * 参数：
 *   @param entries — 要释放的条目数组指针（由 search/list 后端分配）。
 *                    可以为 NULL（安全无操作）。
 *                    注意：释放后此指针变为悬空，调用者不应再使用。
 *   @param count   — 数组中的条目数量。
 *
 * 典型使用（搜索并释放结果）：
 *   cc_memory_entry_t *results = NULL;
 *   size_t result_count = 0;
 *   cc_result_t rc = cc_memory_store_search(store, "查询", 5, &results, &result_count);
 *   if (rc.code == CC_OK && result_count > 0) {
 *       for (size_t i = 0; i < result_count; i++) {
 *           printf("[%s] %s\n", results[i].category, results[i].value);
 *       }
 *   }
 *   cc_memory_entry_free_array(results, result_count);  // 统一释放
 *   // results 已变为悬空指针，不应再使用
 *
 * 为什么需要专用的 free_array 函数：
 *   search/list 返回的是后端 malloc/calloc 分配的数组，包含 count 个
 *   栈上 entry 的副本（实际上整个 array 是堆分配的连续内存）。
 *   调用者必须：1) 释放每个 entry 的字符串字段  2) 释放数组本身。
 *   将这两个步骤封装为一个函数，减少了调用者的出错可能。
 *   如果没有此函数，调用者容易写出只 free(results) 忘记释放内部字符串
 *   （内存泄漏），或者释放顺序错误（先 free 数组再尝试访问内部字段导致崩溃）。
 */
void cc_memory_entry_free_array(cc_memory_entry_t *entries, size_t count)
{
    if (!entries) return;
    for (size_t i = 0; i < count; i++) {
        cc_memory_entry_free(&entries[i]);
    }
    free(entries);
}
