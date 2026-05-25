/**
 * 学习导读：cclaw/ports/include/cc/ports/cc_event_bus.h
 *
 * 所属层次：端口层。
 * 阅读重点：这里定义可替换接口，阅读时重点看 struct + vtable + void *self 如何表达多态和依赖注入。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * cc_event_bus.h — 事件总线端口（Port）
 *
 * @file    cc/ports/cc_event_bus.h
 * @brief   提供发布-订阅模式的事件通信机制，用于组件间松耦合消息传递。
 *
 * 事件总线是应用中的横切关注点（Cross-cutting Concern），
 * 允许多个组件订阅感兴趣的事件类型并在事件发生时收到通知。
 * 发布者和订阅者彼此不感知对方的存在，通过事件类型进行松耦合通信。
 *
 * ─── 接口契约 ─────────────────────────────────────────────────────────
 *
 *   - 事件总线在堆上分配，由 create/destroy 管理生命周期
 *   - 事件类型是自由字符串（如 "tool.call.started"、"llm.response"），
 *     建议使用点分隔的命名空间约定
 *   - 事件负载是 JSON 字符串，订阅者自行解析
 *   - 默认 create 创建同步总线；create_with_config 可创建异步总线
 *   - 同步模式下 publish 调用会依次回调所有匹配的订阅者
 *   - 异步模式下 publish 只入队匹配 handler job，由后台 worker 分发
 *
 * ─── 使用模式 ─────────────────────────────────────────────────────────
 *
 *   发布事件：
 *     cc_event_bus_publish(bus, "tool.call.started", "{\"tool\":\"file_read\"}");
 *
 *   订阅事件：
 *     cc_event_bus_subscribe(bus, "tool.call.started", my_handler, my_data);
 *     cc_event_bus_subscribe(bus, NULL, my_global_handler, my_data);
 *
 * ─── 依赖 ─────────────────────────────────────────────────────────────
 *
 *   依赖 cc/core/cc_result.h。
 */

#ifndef CC_EVENT_BUS_H
#define CC_EVENT_BUS_H

#include "cc/core/cc_result.h"

/**
 * cc_event_handler_fn — 事件处理回调函数类型
 *
 * 订阅者注册的回调函数签名。当匹配的事件发布时被调用。
 * 同步模式下回调在发布线程中执行；异步模式下回调在
 * 事件总线 worker 线程中执行。
 *
 * @param event_type  事件的类型标识符（如 "tool.call.started"）
 * @param event_json  事件的负载数据（JSON 字符串，可为 NULL）
 * @param user_data   订阅时传入的用户数据指针
 */
typedef void (*cc_event_handler_fn)(
    const char *event_type,
    const char *event_json,
    void *user_data
);

/**
 * cc_event_bus_mode_t — 事件总线分发模式
 *
 * SYNC 保持传统语义：publish 返回时匹配 handler 已执行完。
 * ASYNC 提供阻塞隔离：publish 复制事件并入队，handler 在 worker 线程执行。
 */
typedef enum cc_event_bus_mode {
    CC_EVENT_BUS_MODE_SYNC = 0,
    CC_EVENT_BUS_MODE_ASYNC = 1
} cc_event_bus_mode_t;

/**
 * cc_event_bus_config_t — 事件总线创建配置
 *
 * worker_count 和 max_pending 为 0 时使用平台默认值。
 * 同步模式忽略 worker_count 和 max_pending。
 */
typedef struct cc_event_bus_config {
    cc_event_bus_mode_t mode;
    size_t worker_count;
    size_t max_pending;
} cc_event_bus_config_t;

/**
 * cc_event_bus_t — 事件总线（不透明类型）
 *
 * 内部维护一个订阅者列表（event_type → handler 的映射）。
 * 具体实现在 .c 文件中定义，对调用方透明。
 */
typedef struct cc_event_bus cc_event_bus_t;

/**
 * cc_event_bus_default_config — 返回默认事件总线配置
 *
 * 默认配置为同步模式，保持 cc_event_bus_create 的历史行为。
 *
 * @return 默认配置值
 */
cc_event_bus_config_t cc_event_bus_default_config(void);

/**
 * cc_event_bus_create — 创建事件总线实例
 *
 * 在堆上分配同步事件总线，初始状态无任何订阅者。
 *
 * @param out_bus  输出：指向新事件总线的指针（调用者负责 cc_event_bus_destroy）
 * @return         CC_OK 表示成功
 */
cc_result_t cc_event_bus_create(cc_event_bus_t **out_bus);

/**
 * cc_event_bus_create_with_config — 按配置创建事件总线实例
 *
 * config 为 NULL 时等价于 cc_event_bus_create。异步模式会创建后台 worker
 * 和有界队列，提供 handler 阻塞隔离。
 *
 * @param config   创建配置；可为 NULL
 * @param out_bus  输出：指向新事件总线的指针
 * @return         CC_OK 表示成功
 */
cc_result_t cc_event_bus_create_with_config(
    const cc_event_bus_config_t *config,
    cc_event_bus_t **out_bus
);

/**
 * cc_event_bus_destroy — 销毁事件总线
 *
 * 释放所有订阅者和内部数据结构。
 * 传入 NULL 是安全的（无操作）。
 *
 * @param bus  要销毁的事件总线指针
 */
void cc_event_bus_destroy(cc_event_bus_t *bus);

/**
 * cc_event_bus_flush — 等待已发布事件处理完成
 *
 * 同步模式为 no-op。异步模式会阻塞直到队列为空且 worker 没有正在执行的
 * handler job。
 *
 * @param bus  事件总线（不可为 NULL）
 * @return     CC_OK 表示完成
 */
cc_result_t cc_event_bus_flush(cc_event_bus_t *bus);

/**
 * cc_event_bus_subscribe — 订阅事件
 *
 * 注册一个事件处理器，当指定类型的事件发布时被调用。
 * 同一事件类型可以有多个订阅者。同步模式按注册顺序依次回调；
 * 异步模式保证同一 handler 函数 + user_data FIFO，不保证跨 handler 全局顺序。
 * event_type 为 NULL 时表示通配订阅（匹配所有事件）。
 *
 * @param bus         事件总线（不可为 NULL）
 * @param event_type  要订阅的事件类型；NULL 表示通配订阅
 * @param handler     回调函数（不可为 NULL）
 * @param user_data   用户数据指针，在回调中原样传回（可为 NULL）
 * @return            CC_OK 表示成功
 */
cc_result_t cc_event_bus_subscribe(
    cc_event_bus_t *bus,
    const char *event_type,
    cc_event_handler_fn handler,
    void *user_data
);

/**
 * cc_event_bus_publish — 发布事件
 *
 * 同步模式下向所有订阅了该事件类型的处理器同步发送通知。
 * 异步模式下复制事件并将匹配 handler 入队，worker 后台执行。
 * 异步模式隔离慢 handler 对发布线程的阻塞，但不隔离同进程崩溃。
 *
 * @param bus         事件总线（不可为 NULL）
 * @param event_type  要发布的事件类型（不可为 NULL）
 * @param event_json  事件的负载数据（JSON 字符串，可为 NULL）
 * @return            CC_OK 表示成功
 */
cc_result_t cc_event_bus_publish(
    cc_event_bus_t *bus,
    const char *event_type,
    const char *event_json
);

#endif
