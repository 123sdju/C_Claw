# SDK Configuration

`cc_config_t` is the SDK runtime configuration model. Applications choose where
the JSON file lives, load it with `cc_config_load()`, and pass the resulting
config to `cc_runtime_builder_create()`.

Compile-time options still win over runtime configuration: a capability that was
not compiled into `c_claw_runtime` cannot be enabled from JSON.

## Example

```json
{
  "model": {
    "provider": "openai",
    "model": "gpt-4o-mini",
    "base_url": "https://api.openai.com",
    "api_key": "",
    "api_key_env": "OPENAI_API_KEY",
    "max_tokens": 4096,
    "temperature": 0.7,
    "thinking_mode": 0,
    "stream_mode": 0
  },
  "storage": {
    "type": "json",
    "path": "./runtime/data/sessions.json"
  },
  "agents": {
    "defaults": {
      "id": "default",
      "workspace": "./workspace",
      "agentDir": ".agents/default",
      "systemPromptFile": "./prompts/system.md",
      "skills": ["core"]
    },
    "list": []
  },
  "queue": {
    "lanes": { "main": 4, "subagent": 8, "plugin": 4, "mcp": 4 },
    "perSessionConcurrency": 1,
    "mode": "steer",
    "debounceMs": 500,
    "maxPendingPerSession": 20
  },
  "tools": {
    "enabled": ["read", "write", "memory"],
    "defaultTimeoutMs": 30000,
    "perTool": {}
  },
  "plugins": {
    "hotReload": true,
    "reloadDebounceMs": 300,
    "entries": {}
  },
  "skills": {
    "load": {
      "watch": false,
      "watchDebounceMs": 250,
      "extraDirs": [".agents/skills"]
    }
  },
  "mcp": {
    "enabled": false,
    "sessionIdleTtlMs": 600000,
    "servers": {}
  },
  "memory": {
    "backend": "json_file",
    "path": "./runtime/data/memory.json",
    "active": {
      "enabled": false,
      "writeSummary": true,
      "maxValueChars": 1600,
      "category": "active_summary"
    }
  },
  "sandbox": {
    "type": "none",
    "timeout_ms": 30000,
    "shell_requires_approval": true
  },
  "system": {
    "max_steps": 10,
    "context_window_tokens": 8192,
    "context_compress_threshold": 80,
    "context_keep_recent": 20,
    "summary_max_tokens": 1024,
    "summary_temperature": 0.3
  },
  "cli": {
    "debug_mode": false
  }
}
```

## Sections

`model` selects the configured provider name, model, API URL, key, token limit,
temperature, and stream flags. `api_key_env` lets the app keep secrets outside
the tracked JSON file. `thinking_mode=1` forces `stream_mode=1`.

`storage` configures session history. Supported `type` values are `json`,
`local_file`, `memory`, and `sqlite` when `CC_ENABLE_SQLITE=ON`. Missing paths
fall back to profile defaults under the build directory.

`agents` defines default and named agent metadata. The SDK uses the workspace,
agent directory, system prompt path, and skill allowlist when building context.

`queue` configures `cc_run_queue_t`: lane concurrency, per-session serialization,
debounce, and pending queue limits.

`tools` filters tools that the application feature set registered. An empty or
missing `enabled` list means all compiled and registered tools are visible.
Aliases are resolved by the application's `cc_tool_descriptor_t` table.

`plugins` is parsed and validated by the SDK, but process creation and pipe I/O
belong to the application. The SDK supplies JSON-RPC envelope helpers and reload
generation semantics.

`skills` configures SKILL.md catalog loading. File watching is an application
responsibility; the SDK only parses and snapshots skill content.

`mcp` configures MCP client servers. The SDK owns JSON-RPC matching, runtime
cache, TTL, tool bridge, and SSE parsing. The application provides stdio or HTTP
transport factories when it enables MCP.

`memory` configures long-term memory storage. Supported backends are `json_file`,
`sqlite` when compiled, `inmem`, and `noop`/`none`.

`sandbox` is consumed by applications that expose high-risk tools. The SDK
default policy factory can require approval for `shell_run`, but this SDK branch
does not include a shell tool implementation.

`cli` is kept as a generic gateway/debug field for downstream applications. The
SDK does not provide a command-line executable in this branch.

## Validation Behavior

`cc_config_load()` performs syntax parsing, default filling, and semantic
validation. Fatal configuration errors return `CC_ERR_INVALID_ARGUMENT`; external
capability startup failures should be reported through `cc_runtime_diagnostics_t`
by the application feature loader.

Current validation includes:

- queue lane counts must be positive and debounce/cap values non-negative;
- enabled plugin entries must have a `command`, positive worker counts, and
  positive in-flight limits;
- MCP transports must be `stdio`, `http`, `sse`, or `streamable_http`;
- stdio MCP entries require `command`; HTTP-style entries require `url`;
- timeout, TTL, watch debounce, and active memory caps cannot be negative.
