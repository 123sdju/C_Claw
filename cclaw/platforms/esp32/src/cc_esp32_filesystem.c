



#include "cc/ports/cc_filesystem.h"

#ifdef ESP_PLATFORM
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * ESP32 filesystem 私有状态。
 *
 * 当前通过 ESP-IDF VFS 的 stdio/dirent 接口实现，因此不需要额外状态；保留 self 是为了
 * 满足统一 filesystem vtable。
 */
typedef struct cc_esp32_filesystem {
    int dummy;
} cc_esp32_filesystem_t;

/*
 * 读取整个文本文件。
 *
 * 适合小配置/上下文文件；大文件读取应由上层 limits 限制，避免 MCU RAM 被一次性占满。
 */
static cc_result_t esp32_read_text(void *self, const char *path, char **out_text)
{
    (void)self;
    FILE *f = fopen(path, "rb");
    if (!f) return cc_result_errf(CC_ERR_IO, "Cannot open file: %s", path);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = malloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate file buffer");
    }
    size_t n = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[n] = '\0';
    *out_text = buf;
    return cc_result_ok();
}

/* 覆盖写入文本文件；路径安全检查由工具层完成，平台层只负责 VFS I/O。 */
static cc_result_t esp32_write_text(void *self, const char *path, const char *text)
{
    (void)self;
    FILE *f = fopen(path, "wb");
    if (!f) return cc_result_errf(CC_ERR_IO, "Cannot open file for writing: %s", path);
    size_t len = strlen(text ? text : "");
    size_t n = fwrite(text ? text : "", 1, len, f);
    fclose(f);
    if (n != len) return cc_result_error(CC_ERR_IO, "Failed to write all data");
    return cc_result_ok();
}

/* 判断路径是否存在，底层使用 ESP-IDF VFS access。 */
static cc_result_t esp32_exists(void *self, const char *path, int *out_exists)
{
    (void)self;
    *out_exists = (path && access(path, F_OK) == 0) ? 1 : 0;
    return cc_result_ok();
}

/*
 * 列举目录项。
 *
 * 返回数组由调用方逐项 free 后释放数组；容量从 8 开始，控制 MCU 上的初始内存占用。
 */
static cc_result_t esp32_list_dir(void *self, const char *path, char ***out_items, size_t *out_count)
{
    (void)self;
    DIR *dir = opendir(path);
    if (!dir) return cc_result_errf(CC_ERR_IO, "Cannot open directory: %s", path);

    size_t cap = 8;
    size_t count = 0;
    char **items = calloc(cap, sizeof(char *));
    if (!items) {
        closedir(dir);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate directory list");
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        if (count >= cap) {
            cap *= 2;
            char **next = realloc(items, cap * sizeof(char *));
            if (!next) {
                for (size_t i = 0; i < count; i++) free(items[i]);
                free(items);
                closedir(dir);
                return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to grow directory list");
            }
            items = next;
        }
        items[count] = strdup(entry->d_name);
        if (!items[count]) {
            for (size_t i = 0; i < count; i++) free(items[i]);
            free(items);
            closedir(dir);
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy directory entry");
        }
        count++;
    }
    closedir(dir);
    *out_items = items;
    *out_count = count;
    return cc_result_ok();
}

/*
 * 递归创建目录。
 *
 * ESP32 文件系统可能是 SPIFFS/FATFS/VFS，逐级 mkdir 并把 EEXIST 视为成功。
 */
static cc_result_t esp32_make_dir(void *self, const char *path)
{
    (void)self;
    if (!path || !path[0])
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid directory path");

    char *tmp = strdup(path);
    if (!tmp) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate path");
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (tmp[0] && mkdir(tmp, 0755) != 0 && errno != EEXIST) {
            cc_result_t rc = cc_result_errf(CC_ERR_IO, "Cannot create directory: %s", tmp);
            free(tmp);
            return rc;
        }
        *p = '/';
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        cc_result_t rc = cc_result_errf(CC_ERR_IO, "Cannot create directory: %s", path);
        free(tmp);
        return rc;
    }
    free(tmp);
    return cc_result_ok();
}

/* 删除文件或空目录；高风险删除策略不在平台层处理。 */
static cc_result_t esp32_remove(void *self, const char *path)
{
    (void)self;
    if (remove(path) != 0)
        return cc_result_errf(CC_ERR_IO, "Cannot remove: %s", path);
    return cc_result_ok();
}

/* 销毁 ESP32 filesystem 私有对象。 */
static void esp32_destroy(void *self)
{
    free(self);
}

/* ESP32 filesystem vtable。 */
static cc_filesystem_vtable_t esp32_vtable = {
    esp32_read_text,
    esp32_write_text,
    esp32_exists,
    esp32_list_dir,
    esp32_make_dir,
    esp32_remove,
    esp32_destroy
};

/*
 * 创建 ESP32 默认 filesystem 端口。
 *
 * 成功后 out_fs 持有 self/vtable；VFS 挂载应由应用或平台初始化代码提前完成。
 */
cc_result_t cc_filesystem_get_default(cc_filesystem_t *out_fs)
{
    if (!out_fs) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null filesystem output");
    }
    memset(out_fs, 0, sizeof(*out_fs));
    cc_esp32_filesystem_t *self = calloc(1, sizeof(cc_esp32_filesystem_t));
    if (!self) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create ESP32 filesystem");
    out_fs->self = self;
    out_fs->vtable = &esp32_vtable;
    return cc_result_ok();
}

/* 兼容 POSIX 命名入口，在 ESP32 profile 中返回同一个默认 filesystem。 */
cc_result_t cc_filesystem_get_posix(cc_filesystem_t *out_fs)
{
    return cc_filesystem_get_default(out_fs);
}
#else
#error "cc_esp32_filesystem.c must be built under ESP-IDF"
#endif
