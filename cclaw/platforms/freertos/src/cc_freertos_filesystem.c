/**
 * 学习导读：cclaw/platforms/freertos/src/cc_freertos_filesystem.c
 * 所属层次：平台层。
 * 阅读重点：裸机 FreeRTOS 默认没有文件系统；这里提供显式 unsupported 实现，避免静默写入。
 */

#include "cc/ports/cc_filesystem.h"

#include <stdlib.h>

static cc_result_t unsupported_read(void *self, const char *path, char **out_text)
{
    (void)self;
    (void)path;
    if (out_text) *out_text = NULL;
    return cc_result_error(CC_ERR_PLATFORM, "FreeRTOS filesystem is not mounted");
}

static cc_result_t unsupported_write(void *self, const char *path, const char *text)
{
    (void)self;
    (void)path;
    (void)text;
    return cc_result_error(CC_ERR_PLATFORM, "FreeRTOS filesystem is not mounted");
}

static cc_result_t unsupported_exists(void *self, const char *path, int *out_exists)
{
    (void)self;
    (void)path;
    if (out_exists) *out_exists = 0;
    return cc_result_ok();
}

static cc_result_t unsupported_list(void *self, const char *path, char ***out_items, size_t *out_count)
{
    (void)self;
    (void)path;
    if (out_items) *out_items = NULL;
    if (out_count) *out_count = 0;
    return cc_result_error(CC_ERR_PLATFORM, "FreeRTOS filesystem is not mounted");
}

static cc_result_t unsupported_make_dir(void *self, const char *path)
{
    (void)self;
    (void)path;
    return cc_result_error(CC_ERR_PLATFORM, "FreeRTOS filesystem is not mounted");
}

static cc_result_t unsupported_remove(void *self, const char *path)
{
    (void)self;
    (void)path;
    return cc_result_error(CC_ERR_PLATFORM, "FreeRTOS filesystem is not mounted");
}

static void unsupported_destroy(void *self)
{
    free(self);
}

static cc_filesystem_vtable_t freertos_fs_vtable = {
    unsupported_read,
    unsupported_write,
    unsupported_exists,
    unsupported_list,
    unsupported_make_dir,
    unsupported_remove,
    unsupported_destroy
};

cc_result_t cc_filesystem_get_default(cc_filesystem_t *out_fs)
{
    if (!out_fs) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid filesystem output");
    int *dummy = calloc(1, sizeof(int));
    if (!dummy) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate filesystem");
    out_fs->self = dummy;
    out_fs->vtable = &freertos_fs_vtable;
    return cc_result_ok();
}

cc_result_t cc_filesystem_get_posix(cc_filesystem_t *out_fs)
{
    return cc_filesystem_get_default(out_fs);
}
