/**
 * 学习导读：cclaw/platforms/posix/src/cc_posix_thread.c
 *
 * 所属层次：平台层。
 * 阅读重点：这里隐藏 POSIX、Windows、ESP32 的系统 API 差异，阅读时重点看同名端口函数如何按平台实现。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

#include "cc/ports/cc_thread.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>

/**
 * cc_thread_create — 用 pthread 创建平台线程并把句柄写入 out_thread。
 *
 * @param fn 线程入口函数；不可为 NULL。
 * @param arg 传给线程入口的借用上下文。
 * @param out_thread 输出线程句柄；调用方后续 join/destroy。
 * @return CC_OK 表示创建成功；失败返回参数、内存或平台错误。
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

/**
 * cc_thread_join — 等待平台线程结束，并把底层 join 错误转换为统一结果。
 *
 * 位置：POSIX 平台层。注释重点说明当前函数的输入输出、资源边界和错误传播。
 *
 * @param thread 借用的对象；函数不释放该对象本身。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
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

/**
 * cc_mutex_create — 创建、启动或加载组件资源，并把错误统一传播给调用方。
 *
 * 位置：POSIX 平台层。注释重点说明当前函数的输入输出、资源边界和错误传播。
 *
 * @param out_mutex 输出参数；成功时写入有效结果，失败时保持为 NULL 或未定义状态。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
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

/**
 * cc_mutex_destroy — 释放、停止或复位该组件拥有的资源，防止失败路径泄漏。
 *
 * 位置：POSIX 平台层。注释重点说明当前函数的输入输出、资源边界和错误传播。
 *
 * @param mutex 借用的对象；函数不释放该对象本身。
 * 无返回值；副作用体现在对象状态、输出缓冲区或资源释放上。
 */
void cc_mutex_destroy(cc_mutex_t mutex)
{
    if (!mutex) return;
    pthread_mutex_destroy((pthread_mutex_t *)mutex);
    free(mutex);
}

/**
 * cc_mutex_lock — 进入平台互斥锁临界区，保护共享状态读写。
 *
 * 位置：POSIX 平台层。注释重点说明当前函数的输入输出、资源边界和错误传播。
 *
 * @param mutex 借用的对象；函数不释放该对象本身。
 * 无返回值；副作用体现在对象状态、输出缓冲区或资源释放上。
 */
void cc_mutex_lock(cc_mutex_t mutex)
{
    if (!mutex) return;
    pthread_mutex_lock((pthread_mutex_t *)mutex);
}

/**
 * cc_mutex_unlock — 离开平台互斥锁临界区，让其他线程继续访问共享状态。
 *
 * 位置：POSIX 平台层。注释重点说明当前函数的输入输出、资源边界和错误传播。
 *
 * @param mutex 借用的对象；函数不释放该对象本身。
 * 无返回值；副作用体现在对象状态、输出缓冲区或资源释放上。
 */
void cc_mutex_unlock(cc_mutex_t mutex)
{
    if (!mutex) return;
    pthread_mutex_unlock((pthread_mutex_t *)mutex);
}

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

void cc_cond_destroy(cc_cond_t cond)
{
    if (!cond) return;
    pthread_cond_destroy((pthread_cond_t *)cond);
    free(cond);
}

void cc_cond_wait(cc_cond_t cond, cc_mutex_t mutex)
{
    if (!cond || !mutex) return;
    pthread_cond_wait((pthread_cond_t *)cond, (pthread_mutex_t *)mutex);
}

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

void cc_cond_signal(cc_cond_t cond)
{
    if (!cond) return;
    pthread_cond_signal((pthread_cond_t *)cond);
}

void cc_cond_broadcast(cc_cond_t cond)
{
    if (!cond) return;
    pthread_cond_broadcast((pthread_cond_t *)cond);
}
