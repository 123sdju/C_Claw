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

/**
 * cc_esp32_filesystem — filesystem 端口私有实现对象，当前多数平台无需额外状态但仍保留 self 位置。
 *
 * 资源约定：动态缓冲区由该结构拥有；借用指针只在所属调用链有效，count/capacity 字段必须同步维护。
 */
typedef struct cc_esp32_filesystem {
    int dummy;
} cc_esp32_filesystem_t;

/**
 * esp32_read_text — 执行文件系统操作，并把平台错误转换为统一结果。
 *
 * 位置：ESP32/QEMU 层。注释重点说明当前函数的输入输出、资源边界和错误传播。
 *
 * @param self vtable 私有上下文；生命周期由创建该端口的实现管理。
 * @param path 借用的只读字符串；函数不会释放该指针。
 * @param out_text 输出参数；成功时写入有效结果，失败时保持为 NULL 或未定义状态。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
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

/**
 * esp32_write_text — 执行文件系统操作，并把平台错误转换为统一结果。
 *
 * 位置：ESP32/QEMU 层。注释重点说明当前函数的输入输出、资源边界和错误传播。
 *
 * @param self vtable 私有上下文；生命周期由创建该端口的实现管理。
 * @param path 借用的只读字符串；函数不会释放该指针。
 * @param text 借用的只读字符串；函数不会释放该指针。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
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

/**
 * esp32_exists — 执行文件系统操作，并把平台错误转换为统一结果。
 *
 * 位置：ESP32/QEMU 层。注释重点说明当前函数的输入输出、资源边界和错误传播。
 *
 * @param self vtable 私有上下文；生命周期由创建该端口的实现管理。
 * @param path 借用的只读字符串；函数不会释放该指针。
 * @param out_exists 输出参数；成功时写入有效结果，失败时保持为 NULL 或未定义状态。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
static cc_result_t esp32_exists(void *self, const char *path, int *out_exists)
{
    (void)self;
    *out_exists = (path && access(path, F_OK) == 0) ? 1 : 0;
    return cc_result_ok();
}

/**
 * esp32_list_dir — 执行文件系统操作，并把平台错误转换为统一结果。
 *
 * 位置：ESP32/QEMU 层。注释重点说明当前函数的输入输出、资源边界和错误传播。
 *
 * @param self vtable 私有上下文；生命周期由创建该端口的实现管理。
 * @param path 借用的只读字符串；函数不会释放该指针。
 * @param out_items 输出参数；成功时写入有效结果，失败时保持为 NULL 或未定义状态。
 * @param out_count 输出参数；成功时写入有效结果，失败时保持为 NULL 或未定义状态。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
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

/**
 * esp32_make_dir — 执行文件系统操作，并把平台错误转换为统一结果。
 *
 * 位置：ESP32/QEMU 层。注释重点说明当前函数的输入输出、资源边界和错误传播。
 *
 * @param self vtable 私有上下文；生命周期由创建该端口的实现管理。
 * @param path 借用的只读字符串；函数不会释放该指针。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
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

/**
 * esp32_remove — 执行文件系统操作，并把平台错误转换为统一结果。
 *
 * 位置：ESP32/QEMU 层。注释重点说明当前函数的输入输出、资源边界和错误传播。
 *
 * @param self vtable 私有上下文；生命周期由创建该端口的实现管理。
 * @param path 借用的只读字符串；函数不会释放该指针。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
static cc_result_t esp32_remove(void *self, const char *path)
{
    (void)self;
    if (remove(path) != 0)
        return cc_result_errf(CC_ERR_IO, "Cannot remove: %s", path);
    return cc_result_ok();
}

/**
 * esp32_destroy — 释放、停止或复位该组件拥有的资源，防止失败路径泄漏。
 *
 * 位置：ESP32/QEMU 层。注释重点说明当前函数的输入输出、资源边界和错误传播。
 *
 * @param self vtable 私有上下文；生命周期由创建该端口的实现管理。
 * 无返回值；副作用体现在对象状态、输出缓冲区或资源释放上。
 */
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

/**
 * cc_filesystem_get_default — 执行文件系统操作，并把平台错误转换为统一结果。
 *
 * 位置：ESP32/QEMU 层。注释重点说明当前函数的输入输出、资源边界和错误传播。
 *
 * @param out_fs 输出参数；成功时写入有效结果，失败时保持为 NULL 或未定义状态。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
cc_result_t cc_filesystem_get_default(cc_filesystem_t *out_fs)
{
    cc_esp32_filesystem_t *self = calloc(1, sizeof(cc_esp32_filesystem_t));
    if (!self) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create ESP32 filesystem");
    out_fs->self = self;
    out_fs->vtable = &esp32_vtable;
    return cc_result_ok();
}

/**
 * cc_filesystem_get_posix — 执行文件系统操作，并把平台错误转换为统一结果。
 *
 * 位置：ESP32/QEMU 层。注释重点说明当前函数的输入输出、资源边界和错误传播。
 *
 * @param out_fs 输出参数；成功时写入有效结果，失败时保持为 NULL 或未定义状态。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
cc_result_t cc_filesystem_get_posix(cc_filesystem_t *out_fs)
{
    return cc_filesystem_get_default(out_fs);
}
#else
#error "cc_esp32_filesystem.c must be built under ESP-IDF"
#endif
