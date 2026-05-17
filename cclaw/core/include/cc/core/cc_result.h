/**
 * 学习导读：cclaw/core/include/cc/core/cc_result.h
 *
 * 所属层次：核心层。
 * 阅读重点：这里定义 Agent 运行时的数据模型、主循环和通用工具，阅读时重点看所有权、错误返回和 ReAct 数据流。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * cc_result.h — 统一错误传递模块
 *
 * @file    cc/core/cc_result.h
 * @brief   定义整个 c-claw 项目中使用的统一结果和错误码类型。
 *
 * 本项目约定几乎所有函数都返回 cc_result_t，而不是通过 errno、
 * 全局变量或异常来传递错误。这种"值语义"的错误处理方式使
 * 调用方必须显式检查返回值，避免遗漏错误处理。
 *
 * ─── 接口契约 ─────────────────────────────────────────────────────────
 *
 *   - 每个可能失败的函数返回 cc_result_t
 *   - CC_OK（code == 0）表示成功
 *   - 非零 code 表示错误，可通过 cc_error_string() 获取可读描述
 *   - 调用方必须在不再需要时调用 cc_result_free() 释放动态资源
 *
 * ─── 使用模式 ─────────────────────────────────────────────────────────
 *
 *   cc_result_t rc = some_function(...);
 *   if (rc.code != CC_OK) {
 *       fprintf(stderr, "Error: %s\n", rc.message);
 *       cc_result_free(&rc);
 *       return;
 *   }
 *   // 成功路径
 *   cc_result_free(&rc);
 *
 * ─── 依赖 ─────────────────────────────────────────────────────────────
 *
 *   仅依赖 <stddef.h>（size_t 定义）。不依赖任何内部模块。
 *   这是整个项目中依赖最少的模块，被所有其他模块引入。
 */

#ifndef CC_RESULT_H
#define CC_RESULT_H

#include <stddef.h>

/**
 * cc_error_code_t — 错误码枚举
 *
 * 定义项目中所有可能的错误类型。按语义分类，便于上层按类别处理。
 * CC_OK（值为 0）是唯一的成功码。
 */
typedef enum cc_error_code {
    CC_OK = 0,               /**< 操作成功完成，无错误 */

    /* ── 通用错误 ────────────────────────────────────────────── */
    CC_ERR_UNKNOWN,           /**< 未知错误：无法归类的异常情况 */
    CC_ERR_INVALID_ARGUMENT,  /**< 无效参数：传入的参数不符合预期（NULL、越界、
                               *   格式不正确等） */
    CC_ERR_OUT_OF_MEMORY,     /**< 内存不足：malloc/realloc 返回 NULL */
    CC_ERR_NOT_FOUND,         /**< 未找到：查找的资源不存在（文件、会话、键值等） */

    /* ── 权限与安全 ──────────────────────────────────────────── */
    CC_ERR_PERMISSION_DENIED, /**< 权限拒绝：操作被策略引擎或系统权限阻止 */

    /* ── I/O 与网络 ──────────────────────────────────────────── */
    CC_ERR_IO,                /**< IO 错误：文件读写失败 */
    CC_ERR_NETWORK,           /**< 网络错误：HTTP 请求失败、连接超时等 */
    CC_ERR_JSON,              /**< JSON 错误：解析或序列化 JSON 失败 */
    CC_ERR_TIMEOUT,           /**< 超时：操作在指定时间内未完成 */

    /* ── 领域相关 ────────────────────────────────────────────── */
    CC_ERR_CANCELLED,         /**< 已取消：操作被用户或系统提前终止 */
    CC_ERR_MODEL,             /**< 模型错误：LLM 返回了无法理解或格式异常的响应 */
    CC_ERR_TOOL,              /**< 工具错误：工具执行失败或返回异常结果 */
    CC_ERR_STORAGE,           /**< 存储错误：数据持久化失败（文件损坏、DB 故障等） */
    CC_ERR_PLATFORM           /**< 平台错误：操作系统 API 调用失败（fork、exec 等） */
} cc_error_code_t;

/**
 * cc_result_t — 统一结果结构体
 *
 * 封装操作结果：成功时为 CC_OK 且 message 为空；
 * 失败时包含非零的错误码和人类可读的错误描述。
 * message 字段由内部 malloc 分配，必须通过 cc_result_free() 释放。
 */
typedef struct cc_result {
    cc_error_code_t code; /**< 错误码，CC_OK（0）表示成功 */
    char *message;        /**< 人类可读的错误描述，成功时为 NULL。
                           *   使用 cc_result_errf() 创建时可包含格式化信息。
                           *   调用 cc_result_free() 时一同释放。 */
} cc_result_t;

/**
 * cc_result_ok — 创建表示成功的 cc_result_t
 *
 * 返回一个 code=CC_OK, message=NULL 的空结果。
 * 无需调用 cc_result_free()（但调用也是安全的）。
 *
 * @return  成功的空结果
 */
cc_result_t cc_result_ok(void);

/**
 * cc_result_error — 创建表示错误的 cc_result_t
 *
 * 根据错误码和消息文本创建结果。message 会被内部拷贝。
 *
 * @param code     错误码（不可为 CC_OK）
 * @param message  错误描述文本（不可为 NULL）
 * @return         包含错误信息的 cc_result_t，调用方需 cc_result_free()
 */
cc_result_t cc_result_error(cc_error_code_t code, const char *message);

/**
 * cc_result_errf — 创建带格式化消息的错误 cc_result_t
 *
 * 类似于 printf，将格式化文本作为错误消息。适用于需要将
 * 动态信息（如文件名、行号）嵌入错误描述的场合。
 *
 * @param code  错误码（不可为 CC_OK）
 * @param fmt   格式化字符串（printf 风格）
 * @param ...   变长参数列表
 * @return      包含格式化错误信息的 cc_result_t，调用方需 cc_result_free()
 */
cc_result_t cc_result_errf(cc_error_code_t code, const char *fmt, ...);

/**
 * cc_result_free — 释放 cc_result_t 中的动态资源
 *
 * 释放 message 字符串（如果是动态分配的）。不释放 result 指针本身。
 * 传入 code=CC_OK 的 result 是安全的（无操作）。
 * 对同一个 result 多次调用是安全的（内部会将 message 置 NULL）。
 *
 * @param result  要释放的结果指针
 */
void cc_result_free(cc_result_t *result);

/**
 * cc_error_string — 将错误码转换为静态描述字符串
 *
 * 返回错误码的人类可读英文名称，便于日志和调试输出。
 * 该字符串是静态常量，不需要释放。
 *
 * @param code  错误码枚举值
 * @return      对应的错误码名称（如 "CC_ERR_NOT_FOUND"）
 */
const char *cc_error_string(cc_error_code_t code);

#endif