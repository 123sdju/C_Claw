



#include "cc/ports/cc_path.h"

#ifdef ESP_PLATFORM
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 256
#endif

/*
 * ESP32 上对路径做词法绝对化。
 *
 * SPIFFS/FATFS 场景下目标文件可能不存在，realpath 失败时仍需要处理 `.`/`..` 来做
 * workspace 边界检查；PATH_MAX 较小，符合 MCU 内存预算。
 */
static char *normalize_absolute_path(const char *path)
{
    char input[PATH_MAX];
    if (!path) return NULL;
    if (path[0] == '/') {
        snprintf(input, sizeof(input), "%s", path);
    } else {
        char cwd[PATH_MAX];
        if (!getcwd(cwd, sizeof(cwd))) return strdup(path);
        size_t cwd_len = strlen(cwd);
        size_t path_len = strlen(path);
        if (cwd_len + 1 + path_len >= sizeof(input)) return NULL;
        memcpy(input, cwd, cwd_len);
        input[cwd_len] = '/';
        memcpy(input + cwd_len + 1, path, path_len + 1);
    }

    char *parts[PATH_MAX / 2];
    size_t count = 0;
    char work[PATH_MAX];
    snprintf(work, sizeof(work), "%s", input);
    char *save = NULL;
    char *token = strtok_r(work, "/", &save);
    while (token) {
        if (strcmp(token, ".") == 0 || token[0] == '\0') {
        } else if (strcmp(token, "..") == 0) {
            if (count > 0) count--;
        } else if (count < sizeof(parts) / sizeof(parts[0])) {
            parts[count++] = token;
        }
        token = strtok_r(NULL, "/", &save);
    }

    char out[PATH_MAX];
    size_t pos = 0;
    out[pos++] = '/';
    for (size_t i = 0; i < count; i++) {
        size_t len = strlen(parts[i]);
        if (pos + len + 2 >= sizeof(out)) return NULL;
        if (i > 0) out[pos++] = '/';
        memcpy(out + pos, parts[i], len);
        pos += len;
    }
    out[pos] = '\0';
    return strdup(out);
}

/* 拼接 base/child，返回字符串由调用方释放；不在这里做安全检查。 */
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

/*
 * canonical path。
 *
 * 文件存在时使用 realpath；不存在时使用词法归一化，保证写入前的 parent/workspace 检查
 * 仍然可用。
 */
char *cc_path_canonical(const char *path)
{
    if (!path) return NULL;
    char resolved[PATH_MAX];
    if (realpath(path, resolved)) return strdup(resolved);
    return normalize_absolute_path(path);
}

/* 判断目标路径是否位于 base_dir 内，使用 canonical 后的 prefix + 分隔符检查。 */
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

/* 返回父目录字符串；调用方负责 free。 */
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

/* ESP32 文件存在性检查，底层依赖 VFS access。 */
int cc_path_exists(const char *path)
{
    return path && access(path, F_OK) == 0;
}
#else
#error "cc_esp32_path.c must be built under ESP-IDF"
#endif
