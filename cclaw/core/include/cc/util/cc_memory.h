/**
 * 学习导读：cclaw/core/include/cc/util/cc_memory.h
 *
 * 所属层次：核心层。
 * 阅读重点：这里定义 Agent 运行时的数据模型、主循环和通用工具，阅读时重点看所有权、错误返回和 ReAct 数据流。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * cc_memory.h — 内存管理工具模块
 *
 * @file    cc/util/cc_memory.h
 * @brief   提供带追踪的内存分配函数，作为标准 malloc/free 的包装。
 *
 * 本模块封装了标准 C 内存管理函数，额外提供内存使用量追踪功能
 * （通过 cc_memory_allocated() 查询当前已分配的总字节数）。
 * 主要用于发现内存泄漏和资源审计。
 *
 * ─── 接口契约 ─────────────────────────────────────────────────────────
 *
 *   - 所有分配函数的行为与标准 C 对应函数一致
 *   - 失败时返回 NULL（不设置 errno）
 *   - cc_free(NULL) 是安全的（无操作）
 *   - cc_memory_allocated() 返回的数值近似但不保证精确（考虑对齐等开销）
 *
 * ─── 使用建议 ─────────────────────────────────────────────────────────
 *
 *   - 在开发阶段使用本模块的函数以便追踪内存
 *   - 生产环境如需极致性能，可切换回标准 malloc/free（仅需改 include）
 *
 * ─── 依赖 ─────────────────────────────────────────────────────────────
 *
 *   仅依赖 <stddef.h>。不依赖任何 OC 模块。
 */

#ifndef CC_MEMORY_H
#define CC_MEMORY_H

#include <stddef.h>

/**
 * cc_malloc — 分配指定大小的内存块
 *
 * 封装 malloc，同时更新内部分配计数。
 * 内存内容未初始化（可能包含随机值）。
 *
 * @param size  要分配的字节数
 * @return      指向分配内存的指针，失败返回 NULL
 */
void *cc_malloc(size_t size);

/**
 * cc_calloc — 分配并清零内存块
 *
 * 封装 calloc，分配 count * size 字节并全部清零。
 * 同时更新内部分配计数。
 *
 * @param count  元素个数
 * @param size   每个元素的大小（字节）
 * @return       指向已清零内存的指针，失败返回 NULL
 */
void *cc_calloc(size_t count, size_t size);

/**
 * cc_strdup — 复制字符串到新分配的内存
 *
 * 封装 strdup，对 src 做 strlen + malloc + memcpy。
 * 同时更新内部分配计数。与标准 strdup 不同，src 为 NULL 时返回 NULL 而非 crash。
 *
 * @param src  要复制的源字符串（可为 NULL）
 * @return     新分配的字符串副本（需要 cc_free），失败返回 NULL
 */
char *cc_strdup(const char *src);

/**
 * cc_realloc — 重新调整已分配内存块的大小
 *
 * 封装 realloc，同时更新内部分配计数。
 * ptr 为 NULL 时行为同 cc_malloc；size 为 0 时行为同 cc_free。
 *
 * @param ptr   原内存块指针（可为 NULL）
 * @param size  新大小（字节）
 * @return      调整后的内存块指针，可能与 ptr 不同，失败返回 NULL
 */
void *cc_realloc(void *ptr, size_t size);

/**
 * cc_free — 释放内存块
 *
 * 封装 free，同时更新内部分配计数。
 * 传入 NULL 是安全的（无操作）。
 *
 * @param ptr  要释放的内存指针（可为 NULL）
 */
void cc_free(void *ptr);

/**
 * cc_memory_allocated — 查询当前已分配的总内存量
 *
 * 返回自程序启动以来通过本模块分配且尚未释放的内存总字节数。
 * 用于内存泄漏检测和性能监控。
 *
 * @return  当前已分配字节数（近似值）
 */
size_t cc_memory_allocated(void);

#endif