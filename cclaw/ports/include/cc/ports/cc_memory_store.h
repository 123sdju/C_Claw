/**
 * 学习导读：cclaw/ports/include/cc/ports/cc_memory_store.h
 *
 * 所属层次：端口层。
 * 阅读重点：这里定义可替换接口，阅读时重点看 struct + vtable + void *self 如何表达多态和依赖注入。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * cc_memory_store.h — 持久记忆存储端口（Port）
 *
 * @file    cc/ports/cc_memory_store.h
 * @brief   定义 Agent 跨会话知识持久存储的抽象接口。采用 vtable 多态模式。
 *
 * 记忆存储（cc_memory_store_t）是 c-claw 中模拟 Claude Memory 功能的
 * 核心抽象。它允许 Agent 在会话间持久保存知识片段、用户偏好和
 * 学习到的事实，并在后续对话中通过精确查询或语义搜索来检索。
 *
 * ─── 架构定位 ─────────────────────────────────────────────────────────
 *
 * 在端口-适配器（Ports & Adapters）架构中：
 *   - 本模块是"端口"（Port）：定义了记忆存储的抽象合约
 *   - 具体存储后端（JSON 文件、SQLite、嵌入向量索引等）是"适配器"
 *   - 上层业务代码只依赖本端口，不感知存储实现细节
 *
 * ─── 接口契约 ─────────────────────────────────────────────────────────
 *
 *   每个 cc_memory_store_t 由两个部分组成：
 *     - self    : 指向具体存储实现的私有数据
 *     - vtable  : 指向虚函数表，定义了所有 CRUD 操作
 *
 *   实现者必须填充 vtable 中的所有函数指针。
 *   调用者通过便捷函数（如 cc_memory_store_set()）间接调用 vtable，
 *   这些便捷函数负责空指针检查等防御性编程。
 *
 * ─── 操作语义 ─────────────────────────────────────────────────────────
 *
 *   - set()              : 写入/覆盖一条记忆。同一 key 重复调用会更新值和时间戳
 *   - get()              : 通过 key 精确查询一条记忆。不存在时返回 CC_ERR_NOT_FOUND
 *   - search()           : 通过语义相似度搜索记忆。query 是自然语言查询文本，
 *                          实现层可能使用 TF-IDF、嵌入向量或其他算法
 *   - list()             : 按分类列出记忆。category 为 NULL 时返回所有分类的记忆
 *   - delete_entry()     : 删除单条记忆。不存在时返回 CC_ERR_NOT_FOUND
 *   - delete_by_category(): 批量删除某个分类下的所有记忆
 *   - destroy()          : 销毁存储实例，关闭文件/连接，释放资源
 *
 * ─── 使用模式 ─────────────────────────────────────────────────────────
 *
 *   // 1. 通过工厂创建存储实例
 *   cc_memory_store_t store;
 *   cc_memory_store_factory_create(&store, "file", "/path/to/memory.json");
 *
 *   // 2. 保存记忆
 *   cc_memory_store_set(&store, "user_name", "张三",
 *                        "user_prefs", "session_abc");
 *
 *   // 3. 查询记忆
 *   cc_memory_entry_t entry;
 *   cc_memory_entry_init(&entry);
 *   cc_result_t rc = cc_memory_store_get(&store, "user_name", &entry);
 *   if (rc.code == CC_OK) {
 *       printf("值: %s\n", entry.value);
 *   }
 *   cc_memory_entry_free(&entry);
 *
 *   // 4. 搜索记忆
 *   cc_memory_entry_t *results = NULL;
 *   size_t count = 0;
 *   cc_memory_store_search(&store, "用户叫什么名字", 5, &results, &count);
 *   // ... 使用 results ...
 *   cc_memory_entry_free_array(results, count);
 *   free(results);
 *
 *   // 5. 销毁（程序退出时）
 *   cc_memory_store_destroy(&store);
 *
 * ─── 依赖 ─────────────────────────────────────────────────────────────
 *
 *   依赖 cc/core/cc_result.h（统一错误传递）
 *   依赖 cc/core/cc_memory_entry.h（记忆条目数据模型）
 *   不依赖任何其他 cc_* 模块。
 */

#ifndef CC_MEMORY_STORE_H
#define CC_MEMORY_STORE_H

#include "cc/core/cc_result.h"
#include "cc/core/cc_memory_entry.h"

/* ── 前向声明 ───────────────────────────────────────────────────────── */

typedef struct cc_memory_store_vtable cc_memory_store_vtable_t;
/**
 * cc_memory_store_t — 前向声明的端口/服务句柄类型，具体字段在本文件后文或对应端口中定义。
 */
typedef struct cc_memory_store cc_memory_store_t;

/**
 * cc_memory_store_t — 记忆存储实例（多态句柄）
 *
 * 这是一个值语义的结构体，通过 self + vtable 实现多态。
 * 可以直接按值传递和拷贝，浅拷贝后两个实例指向同一个底层存储对象。
 * 生命周期：由工厂函数创建，通过 cc_memory_store_destroy() 销毁。
 *
 * @note 浅拷贝后的实例共享同一个 self，不要在其中一个上调用 destroy，
 *       否则另一个实例将成为悬空指针。
 */
struct cc_memory_store {
    void *self;                        /**< 指向具体存储实现的私有数据。
                                        *   类型因后端而异（文件存储、DB 等）。
                                        *   由工厂函数分配，destroy 时释放。 */
    const cc_memory_store_vtable_t *vtable; /**< 虚函数表指针，定义了所有存储操作 */
};

/**
 * cc_memory_store_vtable_t — 记忆存储虚函数表
 *
 * 定义持久记忆存储的抽象 CRUD 接口。每个具体的存储后端
 * （文件、SQLite、内存等）必须实现此表中的所有函数指针。
 * 类似于面向对象语言中的接口/抽象类。
 */
struct cc_memory_store_vtable {
    /**
     * set — 写入或更新一条记忆
     *
     * 如果 key 对应的记忆已存在，则更新 value、category、session_id
     * 并刷新 updated_at 时间戳；否则创建新条目。
     *
     * @param self       存储私有数据
     * @param key        记忆的唯一键（不可为 NULL，不可为空字符串）
     * @param value      记忆的内容文本（不可为 NULL）
     * @param category   分类标签（可为 NULL，表示无分类）
     * @param session_id 来源会话 ID（可为 NULL）
     * @return           CC_OK 表示成功写入
     */
    cc_result_t (*set)(
        void *self,
        const char *key,
        const char *value,
        const char *category,
        const char *session_id
    );

    /**
     * get — 通过键精确查询一条记忆
     *
     * 将查询到的记忆条目拷贝到 out_entry 中。
     * 调用者需要先调用 cc_memory_entry_init() 初始化 out_entry，
     * 使用完毕后调用 cc_memory_entry_free() 释放。
     *
     * @param self       存储私有数据
     * @param key        要查询的记忆键（不可为 NULL）
     * @param out_entry  输出：查询到的记忆条目。
     *                   调用前需 init，调用后需 free。
     * @return           CC_OK 表示找到，CC_ERR_NOT_FOUND 表示不存在
     */
    cc_result_t (*get)(
        void *self,
        const char *key,
        cc_memory_entry_t *out_entry
    );

    /**
     * search — 语义搜索记忆
     *
     * 根据自然语言查询文本，找到语义上最相关的记忆条目。
     * 具体的相关性算法由存储后端实现（嵌入向量、关键词匹配等）。
     * 结果按相关度降序排列，最多返回 limit 条。
     *
     * @param self         存储私有数据
     * @param query        自然语言查询文本（不可为 NULL）
     * @param limit        返回结果的最大数量。
     *                     设为 0 或负数时，实现方应返回合理默认值（如 10）。
     * @param out_entries  输出：指向结果数组的指针（由实现方 malloc 分配）。
     *                     调用方负责先 cc_memory_entry_free_array() 再 free()。
     * @param out_count    输出：实际返回的结果数量
     * @return             CC_OK 表示搜索成功（即使结果为空）
     */
    cc_result_t (*search)(
        void *self,
        const char *query,
        int limit,
        cc_memory_entry_t **out_entries,
        size_t *out_count
    );

    /**
     * list — 按分类列出记忆
     *
     * 返回指定分类下的所有记忆条目，按更新时间降序排列。
     * 如果 category 为 NULL 或空字符串，则返回所有分类的记忆。
     *
     * @param self         存储私有数据
     * @param category     分类标签。NULL 或 "" 表示不限分类。
     * @param limit        返回结果的最大数量。0 或负数表示无限制（慎用）。
     * @param out_entries  输出：指向结果数组的指针（由实现方 malloc 分配）
     * @param out_count    输出：实际返回的结果数量
     * @return             CC_OK 表示成功
     */
    cc_result_t (*list)(
        void *self,
        const char *category,
        int limit,
        cc_memory_entry_t **out_entries,
        size_t *out_count
    );

    /**
     * delete_entry — 删除单条记忆
     *
     * 根据 key 精确删除一条记忆。如果 key 不存在也返回成功
     * （幂等操作，因为最终状态一致）。
     *
     * @param self  存储私有数据
     * @param key   要删除的记忆键（不可为 NULL）
     * @return      CC_OK 表示删除成功（或键本就不存在）
     */
    cc_result_t (*delete_entry)(
        void *self,
        const char *key
    );

    /**
     * delete_by_category — 按分类批量删除记忆
     *
     * 删除指定分类下的所有记忆条目。这是一个破坏性操作，
     * 调用前应考虑是否需要确认。
     *
     * @param self     存储私有数据
     * @param category 要删除的分类标签（不可为 NULL）
     * @return         CC_OK 表示删除成功
     */
    cc_result_t (*delete_by_category)(
        void *self,
        const char *category
    );

    /**
     * destroy — 销毁存储实例
     *
     * 释放 self 指向的私有数据，关闭文件句柄或数据库连接。
     * 销毁后 store 实例不可再使用。
     * 传入 NULL self 是安全的（无操作）。
     *
     * @param self  存储私有数据（可为 NULL）
     */
    void (*destroy)(void *self);
};

/* ── 便捷包装函数 ─────────────────────────────────────────────────────
 *
 * 以下函数是对 vtable 中对应函数指针的薄封装。
 * 它们负责：
 *   1. 检查 store 和 vtable 非 NULL
 *   2. 检查对应的函数指针非 NULL
 *   3. 转发调用到 vtable 中的实现
 *
 * 上层代码应始终通过以下函数访问存储，而不是直接操作 vtable。
 * ──────────────────────────────────────────────────────────────────── */

/**
 * cc_memory_store_set — 写入或更新一条记忆
 *
 * @param store       记忆存储实例指针（不可为 NULL，vtable 必须完整）
 * @param key         记忆的唯一键（不可为 NULL）
 * @param value       记忆的内容文本（不可为 NULL）
 * @param category    分类标签（可为 NULL）
 * @param session_id  来源会话 ID（可为 NULL）
 * @return            CC_OK 表示成功
 */
cc_result_t cc_memory_store_set(
    cc_memory_store_t *store,
    const char *key,
    const char *value,
    const char *category,
    const char *session_id
);

/**
 * cc_memory_store_get — 通过键精确查询一条记忆
 *
 * @param store      记忆存储实例指针（不可为 NULL）
 * @param key        要查询的记忆键（不可为 NULL）
 * @param out_entry  输出：查询结果（调用前需 init，调用后需 free）
 * @return           CC_OK / CC_ERR_NOT_FOUND
 */
cc_result_t cc_memory_store_get(
    cc_memory_store_t *store,
    const char *key,
    cc_memory_entry_t *out_entry
);

/**
 * cc_memory_store_search — 语义搜索记忆
 *
 * @param store        记忆存储实例指针（不可为 NULL）
 * @param query        自然语言查询文本（不可为 NULL）
 * @param limit        返回结果的最大数量
 * @param out_entries  输出：结果数组（调用方负责释放）
 * @param out_count    输出：结果数量
 * @return             CC_OK 表示搜索成功
 */
cc_result_t cc_memory_store_search(
    cc_memory_store_t *store,
    const char *query,
    int limit,
    cc_memory_entry_t **out_entries,
    size_t *out_count
);

/**
 * cc_memory_store_list — 按分类列出记忆
 *
 * @param store        记忆存储实例指针（不可为 NULL）
 * @param category     分类标签（可为 NULL 表示不限）
 * @param limit        返回结果的最大数量
 * @param out_entries  输出：结果数组（调用方负责释放）
 * @param out_count    输出：结果数量
 * @return             CC_OK 表示成功
 */
cc_result_t cc_memory_store_list(
    cc_memory_store_t *store,
    const char *category,
    int limit,
    cc_memory_entry_t **out_entries,
    size_t *out_count
);

/**
 * cc_memory_store_delete — 删除单条记忆
 *
 * @param store  记忆存储实例指针（不可为 NULL）
 * @param key    要删除的记忆键（不可为 NULL）
 * @return       CC_OK 表示成功
 */
cc_result_t cc_memory_store_delete(
    cc_memory_store_t *store,
    const char *key
);

/**
 * cc_memory_store_delete_category — 按分类批量删除记忆
 *
 * @param store     记忆存储实例指针（不可为 NULL）
 * @param category  要删除的分类标签（不可为 NULL）
 * @return          CC_OK 表示成功
 */
cc_result_t cc_memory_store_delete_category(
    cc_memory_store_t *store,
    const char *category
);

/**
 * cc_memory_store_destroy — 销毁记忆存储实例
 *
 * 释放 store 内部的所有资源。销毁后 store 指针不可再使用。
 * 传入 NULL 是安全的（无操作）。
 * 注意：本函数只释放 self 指向的资源，不释放 store 指针本身。
 *
 * @param store  要销毁的记忆存储实例指针（可为 NULL）
 */
void cc_memory_store_destroy(cc_memory_store_t *store);

#endif