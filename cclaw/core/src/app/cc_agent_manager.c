



#include "cc/app/cc_agent_manager.h"
#include "cc/ports/cc_thread.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* agent id 到 runtime 指针的路由表项；manager 不拥有 runtime。 */
typedef struct cc_agent_manager_entry {
    char *id;
    cc_agent_runtime_t *runtime;
} cc_agent_manager_entry_t;

/* 异步消息任务前置声明，pending run 中保存指向任务的指针。 */
typedef struct cc_agent_manager_message_task cc_agent_manager_message_task_t;

/* 等待 collect 的异步 run；response 由任务写入并在 collect 时转移给调用方。 */
typedef struct cc_agent_manager_pending_run {
    cc_run_id_t run_id;
    char *response;
    cc_agent_manager_message_task_t *task;
    struct cc_agent_manager_pending_run *next;
} cc_agent_manager_pending_run_t;

/*
 * agent manager 内部状态。
 *
 * entries 是 agent 路由表，queue 是运行队列，pending_runs 保存已提交但未 collect 的结果。
 * mutex 保护路由表、当前 agent 和 pending 链表。
 */
struct cc_agent_manager {
    cc_agent_manager_entry_t *entries;
    size_t entry_count;
    size_t entry_capacity;
    cc_run_queue_t *queue;
    int owns_queue;
    char *current_agent_id;
    cc_run_queue_action_t default_action;
    cc_agent_manager_pending_run_t *pending_runs;
    cc_mutex_t mutex;
};

/* 单次用户消息处理任务；由 run queue worker 调用，完成后由 collect 释放。 */
struct cc_agent_manager_message_task {
    cc_agent_runtime_t *runtime;
    char *session_id;
    char *user_input;
    cc_agent_manager_pending_run_t *pending_run;
};

/*
 * run queue worker 执行的消息任务。
 *
 * 任务把 run queue 提供的 cancel token 传给 runtime，同步处理消息并把 response 写入
 * pending_run，等待 collect 转移所有权。
 */
static cc_result_t run_message_task(void *user_data, cc_cancel_token_t *cancel_token)
{
    cc_agent_manager_message_task_t *task = (cc_agent_manager_message_task_t *)user_data;
    if (cc_cancel_token_is_cancelled(cancel_token)) {
        return cc_result_error(CC_ERR_CANCELLED, "Agent run cancelled before execution");
    }
    cc_agent_runtime_run_options_t options;
    memset(&options, 0, sizeof(options));


    options.cancel_token = cancel_token;
    return cc_agent_runtime_handle_message_with_options(
        task->runtime,
        task->session_id,
        task->user_input,
        &options,
        &task->pending_run->response
    );
}

/* 归一化未知 action，避免外部传入非法 enum 破坏队列策略。 */
static cc_run_queue_action_t normalize_action(cc_run_queue_action_t action)
{
    switch (action) {
        case CC_RUN_QUEUE_ACTION_STEER:
        case CC_RUN_QUEUE_ACTION_FOLLOWUP:
        case CC_RUN_QUEUE_ACTION_COLLECT:
        case CC_RUN_QUEUE_ACTION_INTERRUPT:
            return action;
        default:
            return CC_RUN_QUEUE_ACTION_STEER;
    }
}

/* 确保 agent 路由表有空间。 */
static int ensure_entry_capacity(cc_agent_manager_t *manager)
{
    if (manager->entry_count < manager->entry_capacity) return 1;
    size_t next_capacity = manager->entry_capacity ? manager->entry_capacity * 2 : 4;
    cc_agent_manager_entry_t *next = realloc(manager->entries, next_capacity * sizeof(*next));
    if (!next) return 0;
    memset(next + manager->entry_capacity, 0, (next_capacity - manager->entry_capacity) * sizeof(*next));
    manager->entries = next;
    manager->entry_capacity = next_capacity;
    return 1;
}

/*
 * 查找 agent runtime。
 *
 * agent_id 为 NULL 时使用 current_agent_id，再回退到 "default"。调用方必须持有 manager
 * mutex，保证 entries/current_agent_id 不被并发修改。
 */
static cc_agent_runtime_t *find_runtime(cc_agent_manager_t *manager, const char *agent_id)
{
    const char *effective_id = agent_id ? agent_id :
        (manager->current_agent_id ? manager->current_agent_id : "default");
    for (size_t i = 0; i < manager->entry_count; i++) {
        if (manager->entries[i].id && strcmp(manager->entries[i].id, effective_id) == 0) {
            return manager->entries[i].runtime;
        }
    }
    return NULL;
}

/*
 * 构造 run queue session key。
 *
 * key 采用 agent:session，保证不同 agent 的同名 session 不会在队列中互相阻塞或中断。
 */
cc_result_t cc_agent_manager_make_session_key(
    const char *agent_id,
    const char *session_id,
    char **out_key
)
{
    if (!out_key) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null session key output");
    const char *aid = agent_id ? agent_id : "default";
    const char *sid = session_id ? session_id : "";
    size_t len = strlen(aid) + strlen(sid) + 2;
    char *key = malloc(len);
    if (!key) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate session key");
    snprintf(key, len, "%s:%s", aid, sid);
    *out_key = key;
    return cc_result_ok();
}

/* 释放异步消息任务。 */
static void free_message_task(cc_agent_manager_message_task_t *task)
{
    if (!task) return;
    free(task->session_id);
    free(task->user_input);
    free(task);
}

/* 在持锁状态下查找 pending run。 */
static cc_agent_manager_pending_run_t *find_pending_run_locked(
    cc_agent_manager_t *manager,
    cc_run_id_t run_id
)
{
    for (cc_agent_manager_pending_run_t *run = manager->pending_runs; run; run = run->next) {
        if (run->run_id == run_id) return run;
    }
    return NULL;
}

/* 在持锁状态下从 pending 链表摘除 run，所有权交给调用方。 */
static cc_agent_manager_pending_run_t *take_pending_run_locked(
    cc_agent_manager_t *manager,
    cc_run_id_t run_id
)
{
    cc_agent_manager_pending_run_t *prev = NULL;
    cc_agent_manager_pending_run_t *run = manager->pending_runs;
    while (run) {
        if (run->run_id == run_id) {
            if (prev) prev->next = run->next;
            else manager->pending_runs = run->next;
            run->next = NULL;
            return run;
        }
        prev = run;
        run = run->next;
    }
    return NULL;
}

/*
 * 创建 agent manager。
 *
 * 如果外部没有提供 run queue，manager 会创建并拥有一个默认队列；如果提供 default_runtime，
 * 会注册默认 agent 并设为当前 agent。
 */
cc_result_t cc_agent_manager_create(
    const cc_agent_manager_options_t *options,
    cc_agent_manager_t **out_manager
)
{
    if (!out_manager) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null out_manager");
    cc_agent_manager_t *manager = calloc(1, sizeof(cc_agent_manager_t));
    if (!manager) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create agent manager");

    cc_result_t lock_rc = cc_mutex_create(&manager->mutex);
    if (lock_rc.code != CC_OK) {
        free(manager);
        return lock_rc;
    }
    manager->default_action = options ?
        normalize_action(options->default_action) : CC_RUN_QUEUE_ACTION_STEER;

    if (options && options->queue) {
        manager->queue = options->queue;
        manager->owns_queue = options->owns_queue ? 1 : 0;
    } else {
        cc_run_queue_config_t queue_config = cc_run_queue_default_config();
        cc_result_t rc = cc_run_queue_create(&queue_config, &manager->queue);
        if (rc.code != CC_OK) {
            cc_mutex_destroy(manager->mutex);
            free(manager);
            return rc;
        }
        manager->owns_queue = 1;
    }

    if (options && options->default_runtime) {
        const char *default_id = options->default_agent_id ? options->default_agent_id : "default";
        cc_result_t rc = cc_agent_manager_add_agent(manager, default_id, options->default_runtime);
        if (rc.code != CC_OK) {
            cc_agent_manager_destroy(manager);
            return rc;
        }
        manager->current_agent_id = strdup(default_id);
        if (!manager->current_agent_id) {
            cc_agent_manager_destroy(manager);
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy current agent id");
        }
    }

    *out_manager = manager;
    return cc_result_ok();
}

/*
 * 销毁 agent manager。
 *
 * 如果 owns_queue 为真则销毁队列；pending run 尚未 collect 时释放其 response/task，避免
 * 异步调用方泄漏。runtime 指针不由 manager 销毁。
 */
void cc_agent_manager_destroy(cc_agent_manager_t *manager)
{
    if (!manager) return;
    if (manager->owns_queue) {
        cc_run_queue_destroy(manager->queue);
    }
    cc_agent_manager_pending_run_t *run = manager->pending_runs;
    while (run) {
        cc_agent_manager_pending_run_t *next = run->next;
        free(run->response);
        free_message_task(run->task);
        free(run);
        run = next;
    }
    for (size_t i = 0; i < manager->entry_count; i++) {
        free(manager->entries[i].id);
    }
    free(manager->current_agent_id);
    free(manager->entries);
    cc_mutex_destroy(manager->mutex);
    free(manager);
}

/*
 * 注册或替换 agent runtime。
 *
 * agent id 被复制进 manager；runtime 只是借用指针，外部必须保证生命周期覆盖 manager
 * 使用期间。
 */
cc_result_t cc_agent_manager_add_agent(
    cc_agent_manager_t *manager,
    const char *agent_id,
    cc_agent_runtime_t *runtime
)
{
    if (!manager || !agent_id || !runtime) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid agent registration");
    }
    cc_mutex_lock(manager->mutex);
    for (size_t i = 0; i < manager->entry_count; i++) {
        if (strcmp(manager->entries[i].id, agent_id) == 0) {
            manager->entries[i].runtime = runtime;
            cc_mutex_unlock(manager->mutex);
            return cc_result_ok();
        }
    }
    if (!ensure_entry_capacity(manager)) {
        cc_mutex_unlock(manager->mutex);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to grow agent table");
    }
    manager->entries[manager->entry_count].id = strdup(agent_id);
    if (!manager->entries[manager->entry_count].id) {
        cc_mutex_unlock(manager->mutex);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy agent id");
    }
    manager->entries[manager->entry_count].runtime = runtime;
    manager->entry_count++;
    cc_mutex_unlock(manager->mutex);
    return cc_result_ok();
}

/* 使用默认提交选项同步处理消息。 */
cc_result_t cc_agent_manager_handle_message(
    cc_agent_manager_t *manager,
    const char *agent_id,
    const char *session_id,
    const char *user_input,
    char **out_response
)
{
    return cc_agent_manager_handle_message_with_options(
        manager, agent_id, session_id, user_input, NULL, out_response);
}

/*
 * 同步处理消息。
 *
 * 实现复用 submit + collect，保证同步和异步路径共享队列串行化、中断和取消语义。
 */
cc_result_t cc_agent_manager_handle_message_with_options(
    cc_agent_manager_t *manager,
    const char *agent_id,
    const char *session_id,
    const char *user_input,
    const cc_agent_manager_submit_options_t *options,
    char **out_response
)
{
    cc_run_id_t run_id = 0;
    cc_result_t rc = cc_agent_manager_submit_with_options(
        manager,
        agent_id,
        session_id,
        user_input,
        options,
        &run_id
    );
    if (rc.code != CC_OK) return rc;
    rc = cc_agent_manager_collect(manager, run_id, out_response);
    return rc;
}

/* 使用默认提交选项异步提交消息。 */
cc_result_t cc_agent_manager_submit(
    cc_agent_manager_t *manager,
    const char *agent_id,
    const char *session_id,
    const char *user_input,
    cc_run_id_t *out_run_id
)
{
    return cc_agent_manager_submit_with_options(
        manager, agent_id, session_id, user_input, NULL, out_run_id);
}

/*
 * 异步提交消息处理任务。
 *
 * 函数在锁内查找 runtime 和当前 agent，然后复制必要字符串，构造 pending/task 并提交到
 * run queue。提交成功后 pending run 进入链表，等待 collect 取回。
 */
cc_result_t cc_agent_manager_submit_with_options(
    cc_agent_manager_t *manager,
    const char *agent_id,
    const char *session_id,
    const char *user_input,
    const cc_agent_manager_submit_options_t *options,
    cc_run_id_t *out_run_id
)
{
    if (!manager || !session_id || !user_input || !out_run_id) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid agent submit request");
    }

    cc_mutex_lock(manager->mutex);
    cc_agent_runtime_t *runtime = find_runtime(manager, agent_id);
    const char *effective_agent_id = agent_id ? agent_id :
        (manager->current_agent_id ? manager->current_agent_id : "default");
    char *agent_id_copy = strdup(effective_agent_id);
    cc_mutex_unlock(manager->mutex);
    if (!runtime) {
        free(agent_id_copy);
        return cc_result_error(CC_ERR_NOT_FOUND, "Agent runtime not found");
    }
    if (!agent_id_copy) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy agent id");

    char *session_key = NULL;
    cc_result_t rc = cc_agent_manager_make_session_key(agent_id_copy, session_id, &session_key);
    free(agent_id_copy);
    if (rc.code != CC_OK) return rc;

    cc_agent_manager_pending_run_t *pending = calloc(1, sizeof(*pending));
    cc_agent_manager_message_task_t *task = calloc(1, sizeof(*task));
    if (!pending || !task) {
        free(pending);
        free(task);
        free(session_key);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate agent run task");
    }
    task->runtime = runtime;
    task->session_id = strdup(session_id);
    task->user_input = strdup(user_input);
    task->pending_run = pending;
    pending->task = task;
    if (!task->session_id || !task->user_input) {
        free_message_task(task);
        free(pending);
        free(session_key);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy agent run input");
    }

    cc_run_queue_request_t request;
    request.session_key = session_key;
    request.lane = options ? options->lane : CC_RUN_QUEUE_LANE_MAIN;
    request.action = options ? normalize_action(options->action) : manager->default_action;
    cc_run_id_t run_id = 0;
    rc = cc_run_queue_submit_with_token(manager->queue, &request, run_message_task, task, &run_id);
    free(session_key);
    if (rc.code != CC_OK) {
        free_message_task(task);
        free(pending);
        return rc;
    }
    pending->run_id = run_id;
    cc_mutex_lock(manager->mutex);
    pending->next = manager->pending_runs;
    manager->pending_runs = pending;
    cc_mutex_unlock(manager->mutex);
    *out_run_id = run_id;
    return cc_result_ok();
}

/*
 * 收集异步 run 结果。
 *
 * 先确认 pending run 存在，再等待 run queue 完成；完成后从 pending 链表摘除，成功时把
 * response 所有权转移给调用方，失败时释放 response。
 */
cc_result_t cc_agent_manager_collect(
    cc_agent_manager_t *manager,
    cc_run_id_t run_id,
    char **out_response
)
{
    if (!manager || !out_response || run_id == 0) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid agent collect request");
    }
    *out_response = NULL;
    cc_mutex_lock(manager->mutex);
    cc_agent_manager_pending_run_t *pending = find_pending_run_locked(manager, run_id);
    cc_mutex_unlock(manager->mutex);
    if (!pending) return cc_result_error(CC_ERR_NOT_FOUND, "Agent run id not found");

    cc_result_t rc = cc_run_queue_collect(manager->queue, run_id);

    cc_mutex_lock(manager->mutex);
    pending = take_pending_run_locked(manager, run_id);
    cc_mutex_unlock(manager->mutex);
    if (pending) {
        if (rc.code == CC_OK) {
            *out_response = pending->response;
            pending->response = NULL;
        }
        free(pending->response);
        free_message_task(pending->task);
        free(pending);
    }
    return rc;
}

/* 设置当前 agent，并校验目标 agent 已注册。 */
cc_result_t cc_agent_manager_set_current_agent(
    cc_agent_manager_t *manager,
    const char *agent_id
)
{
    if (!manager || !agent_id) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid current agent request");
    }
    cc_mutex_lock(manager->mutex);
    if (!find_runtime(manager, agent_id)) {
        cc_mutex_unlock(manager->mutex);
        return cc_result_error(CC_ERR_NOT_FOUND, "Agent runtime not found");
    }
    char *copy = strdup(agent_id);
    if (!copy) {
        cc_mutex_unlock(manager->mutex);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy agent id");
    }
    free(manager->current_agent_id);
    manager->current_agent_id = copy;
    cc_mutex_unlock(manager->mutex);
    return cc_result_ok();
}

/* 切换当前 agent；当前语义等同 set_current_agent。 */
cc_result_t cc_agent_manager_switch_agent(
    cc_agent_manager_t *manager,
    const char *agent_id
)
{
    return cc_agent_manager_set_current_agent(manager, agent_id);
}

/* 返回当前 agent id；返回值由 manager 拥有。 */
const char *cc_agent_manager_current_agent(cc_agent_manager_t *manager)
{
    if (!manager) return NULL;
    return manager->current_agent_id ? manager->current_agent_id : "default";
}

/*
 * 中断指定 agent/session 的队列任务。
 *
 * 函数先复制 effective agent id，再构造 session key 并转发给 run queue，避免持锁期间调用
 * 队列造成锁顺序问题。
 */
cc_result_t cc_agent_manager_interrupt(
    cc_agent_manager_t *manager,
    const char *agent_id,
    const char *session_id
)
{
    if (!manager || !session_id) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid agent interrupt request");
    }
    cc_mutex_lock(manager->mutex);
    const char *effective_agent_id = agent_id ? agent_id :
        (manager->current_agent_id ? manager->current_agent_id : "default");
    char *agent_id_copy = strdup(effective_agent_id);
    cc_mutex_unlock(manager->mutex);
    if (!agent_id_copy) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy agent id");
    char *session_key = NULL;
    cc_result_t rc = cc_agent_manager_make_session_key(agent_id_copy, session_id, &session_key);
    free(agent_id_copy);
    if (rc.code != CC_OK) return rc;
    rc = cc_run_queue_interrupt_session(manager->queue, session_key);
    free(session_key);
    return rc;
}

/*
 * 重置 session。
 *
 * 重置前先中断该 agent/session 的运行任务，再调用 runtime 的 session store clear_session。
 * 如果 store 不支持 reset，返回明确错误。
 */
cc_result_t cc_agent_manager_reset_session(
    cc_agent_manager_t *manager,
    const char *agent_id,
    const char *session_id
)
{
    if (!manager || !session_id) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid agent reset request");
    }

    cc_mutex_lock(manager->mutex);
    cc_agent_runtime_t *runtime = find_runtime(manager, agent_id);
    const char *effective_agent_id = agent_id ? agent_id :
        (manager->current_agent_id ? manager->current_agent_id : "default");
    char *agent_id_copy = strdup(effective_agent_id);
    cc_mutex_unlock(manager->mutex);
    if (!runtime) {
        free(agent_id_copy);
        return cc_result_error(CC_ERR_NOT_FOUND, "Agent runtime not found");
    }
    if (!agent_id_copy) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy agent id");

    char *session_key = NULL;
    cc_result_t rc = cc_agent_manager_make_session_key(agent_id_copy, session_id, &session_key);
    free(agent_id_copy);
    if (rc.code != CC_OK) return rc;
    rc = cc_run_queue_interrupt_session(manager->queue, session_key);
    free(session_key);
    if (rc.code != CC_OK) return rc;

    cc_session_store_t *store = cc_agent_runtime_session_store(runtime);
    if (!store || !store->vtable || !store->vtable->clear_session) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Session store does not support reset");
    }
    return store->vtable->clear_session(store->self, session_id);
}

/* 列出已注册 agent id，返回数组和字符串所有权交给调用方。 */
cc_result_t cc_agent_manager_list_agents(
    cc_agent_manager_t *manager,
    char ***out_ids,
    size_t *out_count
)
{
    if (!manager || !out_ids || !out_count) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid agent list request");
    }
    *out_ids = NULL;
    *out_count = 0;
    cc_mutex_lock(manager->mutex);
    if (manager->entry_count == 0) {
        cc_mutex_unlock(manager->mutex);
        return cc_result_ok();
    }
    char **ids = calloc(manager->entry_count, sizeof(char *));
    if (!ids) {
        cc_mutex_unlock(manager->mutex);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate agent list");
    }
    for (size_t i = 0; i < manager->entry_count; i++) {
        ids[i] = strdup(manager->entries[i].id ? manager->entries[i].id : "");
        if (!ids[i]) {
            for (size_t j = 0; j < i; j++) free(ids[j]);
            free(ids);
            cc_mutex_unlock(manager->mutex);
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy agent id");
        }
    }
    *out_ids = ids;
    *out_count = manager->entry_count;
    cc_mutex_unlock(manager->mutex);
    return cc_result_ok();
}
