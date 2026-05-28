



#include "cc/ports/cc_filesystem.h"

#ifdef _WIN32
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Windows filesystem 私有状态；当前无额外字段，保留 self 用于统一 vtable 生命周期。 */
typedef struct {
    int dummy;
} cc_windows_filesystem_t;

/* 读取整个文本文件；out_text 成功后由调用方 free。 */
static cc_result_t windows_read_text(void *self, const char *path, char **out_text)
{
    (void)self;
    FILE *f = fopen(path, "rb");
    if (!f) return cc_result_errf(CC_ERR_IO, "Cannot open file: %s", path);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(size + 1);
    if (!buf) { fclose(f); return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate buffer"); }
    size_t n = fread(buf, 1, size, f);
    fclose(f);
    buf[n] = '\0';
    *out_text = buf;
    return cc_result_ok();
}

/* 覆盖写入文本文件；路径安全策略由工具层处理。 */
static cc_result_t windows_write_text(void *self, const char *path, const char *text)
{
    (void)self;
    FILE *f = fopen(path, "w");
    if (!f) return cc_result_errf(CC_ERR_IO, "Cannot open file for writing: %s", path);
    size_t len = strlen(text);
    size_t written = fwrite(text, 1, len, f);
    fclose(f);
    if (written != len) return cc_result_error(CC_ERR_IO, "Failed to write all data");
    return cc_result_ok();
}

/* 使用 GetFileAttributesA 查询路径存在性。 */
static cc_result_t windows_exists(void *self, const char *path, int *out_exists)
{
    (void)self;
    DWORD attr = GetFileAttributesA(path);
    *out_exists = (attr != INVALID_FILE_ATTRIBUTES) ? 1 : 0;
    return cc_result_ok();
}

/*
 * 列举 Windows 目录项。
 *
 * 通过 FindFirstFile/FindNextFile 读取名称，返回数组由调用方逐项 free 后释放。
 */
static cc_result_t windows_list_dir(void *self, const char *path, char ***out_items, size_t *out_count)
{
    (void)self;
    char search_path[MAX_PATH];
    snprintf(search_path, sizeof(search_path), "%s\\*", path);

    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(search_path, &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        return cc_result_errf(CC_ERR_IO, "Cannot open directory: %s", path);
    }

    size_t count = 0;
    size_t cap = 16;
    char **items = malloc(cap * sizeof(char *));
    if (!items) { FindClose(hFind); return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate items"); }

    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        if (count >= cap) {
            cap *= 2;
            items = realloc(items, cap * sizeof(char *));
            if (!items) { FindClose(hFind); return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to grow items"); }
        }
        items[count++] = _strdup(fd.cFileName);
    } while (FindNextFileA(hFind, &fd));

    FindClose(hFind);
    *out_items = items;
    *out_count = count;
    return cc_result_ok();
}

/*
 * 递归创建目录。
 *
 * 兼容 '/' 和 '\\' 分隔符，ERROR_ALREADY_EXISTS 视为成功。
 */
static cc_result_t windows_make_dir(void *self, const char *path)
{
    (void)self;
    if (!path || !path[0])
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid directory path");

    char *tmp = strdup(path);
    if (!tmp) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate path");
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/' && *p != '\\') continue;
        char saved = *p;
        *p = '\0';
        if (tmp[0] && !CreateDirectoryA(tmp, NULL)) {
            DWORD err = GetLastError();
            if (err != ERROR_ALREADY_EXISTS) {
                cc_result_t rc = cc_result_errf(CC_ERR_IO, "Cannot create directory: %s", tmp);
                free(tmp);
                return rc;
            }
        }
        *p = saved;
    }
    if (!CreateDirectoryA(tmp, NULL)) {
        DWORD err = GetLastError();
        if (err != ERROR_ALREADY_EXISTS) {
            cc_result_t rc = cc_result_errf(CC_ERR_IO, "Cannot create directory: %s", path);
            free(tmp);
            return rc;
        }
    }
    free(tmp);
    return cc_result_ok();
}

/* 删除文件；删除目录需要扩展或使用 RemoveDirectoryA。 */
static cc_result_t windows_remove(void *self, const char *path)
{
    (void)self;
    if (!DeleteFileA(path))
        return cc_result_errf(CC_ERR_IO, "Cannot remove: %s", path);
    return cc_result_ok();
}

/* 销毁 Windows filesystem 私有对象。 */
static void windows_destroy(void *self)
{
    free(self);
}

/* Windows filesystem vtable。 */
static cc_filesystem_vtable_t windows_vtable = {
    windows_read_text,
    windows_write_text,
    windows_exists,
    windows_list_dir,
    windows_make_dir,
    windows_remove,
    windows_destroy
};

/*
 * Windows 下兼容 cc_filesystem_get_posix 名称，返回 Windows filesystem 实现。
 *
 * 成功后 out_fs 由调用方通过 vtable destroy 释放。
 */
cc_result_t cc_filesystem_get_posix(cc_filesystem_t *out_fs)
{
    if (!out_fs) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null filesystem output");
    }
    memset(out_fs, 0, sizeof(*out_fs));
    cc_windows_filesystem_t *self = calloc(1, sizeof(cc_windows_filesystem_t));
    if (!self) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create windows filesystem");
    out_fs->self = self;
    out_fs->vtable = &windows_vtable;
    return cc_result_ok();
}

/* 默认 filesystem 入口。 */
cc_result_t cc_filesystem_get_default(cc_filesystem_t *out_fs)
{
    return cc_filesystem_get_posix(out_fs);
}

#endif
