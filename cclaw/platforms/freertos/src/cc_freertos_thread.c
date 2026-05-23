/**
 * 学习导读：cclaw/platforms/freertos/src/cc_freertos_thread.c
 *
 * 所属层次：平台层。
 * 阅读重点：这里隐藏 POSIX、Windows、ESP32 的系统 API 差异，阅读时重点看同名端口函数如何按平台实现。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * cc_freertos_thread.c — FreeRTOS 线程与同步原语封装
 *
 * 在整体架构中的角色和层次：
 *   本模块位于 Platform 层的 FreeRTOS 平台实现子层。
 *   Platform 层是整个系统的最底层，负责封装操作系统差异。
 *   本文件是 cc_thread.h 端口接口在裸 FreeRTOS 平台的具体实现，
 *   基于 FreeRTOS xTaskCreate / xSemaphore 提供线程创建/等待、互斥锁
 *   和条件变量等同步原语。向上层提供统一的线程管理接口。
 *
 * 与 ESP32 版本的关系：
 *   本文件与 cc_esp32_thread.c 共享相同的实现策略（FreeRTOS Task +
 *   Binary Semaphore Join），但因为 ESP32 版本限定在 ESP_PLATFORM 条件，
 *   需要一个独立的裸 FreeRTOS 实现供其他 FreeRTOS 平台（如 stm32）使用。
 *   主要区别：栈大小为可配置宏 CCLAW_FREERTOS_TASK_STACK_WORDS（默认 2048 words），
 *   任务名为 "cclaw"（而非 "cclaw_thread"）。
 *
 * 线程模型（FreeRTOS Task + Binary Semaphore Join）：
 *   FreeRTOS task 本身没有 pthread_join 等价物，因此每个线程 wrapper
 *   （cc_freertos_thread_t）持有一个 binary semaphore。task 结束时
 *   xSemaphoreGive(done) 通知，cc_thread_join 通过 xSemaphoreTake 等待。
 *
 * 互斥锁模型（FreeRTOS Mutex Semaphore）：
 *   使用 xSemaphoreCreateMutex() 创建互斥信号量，lock/unlock 映射为
 *   xSemaphoreTake/xSemaphoreGive。
 *
 * 条件变量模型（Binary Semaphore 模拟）：
 *   wait 操作先释放互斥锁、等待条件信号量、再重新获取互斥锁。
 *   broadcast 不支持（signal 和 broadcast 行为相同）。
 */

#include "cc/ports/cc_thread.h"

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#include <stdlib.h>

#ifndef CCLAW_FREERTOS_TASK_STACK_WORDS
#define CCLAW_FREERTOS_TASK_STACK_WORDS 2048
#endif

typedef struct cc_freertos_thread {
    cc_thread_fn_t fn;
    void *arg;
    SemaphoreHandle_t done;
} cc_freertos_thread_t;

static void cc_freertos_thread_entry(void *arg)
{
    cc_freertos_thread_t *thread = (cc_freertos_thread_t *)arg;
    (void)thread->fn(thread->arg);
    xSemaphoreGive(thread->done);
    vTaskDelete(NULL);
}

cc_result_t cc_thread_create(cc_thread_fn_t fn, void *arg, cc_thread_t *out_thread)
{
    if (!fn || !out_thread)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid thread arguments");

    cc_freertos_thread_t *thread = calloc(1, sizeof(cc_freertos_thread_t));
    if (!thread) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate thread");

    thread->fn = fn;
    thread->arg = arg;
    thread->done = xSemaphoreCreateBinary();
    if (!thread->done) {
        free(thread);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create thread semaphore");
    }

    BaseType_t ok = xTaskCreate(
        cc_freertos_thread_entry,
        "cclaw",
        CCLAW_FREERTOS_TASK_STACK_WORDS,
        thread,
        tskIDLE_PRIORITY + 1,
        NULL);
    if (ok != pdPASS) {
        vSemaphoreDelete(thread->done);
        free(thread);
        return cc_result_error(CC_ERR_PLATFORM, "Failed to create FreeRTOS task");
    }

    *out_thread = thread;
    return cc_result_ok();
}

cc_result_t cc_thread_join(cc_thread_t handle)
{
    cc_freertos_thread_t *thread = (cc_freertos_thread_t *)handle;
    if (!thread) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid thread handle");
    xSemaphoreTake(thread->done, portMAX_DELAY);
    vSemaphoreDelete(thread->done);
    free(thread);
    return cc_result_ok();
}

cc_result_t cc_mutex_create(cc_mutex_t *out_mutex)
{
    if (!out_mutex) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid mutex output");
    SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
    if (!mutex) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create mutex");
    *out_mutex = mutex;
    return cc_result_ok();
}

void cc_mutex_destroy(cc_mutex_t mutex)
{
    if (mutex) vSemaphoreDelete((SemaphoreHandle_t)mutex);
}

void cc_mutex_lock(cc_mutex_t mutex)
{
    if (mutex) xSemaphoreTake((SemaphoreHandle_t)mutex, portMAX_DELAY);
}

void cc_mutex_unlock(cc_mutex_t mutex)
{
    if (mutex) xSemaphoreGive((SemaphoreHandle_t)mutex);
}

cc_result_t cc_cond_create(cc_cond_t *out_cond)
{
    if (!out_cond) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid condition output");
    SemaphoreHandle_t cond = xSemaphoreCreateBinary();
    if (!cond) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create condition semaphore");
    *out_cond = cond;
    return cc_result_ok();
}

void cc_cond_destroy(cc_cond_t cond)
{
    if (cond) vSemaphoreDelete((SemaphoreHandle_t)cond);
}

void cc_cond_wait(cc_cond_t cond, cc_mutex_t mutex)
{
    if (!cond || !mutex) return;
    xSemaphoreGive((SemaphoreHandle_t)mutex);
    xSemaphoreTake((SemaphoreHandle_t)cond, portMAX_DELAY);
    xSemaphoreTake((SemaphoreHandle_t)mutex, portMAX_DELAY);
}

int cc_cond_timedwait(cc_cond_t cond, cc_mutex_t mutex, int timeout_ms)
{
    if (!cond || !mutex) return 0;
    TickType_t ticks = timeout_ms > 0 ? pdMS_TO_TICKS(timeout_ms) : portMAX_DELAY;
    xSemaphoreGive((SemaphoreHandle_t)mutex);
    BaseType_t ok = xSemaphoreTake((SemaphoreHandle_t)cond, ticks);
    xSemaphoreTake((SemaphoreHandle_t)mutex, portMAX_DELAY);
    return ok == pdTRUE ? 1 : 0;
}

void cc_cond_signal(cc_cond_t cond)
{
    if (cond) xSemaphoreGive((SemaphoreHandle_t)cond);
}

void cc_cond_broadcast(cc_cond_t cond)
{
    if (cond) xSemaphoreGive((SemaphoreHandle_t)cond);
}
