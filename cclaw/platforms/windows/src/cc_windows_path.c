/**
 * 学习导读：cclaw/platforms/windows/src/cc_windows_path.c
 *
 * 所属层次：平台层。
 * 阅读重点：这里隐藏 POSIX、Windows、ESP32 的系统 API 差异，阅读时重点看同名端口函数如何按平台实现。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * cc_windows_path.c — Windows 路径操作实现
 *
 * 在整体架构中的角色和层次：
 *   本模块位于 Platform 层的 Windows 平台实现子层。
 *   Platform 层是整个系统的最底层，负责封装操作系统差异。
 *   本文件是 cc_path.h 接口在 Windows（Win32）平台的具体实现，
 *   提供路径拼接、规范化、沙箱边界检查、父目录获取和文件存在性检查
 *   等基础路径操作。上层代码（如 Sandbox 沙箱模块、文件系统模块）
 *   通过统一的 cc_path_* 函数操作，无需关心底层是反斜杠还是正斜杠。
 *
 * Windows 路径的特殊性：
 *   Windows 路径系统与 POSIX 有显著差异，需要在封装层处理：
 *     - 路径分隔符：Windows 使用反斜杠（\），但也接受正斜杠（/）
 *     - 驱动器和 UNC 路径：如 C:\ 或 \\server\share
 *     - MAX_PATH 限制：传统 Win32 API 路径长度限制为 260 字符（MAX_PATH）
 *     - 大小写不敏感：NTFS 默认大小写不敏感但保持原始大小写
 *     - 多根路径：Windows 可以有多个根（C:\、D:\ 等），POSIX 只有一个根（/）
 *
 * 核心技术：
 *   GetFullPathNameA — 路径规范化（Canonicalization）
 *     Windows 原生 API，将相对路径转换为绝对路径，自动处理：
 *       - "." 和 ".." 的相对路径解析
 *       - 路径分隔符统一为反斜杠（\）
 *       - 当前目录的隐式添加
 *       - 输出的绝对路径长度不超过 MAX_PATH
 *
 *   GetFileAttributesA — 文件存在性检查
 *     获取文件/目录属性，返回值 INVALID_FILE_ATTRIBUTES 表示不存在。
 *     比 POSIX access(F_OK) 更轻量，仅查询元数据而不检查权限。
 *
 * 设计决策：
 *   - 路径拼接统一使用反斜杠（\）：cc_path_join 的默认分隔符
 *   - 规范化始终返回绝对路径：cc_path_canonical 内部调用 GetFullPathNameA
 *   - 沙箱检查基于规范化路径比较：cc_path_is_within 先规范化再前缀匹配
 *   - 分隔符识别兼容两种风格：同时接受 '/' 和 '\' 作为分隔符
 *   - 不处理长路径前缀（\\?\）：保持与传统 MAX_PATH 兼容
 *
 * 平台依赖（Windows 特有，不可移植到 POSIX）：
 *   - GetFullPathNameA — 路径规范化为绝对路径
 *   - GetFileAttributesA — 文件/目录属性查询（存在性检查）
 *   - MAX_PATH — 路径最大长度常量（260）
 *   - _strdup — Windows C 运行库的 strdup 别名
 */

#include "cc/ports/cc_path.h"

#ifdef _WIN32
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/*
 * cc_path_join — 拼接两个路径片段
 *
 * 将 base（基础路径）和 child（子路径）拼接为一个完整路径。
 * 自动处理分隔符：如果 base 已经以 '\' 或 '/' 结尾则不再添加分隔符，
 * 否则添加 '\'。
 *
 * 参数：
 *   base  — 基础路径（可为 NULL，此时直接返回 child 的副本）
 *   child — 子路径（可为 NULL，此时直接返回 base 的副本）
 *
 * 返回值：
 *   成功返回新分配的路径字符串（调用者负责 free），失败返回 NULL
 *
 * 平台注意事项：
 *   - 分隔符固定使用反斜杠（'\'），符合 Windows 路径规范
 *   - 同时识别 base 末尾的 '/' 和 '\' 作为已有分隔符
 *   - 不做路径规范化（不解析 . 和 ..）
 */
char *cc_path_join(const char *base, const char *child)
{
    if (!base) return child ? _strdup(child) : NULL;
    if (!child) return _strdup(base);

    size_t base_len = strlen(base);
    int has_slash = (base_len > 0 && (base[base_len - 1] == '/' || base[base_len - 1] == '\\'));

    size_t len = base_len + strlen(child) + (has_slash ? 0 : 1) + 1;
    char *result = malloc(len);
    if (!result) return NULL;

    strcpy(result, base);
    if (!has_slash) strcat(result, "\\");
    strcat(result, child);

    return result;
}

/*
 * cc_path_canonical — 将路径规范化为绝对路径
 *
 * 使用 Windows GetFullPathNameA API 将路径转换为绝对路径，
 * 解析所有 "." 和 ".." 组件，统一使用反斜杠作为分隔符。
 *
 * 参数：
 *   path — 输入路径（相对或绝对，可为 NULL 返回 NULL）
 *
 * 返回值：
 *   成功返回规范化的绝对路径（调用者负责 free），
 *   失败（GetFullPathNameA 返回 0）时回退到 _strdup(path)
 *
 * 平台注意事项：
 *   - GetFullPathNameA 输出缓冲区为 MAX_PATH（260 字符）
 *   - 不处理包含驱动器字母的当前目录（如 "D:file" 会基于 D: 当前目录解析）
 *   - 不解析符号链接和联结（Windows 不支持真正的符号链接，目录联结是 NTFS 特性）
 */
char *cc_path_canonical(const char *path)
{
    if (!path) return NULL;

    char resolved[MAX_PATH];
    if (GetFullPathNameA(path, MAX_PATH, resolved, NULL)) {
        return _strdup(resolved);
    }
    return _strdup(path);
}

/*
 * cc_path_is_within — 检查路径是否在指定目录内（沙箱边界检查）
 *
 * 将 base_dir 和 path 都规范化为绝对路径后，通过字符串前缀匹配
 * 判断 path 是否位于 base_dir 目录树内。用于沙箱安全策略：
 * 确保代码操作的文件路径不超出允许的目录范围。
 *
 * 参数：
 *   base_dir — 基准目录（沙箱根目录）
 *   path     — 要检查的路径
 *
 * 返回值：
 *   1 表示 path 在 base_dir 内，0 表示不在或参数无效
 *
 * 实现要点：
 *   - 双向规范化：将 base_dir 和 path 都通过 cc_path_canonical 转为绝对路径
 *   - 前缀匹配：path 必须以 base_dir 开头
 *   - 边界检查：匹配后 path 的下一个字符必须是 '\'、'/' 或 '\0'
 *     （防止 /dir-a/file 被误判为在 /dir 内）
 *
 * 平台注意事项：
 *   - Windows 路径不区分大小写，但此实现进行精确字符串比较（有待改进）
 *   - 规范化路径使用反斜杠，故边界检查同时兼容 '\' 和 '/'
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
                  (canon_path[base_len] == '\\' || canon_path[base_len] == '/' || canon_path[base_len] == '\0'));

    free(canon_base);
    free(canon_path);
    return result;
}

/*
 * cc_path_dirname — 获取路径的父目录
 *
 * 返回路径中最后一个分隔符之前的部分。如果路径本身是根目录
 * （如 "C:\"），则返回 "\\"。如果路径中没有分隔符，返回 "."。
 *
 * 参数：
 *   path — 输入路径（可为 NULL 返回 NULL）
 *
 * 返回值：
 *   成功返回新分配的父目录路径（调用者负责 free）
 *
 * 平台注意事项：
 *   - 同时识别 '\' 和 '/' 作为分隔符（strrchr 分别查找）
 *   - "C:\dir\" 的 dirname 为 "C:\dir"
 *   - "C:\" 的 dirname 为 "\\\\"（因为 last_sep == copy）
 *   - 没有分隔符时返回 "."（当前目录）
 */
char *cc_path_dirname(const char *path)
{
    if (!path) return NULL;

    char *copy = _strdup(path);
    if (!copy) return NULL;

    char *last_sep = strrchr(copy, '\\');
    if (!last_sep) last_sep = strrchr(copy, '/');

    if (last_sep) {
        if (last_sep == copy) {
            free(copy);
            return _strdup("\\");
        }
        *last_sep = '\0';
        return copy;
    }

    free(copy);
    return _strdup(".");
}

/*
 * cc_path_exists — 检查文件或目录是否存在
 *
 * 使用 Windows GetFileAttributesA API 查询路径的元数据。
 * 如果返回 INVALID_FILE_ATTRIBUTES 则表示路径不存在。
 *
 * 参数：
 *   path — 要检查的路径（可为 NULL，返回 0）
 *
 * 返回值：
 *   1 表示路径存在，0 表示路径不存在或参数无效
 *
 * 平台注意事项：
 *   - 仅检查存在性，不区分文件和目录
 *   - 不会触发权限错误（GetFileAttributesA 无权限时也返回失败）
 *   - 不追踪符号链接和联结（实际上会追踪，因为 GetFileAttributesA 是同步的）
 */
int cc_path_exists(const char *path)
{
    if (!path) return 0;
    DWORD attr = GetFileAttributesA(path);
    return (attr != INVALID_FILE_ATTRIBUTES) ? 1 : 0;
}

#endif