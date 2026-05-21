/**
 * 学习导读：cclaw/core/include/cc/core/cc_session.h
 *
 * 所属层次：核心层。
 * 阅读重点：这里定义 session 元数据，重点看 session_id、workspace 和时间字段
 *           如何被 session store 持久化。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * cc_session.h — 会话数据模型模块
 *
 * @file    cc/core/cc_session.h
 * @brief   定义 Agent 会话实体，表示一次完整的对话交互。
 *
 * 会话（Session）是比消息（Message）更高层级的聚合实体。
 * 一个会话包含多条消息、一个工作区目录、以及生命周期状态。
 * 会话由 cc_session_store_t 负责持久化。
 *
 * ─── 接口契约 ─────────────────────────────────────────────────────────
 *
 * 本模块是纯数据结构定义 + 构造/析构函数。不包含会话管理逻辑
 * （如消息追加、历史加载等），这些由 cc_session_store_t 提供。
 *
 * ─── 生命周期 ─────────────────────────────────────────────────────────
 *
 *   ACTIVE → (用户正常结束) → COMPLETED
 *   ACTIVE → (异常中断)     → ERROR
 *
 * ─── 依赖 ─────────────────────────────────────────────────────────────
 *
 *   依赖 cc_result.h（错误传递）和 cc_message.h（消息类型前向声明）。
 */

#ifndef CC_SESSION_H
#define CC_SESSION_H

#include "cc_result.h"
#include "cc_message.h"

/**
 * cc_session_status_t — 会话状态枚举
 *
 * 跟踪会话的当前生命周期状态，用于存储列表的过滤和 UI 显示。
 */
typedef enum cc_session_status {
    CC_SESSION_ACTIVE,    /**< 进行中：会话正在交互，可继续追加消息 */
    CC_SESSION_COMPLETED, /**< 已完成：用户正常结束对话 */
    CC_SESSION_ERROR      /**< 异常终止：对话因错误中断（LLM 超时、工具崩溃等） */
} cc_session_status_t;

/**
 * cc_session_t — 会话结构体
 *
 * 聚合一条完整对话交互的所有元数据。会话是最小的对话管理单元，
 * 用户可创建多个会话并在它们之间切换。
 */
typedef struct cc_session {
    char *id;                   /**< 会话唯一标识符（调用方生成的唯一字符串） */
    char *name;                 /**< 会话名称，通常使用首条消息摘要或用户自定义 */
    char *workspace_dir;        /**< 会话专用工作区目录路径，所有文件工具操作
                                 *   限定在此目录下 */
    char *model;                /**< 使用的 LLM 模型名称（如 qwen2.5-coder:7b） */
    cc_session_status_t status; /**< 会话当前状态（active/completed/error） */
    char *created_at;           /**< 会话创建时间戳（ISO 8601 格式） */
    char *updated_at;           /**< 会话最后更新时间戳（ISO 8601 格式） */
} cc_session_t;

/**
 * cc_session_create — 创建新会话
 *
 * 在堆上分配并初始化 cc_session_t。创建时默认状态为 CC_SESSION_ACTIVE，
 * created_at 和 updated_at 设为当前时间。
 *
 * @param id            会话唯一标识符（不可为 NULL，调用方生成唯一字符串）
 * @param name          会话名称（可为 NULL）
 * @param workspace_dir 工作区目录路径（不可为 NULL）
 * @param out_session   输出：指向新创建会话的指针（调用者负责 cc_session_destroy）
 * @return              CC_OK 表示成功，否则包含错误码
 */
cc_result_t cc_session_create(
    const char *id,
    const char *name,
    const char *workspace_dir,
    cc_session_t **out_session
);

/**
 * cc_session_destroy — 销毁会话并释放所有关联内存
 *
 * 释放 session 结构体及其所有字符串字段的内存。
 * 注意：不会自动删除工作区中的文件。
 * 传入 NULL 是安全的（无操作）。
 *
 * @param session  要销毁的会话指针
 */
void cc_session_destroy(cc_session_t *session);

#endif
