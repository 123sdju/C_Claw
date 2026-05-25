#include "cc_board_tools_internal.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int read_text_number(const char *path, char *buf, size_t size)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, size - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';
    return 0;
}

static cc_result_t board_adc_call(
    void *self,
    const char *args_json,
    const cc_tool_context_t *ctx,
    cc_tool_result_t *out_result
)
{
    (void)self;
    (void)ctx;
    cc_json_value_t *args = NULL;
    cc_result_t rc = cc_json_parse(args_json && *args_json ? args_json : "{}", &args);
    if (rc.code != CC_OK) {
        cc_board_set_error(out_result, "Invalid ADC arguments JSON");
        return cc_result_ok();
    }
    int channel = cc_board_json_int_or(args, "channel", 0);
    const char *raw_arg = cc_json_string_value(cc_json_object_get(args, "raw_path"));
    const char *scale_arg = cc_json_string_value(cc_json_object_get(args, "scale_path"));
    char raw_path[160];
    char scale_path[160];
    if (raw_arg && *raw_arg) snprintf(raw_path, sizeof(raw_path), "%s", raw_arg);
    else if (channel == 0) snprintf(raw_path, sizeof(raw_path), "%s", CC_BOARD_DEFAULT_ADC_RAW);
    else snprintf(raw_path, sizeof(raw_path),
        "/sys/bus/iio/devices/iio:device0/in_voltage%d_raw", channel);
    if (scale_arg && *scale_arg) snprintf(scale_path, sizeof(scale_path), "%s", scale_arg);
    else if (channel == 0) snprintf(scale_path, sizeof(scale_path), "%s", CC_BOARD_DEFAULT_ADC_SCALE);
    else snprintf(scale_path, sizeof(scale_path),
        "/sys/bus/iio/devices/iio:device0/in_voltage%d_scale", channel);

    char raw_buf[64];
    char scale_buf[64];
    if (read_text_number(raw_path, raw_buf, sizeof(raw_buf)) != 0 ||
        read_text_number(scale_path, scale_buf, sizeof(scale_buf)) != 0) {
        cc_json_destroy(args);
        cc_board_set_error(out_result, "Failed to read ADC raw/scale sysfs files");
        return cc_result_ok();
    }
    int raw = atoi(raw_buf);
    double scale = atof(scale_buf);
    double voltage = raw * scale / 1000.0;
    cc_json_value_t *content = cc_json_create_object();
    cc_json_object_set(content, "ok", cc_json_create_bool(1));
    cc_json_object_set(content, "channel", cc_json_create_number(channel));
    cc_json_object_set(content, "raw", cc_json_create_number(raw));
    cc_json_object_set(content, "scale_mv", cc_json_create_number(scale));
    cc_json_object_set(content, "voltage_v", cc_json_create_number(voltage));
    cc_json_object_set(content, "raw_path", cc_json_create_string(raw_path));
    cc_json_object_set(content, "scale_path", cc_json_create_string(scale_path));
    cc_board_set_success_json(out_result, cc_json_stringify_unformatted(content));
    cc_json_destroy(content);
    cc_json_destroy(args);
    return cc_result_ok();
}

const cc_board_tool_ops_t cc_board_adc_tool_ops = {
    "board.adc",
    "Read STM32MP135 IIO ADC raw and scale sysfs files and return voltage JSON.",
    "{\"type\":\"object\",\"properties\":{\"channel\":{\"type\":\"integer\"},\"raw_path\":{\"type\":\"string\"},\"scale_path\":{\"type\":\"string\"}}}",
    board_adc_call,
    NULL
};
