



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
 * POSIX filesystem 私有状态。
 *
 * 当前实现无额外状态，仍保留 self 对象是为了符合 cc_filesystem_t 的 vtable/OOP 结构，
 * 后续可以在这里加入根目录、权限策略或测试注入状态。
 */
typedef struct {
    int dummy;
} cc_posix_filesystem_t;

/*
 * 读取整个文本文件。
 *
 * out_text 成功后由调用方 free；函数按二进制读取并补 '\0'，适用于配置/源码等文本内容。
 * 大文件限制由上层 runtime/tool limits 控制。
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
 * 覆盖写入文本文件。
 *
 * 不负责路径安全检查，调用方应先通过 workspace/path policy；这里仅执行平台 I/O。
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

/* 查询路径是否存在；out_exists 为 1 表示存在，0 表示不存在或无访问权限。 */
static cc_result_t posix_exists(void *self, const char *path, int *out_exists)
{
    (void)self;
    *out_exists = (access(path, F_OK) == 0) ? 1 : 0;
    return cc_result_ok();
}

/*
 * 列举目录项名称。
 *
 * 返回字符串数组由调用方逐项 free 后再 free 数组；函数过滤掉 "." 和 ".."。
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
 * 递归创建目录。
 *
 * 逐级 mkdir，EEXIST 视为成功；这是 builder 初始化 data_dir/workspace_dir 时需要的基础
 * 平台能力。
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

/* 删除文件或空目录；高风险路径/审批应在工具或 policy 层完成。 */
static cc_result_t posix_remove(void *self, const char *path)
{
    (void)self;
    if (remove(path) != 0)
        return cc_result_errf(CC_ERR_IO, "Cannot remove: %s", path);
    return cc_result_ok();
}

/* 销毁 POSIX filesystem 私有对象。 */
static void posix_destroy(void *self)
{
    free(self);
}

/* POSIX filesystem vtable，绑定 read/write/exists/list/mkdir/remove/destroy。 */
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
 * 创建 POSIX filesystem 端口。
 *
 * 成功后 out_fs 获得 self/vtable，调用方通过 vtable->destroy 释放。该端口不内置安全策略，
 * 保持平台层职责单一。
 */
cc_result_t cc_filesystem_get_posix(cc_filesystem_t *out_fs)
{
    if (!out_fs) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null filesystem output");
    }
    memset(out_fs, 0, sizeof(*out_fs));
    cc_posix_filesystem_t *self = calloc(1, sizeof(cc_posix_filesystem_t));
    if (!self) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create posix filesystem");

    out_fs->self = self;
    out_fs->vtable = &posix_vtable;
    return cc_result_ok();
}

/* 当前 POSIX profile 的默认 filesystem 就是 POSIX filesystem。 */
cc_result_t cc_filesystem_get_default(cc_filesystem_t *out_fs)
{
    return cc_filesystem_get_posix(out_fs);
}
