/*
 * Public factories for SDK-owned built-in tools.
 *
 * These functions create cc_tool_t value objects. On success, the caller should
 * either register the tool with a registry that takes ownership or call the
 * tool destroy callback on failure paths.
 */
#ifndef CC_BUILTIN_TOOLS_H
#define CC_BUILTIN_TOOLS_H

#include "cc/core/cc_result.h"
#include "cc/ports/cc_filesystem.h"
#include "cc/ports/cc_tool.h"

cc_result_t cc_file_read_tool_create(cc_filesystem_t fs, cc_tool_t *out_tool);

cc_result_t cc_file_write_tool_create(cc_filesystem_t fs, cc_tool_t *out_tool);

cc_result_t cc_http_request_tool_create(cc_tool_t *out_tool);

#endif
