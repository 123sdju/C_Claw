/**
 * 学习导读：cclaw/core/src/app/cc_run_queue.c
 *
 * 所属层次：核心层。
 * 阅读重点：这里是 SDK 的统一并发调度器，重点看 session 串行语义、lane
 *          并行度控制、协作式取消（cancel token）和 worker 线程池调度。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * cc_run_queue.c — 跨平台 Agent run 并发调度队列
 *
 * 本模块在整体架构中的角色：
 * ─────────────────────────────
 * 位于 App 层（业务逻辑层），是 SDK 层唯一的多线程 job queue。它对上提供
 * submit/collect/run 三种提交模式，对内管理 worker 线程池、4 条 lane 的
 * 并发限制以及最多 128 个活跃 session 的状态追踪。所有平台（POSIX、Windows、
 * ESP32）复用同一套实现，不依赖平台特定 API。
 *
 * 上游调用方：
 *   - cc_agent_manager.c —— 将每次 handle_message 提交到 run queue，
 *     通过 submit_with_token 让 task 感知 cancel token
 *   - 高层 app（CLI/ESP）—— 直接使用 submit + collect 或同步 run 接口
 *
 * 下游依赖模块：
 *   - cc_thread.c / cc_mutex / cc_cond —— 线程、互斥锁、条件变量原语
 *   - cc_cancel_token.c —— 协作式取消，worker 在执行前后检查 cancel token
 *
 * ─── 数据结构 ───────────────────────────────────────────────────────
 *
 *   cc_run_queue_job_t：
 *     单个异步 job。持有唯一 id、session_key、归属 lane、action 语义、
 *     task 函数指针（含 with_token 版本）、user_data、cancel_source、
 *     执行结果和状态（PENDING/RUNNING/COMPLETED）。通过单向链表串联。
 *
 *   cc_run_queue_session_state_t：
 *     追踪某个 session_key 的飞行中 job 数量和 generation 号。generation
 *     在每次 interrupt 时递增，供 pending job 识别过期输入。
 *
 *   cc_run_queue（主结构体）：
 *     包含配置、4 条 lane 的飞行计数、最多 128 个 session 状态槽、全局
 *     飞行/等待计数器、next_run_id 自增编号、shutting_down 标记、jobs
 *     链表头、worker 线程数组及 mutex/cond 同步原语。
 *
 * ─── 四条 Lane 的并发模型 ───────────────────────────────────────────
 *
 *   CC_RUN_QUEUE_LANE_MAIN（默认并发 4）：
 *     主 agent 交互 turn，cc_agent_manager 提交的消息 run。
 *
 *   CC_RUN_QUEUE_LANE_SUBAGENT（默认并发 8）：
 *     子 agent 委托调用，并发度更高以允许并行子任务。
 *
 *   CC_RUN_QUEUE_LANE_PLUGIN（默认并发 4）：
 *     plugin 工具调用。
 *
 *   CC_RUN_QUEUE_LANE_MCP（默认并发 4）：
 *     MCP 工具调用。
 *
 *   每条 lane 独立维护 in_flight 计数，worker 在 find_runnable_job 中
 *   检查 lane 并发上限和 session 并发上限（默认 per_session_concurrency=1，
 *   即同一 session 串行），都满足时才会取出 job 执行。
 *
 * ─── Action 语义 ────────────────────────────────────────────────────
 *
 *   STEER：提交时取消同 session 的所有 pending 和 running job（通过 cancel
 *          source），然后追加新 job。适用于用户发起新一轮交互输入。
 *
 *   FOLLOWUP：追加 job，不取消已有任务。适用于工具回调等补充任务。
 *
 *   COLLECT：取消同 session 的 pending job，但不中断正在运行的 job。
 *            适用于只等待结果而不希望被后续输入打断的场景。
 *
 *   INTERRUPT：仅取消（同 STEER），不追加新 job。适用于端侧中断请求。
 *
 * ─── Worker 调度流程 ────────────────────────────────────────────────
 *
 *   1. worker 在 cond 上等待，被 submit 或 release 唤醒
 *   2. 持锁调用 find_runnable_job_locked：
 *      a. 遍历 jobs 链表找首个 PENDING job
 *      b. 查 lane_index，无效 lane 则标记完成并跳过
 *      c. 查 session 槽，不存在则创建（最多 128 个活跃 session）
 *      d. 检查 session.in_flight < per_session_concurrency 且
 *         lane_in_flight[lane] < lane_concurrency
 *      e. 满足条件则递增计数、置状态为 RUNNING、返回该 job
 *   3. worker 释放锁，在锁外执行 task
 *   4. 执行完毕，持锁调用 finish_running_job_locked：
 *      a. 递减 lane_in_flight 和 session.in_flight
 *      b. 清理空闲 session 槽（in_flight==0 的 session）
 *      c. 置状态为 COMPLETED
 *   5. broadcast cond，唤醒其他 worker 或 collect 等待者
 *
 * ─── 设计决策 ───────────────────────────────────────────────────────
 *
 *   为什么 worker 在锁外执行 task？
 *     worker 取到 job 后立即释放 mutex 再执行 task。这样长时间运行的
 *     task 不会阻塞其他 session 的 submit/collect 操作，也不会阻塞其他
 *     worker 从链表取新 job。task 内部通过 cancel_token 实现协作取消。
 *
 *   为什么 worker_count = 各 lane 并发之和（上限 32）？
 *     每个 lane 的并发限制由 lane_in_flight 独立执行，但 worker 线程池
 *     大小等于所有 lane 并发之和，确保每条 lane 达到上限时都有足够的
 *     worker 可用。上限 32 避免配置错误在桌面创建过多线程，也是 ESP
 *     profile 的保守保护。
 *
 *   为什么 session 数组是固定大小 CC_RUN_QUEUE_MAX_ACTIVE_SESSIONS=128？
 *     session 数组是线性查找的静态槽池。128 足够覆盖同时活跃的 session
 *     数，超出时创建失败并取消 job。空闲 session 在 in_flight 归零时
 *     立即清理，用 swap-with-last 减少内存移动。
 *
 *   为什么任务取消是协作式的？
 *     队列通过 cancel_source_cancel 标记取消，worker 在执行前后检查
 *     token。队列不强制 kill 线程——plugin worker、MCP transport、shell
 *     子进程各自在安全点释放资源。这是"请求取消"而非"强制终止"语义。
 *
 *   为什么 submit_with_token 的 user_data 默认只借用？
 *     user_data 生命周期由调用方管理。cc_run_queue_submit（不带 token
 *     版本）内部包装了一个 owns_user_data 标记，在 job 取消未执行时由
 *     队列负责清理；带 token 版本的调用方需要自行保证 user_data 在
 *     collect 前有效。
 */

#include "cc/app/cc_run_queue.h"
#include "cc/ports/cc_thread.h"

#include <stdlib.h>
#include <string.h>

/*
 * Run queue 是 SDK 层的真实 job queue。它只依赖 cc_thread/cc_mutex/cc_cond，
 * 所以 POSIX、Windows 和 ESP32 复用同一套 session/lane 语义。
 *
 * 锁约定：
 *   - queue->mutex 保护 jobs 链表、session in-flight、lane in-flight 和计数器。
 *   - worker 取到可运行 job 后在锁外执行 task，避免长时间持锁阻塞其它 session。
 *   - cancel source 属于 job；interrupt 只标记取消，具体 task 在安全点观察 token。
 */
#define CC_RUN_QUEUE_LANE_COUNT 4
#define CC_RUN_QUEUE_MAX_ACTIVE_SESSIONS 128

typedef enum cc_run_job_state {
    CC_RUN_JOB_PENDING = 0,
    CC_RUN_JOB_RUNNING = 1,
    CC_RUN_JOB_COMPLETED = 2
} cc_run_job_state_t;

typedef struct cc_run_queue_session_state {
    char *key;
    int in_flight;
    /* 每次 steer/interrupt 都递增 generation，用于让 pending job 识别过期输入。 */
    unsigned long generation;
} cc_run_queue_session_state_t;

typedef struct cc_run_queue_job {
    cc_run_id_t id;
    char *session_key;
    cc_run_queue_lane_t lane;
    cc_run_queue_action_t action;
    cc_run_queue_task_fn task;
    cc_run_queue_task_with_token_fn task_with_token;
    void *user_data;
    int owns_user_data;
    cc_cancel_source_t *cancel_source;
    cc_result_t result;
    cc_run_job_state_t state;
    struct cc_run_queue_job *next;
} cc_run_queue_job_t;

struct cc_run_queue {
    cc_run_queue_config_t config;
    int lane_in_flight[CC_RUN_QUEUE_LANE_COUNT];
    cc_run_queue_session_state_t sessions[CC_RUN_QUEUE_MAX_ACTIVE_SESSIONS];
    size_t session_count;
    size_t total_in_flight;
    size_t total_pending;
    cc_run_id_t next_run_id;
    int shutting_down;
    cc_run_queue_job_t *jobs;
    cc_thread_t *workers;
    size_t worker_count;
    cc_mutex_t mutex;
    cc_cond_t cond;
};

static cc_result_t cancelled_result(const char *message)
{
    return cc_result_error(CC_ERR_CANCELLED, message ? message : "Run cancelled");
}

static void free_job(cc_run_queue_job_t *job)
{
    if (!job) return;
    /*
     * job 持有 session_key、cancel_source 和 result 错误消息。user_data 默认只借用；
     * 少数内部路径可以设置 owns_user_data，让队列在取消未执行 job 时清理 task 对象。
     */
    free(job->session_key);
    cc_result_free(&job->result);
    cc_cancel_source_destroy(job->cancel_source);
    if (job->owns_user_data) free(job->user_data);
    free(job);
}

cc_run_queue_config_t cc_run_queue_default_config(void)
{
    cc_run_queue_config_t config;
    config.main_concurrency = 4;
    config.subagent_concurrency = 8;
    config.plugin_concurrency = 4;
    config.mcp_concurrency = 4;
    config.per_session_concurrency = 1;
    config.max_pending_per_session = 20;
    return config;
}

static int normalized_limit(int value)
{
    return value <= 0 ? 1 : value;
}

static int lane_index(cc_run_queue_lane_t lane)
{
    switch (lane) {
        case CC_RUN_QUEUE_LANE_MAIN: return 0;
        case CC_RUN_QUEUE_LANE_SUBAGENT: return 1;
        case CC_RUN_QUEUE_LANE_PLUGIN: return 2;
        case CC_RUN_QUEUE_LANE_MCP: return 3;
        default: return -1;
    }
}

static int lane_limit(const cc_run_queue_t *queue, int lane)
{
    switch (lane) {
        case 0: return normalized_limit(queue->config.main_concurrency);
        case 1: return normalized_limit(queue->config.subagent_concurrency);
        case 2: return normalized_limit(queue->config.plugin_concurrency);
        case 3: return normalized_limit(queue->config.mcp_concurrency);
        default: return 1;
    }
}

static int session_limit(const cc_run_queue_t *queue)
{
    return normalized_limit(queue->config.per_session_concurrency);
}

static size_t desired_worker_count(const cc_run_queue_config_t *config)
{
    int main_limit = normalized_limit(config->main_concurrency);
    int subagent_limit = normalized_limit(config->subagent_concurrency);
    int plugin_limit = normalized_limit(config->plugin_concurrency);
    int mcp_limit = normalized_limit(config->mcp_concurrency);
    int total = main_limit + subagent_limit + plugin_limit + mcp_limit;
    if (total < 1) total = 1;
    /*
     * 上限避免配置错误在桌面创建过多线程，也让 ESP profile 即使误开较大并发时
     * 仍有一个保守保护。lane 并发限制仍由 lane_in_flight 单独执行。
     */
    if (total > 32) total = 32;
    return (size_t)total;
}

static int find_session_locked(const cc_run_queue_t *queue, const char *session_key)
{
    for (size_t i = 0; i < queue->session_count; i++) {
        if (queue->sessions[i].key && strcmp(queue->sessions[i].key, session_key) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int create_session_locked(cc_run_queue_t *queue, const char *session_key)
{
    if (queue->session_count >= CC_RUN_QUEUE_MAX_ACTIVE_SESSIONS) return -1;
    cc_run_queue_session_state_t *state = &queue->sessions[queue->session_count];
    state->key = strdup(session_key);
    if (!state->key) return -1;
    state->in_flight = 0;
    state->generation = 0;
    queue->session_count++;
    return (int)(queue->session_count - 1);
}

static void remove_idle_session_locked(cc_run_queue_t *queue, int index)
{
    if (index < 0 || (size_t)index >= queue->session_count) return;
    if (queue->sessions[index].in_flight != 0) return;
    free(queue->sessions[index].key);
    size_t last = queue->session_count - 1;
    if ((size_t)index != last) {
        queue->sessions[index] = queue->sessions[last];
    }
    memset(&queue->sessions[last], 0, sizeof(queue->sessions[last]));
    queue->session_count--;
}

static size_t pending_count_for_session_locked(
    const cc_run_queue_t *queue,
    const char *session_key
)
{
    size_t count = 0;
    for (cc_run_queue_job_t *job = queue->jobs; job; job = job->next) {
        if (job->state == CC_RUN_JOB_PENDING &&
            job->session_key &&
            strcmp(job->session_key, session_key) == 0) {
            count++;
        }
    }
    return count;
}

static void complete_pending_job_locked(cc_run_queue_t *queue, cc_run_queue_job_t *job, const char *message)
{
    if (!job || job->state != CC_RUN_JOB_PENDING) return;
    job->state = CC_RUN_JOB_COMPLETED;
    job->result = cancelled_result(message);
    cc_cancel_source_cancel(job->cancel_source);
    if (queue->total_pending > 0) queue->total_pending--;
}

static void apply_submit_action_locked(
    cc_run_queue_t *queue,
    const cc_run_queue_request_t *request
)
{
    if (!queue || !request || !request->session_key) return;
    int cancel_running = request->action == CC_RUN_QUEUE_ACTION_STEER ||
        request->action == CC_RUN_QUEUE_ACTION_INTERRUPT;
    int cancel_pending = request->action == CC_RUN_QUEUE_ACTION_STEER ||
        request->action == CC_RUN_QUEUE_ACTION_COLLECT ||
        request->action == CC_RUN_QUEUE_ACTION_INTERRUPT;

    for (cc_run_queue_job_t *job = queue->jobs; job; job = job->next) {
        if (!job->session_key || strcmp(job->session_key, request->session_key) != 0) continue;
        if (cancel_pending && job->state == CC_RUN_JOB_PENDING) {
            complete_pending_job_locked(queue, job, "Run superseded by newer input");
        } else if (cancel_running && job->state == CC_RUN_JOB_RUNNING) {
            cc_cancel_source_cancel(job->cancel_source);
        }
    }
}

static cc_run_queue_job_t *find_job_locked(cc_run_queue_t *queue, cc_run_id_t run_id)
{
    for (cc_run_queue_job_t *job = queue->jobs; job; job = job->next) {
        if (job->id == run_id) return job;
    }
    return NULL;
}

static cc_run_queue_job_t *take_job_locked(cc_run_queue_t *queue, cc_run_id_t run_id)
{
    cc_run_queue_job_t *prev = NULL;
    cc_run_queue_job_t *job = queue->jobs;
    while (job) {
        if (job->id == run_id) {
            if (prev) prev->next = job->next;
            else queue->jobs = job->next;
            job->next = NULL;
            return job;
        }
        prev = job;
        job = job->next;
    }
    return NULL;
}

static cc_run_queue_job_t *find_runnable_job_locked(cc_run_queue_t *queue)
{
    for (cc_run_queue_job_t *job = queue->jobs; job; job = job->next) {
        if (job->state != CC_RUN_JOB_PENDING) continue;
        int lane = lane_index(job->lane);
        if (lane < 0) {
            complete_pending_job_locked(queue, job, "Invalid run queue lane");
            continue;
        }
        int session_index = find_session_locked(queue, job->session_key);
        if (session_index < 0) {
            session_index = create_session_locked(queue, job->session_key);
            if (session_index < 0) {
                complete_pending_job_locked(queue, job, "Failed to track session state");
                continue;
            }
        }
        cc_run_queue_session_state_t *session = &queue->sessions[session_index];
        if (session->in_flight < session_limit(queue) &&
            queue->lane_in_flight[lane] < lane_limit(queue, lane)) {
            session->in_flight++;
            queue->lane_in_flight[lane]++;
            queue->total_in_flight++;
            if (queue->total_pending > 0) queue->total_pending--;
            job->state = CC_RUN_JOB_RUNNING;
            return job;
        }
    }
    return NULL;
}

static void finish_running_job_locked(cc_run_queue_t *queue, cc_run_queue_job_t *job)
{
    int lane = lane_index(job->lane);
    if (lane >= 0 && queue->lane_in_flight[lane] > 0) {
        queue->lane_in_flight[lane]--;
    }
    int session_index = find_session_locked(queue, job->session_key);
    if (session_index >= 0) {
        if (queue->sessions[session_index].in_flight > 0) {
            queue->sessions[session_index].in_flight--;
        }
        remove_idle_session_locked(queue, session_index);
    }
    if (queue->total_in_flight > 0) queue->total_in_flight--;
    job->state = CC_RUN_JOB_COMPLETED;
}

static void *worker_main(void *arg)
{
    cc_run_queue_t *queue = (cc_run_queue_t *)arg;
    for (;;) {
        cc_mutex_lock(queue->mutex);
        cc_run_queue_job_t *job = NULL;
        while (!queue->shutting_down && !(job = find_runnable_job_locked(queue))) {
            cc_cond_wait(queue->cond, queue->mutex);
        }
        if (queue->shutting_down && !job) {
            cc_mutex_unlock(queue->mutex);
            return NULL;
        }
        cc_mutex_unlock(queue->mutex);

        if (cc_cancel_token_is_cancelled(cc_cancel_source_token(job->cancel_source))) {
            job->result = cancelled_result("Run cancelled before execution");
        } else if (job->task_with_token) {
            job->result = job->task_with_token(
                job->user_data,
                cc_cancel_source_token(job->cancel_source)
            );
        } else if (job->task) {
            job->result = job->task(job->user_data);
        } else {
            job->result = cc_result_error(CC_ERR_INVALID_ARGUMENT, "Run queue job has no task");
        }

        cc_mutex_lock(queue->mutex);
        finish_running_job_locked(queue, job);
        cc_cond_broadcast(queue->cond);
        cc_mutex_unlock(queue->mutex);
    }
}

cc_result_t cc_run_queue_create(
    const cc_run_queue_config_t *config,
    cc_run_queue_t **out_queue
)
{
    if (!out_queue) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null out_queue");
    cc_run_queue_t *queue = calloc(1, sizeof(cc_run_queue_t));
    if (!queue) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create run queue");
    queue->config = config ? *config : cc_run_queue_default_config();
    queue->next_run_id = 1;
    queue->worker_count = desired_worker_count(&queue->config);

    cc_result_t rc = cc_mutex_create(&queue->mutex);
    if (rc.code != CC_OK) {
        free(queue);
        return rc;
    }
    rc = cc_cond_create(&queue->cond);
    if (rc.code != CC_OK) {
        cc_mutex_destroy(queue->mutex);
        free(queue);
        return rc;
    }
    queue->workers = calloc(queue->worker_count, sizeof(cc_thread_t));
    if (!queue->workers) {
        cc_cond_destroy(queue->cond);
        cc_mutex_destroy(queue->mutex);
        free(queue);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate run queue workers");
    }
    for (size_t i = 0; i < queue->worker_count; i++) {
        rc = cc_thread_create(worker_main, queue, &queue->workers[i]);
        if (rc.code != CC_OK) {
            cc_mutex_lock(queue->mutex);
            queue->shutting_down = 1;
            cc_cond_broadcast(queue->cond);
            cc_mutex_unlock(queue->mutex);
            for (size_t j = 0; j < i; j++) {
                cc_result_t join_rc = cc_thread_join(queue->workers[j]);
                cc_result_free(&join_rc);
            }
            free(queue->workers);
            cc_cond_destroy(queue->cond);
            cc_mutex_destroy(queue->mutex);
            free(queue);
            return rc;
        }
    }
    *out_queue = queue;
    return cc_result_ok();
}

void cc_run_queue_destroy(cc_run_queue_t *queue)
{
    if (!queue) return;
    cc_mutex_lock(queue->mutex);
    queue->shutting_down = 1;
    for (cc_run_queue_job_t *job = queue->jobs; job; job = job->next) {
        if (job->state != CC_RUN_JOB_COMPLETED) {
            cc_cancel_source_cancel(job->cancel_source);
        }
    }
    cc_cond_broadcast(queue->cond);
    cc_mutex_unlock(queue->mutex);

    for (size_t i = 0; i < queue->worker_count; i++) {
        if (queue->workers[i]) {
            cc_result_t rc = cc_thread_join(queue->workers[i]);
            cc_result_free(&rc);
        }
    }
    free(queue->workers);

    cc_run_queue_job_t *job = queue->jobs;
    while (job) {
        cc_run_queue_job_t *next = job->next;
        free_job(job);
        job = next;
    }
    for (size_t i = 0; i < queue->session_count; i++) {
        free(queue->sessions[i].key);
    }
    cc_cond_destroy(queue->cond);
    cc_mutex_destroy(queue->mutex);
    free(queue);
}

cc_result_t cc_run_queue_submit_with_token(
    cc_run_queue_t *queue,
    const cc_run_queue_request_t *request,
    cc_run_queue_task_with_token_fn task,
    void *user_data,
    cc_run_id_t *out_run_id
)
{
    if (!queue || !request || !request->session_key || !task || !out_run_id) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid run queue submit request");
    }
    if (lane_index(request->lane) < 0) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid run queue lane");
    }

    cc_run_queue_job_t *job = calloc(1, sizeof(cc_run_queue_job_t));
    if (!job) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate run queue job");
    job->session_key = strdup(request->session_key);
    if (!job->session_key) {
        free(job);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy session key");
    }
    cc_result_t rc = cc_cancel_source_create(&job->cancel_source);
    if (rc.code != CC_OK) {
        free(job->session_key);
        free(job);
        return rc;
    }
    job->lane = request->lane;
    job->action = request->action;
    job->task_with_token = task;
    job->user_data = user_data;
    job->state = CC_RUN_JOB_PENDING;

    cc_mutex_lock(queue->mutex);
    if (queue->shutting_down) {
        cc_mutex_unlock(queue->mutex);
        free_job(job);
        return cc_result_error(CC_ERR_CANCELLED, "Run queue is shutting down");
    }
    if (request->action != CC_RUN_QUEUE_ACTION_STEER &&
        request->action != CC_RUN_QUEUE_ACTION_COLLECT &&
        queue->config.max_pending_per_session > 0 &&
        pending_count_for_session_locked(queue, request->session_key) >=
            (size_t)queue->config.max_pending_per_session) {
        cc_mutex_unlock(queue->mutex);
        free_job(job);
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Too many pending runs for session");
    }
    apply_submit_action_locked(queue, request);
    job->id = queue->next_run_id++;
    if (!queue->jobs) {
        queue->jobs = job;
    } else {
        cc_run_queue_job_t *tail = queue->jobs;
        while (tail->next) tail = tail->next;
        tail->next = job;
    }
    queue->total_pending++;
    *out_run_id = job->id;
    cc_cond_broadcast(queue->cond);
    cc_mutex_unlock(queue->mutex);
    return cc_result_ok();
}

typedef struct cc_run_queue_plain_task {
    cc_run_queue_task_fn task;
    void *user_data;
} cc_run_queue_plain_task_t;

static cc_result_t run_plain_task_with_token(void *user_data, cc_cancel_token_t *cancel_token)
{
    (void)cancel_token;
    cc_run_queue_plain_task_t *plain = (cc_run_queue_plain_task_t *)user_data;
    return plain->task(plain->user_data);
}

cc_result_t cc_run_queue_submit(
    cc_run_queue_t *queue,
    const cc_run_queue_request_t *request,
    cc_run_queue_task_fn task,
    void *user_data,
    cc_run_id_t *out_run_id
)
{
    if (!task) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null run queue task");
    cc_run_queue_plain_task_t *plain = malloc(sizeof(*plain));
    if (!plain) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate run queue task wrapper");
    plain->task = task;
    plain->user_data = user_data;
    cc_result_t rc = cc_run_queue_submit_with_token(
        queue,
        request,
        run_plain_task_with_token,
        plain,
        out_run_id
    );
    if (rc.code != CC_OK) free(plain);
    else {
        cc_mutex_lock(queue->mutex);
        cc_run_queue_job_t *job = find_job_locked(queue, *out_run_id);
        if (job) job->owns_user_data = 1;
        cc_mutex_unlock(queue->mutex);
    }
    return rc;
}

cc_result_t cc_run_queue_collect(
    cc_run_queue_t *queue,
    cc_run_id_t run_id
)
{
    if (!queue || run_id == 0) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid run queue collect request");
    }
    cc_mutex_lock(queue->mutex);
    cc_run_queue_job_t *job = find_job_locked(queue, run_id);
    while (job && job->state != CC_RUN_JOB_COMPLETED) {
        cc_cond_wait(queue->cond, queue->mutex);
        job = find_job_locked(queue, run_id);
    }
    if (!job) {
        cc_mutex_unlock(queue->mutex);
        return cc_result_error(CC_ERR_NOT_FOUND, "Run id not found");
    }
    job = take_job_locked(queue, run_id);
    cc_mutex_unlock(queue->mutex);

    cc_result_t result = job->result;
    job->result = cc_result_ok();
    free_job(job);
    return result;
}

cc_result_t cc_run_queue_run(
    cc_run_queue_t *queue,
    const cc_run_queue_request_t *request,
    cc_run_queue_task_fn task,
    void *user_data
)
{
    cc_run_id_t run_id = 0;
    cc_result_t rc = cc_run_queue_submit(queue, request, task, user_data, &run_id);
    if (rc.code != CC_OK) return rc;
    return cc_run_queue_collect(queue, run_id);
}

cc_result_t cc_run_queue_interrupt_session(
    cc_run_queue_t *queue,
    const char *session_key
)
{
    if (!queue || !session_key) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid run queue interrupt");
    }
    cc_mutex_lock(queue->mutex);
    int index = find_session_locked(queue, session_key);
    if (index >= 0) queue->sessions[index].generation++;
    for (cc_run_queue_job_t *job = queue->jobs; job; job = job->next) {
        if (!job->session_key || strcmp(job->session_key, session_key) != 0) continue;
        if (job->state == CC_RUN_JOB_PENDING) {
            complete_pending_job_locked(queue, job, "Run interrupted before execution");
        } else if (job->state == CC_RUN_JOB_RUNNING) {
            cc_cancel_source_cancel(job->cancel_source);
        }
    }
    cc_cond_broadcast(queue->cond);
    cc_mutex_unlock(queue->mutex);
    return cc_result_ok();
}

cc_result_t cc_run_queue_interrupt_run(
    cc_run_queue_t *queue,
    cc_run_id_t run_id
)
{
    if (!queue || run_id == 0) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid run queue interrupt request");
    }
    cc_mutex_lock(queue->mutex);
    cc_run_queue_job_t *job = find_job_locked(queue, run_id);
    if (!job) {
        cc_mutex_unlock(queue->mutex);
        return cc_result_error(CC_ERR_NOT_FOUND, "Run id not found");
    }
    if (job->state == CC_RUN_JOB_PENDING) {
        complete_pending_job_locked(queue, job, "Run interrupted before execution");
    } else if (job->state == CC_RUN_JOB_RUNNING) {
        cc_cancel_source_cancel(job->cancel_source);
    }
    cc_cond_broadcast(queue->cond);
    cc_mutex_unlock(queue->mutex);
    return cc_result_ok();
}

size_t cc_run_queue_in_flight(cc_run_queue_t *queue)
{
    if (!queue) return 0;
    cc_mutex_lock(queue->mutex);
    size_t value = queue->total_in_flight;
    cc_mutex_unlock(queue->mutex);
    return value;
}

size_t cc_run_queue_pending(cc_run_queue_t *queue)
{
    if (!queue) return 0;
    cc_mutex_lock(queue->mutex);
    size_t value = queue->total_pending;
    cc_mutex_unlock(queue->mutex);
    return value;
}
