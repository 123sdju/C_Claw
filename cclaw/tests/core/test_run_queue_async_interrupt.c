/**
 * test_run_queue_async_interrupt.c
 *
 * 固定真实 job queue 的 submit/interrupt/collect 行为：job 由 queue worker
 * 线程执行，调用方可在 job 运行期间取消 run，worker 通过 cancel token 协作退出。
 */

#include "cc/app/cc_run_queue.h"
#include "cc/ports/cc_thread.h"

typedef struct interrupt_state {
    cc_mutex_t mutex;
    cc_cond_t cond;
    int started;
} interrupt_state_t;

static cc_result_t cancellable_task(void *user_data, cc_cancel_token_t *token)
{
    interrupt_state_t *state = (interrupt_state_t *)user_data;
    cc_mutex_lock(state->mutex);
    state->started = 1;
    cc_cond_broadcast(state->cond);
    cc_mutex_unlock(state->mutex);

    while (!cc_cancel_token_is_cancelled(token)) {
        for (volatile int i = 0; i < 10000; i++) {
        }
    }
    return cc_result_error(CC_ERR_CANCELLED, "Task observed cancellation");
}

int main(void)
{
    cc_run_queue_config_t config = cc_run_queue_default_config();
    config.main_concurrency = 1;
    config.per_session_concurrency = 1;

    interrupt_state_t state;
    state.mutex = NULL;
    state.cond = NULL;
    state.started = 0;

    if (cc_mutex_create(&state.mutex).code != CC_OK) return 1;
    if (cc_cond_create(&state.cond).code != CC_OK) return 1;

    cc_run_queue_t *queue = NULL;
    if (cc_run_queue_create(&config, &queue).code != CC_OK) return 1;

    cc_run_queue_request_t request;
    request.session_key = "agent:session";
    request.lane = CC_RUN_QUEUE_LANE_MAIN;
    request.action = CC_RUN_QUEUE_ACTION_FOLLOWUP;

    cc_run_id_t run_id = 0;
    cc_result_t rc = cc_run_queue_submit_with_token(
        queue,
        &request,
        cancellable_task,
        &state,
        &run_id
    );
    if (rc.code != CC_OK) return 1;
    cc_result_free(&rc);

    cc_mutex_lock(state.mutex);
    while (!state.started) {
        cc_cond_wait(state.cond, state.mutex);
    }
    cc_mutex_unlock(state.mutex);

    rc = cc_run_queue_interrupt_run(queue, run_id);
    if (rc.code != CC_OK) return 1;
    cc_result_free(&rc);

    rc = cc_run_queue_collect(queue, run_id);
    int ok = rc.code == CC_ERR_CANCELLED &&
        cc_run_queue_in_flight(queue) == 0 &&
        cc_run_queue_pending(queue) == 0;
    cc_result_free(&rc);

    cc_run_queue_destroy(queue);
    cc_cond_destroy(state.cond);
    cc_mutex_destroy(state.mutex);
    return ok ? 0 : 1;
}
