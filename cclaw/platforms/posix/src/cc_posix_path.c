/**
 * 学习导读：cclaw/platforms/posix/src/cc_posix_path.c
 *
 * 所属层次：平台层。
 * 阅读重点：这里隐藏 POSIX、Windows、ESP32 的系统 API 差异，阅读时重点看同名端口函数如何按平台实现。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * cc_posix_path.c — POSIX 平台路径操作工具
 *
 * 在整体架构中的角色和层次：
 *   本模块位于 Platform 层的 POSIX 平台实现子层。
 *   Platform 层是整个系统的最底层，负责封装操作系统差异。
 *   本文件是 cc_path.h 接口在 POSIX（Linux/macOS/BSD/Unix）平台的
 *   具体实现，上层各模块通过统一接口调用，不感知底层操作系统。
 *   本模块向上层提供无状态的路径处理工具函数，不维护任何内部状态。
 *
 * 功能范围：
 *   路径拼接（自动处理分隔符）、路径规范化（解析符号链接和相对路径）、
 *   安全检查（路径穿越攻击防护）、目录名提取、路径存在性检查。
 *
 * 设计决策：
 *   - 无状态设计：所有函数为纯函数，不依赖全局状态，天然线程安全
 *   - 调用者管理内存：所有返回的堆字符串由调用者负责 free()，
 *     遵循 C 语言的经典资源管理契约
 *   - realpath 降级策略：当 realpath() 失败时返回原始路径副本，
 *     避免因文件尚不存在而导致整个操作失败（例如在创建新文件前检查路径合法性）
 *   - 路径穿越防护通过前缀匹配实现：将双方规范化为绝对路径后严格比较，
 *     边界条件检查确保 "/var/app" 不会误匹配 "/var/app_evil"
 *
 * 平台依赖（POSIX 特有，不可移植到 Windows）：
 *   - realpath() — 路径规范化（POSIX.1-2001）
 *   - access(F_OK) — 文件存在性检查（POSIX.1-2001）
 *   - PATH_MAX — 最大路径长度常量（定义于 <limits.h>，Linux 通常为 4096）
 */

#include "cc/ports/cc_path.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>

/*
 * cc_path_join — 拼接两个路径片段
 *
 * 将 base 和 child 拼接为一个完整路径，自动处理分隔符 "/"。
 * 如果 base 已以 "/" 结尾，则不会重复添加分隔符。
 *
 * 参数：
 *   base  — 基础路径（可为 NULL）
 *   child — 子路径（可为 NULL）
 *
 * 返回值：
 *   成功返回动态分配的拼接路径字符串（调用者负责 free）
 *   - base 和 child 均为 NULL 时返回 NULL
 *   - base 为 NULL 时返回 child 的副本
 *   - child 为 NULL 时返回 base 的副本
 *
 * 示例：
 *   cc_path_join("/home/user", "docs")    → "/home/user/docs"
 *   cc_path_join("/home/user/", "docs")   → "/home/user/docs"
 *   cc_path_join("/home/user", NULL)      → "/home/user"
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
 * cc_path_canonical — 获取路径的规范化（绝对）形式
 *
 * 使用 POSIX realpath() 将路径解析为不含符号链接、".." 和 "."
 * 的绝对路径。如果 realpath() 失败（例如文件不存在），则返回
 * 原始路径的副本作为降级方案。
 *
 * 参数：
 *   path — 待规范化的路径（可为 NULL）
 *
 * 返回值：
 *   成功返回规范化的绝对路径（调用者负责 free）
 *   path 为 NULL 时返回 NULL
 *
 * 平台注意事项：
 *   - realpath() 要求路径实际存在才能解析成功
 *   - PATH_MAX 限制了路径的最大长度（Linux 通常为 4096）
 *   - 对于不存在的路径，降级返回原始路径的副本
 */
char *cc_path_canonical(const char *path)
{
    if (!path) return NULL;

    char resolved[PATH_MAX];
    if (realpath(path, resolved)) {
        return strdup(resolved);
    }

    return strdup(path);
}

/*
 * cc_path_is_within — 检查路径是否在指定目录范围内（路径穿越防护）
 *
 * 将 base_dir 和 path 都规范化为绝对路径后，检查 path 是否
 * 以 base_dir 为前缀。这是一种防御路径穿越攻击的安全措施。
 *
 * 参数：
 *   base_dir — 基准目录
 *   path     — 待检查的路径
 *
 * 返回值：
 *   1 — path 在 base_dir 范围内
 *   0 — path 不在范围内，或参数为 NULL，或规范化失败
 *
 * 安全检查逻辑：
 *   - 路径前缀匹配：path 必须以 base_dir 开头
 *   - 边界检查：base_dir 之后必须是 '/' 或 '\0'，防止
 *     例如 "/var/app" 匹配 "/var/app_evil" 的情况
 *
 * 示例：
 *   cc_path_is_within("/var/app", "/var/app/data/file.txt")  → 1
 *   cc_path_is_within("/var/app", "/var/app_evil/file.txt")  → 0
 *   cc_path_is_within("/var/app", "/etc/passwd")             → 0
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
 * cc_path_dirname — 提取路径的目录部分
 *
 * 返回路径中最后一个 '/' 之前的部分。行为类似于 POSIX dirname，
 * 但不修改原始字符串。
 *
 * 参数：
 *   path — 文件路径（可为 NULL）
 *
 * 返回值：
 *   成功返回目录部分（调用者负责 free）
 *   - "/foo/bar.txt" → "/foo"
 *   - "/foo"         → "/"
 *   - "foo"          → "."
 *   - path 为 NULL   → NULL
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

/*
 * cc_path_exists — 检查路径是否存在
 *
 * 使用 POSIX access(F_OK) 检查给定路径是否可访问。
 *
 * 参数：
 *   path — 要检查的路径（可为 NULL）
 *
 * 返回值：
 *   1 — 路径存在
 *   0 — 路径不存在，或 path 为 NULL
 */
int cc_path_exists(const char *path)
{
    if (!path) return 0;
    return access(path, F_OK) == 0;
}