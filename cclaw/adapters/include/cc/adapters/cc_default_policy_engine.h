/*
 * Public factory for the SDK default tool policy engine.
 *
 * The default policy is intentionally small: it can require approval for
 * shell_run and always treats destructive file-delete style operations as
 * approval-required. Applications can replace it with their own policy engine.
 */
#ifndef CC_DEFAULT_POLICY_ENGINE_H
#define CC_DEFAULT_POLICY_ENGINE_H

#include "cc/core/cc_result.h"
#include "cc/ports/cc_policy_engine.h"

cc_result_t cc_policy_engine_create_default(
    int shell_requires_approval,
    cc_policy_engine_t *out_engine
);

#endif
