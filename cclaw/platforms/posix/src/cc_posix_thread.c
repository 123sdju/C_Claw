



#include "cc/ports/cc_thread.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>

/*
 * 创建 POSIX 线程。
 *
 * cc_thread_t 在 public API 中是不透明句柄；POSIX 实现把 pthread_t 放到堆上，join 后释放。
 * 这让不同平台可以用同一个指针型句柄表达线程对象。
 */
cc_result_t cc_thread_create(cc_thread_fn_t fn, void *arg, cc_thread_t *out_thread)
{
    if (!fn || !out_thread) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid thread argument");
    }

    pthread_t *thread = malloc(sizeof(pthread_t));
    if (!thread) {
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate thread");
    }

    if (pthread_create(thread, NULL, fn, arg) != 0) {
        free(thread);
        return cc_result_error(CC_ERR_PLATFORM, "Failed to create thread");
    }

    *out_thread = thread;
    return cc_result_ok();
}

/*
 * 等待线程结束并释放句柄。
 *
 * 该 API 是 join-once 语义：成功或失败后都会释放 pthread_t 包装对象，调用方不能再次 join。
 */
cc_result_t cc_thread_join(cc_thread_t thread)
{
    if (!thread) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null thread");
    }

    pthread_t *pthread = (pthread_t *)thread;
    if (pthread_join(*pthread, NULL) != 0) {
        free(pthread);
        return cc_result_error(CC_ERR_PLATFORM, "Failed to join thread");
    }

    free(pthread);
    return cc_result_ok();
}

/*
 * 创建递归 mutex。
 *
 * runtime 中部分路径可能在同线程中重入锁保护对象，因此 POSIX 端使用
 * PTHREAD_MUTEX_RECURSIVE。嵌入式移植时需要确认 RTOS mutex 是否支持递归。
 */
cc_result_t cc_mutex_create(cc_mutex_t *out_mutex)
{
    if (!out_mutex) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null mutex output");
    }

    pthread_mutex_t *mutex = malloc(sizeof(pthread_mutex_t));
    if (!mutex) {
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate mutex");
    }

    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);

    int rc = pthread_mutex_init(mutex, &attr);
    pthread_mutexattr_destroy(&attr);
    if (rc != 0) {
        free(mutex);
        return cc_result_error(CC_ERR_PLATFORM, "Failed to initialize mutex");
    }

    *out_mutex = mutex;
    return cc_result_ok();
}

/* 销毁 POSIX mutex；调用方必须保证没有线程仍持有或等待该锁。 */
void cc_mutex_destroy(cc_mutex_t mutex)
{
    if (!mutex) return;
    pthread_mutex_destroy((pthread_mutex_t *)mutex);
    free(mutex);
}

/* 加锁；空 mutex 被视为 no-op，便于裁剪 profile 中的防御式调用。 */
void cc_mutex_lock(cc_mutex_t mutex)
{
    if (!mutex) return;
    pthread_mutex_lock((pthread_mutex_t *)mutex);
}

/* 解锁；必须由持锁线程调用，语义与 pthread_mutex_unlock 一致。 */
void cc_mutex_unlock(cc_mutex_t mutex)
{
    if (!mutex) return;
    pthread_mutex_unlock((pthread_mutex_t *)mutex);
}

/*
 * 创建条件变量。
 *
 * 条件变量和 mutex 配合用于 run queue/event bus 等等待通知场景，返回句柄由调用方销毁。
 */
cc_result_t cc_cond_create(cc_cond_t *out_cond)
{
    if (!out_cond) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null condition output");
    }

    pthread_cond_t *cond = malloc(sizeof(pthread_cond_t));
    if (!cond) {
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate condition");
    }
    if (pthread_cond_init(cond, NULL) != 0) {
        free(cond);
        return cc_result_error(CC_ERR_PLATFORM, "Failed to initialize condition");
    }
    *out_cond = cond;
    return cc_result_ok();
}

/* 销毁条件变量；调用方必须保证没有线程还在 wait。 */
void cc_cond_destroy(cc_cond_t cond)
{
    if (!cond) return;
    pthread_cond_destroy((pthread_cond_t *)cond);
    free(cond);
}

/* 无限等待条件变量；调用方进入前必须已经持有 mutex。 */
void cc_cond_wait(cc_cond_t cond, cc_mutex_t mutex)
{
    if (!cond || !mutex) return;
    pthread_cond_wait((pthread_cond_t *)cond, (pthread_mutex_t *)mutex);
}

/*
 * 带超时等待条件变量。
 *
 * timeout_ms <= 0 表示无限等待并返回 1；正数使用 CLOCK_REALTIME 构造绝对 deadline。
 * 返回 0 只表示超时，非超时唤醒或其它 pthread 返回都按 1 处理。
 */
int cc_cond_timedwait(cc_cond_t cond, cc_mutex_t mutex, int timeout_ms)
{
    if (!cond || !mutex) return 0;
    if (timeout_ms <= 0) {
        pthread_cond_wait((pthread_cond_t *)cond, (pthread_mutex_t *)mutex);
        return 1;
    }

    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec += deadline.tv_nsec / 1000000000L;
        deadline.tv_nsec %= 1000000000L;
    }

    int rc = pthread_cond_timedwait(
        (pthread_cond_t *)cond,
        (pthread_mutex_t *)mutex,
        &deadline);
    return rc != ETIMEDOUT;
}

/* 唤醒一个等待者，用于队列单任务到达等场景。 */
void cc_cond_signal(cc_cond_t cond)
{
    if (!cond) return;
    pthread_cond_signal((pthread_cond_t *)cond);
}

/* 唤醒所有等待者，用于 shutdown、cancel 或 flush 等全局状态变化。 */
void cc_cond_broadcast(cc_cond_t cond)
{
    if (!cond) return;
    pthread_cond_broadcast((pthread_cond_t *)cond);
}
