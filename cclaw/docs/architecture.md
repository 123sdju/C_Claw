# C-Claw SDK Architecture

C-Claw is a pure C Agent runtime SDK. The SDK provides reusable runtime,
protocol, adapter, storage, tool, and platform building blocks. Applications
provide gateways, product-specific tools, process supervision, and deployment
policy.

## Layers

```text
Application
  gateway, config location, product tools, plugin process manager, MCP transport

SDK core
  agent runtime, runtime builder, context builder, sessions, queue, cancel token,
  tool executor, tool pool, plugin JSON-RPC envelope, MCP runtime, SSE parser,
  skill catalog

Ports
  cc_llm_provider_t, cc_tool_t, cc_session_store_t, cc_memory_store_t,
  cc_filesystem_t, cc_http_client_t, cc_process_t, cc_thread/cc_mutex/cc_cond,
  cc_policy_engine_t, cc_sandbox_t

Adapters
  HTTP LLM provider strategies, JSON/SQLite/memory storage, memory/file/http
  tools, default policy engine

Platforms
  POSIX, Windows, ESP32, FreeRTOS implementations of the ports
```

Core code depends only on ports and SDK utility types. Concrete providers,
tools, storage, and transports are injected through `cc_runtime_feature_set_t`.

## Runtime Builder Boundary

`cc_runtime_builder_create()` is the composition root inside the SDK. It receives
an application-owned `cc_config_t` and `cc_runtime_feature_set_t`, then creates:

- logger, event bus, filesystem, and platform services;
- session and memory stores;
- policy engine and optional sandbox;
- LLM provider selected by config;
- registered tool registry and optional snapshots;
- agent runtime and optional agent manager/run queue.

The builder owns created resources and releases them in
`cc_runtime_builder_destroy()`. Config and feature tables are borrowed.

## Feature Set Injection

Applications describe their available capabilities with:

- `cc_llm_provider_descriptor_t`
- `cc_tool_descriptor_t`
- `cc_runtime_session_store_create_fn`
- `cc_runtime_memory_store_create_fn`
- `cc_runtime_policy_create_fn`
- optional plugin and MCP load/destroy callbacks

The SDK ships public factories for common adapters, but it does not decide which
ones a product exposes. That decision belongs to the application feature set.

## Build Profiles

The SDK branch ships one profile:

```bash
cmake --preset core-minimal
```

`core-minimal` builds `c_claw_runtime` and SDK tests. It enables core runtime,
file tools, memory, queue, tool pool, and skills while disabling external LLM
providers, HTTP tool, shell, process plugin manager, MCP transports, and SQLite
by default. Downstream applications can override options with `-D`.

Important options:

| Option | Effect |
|--------|--------|
| `CC_TARGET_PLATFORM` | `auto`, `posix`, `windows`, `esp32`, or `freertos`. |
| `CC_ENABLE_FILE_TOOLS` | Compiles file read/write tool factories. |
| `CC_ENABLE_HTTP_TOOL` | Compiles the `http.request` tool factory. |
| `CC_ENABLE_MEMORY` | Compiles long-term memory support. |
| `CC_ENABLE_RUN_QUEUE` | Compiles session serialization and queue actions. |
| `CC_ENABLE_TOOL_POOL` | Compiles lane concurrency and timeout coordination. |
| `CC_ENABLE_SKILLS` | Compiles SKILL.md catalog parsing. |
| `CC_ENABLE_MCP` | Compiles MCP runtime/tool bridge. |
| `CC_ENABLE_SQLITE` | Compiles SQLite session and memory backends. |
| `CC_ENABLE_OPENAI` | Compiles OpenAI-compatible provider factory. |
| `CC_ENABLE_OLLAMA` | Compiles Ollama provider factory. |
| `CC_ENABLE_ANTHROPIC` | Compiles Anthropic provider factory. |

## Data Flow

```text
user input
  -> app gateway
  -> cc_agent_runtime_run
  -> cc_context_builder builds LLM messages from session store
  -> selected cc_llm_provider_t returns assistant text/tool calls
  -> cc_tool_executor validates policy and calls cc_tool_t
  -> tool result is written back to session store
  -> loop continues until final assistant text or max steps
```

The message model keeps visible content, tool calls, tool call ids, reasoning
content, artifacts, and multimodal content parts as structured fields. Provider
adapters serialize those fields to the target API shape.

## Storage

Session storage uses `cc_session_store_t`:

- `memory`: test and temporary sessions;
- `json`/`local_file`: JSON file persistence;
- `sqlite`: SQLite persistence when `CC_ENABLE_SQLITE=ON`.

Memory storage uses `cc_memory_store_t`:

- `inmem`;
- `json_file`;
- `sqlite` when compiled;
- `noop`/`none`.

Factories live in `cc/ports/cc_storage_factory.h` and
`cc/ports/cc_memory_tool_factory.h`.

## Plugins And MCP

The SDK keeps reusable protocol semantics:

- plugin JSON-RPC request/response envelope;
- tool registry generation and reload safety;
- MCP JSON-RPC id matching, runtime cache, TTL, tool bridge;
- SSE parser.

Process management, pipe I/O, external command configuration, and HTTP transport
construction are application responsibilities.

## Platform Porting

To add a platform:

1. Implement the required ports under `cclaw/platforms/<platform>/src`.
2. Add `cclaw/platforms/<platform>/CMakeLists.txt`.
3. Map the platform in `cclaw/CMakeLists.txt` if it needs a new
   `CC_TARGET_PLATFORM` value.
4. Keep platform code behind ports; do not include device SDK headers from core
   or adapter code.
5. Validate with an SDK profile and a downstream application feature set.
