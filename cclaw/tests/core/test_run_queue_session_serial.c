/**
 * test_run_queue_session_serial.c
 *
 * 测试目标：同一个 session key 的 run 默认串行。这个约束保护 session store
 * 和上下文构建，避免两次 turn 同时写入同一段历史。
 */

#include "cc/app/cc_run_queue.h"
#include "cc/ports/cc_thread.h"

typedef struct serial_state {
    cc_run_queue_t *queue;
    cc_mutex_t mutex;
    int active;
    int violation;
} serial_state_t;

static cc_result_t queued_task(void *user_data)
{
    serial_state_t *state = (serial_state_t *)user_data;
    cc_mutex_lock(state->mutex);
    state->active++;
    if (state->active > 1) state->violation = 1;
    cc_mutex_unlock(state->mutex);

    for (volatile int i = 0; i < 5000000; i++) {
    }

    cc_mutex_lock(state->mutex);
    state->active--;
    cc_mutex_unlock(state->mutex);
    return cc_result_ok();
}

static void *worker(void *arg)
{
    serial_state_t *state = (serial_state_t *)arg;
    cc_run_queue_request_t request;
    request.session_key = "same-session";
    request.lane = CC_RUN_QUEUE_LANE_MAIN;
    request.action = CC_RUN_QUEUE_ACTION_STEER;
    cc_result_t rc = cc_run_queue_run(state->queue, &request, queued_task, state);
    cc_result_free(&rc);
    return NULL;
}

int main(void)
{
    cc_run_queue_config_t config = cc_run_queue_default_config();
    config.main_concurrency = 2;
    config.per_session_concurrency = 1;

    serial_state_t state;
    state.queue = NULL;
    state.mutex = NULL;
    state.active = 0;
    state.violation = 0;

    if (cc_mutex_create(&state.mutex).code != CC_OK) return 1;
    if (cc_run_queue_create(&config, &state.queue).code != CC_OK) return 1;

    cc_thread_t a;
    cc_thread_t b;
    if (cc_thread_create(worker, &state, &a).code != CC_OK) return 1;
    if (cc_thread_create(worker, &state, &b).code != CC_OK) return 1;
    if (cc_thread_join(a).code != CC_OK) return 1;
    if (cc_thread_join(b).code != CC_OK) return 1;

    int ok = state.violation == 0 && cc_run_queue_in_flight(state.queue) == 0;
    cc_run_queue_destroy(state.queue);
    cc_mutex_destroy(state.mutex);
    return ok ? 0 : 1;
}
