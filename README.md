# C-Claw SDK

C-Claw SDK 是一个纯 C Agent Runtime 基础库。当前 `sdk` 分支只保留 SDK
源码、构建脚本、测试和接口说明，不保留 POSIX CLI、板级应用、示例插件或构建产物。

SDK 的复用方式是源码子目录集成：下游应用通过 `add_subdirectory(cclaw)` 引入
`c_claw_runtime`，再由应用自己提供 gateway、配置路径和 `cc_runtime_feature_set_t`
能力表。

## 目录

```text
cclaw/core/       Agent 主循环、runtime builder、session、queue、MCP、skills
cclaw/ports/      SDK 对外端口接口，使用 struct + vtable + void *self 表达多态
cclaw/adapters/   SDK 内置适配器：存储、记忆、文件/HTTP 工具、LLM provider、policy
cclaw/platforms/  POSIX、Windows、ESP32、FreeRTOS 平台端口实现
cclaw/profiles/   SDK 构建 profile
cclaw/tests/      SDK 行为测试
docs/sdk-api.md   SDK 接口说明
```

## 构建

```bash
cmake --preset core-minimal
cmake --build --preset core-minimal
ctest --preset core-minimal
```

不使用 preset 时：

```bash
cmake -S . -B build/sdk/core-minimal -DCC_PROFILE=core-minimal
cmake --build build/sdk/core-minimal
ctest --test-dir build/sdk/core-minimal --output-on-failure
```

`build/` 是本地编译结果目录，不应提交到仓库。

## 下游应用接入

```cmake
add_subdirectory(vendor/C_Claw/cclaw)

add_executable(my_agent
    src/main.c
    src/my_features.c
)
target_link_libraries(my_agent PRIVATE c_claw_runtime)
```

应用层负责：

- 加载配置文件和 API key；
- 实现 CLI、Web、UART、HTTP 等 gateway；
- 提供 `cc_runtime_feature_set_t`，声明可用 LLM、工具、存储、policy、plugin/MCP loader；
- 实现应用私有工具、进程插件管理、MCP transport 和硬件能力。

SDK 负责：

- `cc_agent_runtime_t` Agent 主循环；
- `cc_runtime_builder_t` 组合根和资源释放顺序；
- `cc_agent_manager_t` 多 agent/session 调度入口；
- `cc_run_queue_t` session 串行、lane 并发和协作式取消；
- `cc_tool_executor_t` 工具调用、policy 检查和 tool result 写回；
- `cc_plugin_protocol` JSON-RPC envelope；
- `cc_mcp_runtime_manager_t` MCP client runtime 和 tool bridge；
- `cc_skill_catalog_t` SKILL.md 解析和 prompt snapshot；
- 平台端口和通用 adapter。

## 接口文档

只保留一份 SDK 接口说明：

- [docs/sdk-api.md](docs/sdk-api.md)

## License

Apache-2.0
