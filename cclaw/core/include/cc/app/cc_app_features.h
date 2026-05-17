/**
 * 学习导读：cclaw/core/include/cc/app/cc_app_features.h
 *
 * 所属层次：核心层。
 * 阅读重点：这里定义 Agent 运行时的数据模型、主循环和通用工具，阅读时重点看所有权、错误返回和 ReAct 数据流。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

#ifndef CC_APP_FEATURES_H
#define CC_APP_FEATURES_H

#include "cc/app/cc_runtime_features.h"

const cc_runtime_feature_set_t *cc_app_default_features(void);

#endif
