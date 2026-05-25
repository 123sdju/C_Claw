/**
 * 学习导读：cclaw/core/src/app/cc_session_manager.c
 *
 * 所属层次：核心层。
 * 阅读重点：这里管理 session 创建和消息追加，重点看 session store 端口、
 *           session_id 所有权和历史消息写入顺序。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * ===========================================================================
 * cc_session_manager.c — 会话生命周期管理器
 * ===========================================================================
 *
 * 模块在整体架构中的角色：
 * ─────────────────────────────
 * 本模块是 storage 层的"会话门面"——它在 Agent 主循环启动之前执行会话级别的
 * 前置操作（确保会话存在、追加用户消息），为主循环提供干净的起点。
 * 它是 cc_session_manager.h 的唯一实现。
 *
 * 本模块的定位是"薄封装"：它并不实现任何新的存储逻辑，而是将 storage 的
 * 底层操作封装为更高级的"会话管理"语义，让调用方无需关心 storage 的具体实现。
 *
 * 上游调用方：
 *   - cc_agent_runtime.c 的 create_session 辅助接口 —— 对外暴露会话创建能力
 *   - 应用 gateway —— 通过 runtime 创建/选择会话，实际消息追加由 runtime 主循环完成
 *   - 测试代码 —— 注入内存 store，验证会话创建、消息追加和列表能力
 *
 * 下游依赖模块：
 *   - cc_session_store —— 会话和消息的持久化存储（虚接口）
 *   - cc_message —— 消息数据结构（cc_message_t）
 *
 * 与其他模块的职责边界：
 *   - cc_agent_runtime：负责消息的"处理"（调用 LLM、执行工具、存储结果）
 *   - cc_context_builder：负责消息的"加载与组装"（从 storage 加载并构建 messages JSON）
 *   - cc_session_manager（本模块）：负责消息的"入站前准备"（会话创建 + 消息追加）
 *   - cc_session_store：负责实际的序列化/反序列化/持久化
 *
 * 会话的生命周期：
 * ──────────────────
 *   1. 创建（create_session）—— 通常是幂等的，已存在则跳过
 *   2. 活跃（追加消息）—— append_user_message / append_message
 *   3. 查询（list_sessions）—— 列出所有会话供 UI 选择
 *   4. 归档/删除—— 不属于本门面职责，由 storage 层或上层管理接口处理
 */

#include "cc/app/cc_session_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/**
 * generate_id — 生成唯一会话/消息标识符
 *
 * 功能：
 *   为 session_manager 自己创建的会话或用户消息生成本地唯一标识符。
 *
 * 格式：
 *   ses_<Unix秒级时间戳>_<rand() 随机数>
 *   示例：ses_1715000000_42
 *
 * @return 堆上分配的 ID 字符串（调用方负责 free）
 *
 * 与 cc_agent_runtime.c 中 generate_id 的区别：
 *   - 本函数的 ID 前缀是 "ses_"（用于会话 ID）
 *   - agent_runtime 的 ID 前缀是 "msg_"（用于消息 ID）
 *   - 这种前缀约定使得从 ID 字符串本身就能区分是会话还是消息，
 *     在调试和日志分析时非常有用。
 *   - 本函数仍使用 rand()，因此适合作为当前轻量实现和测试辅助；
 *     真正跨进程/高并发场景应由调用方或存储后端提供更强的唯一 ID。
 *
 * 设计决策——为什么本地定义而不共用：
 *   - session_manager 和 agent_runtime 是两个独立的编译单元
 *   - 各自管理自己领域内的 ID 生成，避免跨模块头文件依赖
 *   - 如果未来需要不同的 ID 生成策略（如数据库序列或 UUID），两个模块可独立升级
 */
static char *generate_id(void)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "ses_%ld_%d", (long)time(NULL), rand());
    return strdup(buf);
}

/**
 * now_string — 生成当前时间的 ISO-8601 格式字符串
 *
 * 功能：
 *   获取当前系统时间并格式化为 ISO-8601 简化格式。
 *
 * 输出格式：
 *   YYYY-MM-DDTHH:MM:SS
 *   示例：2026-05-14T15:30:00
 *
 * @return 堆上分配的时间字符串（调用方负责 free）
 *
 * 注意：
 *   - 这是 ISO-8601 的简化版本，不包含时区信息和毫秒精度。
 *     对于会话消息的时间戳来说足够使用。
 *   - localtime() 不是线程安全的。如果多线程并发创建消息，
 *     应改用 localtime_r()（POSIX）或 gmtime_r()。
 */
static char *now_string(void)
{
    time_t t = time(NULL);
    char buf[64];
    struct tm *tm_info = localtime(&t);
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", tm_info);
    return strdup(buf);
}

/**
 * cc_session_manager_create — 创建会话管理器实例（工厂函数）
 *
 * 功能：
 *   分配并初始化一个会话管理器，绑定指定的 storage 后端。
 *   管理器本身非常轻量——只包含一个 storage 接口副本。
 *
 * @param store       会话持久化存储接口（vtable + self 指针的浅拷贝）
 * @param out_manager 输出参数，指向新创建的管理器指针
 *
 * @return CC_OK 成功；CC_ERR_OUT_OF_MEMORY 内存分配失败
 *
 * 设计意图：
 *   管理器只是 storage 的一个包装，不拥有 storage 的所有权。
 *   storage 的生命周期由 runtime 管理，manager 只是"借用"它。
 *   所以 manager 的析构函数不需要释放 storage。
 */
cc_result_t cc_session_manager_create(
    cc_session_store_t store,
    cc_session_manager_t **out_manager
)
{
    cc_session_manager_t *manager = calloc(1, sizeof(cc_session_manager_t));
    if (!manager) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create session manager");

    manager->store = store;
    *out_manager = manager;
    return cc_result_ok();
}

/**
 * cc_session_manager_destroy — 销毁会话管理器实例
 *
 * 功能：
 *   释放 manager 结构体内存。
 *
 * @param manager 待销毁的会话管理器实例
 *
 * 注意：
 *   不会销毁绑定的 storage。因为 storage 由 runtime 管理，
 *   manager 只是持有它的一个浅拷贝引用。
 *
 * 为什么只调用 free 而不做其他清理：
 *   cc_session_manager_t 只有一个 cc_session_store_t 字段，
 *   而 cc_session_store_t 是值类型（包含 void* self 和 vtable*），
 *   不持有堆上资源。所以 free(manager) 即完成所有清理。
 */
void cc_session_manager_destroy(cc_session_manager_t *manager)
{
    free(manager);
}

/**
 * cc_session_manager_ensure_session — 确保会话在存储中存在（幂等操作）
 *
 * 功能：
 *   如果会话 ID 在 storage 中不存在，则创建之；如果已存在则无操作。
 *   这是一个"幂等创建"（Ensure/Create-If-Not-Exists）模式。
 *
 * @param manager       会话管理器实例
 * @param session_id    待确保存在的会话 ID（调用方生成，如 "ses_1715000000_42"）
 * @param workspace_dir 工作空间目录路径（传递给 storage 用于创建会话时关联）
 *
 * @return CC_OK 成功；CC_ERR_INVALID_ARGUMENT 参数为 NULL
 *
 * 幂等性保证：
 *   底层 storage 的 create_session 实现应具备幂等性（已存在则不做任何操作）。
 *   这样调用方无需预先检查会话是否存在，简化了调用逻辑。
 *
 * 典型调用场景：
 *   // 在发送用户消息之前，先确保会话存在
 *   cc_session_manager_ensure_session(mgr, "ses_xxx", "/tmp/workspace");
 *   cc_session_manager_append_user_message(mgr, "ses_xxx", "你好");
 *   cc_agent_runtime_handle_message(runtime, "ses_xxx", "你好", &reply);
 *
 * 为什么不在 manager 内部生成 session_id：
 *   session_id 可以由调用方自定义（如基于用户 ID + 时间戳），
 *   也可以预先生成以供 UI 展示。将 ID 生成权交给调用方更加灵活。
 */
cc_result_t cc_session_manager_ensure_session(
    cc_session_manager_t *manager,
    const char *session_id,
    const char *workspace_dir
)
{
    if (!manager || !session_id) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null argument");

    if (manager->store.vtable && manager->store.vtable->create_session) {
        return manager->store.vtable->create_session(
            manager->store.self, session_id, workspace_dir);
    }
    return cc_result_ok();
}

/**
 * cc_session_manager_append_user_message — 将用户消息追加到会话中
 *
 * 功能：
 *   生成唯一 ID 和时间戳，构造一条 role="user" 的消息，
 *   通过 storage 接口将其持久化到指定会话中。
 *
 * @param manager    会话管理器实例
 * @param session_id 目标会话 ID
 * @param content    用户输入的原始文本（UTF-8 编码）
 *
 * @return CC_OK 成功；CC_ERR_INVALID_ARGUMENT 参数为 NULL
 *
 * 算法步骤：
 *   1. 参数有效性检查（NULL guard）
 *   2. 检查 storage 是否支持 append_message（不支持则静默返回成功）
 *   3. 生成本地消息 ID（ses_<ts>_<random>）和时间戳
 *   4. 调用 cc_message_create 构造 role="user" 的消息对象
 *   5. 设置 created_at 时间戳
 *   6. 调用 storage->append_message 持久化消息
 *   7. 释放临时 ID 和消息对象
 *
 * 设计意图：
 *   此函数应在 Agent 主循环启动前被调用。这样主循环中
 *   cc_context_builder 加载消息时能够看到用户最新输入。
 *
 * 为什么用 cc_message_create 而非直接调用 storage：
 *   cc_message_create 提供了统一的消息对象构造方式，
 *   确保消息对象的字段被正确初始化。storage 的 append_message
 *   只负责序列化/持久化，不负责构造消息对象。
 *
 * 为什么 storage 不支持 append_message 时静默返回成功：
 *   不是所有 storage 后端都需要持久化（如内存后端）。
 *   静默成功保持了接口的包容性。
 *
 * 调用时序约束：
 *   必须先调用 ensure_session 确保会话存在，再调用本函数追加消息。
 *   否则 storage 可能因为会话不存在而拒绝追加。
 */
cc_result_t cc_session_manager_append_user_message(
    cc_session_manager_t *manager,
    const char *session_id,
    const char *content
)
{
    if (!manager || !session_id || !content)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null argument");

    /* 如果 storage 不支持消息追加，静默返回成功 */
    if (!manager->store.vtable || !manager->store.vtable->append_message)
        return cc_result_ok();

    cc_message_t *msg = NULL;
    char *id = generate_id();
    char *now = now_string();

    /* 创建 CC_ROLE_USER 角色消息（用户消息无 tool_call_id） */
    cc_result_t rc = cc_message_create(id, session_id, CC_ROLE_USER, content, NULL, &msg);
    if (rc.code != CC_OK) {
        free(id);
        free(now);
        return rc;
    }

    /* 设置创建时间戳 */
    msg->created_at = now;
    free(id);

    /* 持久化消息到 storage */
    rc = manager->store.vtable->append_message(manager->store.self, msg);
    cc_message_destroy(msg);
    return rc;
}

/**
 * cc_session_manager_list_sessions — 列出所有会话
 *
 * 功能：
 *   从 storage 中获取所有已创建会话的列表。这是一个查询接口，
 *   为交互 gateway 或 Web UI 提供会话列表以供用户选择和切换。
 *
 * @param manager      会话管理器实例
 * @param out_sessions 输出参数，会话结构体数组指针（调用方负责释放每项）
 * @param out_count    输出参数，会话数量
 *
 * @return CC_OK 成功；storage 不支持时返回空列表 {NULL, 0}
 *
 * 典型使用场景：
 *
 *   // 交互 gateway：列出所有历史会话
 *   cc_session_t *sessions = NULL;
 *   size_t count = 0;
 *   cc_session_manager_list_sessions(mgr, &sessions, &count);
 *   for (size_t i = 0; i < count; i++) {
 *       printf("[%zu] session: %s\n", i, sessions[i].id);
 *   }
 *   // 用户选择某个会话后继续对话
 *   cc_agent_runtime_handle_message(runtime, sessions[choice].id, "继续", &reply);
 *
 * 为什么 storage 不支持时返回空列表而非错误：
 *   调用方通常不需要关心 storage 是否支持 list_sessions。
 *   返回空列表是一种优雅的降级行为。
 */
cc_result_t cc_session_manager_list_sessions(
    cc_session_manager_t *manager,
    cc_session_t **out_sessions,
    size_t *out_count
)
{
    if (!manager || !manager->store.vtable || !manager->store.vtable->list_sessions) {
        *out_sessions = NULL;
        *out_count = 0;
        return cc_result_ok();
    }
    return manager->store.vtable->list_sessions(manager->store.self, out_sessions, out_count);
}
