/**
 * 学习导读：cclaw/platforms/posix/src/cc_posix_thread.c
 *
 * 所属层次：平台层。
 * 阅读重点：这里隐藏 POSIX、Windows、ESP32 的系统 API 差异，阅读时重点看同名端口函数如何按平台实现。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

#include "cc/ports/cc_thread.h"

#include <pthread.h>
#include <stdlib.h>

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

/* 学习注释：cc_thread_join 是对外可见或跨模块调用的入口。
 * 阅读时重点确认参数校验、所有权转移、错误码和清理路径是否成对出现。 */
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

/* 学习注释：cc_mutex_create 是对外可见或跨模块调用的入口。
 * 阅读时重点确认参数校验、所有权转移、错误码和清理路径是否成对出现。 */
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

/* 学习注释：cc_mutex_destroy 是对外可见或跨模块调用的入口。
 * 阅读时重点确认参数校验、所有权转移、错误码和清理路径是否成对出现。 */
void cc_mutex_destroy(cc_mutex_t mutex)
{
    if (!mutex) return;
    pthread_mutex_destroy((pthread_mutex_t *)mutex);
    free(mutex);
}

/* 学习注释：cc_mutex_lock 是对外可见或跨模块调用的入口。
 * 阅读时重点确认参数校验、所有权转移、错误码和清理路径是否成对出现。 */
void cc_mutex_lock(cc_mutex_t mutex)
{
    if (!mutex) return;
    pthread_mutex_lock((pthread_mutex_t *)mutex);
}

/* 学习注释：cc_mutex_unlock 是对外可见或跨模块调用的入口。
 * 阅读时重点确认参数校验、所有权转移、错误码和清理路径是否成对出现。 */
void cc_mutex_unlock(cc_mutex_t mutex)
{
    if (!mutex) return;
    pthread_mutex_unlock((pthread_mutex_t *)mutex);
}
