/**
 * 学习导读：cclaw/core/include/cc/app/cc_runtime_builder.h
 *
 * 所属层次：核心层。
 * 阅读重点：这里定义 Agent 运行时的数据模型、主循环和通用工具，阅读时重点看所有权、错误返回和 ReAct 数据流。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

#ifndef CC_RUNTIME_BUILDER_H
#define CC_RUNTIME_BUILDER_H

#include "cc/app/cc_agent_runtime.h"
#include "cc/app/cc_runtime_features.h"
#include "cc/util/cc_config.h"
#include "cc/ports/cc_logger.h"

/**
 * cc_runtime_builder — runtime 组合根，持有构建过程中创建且需要统一销毁的资源。
 *
 * 该类型对外不透明，目的是把启动阶段创建的 logger、event bus、store、
 * provider、tool registry 和 agent runtime 收拢到一个销毁入口，避免 CLI
 * 层直接理解所有释放顺序。
 */
typedef struct cc_runtime_builder cc_runtime_builder_t;

/**
 * cc_runtime_builder_create — 根据配置和 feature set 组装 logger、store、tools、LLM provider 和 Agent runtime。
 *
 * 这是应用启动时的组合根：它先创建平台端口，再按 features 注册内建工具、
 * 插件和 LLM provider，最后创建 cc_agent_runtime_t。config 和 features 都只
 * 被借用；builder 只复制 runtime 需要长期持有的字符串。
 *
 * @param config 借用的只读配置；调用方仍负责在 builder 销毁后释放 config。
 * @param features 借用的静态能力表；生命周期必须覆盖 builder。
 * @param out_builder 输出参数；成功时获得新 builder，调用方负责 destroy。
 * @return CC_OK 表示 runtime 组装完成；失败返回错误码且 out_builder 为 NULL。
 */
cc_result_t cc_runtime_builder_create(
    const cc_config_t *config,
    const cc_runtime_feature_set_t *features,
    cc_runtime_builder_t **out_builder
);

/**
 * cc_runtime_builder_runtime — 返回 builder 持有的 runtime 借用指针，调用方不能释放该指针。
 *
 * @param builder 借用的 builder；可为 NULL。
 * @return builder 内部 runtime 的借用指针；builder 为 NULL 时返回 NULL。
 */
cc_agent_runtime_t *cc_runtime_builder_runtime(cc_runtime_builder_t *builder);

/**
 * cc_runtime_builder_logger — 返回 builder 持有的 logger 借用指针，用于 gateway 输出或诊断。
 *
 * @param builder 借用的 builder；可为 NULL。
 * @return builder 内部 logger 的借用指针；builder 为 NULL 时返回 NULL。
 */
cc_logger_t *cc_runtime_builder_logger(cc_runtime_builder_t *builder);

/**
 * cc_runtime_builder_destroy — 按所有权顺序销毁 builder 创建的 runtime、工具、store、provider 和日志资源。
 *
 * 传入 NULL 是安全的。销毁后所有从 builder 取出的 runtime/logger 借用指针
 * 都立即失效，调用方不能继续使用。
 *
 * @param builder 要销毁的 builder；函数取得并释放该对象的所有权。
 */
void cc_runtime_builder_destroy(cc_runtime_builder_t *builder);

#endif
