



#ifndef CC_EVENT_BUS_H
#define CC_EVENT_BUS_H

#include "cc/core/cc_result.h"

/*
 * event bus 订阅回调。
 *
 * event_type 和 event_json 只在回调期间有效；如果要异步保存，订阅方必须自行复制。
 * user_data 由订阅方拥有，event bus 不释放它。回调中应避免长时间阻塞，尤其在
 * sync 模式会直接拖慢 runtime 业务路径。
 */
typedef void (*cc_event_handler_fn)(
    const char *event_type,
    const char *event_json,
    void *user_data
);

/*
 * event bus 执行模式。
 *
 * SYNC 适合 MCU 或测试，发布者线程直接调用 handler；ASYNC 适合 desktop/embedded
 * Linux，把事件放入队列由 worker 消费。业务实时输出仍使用 stream callback。
 */
typedef enum cc_event_bus_mode {
    CC_EVENT_BUS_MODE_SYNC = 0,
    CC_EVENT_BUS_MODE_ASYNC = 1
} cc_event_bus_mode_t;

/*
 * event bus 配置。
 *
 * worker_count/max_pending 为 0 时使用平台默认值。异步模式下 max_pending 是背压
 * 边界，避免日志/观测高峰无限吃内存；同步模式忽略 worker_count。
 */
typedef struct cc_event_bus_config {
    cc_event_bus_mode_t mode;
    size_t worker_count;
    size_t max_pending;
} cc_event_bus_config_t;

/* event bus 的不透明句柄；内部包含订阅表、队列、线程和同步原语。 */
typedef struct cc_event_bus cc_event_bus_t;

/* 返回默认配置：同步模式，无异步 worker，适合低资源和确定性测试。 */
cc_event_bus_config_t cc_event_bus_default_config(void);

/* 使用默认配置创建 event bus；成功后调用方用 cc_event_bus_destroy() 释放。 */
cc_result_t cc_event_bus_create(cc_event_bus_t **out_bus);

/* 使用指定配置创建 event bus；config 只在调用期间借用。 */
cc_result_t cc_event_bus_create_with_config(
    const cc_event_bus_config_t *config,
    cc_event_bus_t **out_bus
);

/* 销毁 event bus；异步模式会等待 worker 退出并释放剩余队列。 */
void cc_event_bus_destroy(cc_event_bus_t *bus);

/* 等待异步队列排空；同步模式立即成功返回。 */
cc_result_t cc_event_bus_flush(cc_event_bus_t *bus);

/*
 * 订阅事件。
 *
 * event_type 为 NULL 表示订阅所有事件；非 NULL 时只接收完全匹配的事件名。当前没有
 * unsubscribe，订阅生命周期跟随 bus，适合 SDK runtime 内部观测。
 */
cc_result_t cc_event_bus_subscribe(
    cc_event_bus_t *bus,
    const char *event_type,
    cc_event_handler_fn handler,
    void *user_data
);

/*
 * 发布事件 payload。
 *
 * 业务路径原则上通过 cc_observability_publish() 构造统一 schema 后再到达这里。
 * event_json 会在进入 handler 前统一 redaction，避免 token/password 泄漏到日志。
 */
cc_result_t cc_event_bus_publish(
    cc_event_bus_t *bus,
    const char *event_type,
    const char *event_json
);

#endif
