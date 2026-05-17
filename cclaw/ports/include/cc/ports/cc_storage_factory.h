/**
 * 学习导读：cclaw/ports/include/cc/ports/cc_storage_factory.h
 *
 * 所属层次：端口层。
 * 阅读重点：这里定义可替换接口，阅读时重点看 struct + vtable + void *self 如何表达多态和依赖注入。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * cc_storage_factory.h — 存储工厂端口（Port）
 *
 * @file    cc/ports/cc_storage_factory.h
 * @brief   根据配置类型创建具体的会话存储实例。实现工厂模式。
 *
 * 本模块是创建会话存储的统一入口。调用方只需传入 cc_config_t，
 * 工厂函数根据 config.storage_type 创建对应的存储后端，
 * 并以 cc_session_store_t（vtable 多态接口）形式返回。
 *
 * ─── 接口契约 ─────────────────────────────────────────────────────────
 *
 *   - 唯一的公开函数 cc_storage_factory_create_store()
 *   - 根据 storage_type 选择实现："json" → JSON 文件存储，"sqlite" → SQLite 存储
 *   - 返回的 cc_session_store_t 包含 vtable，调用方通过虚函数表操作
 *   - 调用方负责在不再使用时调用 vtable->destroy()
 *
 * ─── 设计意图 ─────────────────────────────────────────────────────────
 *
 *   工厂模式将"创建哪种存储"的决策集中在一处。将来新增存储后端
 *   （如 Redis、PostgreSQL）只需在工厂函数中增加一个分支，
 *   其他代码无需变更。这正是端口-适配器架构的优势。
 *
 * ─── 依赖 ─────────────────────────────────────────────────────────────
 *
 *   依赖 cc/core/cc_result.h、cc/ports/cc_session_store.h、
 *        cc/util/cc_config.h。
 */

#ifndef CC_STORAGE_FACTORY_H
#define CC_STORAGE_FACTORY_H

#include "cc/core/cc_result.h"
#include "cc/ports/cc_session_store.h"
#include "cc/util/cc_config.h"

/**
 * cc_storage_factory_create_store — 根据配置创建会话存储实例
 *
 * 解析 config 中的 storage_type 字段，创建对应类型的存储后端。
 * 当前支持的存储类型：
 *   - "json"   : 基于 JSON 文件的存储（默认），每个会话保存为独立的 JSON 文件
 *   - "sqlite" : 基于 SQLite 数据库的存储（规划中）
 *
 * 返回的 cc_session_store_t 使用 vtable 多态，调用方通过
 * store.vtable->create_session() 等虚函数操作存储。
 *
 * @param config     系统配置（从中读取 storage_type 和 storage_path）
 * @param out_store  输出：创建好的会话存储实例（vtable 多态句柄）
 * @return           CC_OK 表示成功
 */
cc_result_t cc_storage_factory_create_store(
    const cc_config_t *config,
    cc_session_store_t *out_store
);

#endif