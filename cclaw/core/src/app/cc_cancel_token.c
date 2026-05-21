#include "cc/app/cc_cancel_token.h"
#include "cc/ports/cc_thread.h"

#include <stdlib.h>

struct cc_cancel_source {
    cc_mutex_t mutex;
    int cancelled;
    cc_cancel_token_t *token;
};

struct cc_cancel_token {
    cc_cancel_source_t *source;
};

cc_result_t cc_cancel_source_create(cc_cancel_source_t **out_source)
{
    if (!out_source) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null cancel source output");
    }
    cc_cancel_source_t *source = calloc(1, sizeof(cc_cancel_source_t));
    if (!source) {
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate cancel source");
    }
    source->token = calloc(1, sizeof(cc_cancel_token_t));
    if (!source->token) {
        free(source);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate cancel token");
    }
    cc_result_t rc = cc_mutex_create(&source->mutex);
    if (rc.code != CC_OK) {
        free(source->token);
        free(source);
        return rc;
    }
    source->token->source = source;
    *out_source = source;
    return cc_result_ok();
}

void cc_cancel_source_destroy(cc_cancel_source_t *source)
{
    if (!source) return;
    cc_mutex_destroy(source->mutex);
    free(source->token);
    free(source);
}

void cc_cancel_source_cancel(cc_cancel_source_t *source)
{
    if (!source) return;
    cc_mutex_lock(source->mutex);
    source->cancelled = 1;
    cc_mutex_unlock(source->mutex);
}

cc_cancel_token_t *cc_cancel_source_token(cc_cancel_source_t *source)
{
    return source ? source->token : NULL;
}

int cc_cancel_token_is_cancelled(cc_cancel_token_t *token)
{
    if (!token || !token->source) return 0;
    cc_cancel_source_t *source = token->source;
    cc_mutex_lock(source->mutex);
    int cancelled = source->cancelled;
    cc_mutex_unlock(source->mutex);
    return cancelled;
}
