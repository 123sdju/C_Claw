/**
 * 学习导读：cclaw/platforms/freertos/src/cc_freertos_path.c
 *
 * 所属层次：平台层。
 * 阅读重点：这里隐藏 POSIX、Windows、ESP32 的系统 API 差异，阅读时重点看同名端口函数如何按平台实现。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * cc_freertos_path.c — FreeRTOS 路径操作实现
 *
 * 在整体架构中的角色和层次：
 *   本模块位于 Platform 层的 FreeRTOS 平台实现子层。
 *   Platform 层是整个系统的最底层，负责封装操作系统差异。
 *   本文件是 cc_path.h 端口接口在裸 FreeRTOS（无 VFS）环境的纯字符串实现。
 *   完全不依赖 realpath()、access() 等 POSIX 系统调用，所有操作仅进行
 *   字符串级别的路径拼接、规范化、前缀匹配和目录名计算。
 *
 * 纯字符串实现（无系统调用）：
 *   - cc_path_join：使用 '/' 分隔符拼接路径
 *   - cc_path_canonical：直接返回输入路径副本（无 realpath 可用）
 *   - cc_path_is_within：直接字符串前缀匹配（不先规范化），接受 '/' 边界检查
 *   - cc_path_dirname：查找最后一个 '/' 截取父目录
 *   - cc_path_exists：始终返回 0（裸 FreeRTOS 无文件存在性概念）
 *
 * 设计决策：
 *   裸 FreeRTOS 无文件系统，因此 cc_path_exists 恒返回 0，cc_path_canonical
 *   恒返回输入副本。路径操作仅作字符串处理——这意味着路径穿越检查
 *   （cc_path_is_within）仅在路径本身是规范形式时才有效，不解析符号链接
 *   或相对路径。上层需配合沙箱模块共同确保安全。
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
