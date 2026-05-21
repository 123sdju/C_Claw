/**
 * test_cancel_token.c
 *
 * 固定 core SDK 的协作式取消语义：source 负责发起取消，token 是传给 run/tool
 * 的借用视图。取消前 token 返回 false，取消后返回 true，重复取消保持幂等。
 */

#include "cc/app/cc_cancel_token.h"

int main(void)
{
    cc_cancel_source_t *source = NULL;
    cc_result_t rc = cc_cancel_source_create(&source);
    if (rc.code != CC_OK || !source) {
        cc_result_free(&rc);
        return 1;
    }
    cc_result_free(&rc);

    cc_cancel_token_t *token = cc_cancel_source_token(source);
    if (!token) {
        cc_cancel_source_destroy(source);
        return 1;
    }
    if (cc_cancel_token_is_cancelled(token)) {
        cc_cancel_source_destroy(source);
        return 1;
    }
    cc_cancel_source_cancel(source);
    cc_cancel_source_cancel(source);
    int cancelled = cc_cancel_token_is_cancelled(token);
    cc_cancel_source_destroy(source);
    return cancelled ? 0 : 1;
}
