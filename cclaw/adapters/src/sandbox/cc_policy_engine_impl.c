/**
 * 学习导读：cclaw/adapters/src/sandbox/cc_policy_engine_impl.c
 *
 * 所属层次：适配器层。
 * 阅读重点：这里把端口接口落到具体后端，阅读时重点看协议转换、资源释放和失败降级。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * @file cc_policy_engine_impl.c
 * @brief 默认策略引擎实现——检查工具调用风险并决定是否需要用户审批
 *
 * 策略引擎在工具调用执行前进行评估，决定是否允许执行以及是否需要用户审批。
 * 当前默认策略覆盖两类高风险操作：
 *   - shell_run：Shell 命令执行（可通过配置控制是否需要审批）
 *   - file_delete：文件删除操作（始终需要用户审批）
 *
 * 实现 cc_policy_engine_vtable 中的 2 个虚函数：
 *   check_tool_call / destroy
 *
 * 安全注意：
 *   - 策略引擎是安全防线的重要组成部分，应根据部署环境调整风险阈值
 *   - 当前为最小化默认实现，生产环境建议扩展更多规则（如文件写入、网络访问等）
 */

#include "cc/ports/cc_policy_engine.h"
#include <stdlib.h>
#include <string.h>

/**
 * @brief 释放策略决策结果中的动态资源
 *
 * 释放 reason 字段中通过 strdup 分配的字符串内存。
 * 调用者在获取策略决策结果后必须调用此函数。
 *
 * @param decision 指向策略决策结果的指针，可为 NULL
 */
void cc_policy_decision_free(cc_policy_decision_t *decision)
{
    if (!decision) return;
    free(decision->reason);
    decision->reason = NULL;
}

/**
 * @brief 默认策略引擎的私有数据结构
 *
 * @field shell_requires_approval 是否要求对 shell_run 工具调用进行用户审批
 *                                1 = 需要审批, 0 = 自动允许
 */
typedef struct {
    int shell_requires_approval;
} cc_default_policy_engine_t;

/**
 * @brief vtable 函数：检查工具调用是否需要审批
 *
 * 根据工具名称判断是否需要用户审批，当前规则：
 *   1. 无效调用保护：如果 call 或 call->name 为 NULL，直接拒绝（allowed=0）
 *   2. shell_run：根据配置项 shell_requires_approval 决定是否需要审批
 *   3. file.delete：始终要求用户审批
 *   4. 其他工具：默认 allowed=1 且 require_approval=0（直接允许执行）
 *
 * 决策结果通过 out_decision 返回，包含三个关键字段：
 *   - allowed：是否允许执行（0=拒绝, 1=允许）
 *   - require_approval：是否需要用户交互审批（0=自动执行, 1=暂停等待审批）
 *   - reason：审批原因的文本描述（由 strdup 分配，调用者须通过 cc_policy_decision_free 释放）
 *
 * @param self         策略引擎实例指针
 * @param call         待评估的工具调用对象（主要关注 call->name）
 * @param ctx          工具调用上下文（当前实现未使用，保留用于未来扩展）
 * @param out_decision 输出参数，填充策略决策结果
 * @return cc_result_t 始终返回 OK
 */
static cc_result_t default_check_tool_call(
    void *self,
    const cc_tool_call_t *call,
    const cc_tool_context_t *ctx,
    cc_policy_decision_t *out_decision
)
{
    (void)ctx;
    cc_default_policy_engine_t *engine = (cc_default_policy_engine_t *)self;

    memset(out_decision, 0, sizeof(cc_policy_decision_t));

    if (!call || !call->name) {
        out_decision->allowed = 0;
        out_decision->reason = strdup("Invalid tool call");
        return cc_result_ok();
    }

    out_decision->allowed = 1;

    if (strcmp(call->name, "shell_run") == 0) {
        if (engine->shell_requires_approval) {
            out_decision->require_approval = 1;
            out_decision->reason = strdup("Shell execution requires user approval");
        }
    } else if (strcmp(call->name, "file_delete") == 0) {
        out_decision->require_approval = 1;
        out_decision->reason = strdup("File deletion requires user approval");
    }

    return cc_result_ok();
}

/**
 * @brief vtable 函数：销毁策略引擎实例
 *
 * 默认策略引擎无特殊资源需要释放，仅释放结构体内存。
 *
 * @param self 策略引擎实例指针
 */
static void default_destroy(void *self)
{
    free(self);
}

/**
 * @brief 默认策略引擎的虚函数表
 *
 * 绑定 check_tool_call → default_check_tool_call, destroy → default_destroy
 */
static cc_policy_engine_vtable_t default_vtable = {
    default_check_tool_call,
    default_destroy
};

/**
 * @brief 创建默认策略引擎实例（公共工厂函数）
 *
 * 执行流程：
 *   1. 分配 cc_default_policy_engine_t 结构体
 *   2. 设置 shell_requires_approval 配置项
 *   3. 填充 out_engine 的 self 和 vtable
 *
 * @param shell_requires_approval 是否要求 shell_run 工具调用需要用户审批（非零=需要）
 * @param out_engine               输出参数，填充创建好的策略引擎实例
 * @return cc_result_t             成功返回 OK，calloc 失败返回 OUT_OF_MEMORY
 */
cc_result_t cc_policy_engine_create_default(
    int shell_requires_approval,
    cc_policy_engine_t *out_engine
)
{
    cc_default_policy_engine_t *self = calloc(1, sizeof(cc_default_policy_engine_t));
    if (!self) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create policy engine");

    self->shell_requires_approval = shell_requires_approval;
    out_engine->self = self;
    out_engine->vtable = &default_vtable;
    return cc_result_ok();
}