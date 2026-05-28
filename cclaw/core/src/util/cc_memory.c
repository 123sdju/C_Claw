



#include "cc/util/cc_memory.h"
#include <stdlib.h>
#include <string.h>


/* 当前简单 allocator 的分配计数；不做 free 回减，因此更像累计分配指标。 */
static size_t g_allocated = 0;


/*
 * 分配 size 字节。
 *
 * 这是 SDK allocator 的最薄封装，当前直接使用 malloc 并记录累计分配量。保留这一层是
 * 为了嵌入式移植时替换为 arena、内存池或带统计的 allocator。
 */
void *cc_malloc(size_t size)
{
    void *ptr = malloc(size);
    if (ptr) g_allocated += size;
    return ptr;
}


/* 分配并清零 count * size 字节，同时更新累计分配统计。 */
void *cc_calloc(size_t count, size_t size)
{
    void *ptr = calloc(count, size);
    if (ptr) g_allocated += count * size;
    return ptr;
}


/*
 * 复制字符串。
 *
 * NULL 输入保持 NULL，方便大量可选字段复用。成功时复制包含结尾 NUL 的完整缓冲。
 */
char *cc_strdup(const char *src)
{
    if (!src) return NULL;
    size_t len = strlen(src) + 1;
    char *dst = (char *)malloc(len);
    if (dst) {
        memcpy(dst, src, len);
        g_allocated += len;
    }
    return dst;
}


/*
 * 调整缓冲大小。
 *
 * 当前统计只增加新 size，没有扣除旧 size，因此不能用于精确实时 heap 统计；它主要帮助
 * 单元测试和移植阶段观察分配趋势。
 */
void *cc_realloc(void *ptr, size_t size)
{
    void *new_ptr = realloc(ptr, size);
    if (new_ptr) g_allocated += size;
    return new_ptr;
}


/* 释放内存；当前实现不维护分配大小表，因此不更新 g_allocated。 */
void cc_free(void *ptr)
{
    free(ptr);
}


/* 返回累计分配字节数；不是当前仍存活字节数。 */
size_t cc_memory_allocated(void)
{
    return g_allocated;
}
