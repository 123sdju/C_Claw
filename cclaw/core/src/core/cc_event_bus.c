/**
 * 学习导读：cclaw/core/src/core/cc_event_bus.c
 *
 * 所属层次：核心层。
 * 阅读重点：这里实现可配置同步/异步事件总线。同步模式看 handler 快照、
 *           锁外回调和嵌套 publish；异步模式看有界队列、worker 分发、
 *           同 handler 函数 + user_data FIFO 和阻塞隔离。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

#include "cc/ports/cc_event_bus.h"
#include "cc/ports/cc_platform.h"
#include "cc/ports/cc_thread.h"

#include <stdlib.h>
#include <string.h>

#define MAX_HANDLERS 64

#if CC_PLATFORM == CC_PLATFORM_ESP32 || CC_PLATFORM == CC_PLATFORM_FREERTOS
#define DEFAULT_ASYNC_WORKERS 1
#define DEFAULT_ASYNC_PENDING 64
#else
#define DEFAULT_ASYNC_WORKERS 4
#define DEFAULT_ASYNC_PENDING 256
#endif

typedef struct {
    char *event_type;
    cc_event_handler_fn handler;
    void *user_data;
    int busy;
} cc_event_handler_entry_t;

typedef struct {
    size_t handler_index;
    cc_event_handler_fn handler;
    void *user_data;
} cc_event_handler_snapshot_t;

typedef struct cc_event_job {
    size_t handler_index;
    cc_event_handler_fn handler;
    void *user_data;
    char *event_type;
    char *event_json;
    struct cc_event_job *next;
} cc_event_job_t;

struct cc_event_bus {
    cc_event_handler_entry_t handlers[MAX_HANDLERS];
    size_t count;

    cc_event_bus_mode_t mode;
    size_t max_pending;
    size_t pending_count;
    size_t running_count;
    int shutting_down;

    cc_event_job_t *jobs_head;
    cc_event_job_t *jobs_tail;
    cc_thread_t *workers;
    size_t worker_count;

    cc_mutex_t mutex;
    cc_cond_t cond;
};

static char *dup_optional(const char *value)
{
    return value ? strdup(value) : NULL;
}

static void free_job(cc_event_job_t *job)
{
    if (!job) return;
    free(job->event_type);
    free(job->event_json);
    free(job);
}

static int event_matches(const cc_event_handler_entry_t *entry, const char *event_type)
{
    return entry->event_type == NULL || strcmp(entry->event_type, event_type) == 0;
}

static size_t normalized_worker_count(size_t value)
{
    return value ? value : DEFAULT_ASYNC_WORKERS;
}

static size_t normalized_pending_limit(size_t value)
{
    return value ? value : DEFAULT_ASYNC_PENDING;
}

cc_event_bus_config_t cc_event_bus_default_config(void)
{
    cc_event_bus_config_t config;
    config.mode = CC_EVENT_BUS_MODE_SYNC;
    config.worker_count = 0;
    config.max_pending = 0;
    return config;
}

static int handler_group_busy_locked(const cc_event_bus_t *bus, const cc_event_job_t *job)
{
    for (size_t i = 0; i < bus->count; i++) {
        const cc_event_handler_entry_t *entry = &bus->handlers[i];
        if (entry->busy && entry->handler == job->handler && entry->user_data == job->user_data) {
            return 1;
        }
    }
    return 0;
}

static cc_event_job_t *take_runnable_job_locked(cc_event_bus_t *bus)
{
    cc_event_job_t *prev = NULL;
    cc_event_job_t *job = bus->jobs_head;
    while (job) {
        if (job->handler_index < bus->count && !handler_group_busy_locked(bus, job)) {
            if (prev) {
                prev->next = job->next;
            } else {
                bus->jobs_head = job->next;
            }
            if (bus->jobs_tail == job) bus->jobs_tail = prev;
            job->next = NULL;
            bus->pending_count--;
            bus->running_count++;
            bus->handlers[job->handler_index].busy = 1;
            cc_cond_broadcast(bus->cond);
            return job;
        }
        prev = job;
        job = job->next;
    }
    return NULL;
}

static void finish_job_locked(cc_event_bus_t *bus, cc_event_job_t *job)
{
    if (job->handler_index < bus->count) bus->handlers[job->handler_index].busy = 0;
    if (bus->running_count > 0) bus->running_count--;
    cc_cond_broadcast(bus->cond);
}

static void *event_worker_main(void *arg)
{
    cc_event_bus_t *bus = (cc_event_bus_t *)arg;
    for (;;) {
        cc_mutex_lock(bus->mutex);
        cc_event_job_t *job = NULL;
        while (!job) {
            job = take_runnable_job_locked(bus);
            if (job) break;
            if (bus->shutting_down && bus->pending_count == 0) {
                cc_mutex_unlock(bus->mutex);
                return NULL;
            }
            cc_cond_wait(bus->cond, bus->mutex);
        }
        cc_mutex_unlock(bus->mutex);

        job->handler(job->event_type, job->event_json, job->user_data);

        cc_mutex_lock(bus->mutex);
        finish_job_locked(bus, job);
        cc_mutex_unlock(bus->mutex);
        free_job(job);
    }
}

cc_result_t cc_event_bus_create(cc_event_bus_t **out_bus)
{
    return cc_event_bus_create_with_config(NULL, out_bus);
}

cc_result_t cc_event_bus_create_with_config(
    const cc_event_bus_config_t *config,
    cc_event_bus_t **out_bus
)
{
    if (!out_bus) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null out_bus");
    *out_bus = NULL;

    cc_event_bus_config_t defaults = cc_event_bus_default_config();
    const cc_event_bus_config_t *effective = config ? config : &defaults;
    if (effective->mode != CC_EVENT_BUS_MODE_SYNC &&
        effective->mode != CC_EVENT_BUS_MODE_ASYNC) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid event bus mode");
    }

    cc_event_bus_t *bus = calloc(1, sizeof(cc_event_bus_t));
    if (!bus) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create event bus");
    bus->mode = effective->mode;
    bus->max_pending = normalized_pending_limit(effective->max_pending);
    bus->worker_count = bus->mode == CC_EVENT_BUS_MODE_ASYNC ?
        normalized_worker_count(effective->worker_count) : 0;

    cc_result_t rc = cc_mutex_create(&bus->mutex);
    if (rc.code != CC_OK) {
        free(bus);
        return rc;
    }

    if (bus->mode == CC_EVENT_BUS_MODE_ASYNC) {
        rc = cc_cond_create(&bus->cond);
        if (rc.code != CC_OK) {
            cc_mutex_destroy(bus->mutex);
            free(bus);
            return rc;
        }
        bus->workers = calloc(bus->worker_count, sizeof(cc_thread_t));
        if (!bus->workers) {
            cc_cond_destroy(bus->cond);
            cc_mutex_destroy(bus->mutex);
            free(bus);
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate event bus workers");
        }
        for (size_t i = 0; i < bus->worker_count; i++) {
            rc = cc_thread_create(event_worker_main, bus, &bus->workers[i]);
            if (rc.code != CC_OK) {
                cc_mutex_lock(bus->mutex);
                bus->shutting_down = 1;
                cc_cond_broadcast(bus->cond);
                cc_mutex_unlock(bus->mutex);
                for (size_t j = 0; j < i; j++) {
                    if (bus->workers[j]) cc_thread_join(bus->workers[j]);
                }
                free(bus->workers);
                cc_cond_destroy(bus->cond);
                cc_mutex_destroy(bus->mutex);
                free(bus);
                return rc;
            }
        }
    }

    *out_bus = bus;
    return cc_result_ok();
}

void cc_event_bus_destroy(cc_event_bus_t *bus)
{
    if (!bus) return;

    if (bus->mode == CC_EVENT_BUS_MODE_ASYNC) {
        cc_mutex_lock(bus->mutex);
        bus->shutting_down = 1;
        cc_cond_broadcast(bus->cond);
        cc_mutex_unlock(bus->mutex);

        for (size_t i = 0; i < bus->worker_count; i++) {
            if (bus->workers[i]) cc_thread_join(bus->workers[i]);
        }
        free(bus->workers);
    }

    cc_mutex_lock(bus->mutex);
    for (size_t i = 0; i < bus->count; i++) {
        free(bus->handlers[i].event_type);
    }
    cc_event_job_t *job = bus->jobs_head;
    while (job) {
        cc_event_job_t *next = job->next;
        free_job(job);
        job = next;
    }
    cc_mutex_unlock(bus->mutex);

    if (bus->cond) cc_cond_destroy(bus->cond);
    cc_mutex_destroy(bus->mutex);
    free(bus);
}

cc_result_t cc_event_bus_flush(cc_event_bus_t *bus)
{
    if (!bus) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null argument");
    if (bus->mode == CC_EVENT_BUS_MODE_SYNC) return cc_result_ok();

    cc_mutex_lock(bus->mutex);
    while (bus->pending_count > 0 || bus->running_count > 0) {
        cc_cond_wait(bus->cond, bus->mutex);
    }
    cc_mutex_unlock(bus->mutex);
    return cc_result_ok();
}

cc_result_t cc_event_bus_subscribe(
    cc_event_bus_t *bus,
    const char *event_type,
    cc_event_handler_fn handler,
    void *user_data
)
{
    if (!bus || !handler) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null argument");
    cc_mutex_lock(bus->mutex);
    if (bus->shutting_down) {
        cc_mutex_unlock(bus->mutex);
        return cc_result_error(CC_ERR_CANCELLED, "Event bus is shutting down");
    }
    if (bus->count >= MAX_HANDLERS) {
        cc_mutex_unlock(bus->mutex);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Event bus full");
    }

    char *owned_type = dup_optional(event_type);
    if (event_type && !owned_type) {
        cc_mutex_unlock(bus->mutex);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy event type");
    }

    bus->handlers[bus->count].event_type = owned_type;
    bus->handlers[bus->count].handler = handler;
    bus->handlers[bus->count].user_data = user_data;
    bus->handlers[bus->count].busy = 0;
    bus->count++;
    cc_mutex_unlock(bus->mutex);
    return cc_result_ok();
}

static size_t build_snapshot_locked(
    cc_event_bus_t *bus,
    const char *event_type,
    cc_event_handler_snapshot_t *snapshot
)
{
    size_t snapshot_count = 0;
    for (size_t i = 0; i < bus->count; i++) {
        cc_event_handler_entry_t *entry = &bus->handlers[i];
        if (event_matches(entry, event_type)) {
            snapshot[snapshot_count].handler_index = i;
            snapshot[snapshot_count].handler = entry->handler;
            snapshot[snapshot_count].user_data = entry->user_data;
            snapshot_count++;
        }
    }
    return snapshot_count;
}

static cc_result_t publish_sync(
    cc_event_bus_t *bus,
    const char *event_type,
    const char *event_json
)
{
    cc_event_handler_snapshot_t snapshot[MAX_HANDLERS];
    size_t snapshot_count;

    cc_mutex_lock(bus->mutex);
    if (bus->shutting_down) {
        cc_mutex_unlock(bus->mutex);
        return cc_result_error(CC_ERR_CANCELLED, "Event bus is shutting down");
    }
    snapshot_count = build_snapshot_locked(bus, event_type, snapshot);
    cc_mutex_unlock(bus->mutex);

    for (size_t i = 0; i < snapshot_count; i++) {
        snapshot[i].handler(event_type, event_json, snapshot[i].user_data);
    }
    return cc_result_ok();
}

static cc_result_t enqueue_async_job(
    cc_event_bus_t *bus,
    const cc_event_handler_snapshot_t *handler,
    const char *event_type,
    const char *event_json
)
{
    cc_event_job_t *job = calloc(1, sizeof(*job));
    if (!job) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate event job");
    job->handler_index = handler->handler_index;
    job->handler = handler->handler;
    job->user_data = handler->user_data;
    job->event_type = strdup(event_type);
    job->event_json = dup_optional(event_json);
    if (!job->event_type || (event_json && !job->event_json)) {
        free_job(job);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy event payload");
    }

    cc_mutex_lock(bus->mutex);
    while (!bus->shutting_down && bus->pending_count >= bus->max_pending) {
        cc_cond_wait(bus->cond, bus->mutex);
    }
    if (bus->shutting_down) {
        cc_mutex_unlock(bus->mutex);
        free_job(job);
        return cc_result_error(CC_ERR_CANCELLED, "Event bus is shutting down");
    }
    if (bus->jobs_tail) {
        bus->jobs_tail->next = job;
    } else {
        bus->jobs_head = job;
    }
    bus->jobs_tail = job;
    bus->pending_count++;
    cc_cond_broadcast(bus->cond);
    cc_mutex_unlock(bus->mutex);
    return cc_result_ok();
}

static cc_result_t publish_async(
    cc_event_bus_t *bus,
    const char *event_type,
    const char *event_json
)
{
    cc_event_handler_snapshot_t snapshot[MAX_HANDLERS];
    size_t snapshot_count;

    cc_mutex_lock(bus->mutex);
    if (bus->shutting_down) {
        cc_mutex_unlock(bus->mutex);
        return cc_result_error(CC_ERR_CANCELLED, "Event bus is shutting down");
    }
    snapshot_count = build_snapshot_locked(bus, event_type, snapshot);
    cc_mutex_unlock(bus->mutex);

    for (size_t i = 0; i < snapshot_count; i++) {
        cc_result_t rc = enqueue_async_job(bus, &snapshot[i], event_type, event_json);
        if (rc.code != CC_OK) return rc;
    }
    return cc_result_ok();
}

cc_result_t cc_event_bus_publish(
    cc_event_bus_t *bus,
    const char *event_type,
    const char *event_json
)
{
    if (!bus || !event_type) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null argument");
    if (bus->mode == CC_EVENT_BUS_MODE_ASYNC) {
        return publish_async(bus, event_type, event_json);
    }
    return publish_sync(bus, event_type, event_json);
}
