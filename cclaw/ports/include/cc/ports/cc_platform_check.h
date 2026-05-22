/**
 * 学习导读：cclaw/ports/include/cc/ports/cc_platform_check.h
 *
 * 所属层次：端口层。
 * 阅读重点：这里定义可替换接口，阅读时重点看 struct + vtable + void *self 如何表达多态和依赖注入。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * cc_platform_check.h — 编译期平台能力约束校验
 *
 * @file    cc/ports/cc_platform_check.h
 * @brief   在编译期验证平台能力宏的组合是否合法，避免运行时才发现不兼容问题。
 *
 * 本模块不包含任何运行时代码，全部由 #if / #error 预处理指令组成。
 * 它的作用是在编译阶段就拦截不合理的配置组合，例如：
 *   - 启用了需要 fork 的功能但平台不支持 fork
 *   - 启用了 LLM 调用但平台没有网络能力
 *   - 至少需要一个存储后端和一个 Gateway
 *
 * ─── 校验规则总览 ─────────────────────────────────────────────────────
 *
 *   ┌──────────────────────────┬───────────────────────────────────────┐
 *   │  功能特性                  │  依赖的前置能力                      │
 *   ├──────────────────────────┼───────────────────────────────────────┤
 *   │  CC_HAS_FORK             │  必须同时启用 CC_HAS_PIPES            │
 *   │  CC_TOOL_SHELL_RUN       │  需要 CC_HAS_PROCESS_RUN             │
 *   │  CC_TOOL_PLUGIN          │  需要 CC_HAS_PROCESS_PIPE            │
 *   │  CC_SANDBOX_LOCAL        │  需要 CC_HAS_PROCESS_RUN             │
 *   │  CC_LLM_OPENAI/OLLAMA/...│  需要 CC_HAS_NETWORK                  │
 *   │  CC_TOOL_HTTP_REQUEST    │  需要 CC_HAS_NETWORK                  │
 *   └──────────────────────────┴───────────────────────────────────────┘
 *
 * ─── 依赖 ─────────────────────────────────────────────────────────────
 *
 *   依赖 cc/ports/cc_platform.h（平台能力宏必须在之前定义）。
 *   被 cc_platform.h 在末尾自动引入。
 */

#ifndef CC_PLATFORM_CHECK_H
#define CC_PLATFORM_CHECK_H

/* ── 系统能力约束 ──────────────────────────────────────────────────── */

#if CC_HAS_FORK && !CC_HAS_PIPES
#  error "CC_HAS_FORK requires CC_HAS_PIPES"
#endif

#if CC_HAS_PROCESS_PIPE && !CC_HAS_PROCESS_RUN
#  error "CC_HAS_PROCESS_PIPE requires CC_HAS_PROCESS_RUN"
#endif

/* ── 工具能力约束 ──────────────────────────────────────────────────── */

#if CC_TOOL_SHELL_RUN && !CC_HAS_PROCESS_RUN
#  error "CC_TOOL_SHELL_RUN requires CC_HAS_PROCESS_RUN"
#endif

#if CC_TOOL_PLUGIN && !CC_HAS_PROCESS_PIPE
#  error "CC_TOOL_PLUGIN requires CC_HAS_PROCESS_PIPE"
#endif

/* ── 沙箱约束 ──────────────────────────────────────────────────────── */

#if CC_SANDBOX_LOCAL && !CC_HAS_PROCESS_RUN
#  error "CC_SANDBOX_LOCAL requires CC_HAS_PROCESS_RUN"
#endif

/* ── 网络能力约束 ──────────────────────────────────────────────────── */

#if (CC_LLM_OPENAI || CC_LLM_OLLAMA || CC_LLM_ANTHROPIC) && !CC_HAS_NETWORK
#  error "LLM providers require CC_HAS_NETWORK"
#endif

#if CC_TOOL_HTTP_REQUEST && !CC_HAS_NETWORK
#  error "CC_TOOL_HTTP_REQUEST requires CC_HAS_NETWORK"
#endif

#if (CC_LLM_OPENAI || CC_LLM_OLLAMA || CC_LLM_ANTHROPIC || CC_TOOL_HTTP_REQUEST) && !CC_HAS_CURL && CC_PLATFORM != CC_PLATFORM_ESP32 && CC_PLATFORM != CC_PLATFORM_FREERTOS
#  error "HTTP-backed adapters require CC_HAS_CURL or a replacement HTTP client adapter"
#endif

/* ── 存储与网关最低要求 ────────────────────────────────────────────── */

#if !CC_STORAGE_SQLITE && !CC_STORAGE_JSON_FILE && !CC_STORAGE_MEMORY
#  error "At least one storage backend must be enabled"
#endif

#endif
