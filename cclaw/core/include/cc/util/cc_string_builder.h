/**
 * 学习导读：cclaw/core/include/cc/util/cc_string_builder.h
 *
 * 所属层次：核心层。
 * 阅读重点：这里声明 string builder，重点看 append/steal/destroy 的所有权迁移
 *           和 OOM 传播方式。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * cc_string_builder.h — 高效字符串拼接模块
 *
 * @file    cc/util/cc_string_builder.h
 * @brief   提供动态增长的字符串缓冲区，用于高效拼接大量文本。
 *
 * 在 C 语言中，多次 strcat 会导致 O(n^2) 的时间复杂度，
 * 因为每次都需要重新扫描整个字符串。cc_string_builder_t 通过
 * 预分配 + 容量翻倍策略，将拼接复杂度降为均摊 O(n)。
 *
 * ─── 接口契约 ─────────────────────────────────────────────────────────
 *
 *   - 通过 cc_string_builder_init() 初始化（必须在栈或堆上分配结构体）
 *   - 使用 append/appendf 追加内容
 *   - cc_string_builder_take() 获取最终字符串并转移所有权
 *   - cc_string_builder_deinit() 释放内部缓冲区（即使未 take）
 *   - 支持链式调用风格的 append 操作
 *
 * ─── 典型用法 ─────────────────────────────────────────────────────────
 *
 *   cc_string_builder_t sb;
 *   cc_string_builder_init(&sb);
 *   cc_string_builder_append(&sb, "Hello");
 *   cc_string_builder_append(&sb, " World");
 *   char *result = cc_string_builder_take(&sb);  // result = "Hello World"
 *   // sb 空了，无需 deinit
 *   free(result);
 *
 * ─── 依赖 ─────────────────────────────────────────────────────────────
 *
 *   依赖 cc/core/cc_result.h 和 <stddef.h>。
 */

#ifndef CC_STRING_BUILDER_H
#define CC_STRING_BUILDER_H

#include "cc/core/cc_result.h"
#include <stddef.h>

/**
 * cc_string_builder_t — 字符串构建器结构体
 *
 * 维护一个动态增长的字符缓冲区，支持高效的字符串拼接。
 * 不透明的内部字段（data, length, capacity）不应被外部直接修改，
 * 应通过本模块提供的函数操作。
 */
typedef struct cc_string_builder {
    char *data;       /**< 内部字符缓冲区，以 '\0' 结尾。
                       *   总是有效字符串（即使 length=0，data=""）。 */
    size_t length;    /**< 当前字符串长度（不含结尾 '\0'） */
    size_t capacity;  /**< 缓冲区容量（含结尾 '\0' 的空间）。
                       *   当 length + 1 >= capacity 时触发扩容。 */
} cc_string_builder_t;

/**
 * cc_string_builder_init — 初始化字符串构建器
 *
 * 分配初始缓冲区（容量 64 字节），将 length 设为 0，data 置为空串。
 * 调用方必须在 cc_string_builder_t 生命周期结束时
 * 调用 cc_string_builder_deinit() 或 cc_string_builder_take()。
 *
 * @param sb  要初始化的构建器指针（不可为 NULL）
 * @return    CC_OK 表示成功，CC_ERR_OUT_OF_MEMORY 表示分配失败
 */
cc_result_t cc_string_builder_init(cc_string_builder_t *sb);

/**
 * cc_string_builder_append — 追加 C 字符串
 *
 * 将 text 追加到当前内容的末尾。如果缓冲区不足，自动扩容（容量翻倍）。
 *
 * @param sb    构建器指针（不可为 NULL）
 * @param text  要追加的文本（不可为 NULL）
 * @return      CC_OK 表示成功
 */
cc_result_t cc_string_builder_append(cc_string_builder_t *sb, const char *text);

/**
 * cc_string_builder_appendf — 追加格式化字符串
 *
 * 类似 printf，将格式化后的文本追加到缓冲区末尾。
 * 内部使用 vsnprintf 实现，自动处理扩容。
 *
 * @param sb   构建器指针（不可为 NULL）
 * @param fmt  格式化字符串（printf 风格，不可为 NULL）
 * @param ...  变长参数列表
 * @return     CC_OK 表示成功
 */
cc_result_t cc_string_builder_appendf(cc_string_builder_t *sb, const char *fmt, ...);

/**
 * cc_string_builder_append_char — 追加单个字符
 *
 * 在末尾追加一个字符。比 append 一个长度 1 的字符串更高效。
 *
 * @param sb  构建器指针（不可为 NULL）
 * @param c   要追加的字符
 * @return    CC_OK 表示成功
 */
cc_result_t cc_string_builder_append_char(cc_string_builder_t *sb, char c);

/**
 * cc_string_builder_take — 获取最终字符串并转移所有权
 *
 * 返回 data 缓冲区的内容，同时将构建器重置为空状态。
 * 调用方获得返回的字符串的所有权，需要手动 free()。
 * take 之后不需要调用 deinit（构建器已重置）。
 *
 * @param sb  构建器指针（不可为 NULL）
 * @return    最终构建的字符串（需要调用方 free），失败返回 NULL
 */
char *cc_string_builder_take(cc_string_builder_t *sb);

/**
 * cc_string_builder_deinit — 释放字符串构建器的内部资源
 *
 * 释放内部 data 缓冲区，并将所有字段清零。
 * 如果之前已经调用过 take()（data 所有权已转移），
 * 此函数是安全的（等同于无操作）。
 * 传入 NULL 是安全的。
 *
 * @param sb  要释放的构建器指针
 */
void cc_string_builder_deinit(cc_string_builder_t *sb);

/**
 * cc_string_builder_clear — 清空构建器内容而不释放缓冲区
 *
 * 功能：将 length 重置为 0，但保留已分配的缓冲区。
 *       这样后续 append 操作可以复用已有内存，避免反复分配。
 *
 * @param sb  字符串构建器指针
 */
void cc_string_builder_clear(cc_string_builder_t *sb);

/**
 * cc_string_builder_cstr — 获取当前构建内容的只读 C 字符串
 *
 * 返回 data 缓冲区的只读指针，不转移所有权。
 * 返回的指针仅在下次 append 操作之前有效（append 可能触发 realloc）。
 *
 * @param sb  构建器指针（不可为 NULL）
 * @return    当前内容的 C 字符串（只读，不要释放）
 */
const char *cc_string_builder_cstr(const cc_string_builder_t *sb);

#endif
