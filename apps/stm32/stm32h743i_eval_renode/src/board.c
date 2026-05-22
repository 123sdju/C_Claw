#include "board.h"

#include "FreeRTOS.h"
#include "task.h"
#include "stm32h7xx_hal.h"

void renode_uart_init(void);
void renode_uart_write(const char *text);
int renode_uart_getc_nonblocking(char *out);

void board_init(void)
{
    HAL_Init();
    renode_uart_init();
    renode_uart_write("[init] STM32 HAL init ok\n");
}

void board_uart_write(const char *text)
{
    renode_uart_write(text);
}

int board_uart_getc_nonblocking(char *out)
{
    return renode_uart_getc_nonblocking(out);
}

HAL_StatusTypeDef HAL_InitTick(uint32_t TickPriority)
{
    (void)TickPriority;
    return HAL_OK;
}

uint32_t HAL_GetTick(void)
{
    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
        return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    }
    return 0;
}

void HAL_Delay(uint32_t Delay)
{
    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
        vTaskDelay(pdMS_TO_TICKS(Delay));
    }
}

void HAL_ETH_MspInit(ETH_HandleTypeDef *heth)
{
    (void)heth;
    __HAL_RCC_ETH1MAC_CLK_ENABLE();
    __HAL_RCC_ETH1TX_CLK_ENABLE();
    __HAL_RCC_ETH1RX_CLK_ENABLE();
}

void HAL_UART_MspInit(UART_HandleTypeDef *huart)
{
    (void)huart;
}
