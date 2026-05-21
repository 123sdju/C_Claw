/**
 * 学习导读：cclaw/platforms/esp32/src/cc_esp32_thread.c
 *
 * 所属层次：平台层。
 * 阅读重点：这里隐藏 POSIX、Windows、ESP32 的系统 API 差异，阅读时重点看同名端口函数如何按平台实现。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

#include "cc/ports/cc_thread.h"

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <stdlib.h>

/**
 * cc_esp32_thread — FreeRTOS task join 包装。
 *
 * FreeRTOS task 本身没有 pthread_join 等价物，因此 wrapper 持有一个 binary
 * semaphore。task 结束前 give，cc_thread_join take 后释放 wrapper。
 */
typedef struct cc_esp32_thread {
    cc_thread_fn_t fn;
    void *arg;
    SemaphoreHandle_t done;
} cc_esp32_thread_t;

/**
 * cc_esp32_thread_entry — FreeRTOS task trampoline。
 *
 * arg 是 cc_esp32_thread_t*，由 cc_thread_create 分配并由 join 释放。入口函数
 * 执行完后通知 done semaphore，再删除当前 task。
 */
static void cc_esp32_thread_entry(void *arg)
{
    cc_esp32_thread_t *thread = (cc_esp32_thread_t *)arg;
    thread->fn(thread->arg);
    xSemaphoreGive(thread->done);
    vTaskDelete(NULL);
}

/**
 * cc_thread_create — 用 FreeRTOS task 实现 cc_thread_t。
 *
 * wrapper 拥有 done semaphore，但不拥有 arg；arg 生命周期仍由调用方或上层 job
 * 对象管理。栈大小固定为 8192 字节，适合当前 QEMU smoke 和轻量 SDK 任务。
 */
cc_result_t cc_thread_create(cc_thread_fn_t fn, void *arg, cc_thread_t *out_thread)
{
    if (!fn || !out_thread)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid thread arguments");

    cc_esp32_thread_t *thread = calloc(1, sizeof(cc_esp32_thread_t));
    if (!thread) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate thread");

    thread->fn = fn;
    thread->arg = arg;
    thread->done = xSemaphoreCreateBinary();
    if (!thread->done) {
        free(thread);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create thread semaphore");
    }

    BaseType_t ok = xTaskCreate(
        cc_esp32_thread_entry,
        "cclaw_thread",
        8192,
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

/**
 * cc_thread_join — 等待平台线程结束，并把底层 join 错误转换为统一结果。
 *
 * @param handle 按值传入，用于控制本次操作。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
cc_result_t cc_thread_join(cc_thread_t handle)
{
    cc_esp32_thread_t *thread = (cc_esp32_thread_t *)handle;
    if (!thread) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid thread handle");
    xSemaphoreTake(thread->done, portMAX_DELAY);
    vSemaphoreDelete(thread->done);
    free(thread);
    return cc_result_ok();
}

/**
 * cc_mutex_create — 完成对应初始化步骤，失败时返回 cc_result_t 错误。
 *
 * @param out_mutex 输出参数；调用方传入有效指针，成功后接收结果。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
cc_result_t cc_mutex_create(cc_mutex_t *out_mutex)
{
    if (!out_mutex) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid mutex output");
    SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
    if (!mutex) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create mutex");
    *out_mutex = mutex;
    return cc_result_ok();
}

/**
 * cc_mutex_destroy — 释放、停止或复位该组件拥有的资源，防止失败路径泄漏。
 *
 * @param mutex 借用的对象；函数不释放该对象本身。
 */
void cc_mutex_destroy(cc_mutex_t mutex)
{
    if (mutex) vSemaphoreDelete((SemaphoreHandle_t)mutex);
}

/**
 * cc_mutex_lock — 获取 ESP32 FreeRTOS mutex，进入临界区。
 *
 * @param mutex 借用互斥锁句柄；NULL 时函数直接返回。
 */
void cc_mutex_lock(cc_mutex_t mutex)
{
    if (mutex) xSemaphoreTake((SemaphoreHandle_t)mutex, portMAX_DELAY);
}

/**
 * cc_mutex_unlock — 离开平台互斥锁临界区，让其他线程继续访问共享状态。
 *
 * @param mutex 借用的对象；函数不释放该对象本身。
 */
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
#else
#error "cc_esp32_thread.c must be built under ESP-IDF"
#endif
