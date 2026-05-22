/**
 * 学习导读：cclaw/platforms/freertos/src/cc_freertos_path.c
 * 所属层次：平台层。
 * 阅读重点：裸机 FreeRTOS 路径实现只做字符串级规范化，不依赖 realpath/access。
 */

#include "cc/ports/cc_path.h"

#include <stdlib.h>
#include <string.h>

static char *cc_freertos_strdup(const char *text)
{
    if (!text) return NULL;
    size_t len = strlen(text) + 1;
    char *copy = malloc(len);
    if (copy) memcpy(copy, text, len);
    return copy;
}

char *cc_path_join(const char *base, const char *child)
{
    if (!base) return cc_freertos_strdup(child);
    if (!child) return cc_freertos_strdup(base);
    size_t base_len = strlen(base);
    int slash = base_len > 0 && base[base_len - 1] == '/';
    size_t len = base_len + strlen(child) + (slash ? 0U : 1U) + 1U;
    char *out = malloc(len);
    if (!out) return NULL;
    strcpy(out, base);
    if (!slash) strcat(out, "/");
    strcat(out, child);
    return out;
}

char *cc_path_canonical(const char *path)
{
    return cc_freertos_strdup(path);
}

int cc_path_is_within(const char *base_dir, const char *path)
{
    if (!base_dir || !path) return 0;
    size_t n = strlen(base_dir);
    return strncmp(path, base_dir, n) == 0 && (path[n] == '/' || path[n] == '\0');
}

char *cc_path_dirname(const char *path)
{
    if (!path) return NULL;
    char *copy = cc_freertos_strdup(path);
    if (!copy) return NULL;
    char *slash = strrchr(copy, '/');
    if (!slash) {
        free(copy);
        return cc_freertos_strdup(".");
    }
    if (slash == copy) {
        free(copy);
        return cc_freertos_strdup("/");
    }
    *slash = '\0';
    return copy;
}

int cc_path_exists(const char *path)
{
    (void)path;
    return 0;
}
