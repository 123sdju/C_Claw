# SDK Usage Guide

This branch is a source SDK. Downstream projects add it as a subdirectory and
provide the application layer.

## CMake Integration

When the SDK repository is vendored as `vendor/C_Claw`:

```cmake
add_subdirectory(vendor/C_Claw/cclaw)

add_executable(my_agent
    src/main.c
    src/my_features.c
)
target_link_libraries(my_agent PRIVATE c_claw_runtime)
```

If the application uses the repository root, build and test the SDK itself with:

```bash
cmake --preset core-minimal
cmake --build --preset core-minimal
ctest --preset core-minimal
```

Applications usually configure feature flags before adding the SDK:

```cmake
set(CC_ENABLE_OPENAI ON CACHE BOOL "" FORCE)
set(CC_ENABLE_HTTP_TOOL ON CACHE BOOL "" FORCE)
set(CC_ENABLE_SQLITE OFF CACHE BOOL "" FORCE)
add_subdirectory(vendor/C_Claw/cclaw)
```

## Minimal Feature Set

The runtime builder is SDK-owned, but the application decides what to expose:

```c
#include "cc/app/cc_runtime_builder.h"
#include "cc/adapters/cc_builtin_tools.h"
#include "cc/adapters/cc_default_policy_engine.h"
#include "cc/adapters/cc_llm_providers.h"
#include "cc/ports/cc_memory_tool_factory.h"
#include "cc/ports/cc_storage_factory.h"

static cc_result_t create_openai(const cc_config_t *config, cc_llm_provider_t *out)
{
    return cc_openai_provider_create(config->base_url, config->api_key, config->model, out);
}

static cc_result_t create_file_read(const cc_runtime_tool_factory_ctx_t *ctx, cc_tool_t *out)
{
    return cc_file_read_tool_create(ctx->filesystem, out);
}

static cc_result_t create_policy(const cc_config_t *config, cc_policy_engine_t *out)
{
    return cc_policy_engine_create_default(config->shell_requires_approval, out);
}

static cc_result_t create_memory_store(const cc_config_t *config, cc_memory_store_t *out)
{
    return cc_memory_store_factory_create(
        out,
        config->memory_backend ? config->memory_backend : "json_file",
        config->memory_path
    );
}

static const cc_llm_provider_descriptor_t providers[] = {
    { "openai", CC_LLM_OPENAI, create_openai }
};

static const cc_tool_descriptor_t tools[] = {
    { "file_read", "read", CC_TOOL_FILE_READ, create_file_read }
};

static const cc_runtime_feature_set_t features = {
    .llm_providers = providers,
    .llm_provider_count = sizeof(providers) / sizeof(providers[0]),
    .tools = tools,
    .tool_count = sizeof(tools) / sizeof(tools[0]),
    .create_session_store = cc_storage_factory_create_store,
    .create_memory_store = create_memory_store,
    .create_policy_engine = create_policy
};
```

Then the gateway can load config and build the runtime:

```c
cc_config_t config;
cc_runtime_builder_t *builder = NULL;

memset(&config, 0, sizeof(config));
cc_result_t rc = cc_config_load("config.json", &config);
if (rc.code != CC_OK && rc.code == CC_ERR_OUT_OF_MEMORY) {
    cc_result_free(&rc);
    return 1;
}
cc_result_free(&rc);

rc = cc_runtime_builder_create(&config, &features, &builder);
if (rc.code != CC_OK) {
    cc_result_free(&rc);
    cc_config_destroy(&config);
    return 1;
}

/* The app now drives cc_runtime_builder_runtime(builder) or the agent manager. */

cc_runtime_builder_destroy(builder);
cc_config_destroy(&config);
```

## Extension Points

- Add a provider by creating a `cc_llm_provider_t` factory and listing it in
  `cc_llm_provider_descriptor_t`.
- Add a tool by creating a `cc_tool_t` factory and listing it in
  `cc_tool_descriptor_t`.
- Add storage by implementing `cc_session_store_t` or `cc_memory_store_t` and
  wiring the factory into the feature set.
- Add a platform by implementing the ports in `cclaw/ports/include/cc/ports`
  and adding a `cclaw/platforms/<name>/CMakeLists.txt`.
- Add plugin or MCP process transport in the application; keep protocol
  envelope and runtime/cache logic in the SDK.
