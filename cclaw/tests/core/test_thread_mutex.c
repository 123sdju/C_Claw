/**
 * 学习导读：cclaw/tests/core/test_thread_mutex.c
 *
 * 所属层次：测试层。
 * 阅读重点：这里用小型 Given/When/Then 场景固定行为，阅读时重点看每个断言防止哪类回归。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/*
 * test_thread_mutex.c
 *
 * 测试目标：验证互斥锁（Mutex）在多线程并发自增操作中的正确性。
 *
 * 测试方法：
 * - 创建一个共享计数器（counter_t），包含一个 mutex 和一个整数值 value。
 * - 启动 8 个线程，每个线程在互斥锁保护下对 value 执行 LOOPS（10000）次自增。
 * - 理论正确值 = THREADS × LOOPS = 8 × 10000 = 80000。
 * - 所有线程完成后，主线程检查 value 是否精确等于 80000。
 *
 * 边界条件与验证点：
 * - 数据竞争检测：如果 mutex 实现有缺陷（如非原子操作、虚假唤醒未处理等），
 *   并发自增会产生丢失更新（lost update），
 *   导致最终 value < 80000。
 * - 高压力：80000 次加锁/解锁操作提供了足够的并发压力，
 *   足以暴露大多数锁实现的缺陷。
 * - 死锁检测：测试正常退出则无死锁。
 * - 线程创建/加入错误处理：检查线程创建和 join 的返回码。
 *
 * 通过标准：counter.value 精确等于 THREADS * LOOPS（80000）。
 */

#include "cc/ports/cc_thread.h"

#include <stdio.h>

#define THREADS 8
#define LOOPS 10000

/* 共享计数器：包含互斥锁和受保护的整数值 */
typedef struct {
    cc_mutex_t mutex;
    int value;
} counter_t;

/*
 * 工作线程函数
 * 在互斥锁的临界区内对共享计数器执行自增操作。
 * 循环 LOOPS 次，每次 lock → value++ → unlock，
 * 确保原子性的读写-修改-写回。
 */
static void *worker(void *arg)
{
    counter_t *counter = (counter_t *)arg;
    for (int i = 0; i < LOOPS; i++) {
        cc_mutex_lock(counter->mutex);   /* 进入临界区 */
        counter->value++;                /* 原子性的自增操作 */
        cc_mutex_unlock(counter->mutex); /* 退出临界区 */
    }
    return NULL;
}

/**
 * main — 执行本文件的 Given/When/Then 回归测试，失败时返回非零退出码。
 *
 * @return 0 表示断言全部通过，非 0 表示行为回归。
 */
int main(void)
{
    counter_t counter = {0};
    /* 创建互斥锁 */
    if (cc_mutex_create(&counter.mutex).code != CC_OK) return 1;

    /* 创建 THREADS 个线程并发执行自增 */
    cc_thread_t threads[THREADS];
    for (int i = 0; i < THREADS; i++) {
        if (cc_thread_create(worker, &counter, &threads[i]).code != CC_OK) return 1;
    }

    /* 等待所有线程完成 */
    for (int i = 0; i < THREADS; i++) {
        if (cc_thread_join(threads[i]).code != CC_OK) return 1;
    }

    /* 销毁互斥锁 */
    cc_mutex_destroy(counter.mutex);

    /* 验证计数器值：必须精确等于 THREADS * LOOPS，否则存在数据竞争 */
    if (counter.value != THREADS * LOOPS) {
        fprintf(stderr, "counter=%d\n", counter.value);
        return 1;
    }
    return 0;
}
