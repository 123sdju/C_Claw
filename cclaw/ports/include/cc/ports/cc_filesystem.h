/**
 * 学习导读：cclaw/ports/include/cc/ports/cc_filesystem.h
 *
 * 所属层次：端口层。
 * 阅读重点：这里定义可替换接口，阅读时重点看 struct + vtable + void *self 如何表达多态和依赖注入。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * cc_filesystem.h — 文件系统抽象端口（Port）
 *
 * @file    cc/ports/cc_filesystem.h
 * @brief   提供 POSIX 文件系统的抽象接口，屏蔽平台差异，支持测试替换。
 *
 * 将文件操作（读、写、遍历、删除等）抽象为 vtable 接口，
 * 使得上层代码（如 file_read_tool）不需要直接调用 POSIX API。
 * 带来的好处：
 *   - 单元测试可注入内存文件系统
 *   - 可统一添加路径安全校验（如限制访问工作区外的文件）
 *   - 跨平台时只需替换 vtable 实现（Windows 用不同 API）
 *
 * ─── 接口契约 ─────────────────────────────────────────────────────────
 *
 *   - cc_filesystem_get_default() 返回当前平台的默认文件系统实现
 *   - 所有方法返回 cc_result_t，失败时包含错误描述
 *   - read_text 返回的字符串由调用者负责 free()
 *   - list_dir 返回的字符串数组由调用者负责逐一 free 并释放数组
 *
 * ─── 依赖 ─────────────────────────────────────────────────────────────
 *
 *   仅依赖 cc/core/cc_result.h。
 */

#ifndef CC_FILESYSTEM_H
#define CC_FILESYSTEM_H

#include "cc/core/cc_result.h"

/* ── 前向声明 ───────────────────────────────────────────────────────── */

typedef struct cc_filesystem_vtable cc_filesystem_vtable_t;
/**
 * cc_filesystem_t — 前向声明的端口/服务句柄类型，具体字段在本文件后文或对应端口中定义。
 */
typedef struct cc_filesystem cc_filesystem_t;

/**
 * cc_filesystem_t — 文件系统实例（多态句柄）
 *
 * 值语义结构体，通过 self + vtable 实现文件操作多态。
 * 默认实现通过 cc_filesystem_get_default() 获取。
 */
struct cc_filesystem {
    void *self;                           /**< 指向具体实现的私有数据 */
    const cc_filesystem_vtable_t *vtable; /**< 虚函数表 */
};

/**
 * cc_filesystem_vtable_t — 文件系统虚函数表
 *
 * 定义文件系统操作的抽象接口。每个方法对应一个文件系统原语。
 */
struct cc_filesystem_vtable {
    /**
     * read_text — 以文本模式读取文件内容
     *
     * 读取文件全部内容并作为 C 字符串返回。
     * 二进制文件（含 '\0'）可能被截断，仅适用于文本文件。
     *
     * @param self      文件系统私有数据
     * @param path      文件路径
     * @param out_text  输出：文件内容字符串（调用者负责 free）
     * @return          CC_OK 表示成功
     */
    cc_result_t (*read_text)(
        void *self,
        const char *path,
        char **out_text
    );

    /**
     * write_text — 以文本模式写入文件
     *
     * 将 text 写入指定路径。如果文件已存在则覆盖，不存在则创建。
     * 父目录必须已存在。
     *
     * @param self  文件系统私有数据
     * @param path  文件路径
     * @param text  要写入的内容（C 字符串）
     * @return      CC_OK 表示成功
     */
    cc_result_t (*write_text)(
        void *self,
        const char *path,
        const char *text
    );

    /**
     * exists — 检查文件或目录是否存在
     *
     * @param self        文件系统私有数据
     * @param path        要检查的路径
     * @param out_exists  输出：1 = 存在, 0 = 不存在
     * @return            CC_OK 表示成功
     */
    cc_result_t (*exists)(
        void *self,
        const char *path,
        int *out_exists
    );

    /**
     * list_dir — 列出目录内容
     *
     * 返回目录中所有条目（文件和子目录）的名称数组。
     * 不包含 "." 和 ".."。
     *
     * @param self       文件系统私有数据
     * @param path       目录路径
     * @param out_items  输出：条目名称数组（调用者负责逐一 free + free 数组）
     * @param out_count  输出：条目数量
     * @return           CC_OK 表示成功
     */
    cc_result_t (*list_dir)(
        void *self,
        const char *path,
        char ***out_items,
        size_t *out_count
    );

    /**
     * make_dir — 创建目录
     *
     * 创建指定路径的目录（包含所有必要的父目录，类似 mkdir -p）。
     *
     * @param self  文件系统私有数据
     * @param path  要创建的目录路径
     * @return      CC_OK 表示成功
     */
    cc_result_t (*make_dir)(
        void *self,
        const char *path
    );

    /**
     * remove — 删除文件
     *
     * 删除指定路径的文件。不适用于目录（目录删除需要特殊处理）。
     *
     * @param self  文件系统私有数据
     * @param path  要删除的文件路径
     * @return      CC_OK 表示成功
     */
    cc_result_t (*remove)(
        void *self,
        const char *path
    );

    /**
     * destroy — 销毁文件系统实例
     *
     * 释放文件系统实现持有的所有资源。
     *
     * @param self  文件系统私有数据
     */
    void (*destroy)(void *self);
};

/**
 * cc_filesystem_get_default — 获取当前平台默认文件系统实现
 *
 * 返回当前平台的文件系统实例。桌面 POSIX 使用 POSIX API，
 * Windows 使用 Win32 API，其他设备可以提供自己的实现。
 *
 * @param out_fs  输出：POSIX 文件系统实例（调用方式需要 destroy 的，
 *                应在使用完毕后调用 vtable->destroy）
 * @return        CC_OK 表示成功
 */
cc_result_t cc_filesystem_get_default(cc_filesystem_t *out_fs);

/*
 * Backward-compatible alias for older call sites. New code should call
 * cc_filesystem_get_default() so platform selection remains explicit.
 */
cc_result_t cc_filesystem_get_posix(cc_filesystem_t *out_fs);

#endif
