/**
 * 学习导读：cclaw/platforms/windows/src/cc_windows_filesystem.c
 *
 * 所属层次：平台层。
 * 阅读重点：这里隐藏 POSIX、Windows、ESP32 的系统 API 差异，阅读时重点看同名端口函数如何按平台实现。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * cc_windows_filesystem.c — Windows 文件系统操作实现
 *
 * 在整体架构中的角色和层次：
 *   本模块位于 Platform 层的 Windows 平台实现子层。
 *   Platform 层是整个系统的最底层，负责封装操作系统差异。
 *   本文件是 cc_filesystem.h 接口在 Windows（Win32）平台的具体实现，
 *   通过虚表（vtable）模式向上层提供多态的文件系统操作。
 *   上层代码通过 cc_filesystem_t 句柄调用，完全不感知底层平台是 Windows
 *   还是 POSIX，实现了真正的跨平台抽象。
 *
 * 虚表多态设计：
 *   本模块是 Platform 层中唯一使用虚表模式的模块。
 *   cc_filesystem_vtable_t 定义了一组函数指针（读/写/存在性检查/目录遍历/
 *   创建目录/删除/销毁），本文件中的 windows_vtable 静态变量将这些函数指针
 *   绑定到 Windows 版本的实现。cc_filesystem_get_posix() 作为工厂函数，
 *   分配实例并绑定虚表。上层代码通过 vtable 间接调用，实现运行时多态。
 *
 *   注意：函数名仍为 cc_filesystem_get_posix，因为 cc_filesystem.h 中
 *   将该函数声明为统一的跨平台入口点。在 Windows 编译时它返回 Windows 实现。
 *
 * Windows 文件系统 API 映射：
 *   - read_text   → fopen("rb") / fread        标准 C 库文件读取
 *   - write_text  → fopen("w") / fwrite         标准 C 库文件写入
 *   - exists      → GetFileAttributesA          查询文件/目录属性
 *   - list_dir    → FindFirstFileA / FindNextFileA 目录遍历
 *   - make_dir    → CreateDirectoryA            创建目录
 *   - remove      → DeleteFileA                 删除文件
 *
 * FindFirstFile/FindNextFile 目录遍历机制：
 *   Windows 没有 POSIX 的 opendir/readdir/closedir，而是使用
 *   FindFirstFileA / FindNextFileA / FindClose 三步走：
 *     1. FindFirstFileA 传入 "<path>\\*" 模式，返回第一个匹配项的句柄
 *     2. FindNextFileA 循环获取后续匹配项
 *     3. FindClose 关闭搜索句柄
 *   WIN32_FIND_DATAA 结构体中的 cFileName 字段包含文件/目录名。
 *
 * 与 POSIX 版本的关键区别：
 *   - 目录遍历：FindFirstFile/FindNextFile vs opendir/readdir
 *   - 存在性检查：GetFileAttributesA vs access(F_OK)
 *   - 目录创建：CreateDirectoryA vs mkdir()
 *   - 文件删除：DeleteFileA（仅文件） vs remove()（文件和空目录）
 *
 * 设计决策：
 *   - 使用虚表而非编译期条件编译（#ifdef）：虚表模式支持在同一可执行文件中
 *     同时存在 POSIX 和 Windows 实现（编译期选择），便于测试和未来扩展
 *   - 文件系统操作为无状态调用：cc_windows_filesystem_t 仅作占位
 *   - 容错设计：CreateDirectoryA 对 ERROR_ALREADY_EXISTS 视为成功，保证幂等性
 *   - 目录遍历动态扩容：初始容量 16，满后翻倍，平衡内存使用与 realloc 频率
 *   - DeleteFileA 不支持删除非空目录：仅用于文件删除
 *
 * 平台依赖（Windows 特有，不可移植到 POSIX）：
 *   - FindFirstFileA / FindNextFileA / FindClose — 目录遍历
 *   - CreateDirectoryA — 目录创建
 *   - DeleteFileA — 文件删除
 *   - GetFileAttributesA — 文件属性查询
 *   - _strdup — Windows C 运行库的 strdup 别名
 */

#include "cc/ports/cc_filesystem.h"

#ifdef _WIN32
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * cc_windows_filesystem_t — Windows 文件系统实例结构体
 *
 * 当前实现中该结构体仅用作占位标识，不包含实际状态数据。
 * 所有文件系统操作均为无状态调用，直接委托给 Windows API
 * 或标准 C 库函数。
 *
 * 字段：
 *   dummy — 占位字段，确保结构体非空（C 标准不允许空结构体）
 */
typedef struct {
    int dummy;
} cc_windows_filesystem_t;

/*
 * windows_read_text — 以文本模式读取文件全部内容
 *
 * 打开指定路径的文件，读取全部内容，以 NUL 结尾的字符串形式返回。
 * 调用者负责释放返回的字符串内存（通过 free()）。
 *
 * 参数：
 *   self     — 文件系统实例指针（当前实现未使用）
 *   path     — 要读取的文件路径（Windows 风格，如 "C:\\dir\\file.txt"）
 *   out_text — 输出参数，指向读取到的文本内容（由调用者释放）
 *
 * 返回值：
 *   成功返回 cc_result_ok()，失败返回 CC_ERR_IO 或 CC_ERR_OUT_OF_MEMORY
 *
 * 平台注意事项：
 *   - 使用二进制模式 "rb" 打开，避免 Windows 上的 CRLF 自动转换
 *   - 通过 fseek/ftell 获取文件大小，对大文件（>2GB）在 32 位平台上可能不准确
 *   - 此实现与 POSIX 版本一致（都使用标准 C 库 fopen/fread）
 */
static cc_result_t windows_read_text(void *self, const char *path, char **out_text)
{
    (void)self;
    FILE *f = fopen(path, "rb");
    if (!f) return cc_result_errf(CC_ERR_IO, "Cannot open file: %s", path);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(size + 1);
    if (!buf) { fclose(f); return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate buffer"); }
    size_t n = fread(buf, 1, size, f);
    fclose(f);
    buf[n] = '\0';
    *out_text = buf;
    return cc_result_ok();
}

/*
 * windows_write_text — 将文本内容写入文件
 *
 * 打开（或创建）指定路径的文件，以文本模式写入全部内容。
 * 如果文件已存在，其内容将被覆盖。
 *
 * 参数：
 *   self — 文件系统实例指针（当前实现未使用）
 *   path — 目标文件路径
 *   text — 要写入的 NUL 结尾文本内容
 *
 * 返回值：
 *   成功返回 cc_result_ok()，失败返回 CC_ERR_IO
 *
 * 平台注意事项：
 *   - 使用 "w" 模式打开，若文件不存在则创建，若存在则截断
 *   - 写完后校验实际写入字节数，防止磁盘满等异常情况
 *   - Windows 上文本模式写入时 '\n' 会被转换为 '\r\n'（与其他系统不同）
 */
static cc_result_t windows_write_text(void *self, const char *path, const char *text)
{
    (void)self;
    FILE *f = fopen(path, "w");
    if (!f) return cc_result_errf(CC_ERR_IO, "Cannot open file for writing: %s", path);
    size_t len = strlen(text);
    size_t written = fwrite(text, 1, len, f);
    fclose(f);
    if (written != len) return cc_result_error(CC_ERR_IO, "Failed to write all data");
    return cc_result_ok();
}

/*
 * windows_exists — 检查文件或目录是否存在
 *
 * 使用 Windows GetFileAttributesA API 查询给定路径的文件属性。
 * 如果返回 INVALID_FILE_ATTRIBUTES 则表示路径不存在。
 *
 * 参数：
 *   self       — 文件系统实例指针（当前实现未使用）
 *   path       — 要检查的路径
 *   out_exists — 输出参数，1 表示存在，0 表示不存在
 *
 * 返回值：
 *   总是返回 cc_result_ok()（错误通过 out_exists=0 表示）
 *
 * 平台注意事项：
 *   - 不区分文件和目录，二者都返回存在
 *   - 无权限访问时也返回不存在（与 POSIX access 行为一致）
 *   - 比 cc_path_exists 更底层：后者是独立函数，此函数通过虚表调用
 */
static cc_result_t windows_exists(void *self, const char *path, int *out_exists)
{
    (void)self;
    DWORD attr = GetFileAttributesA(path);
    *out_exists = (attr != INVALID_FILE_ATTRIBUTES) ? 1 : 0;
    return cc_result_ok();
}

/*
 * windows_list_dir — 列出目录中的所有条目
 *
 * 使用 Windows FindFirstFileA / FindNextFileA API 遍历指定目录，
 * 返回目录中所有文件和子目录的名称列表。自动跳过 "." 和 ".." 特殊目录项。
 *
 * 参数：
 *   self      — 文件系统实例指针（当前实现未使用）
 *   path      — 要遍历的目录路径
 *   out_items — 输出参数，指向字符串数组（每个元素需调用者 free）
 *   out_count — 输出参数，数组中条目的数量
 *
 * 返回值：
 *   成功返回 cc_result_ok()，失败返回 CC_ERR_IO 或 CC_ERR_OUT_OF_MEMORY
 *
 * 实现要点：
 *   1. 构造搜索模式 "<path>\\*" 匹配目录下所有条目
 *   2. FindFirstFileA 返回第一个匹配项的句柄
 *   3. do-while 循环中跳过 "." 和 ".."，其他条目通过 _strdup 添加到数组
 *   4. 动态扩容：初始容量 16，满后翻倍
 *   5. FindClose 关闭搜索句柄
 *
 * 平台注意事项：
 *   - WIN32_FIND_DATAA.cFileName 仅包含文件名，不含路径前缀
 *   - 条目顺序由 NTFS 文件系统决定，不保证字母顺序
 *   - FindFirstFileA 可能因权限不足或路径不存在而失败
 */
static cc_result_t windows_list_dir(void *self, const char *path, char ***out_items, size_t *out_count)
{
    (void)self;
    char search_path[MAX_PATH];
    snprintf(search_path, sizeof(search_path), "%s\\*", path);

    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(search_path, &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        return cc_result_errf(CC_ERR_IO, "Cannot open directory: %s", path);
    }

    size_t count = 0;
    size_t cap = 16;
    char **items = malloc(cap * sizeof(char *));
    if (!items) { FindClose(hFind); return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate items"); }

    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        if (count >= cap) {
            cap *= 2;
            items = realloc(items, cap * sizeof(char *));
            if (!items) { FindClose(hFind); return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to grow items"); }
        }
        items[count++] = _strdup(fd.cFileName);
    } while (FindNextFileA(hFind, &fd));

    FindClose(hFind);
    *out_items = items;
    *out_count = count;
    return cc_result_ok();
}

/*
 * windows_make_dir — 创建目录
 *
 * 使用 Windows CreateDirectoryA API 创建单级目录。
 * 如果目录已存在（ERROR_ALREADY_EXISTS），视为成功（幂等操作）。
 *
 * 参数：
 *   self — 文件系统实例指针（当前实现未使用）
 *   path — 要创建的目录路径
 *
 * 返回值：
 *   成功返回 cc_result_ok()，失败返回 CC_ERR_IO
 *
 * 平台注意事项：
 *   - 只创建最后一级目录，不会创建中间不存在的父目录
 *   - 安全属性为 NULL，目录继承父目录的 ACL
 *   - GetLastError() == ERROR_ALREADY_EXISTS 时视为成功
 *   - 不支持递归创建（mkdir -p 语义），需自行逐级创建
 */
static cc_result_t windows_make_dir(void *self, const char *path)
{
    (void)self;
    if (!path || !path[0])
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid directory path");

    char *tmp = strdup(path);
    if (!tmp) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate path");
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/' && *p != '\\') continue;
        char saved = *p;
        *p = '\0';
        if (tmp[0] && !CreateDirectoryA(tmp, NULL)) {
            DWORD err = GetLastError();
            if (err != ERROR_ALREADY_EXISTS) {
                cc_result_t rc = cc_result_errf(CC_ERR_IO, "Cannot create directory: %s", tmp);
                free(tmp);
                return rc;
            }
        }
        *p = saved;
    }
    if (!CreateDirectoryA(tmp, NULL)) {
        DWORD err = GetLastError();
        if (err != ERROR_ALREADY_EXISTS) {
            cc_result_t rc = cc_result_errf(CC_ERR_IO, "Cannot create directory: %s", path);
            free(tmp);
            return rc;
        }
    }
    free(tmp);
    return cc_result_ok();
}

/*
 * windows_remove — 删除文件
 *
 * 使用 Windows DeleteFileA API 删除指定路径的文件。
 * 注意：此 API 不支持删除非空目录，仅适用于文件删除。
 *
 * 参数：
 *   self — 文件系统实例指针（当前实现未使用）
 *   path — 要删除的文件路径
 *
 * 返回值：
 *   成功返回 cc_result_ok()，失败返回 CC_ERR_IO
 *
 * 平台注意事项：
 *   - DeleteFileA 不支持删除目录（空目录也不行）
 *   - 文件被其他进程锁定时删除会失败
 *   - 删除后文件不可恢复（不经过回收站）
 */
static cc_result_t windows_remove(void *self, const char *path)
{
    (void)self;
    if (!DeleteFileA(path))
        return cc_result_errf(CC_ERR_IO, "Cannot remove: %s", path);
    return cc_result_ok();
}

/*
 * windows_destroy — 销毁 Windows 文件系统实例
 *
 * 释放由 cc_filesystem_get_posix() 分配的实例内存。
 *
 * 参数：
 *   self — 要销毁的文件系统实例指针
 */
static void windows_destroy(void *self)
{
    free(self);
}

/*
 * windows_vtable — Windows 文件系统操作虚表
 *
 * 将各个静态函数指针绑定到虚表结构中，供上层通过 cc_filesystem_t
 * 接口进行多态调用。顺序必须与 cc_filesystem_vtable_t 结构体定义一致。
 */
static cc_filesystem_vtable_t windows_vtable = {
    windows_read_text,
    windows_write_text,
    windows_exists,
    windows_list_dir,
    windows_make_dir,
    windows_remove,
    windows_destroy
};

/*
 * cc_filesystem_get_posix — 获取当前平台的文件系统实现
 *
 * 分配并初始化一个 Windows 文件系统实例，绑定虚表指针。
 * 这是跨平台的文件系统工厂函数（Windows 编译时返回 Windows 实现，
 * POSIX 编译时返回 POSIX 实现）。调用者使用完毕后需通过 vtable->destroy
 * 释放资源。
 *
 * 参数：
 *   out_fs — 输出参数，指向初始化完成的 cc_filesystem_t 结构体
 *
 * 返回值：
 *   成功返回 cc_result_ok()，失败返回 CC_ERR_OUT_OF_MEMORY
 *
 * 注意：函数名保留 _posix 后缀是因为 cc_filesystem.h 中声明为该名称，
 *       在 Windows 编译条件下同样使用此函数名以保持 API 一致性。
 */
cc_result_t cc_filesystem_get_posix(cc_filesystem_t *out_fs)
{
    cc_windows_filesystem_t *self = calloc(1, sizeof(cc_windows_filesystem_t));
    if (!self) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create windows filesystem");
    out_fs->self = self;
    out_fs->vtable = &windows_vtable;
    return cc_result_ok();
}

/* 学习注释：cc_filesystem_get_default 是对外可见或跨模块调用的入口。
 * 阅读时重点确认参数校验、所有权转移、错误码和清理路径是否成对出现。 */
cc_result_t cc_filesystem_get_default(cc_filesystem_t *out_fs)
{
    return cc_filesystem_get_posix(out_fs);
}

#endif
