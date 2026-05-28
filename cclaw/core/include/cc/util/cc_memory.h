



#ifndef CC_MEMORY_H
#define CC_MEMORY_H

#include <stddef.h>

/*
 * SDK 内存分配封装。
 *
 * 当前实现可直接转发 malloc/calloc/realloc/free，并统计分配量；保留封装是为了 MCU/RTOS
 * 移植时替换为内存池、静态 arena 或调试 allocator。
 */
void *cc_malloc(size_t size);

/* 分配并清零 count * size 字节。 */
void *cc_calloc(size_t count, size_t size);

/* 复制字符串；NULL 输入返回 NULL。 */
char *cc_strdup(const char *src);

/* 调整缓冲大小；语义与 C realloc 一致。 */
void *cc_realloc(void *ptr, size_t size);

/* 释放通过 SDK allocator 分配的内存；允许 NULL。 */
void cc_free(void *ptr);

/* 返回当前统计的已分配字节数；用于测试和嵌入式内存预算观察。 */
size_t cc_memory_allocated(void);

#endif
