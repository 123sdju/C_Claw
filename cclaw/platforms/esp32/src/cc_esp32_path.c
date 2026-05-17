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

/* 学习注释：cc_path_join 是对外可见或跨模块调用的入口。
 * 阅读时重点确认参数校验、所有权转移、错误码和清理路径是否成对出现。 */
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

/* 学习注释：cc_path_canonical 是对外可见或跨模块调用的入口。
 * 阅读时重点确认参数校验、所有权转移、错误码和清理路径是否成对出现。 */
char *cc_path_canonical(const char *path)
{
    if (!path) return NULL;
    char resolved[PATH_MAX];
    if (realpath(path, resolved)) return strdup(resolved);
    return strdup(path);
}

/* 学习注释：cc_path_is_within 是对外可见或跨模块调用的入口。
 * 阅读时重点确认参数校验、所有权转移、错误码和清理路径是否成对出现。 */
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

/* 学习注释：cc_path_dirname 是对外可见或跨模块调用的入口。
 * 阅读时重点确认参数校验、所有权转移、错误码和清理路径是否成对出现。 */
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

/* 学习注释：cc_path_exists 是对外可见或跨模块调用的入口。
 * 阅读时重点确认参数校验、所有权转移、错误码和清理路径是否成对出现。 */
int cc_path_exists(const char *path)
{
    return path && access(path, F_OK) == 0;
}
#else
#error "cc_esp32_path.c must be built under ESP-IDF"
#endif
