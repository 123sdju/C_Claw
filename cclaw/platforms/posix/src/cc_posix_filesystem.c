/**
 * 学习导读：cclaw/platforms/posix/src/cc_posix_filesystem.c
 *
 * 所属层次：平台层。
 * 阅读重点：这里隐藏 POSIX、Windows、ESP32 的系统 API 差异，阅读时重点看同名端口函数如何按平台实现。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * cc_posix_filesystem.c — POSIX 文件系统操作实现
 *
 * 在整体架构中的角色和层次：
 *   本模块位于 Platform 层的 POSIX 平台实现子层。
 *   Platform 层是整个系统的最底层，负责封装操作系统差异。
 *   本文件是 cc_filesystem.h 接口在 POSIX（Linux/macOS/BSD/Unix）平台的
 *   具体实现，通过虚表（vtable）模式向上层提供多态的文件系统操作。
 *   上层代码通过 cc_filesystem_t 句柄调用，完全不感知底层平台是 POSIX
 *   还是其他系统（如 Windows），实现了真正的跨平台抽象。
 *
 * 虚表多态设计：
 *   本模块是 Platform 层中唯一使用虚表模式的模块。
 *   cc_filesystem_vtable_t 定义了一组函数指针（读/写/存在性检查/目录遍历/
 *   创建目录/删除/销毁），本文件中的 posix_vtable 静态变量将这些函数指针
 *   绑定到 POSIX 版本的实现。cc_filesystem_get_posix() 作为工厂函数，
 *   分配实例并绑定虚表。上层代码通过 vtable 间接调用，实现运行时多态。
 *
 * 设计决策：
 *   - 使用虚表而非编译期条件编译（#ifdef）：虚表模式支持在同一可执行文件中
 *     同时存在 POSIX 和 Windows 实现（如果平台支持），便于测试和未来扩展
 *   - 文件系统操作为无状态调用：虽然 cc_posix_filesystem_t 结构体存在，
 *     但当前仅作占位，所有操作不依赖实例状态
 *   - 容错设计：mkdir 对 EEXIST（目录已存在）视为成功，保证幂等性
 *   - 目录遍历动态扩容：初始容量 16，满后翻倍，平衡内存使用与 realloc 频率
 *
 * 功能范围：
 *   - 文本文件读写（read_text / write_text）
 *   - 文件/目录存在性检查（exists，基于 access(F_OK)）
 *   - 目录条目列表（list_dir，自动跳过 . 和 ..）
 *   - 单级目录创建（make_dir，权限 0755，受 umask 影响）
 *   - 文件/空目录删除（remove）
 *
 * 平台依赖（POSIX 特有，不可移植到 Windows）：
 *   - fopen/fread/fwrite — 标准 C 库文件 I/O
 *   - access(F_OK) — 文件存在性检查（POSIX.1-2001）
 *   - opendir/readdir/closedir — 目录遍历（POSIX.1-2001）
 *   - mkdir(0755) — 目录创建（POSIX.1-2001）
 *   - remove() — 文件/空目录删除（标准 C 库，POSIX 兼容）
 */

#include "cc/ports/cc_filesystem.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include <unistd.h>

/*
 * cc_posix_filesystem_t — POSIX 文件系统实例结构体
 *
 * 当前实现中该结构体仅用作占位标识，不包含实际状态数据。
 * 所有文件系统操作均为无状态调用，直接委托给 POSIX 系统调用。
 *
 * 字段：
 *   dummy — 占位字段，确保结构体非空（C 标准不允许空结构体）
 */
typedef struct {
    int dummy;
} cc_posix_filesystem_t;

/*
 * posix_read_text — 以文本模式读取文件全部内容
 *
 * 打开指定路径的文件，读取全部内容，以 NUL 结尾的字符串形式返回。
 * 调用者负责释放返回的字符串内存（通过 free()）。
 *
 * 参数：
 *   self     — 文件系统实例指针（当前实现未使用）
 *   path     — 要读取的文件路径（POSIX 风格，如 "/home/user/file.txt"）
 *   out_text — 输出参数，指向读取到的文本内容（由调用者释放）
 *
 * 返回值：
 *   成功返回 cc_result_ok()，失败返回 CC_ERR_IO 或 CC_ERR_OUT_OF_MEMORY
 *
 * 平台注意事项：
 *   - 使用二进制模式 "rb" 打开，避免 Windows 上的 CRLF 转换问题
 *   - 通过 fseek/ftell 获取文件大小，对大文件（>2GB）在 32 位平台上可能不准确
 */
static cc_result_t posix_read_text(void *self, const char *path, char **out_text)
{
    (void)self;
    FILE *f = fopen(path, "rb");
    if (!f) return cc_result_errf(CC_ERR_IO, "Cannot open file: %s", path);

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = malloc(size + 1);
    if (!buf) {
        fclose(f);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate buffer");
    }

    size_t read_size = fread(buf, 1, size, f);
    fclose(f);
    buf[read_size] = '\0';
    *out_text = buf;
    return cc_result_ok();
}

/*
 * posix_write_text — 将文本内容写入文件
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
 */
static cc_result_t posix_write_text(void *self, const char *path, const char *text)
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
 * posix_exists — 检查文件或目录是否存在
 *
 * 使用 POSIX access() 系统调用检查给定路径是否可访问。
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
 *   - 使用 F_OK 模式，仅检查存在性，不检查读写权限
 *   - access() 使用进程的真实 UID/GID，而非有效 UID/GID
 */
static cc_result_t posix_exists(void *self, const char *path, int *out_exists)
{
    (void)self;
    *out_exists = (access(path, F_OK) == 0) ? 1 : 0;
    return cc_result_ok();
}

/*
 * posix_list_dir — 列出目录中的所有条目
 *
 * 遍历指定目录，返回目录中所有文件和子目录的名称列表。
 * 自动跳过 "." 和 ".." 特殊目录项。
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
 * 平台注意事项：
 *   - 使用 POSIX opendir/readdir/closedir，线程安全依赖于 libc 实现
 *   - 条目顺序由底层文件系统决定，不保证任何特定顺序
 *   - 使用动态扩容策略：初始容量 16，满后翻倍
 */
static cc_result_t posix_list_dir(void *self, const char *path, char ***out_items, size_t *out_count)
{
    (void)self;
    DIR *d = opendir(path);
    if (!d) return cc_result_errf(CC_ERR_IO, "Cannot open directory: %s", path);

    size_t count = 0;
    size_t cap = 16;
    char **items = malloc(cap * sizeof(char *));
    if (!items) {
        closedir(d);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate items");
    }

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        if (count >= cap) {
            cap *= 2;
            items = realloc(items, cap * sizeof(char *));
            if (!items) {
                closedir(d);
                return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to grow items");
            }
        }
        items[count++] = strdup(entry->d_name);
    }
    closedir(d);

    *out_items = items;
    *out_count = count;
    return cc_result_ok();
}

/*
 * posix_make_dir — 创建目录
 *
 * 使用 POSIX mkdir() 创建目录，权限设置为 0755（rwxr-xr-x）。
 * 如果目录已存在（EEXIST），视为成功。
 *
 * 参数：
 *   self — 文件系统实例指针（当前实现未使用）
 *   path — 要创建的目录路径
 *
 * 返回值：
 *   成功返回 cc_result_ok()，失败返回 CC_ERR_IO
 *
 * 平台注意事项：
 *   - mkdir 只创建最后一级目录，不会创建中间不存在的父目录
 *   - 权限 0755 受进程 umask 影响，最终权限 = 0755 & ~umask
 *   - errno == EEXIST 时视为幂等操作成功
 */
static cc_result_t posix_make_dir(void *self, const char *path)
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

/*
 * posix_remove — 删除文件或空目录
 *
 * 使用 POSIX remove() 删除指定路径的文件或空目录。
 *
 * 参数：
 *   self — 文件系统实例指针（当前实现未使用）
 *   path — 要删除的文件或目录路径
 *
 * 返回值：
 *   成功返回 cc_result_ok()，失败返回 CC_ERR_IO
 *
 * 平台注意事项：
 *   - remove() 对非空目录的行为是未定义的，POSIX 标准建议返回错误
 *   - 不区分文件和目录，统一调用 remove()
 */
static cc_result_t posix_remove(void *self, const char *path)
{
    (void)self;
    if (remove(path) != 0)
        return cc_result_errf(CC_ERR_IO, "Cannot remove: %s", path);
    return cc_result_ok();
}

/*
 * posix_destroy — 销毁 POSIX 文件系统实例
 *
 * 释放由 cc_filesystem_get_posix() 分配的实例内存。
 *
 * 参数：
 *   self — 要销毁的文件系统实例指针
 */
static void posix_destroy(void *self)
{
    free(self);
}

/*
 * posix_vtable — POSIX 文件系统操作虚表
 *
 * 将各个静态函数指针绑定到虚表结构中，供上层通过 cc_filesystem_t
 * 接口进行多态调用。顺序必须与 cc_filesystem_vtable_t 结构体定义一致。
 */
static cc_filesystem_vtable_t posix_vtable = {
    posix_read_text,
    posix_write_text,
    posix_exists,
    posix_list_dir,
    posix_make_dir,
    posix_remove,
    posix_destroy
};

/*
 * cc_filesystem_get_posix — 获取 POSIX 平台的文件系统实现
 *
 * 分配并初始化一个 POSIX 文件系统实例，绑定虚表指针。
 * 这是 POSIX 平台的文件系统工厂函数，上层代码通过此函数获取
 * 平台特定的文件系统实现。调用者使用完毕后需通过 vtable->destroy
 * 释放资源。
 *
 * 参数：
 *   out_fs — 输出参数，指向初始化完成的 cc_filesystem_t 结构体
 *
 * 返回值：
 *   成功返回 cc_result_ok()，失败返回 CC_ERR_OUT_OF_MEMORY
 */
cc_result_t cc_filesystem_get_posix(cc_filesystem_t *out_fs)
{
    cc_posix_filesystem_t *self = calloc(1, sizeof(cc_posix_filesystem_t));
    if (!self) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create posix filesystem");

    out_fs->self = self;
    out_fs->vtable = &posix_vtable;
    return cc_result_ok();
}

/**
 * cc_filesystem_get_default — 执行文件系统操作，并把平台错误转换为统一结果。
 *
 * @param out_fs 输出参数；调用方传入有效指针，成功后接收结果。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
cc_result_t cc_filesystem_get_default(cc_filesystem_t *out_fs)
{
    return cc_filesystem_get_posix(out_fs);
}
