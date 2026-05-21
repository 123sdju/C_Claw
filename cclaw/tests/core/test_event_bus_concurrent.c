/**
 * 学习导读：cclaw/tests/core/test_event_bus_concurrent.c
 *
 * 所属层次：测试层。
 * 阅读重点：这里用小型 Given/When/Then 场景固定行为，阅读时重点看每个断言防止哪类回归。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/*
 * test_event_bus_concurrent.c
 *
 * 测试目标：验证 Event Bus 在并发发布/订阅及嵌套事件触发场景下的线程安全性。
 *
 * 测试方法：
 * - 订阅一个通配 handler（event_type 为 NULL 表示匹配所有事件）。
 * - 4 个线程同时循环发布 "root" 事件，共发布 THREADS * LOOPS 条。
 * - handler 中每当收到以 'r' 开头的事件类型（即 "root" 事件），
 *   会嵌套发布一个 "nested" 事件。
 * - 使用互斥锁保护共享计数器 count。
 *
 * 边界条件与验证点：
 * - 并发发布：多线程同时调用 cc_event_bus_publish，验证事件总线的内部锁机制。
 * - 嵌套事件：handler 在处理事件过程中再次发布事件，
 *   验证事件总线是否支持递归/嵌套发布而不发生死锁。
 * - 计数正确性：期望每种事件类型都被正确计数，
 *   最终 count == THREADS * LOOPS * 2（每个 "root" 触发一个 "nested"）。
 *
 * 通过标准：count 精确等于 THREADS * LOOPS * 2。
 */

#include "cc/ports/cc_event_bus.h"
#include "cc/ports/cc_thread.h"

#define THREADS 4
#define LOOPS 200

/* 事件上下文：持有事件总线引用、保护计数器的互斥锁、以及事件总数计数器 */
typedef struct {
    cc_event_bus_t *bus;
    cc_mutex_t mutex;
    int count;
} event_ctx_t;

/*
 * 事件处理器
 * - 对任意事件类型，递增全局计数器。
 * - 如果事件类型以 'r' 开头（即 "root" 事件），
 *   嵌套发布一个 "nested" 事件，验证嵌套发布能力。
 * - 所有对 count 的操作都在互斥锁保护下进行。
 */
static void handler(const char *event_type, const char *event_json, void *user_data)
{
    (void)event_json;
    event_ctx_t *ctx = (event_ctx_t *)user_data;
    cc_mutex_lock(ctx->mutex);
    ctx->count++;
    cc_mutex_unlock(ctx->mutex);
    if (event_type && event_type[0] == 'r') {
        cc_event_bus_publish(ctx->bus, "nested", "{}");
    }
}

/*
 * 发布者线程函数
 * 每个线程循环 LOOPS 次，每次发布一个 "root" 事件。
 * 由于 handler 会对每个 "root" 事件再发布一个 "nested" 事件，
 * 因此每个 "root" 最终产生 2 次 handler 调用。
 */
static void *publisher(void *arg)
{
    event_ctx_t *ctx = (event_ctx_t *)arg;
    for (int i = 0; i < LOOPS; i++) {
        cc_event_bus_publish(ctx->bus, "root", "{}");
    }
    return NULL;
}

/**
 * main — 执行本文件的 Given/When/Then 回归测试，失败时以非零退出码暴露问题。
 *
 * @return 0 通常表示成功完成，非 0 表示失败或应向进程层传播的状态。
 */
int main(void)
{
    event_ctx_t ctx = {0};
    if (cc_event_bus_create(&ctx.bus).code != CC_OK) return 1;
    if (cc_mutex_create(&ctx.mutex).code != CC_OK) return 1;
    /* 订阅所有事件类型（event_type == NULL 为通配订阅） */
    if (cc_event_bus_subscribe(ctx.bus, NULL, handler, &ctx).code != CC_OK) return 1;

    cc_thread_t threads[THREADS];
    for (int i = 0; i < THREADS; i++) cc_thread_create(publisher, &ctx, &threads[i]);
    for (int i = 0; i < THREADS; i++) cc_thread_join(threads[i]);

    /* 验证计数：每个 root 事件触发 handler 一次 + 嵌套 nested 事件再触 handler 一次 = 2 倍 */
    int ok = ctx.count == THREADS * LOOPS * 2;
    cc_mutex_destroy(ctx.mutex);
    cc_event_bus_destroy(ctx.bus);
    return ok ? 0 : 1;
}
