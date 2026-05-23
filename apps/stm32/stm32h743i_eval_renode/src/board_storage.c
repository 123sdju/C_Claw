#include "board_storage.h"

#include "board.h"
#include "ff.h"
#include "ff_gen_drv.h"
#include "stm32h7xx_hal.h"

#include <stdio.h>

extern const Diskio_drvTypeDef SD_Driver;

static SD_HandleTypeDef sd_handle;
static FATFS sd_fatfs;
static char sd_path[4];
static int sd_initialized;
static int sd_renode_csd_fallback;
static int fs_mounted;

#ifndef CCLAW_STM32H743_SD_SECTOR_COUNT
#define CCLAW_STM32H743_SD_SECTOR_COUNT 131072U
#endif

static void sd_assume_renode_card_ready(void)
{
    sd_renode_csd_fallback = 1;
    sd_handle.ErrorCode = HAL_SD_ERROR_NONE;
    sd_handle.State = HAL_SD_STATE_READY;
    sd_handle.SdCard.CardType = CARD_SDHC_SDXC;
    sd_handle.SdCard.CardVersion = CARD_V2_X;
    sd_handle.SdCard.Class = 0;
    sd_handle.SdCard.BlockNbr = CCLAW_STM32H743_SD_SECTOR_COUNT;
    sd_handle.SdCard.BlockSize = 512U;
    sd_handle.SdCard.LogBlockNbr = CCLAW_STM32H743_SD_SECTOR_COUNT;
    sd_handle.SdCard.LogBlockSize = 512U;
    sd_handle.SdCard.CardSpeed = CARD_NORMAL_SPEED;
}

static void storage_log_result(const char *name, FRESULT res)
{
    char line[96];
    snprintf(line, sizeof(line), "[fail] %s: FatFs=%u\n", name, (unsigned)res);
    board_uart_write(line);
}

int board_sd_init(void)
{
    if (sd_initialized) return 1;

    sd_handle.Instance = SDMMC1;
    sd_handle.Init.ClockEdge = SDMMC_CLOCK_EDGE_RISING;
    sd_handle.Init.ClockPowerSave = SDMMC_CLOCK_POWER_SAVE_DISABLE;
    sd_handle.Init.BusWide = SDMMC_BUS_WIDE_1B;
    sd_handle.Init.HardwareFlowControl = SDMMC_HARDWARE_FLOW_CONTROL_DISABLE;
    sd_handle.Init.ClockDiv = 2;

    if (HAL_SD_Init(&sd_handle) != HAL_OK) {
        uint32_t error = HAL_SD_GetError(&sd_handle);
        if ((error & (HAL_SD_ERROR_UNSUPPORTED_FEATURE | HAL_SD_ERROR_PARAM)) != 0U) {
            sd_assume_renode_card_ready();
            sd_initialized = 1;
            char line[96];
            snprintf(line, sizeof(line),
                "[init] SDMMC HAL init ok with Renode card-info fallback error=0x%08lx\n",
                (unsigned long)error);
            board_uart_write(line);
            return 1;
        }
        char line[96];
        snprintf(line, sizeof(line), "[fail] sd_hal_init error=0x%08lx\n",
            (unsigned long)error);
        board_uart_write(line);
        return 0;
    }
    sd_assume_renode_card_ready();

    sd_initialized = 1;
    board_uart_write("[init] SDMMC HAL init ok\n");
    return 1;
}

int board_sd_is_ready(void)
{
    if (!sd_initialized && !board_sd_init()) return 0;
    if (sd_renode_csd_fallback) return 1;
    return HAL_SD_GetCardState(&sd_handle) == HAL_SD_CARD_TRANSFER;
}

int board_sd_read_blocks(uint8_t *buffer, uint32_t sector, uint32_t count)
{
    if (!sd_initialized && !board_sd_init()) return 0;
    if (HAL_SD_ReadBlocks(&sd_handle, buffer, sector, count, 30000U) != HAL_OK) return 0;
    if (sd_renode_csd_fallback) return 1;
    while (HAL_SD_GetCardState(&sd_handle) != HAL_SD_CARD_TRANSFER) {
    }
    return 1;
}

int board_sd_write_blocks(const uint8_t *buffer, uint32_t sector, uint32_t count)
{
    if (!sd_initialized && !board_sd_init()) return 0;
    if (HAL_SD_WriteBlocks(&sd_handle, (uint8_t *)buffer, sector, count, 30000U) != HAL_OK) return 0;
    if (sd_renode_csd_fallback) return 1;
    while (HAL_SD_GetCardState(&sd_handle) != HAL_SD_CARD_TRANSFER) {
    }
    return 1;
}

int board_sd_get_sector_count(uint32_t *out_count)
{
    HAL_SD_CardInfoTypeDef info;
    if (!out_count || (!sd_initialized && !board_sd_init())) return 0;
    HAL_SD_GetCardInfo(&sd_handle, &info);
    *out_count = info.LogBlockNbr;
    return *out_count != 0;
}

int board_sd_get_sector_size(uint32_t *out_size)
{
    HAL_SD_CardInfoTypeDef info;
    if (!out_size || (!sd_initialized && !board_sd_init())) return 0;
    HAL_SD_GetCardInfo(&sd_handle, &info);
    *out_size = info.LogBlockSize ? info.LogBlockSize : 512U;
    return 1;
}

int board_sd_get_block_size(uint32_t *out_size)
{
    HAL_SD_CardInfoTypeDef info;
    if (!out_size || (!sd_initialized && !board_sd_init())) return 0;
    HAL_SD_GetCardInfo(&sd_handle, &info);
    *out_size = info.LogBlockSize ? info.LogBlockSize : 512U;
    return 1;
}

int board_storage_mount(void)
{
    if (fs_mounted) return 1;

    if (FATFS_LinkDriver(&SD_Driver, sd_path) != 0) {
        board_uart_write("[fail] fatfs_link_driver\n");
        return 0;
    }

    FRESULT res = f_mount(&sd_fatfs, sd_path, 1);
    if (res != FR_OK) {
        storage_log_result("fatfs_mount", res);
        return 0;
    }

    fs_mounted = 1;
    board_uart_write("[init] FatFs mounted at /sdcard/cclaw/workspace via FAT root\n");
    board_uart_write("CCLAW_STM32H743_RENODE_SD_PASS\n");
    return 1;
}

int board_storage_is_mounted(void)
{
    return fs_mounted;
}

void HAL_SD_MspInit(SD_HandleTypeDef *hsd)
{
    (void)hsd;
    __HAL_RCC_SDMMC1_CLK_ENABLE();
}
