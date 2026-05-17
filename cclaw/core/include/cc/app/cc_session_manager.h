/**
 * 学习导读：cclaw/core/include/cc/app/cc_session_manager.h
 *
 * 所属层次：核心层。
 * 阅读重点：这里定义 Agent 运行时的数据模型、主循环和通用工具，阅读时重点看所有权、错误返回和 ReAct 数据流。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * cc_session_manager.h — 会话生命周期管理层
 *
 * @file    cc/app/cc_session_manager.h
 * @brief   管理会话的创建、消息追加、列表查询等生命周期操作。
 *
 * 本模块是会话持久化存储（cc_session_store_t）之上的业务逻辑层。
 * 它不直接实现存储（那是 cc_session_store_t 的职责），而是在
 * 存储层之上提供以下能力：
 *   - 懒创建：首次访问时自动创建会话（ensure 语义）
 *   - 消息追加：将用户输入格式化为 cc_message_t 并追加到存储
 *   - 会话列表：列出所有会话供 UI 展示
 *
 * ─── 接口契约 ─────────────────────────────────────────────────────────
 *
 *   - cc_session_manager_create() 创建管理器实例，注入底层 store
 *   - cc_session_manager_destroy() 销毁管理器（但 store 的所有权归调用方）
 *   - cc_session_manager_ensure_session() 确保会话存在（幂等操作）
 *   - cc_session_manager_append_user_message() 追加用户消息
 *   - cc_session_manager_list_sessions() 列出全部会话
 *
 * ─── 与 cc_session_store_t 的关系 ────────────────────────────────────
 *
 *   cc_session_manager_t 是上层"管理逻辑"层
 *          │
 *          │ 委托（delegate）
 *          ▼
 *   cc_session_store_t    是底层"持久化存储"抽象
 *
 *   Manager 本身不负责具体的读写、序列化，它只是封装了常见的
 *   组合操作（如"不存在则创建后再追加消息"），简化 Agent Runtime
 *   的调用代码。
 *
 * ─── 依赖 ─────────────────────────────────────────────────────────────
 *
 *   依赖 cc/core/cc_result.h（错误传递）和
 *        cc/ports/cc_session_store.h（会话持久化端口）。
 */

#ifndef CC_SESSION_MANAGER_H
#define CC_SESSION_MANAGER_H

#include "cc/core/cc_result.h"
#include "cc/ports/cc_session_store.h"

/**
 * cc_session_manager_t — 会话管理器结构体
 *
 * 持有一个会话存储实例（值拷贝），所有操作最终委托给该 store。
 * 管理器本身不管理 store 的生命周期（store 在创建时拷贝，
 * 但 store.self 指向的对象仍由 store 自身管理）。
 */
typedef struct cc_session_manager {
    cc_session_store_t store; /**< 底层会话持久化存储实例（值拷贝） */
} cc_session_manager_t;

/**
 * cc_session_manager_create — 创建会话管理器实例
 *
 * 分配 cc_session_manager_t 并注入给定的 store（值拷贝）。
 *
 * @param store        底层会话存储实例（值拷贝）
 * @param out_manager  输出：指向新创建管理器的指针（调用者负责 destroy）
 * @return             CC_OK 表示成功，CC_ERR_OUT_OF_MEMORY 表示分配失败
 */
cc_result_t cc_session_manager_create(
    cc_session_store_t store,
    cc_session_manager_t **out_manager
);

/**
 * cc_session_manager_destroy — 销毁会话管理器
 *
 * 释放管理器实例及其持有的资源。传入 NULL 是安全的。
 * 注意：不销毁内部 store.self 指向的存储实现（其生命周期由外部管理）。
 *
 * @param manager  待销毁的管理器实例（可为 NULL）
 */
void cc_session_manager_destroy(cc_session_manager_t *manager);

/**
 * cc_session_manager_ensure_session — 确保会话存在（幂等操作）
 *
 * 如果 session_id 对应的会话已存在，则无操作（幂等）。
 * 如果不存在，则在 store 中创建新的会话记录并关联 workspace_dir。
 *
 * @param manager       会话管理器实例（不可为 NULL）
 * @param session_id    会话唯一标识符（不可为 NULL）
 * @param workspace_dir 会话工作区目录路径（不可为 NULL）
 * @return              CC_OK 表示会话已就绪
 */
cc_result_t cc_session_manager_ensure_session(
    cc_session_manager_t *manager,
    const char *session_id,
    const char *workspace_dir
);

/**
 * cc_session_manager_append_user_message — 向会话追加用户消息
 *
 * 创建一条 CC_ROLE_USER 类型的消息记录，并将其追加到指定会话的
 * 消息历史末尾。内部自动生成消息 ID 和时间戳。
 *
 * @param manager     会话管理器实例（不可为 NULL）
 * @param session_id  目标会话 ID（不可为 NULL）
 * @param content     用户输入的文本内容（不可为 NULL）
 * @return            CC_OK 表示追加成功
 */
cc_result_t cc_session_manager_append_user_message(
    cc_session_manager_t *manager,
    const char *session_id,
    const char *content
);

/**
 * cc_session_manager_list_sessions — 列出所有会话
 *
 * 返回存储中所有会话的列表，按更新时间倒序排列。
 * 调用者负责逐一 cc_session_destroy 并 free 数组。
 *
 * @param manager       会话管理器实例（不可为 NULL）
 * @param out_sessions  输出：会话数组（调用者负责逐一 cc_session_destroy + free）
 * @param out_count     输出：会话总数
 * @return              CC_OK 表示成功
 */
cc_result_t cc_session_manager_list_sessions(
    cc_session_manager_t *manager,
    cc_session_t **out_sessions,
    size_t *out_count
);

#endif