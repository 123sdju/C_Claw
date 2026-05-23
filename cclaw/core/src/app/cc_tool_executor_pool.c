/**
 * 学习导读：cclaw/core/src/app/cc_tool_executor_pool.c
 *
 * 所属层次：核心层。
 * 阅读重点：工具执行的并发门控与超时策略，重点看 lane 的动态创建和并发
 *          上限、acquire/release 的 ticket 机制、cancel token 在等待
 *          循环中的协作式检查以及 timeout_ms 查询。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * cc_tool_executor_pool.c — 工具执行并发池与超时策略模块
 *
 * 本模块在整体架构中的角色：
 * ─────────────────────────────
 * 位于 App 层（业务逻辑层），只管理"能不能开始执行"和"应该用什么 timeout"。
 * 不拥有 tool、不调用 tool、不关闭进程/HTTP——真正的执行由 cc_tool_executor
 * 或 plugin/MCP adapter 完成。这样把并发控制和 timeout 策略放在可移植 core
 * 中，把进程、HTTP、stdio 等平台能力留给 app/adapters。
 *
 * 上游调用方：
 *   - cc_tool_executor.c —— 在调用具体工具前先 acquire 获取 lane 令牌，
 *     并用 timeout_ms 填充 cc_tool_context_t
 *
 * 下游依赖模块：
 *   - cc_thread.c / cc_mutex / cc_cond —— 互斥锁和条件变量用于等待队列
 *   - cc_cancel_token.c —— acquire 等待循环中检查取消标记
 *
 * ─── 内部数据结构 ───────────────────────────────────────────────────
 *
 *   cc_tool_executor_lane_t：
 *     一条工具执行 lane。持有 name（lane 名称，pool 拥有）、concurrency
 *     （该 lane 的并发上限，经 normalized_concurrency 保证 >=1）、
 *     timeout_ms（该 lane 的调用超时，为 0 时使用 pool 默认值）和
 *     in_flight（当前飞行中的调用数）。
 *
 *   cc_tool_executor_pool（主结构体）：
 *     持有 default_concurrency（默认 4）、default_timeout_ms（默认 30000）、
 *     lanes 动态数组、lane_count/lane_capacity 以及 mutex/cond 同步原语。
 *
 * ─── Lane 的动态管理 ────────────────────────────────────────────────
 *
 *   lane 采用懒创建策略：首次 acquire 某个名称时，lane_index_locked 会
 *   查找已有 lane，没有则调用 add_lane_locked 动态创建。新 lane 使用
 *   全局默认的 concurrency 和 timeout_ms，除非在 policies 数组中预先
 *   配置了该 lane 的策略。
 *
 *   lane 数组容量以 2 倍扩容，初始为 8。lane 创建后不会被释放，即使
 *   in_flight 归零也不回收——这是简单且无悬挂指针的设计选择。
 *
 * ─── Acquire / Release 机制 ─────────────────────────────────────────
 *
 *   acquire_with_cancel 流程：
 *     1. 参数校验 + cancel token 快速失败检查
 *     2. 持锁查/lazy-create lane
 *     3. 若 in_flight >= concurrency，进入等待循环：
 *        a. 每次最多等 50ms（cc_cond_timedwait）
 *        b. 醒来后检查 cancel token，已取消则释放锁并返回 CANCELLED
 *        c. 重新读取 lane 指针（数组可能因 realloc 移动）
 *     4. in_flight++，设置 ticket.lane_index，返回 OK
 *
 *   release 流程：
 *     1. 持锁检查 lane_index 有效且 in_flight > 0
 *     2. in_flight--
 *     3. broadcast cond 唤醒所有等待者
 *
 *   为什么等待间隔是 50ms？
 *     50ms 足够短，cancel token 检查延迟可以接受；足够长，不会造成
 *     busy-wait 的 CPU 浪费。这是协作式取消的典型折衷。
 *
 * ─── 超时配置 ───────────────────────────────────────────────────────
 *
 *   timeout_ms 查询：
 *     cc_tool_executor_pool_timeout_ms 根据 lane name 查询对应 timeout。
 *     若 lane 不存在则返回 pool->default_timeout_ms（默认 30000ms）。
 *     这个值由 cc_tool_executor 写入 cc_tool_context_t，具体工具再决定
 *     如何落到 pipe read、HTTP request 或本地操作上。
 *
 *   默认配置：
 *     - default_concurrency = 4：每条 lane 默认最多 4 个并发工具调用
 *     - default_timeout_ms = 30000：工具调用默认 30 秒超时
 *
 * ─── 设计决策 ───────────────────────────────────────────────────────
 *
 *   为什么 pool 不调用或不拥有 tool？
 *     pool 的职责边界是"准入控制"和"超时策略"——它决定名字为 X 的 lane
 *     上最多同时有多少个调用、本次调用应该用多少毫秒的超时。至于调哪个
 *     工具、怎么调、结果如何处理，由 cc_tool_executor 和具体 adapter 负责。
 *     这种分离让 pool 保持极简且可被任何工具执行路径复用。
 *
 *   为什么 lane 创建后永不删除？
 *     lane 数量通常很小（工具类总数有限），回收 lane 需要处理 ticket
 *     悬挂问题（release 时 lane 可能已被删除），复杂性远大于收益。
 *     一个进程生命周期内 lane 内存占用可忽略。
 *
 *   为什么 release 用 broadcast 而非 signal？
 *     signal 只唤醒一个等待者，但该等待者可能属于已被取消的 job
 *     （cancel token 检查失败后返回），导致其他可运行的等待者错过通知。
 *     broadcast 唤醒所有人，让每个等待者自己判断是否可以继续。
 */

#include "cc/app/cc_tool_executor_pool.h"
#include "cc/ports/cc_thread.h"

#include <stdlib.h>
#include <string.h>

/*
 * Tool executor pool 管 lane，而不管工具怎么执行。它的职责是：
 *   - 为 tool.<name> / plugin.<id> / mcp.<server> 这类 lane 维护并发上限。
 *   - 提供本次调用应使用的 timeout_ms。
 *   - 在 acquire 等待时观察 cancel token，避免被取消的 run 卡在队列里。
 *
 * pool 不拥有 tool，不调用 tool，也不关闭进程/HTTP；这些都由具体 adapter 处理。
 */
typedef struct cc_tool_executor_lane {
    char *name;
    int concurrency;
    int timeout_ms;
    int in_flight;
} cc_tool_executor_lane_t;

struct cc_tool_executor_pool {
    int default_concurrency;
    int default_timeout_ms;
    cc_tool_executor_lane_t *lanes;
    size_t lane_count;
    size_t lane_capacity;
    cc_mutex_t mutex;
    cc_cond_t cond;
};

cc_tool_executor_pool_config_t cc_tool_executor_pool_default_config(void)
{
    cc_tool_executor_pool_config_t config;
    config.default_concurrency = 4;
    config.default_timeout_ms = 30000;
    config.policies = NULL;
    config.policy_count = 0;
    return config;
}

static int normalized_concurrency(int value)
{
    return value <= 0 ? 1 : value;
}

static int find_lane_locked(cc_tool_executor_pool_t *pool, const char *name)
{
    for (size_t i = 0; i < pool->lane_count; i++) {
        if (pool->lanes[i].name && strcmp(pool->lanes[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int ensure_capacity_locked(cc_tool_executor_pool_t *pool)
{
    if (pool->lane_count < pool->lane_capacity) return 1;
    size_t next_capacity = pool->lane_capacity ? pool->lane_capacity * 2 : 8;
    cc_tool_executor_lane_t *next = realloc(pool->lanes, next_capacity * sizeof(*next));
    if (!next) return 0;
    memset(next + pool->lane_capacity, 0, (next_capacity - pool->lane_capacity) * sizeof(*next));
    pool->lanes = next;
    pool->lane_capacity = next_capacity;
    return 1;
}

static int add_lane_locked(
    cc_tool_executor_pool_t *pool,
    const char *name,
    int concurrency,
    int timeout_ms
)
{
    if (!ensure_capacity_locked(pool)) return -1;
    cc_tool_executor_lane_t *lane = &pool->lanes[pool->lane_count];
    lane->name = strdup(name);
    if (!lane->name) return -1;
    lane->concurrency = normalized_concurrency(concurrency);
    lane->timeout_ms = timeout_ms > 0 ? timeout_ms : pool->default_timeout_ms;
    lane->in_flight = 0;
    pool->lane_count++;
    return (int)(pool->lane_count - 1);
}

static int lane_index_locked(cc_tool_executor_pool_t *pool, const char *name)
{
    int index = find_lane_locked(pool, name);
    if (index >= 0) return index;
    return add_lane_locked(pool, name, pool->default_concurrency, pool->default_timeout_ms);
}

cc_result_t cc_tool_executor_pool_create(
    const cc_tool_executor_pool_config_t *config,
    cc_tool_executor_pool_t **out_pool
)
{
    if (!out_pool) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null out_pool");
    cc_tool_executor_pool_config_t defaults = cc_tool_executor_pool_default_config();
    const cc_tool_executor_pool_config_t *effective = config ? config : &defaults;

    cc_tool_executor_pool_t *pool = calloc(1, sizeof(cc_tool_executor_pool_t));
    if (!pool) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create tool executor pool");
    pool->default_concurrency = normalized_concurrency(effective->default_concurrency);
    pool->default_timeout_ms = effective->default_timeout_ms > 0 ? effective->default_timeout_ms : 30000;

    cc_result_t rc = cc_mutex_create(&pool->mutex);
    if (rc.code != CC_OK) {
        free(pool);
        return rc;
    }
    rc = cc_cond_create(&pool->cond);
    if (rc.code != CC_OK) {
        cc_mutex_destroy(pool->mutex);
        free(pool);
        return rc;
    }

    cc_mutex_lock(pool->mutex);
    for (size_t i = 0; i < effective->policy_count; i++) {
        const cc_tool_executor_pool_policy_t *policy = &effective->policies[i];
        if (policy->name && add_lane_locked(pool, policy->name, policy->concurrency, policy->timeout_ms) < 0) {
            cc_mutex_unlock(pool->mutex);
            cc_tool_executor_pool_destroy(pool);
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to add tool pool policy");
        }
    }
    cc_mutex_unlock(pool->mutex);

    *out_pool = pool;
    return cc_result_ok();
}

void cc_tool_executor_pool_destroy(cc_tool_executor_pool_t *pool)
{
    if (!pool) return;
    for (size_t i = 0; i < pool->lane_count; i++) {
        free(pool->lanes[i].name);
    }
    free(pool->lanes);
    cc_cond_destroy(pool->cond);
    cc_mutex_destroy(pool->mutex);
    free(pool);
}

cc_result_t cc_tool_executor_pool_acquire(
    cc_tool_executor_pool_t *pool,
    const char *lane_name,
    cc_tool_executor_pool_ticket_t *out_ticket
)
{
    return cc_tool_executor_pool_acquire_with_cancel(pool, lane_name, NULL, out_ticket);
}

cc_result_t cc_tool_executor_pool_acquire_with_cancel(
    cc_tool_executor_pool_t *pool,
    const char *lane_name,
    cc_cancel_token_t *cancel_token,
    cc_tool_executor_pool_ticket_t *out_ticket
)
{
    if (!pool || !lane_name || !out_ticket) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid tool executor pool acquire");
    }
    if (cc_cancel_token_is_cancelled(cancel_token)) {
        return cc_result_error(CC_ERR_CANCELLED, "Tool executor pool acquire cancelled");
    }

    cc_mutex_lock(pool->mutex);
    int index = lane_index_locked(pool, lane_name);
    if (index < 0) {
        cc_mutex_unlock(pool->mutex);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create tool executor lane");
    }

    cc_tool_executor_lane_t *lane = &pool->lanes[index];
    while (lane->in_flight >= lane->concurrency) {
        cc_cond_timedwait(pool->cond, pool->mutex, 50);
        if (cc_cancel_token_is_cancelled(cancel_token)) {
            cc_mutex_unlock(pool->mutex);
            return cc_result_error(CC_ERR_CANCELLED, "Tool executor pool acquire cancelled");
        }
        lane = &pool->lanes[index];
    }
    lane->in_flight++;
    out_ticket->lane_index = (size_t)index;
    cc_mutex_unlock(pool->mutex);
    return cc_result_ok();
}

void cc_tool_executor_pool_release(
    cc_tool_executor_pool_t *pool,
    cc_tool_executor_pool_ticket_t ticket
)
{
    if (!pool) return;
    cc_mutex_lock(pool->mutex);
    if (ticket.lane_index < pool->lane_count && pool->lanes[ticket.lane_index].in_flight > 0) {
        pool->lanes[ticket.lane_index].in_flight--;
    }
    cc_cond_broadcast(pool->cond);
    cc_mutex_unlock(pool->mutex);
}

int cc_tool_executor_pool_timeout_ms(
    cc_tool_executor_pool_t *pool,
    const char *lane_name
)
{
    if (!pool || !lane_name) return 0;
    cc_mutex_lock(pool->mutex);
    int index = lane_index_locked(pool, lane_name);
    int timeout_ms = index >= 0 ? pool->lanes[index].timeout_ms : pool->default_timeout_ms;
    cc_mutex_unlock(pool->mutex);
    return timeout_ms;
}
