#ifndef CCLAW_STM32H743_BOARD_STORAGE_H
#define CCLAW_STM32H743_BOARD_STORAGE_H

#include <stdint.h>

#define CCLAW_STM32H743_WORKSPACE "/sdcard/cclaw/workspace"

int board_storage_mount(void);
int board_storage_is_mounted(void);

int board_sd_init(void);
int board_sd_read_blocks(uint8_t *buffer, uint32_t sector, uint32_t count);
int board_sd_write_blocks(const uint8_t *buffer, uint32_t sector, uint32_t count);
int board_sd_get_sector_count(uint32_t *out_count);
int board_sd_get_sector_size(uint32_t *out_size);
int board_sd_get_block_size(uint32_t *out_size);
int board_sd_is_ready(void);

#endif
