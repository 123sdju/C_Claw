#include "cc/app/cc_tool_executor_pool.h"
#include "cc/ports/cc_thread.h"

#include <stdlib.h>
#include <string.h>

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
