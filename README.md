# C-Claw SDK

C-Claw SDK is a pure C Agent runtime foundation. This branch keeps only the
portable SDK, SDK tests, profiles, platform ports, adapters, and documentation.
Product gateways and application-specific tools live in downstream projects.

The SDK uses Ports & Adapters with `struct + vtable + void *self` so an
application can inject LLM providers, tools, storage, policy, platform services,
and its own gateway without changing the core runtime.

## What Is Included

```text
cclaw/core/       Agent loop, runtime builder, sessions, tools, queue, MCP, skills
cclaw/ports/      Public vtable interfaces for platform and service boundaries
cclaw/adapters/   Built-in storage, memory, file/http tools, policy, LLM adapters
cclaw/platforms/  POSIX, Windows, ESP32, and FreeRTOS port implementations
cclaw/profiles/   SDK build profiles
cclaw/tests/      SDK unit and behavior tests
docs/             Configuration and SDK integration notes
```

This branch intentionally does not build an executable. A downstream
application links `c_claw_runtime`, provides a `cc_runtime_feature_set_t`, loads
configuration, and calls the runtime from its own gateway.

## Requirements

- CMake >= 3.16 for direct `-S/-B` builds.
- CMake >= 3.21 for the provided `CMakePresets.json`.
- A C99 compiler.
- Threads on POSIX/Windows builds.
- libcurl only when HTTP-backed features are enabled, such as OpenAI/Ollama/
  Anthropic providers, `http.request`, or HTTP MCP transport.

SQLite and cJSON are vendored. JSON and in-memory storage remain available when
SQLite is disabled.

## Build And Test

Preset path:

```bash
cmake --preset core-minimal
cmake --build --preset core-minimal
ctest --preset core-minimal
```

Equivalent explicit path:

```bash
cmake -S . -B build/sdk/core-minimal -DCC_PROFILE=core-minimal -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build/sdk/core-minimal
ctest --test-dir build/sdk/core-minimal --output-on-failure
```

The default SDK profile is `core-minimal`. It builds `c_claw_runtime` plus SDK
tests with no gateway, process plugin manager, shell tool, or product app.

## Using From An Application

For source-subdirectory reuse, add the SDK and link the runtime:

```cmake
add_subdirectory(vendor/C_Claw/cclaw)

add_executable(my_agent main.c my_features.c)
target_link_libraries(my_agent PRIVATE c_claw_runtime)
```

The application owns:

- configuration file location and API keys;
- gateway/UI/transport entrypoint;
- the `cc_runtime_feature_set_t` table passed to `cc_runtime_builder_create`;
- application-specific tools, plugin process management, MCP transports, and
  hardware integrations.

The SDK provides public factories for common building blocks:

- `cc/adapters/cc_llm_providers.h`
- `cc/adapters/cc_builtin_tools.h`
- `cc/adapters/cc_default_policy_engine.h`
- `cc/ports/cc_storage_factory.h`
- `cc/ports/cc_memory_tool_factory.h`

See [docs/sdk-usage.md](docs/sdk-usage.md) for a minimal feature-set pattern.

## Main Build Options

| Option | Default in `core-minimal` | Notes |
|--------|---------------------------|-------|
| `CC_PROFILE` | `core-minimal` | SDK profile selection. |
| `CC_TARGET_PLATFORM` | `auto` | Resolves to `posix`, `windows`, `esp32`, or `freertos`. |
| `CC_ENABLE_FILE_TOOLS` | ON | Compiles file read/write tool factories. |
| `CC_ENABLE_MEMORY` | ON | Compiles memory store and memory tool support. |
| `CC_ENABLE_RUN_QUEUE` | ON | Enables session serialization and queue actions. |
| `CC_ENABLE_TOOL_POOL` | ON | Enables lane concurrency and timeout coordination. |
| `CC_ENABLE_SKILLS` | ON | Enables SKILL.md catalog parsing. |
| `CC_ENABLE_SQLITE` | OFF | Enables SQLite session and memory backends. |
| `CC_ENABLE_OPENAI` | OFF | Enables OpenAI-compatible HTTP provider factory. |
| `CC_ENABLE_OLLAMA` | OFF | Enables Ollama HTTP provider factory. |
| `CC_ENABLE_ANTHROPIC` | OFF | Enables Anthropic HTTP provider factory. |
| `CC_ENABLE_HTTP_TOOL` | OFF | Enables `http.request` tool factory. |
| `CC_ENABLE_MCP` | OFF | Enables SDK MCP runtime/tool bridge. |

Command-line `-D` values override profile defaults.

## Documentation

- [docs/configuration.md](docs/configuration.md): SDK configuration model.
- [docs/sdk-usage.md](docs/sdk-usage.md): downstream integration guide.
- [cclaw/docs/architecture.md](cclaw/docs/architecture.md): SDK architecture.
- [cclaw/docs/concurrency.md](cclaw/docs/concurrency.md): queue, cancel, pool, reload semantics.
- [cclaw/docs/mcp.md](cclaw/docs/mcp.md): MCP runtime and transport boundary.
- [cclaw/docs/plugins.md](cclaw/docs/plugins.md): plugin protocol boundary.
- [cclaw/docs/skills.md](cclaw/docs/skills.md): SKILL.md catalog behavior.

## License

Apache-2.0
