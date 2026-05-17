/**
 * 学习导读：cclaw/tests/adapters/test_tool_executor_policy_approval.c
 *
 * 所属层次：测试层。
 * 阅读重点：这里用小型 Given/When/Then 场景固定行为，阅读时重点看每个断言防止哪类回归。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

#include "cc/app/cc_tool_executor.h"
#include "cc/app/cc_agent_runtime.h"
#include "cc/ports/cc_policy_engine.h"
#include "cc/ports/cc_tool_registry.h"

#include <stdlib.h>
#include <string.h>

extern cc_result_t cc_policy_engine_create_default(
    int shell_requires_approval,
    cc_policy_engine_t *out_engine
);
extern cc_result_t cc_memory_session_store_create(cc_session_store_t *out_store);

/* 学习注释：fake_name 是测试场景的一部分。先看测试准备的数据，
 * 再看触发的 API，最后看断言；这就是本测试的 Given/When/Then 主线。 */
static const char *fake_name(void *self)
{
    (void)self;
    return "shell_run";
}

/* 学习注释：fake_description 是测试场景的一部分。先看测试准备的数据，
 * 再看触发的 API，最后看断言；这就是本测试的 Given/When/Then 主线。 */
static const char *fake_description(void *self)
{
    (void)self;
    return "fake shell tool";
}

/* 学习注释：fake_schema 是测试场景的一部分。先看测试准备的数据，
 * 再看触发的 API，最后看断言；这就是本测试的 Given/When/Then 主线。 */
static const char *fake_schema(void *self)
{
    (void)self;
    return "{}";
}

/* 学习注释：fake_call 是测试场景的一部分。先看测试准备的数据，
 * 再看触发的 API，最后看断言；这就是本测试的 Given/When/Then 主线。 */
static cc_result_t fake_call(
    void *self,
    const char *args_json,
    const cc_tool_context_t *ctx,
    cc_tool_result_t *out_result
)
{
    (void)args_json;
    (void)ctx;
    int *called = (int *)self;
    (*called)++;
    memset(out_result, 0, sizeof(*out_result));
    out_result->ok = 1;
    out_result->content = strdup("called");
    return cc_result_ok();
}

static cc_tool_vtable_t fake_vtable = {
    fake_name,
    fake_description,
    fake_schema,
    fake_call,
    NULL
};

/* 学习注释：approval_approve 是测试场景的一部分。先看测试准备的数据，
 * 再看触发的 API，最后看断言；这就是本测试的 Given/When/Then 主线。 */
static int approval_approve(
    const char *tool_name,
    const char *arguments_json,
    const char *reason,
    void *user_data
)
{
    int *seen = (int *)user_data;
    (void)tool_name;
    (void)arguments_json;
    (void)reason;
    if (seen) (*seen)++;
    return 1;
}

/* 学习注释：approval_deny 是测试场景的一部分。先看测试准备的数据，
 * 再看触发的 API，最后看断言；这就是本测试的 Given/When/Then 主线。 */
static int approval_deny(
    const char *tool_name,
    const char *arguments_json,
    const char *reason,
    void *user_data
)
{
    int *seen = (int *)user_data;
    (void)tool_name;
    (void)arguments_json;
    (void)reason;
    if (seen) (*seen)++;
    return 0;
}

/* 学习注释：clear_tool_result 是测试场景的一部分。先看测试准备的数据，
 * 再看触发的 API，最后看断言；这就是本测试的 Given/When/Then 主线。 */
static void clear_tool_result(cc_tool_result_t *result)
{
    free(result->content);
    free(result->error);
    free(result->metadata_json);
    memset(result, 0, sizeof(*result));
}

/* 学习注释：main 是测试场景的一部分。先看测试准备的数据，
 * 再看触发的 API，最后看断言；这就是本测试的 Given/When/Then 主线。 */
int main(void)
{
    cc_tool_registry_t *registry = NULL;
    cc_policy_engine_t policy = {0};
    cc_session_store_t store = {0};
    cc_agent_runtime_t *runtime = NULL;
    cc_tool_result_t result;
    int called = 0;
    int approvals = 0;
    int failed = 0;

    memset(&result, 0, sizeof(result));

    if (cc_tool_registry_create(&registry).code != CC_OK) return 1;

    cc_tool_t tool = { &called, &fake_vtable };
    if (cc_tool_registry_add(registry, tool).code != CC_OK) {
        cc_tool_registry_destroy(registry);
        return 1;
    }
    cc_tool_registry_freeze(registry);

    if (cc_policy_engine_create_default(1, &policy).code != CC_OK) {
        cc_tool_registry_destroy(registry);
        return 1;
    }
    if (cc_memory_session_store_create(&store).code != CC_OK) {
        if (policy.vtable && policy.vtable->destroy) policy.vtable->destroy(policy.self);
        cc_tool_registry_destroy(registry);
        return 1;
    }

    cc_agent_runtime_deps_t deps;
    memset(&deps, 0, sizeof(deps));
    deps.tool_registry = registry;
    deps.policy = policy;
    deps.store = store;
    cc_agent_runtime_options_t options;
    memset(&options, 0, sizeof(options));
    options.config.max_steps = 1;
    options.config.workspace_dir = ".";
    if (cc_agent_runtime_create(&deps, &options, &runtime).code != CC_OK) {
        store.vtable->destroy(store.self);
        if (policy.vtable && policy.vtable->destroy) policy.vtable->destroy(policy.self);
        cc_tool_registry_destroy(registry);
        return 1;
    }

    cc_tool_call_t call = {
        .id = "call_approval",
        .name = "shell_run",
        .arguments_json = "{\"command\":\"echo should-not-run\"}"
    };

    cc_result_t rc = cc_tool_executor_execute(runtime, "ses_approval", &call, &result);
    if (rc.code != CC_OK) failed = 1;
    if (result.ok != 0) failed = 1;
    if (!result.error || !strstr(result.error, "approval")) failed = 1;
    if (called != 0) failed = 1;
    cc_result_free(&rc);
    clear_tool_result(&result);

    cc_agent_runtime_set_tool_approval(runtime, approval_deny, &approvals);
    rc = cc_tool_executor_execute(runtime, "ses_approval", &call, &result);
    if (rc.code != CC_OK) failed = 1;
    if (result.ok != 0) failed = 1;
    if (!result.error || !strstr(result.error, "denied")) failed = 1;
    if (called != 0) failed = 1;
    if (approvals != 1) failed = 1;
    cc_result_free(&rc);
    clear_tool_result(&result);

    cc_agent_runtime_set_tool_approval(runtime, approval_approve, &approvals);
    rc = cc_tool_executor_execute(runtime, "ses_approval", &call, &result);
    if (rc.code != CC_OK) failed = 1;
    if (result.ok != 1) failed = 1;
    if (!result.content || strcmp(result.content, "called") != 0) failed = 1;
    if (called != 1) failed = 1;
    if (approvals != 2) failed = 1;
    cc_result_free(&rc);

    cc_agent_runtime_destroy(runtime);
    clear_tool_result(&result);
    store.vtable->destroy(store.self);
    if (policy.vtable && policy.vtable->destroy) policy.vtable->destroy(policy.self);
    cc_tool_registry_destroy(registry);
    return failed ? 1 : 0;
}
