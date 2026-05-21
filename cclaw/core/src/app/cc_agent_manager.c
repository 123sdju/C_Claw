#include "cc/app/cc_agent_manager.h"
#include "cc/ports/cc_thread.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/*
 * Agent manager 是 CLI/app 面向 SDK 的主入口。它不拥有 runtime 的内部依赖，
 * 只保存 agent_id -> runtime 的映射，并把每次消息提交到 run queue。
 *
 * 生命周期：
 *   - runtime 指针由 runtime_builder 或 app 创建，manager 只借用。
 *   - queue 可借用也可由 manager 拥有，取决于 owns_queue。
 *   - pending_run 保存异步 run 的响应指针；collect 后释放节点。
 */
typedef struct cc_agent_manager_entry {
    char *id;
    cc_agent_runtime_t *runtime;
} cc_agent_manager_entry_t;

typedef struct cc_agent_manager_message_task cc_agent_manager_message_task_t;

typedef struct cc_agent_manager_pending_run {
    cc_run_id_t run_id;
    char *response;
    cc_agent_manager_message_task_t *task;
    struct cc_agent_manager_pending_run *next;
} cc_agent_manager_pending_run_t;

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

struct cc_agent_manager_message_task {
    cc_agent_runtime_t *runtime;
    char *session_id;
    char *user_input;
    cc_agent_manager_pending_run_t *pending_run;
};

static cc_result_t run_message_task(void *user_data, cc_cancel_token_t *cancel_token)
{
    cc_agent_manager_message_task_t *task = (cc_agent_manager_message_task_t *)user_data;
    if (cc_cancel_token_is_cancelled(cancel_token)) {
        return cc_result_error(CC_ERR_CANCELLED, "Agent run cancelled before execution");
    }
    cc_agent_runtime_run_options_t options;
    memset(&options, 0, sizeof(options));
    /*
     * cancel_token 直接透传到 runtime。runtime 再把它传给 LLM stream fallback、
     * tool executor、plugin/MCP transport。manager 自己不解释取消细节。
     */
    options.cancel_token = cancel_token;
    return cc_agent_runtime_handle_message_with_options(
        task->runtime,
        task->session_id,
        task->user_input,
        &options,
        &task->pending_run->response
    );
}

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

static void free_message_task(cc_agent_manager_message_task_t *task)
{
    if (!task) return;
    free(task->session_id);
    free(task->user_input);
    free(task);
}

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

cc_result_t cc_agent_manager_switch_agent(
    cc_agent_manager_t *manager,
    const char *agent_id
)
{
    return cc_agent_manager_set_current_agent(manager, agent_id);
}

const char *cc_agent_manager_current_agent(cc_agent_manager_t *manager)
{
    if (!manager) return NULL;
    return manager->current_agent_id ? manager->current_agent_id : "default";
}

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
