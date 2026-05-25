#ifndef CC_BOARD_TOOLS_INTERNAL_H
#define CC_BOARD_TOOLS_INTERNAL_H

#include "cc/core/cc_result.h"
#include "cc/ports/cc_tool.h"
#include "cc/util/cc_json.h"

#include <stddef.h>

#ifndef CC_BOARD_DEFAULT_CAMERA_DEVICE
#define CC_BOARD_DEFAULT_CAMERA_DEVICE "/dev/video0"
#endif

#ifndef CC_BOARD_DEFAULT_FB_DEVICE
#define CC_BOARD_DEFAULT_FB_DEVICE "/dev/fb0"
#endif

#ifndef CC_BOARD_DEFAULT_AUDIO_DEVICE
#define CC_BOARD_DEFAULT_AUDIO_DEVICE "hw:0,0"
#endif

#ifndef CC_BOARD_DEFAULT_ADC_RAW
#define CC_BOARD_DEFAULT_ADC_RAW "/sys/bus/iio/devices/iio:device0/in_voltage0_raw"
#endif

#ifndef CC_BOARD_DEFAULT_ADC_SCALE
#define CC_BOARD_DEFAULT_ADC_SCALE "/sys/bus/iio/devices/iio:device0/in_voltage_scale"
#endif

typedef cc_result_t (*cc_board_tool_call_fn)(
    void *self,
    const char *args_json,
    const cc_tool_context_t *ctx,
    cc_tool_result_t *out_result
);

typedef struct cc_board_tool_ops {
    const char *name;
    const char *description;
    const char *schema;
    cc_board_tool_call_fn call;
    void (*shutdown)(void);
} cc_board_tool_ops_t;

extern const cc_board_tool_ops_t cc_board_camera_tool_ops;
extern const cc_board_tool_ops_t cc_board_audio_tool_ops;
extern const cc_board_tool_ops_t cc_board_can_tool_ops;
extern const cc_board_tool_ops_t cc_board_adc_tool_ops;

char *cc_board_strdup(const char *text);
const char *cc_board_json_string_or(const cc_json_value_t *obj, const char *key, const char *fallback);
int cc_board_json_int_or(const cc_json_value_t *obj, const char *key, int fallback);
int cc_board_json_bool_or(const cc_json_value_t *obj, const char *key, int fallback);

char *cc_board_now_id(const char *prefix);
char *cc_board_now_iso8601(void);
char *cc_board_output_path(
    const cc_tool_context_t *ctx,
    const cc_json_value_t *args,
    const char *key,
    const char *prefix,
    const char *ext
);

void cc_board_set_error(cc_tool_result_t *out_result, const char *message);
void cc_board_set_success_json(cc_tool_result_t *out_result, char *content_json);

int cc_board_read_file_all(const char *path, unsigned char **out_data, size_t *out_size);
char *cc_board_file_base64_if_requested(const char *path, int embed_base64, size_t *out_size);
char *cc_board_artifact_json(
    const char *id,
    const char *kind,
    const char *mime,
    const char *path,
    const char *data_base64,
    size_t bytes,
    int width,
    int height,
    int duration_ms
);

#endif
