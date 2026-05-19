/**
 * 学习导读：cclaw/ports/include/cc/ports/cc_policy_engine.h
 *
 * 所属层次：端口层。
 * 阅读重点：这里定义可替换接口，阅读时重点看 struct + vtable + void *self 如何表达多态和依赖注入。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * cc_policy_engine.h — 工具执行策略引擎端口（Port）
 *
 * @file    cc/ports/cc_policy_engine.h
 * @brief   定义工具调用前的安全检查抽象接口。采用 vtable 多态模式。
 *
 * 策略引擎（cc_policy_engine_t）在整个 c-claw 项目中充当"安全闸门"：
 * 每次 Agent 的工具调用在执行之前，都必须经过策略引擎的审查。
 * 策略引擎根据风险评估结果决定该工具调用是否被允许，或者是否需要
 * 用户明确批准。
 *
 * ─── 架构定位 ─────────────────────────────────────────────────────────
 *
 * 在端口-适配器（Ports & Adapters）架构中：
 *   - 本模块是"端口"（Port）：定义了安全策略检查的抽象合约
 *   - 具体策略实现（规则引擎、ML 模型、配置驱动的检查器等）是"适配器"
 *   - cc_tool_executor 在执行前调用策略引擎，实现"先检查后执行"
 *
 * 数据流：
 *   Agent 决策 → cc_tool_call_t → cc_policy_engine.check_tool_call()
 *                                → cc_policy_decision_t { allowed, require_approval }
 *                                → 如果 allowed=0 → 拒绝，返回错误给 LLM
 *                                → 如果 require_approval=1 → 等待用户确认
 *                                → 如果 allowed=1 && require_approval=0 → 直接执行
 *
 * ─── 风险等级模型 ─────────────────────────────────────────────────────
 *
 *   - CC_RISK_SAFE     : 安全操作（如读取文件、列出目录）
 *                        默认允许，不需要用户确认
 *   - CC_RISK_MEDIUM   : 中等风险（如写入文件、网络请求）
 *                        允许但需要用户确认（或配置允许自动批准）
 *   - CC_RISK_DANGEROUS : 高风险（如删除文件、执行系统命令）
 *                        默认拒绝，或需要用户强制确认
 *
 *  注意：风险等级是策略引擎的评估结果，决策逻辑（allowed/require_approval）
 *  由具体适配器实现。不同适配器可能对同一风险等级做出不同决策。
 *
 * ─── 接口契约 ─────────────────────────────────────────────────────────
 *
 *   每个 cc_policy_engine_t 由两个部分组成：
 *     - self    : 指向具体策略实现的私有数据（规则列表、允许列表等）
 *     - vtable  : 指向虚函数表，定义了 check_tool_call() 和 destroy()
 *
 *   实现者必须填充 vtable 中的所有函数指针。
 *   调用者只依赖 vtable，不感知具体策略实现方式。
 *
 * ─── 使用模式 ─────────────────────────────────────────────────────────
 *
 *   cc_policy_decision_t decision;
 *   cc_result_t rc = engine->vtable->check_tool_call(
 *       engine->self, tool_call, ctx, &decision);
 *   if (rc.code != CC_OK) {
 *       // 策略检查本身失败（内部错误）
 *       return rc;
 *   }
 *   if (!decision.allowed) {
 *       // 工具调用被拒绝
 *       fprintf(stderr, "拒绝: %s\n", decision.reason);
 *       cc_policy_decision_free(&decision);
 *       return cc_result_error(CC_ERR_PERMISSION_DENIED, decision.reason);
 *   }
 *   if (decision.require_approval) {
 *       // 需要用户确认（例如：展示确认对话框）
 *   }
 *   cc_policy_decision_free(&decision);
 *   // 继续执行工具
 *
 * ─── 依赖 ─────────────────────────────────────────────────────────────
 *
 *   依赖 cc/core/cc_result.h（统一错误传递）
 *   依赖 cc/core/cc_tool_call.h（工具调用数据模型）
 *   依赖 cc/ports/cc_tool.h（工具上下文类型）
 */

#ifndef CC_POLICY_ENGINE_H
#define CC_POLICY_ENGINE_H

#include "cc/core/cc_result.h"
#include "cc/core/cc_tool_call.h"
#include "cc/ports/cc_tool.h"

/**
 * cc_risk_level_t — 工具调用风险等级枚举
 *
 * 策略引擎对每个工具调用进行风险评估后给出的风险等级。
 * 等级越高，需要的安全控制越严格。
 *
 * 具体的等级划分逻辑由策略引擎适配器实现。
 * 以下定义的三个等级是最小公倍数——适配器可以
 * 在此基础上定义更细粒度的子等级。
 */
typedef enum cc_risk_level {
    CC_RISK_SAFE,      /**< 安全级别：仅读取数据的操作。
                        *   例如：读取文件、查询数据库、获取当前时间。
                        *   此类操作不会修改系统状态或泄漏数据。 */
    CC_RISK_MEDIUM,    /**< 中等级别：可能修改数据或产生网络请求的操作。
                        *   例如：写入文件、发送 HTTP 请求、修改配置。
                        *   此类操作有副作用，但通常是可逆的。 */
    CC_RISK_DANGEROUS  /**< 危险级别：不可逆或可能产生安全影响的操作。
                        *   例如：删除文件、执行系统命令、修改权限。
                        *   此类操作应谨慎处理，通常需要用户明确确认。 */
} cc_risk_level_t;

/**
 * cc_policy_decision_t — 策略决策结果
 *
 * 封装策略引擎对单次工具调用的审查结果。
 * 包含三个关键信息：是否允许、是否需要用户确认、以及决策理由。
 *
 * reason 字段由策略引擎动态分配，调用方需要通过
 * cc_policy_decision_free() 释放。
 */
typedef struct cc_policy_decision {
    int allowed;          /**< 是否允许执行。
                           *   1 = 允许，0 = 拒绝。
                           *   当 allowed=0 时，工具调用将不会被执行，
                           *   上层应将拒绝原因返回给 LLM。 */
    int require_approval; /**< 是否需要用户批准。
                           *   1 = 需要用户确认后才能执行，
                           *   0 = 可以直接执行。
                           *   注意：此字段仅在 allowed=1 时有意义。
                           *   如果 allowed=0，此值应被忽略。 */
    char *reason;         /**< 决策理由文本。说明为什么允许/拒绝/需要确认。
                           *   例如 "文件路径在允许列表之外"、
                           *   "该命令包含危险的 sudo 操作"。
                           *   该字符串由策略引擎 malloc 分配，
                           *   调用方需通过 cc_policy_decision_free() 释放。
                           *   不为 NULL（至少是空字符串）。 */
} cc_policy_decision_t;

/* ── 前向声明 ───────────────────────────────────────────────────────── */

typedef struct cc_policy_engine_vtable cc_policy_engine_vtable_t;
/**
 * cc_policy_engine_t — 前向声明的端口/服务句柄类型，具体字段在本文件后文或对应端口中定义。
 */
typedef struct cc_policy_engine cc_policy_engine_t;

/**
 * cc_policy_engine_t — 策略引擎实例（多态句柄）
 *
 * 这是一个值语义的结构体，通过 self + vtable 实现多态。
 * 可以直接按值传递和拷贝，浅拷贝后两个实例指向同一个底层引擎。
 *
 * @note 浅拷贝后的实例共享同一个 self，不要在其中一个上调用 destroy，
 *       否则另一个实例将成为悬空指针。
 */
struct cc_policy_engine {
    void *self;                           /**< 指向具体策略实现的私有数据。
                                           *   可能包含规则列表、允许/拒绝列表、
                                           *   风险评分模型等数据。 */
    const cc_policy_engine_vtable_t *vtable; /**< 虚函数表指针，定义策略检查接口 */
};

/**
 * cc_policy_engine_vtable_t — 策略引擎虚函数表
 *
 * 定义工具调用前的安全策略检查接口。每个具体的策略实现
 * （基于规则的、基于 ML 的、混合模式等）通过实现此表来提供
 * 安全检查服务。
 */
struct cc_policy_engine_vtable {
    /**
     * check_tool_call — 检查工具调用是否合规
     *
     * 这是策略引擎的核心函数。接收一个待执行的工具调用请求和
     * 执行上下文，评估其风险并产出策略决策。
     *
     * 实现要点：
     *   - 评估应基于以下信息：
     *     * 工具名称和参数（call->name, call->arguments_json）
     *     * 执行上下文（ctx->workspace_dir, ctx->session_id）
     *     * 引擎内部维护的安全规则和历史记录
     *   - 决策应具有确定性：相同的输入应产生相同的输出
     *   - 如果策略引擎内部出错（如规则文件损坏），返回非 CC_OK 的错误码
     *   - 调用方应先检查 cc_result_t 是否成功，再读取 decision
     *
     * @param self          策略引擎私有数据
     * @param call          待检查的工具调用（不可为 NULL）。
     *                      包含工具名称和 JSON 参数。
     * @param ctx           执行上下文（不可为 NULL）。
     *                      包含会话 ID、工作目录等环境信息。
     * @param out_decision  输出：策略决策结果。调用方需通过
     *                      cc_policy_decision_free() 释放其中的 reason 字符串。
     * @return              CC_OK 表示策略评估成功，
     *                      非 CC_OK 表示策略引擎自身出错
     */
    cc_result_t (*check_tool_call)(
        void *self,
        const cc_tool_call_t *call,
        const cc_tool_context_t *ctx,
        cc_policy_decision_t *out_decision
    );

    /**
     * destroy — 销毁策略引擎实例
     *
     * 释放 self 指向的私有数据和引擎持有的所有资源。
     * 销毁后 engine 实例不可再使用。
     * 传入 NULL self 是安全的（无操作）。
     *
     * @param self  策略引擎私有数据（可为 NULL）
     */
    void (*destroy)(void *self);
};

/**
 * cc_policy_decision_free — 释放策略决策中的动态内存
 *
 * 释放 decision 中 reason 字符串的内存。
 * 不释放 decision 指针本身（它通常在栈上）。
 * 传入 NULL 是安全的。重复调用也是安全的。
 *
 * @param decision  要释放的策略决策指针（可为 NULL）
 */
void cc_policy_decision_free(cc_policy_decision_t *decision);

#endif