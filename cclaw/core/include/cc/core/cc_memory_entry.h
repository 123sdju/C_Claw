/**
 * 学习导读：cclaw/core/include/cc/core/cc_memory_entry.h
 *
 * 所属层次：核心层。
 * 阅读重点：这里定义 Agent 运行时的数据模型、主循环和通用工具，阅读时重点看所有权、错误返回和 ReAct 数据流。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * cc_memory_entry.h — 记忆条目数据模型
 *
 * @file    cc/core/cc_memory_entry.h
 * @brief   定义 Agent 持久记忆中单条记录的数据结构和生命周期管理函数。
 *
 * 一条记忆条目（cc_memory_entry_t）是持久记忆存储（cc_memory_store_t）
 * 中的最小数据单元。它模拟了类似 Claude Memory 功能的概念：
 * Agent 可以将跨会话的知识以 key-value 形式持久保存，并在后续对话中
 * 通过语义搜索或精确查询来检索。
 *
 * ─── 接口契约 ─────────────────────────────────────────────────────────
 *
 *   本模块是纯数据结构 + 构造/析构函数定义。记忆条目不感知具体的
 *   存储后端（文件、数据库等），仅表达数据本身。
 *
 * ─── 生命周期 ─────────────────────────────────────────────────────────
 *
 *   1. 声明：在栈上声明 cc_memory_entry_t 变量
 *   2. 初始化：调用 cc_memory_entry_init() 将所有指针置零
 *   3. 填充：由存储层（cc_memory_store_t.get / search / list）填充字段
 *   4. 使用：读取各字段获取记忆内容
 *   5. 释放：调用 cc_memory_entry_free() 释放内部字符串内存
 *
 *   对于数组：使用 cc_memory_entry_free_array() 一次性释放所有条目。
 *
 * ─── 字段语义 ─────────────────────────────────────────────────────────
 *
 *   - key       : 记忆的唯一键，用于精确查询和更新
 *   - value     : 记忆的内容文本，可以是一段知识、偏好、事实等
 *   - category  : 记忆的分类标签，便于按类别批量列出和管理
 *   - session_id: 创建该记忆的会话标识，追溯记忆来源
 *   - created_at: 创建时间戳（Unix time_t 格式），用于排序和过期管理
 *   - updated_at: 最后更新时间戳，修改记忆时自动更新
 *
 * ─── 依赖 ─────────────────────────────────────────────────────────────
 *
 *   仅依赖 <time.h>（提供 time_t 类型）。
 *   不依赖任何 cc_* 模块。
 */

#ifndef CC_MEMORY_ENTRY_H
#define CC_MEMORY_ENTRY_H

#include <time.h>

/**
 * cc_memory_entry_t — 记忆条目结构体
 *
 * 表示 Agent 持久记忆中的一条记录。每条记忆由一个唯一键（key）索引，
 * 包含值（value）、分类（category）和来源会话（session_id）等元数据。
 * 记忆条目支持创建、查询、更新和删除（由 cc_memory_store_t 的接口实现）。
 *
 * 所有字符串字段均为动态分配，由存储层填充。
 * 上层使用者不负责分配这些字段，但需要通过 cc_memory_entry_free()
 * 释放查询结果中的动态内存。
 */
typedef struct cc_memory_entry {
    char *key;          /**< 记忆的唯一键，用于精确索引和去重。
                         *   建议使用有意义的标识符（如 "user_pref_language"）。
                         *   对同一个 key 调用 set 会覆盖旧值并更新 updated_at。 */
    char *value;        /**< 记忆的值/内容文本。可以是任意自然语言信息，
                         *   如"用户偏好中文回复"、"上次讨论的主题是微服务架构"等。
                         *   此字段由搜索/查询操作填充，NULL 表示未设置。 */
    char *category;     /**< 记忆的分类标签。用于将相关记忆分组管理。
                         *   例如 "user_prefs"、"project_context"、"learned_facts"。
                         *   通过 cc_memory_store_list() 可按分类列出。可为 NULL。 */
    char *session_id;   /**< 创建或更新该记忆的会话 ID。用于追溯记忆来源，
                         *   便于在会话结束后保留关键上下文。可为 NULL。 */
    time_t created_at;  /**< 记忆首次创建的时间戳（Unix time_t 格式）。
                         *   由存储层在 set 时自动设置，用于排序和过期策略。 */
    time_t updated_at;  /**< 记忆最后一次更新时间戳（Unix time_t 格式）。
                         *   新建时与 created_at 相同，每次 set 时更新。 */
} cc_memory_entry_t;

/**
 * cc_memory_entry_init — 初始化栈上的记忆条目
 *
 * 将 cc_memory_entry_t 的所有字段置为安全的默认值：
 * 字符串指针置 NULL，时间戳置 0。
 * 在声明栈上的 cc_memory_entry_t 后必须调用此函数。
 *
 * @param entry  要初始化的记忆条目指针（不可为 NULL）
 */
void cc_memory_entry_init(cc_memory_entry_t *entry);

/**
 * cc_memory_entry_free — 释放记忆条目中的动态内存
 *
 * 释放 entry 中所有动态分配的字符串字段（key、value、category、session_id）。
 * 不释放 entry 指针本身（它可能在栈上或被包含在更大的结构中）。
 * 传入 NULL 是安全的（无操作）。重复调用也是安全的（字段会被置 NULL）。
 *
 * @param entry  要释放内部资源的记忆条目指针（可为 NULL）
 */
void cc_memory_entry_free(cc_memory_entry_t *entry);

/**
 * cc_memory_entry_free_array — 批量释放记忆条目数组
 *
 * 对数组中的每个条目调用 cc_memory_entry_free()，然后通常配合
 * 调用方的 free() 释放数组指针本身（本函数不释放数组指针）。
 *
 * 典型使用模式：
 * @code
 *   cc_memory_entry_t *entries = NULL;
 *   size_t count = 0;
 *   cc_memory_store_search(store, "query", 10, &entries, &count);
 *   // ... 使用 entries ...
 *   cc_memory_entry_free_array(entries, count);
 *   free(entries);
 * @endcode
 *
 * @param entries  指向记忆条目数组首元素的指针（可为 NULL）
 * @param count    数组中的条目数量
 */
void cc_memory_entry_free_array(cc_memory_entry_t *entries, size_t count);

#endif