/**
 * test_tool_executor_pool_lane.c
 *
 * 测试目标：同一个 tool/plugin/MCP lane 的执行数量受配置限制。真实工具调用
 * 由 adapter 完成，本测试只固定 core 层“开始执行前必须 acquire”的并发语义。
 */

#include "cc/app/cc_tool_executor_pool.h"
#include "cc/app/cc_cancel_token.h"
#include "cc/ports/cc_thread.h"

typedef struct pool_state {
    cc_tool_executor_pool_t *pool;
    cc_mutex_t mutex;
    int active;
    int violation;
} pool_state_t;

static void *pool_worker(void *arg)
{
    pool_state_t *state = (pool_state_t *)arg;
    cc_tool_executor_pool_ticket_t ticket;
    cc_result_t rc = cc_tool_executor_pool_acquire(state->pool, "plugin.echo", &ticket);
    if (rc.code != CC_OK) {
        cc_result_free(&rc);
        return NULL;
    }

    cc_mutex_lock(state->mutex);
    state->active++;
    if (state->active > 1) state->violation = 1;
    cc_mutex_unlock(state->mutex);

    for (volatile int i = 0; i < 5000000; i++) {
    }

    cc_mutex_lock(state->mutex);
    state->active--;
    cc_mutex_unlock(state->mutex);
    cc_tool_executor_pool_release(state->pool, ticket);
    return NULL;
}

int main(void)
{
    cc_tool_executor_pool_policy_t policy;
    policy.name = "plugin.echo";
    policy.concurrency = 1;
    policy.timeout_ms = 12345;

    cc_tool_executor_pool_config_t config = cc_tool_executor_pool_default_config();
    config.default_concurrency = 4;
    config.policies = &policy;
    config.policy_count = 1;

    pool_state_t state;
    state.pool = NULL;
    state.mutex = NULL;
    state.active = 0;
    state.violation = 0;

    if (cc_mutex_create(&state.mutex).code != CC_OK) return 1;
    if (cc_tool_executor_pool_create(&config, &state.pool).code != CC_OK) return 1;
    if (cc_tool_executor_pool_timeout_ms(state.pool, "plugin.echo") != 12345) return 1;

    cc_thread_t a;
    cc_thread_t b;
    if (cc_thread_create(pool_worker, &state, &a).code != CC_OK) return 1;
    if (cc_thread_create(pool_worker, &state, &b).code != CC_OK) return 1;
    if (cc_thread_join(a).code != CC_OK) return 1;
    if (cc_thread_join(b).code != CC_OK) return 1;

    cc_tool_executor_pool_ticket_t held;
    if (cc_tool_executor_pool_acquire(state.pool, "plugin.echo", &held).code != CC_OK) return 1;
    cc_cancel_source_t *source = NULL;
    if (cc_cancel_source_create(&source).code != CC_OK) return 1;
    cc_cancel_source_cancel(source);
    cc_tool_executor_pool_ticket_t cancelled_ticket;
    cc_result_t cancelled_rc = cc_tool_executor_pool_acquire_with_cancel(
        state.pool,
        "plugin.echo",
        cc_cancel_source_token(source),
        &cancelled_ticket);
    int cancel_ok = cancelled_rc.code == CC_ERR_CANCELLED;
    cc_result_free(&cancelled_rc);
    cc_cancel_source_destroy(source);
    cc_tool_executor_pool_release(state.pool, held);

    int ok = state.violation == 0 && cancel_ok;
    cc_tool_executor_pool_destroy(state.pool);
    cc_mutex_destroy(state.mutex);
    return ok ? 0 : 1;
}
