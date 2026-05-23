#include "board_storage.h"

#include "diskio.h"
#include "ff_gen_drv.h"

#define SD_DEFAULT_BLOCK_SIZE 512U

static volatile DSTATUS sd_status_flags = STA_NOINIT;

static DSTATUS sd_initialize(BYTE lun)
{
    (void)lun;
    if (board_sd_init()) {
        sd_status_flags &= (DSTATUS)~STA_NOINIT;
    } else {
        sd_status_flags = STA_NOINIT;
    }
    return sd_status_flags;
}

static DSTATUS sd_status(BYTE lun)
{
    (void)lun;
    if (board_sd_is_ready()) {
        sd_status_flags &= (DSTATUS)~STA_NOINIT;
    } else {
        sd_status_flags = STA_NOINIT;
    }
    return sd_status_flags;
}

static DRESULT sd_read(BYTE lun, BYTE *buffer, DWORD sector, UINT count)
{
    (void)lun;
    if (!buffer || count == 0) return RES_PARERR;
    return board_sd_read_blocks(buffer, (uint32_t)sector, (uint32_t)count) ? RES_OK : RES_ERROR;
}

#if _USE_WRITE == 1
static DRESULT sd_write(BYTE lun, const BYTE *buffer, DWORD sector, UINT count)
{
    (void)lun;
    if (!buffer || count == 0) return RES_PARERR;
    return board_sd_write_blocks(buffer, (uint32_t)sector, (uint32_t)count) ? RES_OK : RES_ERROR;
}
#endif

#if _USE_IOCTL == 1
static DRESULT sd_ioctl(BYTE lun, BYTE cmd, void *buffer)
{
    (void)lun;
    if (!buffer && cmd != CTRL_SYNC) return RES_PARERR;
    if (sd_status(0) & STA_NOINIT) return RES_NOTRDY;

    uint32_t value = 0;
    switch (cmd) {
    case CTRL_SYNC:
        return RES_OK;
    case GET_SECTOR_COUNT:
        if (!board_sd_get_sector_count(&value)) return RES_ERROR;
        *(DWORD *)buffer = (DWORD)value;
        return RES_OK;
    case GET_SECTOR_SIZE:
        if (!board_sd_get_sector_size(&value)) return RES_ERROR;
        *(WORD *)buffer = (WORD)value;
        return RES_OK;
    case GET_BLOCK_SIZE:
        if (!board_sd_get_block_size(&value)) value = SD_DEFAULT_BLOCK_SIZE;
        *(DWORD *)buffer = (DWORD)(value / SD_DEFAULT_BLOCK_SIZE);
        return RES_OK;
    default:
        return RES_PARERR;
    }
}
#endif

const Diskio_drvTypeDef SD_Driver = {
    sd_initialize,
    sd_status,
    sd_read,
#if _USE_WRITE == 1
    sd_write,
#endif
#if _USE_IOCTL == 1
    sd_ioctl,
#endif
};
