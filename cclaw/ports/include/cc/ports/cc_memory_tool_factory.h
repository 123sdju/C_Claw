/**
 * 学习导读：cclaw/ports/include/cc/ports/cc_memory_tool_factory.h
 *
 * 所属层次：端口层。
 * 阅读重点：这里定义可替换接口，阅读时重点看 struct + vtable + void *self 如何表达多态和依赖注入。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * cc_memory_tool_factory.h — 记忆工具与存储工厂模块
 *
 * @file    cc/ports/cc_memory_tool_factory.h
 * @brief   提供创建记忆工具（Agent 可调用的记忆操作工具箱）和
 *          记忆存储后端的工厂函数。
 *
 * 本模块在持久记忆系统中扮演"装配者"角色。它不直接操作记忆数据，
 * 而是负责：
 *
 *   1. **工具创建**：将底层记忆存储封装为 Agent 可调用的标准工具
 *      （cc_tool_t），使 LLM 能够通过函数调用接口读写记忆。
 *
 *   2. **存储创建**：根据后端标识符（如 "json_file"、"sqlite"）和配置路径，
 *      创建具体的 cc_memory_store_t 实例（适配器）。
 *
 * ─── 架构定位 ─────────────────────────────────────────────────────────
 *
 * 在端口-适配器架构中，本模块位于"适配器注册"层：
 *
 *   cc_memory_tool_factory
 *        │
 *        ├── cc_memory_tool_create()  ──→ cc_tool_t { name: "memory_set", ... }
 *        │       将 cc_memory_store_t 包装为工具
 *        │
 *        └── cc_memory_store_factory_create()  ──→ cc_memory_store_t
 *                根据 backend 标识创建具体存储实例
 *
 * ─── 工具列表 ─────────────────────────────────────────────────────────
 *
 *   cc_memory_tool_create() 创建的 cc_tool_t 实例通常是"多工具"设计：
 *   一个工具句柄实际暴露了多个子操作（memory_set、memory_get、
 *   memory_search、memory_list、memory_delete），通过 args_json
 *   中的 "operation" 字段区分。这种设计减少了工具注册表中的
 *   工具数量，让 LLM 更容易选择合适的工具。
 *
 *   LLM 调用示例：
 *   @code
 *   // 写入记忆
 *   {"operation": "set", "key": "user_pref_language", "value": "中文"}
 *
 *   // 搜索记忆
 *   {"operation": "search", "query": "用户的语言偏好"}
 *
 *   // 列出某分类的记忆
 *   {"operation": "list", "category": "user_prefs"}
 *   @endcode
 *
 * ─── 存储后端 ─────────────────────────────────────────────────────────
 *
 *   cc_memory_store_factory_create() 支持的 backend 参数：
 *     - "json_file" : 基于 JSON 文件的存储后端。简单、无需额外依赖。
 *                  适合单机开发和轻量级场景。path 参数指定 JSON 文件路径。
 *     - "sqlite" : 基于 SQLite 的存储后端。支持并发读写、事务、
 *                  复杂查询。适合生产环境。path 参数指定数据库文件路径。
 *     - "inmem"  : 纯内存存储后端。进程退出后数据丢失。
 *     - "noop"   : 空操作后端，用于禁用记忆能力。
 *                  适合测试和不需持久化的场景。path 参数忽略。
 *
 * ─── 使用模式 ─────────────────────────────────────────────────────────
 *
 *   // 1. 创建记忆存储后端
 *   cc_memory_store_t store;
 *   cc_memory_store_factory_create(&store, "json_file", "./agent_memory.json");
 *
 *   // 2. 将存储包装为 Agent 工具
 *   cc_tool_t memory_tool;
 *   cc_memory_tool_create(&store, &memory_tool);
 *
 *   // 3. 注册工具到工具注册表
 *   cc_tool_registry_register(&registry, memory_tool);
 *
 *   // 4. 清理（程序退出时）
 *   cc_memory_store_destroy(&store);
 *   // memory_tool 的 vtable->destroy 由注册表或调用方管理
 *
 * ─── 依赖 ─────────────────────────────────────────────────────────────
 *
 *   依赖 cc/core/cc_result.h（统一错误传递）
 *   依赖 cc/ports/cc_tool.h（工具抽象接口）
 *   依赖 cc/ports/cc_memory_store.h（持久记忆存储端口）
 */

#ifndef CC_MEMORY_TOOL_FACTORY_H
#define CC_MEMORY_TOOL_FACTORY_H

#include "cc/core/cc_result.h"
#include "cc/ports/cc_tool.h"
#include "cc/ports/cc_memory_store.h"

/**
 * cc_memory_tool_create — 从记忆存储创建 Agent 工具
 *
 * 将持久记忆存储包装为一个标准的 cc_tool_t 实例，使 LLM 能够
 * 通过函数调用接口来操作记忆（保存、查询、搜索、删除等）。
 *
 * 创建的 cc_tool_t 实例的 vtable 会被完整填充，包括 name()、
 * description()、schema_json() 和 call() 等函数指针。
 * 工具名称通常为 "memory"，并在 description 和 schema 中
 * 向 LLM 说明如何通过 "operation" 参数选择不同的子操作。
 *
 * 内存所有权：
 *   - 创建的 cc_tool_t 内部持有 store 的引用，不拷贝 store
 *   - store 的生命周期必须长于创建的 tool
 *   - 销毁 tool 时（通过 vtable->destroy）不会销毁 store
 *   - 调用者仍需在适当时候调用 cc_memory_store_destroy()
 *
 * @param store    已创建的持久记忆存储实例指针（不可为 NULL）。
 *                 其生命周期由调用方管理。
 * @param out_tool 输出：填充完整的工具实例（调用方负责生命周期管理）。
 *                 该 cc_tool_t 实例通常被传递给 cc_tool_registry_register()。
 * @return         CC_OK 表示工具创建成功，
 *                 CC_ERR_OUT_OF_MEMORY 表示内存分配失败
 */
cc_result_t cc_memory_tool_create(cc_memory_store_t *store, cc_tool_t *out_tool);

/**
 * cc_memory_store_factory_create — 创建持久记忆存储实例
 *
 * 根据后端标识符和配置路径，创建具体的 cc_memory_store_t 实例。
 * 这是记忆存储的工厂函数，类似于依赖注入中的 Provider。
 *
 * 工厂函数内部通过字符串匹配来选择后端实现：
 *   - "json_file" → JSON 文件存储
 *   - "sqlite"  → SQLite 数据库存储
 *   - "inmem"   → 纯内存存储
 *   - "noop"    → 空操作存储
 *   未来可能扩展更多后端（如 Redis、PostgreSQL 等）。
 *
 * 创建的 cc_memory_store_t 实例的 vtable 会被完整填充，
 * 所有 CRUD 操作（set、get、search、list、delete 等）都可用。
 * 调用方最终必须通过 cc_memory_store_destroy() 释放实例。
 *
 * @param out_store  输出：填充完整的存储实例。
 *                   调用方负责 cc_memory_store_destroy()。
 * @param backend    后端类型标识符字符串（不可为 NULL）。
 *                   目前支持 "json_file"、"sqlite"、"inmem"、"noop"。
 *                   不支持的 backend 将返回 CC_ERR_INVALID_ARGUMENT。
 * @param path       存储后端的数据路径（可为 NULL）。
 *                   - "json_file": JSON 文件的完整路径
 *                   - "sqlite":  SQLite 数据库文件路径
 *                   - "inmem":   忽略此参数
 *                   - "noop":    忽略此参数
 *                   - 为 NULL 时使用默认路径
 * @return           CC_OK 表示存储实例创建成功，
 *                   CC_ERR_INVALID_ARGUMENT 表示不支持的 backend 类型，
 *                   CC_ERR_OUT_OF_MEMORY 表示内存分配失败，
 *                   CC_ERR_IO 表示文件/数据库创建失败
 */
cc_result_t cc_memory_store_factory_create(
    cc_memory_store_t *out_store,
    const char *backend,
    const char *path
);

#endif
