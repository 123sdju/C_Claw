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

typedef struct cc_runtime_builder cc_runtime_builder_t;

cc_result_t cc_runtime_builder_create(
    const cc_config_t *config,
    const cc_runtime_feature_set_t *features,
    cc_runtime_builder_t **out_builder
);

cc_agent_runtime_t *cc_runtime_builder_runtime(cc_runtime_builder_t *builder);

cc_logger_t *cc_runtime_builder_logger(cc_runtime_builder_t *builder);

void cc_runtime_builder_destroy(cc_runtime_builder_t *builder);

#endif
