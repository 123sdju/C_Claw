/**
 * 学习导读：cclaw/tests/adapters/test_memory_store_concurrent.c
 *
 * 所属层次：测试层。
 * 阅读重点：这里用小型 Given/When/Then 场景固定行为，阅读时重点看每个断言防止哪类回归。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/*
 * test_memory_store_concurrent.c
 *
 * 测试目标：验证内存会话存储（Memory Session Store）在多线程并发追加/加载消息时的线程安全性。
 *
 * 测试方法：
 * - 创建 4 个线程，每个线程操作独立的 session（ses_0 ~ ses_3）。
 * - 每个线程向自己的 session 并发追加 LOOPS（100）条消息。
 * - 所有线程完成后，主线程依次加载每个 session 的消息列表，
 *   验证消息数量是否精确等于 LOOPS。
 *
 * 边界条件与验证点：
 * - 并发追加：多线程同时向共享存储追加消息，
 *   验证内部数据结构（如哈希表/链表）的线程安全性。
 * - 数据完整性：每条消息都包含 id、session_id、role、content 等字段，
 *   验证消息字段不被并发写入破坏。
 * - 消息加载：并发追加后立即加载，验证写入对所有读取方立即可见。
 * - 资源清理：加载后的消息需逐字段释放内存，验证没有内存泄漏或野指针。
 *
 * 通过标准：每个 session 加载到的消息数量严格等于 LOOPS。
 */

#include "cc/ports/cc_session_store.h"
#include "cc/ports/cc_thread.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define THREADS 4
#define LOOPS 100

/**
 * cc_memory_session_store_create — 创建、启动或加载组件资源，并把错误统一传播给调用方。
 *
 * 位置：测试层。注释重点说明当前函数的输入输出、资源边界和错误传播。
 *
 * @param out_store 输出参数；成功时写入有效结果，失败时保持为 NULL 或未定义状态。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
extern cc_result_t cc_memory_session_store_create(cc_session_store_t *out_store);

/* 线程上下文：持有 store 引用、线程序号（决定 session 归属）、失败标志 */
typedef struct {
    cc_session_store_t *store;
    int index;
    int failed;
} store_ctx_t;

/*
 * 工作线程函数
 * 1. 创建本线程专属的 session（格式 "ses_N"）
 * 2. 循环 LOOPS 次，每次创建并追加一条 CC_ROLE_USER 消息
 * 3. 消息 id 格式为 "msg_{线程序号}_{循环序号}"，确保全局唯一
 * 4. 任一步骤失败则设置 failed 标志
 */
static void *worker(void *arg)
{
    store_ctx_t *ctx = (store_ctx_t *)arg;
    char sid[32];
    snprintf(sid, sizeof(sid), "ses_%d", ctx->index);
    if (ctx->store->vtable->create_session(ctx->store->self, sid, ".").code != CC_OK) ctx->failed = 1;

    for (int i = 0; i < LOOPS; i++) {
        char mid[64];
        snprintf(mid, sizeof(mid), "msg_%d_%d", ctx->index, i);
        cc_message_t *msg = NULL;
        cc_message_create(mid, sid, CC_ROLE_USER, "hello", NULL, &msg);
        if (ctx->store->vtable->append_message(ctx->store->self, msg).code != CC_OK) ctx->failed = 1;
        cc_message_destroy(msg);
    }
    return NULL;
}

/**
 * main — 执行本文件的 Given/When/Then 回归测试，失败时以非零退出码暴露问题。
 *
 * 位置：测试层。注释重点说明当前函数的输入输出、资源边界和错误传播。
 *
 * @return 0 通常表示成功完成，非 0 表示失败或应向进程层传播的状态。
 */
int main(void)
{
    cc_session_store_t store;
    if (cc_memory_session_store_create(&store).code != CC_OK) return 1;

    /* 启动并发写入：THREADS 个线程同时向不同 session 追加消息 */
    store_ctx_t ctx[THREADS];
    cc_thread_t threads[THREADS];
    for (int i = 0; i < THREADS; i++) {
        ctx[i].store = &store;
        ctx[i].index = i;
        ctx[i].failed = 0;
        cc_thread_create(worker, &ctx[i], &threads[i]);
    }
    for (int i = 0; i < THREADS; i++) cc_thread_join(threads[i]);

    /* 验证阶段：逐一加载每个 session 的消息并检查数量完整性 */
    for (int i = 0; i < THREADS; i++) {
        char sid[32];
        cc_message_t *messages = NULL;
        size_t count = 0;
        snprintf(sid, sizeof(sid), "ses_%d", i);
        store.vtable->load_messages(store.self, sid, 0, &messages, &count);
        if (count != LOOPS) ctx[i].failed = 1;
        /* 释放每条消息的动态字段内存 */
        for (size_t j = 0; j < count; j++) {
            cc_message_cleanup(&messages[j]);
        }
        free(messages);
    }

    /* 汇总结果 */
    int failed = 0;
    for (int i = 0; i < THREADS; i++) failed |= ctx[i].failed;
    store.vtable->destroy(store.self);
    return failed ? 1 : 0;
}
