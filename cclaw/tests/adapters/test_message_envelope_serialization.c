/**
 * 学习导读：cclaw/tests/adapters/test_message_envelope_serialization.c
 *
 * 所属层次：测试层。
 * 阅读重点：这里用小型 Given/When/Then 场景固定行为，阅读时重点看每个断言防止哪类回归。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

#include "cc/app/cc_agent_runtime.h"
#include "cc/app/cc_context_builder.h"
#include "cc/ports/cc_session_store.h"
#include "cc/ports/cc_tool_registry.h"
#include "cc/core/cc_message.h"

#include <stdlib.h>
#include <string.h>

/**
 * cc_memory_session_store_create — 完成对应初始化步骤，失败时返回 cc_result_t 错误。
 *
 * @param out_store 输出参数；调用方传入有效指针，成功后接收结果。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
extern cc_result_t cc_memory_session_store_create(cc_session_store_t *out_store);

/**
 * fake_chat — 测试桩函数，用最小行为替代真实依赖，帮助测试聚焦当前场景。
 *
 * Given：测试先构造最小依赖、mock 或输入数据。
 * When：调用被验证的公开 API 或并发入口。
 * Then：通过断言确认行为、错误路径或资源释放没有回归。
 */
static cc_result_t fake_chat(void *self, const cc_llm_chat_request_t *request, cc_llm_response_t *out)
{
    (void)self;
    (void)request;
    memset(out, 0, sizeof(*out));
    out->has_text = 1;
    out->finished = 1;
    out->text = strdup("ok");
    return cc_result_ok();
}

static cc_llm_provider_vtable_t fake_llm_vtable = {
    fake_chat,
    NULL,
    NULL
};

/**
 * main — 执行本文件的 Given/When/Then 回归测试，失败时以非零退出码暴露问题。
 *
 * @return 0 通常表示成功完成，非 0 表示失败或应向进程层传播的状态。
 */
int main(void)
{
    int failed = 0;
    cc_tool_registry_t *registry = NULL;
    cc_session_store_t store = {0};
    cc_agent_runtime_t *runtime = NULL;

    if (cc_tool_registry_create(&registry).code != CC_OK) return 1;
    cc_tool_registry_freeze(registry);
    if (cc_memory_session_store_create(&store).code != CC_OK) return 1;
    store.vtable->create_session(store.self, "ses_msg", ".");

    cc_llm_provider_t llm = { NULL, &fake_llm_vtable };
    cc_agent_runtime_deps_t deps = {0};
    deps.llm = llm;
    deps.tool_registry = registry;
    deps.store = store;

    cc_agent_runtime_options_t options = {0};
    options.config.max_steps = 1;
    options.config.system_prompt = "system";
    options.config.workspace_dir = ".";
    options.config.model = "fake";
    options.thinking_mode = 1;
    if (cc_agent_runtime_create(&deps, &options, &runtime).code != CC_OK) return 1;

    cc_message_t *msg = NULL;
    cc_message_create("m1", "ses_msg", CC_ROLE_ASSISTANT, NULL, "call_1", &msg);
    cc_message_set_tool_calls_json(msg,
        "[{\"id\":\"call_1\",\"type\":\"function\",\"function\":{\"name\":\"file_read\",\"arguments\":\"{}\"}}]");
    cc_message_set_reasoning_content(msg, "thinking");
    store.vtable->append_message(store.self, msg);
    cc_message_destroy(msg);

    cc_message_create("m2", "ses_msg", CC_ROLE_TOOL, "{\"ok\":true}", "call_1", &msg);
    store.vtable->append_message(store.self, msg);
    cc_message_destroy(msg);

    char *messages_json = NULL;
    cc_result_t rc = cc_context_builder_build_messages(
        runtime, "ses_msg", "system", &messages_json);
    if (rc.code != CC_OK || !messages_json) failed = 1;
    if (messages_json && !strstr(messages_json, "\"tool_calls\"")) failed = 1;
    if (messages_json && !strstr(messages_json, "\"reasoning_content\"")) failed = 1;
    if (messages_json && strstr(messages_json, "\\\"tool_calls\\\"")) failed = 1;

    free(messages_json);
    cc_result_free(&rc);
    cc_agent_runtime_destroy(runtime);
    store.vtable->destroy(store.self);
    cc_tool_registry_destroy(registry);
    return failed ? 1 : 0;
}
