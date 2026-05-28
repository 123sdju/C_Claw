



#include "cc/ports/cc_path.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>
#include <stdio.h>

/*
 * 对不存在的路径做词法归一化。
 *
 * realpath 要求目标存在，但写文件工具需要在目标文件尚不存在时检查 parent/workspace 边界。
 * 这里把相对路径转成基于 cwd 的绝对路径，再手工处理 `.` 和 `..`，不解析符号链接。
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

/*
 * 拼接 base 和 child。
 *
 * 返回字符串由调用方 free；函数只做字符串拼接，不做 canonical 或安全检查，安全边界由
 * cc_path_is_within 负责。
 */
char *cc_path_join(const char *base, const char *child)
{
    if (!base) return child ? strdup(child) : NULL;
    if (!child) return strdup(base);

    size_t base_len = strlen(base);
    int has_slash = (base_len > 0 && base[base_len - 1] == '/');

    size_t len = base_len + strlen(child) + (has_slash ? 0 : 1) + 1;
    char *result = malloc(len);
    if (!result) return NULL;

    strcpy(result, base);
    if (!has_slash) strcat(result, "/");
    strcat(result, child);

    return result;
}

/*
 * 获取 canonical path。
 *
 * 目标存在时使用 realpath 解析符号链接；目标不存在时退回词法归一化，保证写入前也能做
 * workspace prefix 检查。
 */
char *cc_path_canonical(const char *path)
{
    if (!path) return NULL;

    char resolved[PATH_MAX];
    if (realpath(path, resolved)) {
        return strdup(resolved);
    }

    return normalize_absolute_path(path);
}

/*
 * 判断 path 是否位于 base_dir 内。
 *
 * 两边先 canonical，再做 prefix + 路径分隔符检查，避免 `/tmp/ws2` 被误判为 `/tmp/ws`
 * 内部路径。返回 1 表示允许，0 表示越界或解析失败。
 */
int cc_path_is_within(const char *base_dir, const char *path)
{
    if (!base_dir || !path) return 0;

    char *canon_base = cc_path_canonical(base_dir);
    char *canon_path = cc_path_canonical(path);

    if (!canon_base || !canon_path) {
        free(canon_base);
        free(canon_path);
        return 0;
    }

    size_t base_len = strlen(canon_base);
    int result = (strncmp(canon_path, canon_base, base_len) == 0 &&
                  (canon_path[base_len] == '/' || canon_path[base_len] == '\0'));

    free(canon_base);
    free(canon_path);
    return result;
}

/*
 * 返回路径的父目录。
 *
 * 返回字符串由调用方 free；根目录返回 `/`，没有斜杠的相对文件返回 `.`。
 */
char *cc_path_dirname(const char *path)
{
    if (!path) return NULL;

    char *copy = strdup(path);
    if (!copy) return NULL;

    char *last_slash = strrchr(copy, '/');
    if (last_slash) {
        if (last_slash == copy) {
            free(copy);
            return strdup("/");
        }
        *last_slash = '\0';
        return copy;
    }

    free(copy);
    return strdup(".");
}

/* 判断路径是否存在；POSIX 实现使用 access(F_OK)。 */
int cc_path_exists(const char *path)
{
    if (!path) return 0;
    return access(path, F_OK) == 0;
}
