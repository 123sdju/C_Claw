#ifndef STM32H7XX_HAL_CONF_H
#define STM32H7XX_HAL_CONF_H

#define HAL_MODULE_ENABLED
#define HAL_CORTEX_MODULE_ENABLED
#define HAL_DMA_MODULE_ENABLED
#define HAL_ETH_MODULE_ENABLED
#define HAL_FLASH_MODULE_ENABLED
#define HAL_GPIO_MODULE_ENABLED
#define HAL_PWR_MODULE_ENABLED
#define HAL_RCC_MODULE_ENABLED
#define HAL_SD_MODULE_ENABLED
#define HAL_UART_MODULE_ENABLED

#define HSE_VALUE 25000000UL
#define HSI_VALUE 64000000UL
#define CSI_VALUE 4000000UL
#define LSI_VALUE 32000UL
#define LSE_VALUE 32768UL
#define HSE_STARTUP_TIMEOUT 100UL
#define LSE_STARTUP_TIMEOUT 5000UL
#define EXTERNAL_CLOCK_VALUE 12288000UL
#define VDD_VALUE 3300UL
#define TICK_INT_PRIORITY 0x0FUL
#define USE_RTOS 0U
#define PREFETCH_ENABLE 0U
#define USE_HAL_ETH_REGISTER_CALLBACKS 0U

#define ETH_TX_DESC_CNT 4
#define ETH_RX_DESC_CNT 4
#define ETH_MAC_ADDR0 0x02
#define ETH_MAC_ADDR1 0x00
#define ETH_MAC_ADDR2 0x00
#define ETH_MAC_ADDR3 0x12
#define ETH_MAC_ADDR4 0x34
#define ETH_MAC_ADDR5 0x56

#include "stm32h7xx_hal_rcc.h"
#include "stm32h7xx_hal_gpio.h"
#include "stm32h7xx_hal_dma.h"
#include "stm32h7xx_hal_cortex.h"
#include "stm32h7xx_hal_pwr.h"
#include "stm32h7xx_hal_flash.h"
#include "stm32h7xx_hal_sd.h"
#include "stm32h7xx_hal_eth.h"
#include "stm32h7xx_hal_uart.h"

#ifndef assert_param
#define assert_param(expr) ((void)0U)
#endif

#ifdef  USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line);
#endif

#endif
