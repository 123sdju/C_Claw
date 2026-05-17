/**
 * 学习导读：cclaw/core/src/core/cc_session.c
 *
 * 所属层次：核心层。
 * 阅读重点：这里定义 Agent 运行时的数据模型、主循环和通用工具，阅读时重点看所有权、错误返回和 ReAct 数据流。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * cc_session.c — 会话结构实现模块
 *
 * 当前职责：
 *   本文件只负责 cc_session_t 的构造和析构，不保存消息列表，也不直接访问
 *   LLM、工具或具体存储后端。消息的追加、历史加载和持久化由
 *   cc_session_store_t 及其适配器完成。
 *
 * 阅读重点：
 *   - cc_session_create 会深拷贝 id/name/workspace_dir，并把状态初始化为 ACTIVE。
 *   - model、created_at、updated_at 当前保持 NULL，等待上层或存储后端按实际事件填充。
 *   - cc_session_destroy 只释放结构体字段和结构体本身，不删除工作区目录或数据库记录。
 *
 * 数据流位置：
 *   runtime/gateway 选择 session_id → session_store 创建或加载 session 元数据
 *   → context_builder 另行加载 messages → runtime 将 messages 发送给 LLM provider。
 */

#include "cc/core/cc_session.h"
#include <stdlib.h>
#include <string.h>

/*
 * cc_session_create - 创建新的会话对象
 *
 * 功能：
 *   分配 cc_session_t，并深拷贝 id、name、workspace_dir。这个函数只创建
 *   会话元数据对象，不创建消息数组，也不写入 session store。
 *
 * 参数：
 *   id            会话唯一字符串；当前实现允许 NULL，但常规调用应传入上层生成的 session_id。
 *   name          人类可读名称，可为 NULL。
 *   workspace_dir 会话工作区目录，可为 NULL；文件工具是否可用由调用方和工具层决定。
 *   out_session   输出新对象；成功后调用方负责 cc_session_destroy。
 *
 * 所有权：
 *   传入字符串仍归调用方所有，本函数内部 strdup 后由 session 自己持有。
 */
cc_result_t cc_session_create(
    const char *id,
    const char *name,
    const char *workspace_dir,
    cc_session_t **out_session
)
{
    cc_session_t *session = calloc(1, sizeof(cc_session_t));
    if (!session) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate session");

    session->id = id ? strdup(id) : NULL;
    session->name = name ? strdup(name) : NULL;
    session->workspace_dir = workspace_dir ? strdup(workspace_dir) : NULL;
    session->status = CC_SESSION_ACTIVE;

    *out_session = session;
    return cc_result_ok();
}

/*
 * cc_session_destroy - 销毁会话对象并释放所有关联资源
 *
 * 功能：
 *   释放 cc_session_t 持有的字符串字段和结构体本身。传入 NULL 安全无操作。
 *
 * 注意：
 *   本函数不删除工作区目录、不删除持久化存储中的记录，也不处理消息历史；
 *   这些职责分别属于平台文件系统、session store 或上层管理逻辑。
 */
void cc_session_destroy(cc_session_t *session)
{
    if (!session) return;
    free(session->id);
    free(session->name);
    free(session->workspace_dir);
    free(session->model);
    free(session->created_at);
    free(session->updated_at);
    free(session);
}