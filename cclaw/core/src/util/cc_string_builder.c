/**
 * 学习导读：cclaw/core/src/util/cc_string_builder.c
 *
 * 所属层次：核心层。
 * 阅读重点：这里定义 Agent 运行时的数据模型、主循环和通用工具，阅读时重点看所有权、错误返回和 ReAct 数据流。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * cc_string_builder.c — 动态字符串构建器
 *
 * 在整体架构中的角色和层次：
 *   本模块位于 Util 层，是高效字符串拼接的基础设施。
 *   Util 层位于 Platform 层之上，为上层业务模块提供通用工具。
 *   本模块为所有需要大量字符串拼接的场景（如 JSON 序列化、日志格式化、
 *   HTTP 响应构建、LLM prompt 拼接）提供 O(1) 均摊追加性能的字符串构建器。
 *   在 C 语言中，频繁使用 strcat/strncat 会导致 O(N²) 的时间复杂度，
 *   本模块通过预分配 + 倍增扩容策略解决了这一问题。
 *
 * 核心设计：
 *   - 初始容量 256 字节（SB_INITIAL_CAPACITY），兼顾大多数短字符串场景
 *     的内存效率与扩容频率
 *   - 倍增扩容策略（capacity *= 2）：确保追加 N 个字符的均摊时间复杂度
 *     为 O(N)，避免线性增长导致的 O(N²) 退化
 *   - 始终维护末尾 NUL 终止符（\0）：保证与 C 字符串 API 兼容，
 *     可随时通过 cc_string_builder_cstr() 获取标准 C 字符串
 *   - 支持 take 语义（所有权转移）：cc_string_builder_take() 将内部缓冲区
 *     所有权转移给调用者，避免不必要的内存拷贝
 *   - 统一错误处理：所有修改操作通过 cc_result_t 返回错误码，
 *     与项目整体错误处理策略保持一致
 *
 * 设计决策：
 *   - 使用动态分配而非栈上固定数组：允许缓冲区在运行时无界增长，
 *     适合内容长度不可预测的场景（如 LLM 响应）
 *   - cc_string_builder_appendf 使用两遍 vsnprintf 策略：
 *     第一遍 vsnprintf(NULL, 0, ...) 计算所需长度，
 *     第二遍实际写入。这避免了猜测缓冲区大小导致截断的问题，
 *     是 C 标准推荐的格式化字符串安全写入方式
 *   - 对 NULL 输入静默忽略：cc_string_builder_append(NULL) 为空操作，
 *     简化上层调用代码，避免每次调用前都需 NULL 检查
 *   - cc_string_builder_cstr 对空构建器返回 "" 而非 NULL：
 *     避免调用者需要额外的 NULL 检查，提升 API 易用性
 *   - 容量倍增而非线性增长：虽可能造成少量内存浪费（最多约 50%），
 *     但换来的是从 O(N²) 到 O(N) 的性能提升，是经典的时空权衡
 *
 * 使用模式（典型生命周期）：
 *   1. cc_string_builder_init(&sb) — 初始化
 *   2. cc_string_builder_append(&sb, "Hello") — 多次追加
 *   3. cc_string_builder_appendf(&sb, " %d", 42) — 格式化追加
 *   4. char *result = cc_string_builder_take(&sb) — 取出结果（零拷贝）
 *   5. free(result) — 释放结果
 *   或者：
 *   4. const char *view = cc_string_builder_cstr(&sb) — 只读访问
 *   5. cc_string_builder_deinit(&sb) — 销毁资源
 *
 * 依赖：
 *   - cc/core/cc_result.h — 统一结果类型（cc_result_t / cc_result_ok / cc_result_error）
 *   - 标准 C 库 — stdio（vsnprintf）、stdlib（malloc/realloc/free）、
 *     string（memcpy/strlen）、stdarg（va_list/va_start/va_end）
 */

#include "cc/util/cc_string_builder.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* 字符串构建器的初始容量（字节）。
 * 选择 256 作为默认值是基于大多数短字符串场景的权衡：
 * 太小会导致频繁扩容，太大会浪费内存。 */
#define SB_INITIAL_CAPACITY 256

/*
 * cc_string_builder_init — 初始化字符串构建器
 *
 * 功能：分配初始缓冲区（SB_INITIAL_CAPACITY 字节），并将 length/capacity 归位。
 *       调用者需要确保 sb 指向有效的内存位置。
 *
 * 参数：
 *   sb — 指向待初始化的 cc_string_builder_t 结构体的指针
 *
 * 返回值：
 *   cc_result_ok() — 初始化成功
 *   cc_result_error(CC_ERR_OUT_OF_MEMORY) — 内存分配失败
 *
 * 设计决策：使用动态分配的缓冲区而非栈上固定数组，
 *           以便后续通过 realloc 灵活扩容。
 */
cc_result_t cc_string_builder_init(cc_string_builder_t *sb)
{
    sb->data = malloc(SB_INITIAL_CAPACITY);
    if (!sb->data) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to init string builder");
    sb->data[0] = '\0';
    sb->length = 0;
    sb->capacity = SB_INITIAL_CAPACITY;
    return cc_result_ok();
}

/*
 * ensure_capacity — 确保缓冲区有足够空间容纳追加内容（内部辅助函数）
 *
 * 功能：检查当前容量能否容纳 length + additional + 1（含 '\0'）字节。
 *       如果不足，则采用倍增策略扩容（capacity *= 2），直到满足需求为止。
 *
 * 参数：
 *   sb         — 字符串构建器指针
 *   additional — 即将追加的字节数（不含 '\0'）
 *
 * 返回值：
 *   cc_result_ok() — 容量足够或扩容成功
 *   cc_result_error(CC_ERR_OUT_OF_MEMORY) — realloc 失败
 *
 * 设计决策：使用倍增策略而非线性增长，确保追加 N 个字符的
 *           均摊时间复杂度为 O(N)，避免 O(N²) 的退化行为。
 */
static cc_result_t ensure_capacity(cc_string_builder_t *sb, size_t additional)
{
    size_t needed = sb->length + additional + 1;
    if (needed <= sb->capacity) return cc_result_ok();

    size_t new_capacity = sb->capacity * 2;
    while (new_capacity < needed) new_capacity *= 2;

    char *new_data = realloc(sb->data, new_capacity);
    if (!new_data) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to grow string builder");

    sb->data = new_data;
    sb->capacity = new_capacity;
    return cc_result_ok();
}

/*
 * cc_string_builder_append — 向构建器末尾追加 C 字符串
 *
 * 功能：将 text 指向的字符串内容复制到缓冲区末尾，自动维护 '\0' 终止符。
 *       如果 text 为 NULL，则静默跳过（空操作）。
 *
 * 参数：
 *   sb   — 字符串构建器指针
 *   text — 待追加的 C 字符串（可为 NULL）
 *
 * 返回值：
 *   cc_result_ok() — 追加成功（含 text 为 NULL 的情况）
 *   错误码 — 扩容失败时返回 CC_ERR_OUT_OF_MEMORY
 *
 * 设计决策：对 NULL 输入采取静默忽略策略，避免调用者在每次调用前
 *           都需要进行 NULL 检查，简化上层代码。
 */
cc_result_t cc_string_builder_append(cc_string_builder_t *sb, const char *text)
{
    if (!text) return cc_result_ok();
    size_t len = strlen(text);
    cc_result_t rc = ensure_capacity(sb, len);
    if (rc.code != CC_OK) return rc;

    memcpy(sb->data + sb->length, text, len);
    sb->length += len;
    sb->data[sb->length] = '\0';
    return cc_result_ok();
}

/*
 * cc_string_builder_appendf — 按格式化字符串向构建器追加内容
 *
 * 功能：类似 printf，支持 printf 格式说明符。
 *       采用两遍 vsnprintf 策略：第一遍计算所需长度，第二遍实际写入。
 *       这种策略避免了猜测缓冲区大小的问题，保证一次写够。
 *
 * 参数：
 *   sb  — 字符串构建器指针
 *   fmt — printf 风格的格式化字符串
 *   ... — 格式化参数
 *
 * 返回值：
 *   cc_result_ok() — 追加成功
 *   cc_result_error(CC_ERR_UNKNOWN) — 格式化错误（vsnprintf 返回负值）
 *   错误码 — 扩容失败时返回 CC_ERR_OUT_OF_MEMORY
 *
 * 设计决策：采用两遍 vsnprintf 而非猜测固定缓冲区大小，
 *           确保不会发生截断，适合任意长度的格式化输出。
 */
cc_result_t cc_string_builder_appendf(cc_string_builder_t *sb, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int needed = vsnprintf(NULL, 0, fmt, args);
    va_end(args);

    if (needed < 0) return cc_result_error(CC_ERR_UNKNOWN, "Format error");

    cc_result_t rc = ensure_capacity(sb, (size_t)needed);
    if (rc.code != CC_OK) {
        return rc;
    }

    va_start(args, fmt);
    vsnprintf(sb->data + sb->length, sb->capacity - sb->length, fmt, args);
    va_end(args);

    sb->length += (size_t)needed;
    return cc_result_ok();
}

/*
 * cc_string_builder_append_char — 向构建器末尾追加单个字符
 *
 * 功能：追加单个 ASCII/UTF-8 字节，并维护 '\0' 终止符。
 *       对于多字节 UTF-8 字符，需要调用者自行拆分后多次调用。
 *
 * 参数：
 *   sb — 字符串构建器指针
 *   c  — 待追加的字符
 *
 * 返回值：
 *   cc_result_ok() — 追加成功
 *   错误码 — 扩容失败时返回 CC_ERR_OUT_OF_MEMORY
 */
cc_result_t cc_string_builder_append_char(cc_string_builder_t *sb, char c)
{
    cc_result_t rc = ensure_capacity(sb, 1);
    if (rc.code != CC_OK) return rc;

    sb->data[sb->length++] = c;
    sb->data[sb->length] = '\0';
    return cc_result_ok();
}

/*
 * cc_string_builder_take — 取出内部缓冲区所有权
 *
 * 功能：将内部动态分配的缓冲区指针返回给调用者，并将构建器重置为空状态。
 *       调用者获得缓冲区所有权，需要自行调用 free 释放。
 *       这是一种零拷贝的所有权转移操作。
 *
 * 参数：
 *   sb — 字符串构建器指针
 *
 * 返回值：
 *   返回内部缓冲区指针，调用者负责释放。
 *   如果构建器未初始化或已被 take，则返回 NULL。
 *
 * 注意事项：调用 take 后，构建器处于无效状态，需要重新 init 后才能使用。
 *           不要在 take 之后再调用 append 等操作，除非先重新 init。
 */
char *cc_string_builder_take(cc_string_builder_t *sb)
{
    char *result = sb->data;
    sb->data = NULL;
    sb->length = 0;
    sb->capacity = 0;
    return result;
}

/*
 * cc_string_builder_deinit — 销毁字符串构建器，释放内部缓冲区
 *
 * 功能：释放内部动态分配的缓冲区，并将状态字段重置为零。
 *       调用此函数后不可再使用该构建器，除非重新调用 init。
 *
 * 参数：
 *   sb — 字符串构建器指针
 *
 * 返回值：无
 *
 * 设计决策：与 init 配对使用，遵循 C 语言中常见的 init/deinit 资源管理模式。
 */
void cc_string_builder_deinit(cc_string_builder_t *sb)
{
    free(sb->data);
    sb->data = NULL;
    sb->length = 0;
    sb->capacity = 0;
}

/*
 * cc_string_builder_clear — 清空构建器内容而不释放缓冲区
 *
 * 功能：将 length 重置为 0，但保留已分配的缓冲区。
 *       这样后续 append 操作可以复用已有内存，避免反复分配。
 *
 * 使用场景：
 *   流式循环中每轮迭代后需要清空 builder 的内容，但重复分配/释放
 *   缓冲区是不必要的开销。clear + append 比 deinit + init + append
 *   更高效。
 *
 * 参数：sb — 字符串构建器指针
 */
void cc_string_builder_clear(cc_string_builder_t *sb)
{
    sb->length = 0;
    if (sb->data) sb->data[0] = '\0';
}

/*
 * cc_string_builder_cstr — 获取构建器内容的 C 字符串只读视图
 *
 * 功能：返回内部缓冲区的只读指针，提供与 C 字符串 API 的互操作性。
 *       如果构建器为空（data 为 NULL），则返回空字符串 "" 而非 NULL，
 *       避免调用者需要额外的 NULL 检查。
 *
 * 参数：
 *   sb — 字符串构建器指针（const，保证只读访问）
 *
 * 返回值：
 *   指向内部缓冲区的 const char *，永不为 NULL。
 */
const char *cc_string_builder_cstr(const cc_string_builder_t *sb)
{
    return sb->data ? sb->data : "";
}