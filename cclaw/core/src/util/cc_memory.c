/**
 * 学习导读：cclaw/core/src/util/cc_memory.c
 *
 * 所属层次：核心层。
 * 阅读重点：这里封装内存辅助函数，重点看和标准库一致的语义、NULL 安全性
 *           以及受限平台下的可替换边界。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * cc_memory.c — 统一内存管理包装层
 *
 * 在整体架构中的角色和层次：
 *   本模块位于 Util 层，是对 C 标准库 malloc/calloc/realloc/free/strdup
 *   的薄包装层。Util 层位于 Platform 层之上，为上层业务模块提供通用工具。
 *   本模块是项目中所有动态内存分配的"守门人"——上层模块应通过本模块进行
 *   所有内存分配，而非直接调用标准 C 库函数。
 *   通过统一包装，实现了全局内存使用量追踪，为性能分析和泄漏检测提供基础数据。
 *
 * 包装层存在的理由（Wrapper Rationale）：
 * ─────────────────────────────────────────
 *   为什么要包装标准 C 库的内存函数？
 *
 *   1. 全局追踪（Global Tracking）：
 *      通过 g_allocated 计数器，可以在程序运行时查询累计分配量。
 *      这对于性能调优（"为什么我的程序吃了 2GB 内存？"）、
 *      内存预算控制（"限制单次请求不超过 X MB"）和离线分析都很有价值。
 *      直接使用 malloc 无法获得这些信息。
 *
 *   2. 统一扩展点（Single Allocation Boundary）：
 *      所有上层模块通过同一组包装函数分配和释放内存，诊断能力可以集中在
 *      本模块维护，例如累计分配计数、分配失败日志、内存池、限额控制或
 *      调试 guard bytes。上层直接散落 malloc/free 会让这些能力难以统一。
 *
 *   3. 可测试性（Testability）：
 *      通过统一的分配接口，在单元测试中可以注入 Mock 分配器
 *      （如模拟 OOM 场景：让第 N 次分配返回 NULL）。
 *      直接调用 malloc 无法在测试中控制分配行为。
 *
 *   4. 代码一致性（Code Consistency）：
 *      项目规范要求所有堆分配通过 cc_* 系列函数完成。
 *      这便于 code review 中快速识别所有内存分配点，
 *      也便于静态分析工具（如 clang-tidy）统一检查。
 *
 * 核心设计：
 *   - 薄包装层（thin wrapper）：每个函数在调用对应标准库函数后，将分配
 *     大小累加到全局计数器 g_allocated，其余行为与标准库完全一致
 *   - 累计分配量追踪：g_allocated 记录自程序启动以来的累计分配字节数，
 *     不因 free 调用而减少。这提供了"内存吞吐量"的宏观视图
 *   - 单线程设计：g_allocated 使用普通 size_t 变量，非原子操作，
 *     仅在单线程环境下安全。如需要多线程支持，需改为 _Atomic size_t
 *   - 与标准库接口一致：函数签名直接对应标准库函数，降低迁移成本
 *
 * 设计决策：
 *   - cc_free 不减少计数器：简化实现，避免线程安全问题（精确追踪需要
 *     原子操作或锁）。精确的内存泄漏检测应使用 valgrind 或 AddressSanitizer
 *   - g_allocated 语义是"累计请求量"而非"当前占用量"：这意味着对于
 *     realloc，计数器累加新大小而非 (新大小 - 旧大小)。
 *     这种设计便于评估程序的内存分配频率和总量，但不直接反映当前内存压力
 *   - 不添加错误处理：分配失败时不输出日志（日志模块可能依赖本模块），
 *     将错误处理责任完全交给调用者，保持接口简洁
 *   - 单独包装 calloc：虽然等同于 cc_malloc + memset，但操作系统对 calloc
 *     有零页优化（overcommit 系统下 calloc 可能更高效）
 *
 * 分配追踪的局限性说明：
 * ──────────────────────────
 *   g_allocated 提供的是"累计分配字节数"，用于宏观的性能评估。
 *   它不能替代专业的内存泄漏检测工具。以下场景 g_allocated 无法检测：
 *     - 内存泄漏：free 不减少计数器，无法判断是否所有已分配内存都被释放
 *     - 当前内存占用：计数器只增不减，无法知道当前时刻的实际内存使用量
 *     - 碎片化程度：计数器不反映内存碎片信息
 *
 *   对于精确的内存分析，请使用：
 *     - valgrind --leak-check=full    （泄漏检测）
 *     - AddressSanitizer (ASan)       （越界访问 + 泄漏检测）
 *     - heaptrack / massif            （堆分析）
 *
 * 依赖：
 *   - cc/util/cc_memory.h — 本模块的公共接口声明
 *   - 标准 C 库 — stdlib（malloc/calloc/realloc/free）、string（strdup/strlen）
 */

#include "cc/util/cc_memory.h"
#include <stdlib.h>
#include <string.h>

/*
 * g_allocated — 全局内存分配计数器
 *
 * 记录自程序启动以来通过 cc_malloc/cc_calloc/cc_strdup/cc_realloc 成功分配的总字节数。
 * 注意：cc_free 不会减少此计数器，因此该值表示累计分配量而非当前占用量。
 *       用于性能调优和粗略的内存使用评估，精确泄漏检测请使用 valgrind 等专业工具。
 */
static size_t g_allocated = 0;

/*
 * cc_malloc — 分配指定大小的内存块
 *
 * 功能：包装标准 malloc，在成功分配后将分配大小累加到全局计数器。
 *
 * 参数：
 *   size — 需要分配的字节数
 *
 * 返回值：
 *   分配成功 — 返回指向新内存块的指针
 *   分配失败 — 返回 NULL（不会设置 errno，与 malloc 行为一致）
 *
 * 设计决策：不对分配失败做额外处理（如日志输出），将错误处理责任交给调用者，
 *           保持与标准 malloc 接口的一致性。
 */
void *cc_malloc(size_t size)
{
    void *ptr = malloc(size);
    if (ptr) g_allocated += size;
    return ptr;
}

/*
 * cc_calloc — 分配并零初始化内存块
 *
 * 功能：包装标准 calloc，分配 count * size 字节并全部置零。
 *       成功后将 count * size 累加到全局计数器。
 *
 * 参数：
 *   count — 元素个数
 *   size  — 每个元素的大小（字节）
 *
 * 返回值：
 *   分配成功 — 返回指向零初始化内存块的指针
 *   分配失败 — 返回 NULL
 *
 * 设计决策：单独包装 calloc 而非让调用者用 cc_malloc + memset，
 *           因为 calloc 在操作系统层面可能有更高效的零页分配优化。
 */
void *cc_calloc(size_t count, size_t size)
{
    void *ptr = calloc(count, size);
    if (ptr) g_allocated += count * size;
    return ptr;
}

/*
 * cc_strdup — 复制字符串到堆上
 *
 * 功能：包装标准 strdup，为源字符串分配足够内存并复制内容。
 *       成功后将 strlen(src) + 1 累加到全局计数器。
 *       如果 src 为 NULL，直接返回 NULL 而不分配内存。
 *
 * 参数：
 *   src — 源字符串指针（可为 NULL）
 *
 * 返回值：
 *   成功 — 返回指向新分配字符串的指针
 *   src 为 NULL — 返回 NULL
 *   分配失败 — 返回 NULL
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
 * cc_realloc — 调整已分配内存块的大小
 *
 * 功能：包装标准 realloc，将 ptr 指向的内存块调整为 size 字节。
 *       成功后将新大小 size 累加到全局计数器。
 *
 * 参数：
 *   ptr  — 指向已分配内存块的指针（若为 NULL，行为同 malloc）
 *   size — 新的内存块大小（字节）
 *
 * 返回值：
 *   成功 — 返回指向调整后内存块的指针（可能与原指针不同）
 *   失败 — 返回 NULL，原内存块保持不变
 *
 * 注意事项：此处的计数策略是将新大小直接累加，不减去旧大小。
 *           因此对于 realloc 场景，g_allocated 的语义是"累计请求量"而非"净占用量"。
 */
void *cc_realloc(void *ptr, size_t size)
{
    void *new_ptr = realloc(ptr, size);
    if (new_ptr) g_allocated += size;
    return new_ptr;
}

/*
 * cc_free — 释放动态分配的内存
 *
 * 功能：包装标准 free，释放 ptr 指向的内存块。
 *       与 cc_malloc/cc_calloc/cc_realloc 配对使用。
 *
 * 参数：
 *   ptr — 指向待释放内存块的指针（若为 NULL，则无操作）
 *
 * 返回值：无
 *
 * 设计决策：cc_free 不减少 g_allocated 计数器。
 *           这样做是为了保持实现的简单性：线程安全地维护净占用量
 *           需要原子操作或锁，对于本项目的使用场景来说过于复杂。
 *           精确的内存泄漏检测应使用 valgrind 或 AddressSanitizer。
 */
void cc_free(void *ptr)
{
    free(ptr);
}

/*
 * cc_memory_allocated — 查询累计内存分配量
 *
 * 功能：返回自程序启动以来通过本模块所有分配函数成功分配的累计字节数。
 *       这是一个累计值，不会因 cc_free 调用而减少。
 *
 * 参数：无
 *
 * 返回值：
 *   返回 g_allocated 的当前值（size_t）
 *
 * 使用场景：可用于性能分析、粗略的内存使用评估、
 *           或在程序退出时输出内存分配统计信息。
 */
size_t cc_memory_allocated(void)
{
    return g_allocated;
}
