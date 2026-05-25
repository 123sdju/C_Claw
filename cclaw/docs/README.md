# C-Claw SDK Docs

`cclaw/` is the portable SDK layer. Keep runtime, port, adapter, profile, and
kernel-style testing notes here. Product-specific gateway and deployment docs
belong in downstream application repositories.

- [architecture.md](architecture.md): SDK architecture, profiles, and extension points.
- [concurrency.md](concurrency.md): shared POSIX/Windows/ESP queue, cancel, pool, and reload semantics.
- [plugins.md](plugins.md): plugin protocol boundary, worker concurrency, and reload behavior.
- [skills.md](skills.md): SKILL.md catalog loading, allowlist, and watcher boundary.
- [mcp.md](mcp.md): MCP client runtime, SSE/streamable HTTP behavior, and transport boundary.
