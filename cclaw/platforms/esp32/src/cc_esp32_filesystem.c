/**
 * 学习导读：cclaw/platforms/esp32/src/cc_esp32_filesystem.c
 *
 * 所属层次：平台层。
 * 阅读重点：这里隐藏 POSIX、Windows、ESP32 的系统 API 差异，阅读时重点看同名端口函数如何按平台实现。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

#include "cc/ports/cc_filesystem.h"

#ifdef ESP_PLATFORM
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct cc_esp32_filesystem {
    int dummy;
} cc_esp32_filesystem_t;

/* 学习注释：esp32_read_text 是本文件内部辅助函数。
 * 阅读时先看它服务哪个 public API，再看它如何处理边界条件和资源释放。 */
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

/* 学习注释：esp32_write_text 是本文件内部辅助函数。
 * 阅读时先看它服务哪个 public API，再看它如何处理边界条件和资源释放。 */
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

/* 学习注释：esp32_exists 是本文件内部辅助函数。
 * 阅读时先看它服务哪个 public API，再看它如何处理边界条件和资源释放。 */
static cc_result_t esp32_exists(void *self, const char *path, int *out_exists)
{
    (void)self;
    *out_exists = (path && access(path, F_OK) == 0) ? 1 : 0;
    return cc_result_ok();
}

/* 学习注释：esp32_list_dir 是本文件内部辅助函数。
 * 阅读时先看它服务哪个 public API，再看它如何处理边界条件和资源释放。 */
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

/* 学习注释：esp32_make_dir 是本文件内部辅助函数。
 * 阅读时先看它服务哪个 public API，再看它如何处理边界条件和资源释放。 */
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

/* 学习注释：esp32_remove 是本文件内部辅助函数。
 * 阅读时先看它服务哪个 public API，再看它如何处理边界条件和资源释放。 */
static cc_result_t esp32_remove(void *self, const char *path)
{
    (void)self;
    if (remove(path) != 0)
        return cc_result_errf(CC_ERR_IO, "Cannot remove: %s", path);
    return cc_result_ok();
}

/* 学习注释：esp32_destroy 是本文件内部辅助函数。
 * 阅读时先看它服务哪个 public API，再看它如何处理边界条件和资源释放。 */
static void esp32_destroy(void *self)
{
    free(self);
}

static cc_filesystem_vtable_t esp32_vtable = {
    esp32_read_text,
    esp32_write_text,
    esp32_exists,
    esp32_list_dir,
    esp32_make_dir,
    esp32_remove,
    esp32_destroy
};

/* 学习注释：cc_filesystem_get_default 是对外可见或跨模块调用的入口。
 * 阅读时重点确认参数校验、所有权转移、错误码和清理路径是否成对出现。 */
cc_result_t cc_filesystem_get_default(cc_filesystem_t *out_fs)
{
    cc_esp32_filesystem_t *self = calloc(1, sizeof(cc_esp32_filesystem_t));
    if (!self) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create ESP32 filesystem");
    out_fs->self = self;
    out_fs->vtable = &esp32_vtable;
    return cc_result_ok();
}

/* 学习注释：cc_filesystem_get_posix 是对外可见或跨模块调用的入口。
 * 阅读时重点确认参数校验、所有权转移、错误码和清理路径是否成对出现。 */
cc_result_t cc_filesystem_get_posix(cc_filesystem_t *out_fs)
{
    return cc_filesystem_get_default(out_fs);
}
#else
#error "cc_esp32_filesystem.c must be built under ESP-IDF"
#endif
