/**
 * cc_mcp_manager.h — 桌面 CLI 的 MCP transport adapter root。
 *
 * 所属层次：POSIX/Windows app 层。
 *
 * MCP 协议、session cache、TTL 和 tool bridge 已在 core SDK 的
 * cc_mcp_runtime_manager 中实现。本 app 层 manager 只把 POSIX/Windows 可用的
 * stdio、HTTP、SSE、streamable HTTP transport factory 注入 core。
 */

#ifndef CC_MCP_MANAGER_H
#define CC_MCP_MANAGER_H

#include "cc/core/cc_result.h"
#include "cc/app/cc_runtime_features.h"
#include "cc/ports/cc_tool_registry.h"
#include "cc/util/cc_config.h"

typedef struct cc_mcp_manager cc_mcp_manager_t;

cc_result_t cc_mcp_manager_create(cc_mcp_manager_t **out_manager);

void cc_mcp_manager_destroy(cc_mcp_manager_t *manager);

cc_result_t cc_mcp_manager_load_tools(
    cc_mcp_manager_t *manager,
    const cc_config_t *config,
    cc_tool_registry_t *registry,
    cc_runtime_diagnostics_t *diagnostics
);

#endif
