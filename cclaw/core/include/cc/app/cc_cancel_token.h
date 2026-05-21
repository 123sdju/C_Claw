/**
 * cc_cancel_token.h — core SDK 统一取消令牌。
 *
 * 所属层次：核心 SDK。
 *
 * cancel token 用来把“用户 interrupt、队列替换、timeout、shutdown”统一成
 * 一个可查询的状态。SDK 只提供协作式取消：它不会强杀线程，也不会直接关闭
 * POSIX/Windows 进程。具体工具、plugin worker、MCP transport 在安全检查点
 * 调用 cc_cancel_token_is_cancelled()，再决定如何释放自己的平台资源。
 */

#ifndef CC_CANCEL_TOKEN_H
#define CC_CANCEL_TOKEN_H

#include "cc/core/cc_result.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cc_cancel_source cc_cancel_source_t;
typedef struct cc_cancel_token cc_cancel_token_t;

/**
 * 创建一个取消源。source 拥有 token 的生命周期。
 */
cc_result_t cc_cancel_source_create(cc_cancel_source_t **out_source);

/**
 * 销毁取消源。调用方必须保证没有线程继续持有 source/token 借用指针。
 */
void cc_cancel_source_destroy(cc_cancel_source_t *source);

/**
 * 请求取消。该函数线程安全且幂等，重复调用保持 cancelled=1。
 */
void cc_cancel_source_cancel(cc_cancel_source_t *source);

/**
 * 获取 source 内部 token 的借用指针。
 */
cc_cancel_token_t *cc_cancel_source_token(cc_cancel_source_t *source);

/**
 * 查询 token 是否已经取消。NULL token 视为未取消，方便裁剪 profile 使用。
 */
int cc_cancel_token_is_cancelled(cc_cancel_token_t *token);

#ifdef __cplusplus
}
#endif

#endif
