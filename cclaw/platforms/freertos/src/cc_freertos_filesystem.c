#include "cc/ports/cc_filesystem.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#if defined(CCLAW_FREERTOS_ENABLE_FATFS) && CCLAW_FREERTOS_ENABLE_FATFS

#include "ff.h"

typedef struct cc_freertos_fatfs {
    int unused;
} cc_freertos_fatfs_t;

static const char *const k_mount_prefix = "/sdcard";
static const char *const k_workspace_prefix = "/sdcard/cclaw/workspace";

static cc_result_t map_path(const char *path, char *out, size_t out_len)
{
    if (!path || !out || out_len == 0) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid filesystem path");
    }

    const char *rel = path;
    const char *fat_prefix = "0:/";
    size_t workspace_len = strlen(k_workspace_prefix);
    if (strncmp(path, k_workspace_prefix, workspace_len) == 0 &&
        (path[workspace_len] == '\0' || path[workspace_len] == '/')) {
        rel = path + workspace_len;
        while (*rel == '/') rel++;
        int n = *rel
            ? snprintf(out, out_len, "0:/%s", rel)
            : snprintf(out, out_len, "0:/");
        if (n < 0 || (size_t)n >= out_len) {
            return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Filesystem path is too long");
        }
        return cc_result_ok();
    }

    size_t prefix_len = strlen(k_mount_prefix);
    if (strncmp(path, k_mount_prefix, prefix_len) == 0) {
        rel = path + prefix_len;
    }
    while (*rel == '/') rel++;

    int n = *rel
        ? snprintf(out, out_len, "%s%s", fat_prefix, rel)
        : snprintf(out, out_len, "0:/");
    if (n < 0 || (size_t)n >= out_len) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Filesystem path is too long");
    }
    return cc_result_ok();
}

static cc_result_t ensure_parent_dir(const char *fpath)
{
    char parent[256];
    size_t len = strlen(fpath);
    if (len >= sizeof(parent)) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Directory path is too long");
    strcpy(parent, fpath);

    char *slash = strrchr(parent + 3, '/');
    if (!slash) return cc_result_ok();
    *slash = '\0';

    for (char *p = parent + 3; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            FRESULT fr = f_mkdir(parent);
            if (fr != FR_OK && fr != FR_EXIST) return cc_result_error(CC_ERR_PLATFORM, "FatFs parent mkdir failed");
            *p = '/';
        }
    }
    FRESULT fr = f_mkdir(parent);
    if (fr != FR_OK && fr != FR_EXIST) return cc_result_error(CC_ERR_PLATFORM, "FatFs parent mkdir failed");
    return cc_result_ok();
}

static cc_result_t fatfs_error(FRESULT res, const char *op)
{
    char msg[96];
    snprintf(msg, sizeof(msg), "%s failed: FatFs=%u", op, (unsigned)res);
    return cc_result_error(CC_ERR_PLATFORM, msg);
}

static cc_result_t fatfs_read(void *self, const char *path, char **out_text)
{
    (void)self;
    if (out_text) *out_text = NULL;

    char fpath[256];
    cc_result_t rc = map_path(path, fpath, sizeof(fpath));
    if (rc.code != CC_OK) return rc;

    FIL file;
    FRESULT fr = f_open(&file, fpath, FA_READ);
    if (fr != FR_OK) return fatfs_error(fr, "f_open");

    FSIZE_t size = f_size(&file);
    if (size > 256U * 1024U) {
        f_close(&file);
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "File is too large for STM32 text read");
    }

    char *text = malloc((size_t)size + 1U);
    if (!text) {
        f_close(&file);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate file buffer");
    }

    UINT got = 0;
    fr = f_read(&file, text, (UINT)size, &got);
    f_close(&file);
    if (fr != FR_OK) {
        free(text);
        return fatfs_error(fr, "f_read");
    }
    text[got] = '\0';
    if (out_text) *out_text = text;
    else free(text);
    return cc_result_ok();
}

static cc_result_t fatfs_write(void *self, const char *path, const char *text)
{
    (void)self;
    char fpath[256];
    cc_result_t rc = map_path(path, fpath, sizeof(fpath));
    if (rc.code != CC_OK) return rc;

    FIL file;
    FRESULT fr = f_open(&file, fpath, FA_CREATE_ALWAYS | FA_WRITE);
    if (fr == FR_NO_PATH) {
        rc = ensure_parent_dir(fpath);
        if (rc.code != CC_OK) return rc;
        fr = f_open(&file, fpath, FA_CREATE_ALWAYS | FA_WRITE);
    }
    if (fr != FR_OK) return fatfs_error(fr, "f_open");

    UINT wrote = 0;
    size_t len = text ? strlen(text) : 0;
    fr = f_write(&file, text ? text : "", (UINT)len, &wrote);
    FRESULT close_fr = f_close(&file);
    if (fr != FR_OK) return fatfs_error(fr, "f_write");
    if (close_fr != FR_OK) return fatfs_error(close_fr, "f_close");
    if (wrote != len) return cc_result_error(CC_ERR_PLATFORM, "Short FatFs write");
    return cc_result_ok();
}

static cc_result_t fatfs_exists(void *self, const char *path, int *out_exists)
{
    (void)self;
    if (out_exists) *out_exists = 0;

    char fpath[256];
    cc_result_t rc = map_path(path, fpath, sizeof(fpath));
    if (rc.code != CC_OK) return rc;

    FILINFO info;
    FRESULT fr = f_stat(fpath, &info);
    if (fr == FR_OK) {
        if (out_exists) *out_exists = 1;
        return cc_result_ok();
    }
    if (fr == FR_NO_FILE || fr == FR_NO_PATH) return cc_result_ok();
    return fatfs_error(fr, "f_stat");
}

static cc_result_t fatfs_list(void *self, const char *path, char ***out_items, size_t *out_count)
{
    (void)self;
    if (out_items) *out_items = NULL;
    if (out_count) *out_count = 0;

    char fpath[256];
    cc_result_t rc = map_path(path, fpath, sizeof(fpath));
    if (rc.code != CC_OK) return rc;

    DIR dir;
    FRESULT fr = f_opendir(&dir, fpath);
    if (fr != FR_OK) return fatfs_error(fr, "f_opendir");

    char **items = NULL;
    size_t count = 0;
    for (;;) {
        FILINFO info;
        fr = f_readdir(&dir, &info);
        if (fr != FR_OK || info.fname[0] == '\0') break;
        char **next = realloc(items, sizeof(char *) * (count + 1));
        if (!next) {
            fr = FR_NOT_ENOUGH_CORE;
            break;
        }
        items = next;
        items[count] = strdup(info.fname);
        if (!items[count]) {
            fr = FR_NOT_ENOUGH_CORE;
            break;
        }
        count++;
    }
    f_closedir(&dir);

    if (fr != FR_OK) {
        for (size_t i = 0; i < count; i++) free(items[i]);
        free(items);
        return fatfs_error(fr, "f_readdir");
    }

    if (out_items) *out_items = items;
    else {
        for (size_t i = 0; i < count; i++) free(items[i]);
        free(items);
    }
    if (out_count) *out_count = count;
    return cc_result_ok();
}

static cc_result_t fatfs_make_dir(void *self, const char *path)
{
    (void)self;
    char fpath[256];
    cc_result_t rc = map_path(path, fpath, sizeof(fpath));
    if (rc.code != CC_OK) return rc;

    char partial[256];
    size_t len = strlen(fpath);
    if (len >= sizeof(partial)) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Directory path is too long");
    strcpy(partial, fpath);

    for (char *p = partial + 3; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            FRESULT fr = f_mkdir(partial);
            if (fr != FR_OK && fr != FR_EXIST) return fatfs_error(fr, "f_mkdir");
            *p = '/';
        }
    }
    FRESULT fr = f_mkdir(partial);
    if (fr != FR_OK && fr != FR_EXIST) return fatfs_error(fr, "f_mkdir");
    return cc_result_ok();
}

static cc_result_t fatfs_remove(void *self, const char *path)
{
    (void)self;
    char fpath[256];
    cc_result_t rc = map_path(path, fpath, sizeof(fpath));
    if (rc.code != CC_OK) return rc;
    FRESULT fr = f_unlink(fpath);
    if (fr != FR_OK) return fatfs_error(fr, "f_unlink");
    return cc_result_ok();
}

static void fatfs_destroy(void *self)
{
    free(self);
}

static cc_filesystem_vtable_t freertos_fs_vtable = {
    fatfs_read,
    fatfs_write,
    fatfs_exists,
    fatfs_list,
    fatfs_make_dir,
    fatfs_remove,
    fatfs_destroy
};

#else

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

#endif

cc_result_t cc_filesystem_get_default(cc_filesystem_t *out_fs)
{
    if (!out_fs) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid filesystem output");
#if defined(CCLAW_FREERTOS_ENABLE_FATFS) && CCLAW_FREERTOS_ENABLE_FATFS
    cc_freertos_fatfs_t *self = calloc(1, sizeof(*self));
#else
    int *self = calloc(1, sizeof(int));
#endif
    if (!self) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate filesystem");
    out_fs->self = self;
    out_fs->vtable = &freertos_fs_vtable;
    return cc_result_ok();
}

cc_result_t cc_filesystem_get_posix(cc_filesystem_t *out_fs)
{
    return cc_filesystem_get_default(out_fs);
}
