

#include "cc/ports/cc_event_bus.h"
#include "cc/ports/cc_thread.h"

#include <string.h>

/* 慢 handler 隔离测试状态：慢订阅者阻塞时，快订阅者仍应由其它 worker 执行。 */
typedef struct {
    cc_event_bus_t *bus;
    cc_mutex_t mutex;
    cc_cond_t cond;
    int slow_started;
    int release_slow;
    int fast_count;
} isolation_state_t;

/* 慢 handler：进入后等待测试释放，用来占住一个 async worker。 */
static void slow_handler(const char *event_type, const char *event_json, void *user_data)
{
    (void)event_type;
    (void)event_json;
    isolation_state_t *state = (isolation_state_t *)user_data;
    cc_mutex_lock(state->mutex);
    state->slow_started = 1;
    cc_cond_broadcast(state->cond);
    while (!state->release_slow) {
        cc_cond_wait(state->cond, state->mutex);
    }
    cc_mutex_unlock(state->mutex);
}

/* 快 handler：只递增计数，用来验证没有被慢 handler 阻塞。 */
static void fast_handler(const char *event_type, const char *event_json, void *user_data)
{
    (void)event_type;
    (void)event_json;
    isolation_state_t *state = (isolation_state_t *)user_data;
    cc_mutex_lock(state->mutex);
    state->fast_count++;
    cc_cond_broadcast(state->cond);
    cc_mutex_unlock(state->mutex);
}

/* 验证多 worker 异步 event bus 对不同 handler 的隔离能力。 */
static int test_slow_handler_does_not_block_fast_handler(void)
{
    isolation_state_t state = {0};
    if (cc_mutex_create(&state.mutex).code != CC_OK) return 0;
    if (cc_cond_create(&state.cond).code != CC_OK) return 0;

    cc_event_bus_config_t config = cc_event_bus_default_config();
    config.mode = CC_EVENT_BUS_MODE_ASYNC;
    config.worker_count = 2;
    config.max_pending = 8;
    if (cc_event_bus_create_with_config(&config, &state.bus).code != CC_OK) return 0;
    if (cc_event_bus_subscribe(state.bus, "slow", slow_handler, &state).code != CC_OK) return 0;
    if (cc_event_bus_subscribe(state.bus, "fast", fast_handler, &state).code != CC_OK) return 0;

    if (cc_event_bus_publish(state.bus, "slow", "{}").code != CC_OK) return 0;
    cc_mutex_lock(state.mutex);
    while (!state.slow_started) cc_cond_wait(state.cond, state.mutex);
    cc_mutex_unlock(state.mutex);

    if (cc_event_bus_publish(state.bus, "fast", "{}").code != CC_OK) return 0;
    cc_mutex_lock(state.mutex);
    while (state.fast_count == 0) cc_cond_wait(state.cond, state.mutex);
    int ok = state.slow_started && state.fast_count == 1;
    state.release_slow = 1;
    cc_cond_broadcast(state.cond);
    cc_mutex_unlock(state.mutex);

    ok = ok && cc_event_bus_flush(state.bus).code == CC_OK;
    cc_event_bus_destroy(state.bus);
    cc_cond_destroy(state.cond);
    cc_mutex_destroy(state.mutex);
    return ok;
}

/* FIFO 测试状态：expected 记录同一 handler 应看到的下一个序号。 */
typedef struct {
    cc_mutex_t mutex;
    int expected;
    int violation;
} fifo_state_t;

/* 同一 handler 的事件必须按 publish 顺序执行。 */
static void fifo_handler(const char *event_type, const char *event_json, void *user_data)
{
    (void)event_type;
    fifo_state_t *state = (fifo_state_t *)user_data;
    int value = event_json && event_json[0] ? event_json[0] - '0' : -1;
    cc_mutex_lock(state->mutex);
    if (value != state->expected) state->violation = 1;
    state->expected++;
    cc_mutex_unlock(state->mutex);
}

/* 验证同一订阅 handler 即使在多 worker 下也保持 FIFO。 */
static int test_same_handler_fifo(void)
{
    fifo_state_t state = {0};
    if (cc_mutex_create(&state.mutex).code != CC_OK) return 0;

    cc_event_bus_config_t config = cc_event_bus_default_config();
    config.mode = CC_EVENT_BUS_MODE_ASYNC;
    config.worker_count = 4;
    config.max_pending = 16;
    cc_event_bus_t *bus = NULL;
    if (cc_event_bus_create_with_config(&config, &bus).code != CC_OK) return 0;
    if (cc_event_bus_subscribe(bus, "seq", fifo_handler, &state).code != CC_OK) return 0;

    const char *values[] = {"0", "1", "2", "3", "4"};
    for (int i = 0; i < 5; i++) {
        if (cc_event_bus_publish(bus, "seq", values[i]).code != CC_OK) return 0;
    }
    int ok = cc_event_bus_flush(bus).code == CC_OK;
    cc_mutex_lock(state.mutex);
    ok = ok && state.expected == 5 && state.violation == 0;
    cc_mutex_unlock(state.mutex);
    cc_event_bus_destroy(bus);
    cc_mutex_destroy(state.mutex);
    return ok;
}

/* 背压测试状态：用阻塞 handler 填满 worker 和队列，再观察第三次 publish 是否阻塞。 */
typedef struct {
    cc_event_bus_t *bus;
    cc_mutex_t mutex;
    cc_cond_t cond;
    int started;
    int release;
    int publish_thread_started;
    int publish_thread_done;
} backpressure_state_t;

/* 阻塞 handler：占住唯一 worker，直到测试释放。 */
static void blocking_handler(const char *event_type, const char *event_json, void *user_data)
{
    (void)event_type;
    (void)event_json;
    backpressure_state_t *state = (backpressure_state_t *)user_data;
    cc_mutex_lock(state->mutex);
    state->started = 1;
    cc_cond_broadcast(state->cond);
    while (!state->release) cc_cond_wait(state->cond, state->mutex);
    cc_mutex_unlock(state->mutex);
}

/* 第三次 publish 运行在线程中，用来观察队列满时 publish 是否等待空间。 */
static void *third_publish_thread(void *arg)
{
    backpressure_state_t *state = (backpressure_state_t *)arg;
    cc_mutex_lock(state->mutex);
    state->publish_thread_started = 1;
    cc_cond_broadcast(state->cond);
    cc_mutex_unlock(state->mutex);

    cc_event_bus_publish(state->bus, "block", "3");

    cc_mutex_lock(state->mutex);
    state->publish_thread_done = 1;
    cc_cond_broadcast(state->cond);
    cc_mutex_unlock(state->mutex);
    return NULL;
}

/* 验证 max_pending 满时 publisher 会被背压阻塞，而不是丢事件或越界增长。 */
static int test_queue_full_blocks_publisher(void)
{
    backpressure_state_t state = {0};
    if (cc_mutex_create(&state.mutex).code != CC_OK) return 0;
    if (cc_cond_create(&state.cond).code != CC_OK) return 0;

    cc_event_bus_config_t config = cc_event_bus_default_config();
    config.mode = CC_EVENT_BUS_MODE_ASYNC;
    config.worker_count = 1;
    config.max_pending = 1;
    if (cc_event_bus_create_with_config(&config, &state.bus).code != CC_OK) return 0;
    if (cc_event_bus_subscribe(state.bus, "block", blocking_handler, &state).code != CC_OK) return 0;

    if (cc_event_bus_publish(state.bus, "block", "1").code != CC_OK) return 0;
    cc_mutex_lock(state.mutex);
    while (!state.started) cc_cond_wait(state.cond, state.mutex);
    cc_mutex_unlock(state.mutex);
    if (cc_event_bus_publish(state.bus, "block", "2").code != CC_OK) return 0;

    cc_thread_t thread;
    if (cc_thread_create(third_publish_thread, &state, &thread).code != CC_OK) return 0;
    cc_mutex_lock(state.mutex);
    while (!state.publish_thread_started) cc_cond_wait(state.cond, state.mutex);
    int woke = cc_cond_timedwait(state.cond, state.mutex, 50);
    int ok = (!woke || !state.publish_thread_done) && state.publish_thread_done == 0;
    state.release = 1;
    cc_cond_broadcast(state.cond);
    cc_mutex_unlock(state.mutex);

    cc_thread_join(thread);
    ok = ok && cc_event_bus_flush(state.bus).code == CC_OK;
    cc_event_bus_destroy(state.bus);
    cc_cond_destroy(state.cond);
    cc_mutex_destroy(state.mutex);
    return ok;
}

/* destroy drain 测试状态。 */
typedef struct {
    cc_mutex_t mutex;
    int count;
} count_state_t;

/* 简单计数 handler，用来验证 destroy 前 pending 事件被处理。 */
static void count_handler(const char *event_type, const char *event_json, void *user_data)
{
    (void)event_type;
    (void)event_json;
    count_state_t *state = (count_state_t *)user_data;
    cc_mutex_lock(state->mutex);
    state->count++;
    cc_mutex_unlock(state->mutex);
}

/* 验证 destroy 会 drain 已发布事件，避免异步事件静默丢失。 */
static int test_destroy_drains_pending_events(void)
{
    count_state_t state = {0};
    if (cc_mutex_create(&state.mutex).code != CC_OK) return 0;

    cc_event_bus_config_t config = cc_event_bus_default_config();
    config.mode = CC_EVENT_BUS_MODE_ASYNC;
    config.worker_count = 1;
    config.max_pending = 8;
    cc_event_bus_t *bus = NULL;
    if (cc_event_bus_create_with_config(&config, &bus).code != CC_OK) return 0;
    if (cc_event_bus_subscribe(bus, "count", count_handler, &state).code != CC_OK) return 0;
    if (cc_event_bus_publish(bus, "count", "{}").code != CC_OK) return 0;
    cc_event_bus_destroy(bus);

    cc_mutex_lock(state.mutex);
    int ok = state.count == 1;
    cc_mutex_unlock(state.mutex);
    cc_mutex_destroy(state.mutex);
    return ok;
}

/* 嵌套发布测试状态。 */
typedef struct {
    cc_event_bus_t *bus;
    cc_mutex_t mutex;
    int root_count;
    int nested_count;
} nested_state_t;

/* root handler 内再次发布 nested，验证异步 event bus 不会因重入 publish 死锁。 */
static void nested_handler(const char *event_type, const char *event_json, void *user_data)
{
    (void)event_json;
    nested_state_t *state = (nested_state_t *)user_data;
    cc_mutex_lock(state->mutex);
    if (strcmp(event_type, "root") == 0) {
        state->root_count++;
        cc_mutex_unlock(state->mutex);
        cc_event_bus_publish(state->bus, "nested", "{}");
        return;
    }
    if (strcmp(event_type, "nested") == 0) state->nested_count++;
    cc_mutex_unlock(state->mutex);
}

/* 验证 handler 内嵌套 publish 能被 flush 正确处理。 */
static int test_nested_publish_does_not_deadlock(void)
{
    nested_state_t state = {0};
    if (cc_mutex_create(&state.mutex).code != CC_OK) return 0;

    cc_event_bus_config_t config = cc_event_bus_default_config();
    config.mode = CC_EVENT_BUS_MODE_ASYNC;
    config.worker_count = 2;
    config.max_pending = 8;
    if (cc_event_bus_create_with_config(&config, &state.bus).code != CC_OK) return 0;
    if (cc_event_bus_subscribe(state.bus, NULL, nested_handler, &state).code != CC_OK) return 0;
    if (cc_event_bus_publish(state.bus, "root", "{}").code != CC_OK) return 0;

    int ok = cc_event_bus_flush(state.bus).code == CC_OK;
    cc_mutex_lock(state.mutex);
    ok = ok && state.root_count == 1 && state.nested_count == 1;
    cc_mutex_unlock(state.mutex);
    cc_event_bus_destroy(state.bus);
    cc_mutex_destroy(state.mutex);
    return ok;
}

/* 顺序运行异步 event bus 的全部并发契约测试。 */
int main(void)
{
    if (!test_slow_handler_does_not_block_fast_handler()) return 1;
    if (!test_same_handler_fifo()) return 1;
    if (!test_queue_full_blocks_publisher()) return 1;
    if (!test_destroy_drains_pending_events()) return 1;
    if (!test_nested_publish_does_not_deadlock()) return 1;
    return 0;
}
