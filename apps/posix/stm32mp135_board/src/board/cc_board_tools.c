#include "cc_board_tools_internal.h"

#include <stdlib.h>
#include <string.h>

typedef struct cc_board_tool {
    const cc_board_tool_ops_t *ops;
} cc_board_tool_t;

static const char *board_tool_name(void *self)
{
    cc_board_tool_t *tool = (cc_board_tool_t *)self;
    return (tool && tool->ops && tool->ops->name) ? tool->ops->name : "board.unknown";
}

static const char *board_tool_description(void *self)
{
    cc_board_tool_t *tool = (cc_board_tool_t *)self;
    return (tool && tool->ops && tool->ops->description) ? tool->ops->description : "Unknown board tool.";
}

static const char *board_tool_schema(void *self)
{
    cc_board_tool_t *tool = (cc_board_tool_t *)self;
    return (tool && tool->ops && tool->ops->schema) ? tool->ops->schema : "{\"type\":\"object\"}";
}

static cc_result_t board_tool_call(
    void *self,
    const char *args_json,
    const cc_tool_context_t *ctx,
    cc_tool_result_t *out_result
)
{
    cc_board_tool_t *tool = (cc_board_tool_t *)self;
    if (!tool || !tool->ops || !tool->ops->call) {
        cc_board_set_error(out_result, "Unknown board tool kind");
        return cc_result_ok();
    }
    return tool->ops->call(self, args_json, ctx, out_result);
}

static void board_tool_destroy(void *self)
{
    cc_board_tool_t *tool = (cc_board_tool_t *)self;
    if (tool && tool->ops && tool->ops->shutdown) tool->ops->shutdown();
    free(tool);
}

static const cc_tool_vtable_t board_tool_vtable = {
    board_tool_name,
    board_tool_description,
    board_tool_schema,
    board_tool_call,
    board_tool_destroy
};

static cc_result_t board_tool_create(const cc_board_tool_ops_t *ops, cc_tool_t *out_tool)
{
    if (!ops || !out_tool) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid board tool descriptor");
    memset(out_tool, 0, sizeof(*out_tool));
    cc_board_tool_t *tool = calloc(1, sizeof(*tool));
    if (!tool) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate board tool");
    tool->ops = ops;
    out_tool->self = tool;
    out_tool->vtable = &board_tool_vtable;
    return cc_result_ok();
}

cc_result_t cc_board_camera_tool_create(cc_tool_t *out_tool)
{
    return board_tool_create(&cc_board_camera_tool_ops, out_tool);
}

cc_result_t cc_board_audio_tool_create(cc_tool_t *out_tool)
{
    return board_tool_create(&cc_board_audio_tool_ops, out_tool);
}

cc_result_t cc_board_can_tool_create(cc_tool_t *out_tool)
{
    return board_tool_create(&cc_board_can_tool_ops, out_tool);
}

cc_result_t cc_board_adc_tool_create(cc_tool_t *out_tool)
{
    return board_tool_create(&cc_board_adc_tool_ops, out_tool);
}
