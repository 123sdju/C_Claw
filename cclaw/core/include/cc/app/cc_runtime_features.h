/**
 * 学习导读：cclaw/core/include/cc/app/cc_runtime_features.h
 *
 * 所属层次：核心层。
 * 阅读重点：这里定义 Agent 运行时的数据模型、主循环和通用工具，阅读时重点看所有权、错误返回和 ReAct 数据流。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

#ifndef CC_RUNTIME_FEATURES_H
#define CC_RUNTIME_FEATURES_H

#include "cc/core/cc_result.h"
#include "cc/ports/cc_filesystem.h"
#include "cc/ports/cc_llm_provider.h"
#include "cc/ports/cc_memory_store.h"
#include "cc/ports/cc_policy_engine.h"
#include "cc/ports/cc_sandbox.h"
#include "cc/ports/cc_session_store.h"
#include "cc/ports/cc_tool.h"
#include "cc/ports/cc_tool_registry.h"
#include "cc/util/cc_config.h"

typedef cc_result_t (*cc_runtime_llm_create_fn)(
    const cc_config_t *config,
    cc_llm_provider_t *out_provider
);

typedef cc_result_t (*cc_runtime_session_store_create_fn)(
    const cc_config_t *config,
    cc_session_store_t *out_store
);

typedef cc_result_t (*cc_runtime_memory_store_create_fn)(
    const cc_config_t *config,
    cc_memory_store_t *out_store
);

typedef cc_result_t (*cc_runtime_policy_create_fn)(
    const cc_config_t *config,
    cc_policy_engine_t *out_policy
);

typedef cc_result_t (*cc_runtime_sandbox_create_fn)(
    const cc_config_t *config,
    cc_sandbox_t *out_sandbox
);

typedef struct cc_runtime_tool_factory_ctx {
    const cc_config_t *config;
    cc_filesystem_t filesystem;
    cc_memory_store_t *memory_store;
    cc_runtime_sandbox_create_fn create_sandbox;
} cc_runtime_tool_factory_ctx_t;

typedef cc_result_t (*cc_runtime_tool_create_fn)(
    const cc_runtime_tool_factory_ctx_t *ctx,
    cc_tool_t *out_tool
);

typedef cc_result_t (*cc_runtime_plugin_load_fn)(
    const cc_config_t *config,
    cc_tool_registry_t *registry,
    void **out_state
);

typedef void (*cc_runtime_plugin_destroy_fn)(void *state);

typedef struct cc_llm_provider_descriptor {
    const char *name;
    int compiled;
    cc_runtime_llm_create_fn create;
} cc_llm_provider_descriptor_t;

typedef struct cc_tool_descriptor {
    const char *name;
    const char *alias;
    int compiled;
    cc_runtime_tool_create_fn create;
} cc_tool_descriptor_t;

typedef struct cc_runtime_feature_set {
    const cc_llm_provider_descriptor_t *llm_providers;
    size_t llm_provider_count;

    const cc_tool_descriptor_t *tools;
    size_t tool_count;

    cc_runtime_session_store_create_fn create_session_store;
    cc_runtime_memory_store_create_fn create_memory_store;
    cc_runtime_policy_create_fn create_policy_engine;
    cc_runtime_sandbox_create_fn create_sandbox;

    cc_runtime_plugin_load_fn load_plugins;
    cc_runtime_plugin_destroy_fn destroy_plugins;
} cc_runtime_feature_set_t;

#endif
