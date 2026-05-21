/**
 * 学习导读：cclaw/core/include/cc/app/cc_app_features.h
 *
 * 所属层次：核心层。
 * 阅读重点：这里定义 app feature 扩展入口，重点看 app/board 如何向 runtime
 *           注入工具、provider、store 和平台能力。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

#ifndef CC_APP_FEATURES_H
#define CC_APP_FEATURES_H

#include "cc/app/cc_runtime_features.h"

/**
 * cc_app_default_features — 返回当前应用/profile 编译进来的能力表。
 *
 * 这个函数由具体应用层实现，例如 POSIX CLI、Windows CLI 或 ESP32 QEMU。
 * 核心 runtime_builder 只依赖这张表来发现可用 provider、tool、store、
 * sandbox 和插件加载器，从而避免核心层直接引用平台实现。
 *
 * @return 静态只读 feature set 借用指针；调用方不得修改或释放。
 */
const cc_runtime_feature_set_t *cc_app_default_features(void);

#endif
