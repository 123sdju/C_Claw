



#ifndef CC_SESSION_STORE_H
#define CC_SESSION_STORE_H

#include "cc/core/cc_result.h"
#include "cc/core/cc_message.h"
#include "cc/core/cc_tool_call.h"
#include "cc/core/cc_session.h"


/* session store vtable 前置声明。 */
typedef struct cc_session_store_vtable cc_session_store_vtable_t;

/* session store port 前置声明。 */
typedef struct cc_session_store cc_session_store_t;

/*
 * session store 接口对象。
 *
 * self 指向 JSON 文件、内存、SQLite 等具体实现，vtable 提供会话和消息持久化操作。
 * 核心 runtime 不直接依赖文件/数据库，便于 MCU profile 换成轻量存储。
 */
struct cc_session_store {
    void *self;
    const cc_session_store_vtable_t *vtable;
};


/*
 * session store vtable。
 *
 * 所有输入对象都是借用指针；返回的 session/message 数组由调用方负责逐项 cleanup 后
 * free。append_* 应深拷贝需要持久化的数据，不能保存调用方临时指针。
 */
struct cc_session_store_vtable {


    /* 创建或打开一个 session 记录。 */
    cc_result_t (*create_session)(
        void *self,
        const char *session_id,
        const char *workspace_dir
    );



    /* 追加一条 message；store 应保存 message 的深拷贝或序列化形式。 */
    cc_result_t (*append_message)(
        void *self,
        const cc_message_t *message
    );



    /* 加载最近 limit 条消息；out_messages 数组由调用方释放。 */
    cc_result_t (*load_messages)(
        void *self,
        const char *session_id,
        int limit,
        cc_message_t **out_messages,
        size_t *out_count
    );



    /* 记录一次 assistant tool call，用于审计和会话恢复。 */
    cc_result_t (*append_tool_call)(
        void *self,
        const char *session_id,
        const cc_tool_call_t *call
    );



    /* 记录工具执行结果；tool_call_id 用于关联原始调用。 */
    cc_result_t (*append_tool_result)(
        void *self,
        const char *session_id,
        const char *tool_call_id,
        const cc_tool_result_t *result
    );



    /* 列出已有 session；out_sessions 数组由调用方销毁。 */
    cc_result_t (*list_sessions)(
        void *self,
        cc_session_t **out_sessions,
        size_t *out_count
    );



    /* 清空某个 session 的持久化数据。 */
    cc_result_t (*clear_session)(
        void *self,
        const char *session_id
    );



    /* 销毁 store self。 */
    void (*destroy)(void *self);
};

#endif
