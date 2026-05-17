/**
 * 学习导读：cclaw/ports/include/cc/ports/cc_session_store.h
 *
 * 所属层次：端口层。
 * 阅读重点：这里定义可替换接口，阅读时重点看 struct + vtable + void *self 如何表达多态和依赖注入。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * cc_session_store.h — 会话存储抽象端口（Port）
 *
 * @file    cc/ports/cc_session_store.h
 * @brief   定义会话持久化存储的抽象接口。采用 vtable 多态模式。
 *
 * 会话存储是 Agent 对话历史、工具调用记录等数据的持久化层。
 * 通过 cc_storage_factory_create_store() 创建具体实例，
 * 不同的存储后端（JSON 文件、SQLite 等）实现相同的 vtable 接口。
 *
 * ─── 接口契约 ─────────────────────────────────────────────────────────
 *
 *   - 每个 cc_session_store_t 由 self（私有数据）+ vtable（虚函数表）组成
 *   - 所有方法均返回 cc_result_t，失败时包含错误描述
 *   - load_messages 分配的消息数组由调用方负责逐一 cc_message_destroy 并 free
 *   - list_sessions 分配的会话数组由调用方负责逐一 cc_session_destroy 并 free
 *   - 调用方负责在不再使用时调用 vtable->destroy()
 *
 * ─── 数据流 ───────────────────────────────────────────────────────────
 *
 *   消息→ append_message() → 存储后端 → load_messages() → LLM 上下文构建
 *   工具调用 → append_tool_call() / append_tool_result() → 存储后端
 *
 * ─── 依赖 ─────────────────────────────────────────────────────────────
 *
 *   依赖 cc/core/cc_result.h、cc/core/cc_message.h、
 *        cc/core/cc_tool_call.h、cc/core/cc_session.h。
 */

#ifndef CC_SESSION_STORE_H
#define CC_SESSION_STORE_H

#include "cc/core/cc_result.h"
#include "cc/core/cc_message.h"
#include "cc/core/cc_tool_call.h"
#include "cc/core/cc_session.h"

/* ── 前向声明 ───────────────────────────────────────────────────────── */

typedef struct cc_session_store_vtable cc_session_store_vtable_t;
typedef struct cc_session_store cc_session_store_t;

/**
 * cc_session_store_t — 会话存储实例（多态句柄）
 *
 * 值语义结构体，通过 self + vtable 实现多态存储。
 * 具体是 JSON 文件存储还是 SQLite 存储，由 vtable 指向的
 * 函数实现决定。
 */
struct cc_session_store {
    void *self;                            /**< 指向具体存储实现的私有数据 */
    const cc_session_store_vtable_t *vtable; /**< 虚函数表 */
};

/**
 * cc_session_store_vtable_t — 会话存储虚函数表
 *
 * 定义会话持久化的完整接口。每个方法映射到具体的存储实现
 * （JSON 文件 / SQLite / 等）中的对应操作。
 */
struct cc_session_store_vtable {
    /**
     * create_session — 创建新会话记录
     *
     * 在存储后端中创建一个新会话。实现应初始化会话的元数据
     * （ID、工作区、时间戳等），并准备接收后续消息。
     *
     * @param self           存储私有数据
     * @param session_id     会话唯一标识符
     * @param workspace_dir  会话工作区目录
     * @return               CC_OK 表示成功
     */
    cc_result_t (*create_session)(
        void *self,
        const char *session_id,
        const char *workspace_dir
    );

    /**
     * append_message — 追加一条消息到会话
     *
     * 将 message 追加到对应会话的消息历史末尾。
     * 消息按追加顺序保留，供 load_messages 按时间序列返回。
     *
     * @param self      存储私有数据
     * @param message   要追加的消息（不可为 NULL）
     * @return          CC_OK 表示成功
     */
    cc_result_t (*append_message)(
        void *self,
        const cc_message_t *message
    );

    /**
     * load_messages — 加载会话的消息历史
     *
     * 按时间顺序（从早到晚）返回会话中的消息列表。
     * limit 参数控制返回条数，0 表示不限制。
     *
     * @param self          存储私有数据
     * @param session_id    会话 ID
     * @param limit         最大返回条数（0 = 全部）
     * @param out_messages  输出：消息数组（调用者负责逐一 cc_message_destroy + free）
     * @param out_count     输出：消息数量
     * @return              CC_OK 表示成功
     */
    cc_result_t (*load_messages)(
        void *self,
        const char *session_id,
        int limit,
        cc_message_t **out_messages,
        size_t *out_count
    );

    /**
     * append_tool_call — 记录工具调用请求
     *
     * 将 LLM 发起的工具调用请求记录到会话中。
     * 用于审计和构建完整的对话上下文。
     *
     * @param self        存储私有数据
     * @param session_id  会话 ID
     * @param call        工具调用信息
     * @return            CC_OK 表示成功
     */
    cc_result_t (*append_tool_call)(
        void *self,
        const char *session_id,
        const cc_tool_call_t *call
    );

    /**
     * append_tool_result — 记录工具调用结果
     *
     * 将工具执行结果关联到对应的工具调用请求。
     * tool_call_id 用于与 append_tool_call 记录关联。
     *
     * @param self          存储私有数据
     * @param session_id    会话 ID
     * @param tool_call_id  关联的工具调用 ID
     * @param result        工具执行结果
     * @return              CC_OK 表示成功
     */
    cc_result_t (*append_tool_result)(
        void *self,
        const char *session_id,
        const char *tool_call_id,
        const cc_tool_result_t *result
    );

    /**
     * list_sessions — 列出所有会话
     *
     * 返回存储中所有会话的列表，按更新时间倒序排列。
     *
     * @param self          存储私有数据
     * @param out_sessions  输出：会话数组（调用者负责逐一 cc_session_destroy + free）
     * @param out_count     输出：会话数量
     * @return              CC_OK 表示成功
     */
    cc_result_t (*list_sessions)(
        void *self,
        cc_session_t **out_sessions,
        size_t *out_count
    );

    /**
     * destroy — 销毁存储实例
     *
     * 释放文件句柄、关闭数据库连接、刷新缓冲区等。
     *
     * @param self  存储私有数据
     */
    void (*destroy)(void *self);
};

#endif