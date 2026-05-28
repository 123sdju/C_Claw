#include "cc/ports/cc_filesystem.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#if defined(CCLAW_FREERTOS_ENABLE_FATFS) && CCLAW_FREERTOS_ENABLE_FATFS

#include "ff.h"

/* FatFs filesystem 私有对象；当前无额外状态，保留 self 用于统一 vtable 生命周期。 */
typedef struct cc_freertos_fatfs {
    int unused;
} cc_freertos_fatfs_t;

static const char *const k_mount_prefix = "/sdcard";
static const char *const k_workspace_prefix = "/sdcard/cclaw/workspace";

/*
 * 将 SDK 路径映射到 FatFs 路径。
 *
 * SDK 使用类 POSIX 的 `/sdcard/...` 路径，FatFs 使用 `0:/...`；workspace 前缀会被映射到
 * FatFs 卷根下的相对路径，便于 MCU 配置固定工作区。
 */
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

/*
 * 确保目标文件的父目录存在。
 *
 * FatFs 写文件遇到 FR_NO_PATH 时调用该 helper，逐级 f_mkdir；路径缓冲固定 256 字节，
 * 符合 FreeRTOS profile 的内存预算。
 */
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

/* 将 FatFs 错误码包装成 SDK cc_result_t，方便上层统一处理平台错误。 */
static cc_result_t fatfs_error(FRESULT res, const char *op)
{
    char msg[96];
    snprintf(msg, sizeof(msg), "%s failed: FatFs=%u", op, (unsigned)res);
    return cc_result_error(CC_ERR_PLATFORM, msg);
}

/*
 * FatFs 读取文本文件。
 *
 * 为保护 MCU RAM，单文件读取限制在 256 KiB；out_text 成功后由调用方 free。
 */
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

/*
 * FatFs 覆盖写入文本文件。
 *
 * 如果父目录不存在则先创建父目录，再重新打开写入；写入完成后检查短写和 close 错误。
 */
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

/* FatFs 路径存在性检查；FR_NO_FILE/FR_NO_PATH 被视为 exists=0 且不是错误。 */
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

/*
 * FatFs 列举目录。
 *
 * 返回数组由调用方释放；按需逐项 realloc，适合小目录，超大目录应在产品层限制。
 */
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

/*
 * FatFs 递归创建目录。
 *
 * 逐级 f_mkdir，FR_EXIST 视为成功；用于初始化 workspace/data 目录。
 */
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

/* FatFs 删除文件或空目录；安全审批和 workspace 检查由上层工具/policy 完成。 */
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

/* 销毁 FatFs filesystem 私有对象。 */
static void fatfs_destroy(void *self)
{
    free(self);
}

/* FreeRTOS FatFs vtable。 */
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

/* 未启用 FatFs 时，读取明确返回平台不支持，避免静默返回空数据。 */
static cc_result_t unsupported_read(void *self, const char *path, char **out_text)
{
    (void)self;
    (void)path;
    if (out_text) *out_text = NULL;
    return cc_result_error(CC_ERR_PLATFORM, "FreeRTOS filesystem is not mounted");
}

/* 未挂载文件系统时，写入返回平台错误。 */
static cc_result_t unsupported_write(void *self, const char *path, const char *text)
{
    (void)self;
    (void)path;
    (void)text;
    return cc_result_error(CC_ERR_PLATFORM, "FreeRTOS filesystem is not mounted");
}

/* unsupported profile 下 exists 返回 0，允许上层把能力视为不可用而不是崩溃。 */
static cc_result_t unsupported_exists(void *self, const char *path, int *out_exists)
{
    (void)self;
    (void)path;
    if (out_exists) *out_exists = 0;
    return cc_result_ok();
}

/* 未挂载文件系统时，列目录返回平台错误并清空输出。 */
static cc_result_t unsupported_list(void *self, const char *path, char ***out_items, size_t *out_count)
{
    (void)self;
    (void)path;
    if (out_items) *out_items = NULL;
    if (out_count) *out_count = 0;
    return cc_result_error(CC_ERR_PLATFORM, "FreeRTOS filesystem is not mounted");
}

/* 未挂载文件系统时，创建目录返回平台错误。 */
static cc_result_t unsupported_make_dir(void *self, const char *path)
{
    (void)self;
    (void)path;
    return cc_result_error(CC_ERR_PLATFORM, "FreeRTOS filesystem is not mounted");
}

/* 未挂载文件系统时，删除返回平台错误。 */
static cc_result_t unsupported_remove(void *self, const char *path)
{
    (void)self;
    (void)path;
    return cc_result_error(CC_ERR_PLATFORM, "FreeRTOS filesystem is not mounted");
}

/* 销毁 unsupported filesystem 占位对象。 */
static void unsupported_destroy(void *self)
{
    free(self);
}

/* FreeRTOS unsupported filesystem vtable，保持端口存在但能力显式失败。 */
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

/*
 * 创建 FreeRTOS 默认 filesystem。
 *
 * 编译启用 FatFs 时返回 FatFs adapter，否则返回 unsupported adapter。这样 core 可以链接
 * 同一 filesystem port，而能力差异由运行时错误体现。
 */
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

/* 兼容 POSIX 命名入口，FreeRTOS 下返回默认 filesystem。 */
cc_result_t cc_filesystem_get_posix(cc_filesystem_t *out_fs)
{
    return cc_filesystem_get_default(out_fs);
}
