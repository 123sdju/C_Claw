/**
 * 学习导读：cclaw/core/src/app/cc_cancel_token.c
 *
 * 所属层次：核心层。
 * 阅读重点：source/token 的所有权分离模型——source 拥有可变状态（cancelled
 *          标志 + mutex），token 只是指回 source 的借用句柄。理解 create 时
 *          同时分配两者的生命周期管理，以及 cancel() 的幂等语义和 is_cancelled()
 *          的 NULL-safe 设计。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * cc_cancel_token.c — core SDK 统一协作式取消令牌
 *
 * 本模块在整体架构中的角色：
 * ─────────────────────────────
 * 位于 Core 层（与平台无关），是 SDK 中所有取消场景的统一原语。它把"用户
 * interrupt、队列替换、timeout、shutdown"统一成一个可查询的状态，通过
 * source/token 所有权分离实现安全的跨组件借出。SDK 只提供协作式取消——
 * 不强制杀线程，也不直接关闭 POSIX/Windows 进程。
 *
 * 上游调用方：
 *   - agent 运行时（cc_agent_runtime）—— 在用户中断、超时或 shutdown 时
 *     调用 cc_cancel_source_cancel() 设置取消标志
 *   - platform app 层——创建 cancel source，将 token 借给 tool runner、
 *     plugin worker、MCP transport 等组件
 *
 * 下游使用者（token 持有方）：
 *   - 工具执行、插件 worker、MCP transport——在安全检查点周期性调用
 *     cc_cancel_token_is_cancelled()，若返回真则自行释放平台资源并退出
 *
 * 下游依赖模块：
 *   - cc_thread.c（cc_mutex_t）—— 通过 mutex 提供跨平台的内存可见性保证，
 *     使 POSIX/Windows/ESP32 上都有一致的取消查询行为
 *
 * ─── 所有权模型 ────────────────────────────────────────────────────────
 *
 *   本模块使用 source/token 分离，很像 thread/std::thread::get_id 的模式：
 *
 *     cc_cancel_source_t  —— 拥有 mutable state（cancelled 标志）和 mutex，
 *                             同时拥有 token 对象。source 是唯一可 cancel 的
 *                             入口，必须在所有借用方完成后 destroy。
 *
 *     cc_cancel_token_t   —— 借用的只读句柄，内部只有一个指向 source 的指针。
 *                             不拥有任何资源，不能被独立 free。调用方通过
 *                             cc_cancel_source_token() 获取，传 token 而非
 *                             source 给各个消费方，防止误调用 cancel()。
 *
 *   create 时同时分配 source 和 token，token 生命周期严格跟随 source。
 *   调用方不能单独释放 token，也不能在 source destroy 后继续查询 token。
 *
 * ─── 关键设计决策 ──────────────────────────────────────────────────────
 *
 *   - 为什么用 mutex 而不是裸 int？
 *     POSIX/Windows/ESP32 的内存模型不同，裸 int 的 store/load 在不同
 *     平台上未必提供一致的可见性。mutex 保证所有平台上的 happens-before
 *     语义一致，且锁粒度极细（仅包裹 store/load），性能开销可忽略。
 *
 *   - 为什么 is_cancelled(NULL) 返回 0（未取消）？
 *     方便裁剪 profile：不需要取消功能的调用方直接传 NULL token，所有
 *     安全检查点无需条件判断即可工作。NULL token = 永不被取消。
 *
 *   - cancel() 是幂等的：重复调用保持 cancelled=1，不产生副作用。这使
 *     多个信号源（timeout + user interrupt + shutdown）可以安全地同时触发。
 */

#include "cc/app/cc_cancel_token.h"
#include "cc/ports/cc_thread.h"

#include <stdlib.h>

/*
 * Cancel token 是 core SDK 的最小取消原语。source 拥有可变状态和 mutex；
 * token 只是指回 source 的借用句柄，方便传给 tool、transport 或 run task。
 * 这样 timeout、interrupt、shutdown 都能使用同一套协作式语义，而不把线程
 * 强杀或进程终止策略硬塞进 core。
 */
struct cc_cancel_source {
    cc_mutex_t mutex;
    int cancelled;
    cc_cancel_token_t *token;
};

struct cc_cancel_token {
    cc_cancel_source_t *source;
};

/*
 * 创建时同时分配 source 和 token。token 生命周期严格跟随 source；调用方不能
 * 单独释放 token，也不能在 source destroy 后继续查询 token。
 */
cc_result_t cc_cancel_source_create(cc_cancel_source_t **out_source)
{
    if (!out_source) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null cancel source output");
    }
    cc_cancel_source_t *source = calloc(1, sizeof(cc_cancel_source_t));
    if (!source) {
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate cancel source");
    }
    source->token = calloc(1, sizeof(cc_cancel_token_t));
    if (!source->token) {
        free(source);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate cancel token");
    }
    cc_result_t rc = cc_mutex_create(&source->mutex);
    if (rc.code != CC_OK) {
        free(source->token);
        free(source);
        return rc;
    }
    source->token->source = source;
    *out_source = source;
    return cc_result_ok();
}

void cc_cancel_source_destroy(cc_cancel_source_t *source)
{
    if (!source) return;
    cc_mutex_destroy(source->mutex);
    free(source->token);
    free(source);
}

void cc_cancel_source_cancel(cc_cancel_source_t *source)
{
    if (!source) return;
    /*
     * 取消是幂等写入。用 mutex 而不是裸 int，是为了让 POSIX/Windows/ESP32
     * 都有一致的内存可见性；等待方只需要周期性调用 is_cancelled。
     */
    cc_mutex_lock(source->mutex);
    source->cancelled = 1;
    cc_mutex_unlock(source->mutex);
}

cc_cancel_token_t *cc_cancel_source_token(cc_cancel_source_t *source)
{
    return source ? source->token : NULL;
}

int cc_cancel_token_is_cancelled(cc_cancel_token_t *token)
{
    if (!token || !token->source) return 0;
    cc_cancel_source_t *source = token->source;
    cc_mutex_lock(source->mutex);
    int cancelled = source->cancelled;
    cc_mutex_unlock(source->mutex);
    return cancelled;
}
