

#include "cc/ports/cc_event_bus.h"
#include "cc/ports/cc_platform.h"
#include "cc/ports/cc_thread.h"
#include "cc/util/cc_redaction.h"

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

/* 单个订阅项；event_type 为 NULL 时代表通配订阅。 */
typedef struct {
    char *event_type;
    cc_event_handler_fn handler;
    void *user_data;
    int busy;
} cc_event_handler_entry_t;

/*
 * 发布前拍下的订阅快照。
 *
 * sync 模式使用快照后释放锁再执行回调，避免 handler 里再次订阅/发布造成死锁。
 */
typedef struct {
    size_t handler_index;
    cc_event_handler_fn handler;
    void *user_data;
} cc_event_handler_snapshot_t;

/* 异步模式队列任务；payload 字符串由 job 拥有，worker 执行后释放。 */
typedef struct cc_event_job {
    size_t handler_index;
    cc_event_handler_fn handler;
    void *user_data;
    char *event_type;
    char *event_json;
    struct cc_event_job *next;
} cc_event_job_t;

/*
 * event bus 内部状态。
 *
 * handlers 是固定上限数组，避免订阅表在运行中无限扩张；异步模式下 jobs_* 组成
 * FIFO 队列，mutex/cond 由平台线程 port 提供，便于 POSIX/FreeRTOS/ESP32 替换。
 */
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

/*
 * 复制可选字符串。
 *
 * event_type 可以是 NULL 通配符，event_json 也允许为空；这个 helper 保留 NULL 语义。
 */
static char *dup_optional(const char *value)
{
    return value ? strdup(value) : NULL;
}

/*
 * 释放异步事件任务。
 *
 * job 中 event_type/event_json 都是发布时复制的快照，释放时不影响原调用方缓冲。
 */
static void free_job(cc_event_job_t *job)
{
    if (!job) return;
    free(job->event_type);
    free(job->event_json);
    free(job);
}

/*
 * 判断订阅项是否匹配事件。
 *
 * NULL 订阅代表观察所有事件，非 NULL 订阅使用精确匹配；不支持通配模式是为了保持
 * event bus 简单可预测。
 */
static int event_matches(const cc_event_handler_entry_t *entry, const char *event_type)
{
    return entry->event_type == NULL || strcmp(entry->event_type, event_type) == 0;
}

/* worker_count 为 0 时落到平台默认值。 */
static size_t normalized_worker_count(size_t value)
{
    return value ? value : DEFAULT_ASYNC_WORKERS;
}

/* max_pending 为 0 时落到平台默认值，避免异步队列没有背压。 */
static size_t normalized_pending_limit(size_t value)
{
    return value ? value : DEFAULT_ASYNC_PENDING;
}

/*
 * 返回默认 event bus 配置。
 *
 * 默认同步模式让最小 SDK 和单元测试具备确定性；需要异步观测时由 runtime builder
 * 或应用显式开启。
 */
cc_event_bus_config_t cc_event_bus_default_config(void)
{
    cc_event_bus_config_t config;
    config.mode = CC_EVENT_BUS_MODE_SYNC;
    config.worker_count = 0;
    config.max_pending = 0;
    return config;
}

/*
 * 判断某个 handler/user_data 组合是否正在执行。
 *
 * 异步模式允许多个 worker 并发处理不同订阅者，但同一个订阅者串行执行，避免 handler
 * 自身不是线程安全时被并发重入。
 */
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

/*
 * 从队列中取出当前可运行的 job。
 *
 * 函数在持锁状态下调用，会跳过同一 handler 正忙的任务，从而在保持单订阅者串行的
 * 同时让其他订阅者继续消费事件。
 */
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

/*
 * 标记 job 完成。
 *
 * 释放 handler busy 标记并递减 running_count，然后广播条件变量，唤醒 flush、
 * destroy 或等待队列空间的 publisher。
 */
static void finish_job_locked(cc_event_bus_t *bus, cc_event_job_t *job)
{
    if (job->handler_index < bus->count) bus->handlers[job->handler_index].busy = 0;
    if (bus->running_count > 0) bus->running_count--;
    cc_cond_broadcast(bus->cond);
}

/*
 * 异步 worker 主循环。
 *
 * worker 在条件变量上等待可运行任务；shutdown 且无 pending 任务时退出。回调在
 * 不持锁状态下执行，防止 handler 内部发布事件时发生锁重入死锁。
 */
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

/* 使用默认配置创建 event bus。 */
cc_result_t cc_event_bus_create(cc_event_bus_t **out_bus)
{
    return cc_event_bus_create_with_config(NULL, out_bus);
}

/*
 * 创建 event bus。
 *
 * 根据配置初始化 mutex/cond/worker。任一阶段失败都按反向顺序释放已创建资源，
 * 这是嵌入式 C 中构造复杂对象时最重要的错误路径模式之一。
 */
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

/*
 * 销毁 event bus。
 *
 * 异步模式先设置 shutting_down 并唤醒 worker，再 join 确保不会有回调继续访问 bus。
 * 最后释放订阅字符串和残留队列任务。
 */
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

/*
 * 等待异步事件全部处理完成。
 *
 * flush 不关闭 bus，只等待 pending 和 running 都归零；测试和需要强一致日志的应用
 * 可以在关键点调用它。同步模式没有队列，直接成功。
 */
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

/*
 * 注册事件订阅。
 *
 * 订阅表受 mutex 保护，event_type 被深拷贝到 bus 内部。当前设计没有 unsubscribe，
 * 这样可以避免异步 worker 正在使用 handler 时出现悬垂订阅项。
 */
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

/*
 * 在持锁状态下构建匹配订阅快照。
 *
 * 快照只保存 handler 指针和 user_data，不复制 event_type；发布路径随后释放锁再执行
 * 回调，避免用户回调阻塞订阅表。
 */
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

/*
 * 同步发布事件。
 *
 * 先拍快照后解锁，再顺序调用 handler。这样保持同步模式的确定性，同时避免 handler
 * 重入 event bus 时锁住自己。
 */
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

/*
 * 为异步订阅者入队一个 job。
 *
 * payload 在入队前复制，发布者返回后可以释放自己的缓冲。队列达到 max_pending 时
 * 发布线程等待，这是内存受限系统里必须有的背压策略。
 */
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

/*
 * 异步发布事件。
 *
 * 与同步发布一样先构建订阅快照，再为每个订阅者独立入队。这样一个慢 handler 不会
 * 阻塞其他 handler 的任务生成，只会受到队列背压影响。
 */
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

/*
 * 发布事件并统一脱敏。
 *
 * 这是 event bus 的底层入口；业务代码应优先走 observability 构造层。这里对 payload
 * 做 JSON-aware redaction，确保即使上层忘记脱敏，也不会把 token/password 等敏感
 * 字段交给订阅者。
 */
cc_result_t cc_event_bus_publish(
    cc_event_bus_t *bus,
    const char *event_type,
    const char *event_json
)
{
    if (!bus || !event_type) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null argument");
    char *redacted = cc_redact_secrets(event_json);
    const char *payload = redacted ? redacted : event_json;
    cc_result_t rc;
    if (bus->mode == CC_EVENT_BUS_MODE_ASYNC) {
        rc = publish_async(bus, event_type, payload);
    } else {
        rc = publish_sync(bus, event_type, payload);
    }
    free(redacted);
    return rc;
}
