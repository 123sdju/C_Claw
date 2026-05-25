# Pure C C-Claw SDK Design Notes

This memo records the design constraints behind the SDK branch.

## Goals

1. Keep the Agent runtime reusable across products and platforms.
2. Express interfaces in C with `struct + vtable + void *self`.
3. Keep core runtime independent from OS, device SDKs, process APIs, and UI.
4. Let CMake options compile out unavailable capabilities.
5. Make ownership explicit: creator releases, borrowed pointers stay borrowed,
   and `cc_result_t` messages are freed by the caller.

## Boundaries

`cclaw/core` owns runtime semantics: ReAct loop, context building, session
management, run queue, cancel token, tool execution, tool snapshots, skills,
plugin envelope, MCP runtime, and SSE parsing.

`cclaw/ports` defines replaceable interfaces for LLMs, tools, storage, platform
I/O, HTTP, process, thread, policy, sandbox, and event delivery.

`cclaw/adapters` contains reusable implementations such as JSON/SQLite storage,
memory store, default policy, file/http/memory tools, and HTTP-backed LLM
providers.

`cclaw/platforms` hides POSIX, Windows, ESP32, and FreeRTOS differences behind
the port interfaces.

Applications provide gateways and feature sets. They decide which providers,
tools, transports, and policies are exposed.

## Build Contract

The SDK branch default is:

```bash
cmake --preset core-minimal
cmake --build --preset core-minimal
ctest --preset core-minimal
```

The main output is `c_claw_runtime`. Downstream applications link this target
and pass an application-owned `cc_runtime_feature_set_t` to
`cc_runtime_builder_create()`.

## Extension Rules

- Put reusable protocol and lifecycle semantics in `cclaw/core`.
- Put reusable backend implementations in `cclaw/adapters`.
- Put OS and board SDK calls behind `cclaw/ports`.
- Keep product policy, gateway UX, hardware behavior, process supervision, and
  deployment-specific configuration in downstream applications.
- Add public headers for reusable factories; do not require downstream projects
  to copy `extern` declarations.
