/**
 * 学习导读：apps/windows/cli/src/tools/cc_shell_run_tool.c
 *
 * 所属层次：Windows CLI 应用层。
 * 阅读重点：这里镜像桌面 CLI 能力但使用 Windows 平台实现，阅读时重点比较与 POSIX 版本的差异。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/*
 * cc_shell_run_tool.c — shell_run 工具适配器
 *
 * 模块说明：
 *   本文件实现了 "shell_run" 工具的适配器（Adapter）。
 *   设计模式：Adapter（适配器）模式 —— 将底层 cc_sandbox_t 沙箱接口
 *   适配为 cc_tool vtable 接口，使 LLM 可通过统一工具接口执行 shell 命令。
 *
 * 实现接口：
 *   - cc_tool_vtable_t（5 个虚拟方法：name / description / schema_json / call / destroy）
 *
 * 安全约束：
 *   - 通过 sandbox 执行命令，隔离于宿主环境
 *   - 命令在工作目录（workspace_dir）中执行
 *   - 默认超时时间由注入的 sandbox 配置决定
 */

#include "cc/ports/cc_tool.h"
#include "cc/ports/cc_sandbox.h"
#include "cc/util/cc_json.h"
#include "cc/util/cc_string_builder.h"
#include <stdlib.h>
#include <string.h>

/*
 * cc_shell_run_tool_t — shell_run 工具的内部数据结构
 *
 * 字段说明：
 *   sandbox — 底层沙箱实例（cc_sandbox_t），
 *             提供 run 方法用于在隔离环境中执行命令
 */
typedef struct {
    cc_sandbox_t sandbox;
} cc_shell_run_tool_t;

/*
 * shell_run_name — 返回工具名称
 *
 * 功能：返回该工具在工具注册表中的唯一标识名称。
 * 参数：self — 工具实例指针（本函数未使用）
 * 返回值：工具名称字符串 "shell_run"
 */
static const char *shell_run_name(void *self)
{
    (void)self;
    return "shell_run";
}

/*
 * shell_run_description — 返回工具描述
 *
 * 功能：返回工具的自然语言描述，供 LLM 理解工具用途。
 * 参数：self — 工具实例指针（本函数未使用）
 * 返回值：工具描述字符串 "Execute a shell command"
 */
static const char *shell_run_description(void *self)
{
    (void)self;
    return "Execute a shell command";
}

/*
 * shell_run_schema_json — 返回工具参数的 JSON Schema
 *
 * 功能：定义工具调用时必须/可选的参数及其类型，符合 JSON Schema 规范。
 * 参数：self — 工具实例指针（本函数未使用）
 * 返回值：JSON Schema 字符串，定义了 command 参数（string 类型，必填）
 */
static const char *shell_run_schema_json(void *self)
{
    (void)self;
    return "{"
        "\"type\":\"object\","
        "\"properties\":{"
            "\"command\":{\"type\":\"string\",\"description\":\"The shell command to execute\"}"
        "},"
        "\"required\":[\"command\"]"
    "}";
}

/*
 * shell_run_call — 执行 shell 命令
 *
 * 功能：
 *   1. 解析 JSON 参数，提取 shell 命令
 *   2. 构造 cc_sandbox_command_t，设置工作目录和超时时间
 *   3. 调用沙箱 run 方法执行命令
 *   4. 收集 stdout、stderr 和退出码，拼接为格式化输出
 *
 * 关键逻辑：
 *   - timeout_ms 设为 0，交给 sandbox 使用配置文件中的默认超时
 *   - 收集 stdout、stderr 和退出码，统一格式化输出
 *   - 若超时，额外附加 TIMED_OUT 标记
 *   - 退出码非 0 时 ok = 0，但内容仍然返回
 *
 * 参数：
 *   self      — 工具实例指针
 *   args_json — JSON 格式的调用参数（必须包含 "command" 字段）
 *   ctx       — 工具上下文，包含 workspace_dir（命令工作目录）
 *   out_result— 输出结果结构体，包含 ok/content/error 字段
 *
 * 返回值：cc_result_t，始终返回 OK（业务错误通过 out_result->ok 标识）
 */
static cc_result_t shell_run_call(
    void *self,
    const char *args_json,
    const cc_tool_context_t *ctx,
    cc_tool_result_t *out_result
)
{
    cc_shell_run_tool_t *tool = (cc_shell_run_tool_t *)self;

    memset(out_result, 0, sizeof(cc_tool_result_t));

    cc_json_value_t *args = NULL;
    cc_result_t rc = cc_json_parse(args_json, &args);
    if (rc.code != CC_OK) {
        out_result->ok = 0;
        out_result->error = strdup("Failed to parse arguments JSON");
        return cc_result_ok();
    }

    cc_json_value_t *cmd_val = cc_json_object_get(args, "command");
    const char *command = cc_json_string_value(cmd_val);

    if (!command) {
        out_result->ok = 0;
        out_result->error = strdup("Missing required parameter: command");
        cc_json_destroy(args);
        return cc_result_ok();
    }

    cc_sandbox_command_t sandbox_cmd;
    memset(&sandbox_cmd, 0, sizeof(sandbox_cmd));
    sandbox_cmd.command = (char *)command;
    sandbox_cmd.working_dir = (char *)ctx->workspace_dir;
    sandbox_cmd.timeout_ms = 0;

    cc_sandbox_result_t sandbox_result;
    rc = tool->sandbox.vtable->run(tool->sandbox.self, &sandbox_cmd, &sandbox_result);
    cc_json_destroy(args);

    if (rc.code != CC_OK) {
        out_result->ok = 0;
        out_result->error = strdup(rc.message ? rc.message : "Sandbox execution failed");
        cc_result_free(&rc);
        return cc_result_ok();
    }

    out_result->ok = (sandbox_result.exit_code == 0) ? 1 : 0;

    cc_string_builder_t sb;
    cc_string_builder_init(&sb);
    if (sandbox_result.stdout_text && strlen(sandbox_result.stdout_text) > 0) {
        cc_string_builder_appendf(&sb, "STDOUT:\n%s\n", sandbox_result.stdout_text);
    }
    if (sandbox_result.stderr_text && strlen(sandbox_result.stderr_text) > 0) {
        cc_string_builder_appendf(&sb, "STDERR:\n%s\n", sandbox_result.stderr_text);
    }
    cc_string_builder_appendf(&sb, "EXIT_CODE: %d", sandbox_result.exit_code);
    if (sandbox_result.timed_out) {
        cc_string_builder_append(&sb, "\nTIMED_OUT: true");
    }
    out_result->content = cc_string_builder_take(&sb);

    cc_sandbox_result_free(&sandbox_result);
    return cc_result_ok();
}

/*
 * shell_run_destroy — 销毁 shell_run 工具实例
 *
 * 功能：先销毁持有的 sandbox 实例，再释放工具自身内存。
 * 参数：self — 工具实例指针
 * 返回值：无
 */
static void shell_run_destroy(void *self)
{
    cc_shell_run_tool_t *tool = (cc_shell_run_tool_t *)self;
    if (tool->sandbox.vtable && tool->sandbox.vtable->destroy) {
        tool->sandbox.vtable->destroy(tool->sandbox.self);
    }
    free(self);
}

/*
 * shell_run_vtable — shell_run 工具的虚拟方法表
 *
 * 说明：将 5 个静态函数绑定为 cc_tool_vtable_t 接口的实现，
 *       使用 Adapter 模式将沙箱功能适配为标准工具接口。
 */
static cc_tool_vtable_t shell_run_vtable = {
    shell_run_name,
    shell_run_description,
    shell_run_schema_json,
    shell_run_call,
    shell_run_destroy
};

/*
 * cc_shell_run_tool_create — 创建 shell_run 工具实例（工厂函数）   
 *
 * 功能：
 *   1. 分配并初始化 cc_shell_run_tool_t 结构体
 *   2. 注入底层沙箱依赖
 *   3. 填充 cc_tool_t 输出参数，返回工厂模式创建的实例
 *
 * 参数：
 *   sandbox  — 底层沙箱实例（依赖注入）
 *   out_tool — 输出参数，创建成功后包含工具 self 指针和 vtable
 *
 * 返回值：cc_result_t，成功返回 CC_OK，内存不足返回 CC_ERR_OUT_OF_MEMORY
 */
cc_result_t cc_shell_run_tool_create(cc_sandbox_t sandbox, cc_tool_t *out_tool)
{
    cc_shell_run_tool_t *self = calloc(1, sizeof(cc_shell_run_tool_t));
    if (!self) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create shell run tool");

    self->sandbox = sandbox;
    out_tool->self = self;
    out_tool->vtable = &shell_run_vtable;
    return cc_result_ok();
}
