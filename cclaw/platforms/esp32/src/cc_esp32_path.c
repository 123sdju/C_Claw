/**
 * 学习导读：cclaw/platforms/esp32/src/cc_esp32_path.c
 *
 * 所属层次：平台层。
 * 阅读重点：这里隐藏 POSIX、Windows、ESP32 的系统 API 差异，阅读时重点看同名端口函数如何按平台实现。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

#include "cc/ports/cc_path.h"

#ifdef ESP_PLATFORM
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 256
#endif

/**
 * cc_path_join — 拼接基础路径和子路径，返回新分配的规范 C 字符串。
 *
 * @param base 借用的只读字符串；函数不会释放该指针。
 * @param child 借用的只读字符串；函数不会释放该指针。
 * @return 新分配字符串；返回 NULL 表示分配或输入校验失败，调用方负责 free。
 */
char *cc_path_join(const char *base, const char *child)
{
    if (!base) return child ? strdup(child) : NULL;
    if (!child) return strdup(base);
    size_t base_len = strlen(base);
    int slash = base_len > 0 && base[base_len - 1] == '/';
    size_t len = base_len + strlen(child) + (slash ? 0 : 1) + 1;
    char *out = malloc(len);
    if (!out) return NULL;
    strcpy(out, base);
    if (!slash) strcat(out, "/");
    strcat(out, child);
    return out;
}

/**
 * cc_path_canonical — 解析路径的真实位置；失败时回退为输入路径副本。
 *
 * @param path 借用的只读字符串；函数不会释放该指针。
 * @return 新分配字符串；返回 NULL 表示分配或输入校验失败，调用方负责 free。
 */
char *cc_path_canonical(const char *path)
{
    if (!path) return NULL;
    char resolved[PATH_MAX];
    if (realpath(path, resolved)) return strdup(resolved);
    return strdup(path);
}

/**
 * cc_path_is_within — 判断目标路径规范化后是否仍位于给定基础目录内。
 *
 * @param base_dir 借用的只读字符串；函数不会释放该指针。
 * @param path 借用的只读字符串；函数不会释放该指针。
 */
int cc_path_is_within(const char *base_dir, const char *path)
{
    if (!base_dir || !path) return 0;
    char *base = cc_path_canonical(base_dir);
    char *target = cc_path_canonical(path);
    if (!base || !target) {
        free(base);
        free(target);
        return 0;
    }
    size_t n = strlen(base);
    int ok = strncmp(target, base, n) == 0 && (target[n] == '/' || target[n] == '\0');
    free(base);
    free(target);
    return ok;
}

/**
 * cc_path_dirname — 计算路径所在目录，返回新分配字符串。
 *
 * @param path 借用的只读字符串；函数不会释放该指针。
 * @return 新分配字符串；返回 NULL 表示分配或输入校验失败，调用方负责 free。
 */
char *cc_path_dirname(const char *path)
{
    if (!path) return NULL;
    char *copy = strdup(path);
    if (!copy) return NULL;
    char *slash = strrchr(copy, '/');
    if (!slash) {
        free(copy);
        return strdup(".");
    }
    if (slash == copy) {
        free(copy);
        return strdup("/");
    }
    *slash = '\0';
    return copy;
}

/**
 * cc_path_exists — 查询路径是否存在，并把平台 API 的结果折叠为布尔值。
 *
 * @param path 借用的只读字符串；函数不会释放该指针。
 * @return 非 0 表示条件成立，0 表示条件不成立。
 */
int cc_path_exists(const char *path)
{
    return path && access(path, F_OK) == 0;
}
#else
#error "cc_esp32_path.c must be built under ESP-IDF"
#endif
