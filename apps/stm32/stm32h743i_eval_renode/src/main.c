#include "FreeRTOS.h"
#include "task.h"

#include "board.h"
#include "cc/ports/cc_thread.h"
#include "cc/util/cc_json.h"
#include "http_smoke.h"
#include "network.h"
#include "real_llm_smoke.h"
#include "uart_chat.h"

#include <stdint.h>
#include <string.h>

#ifndef CCLAW_STM32H743_HTTP_SMOKE_URL
#define CCLAW_STM32H743_HTTP_SMOKE_URL "http://10.0.2.2:8080/smoke"
#endif
#ifndef CCLAW_STM32H743_ENABLE_HTTP_SMOKE
#define CCLAW_STM32H743_ENABLE_HTTP_SMOKE 0
#endif
#ifndef CCLAW_STM32H743_ENABLE_REAL_LLM
#define CCLAW_STM32H743_ENABLE_REAL_LLM 0
#endif
#ifndef CCLAW_STM32H743_ENABLE_UART_CHAT
#define CCLAW_STM32H743_ENABLE_UART_CHAT 0
#endif

static void pass(const char *name)
{
    board_uart_write("[pass] ");
    board_uart_write(name);
    board_uart_write("\n");
}

static void fail(const char *name, cc_result_t rc)
{
    board_uart_write("[fail] ");
    board_uart_write(name);
    board_uart_write(": ");
    board_uart_write(rc.message ? rc.message : cc_error_string(rc.code));
    board_uart_write("\n");
    cc_result_free(&rc);
}

static void *thread_probe(void *arg)
{
    int *value = (int *)arg;
    *value = 743;
    return NULL;
}

static int run_smoke(void)
{
    cc_json_value_t *root = NULL;
    cc_result_t rc = cc_json_parse("{\"board\":\"stm32h743i-eval\",\"renode\":true}", &root);
    if (rc.code != CC_OK) {
        fail("json_parse", rc);
        return 0;
    }

    const char *board = cc_json_string_value(cc_json_object_get(root, "board"));
    if (!board || strcmp(board, "stm32h743i-eval") != 0) {
        cc_json_destroy(root);
        board_uart_write("[fail] json_board\n");
        return 0;
    }
    cc_json_destroy(root);
    pass("json");

    int value = 0;
    cc_thread_t thread = NULL;
    rc = cc_thread_create(thread_probe, &value, &thread);
    if (rc.code != CC_OK) {
        fail("thread_create", rc);
        return 0;
    }

    rc = cc_thread_join(thread);
    if (rc.code != CC_OK) {
        fail("thread_join", rc);
        return 0;
    }
    if (value != 743) {
        board_uart_write("[fail] thread_value\n");
        return 0;
    }
    pass("freertos_thread");

    board_uart_write("CCLAW_STM32H743_RENODE_PASS\n");
    return 1;
}

#if CCLAW_STM32H743_ENABLE_HTTP_SMOKE || CCLAW_STM32H743_ENABLE_REAL_LLM || CCLAW_STM32H743_ENABLE_UART_CHAT
static void ethernet_poll_task(void *arg)
{
    (void)arg;
    for (;;) {
        network_poll();
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
#endif

static void net_task(void *arg)
{
    const char *url = (const char *)arg;
    board_uart_write("[init] network task start\n");
    if (network_init_static()) {
#if CCLAW_STM32H743_ENABLE_HTTP_SMOKE
        xTaskCreate(ethernet_poll_task, "ethrx", 1024, NULL, tskIDLE_PRIORITY + 2, NULL);
        vTaskDelay(pdMS_TO_TICKS(100));
        http_smoke_run(url);
#elif CCLAW_STM32H743_ENABLE_UART_CHAT
        (void)url;
        xTaskCreate(ethernet_poll_task, "ethrx", 1024, NULL, tskIDLE_PRIORITY + 2, NULL);
        vTaskDelay(pdMS_TO_TICKS(250));
        uart_chat_run();
#elif CCLAW_STM32H743_ENABLE_REAL_LLM
        (void)url;
        xTaskCreate(ethernet_poll_task, "ethrx", 1024, NULL, tskIDLE_PRIORITY + 2, NULL);
        vTaskDelay(pdMS_TO_TICKS(250));
        real_llm_smoke_run();
#else
        (void)url;
#endif
    }
    for (;;) {
        network_poll();
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

static void app_task(void *arg)
{
    const char *url = (const char *)arg;
    board_uart_write("c-claw STM32H743I-EVAL Renode FreeRTOS/lwIP/HAL smoke\n");
    (void)run_smoke();
    xTaskCreate(net_task, "net", 4096, (void *)url, tskIDLE_PRIORITY + 1, NULL);
    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}

int main(void)
{
    board_init();
    xTaskCreate(app_task, "app", 2048, CCLAW_STM32H743_HTTP_SMOKE_URL, tskIDLE_PRIORITY + 3, NULL);
    vTaskStartScheduler();
    board_uart_write("[fail] scheduler_returned\n");
    for (;;) {
    }
}

void vApplicationMallocFailedHook(void)
{
    board_uart_write("[fail] malloc\n");
    taskDISABLE_INTERRUPTS();
    for (;;) {
    }
}

void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
    (void)task;
    board_uart_write("[fail] stack_overflow ");
    board_uart_write(name ? name : "?");
    board_uart_write("\n");
    taskDISABLE_INTERRUPTS();
    for (;;) {
    }
}
