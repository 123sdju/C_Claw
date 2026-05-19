/**
 * 学习导读：cclaw/tests/core/test_tool_registry_freeze.c
 *
 * 所属层次：测试层。
 * 阅读重点：这里用小型 Given/When/Then 场景固定行为，阅读时重点看每个断言防止哪类回归。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/*
 * test_tool_registry_freeze.c
 *
 * 测试目标：验证 Tool Registry 的 freeze（冻结）机制正确性
 * 以及冻结后的并发读取安全性。
 *
 * 测试方法：
 * - 创建一个 dummy 工具（模拟真实的 tool 实现），其 vtable 提供 name/desc/schema/call。
 * - 将 dummy 工具注册到 registry，然后调用 freeze 冻结注册表。
 * - 验证冻结后的行为：
 *   a) is_frozen 返回 true
 *   b) 再次调用 add 应该失败（冻结后不可修改）
 * - 启动 4 个线程，在冻结后的 registry 上并发执行大量读取操作
 *   （find 查找工具 + build_schema_json 构建 schema），
 *   每个线程执行 LOOPS（1000）次。
 *
 * 边界条件与验证点：
 * - 冻结正确性：冻结后 is_frozen 必须返回 true，
 *   且 add 操作必须返回非 CC_OK。
 * - 冻结后并发读：大量并发 find 和 build_schema_json 调用，
 *   验证读操作不触发写锁或数据竞争。
 * - 返回值检查：每次 find 和 build_schema_json 都检查返回码，
 *   确保冻结状态下的返回值稳定一致。
 *
 * 通过标准：冻结行为正确，且所有并发读取操作均返回 CC_OK。
 */

#include "cc/ports/cc_tool_registry.h"
#include "cc/ports/cc_thread.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define THREADS 4
#define LOOPS 1000

/* Dummy 工具的 vtable 实现 - 提供最小化的工具接口 */
/**
 * dummy_name — 返回 dummy 工具名称的静态字符串。
 */
static const char *dummy_name(void *self) { (void)self; return "dummy"; }
/**
 * dummy_desc — 返回 dummy 工具描述的静态字符串。
 */
static const char *dummy_desc(void *self) { (void)self; return "Dummy tool"; }
/**
 * dummy_schema — 返回 dummy 工具 schema 的静态 JSON 字符串。
 */
static const char *dummy_schema(void *self) { (void)self; return "{\"type\":\"object\",\"properties\":{}}"; }
/**
 * dummy_call — 填充 dummy 工具成功结果，用于 registry freeze 测试。
 */
static cc_result_t dummy_call(void *self, const char *args, const cc_tool_context_t *ctx, cc_tool_result_t *out)
{
    (void)self; (void)args; (void)ctx;
    memset(out, 0, sizeof(*out));
    out->ok = 1;
    out->content = strdup("{}");
    return cc_result_ok();
}

static cc_tool_vtable_t dummy_vtable = {
    dummy_name, dummy_desc, dummy_schema, dummy_call, NULL
};

/* 读者线程上下文：持有已冻结的 registry 引用和失败标志 */
typedef struct {
    cc_tool_registry_t *registry;
    int failed;
} read_ctx_t;

/*
 * 读者线程函数
 * 在已冻结的 registry 上反复执行读取操作：
 * - find: 按名称查找工具
 * - build_schema_json: 构建所有工具的 JSON Schema
 * 任一操作失败则标记 failed。
 */
static void *reader(void *arg)
{
    read_ctx_t *ctx = (read_ctx_t *)arg;
    for (int i = 0; i < LOOPS; i++) {
        cc_tool_t tool;
        char *schema = NULL;
        if (cc_tool_registry_find(ctx->registry, "dummy", &tool).code != CC_OK) ctx->failed = 1;
        if (cc_tool_registry_build_schema_json(ctx->registry, &schema).code != CC_OK || !schema) ctx->failed = 1;
        free(schema);
    }
    return NULL;
}

/**
 * main — 执行本文件的 Given/When/Then 回归测试，失败时以非零退出码暴露问题。
 *
 * 位置：工具适配层。注释重点说明当前函数的输入输出、资源边界和错误传播。
 *
 * @return 0 通常表示成功完成，非 0 表示失败或应向进程层传播的状态。
 */
int main(void)
{
    cc_tool_registry_t *registry = NULL;
    if (cc_tool_registry_create(&registry).code != CC_OK) return 1;

    /* 注册 dummy 工具 */
    cc_tool_t tool = {0};
    tool.vtable = &dummy_vtable;
    if (cc_tool_registry_add(registry, tool).code != CC_OK) return 1;

    /* 冻结 registry 并验证冻结行为 */
    if (cc_tool_registry_freeze(registry).code != CC_OK) return 1;       /* 冻结操作本身应成功 */
    if (!cc_tool_registry_is_frozen(registry)) return 1;                 /* 冻结后 is_frozen 必须返回 true */
    if (cc_tool_registry_add(registry, tool).code == CC_OK) return 1;   /* 冻结后 add 必须失败 */

    /* 启动并发读测试：多线程在冻结的 registry 上执行大量读取操作 */
    read_ctx_t ctx = { registry, 0 };
    cc_thread_t threads[THREADS];
    for (int i = 0; i < THREADS; i++) cc_thread_create(reader, &ctx, &threads[i]);
    for (int i = 0; i < THREADS; i++) cc_thread_join(threads[i]);

    cc_tool_registry_destroy(registry);
    return ctx.failed ? 1 : 0;
}
