/**
 * 学习导读：cclaw/core/src/core/cc_result.c
 *
 * 所属层次：核心层。
 * 阅读重点：这里定义 Agent 运行时的数据模型、主循环和通用工具，阅读时重点看所有权、错误返回和 ReAct 数据流。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

#define _POSIX_C_SOURCE 200809L

/**
 * cc_result.c — 统一错误返回类型模块
 *
 * 模块在整体架构中的角色：
 *   本模块是 c-claw 框架 Core 层的基础设施模块。它定义了一个统一的
 *   cc_result_t 带内联错误返回类型（类似 Rust 的 Result<T, E>），使得
 *   所有 Core 层 API 都能以一致的方式返回成功或失败信息，而无需依赖
 *   C 语言传统的"返回错误码 + 输出参数"或更差的"返回 NULL 表示错误"模式。
 *
 *   这是整个框架的"错误传递血管"——没有这个模块，每个子模块都需要
 *   自行定义错误传递机制，导致 API 不一致和调用者困惑。
 *
 * 依赖的其他模块：
 *   - cc_result.h — 定义 cc_result_t 结构体和 cc_error_code_t 枚举
 *   - POSIX 标准 (_POSIX_C_SOURCE 200809L) — 确保 vsnprintf 可用
 *   - 标准库 (stdlib.h, string.h, stdio.h, stdarg.h)
 *
 * 被哪些模块使用：
 *   Core 层所有模块（message, session, tool_call, storage 等）都依赖本模块
 *   作为统一的错误传递机制。上层 runtime、app 和 gateway也通过本模块接收错误信息，
 *   并使用 cc_error_string() 将错误码转换为面向用户的文本。
 *   任何需要返回"成功或失败"信息的函数都可以使用 cc_result_t。
 *
 * 错误处理模式（框架中的两种标准用法）：
 *   模式一：检查 code（最常用）
 *     cc_result_t rc = some_function();
 *     if (rc.code != CC_OK) {
 *         cc_log_error("操作失败: %s", rc.message);
 *         cc_result_free(&rc);
 *         return rc; // 向上传播
 *     }
 *     // 继续正常流程...
 *     cc_result_free(&rc);
 *
 *   模式二：直接传播（适用于简单的包装函数）
 *     cc_result_t rc = inner_function();
 *     if (rc.code != CC_OK) return rc;  // message 的所有权转移给上层
 *     // 继续正常流程...
 *
 *   模式三：成功时无操作释放（适用无需传播的场景）
 *     cc_result_t rc = some_function();
 *     if (rc.code == CC_OK) handle_success();
 *     cc_result_free(&rc); // 无论成功失败都调用，因为成功时 message=NULL
 *
 * 设计决策（为什么这样设计）：
 *   1. 使用栈分配的 struct（而非堆分配指针）作为返回值。
 *      为什么：避免调用者关心释放问题——只有 message 字段是堆分配的字符串，
 *      需要显式调用 cc_result_free。struct 本身 <= 16 字节，按值返回的成本
 *      远低于一次 malloc 调用。GCC/Clang 在 x86-64 上会将 <=16 字节的结构体
 *      通过寄存器返回（rdx:rax），与返回 int 开销相同。
 *   2. 成功的 result 可以零开销使用——cc_result_ok() 返回一个全零结构体，
 *      message 为 NULL，无需任何堆分配。
 *      为什么：大多数 API 调用都会成功，优化成功路径的延迟是最有价值的优化。
 *      按照 Amdahl 定律，优化常见路径的收益远大于优化罕见路径。
 *   3. 错误 result 通过 strdup 拷贝消息字符串。
 *      为什么：确保调用者的栈字符串不会被悬空引用。调用者传入 char buf[256]
 *      这类局部缓冲区也是安全的。持有独立副本是防御性编程的最低要求。
 *   4. 提供 cc_result_errf() 格式化接口，方便构造带上下文的错误消息。
 *      为什么：运行时错误通常需要嵌入变量值（如文件名、函数名），printf 风格的
 *      格式化是 C 程序员最熟悉的方式，学习成本为零。
 *      实现使用两阶段 vsnprintf 确保安全性（详见函数注释）。
 *   5. error code 和 message 分离存储。
 *      为什么：code 用于程序化判断（if (rc.code != CC_OK)），message 用于
 *      日志输出和用户展示，两者服务不同消费者，不应混合。
 *      code 是小整数（枚举），可以直接用 switch 分支处理；message 是可变字符串，
 *      适合人类阅读。分离后可以分别优化各自的消费者。
 *   6. 为什么 code=0 表示成功（CC_OK=0）：
 *      与 Unix/POSIX 惯例一致（EXIT_SUCCESS = 0），也与 C 语言的 false/0/失败
 *      的等价关系一致。非零值更自然地表示多种不同的错误类别。
 */

#include "cc/core/cc_result.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

/*
 * cc_result_ok - 创建表示成功的 cc_result_t
 *
 * 功能：
 *   构造一个代表"操作成功"的 cc_result_t 值。code 设为 CC_OK (0)，
 *   message 设为 NULL。这是调用者判断成功与否的标准方式：
 *   if (result.code == CC_OK) { ... }。
 *
 *   这是整个框架中最常用的函数之一——几乎所有返回 cc_result_t 的
 *   函数在成功路径上的最后一行都是 return cc_result_ok()。
 *
 * @return code = CC_OK (0), message = NULL 的 cc_result_t 结构体。
 *         该结构体完全在栈上分配（编译器可能通过寄存器优化掉内存操作），
 *         无需调用 cc_result_free。
 *
 * 典型用法：
 *   cc_result_t rc = cc_result_ok();
 *   return rc; // 或者直接 return cc_result_ok();
 *
 * 为什么是值返回而非指针返回：
 *   cc_result_t 在 x86-64 上为 16 字节（8 字节 code + 8 字节指针），
 *   正好填满 rdx:rax 两个寄存器。值返回避免了 malloc 调用和指针解引用，
 *   是零开销抽象。调用者可以直接 result.code 访问字段，无指针语法噪音。
 *
 * 为什么 message 为 NULL：
 *   NULL 是 free() 的安全参数——调用者可以"无脑"调用 cc_result_free(&result)，
 *   不需要先判断是否成功。这降低了错误传播路径上的代码复杂度。
 *
 * 为什么不需要初始化 message 为任何值：
 *   GCC/Clang 的结构体初始化 {CC_OK, NULL} 保证所有未指定字段为零。
 *   code 显式指定为 CC_OK (0)，message 显式指定为 NULL。这是 C99 指定的
 *   结构体初始化语法，比 memset 更精确（编译器可以验证字段名）。
 */
cc_result_t cc_result_ok(void)
{
    cc_result_t result = {CC_OK, NULL};
    return result;
}

/*
 * cc_result_error - 创建带固定错误消息的 cc_result_t
 *
 * 功能：
 *   构造一个代表"操作失败"的 cc_result_t 值。code 设为由调用者指定的
 *   错误码，message 通过 strdup 拷贝传入的错误描述字符串。
 *
 *   这是创建错误返回值的标准方式——比 cc_result_errf 更简单（无需格式化），
 *   适用于错误描述固定或不需要嵌入运行时上下文的场景。
 *
 * 参数:
 *   @param code    - 错误码，必须是 CC_ERR_* 枚举值之一，传入 CC_OK 会产生
 *                    语义矛盾（虽然函数本身不会阻止）
 *   @param message - 人类可读的错误描述字符串，可以为 NULL。
 *                    如果为 NULL，message 字段也为 NULL。
 *                    如果非 NULL，通过 strdup 创建堆上的独立副本。
 *                    为什么需要副本：调用者可能在栈上构造临时字符串（如
 *                    sprintf(buf, ...)），函数返回后该缓冲区可能被覆写。
 *                    持有独立副本消除了这种悬空引用风险。
 *
 * @return 包含指定错误码和消息副本的 cc_result_t。
 *         message 通过 strdup 堆分配，调用者必须在不再需要时调用
 *         cc_result_free 释放 message，否则会内存泄漏。
 *
 * 典型用法（日志 + 中断）：
 *   cc_result_t rc = cc_result_error(CC_ERR_NOT_FOUND, "Session not found");
 *   cc_log_error("Lookup failed: %s", rc.message);
 *   cc_result_free(&rc);
 *
 * 典型用法（错误传播）：
 *   // 在包装函数中构造错误并向上传递
 *   return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null session pointer");
 *
 * 为什么 strdup 返回 NULL 时不做特殊处理：
 *   如果 strdup 返回 NULL（系统内存耗尽），message 字段将是 NULL。
 *   这是可接受的降级行为——调用者仍可通过 code 判断错误类型，
 *   只是丢失了人类可读的错误描述。在 OOM（Out Of Memory）场景下，
 *   尝试产生另一个错误消息本身就是不可能的。
 */
cc_result_t cc_result_error(cc_error_code_t code, const char *message)
{
    cc_result_t result;
    result.code = code;
    result.message = message ? strdup(message) : NULL;
    return result;
}

/*
 * cc_result_errf - 创建带格式化错误消息的 cc_result_t
 *
 * 功能：
 *   与 cc_result_error 相同，但消息通过 printf 风格的格式化字符串构造，
 *   允许在错误消息中嵌入运行时上下文信息（如变量名、文件名、行号等）。
 *
 *   这是最灵活的错误构造方式——运行时错误往往需要嵌入具体参数来
 *   提供诊断上下文。例如 "Tool 'http.request' not found" 比 "Tool not found"
 *   更有利于调试和日志分析。
 *
 * 参数:
 *   @param code - 错误码，必须是 CC_ERR_* 枚举值之一
 *   @param fmt  - printf 风格的格式化字符串，可以为 NULL（此时 message 为 NULL）
 *                 为什么允许 NULL：某些错误不需要附加消息，只需 code 即可表达。
 *   @param ...  - 格式化参数，与 fmt 中的格式说明符一一对应。
 *                 类型必须与格式说明符匹配，否则是未定义行为。
 *
 * @return 包含指定错误码和格式化消息的 cc_result_t。
 *
 * 典型用法：
 *   return cc_result_errf(CC_ERR_NOT_FOUND, "Tool '%s' not registered", tool_name);
 *   return cc_result_errf(CC_ERR_IO, "Failed to read %zu bytes from '%s'", size, path);
 *   return cc_result_errf(CC_ERR_JSON, "Invalid JSON at line %d: %s", line, detail);
 *
 * 实现细节（两阶段格式化模式）：
 *   阶段1: vsnprintf(NULL, 0, fmt, args) — 计算所需缓冲区大小
 *           vsnprintf 在 C99 标准中定义：当 bufsz=0 且 buf=NULL 时，
 *           函数只计算输出长度而不写入任何内容。返回值为"如果缓冲区
 *           足够大则会被写入的字符数"（不含终止 null）。
 *           为什么这样做：避免猜测缓冲区大小——字符串长度可能远超预期。
 *   阶段2: malloc(len+1) + vsnprintf(buf, len+1, fmt, args) — 实际写入
 *           len+1 是因为 vsnprintf 的返回值不含终止 null。
 *           如果 malloc 返回 NULL（OOM），跳过阶段2的 vsnprintf 调用，
 *           message 保持为 NULL，调用者仍可通过 code 获取错误类型。
 *
 *   va_copy 的使用（为什么需要两次 va_start）：
 *   变参列表在第一次 vsnprintf 后被消耗，不能再传递给第二次 vsnprintf。
 *   因此需要 va_end 后再 va_start 重新初始化。这是 C 语言 va_list
 *   的标准使用模式。注意：GCC 在某些平台上 va_list 是引用类型，
 *   可以直接复用（不需要 va_end/va_start），但为保持最大可移植性，
 *   本实现遵循标准的两次 va_start 模式。
 *
 *   如果 fmt 为 NULL，message 字段设置为 NULL，且不执行任何格式化操作。
 *   这等价于 cc_result_error(code, NULL) 的语义。
 *
 * 为什么不用 vasprintf：
 *   vasprintf 是 GNU 扩展，非 POSIX 标准函数，在 musl libc 等环境下
 *   不可用。为保持最大可移植性，手动实现两阶段 vsnprintf 是更可靠的选择。
 *
 * 与 cc_result_error 的选择指南：
 *   - 错误消息是固定字符串 → 使用 cc_result_error（更简洁，无格式化开销）
 *   - 错误消息需要嵌入变量 → 使用 cc_result_errf（动态构造上下文）
 */
cc_result_t cc_result_errf(cc_error_code_t code, const char *fmt, ...)
{
    cc_result_t result;
    result.code = code;
    if (fmt) {
        va_list args;

        /* 阶段1: 计算所需缓冲区大小（不分配、不写入） */
        va_start(args, fmt);
        int len = vsnprintf(NULL, 0, fmt, args);
        va_end(args);

        /* 阶段2: 分配缓冲区并实际写入格式化内容 */
        result.message = malloc(len + 1);
        if (result.message) {
            va_start(args, fmt);
            vsnprintf(result.message, len + 1, fmt, args);
            va_end(args);
        }
        /* 如果 malloc 返回 NULL，result.message 保持 NULL，
           调用者仍可通过 result.code 获取错误类型，只是丢失了描述信息 */
    } else {
        result.message = NULL;
    }
    return result;
}

/*
 * cc_result_free - 释放 cc_result_t 的堆分配资源
 *
 * 功能：
 *   释放 cc_result_t 中 message 字段指向的堆内存。不释放 cc_result_t 结构体
 *   本身（因为它通常是栈分配的，由编译器自动管理生命周期）。
 *
 *   这是 cc_result_t 生命周期管理的出口——每个通过 cc_result_error 或
 *   cc_result_errf 创建的错误 result 都必须最终调用此函数释放。
 *
 * 参数:
 *   @param result - 指向要释放内部资源的 cc_result_t 的指针。
 *                   可以为 NULL（安全无操作），方便在清理路径上无脑调用。
 *
 * 行为:
 *   释放 message 字段指向的堆内存，并将 message 置为 NULL。
 *   注意：不释放 cc_result_t 结构体本身（它通常是栈分配的）。
 *   对于 cc_result_ok() 创建的成功 result（message == NULL），此函数是无操作。
 *
 * 典型使用模式（错误传播）：
 *   cc_result_t rc = inner_function();
 *   if (rc.code != CC_OK) {
 *       // 方案A：记录日志后传播——message 所有权转移给上层
 *       cc_log_error("inner failed: %s", rc.message);
 *       return rc; // 上层负责 cc_result_free
 *
 *       // 方案B：包装错误后传播——需要释放原 message 再构造新错误
 *       cc_result_t wrapped = cc_result_errf(CC_ERR_STORAGE,
 *           "Save session failed: %s", rc.message);
 *       cc_result_free(&rc); // 原 message 已消费，必须释放
 *       return wrapped;
 *   }
 *   cc_result_free(&rc); // 成功路径上 message 为 NULL，此调用无害
 *
 * 典型使用模式（最终消费——不再传播）：
 *   cc_result_t rc = some_function();
 *   if (rc.code != CC_OK) {
 *       fprintf(stderr, "Error: %s\n", rc.message);
 *   }
 *   cc_result_free(&rc); // 无论成功还是失败都调用，message 为 NULL 时无害
 *                         // 这是推荐的"安全释放"模式——始终在作用域末尾调用
 *
 * 为什么在判断成功/失败后才释放（而非统一在末尾释放）：
 *   在某些场景下，调用者需要将 message 向上传播（return rc），
 *   此时不应释放 message——所有权转移给了上层调用者。
 *   在其他场景下，调用者是错误的最终消费者（不需要传播），
 *   此时必须在判断后立即释放。
 *   判断"是否需要传播"是调用者的职责，本函数只提供释放能力。
 *
 * 为什么接受 NULL 指针：
 *   "安全释放"模式——调用者可以在任何清理路径上调用此函数，无需
 *   先检查 result 指针是否有效。例如在多分支错误处理中，某些分支
 *   可能使用了局部变量的地址而另一些分支使用堆分配指针，统一调用
 *   cc_result_free(result_ptr) 比在每个分支中加 if 判断更简洁。
 *
 * 为什么释放后置 NULL：
 *   防止 double-free 问题——如果调用者不小心对同一个 cc_result_t
 *   调用了两次 cc_result_free，第二次 free(NULL) 是安全的无操作，
 *   不会导致程序崩溃或堆损坏。这是防御性编程的最低要求。
 */
void cc_result_free(cc_result_t *result)
{
    if (result && result->message) {
        free(result->message);
        result->message = NULL;
    }
}

/*
 * cc_error_string - 将错误码转换为人类可读的字符串
 *
 * 功能：
 *   将 cc_error_code_t 枚举值映射为简短的英文描述字符串。主要用于日志输出、
 *   调试信息和面向开发者的错误展示（不面向最终用户）。
 *
 *   这是错误码的"人类可读化"——在日志、stderr 输出、调试器中看到
 *   "Network error" 比看到 "7" 要好理解得多。
 *
 * 参数:
 *   @param code - cc_error_code_t 枚举值，可以是 CC_OK 或任何 CC_ERR_*
 *
 * @return 指向静态字符串常量（只读）的指针，表示错误码的英文描述。
 *         返回值不需要调用者释放，生命周期等同于程序运行期。
 *         对于未识别的 code 返回 "Unknown"。
 *
 * 典型用法：
 *   fprintf(stderr, "Error: %s\n", cc_error_string(rc.code));
 *
 * 典型用法（日志宏中）：
 *   #define CC_LOG_RESULT(rc) \
 *       if ((rc).code != CC_OK) \
 *           cc_log_error("[%s] %s", cc_error_string((rc).code), (rc).message)
 *
 * 为什么用 switch 而非查找表（数组）：
 *   对于 15 个以内的条目，switch 分支和数组查找的性能差异可忽略，
 *   但 switch 的可读性和维护性更好。每个 case 标签明确关联了枚举值
 *   和字符串，新增条目时不容易出现数组索引偏移错误。
 *   编译器通常会将连续值的 switch 优化为跳转表，与数组查找性能相同。
 *
 * default 分支的设计哲学：
 *   返回 "Unknown" 而非 NULL 或 abort()——安全降级策略。
 *   如果未来增加了新的错误码但忘记更新此函数，至少不会返回空指针
 *   导致崩溃。返回 "Unknown" 确保日志中始终有可读信息，开发者可以
 *   通过"Unknown"关键字搜索到遗漏的错误码位置。
 *
 * 本地化（i18n）考虑：
 *   此函数不处理本地化，始终返回英文字符串，面向开发者而非最终用户。
 *   如果需要面向用户展示错误信息，应在 gateway 或 UI 层做本地化映射——
 *   将 code 映射到用户语言的描述，将 message 作为详细信息展示。
 *   保持 Core 层的字符串为英文有助于全球化协作和日志分析（日志中
 *   出现中文可能在非 UTF-8 终端上显示乱码）。
 */
const char *cc_error_string(cc_error_code_t code)
{
    switch (code) {
    case CC_OK:                  return "OK";
    case CC_ERR_UNKNOWN:         return "Unknown error";
    case CC_ERR_INVALID_ARGUMENT: return "Invalid argument";
    case CC_ERR_OUT_OF_MEMORY:   return "Out of memory";
    case CC_ERR_NOT_FOUND:       return "Not found";
    case CC_ERR_PERMISSION_DENIED: return "Permission denied";
    case CC_ERR_IO:              return "I/O error";
    case CC_ERR_NETWORK:         return "Network error";
    case CC_ERR_JSON:            return "JSON error";
    case CC_ERR_TIMEOUT:         return "Timeout";
    case CC_ERR_CANCELLED:       return "Cancelled";
    case CC_ERR_MODEL:           return "Model error";
    case CC_ERR_TOOL:            return "Tool error";
    case CC_ERR_STORAGE:         return "Storage error";
    case CC_ERR_PLATFORM:        return "Platform error";
    default:                     return "Unknown";
    }
}