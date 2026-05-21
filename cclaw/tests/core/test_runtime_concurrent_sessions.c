/**
 * 学习导读：cclaw/tests/core/test_runtime_concurrent_sessions.c
 *
 * 所属层次：测试层。
 * 阅读重点：这里用小型 Given/When/Then 场景固定行为，阅读时重点看每个断言防止哪类回归。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/*
 * test_runtime_concurrent_sessions.c
 *
 * 测试目标：验证 Agent Runtime 在多线程并发调用 handle_message 时的正确性。
 *
 * 测试方法：
 * - 使用 Mock LLM（fake_chat）模拟大模型响应，避免依赖真实外部服务。
 * - 创建 4 个线程，每个线程使用独立的 session_id，
 *   并发调用 cc_agent_runtime_handle_message 发送消息。
 * - 每个线程验证返回的响应内容是否正确（期望 "ok"）。
 *
 * 边界条件与验证点：
 * - 多 session 并发：同时处理多个不相关的会话，验证 session 隔离性。
 * - Mock LLM 根据 thinking_mode 返回不同内容，验证 thinking 模式下的路由正确性。
 * - 每个线程记录独立的失败标记（failed），最后汇总检查是否有任何线程失败。
 *
 * 通过标准：所有线程的 handle_message 调用均返回 CC_OK 且响应为 "ok"。
 */

#include "cc/app/cc_agent_runtime.h"
#include "cc/ports/cc_thread.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define THREADS 4

/**
 * cc_memory_session_store_create — 完成对应初始化步骤，失败时返回 cc_result_t 错误。
 *
 * @param out_store 输出参数；调用方传入有效指针，成功后接收结果。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
extern cc_result_t cc_memory_session_store_create(cc_session_store_t *out_store);

/*
 * Mock LLM 的 chat 实现
 * 不执行真实推理，直接返回固定字符串：
 *   - thinking 模式下返回 "thinking-ok"
 *   - 普通模式下返回 "ok"
 * 模拟最小化的大模型交互，确保并发测试不受外部 IO 干扰。
 */
static cc_result_t fake_chat(void *self, const cc_llm_chat_request_t *request, cc_llm_response_t *out)
{
    (void)self;
    memset(out, 0, sizeof(*out));
    out->has_text = 1;
    out->finished = 1;
    out->text = strdup(request->thinking_mode ? "thinking-ok" : "ok");
    return cc_result_ok();
}

/**
 * fake_destroy — 释放本模块拥有的资源；传入的借用对象不在这里释放。
 *
 * Given：测试先构造最小依赖、mock 或输入数据。
 * When：调用被验证的公开 API 或并发入口。
 * Then：通过断言确认行为、错误路径或资源释放没有回归。
 */
static void fake_destroy(void *self) { (void)self; }

static cc_llm_provider_vtable_t fake_vtable = {
    fake_chat,
    NULL,   /* chat_stream: 测试不需要流式 */
    fake_destroy
};

/* 线程上下文：携带 runtime 引用、线程序号、失败标志 */
typedef struct {
    cc_agent_runtime_t *runtime;
    int index;
    int failed;
} runtime_ctx_t;

/*
 * 工作线程函数
 * 1. 用线程序号创建独立 session（格式 "ses_N"）
 * 2. 调用 cc_agent_runtime_handle_message 发送 "hello"
 * 3. 断言返回码为 CC_OK 且响应内容为 "ok"
 * 任一断言失败时设置 ctx->failed = 1
 */
static void *worker(void *arg)
{
    runtime_ctx_t *ctx = (runtime_ctx_t *)arg;
    char sid[32];
    char *response = NULL;
    snprintf(sid, sizeof(sid), "ses_%d", ctx->index);
    cc_result_t rc = cc_agent_runtime_create_session(ctx->runtime, sid, ".");
    if (rc.code != CC_OK) ctx->failed = 1;
    cc_result_free(&rc);
    rc = cc_agent_runtime_handle_message(ctx->runtime, sid, "hello", &response);
    if (rc.code != CC_OK || !response || strcmp(response, "ok") != 0) ctx->failed = 1;
    cc_result_free(&rc);
    free(response);
    return NULL;
}

/**
 * main — 执行本文件的 Given/When/Then 回归测试，失败时以非零退出码暴露问题。
 *
 * @return 0 通常表示成功完成，非 0 表示失败或应向进程层传播的状态。
 */
int main(void)
{
    cc_tool_registry_t *registry = NULL;
    cc_session_store_t store;
    cc_agent_runtime_t *runtime = NULL;

    /* 准备测试环境：创建并冻结 tool registry，创建内存 session store */
    if (cc_tool_registry_create(&registry).code != CC_OK) return 1;
    cc_tool_registry_freeze(registry);
    if (cc_memory_session_store_create(&store).code != CC_OK) return 1;

    /* 组装 Agent Runtime 所需组件：Mock LLM + 空 policy + 空 sandbox */
    cc_llm_provider_t llm = { NULL, &fake_vtable };
    cc_policy_engine_t policy = {0};
    cc_sandbox_t sandbox = {0};
    cc_agent_runtime_config_t config = {
        .max_steps = 2,
        .system_prompt = "system",
        .workspace_dir = ".",
        .model = "fake"
    };
    cc_agent_runtime_deps_t deps;
    memset(&deps, 0, sizeof(deps));
    deps.llm = llm;
    deps.tool_registry = registry;
    deps.store = store;
    deps.policy = policy;
    deps.sandbox = sandbox;
    cc_agent_runtime_options_t options;
    memset(&options, 0, sizeof(options));
    options.config = config;
    if (cc_agent_runtime_create(&deps, &options, &runtime).code != CC_OK) return 1;

    /* 启动并发测试：THREADS 个线程同时操作同一个 Runtime */
    runtime_ctx_t ctx[THREADS];
    cc_thread_t threads[THREADS];
    for (int i = 0; i < THREADS; i++) {
        ctx[i].runtime = runtime;
        ctx[i].index = i;
        ctx[i].failed = 0;
        cc_thread_create(worker, &ctx[i], &threads[i]);
    }
    for (int i = 0; i < THREADS; i++) cc_thread_join(threads[i]);

    /* 汇总结果：任一线程失败则整体测试不通过 */
    int failed = 0;
    for (int i = 0; i < THREADS; i++) failed |= ctx[i].failed;

    cc_agent_runtime_destroy(runtime);
    store.vtable->destroy(store.self);
    cc_tool_registry_destroy(registry);
    return failed ? 1 : 0;
}
