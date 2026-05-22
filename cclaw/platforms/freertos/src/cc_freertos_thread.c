/**
 * 学习导读：cclaw/platforms/freertos/src/cc_freertos_thread.c
 * 所属层次：平台层。
 * 阅读重点：这里用 FreeRTOS task/semaphore 实现 c-claw 的线程和条件变量端口。
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
