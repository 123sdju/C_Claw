



#ifndef CC_FILESYSTEM_H
#define CC_FILESYSTEM_H

#include "cc/core/cc_result.h"


/* filesystem vtable 前置声明。 */
typedef struct cc_filesystem_vtable cc_filesystem_vtable_t;

/* filesystem port 前置声明。 */
typedef struct cc_filesystem cc_filesystem_t;

/*
 * 文件系统接口对象。
 *
 * self 指向 POSIX/ESP32/FreeRTOS/Windows 等平台实现，vtable 提供文件操作函数。
 * 核心工具只依赖该接口，避免直接调用 fopen/open，从而便于移植和测试替换。
 */
struct cc_filesystem {
    void *self;
    const cc_filesystem_vtable_t *vtable;
};


/*
 * 文件系统 vtable。
 *
 * 路径安全检查不应只依赖调用方；平台实现和工具层都需要配合 canonical/path boundary
 * 策略。返回的字符串或列表由调用方释放，destroy 释放 self。
 */
struct cc_filesystem_vtable {


    /* 读取 UTF-8/文本文件；out_text 成功后由调用方 free()。 */
    cc_result_t (*read_text)(
        void *self,
        const char *path,
        char **out_text
    );



    /* 写入文本文件；实现应确保父目录存在或返回明确错误。 */
    cc_result_t (*write_text)(
        void *self,
        const char *path,
        const char *text
    );



    /* 查询路径是否存在；out_exists 写 0/1。 */
    cc_result_t (*exists)(
        void *self,
        const char *path,
        int *out_exists
    );



    /* 列出目录项；out_items 数组和字符串由调用方释放。 */
    cc_result_t (*list_dir)(
        void *self,
        const char *path,
        char ***out_items,
        size_t *out_count
    );



    /* 创建目录；是否递归创建由具体平台实现文档决定。 */
    cc_result_t (*make_dir)(
        void *self,
        const char *path
    );



    /* 删除文件或目录；危险操作应由上层 policy/approval 控制。 */
    cc_result_t (*remove)(
        void *self,
        const char *path
    );



    /* 销毁平台 filesystem self；cc_filesystem_t 容器通常由调用方持有。 */
    void (*destroy)(void *self);
};

/* 获取当前 profile 默认 filesystem 实现。 */
cc_result_t cc_filesystem_get_default(cc_filesystem_t *out_fs);

/* 获取 POSIX filesystem 实现；非 POSIX profile 可能返回 unsupported。 */
cc_result_t cc_filesystem_get_posix(cc_filesystem_t *out_fs);

#endif
