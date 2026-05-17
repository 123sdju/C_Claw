/**
 * 学习导读：apps/esp32/esp32_s3_qemu/main/tools/cc_esp32_gpio_tool.c
 *
 * 所属层次：ESP32 应用层。
 * 阅读重点：这里展示设备 profile 的能力裁剪和 QEMU 示例，阅读时重点看哪些桌面能力被禁用或替换。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

#include "cc/ports/cc_tool.h"
#include "cc/util/cc_json.h"
#include "driver/gpio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct cc_esp32_gpio_tool {
    int configured[GPIO_NUM_MAX];
} cc_esp32_gpio_tool_t;

/* 学习注释：gpio_allowed 是本文件内部辅助函数。
 * 阅读时先看它服务哪个 public API，再看它如何处理边界条件和资源释放。 */
static int gpio_allowed(int pin)
{
    static const int allowed[] = {
        2, 4, 5, 6, 7, 8, 9, 10,
        11, 12, 13, 14, 15, 16, 17, 18,
        21
    };
    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
        if (allowed[i] == pin) return 1;
    }
    return 0;
}

/* 学习注释：gpio_tool_name 是本文件内部辅助函数。
 * 阅读时先看它服务哪个 public API，再看它如何处理边界条件和资源释放。 */
static const char *gpio_tool_name(void *self)
{
    (void)self;
    return "gpio";
}

/* 学习注释：gpio_tool_description 是本文件内部辅助函数。
 * 阅读时先看它服务哪个 public API，再看它如何处理边界条件和资源释放。 */
static const char *gpio_tool_description(void *self)
{
    (void)self;
    return "Read, write, or toggle an allowed ESP32 GPIO pin";
}

/* 学习注释：gpio_tool_schema_json 是本文件内部辅助函数。
 * 阅读时先看它服务哪个 public API，再看它如何处理边界条件和资源释放。 */
static const char *gpio_tool_schema_json(void *self)
{
    (void)self;
    return "{"
        "\"type\":\"object\","
        "\"properties\":{"
            "\"operation\":{\"type\":\"string\",\"enum\":[\"read\",\"write\",\"toggle\"],"
                "\"description\":\"GPIO operation to perform\"},"
            "\"pin\":{\"type\":\"integer\",\"description\":\"Allowed GPIO number\"},"
            "\"level\":{\"type\":\"integer\",\"enum\":[0,1],"
                "\"description\":\"Output level for write operations\"}"
        "},"
        "\"required\":[\"operation\",\"pin\"]"
    "}";
}

/* 学习注释：set_tool_error 是本文件内部辅助函数。
 * 阅读时先看它服务哪个 public API，再看它如何处理边界条件和资源释放。 */
static void set_tool_error(cc_tool_result_t *out_result, const char *message)
{
    out_result->ok = 0;
    out_result->error = strdup(message ? message : "GPIO tool error");
}

/* 学习注释：ensure_direction 是本文件内部辅助函数。
 * 阅读时先看它服务哪个 public API，再看它如何处理边界条件和资源释放。 */
static cc_result_t ensure_direction(cc_esp32_gpio_tool_t *tool, int pin, gpio_mode_t mode)
{
    esp_err_t err = gpio_reset_pin((gpio_num_t)pin);
    if (err != ESP_OK) {
        return cc_result_error(CC_ERR_IO, esp_err_to_name(err));
    }
    err = gpio_set_direction((gpio_num_t)pin, mode);
    if (err != ESP_OK) {
        return cc_result_error(CC_ERR_IO, esp_err_to_name(err));
    }
    tool->configured[pin] = 1;
    return cc_result_ok();
}

/* 学习注释：gpio_tool_call 是本文件内部辅助函数。
 * 阅读时先看它服务哪个 public API，再看它如何处理边界条件和资源释放。 */
static cc_result_t gpio_tool_call(
    void *self,
    const char *args_json,
    const cc_tool_context_t *ctx,
    cc_tool_result_t *out_result
)
{
    (void)ctx;
    cc_esp32_gpio_tool_t *tool = (cc_esp32_gpio_tool_t *)self;
    memset(out_result, 0, sizeof(*out_result));

    cc_json_value_t *args = NULL;
    cc_result_t rc = cc_json_parse(args_json, &args);
    if (rc.code != CC_OK) {
        set_tool_error(out_result, "Failed to parse arguments JSON");
        return cc_result_ok();
    }

    const char *operation = cc_json_string_value(cc_json_object_get(args, "operation"));
    cc_json_value_t *pin_value = cc_json_object_get(args, "pin");
    cc_json_value_t *level_value = cc_json_object_get(args, "level");
    int pin = cc_json_int_value(pin_value);
    int level = cc_json_int_value(level_value);

    if (!operation || !pin_value) {
        set_tool_error(out_result, "Missing required parameters: operation and pin");
        cc_json_destroy(args);
        return cc_result_ok();
    }
    if (!gpio_allowed(pin)) {
        set_tool_error(out_result, "GPIO pin is not in this board tool's allowlist");
        cc_json_destroy(args);
        return cc_result_ok();
    }

    if (strcmp(operation, "read") == 0) {
        if (!tool->configured[pin]) {
            rc = ensure_direction(tool, pin, GPIO_MODE_INPUT);
            if (rc.code != CC_OK) goto io_error;
        }
        int current = gpio_get_level((gpio_num_t)pin);
        char buffer[96];
        snprintf(buffer, sizeof(buffer), "{\"pin\":%d,\"level\":%d}", pin, current ? 1 : 0);
        out_result->ok = 1;
        out_result->content = strdup(buffer);
    } else if (strcmp(operation, "write") == 0) {
        if (!level_value || (level != 0 && level != 1)) {
            set_tool_error(out_result, "write operation requires level 0 or 1");
            cc_json_destroy(args);
            return cc_result_ok();
        }
        rc = ensure_direction(tool, pin, GPIO_MODE_OUTPUT);
        if (rc.code != CC_OK) goto io_error;
        esp_err_t err = gpio_set_level((gpio_num_t)pin, level);
        if (err != ESP_OK) {
            rc = cc_result_error(CC_ERR_IO, esp_err_to_name(err));
            goto io_error;
        }
        char buffer[96];
        snprintf(buffer, sizeof(buffer), "{\"pin\":%d,\"level\":%d}", pin, level);
        out_result->ok = 1;
        out_result->content = strdup(buffer);
    } else if (strcmp(operation, "toggle") == 0) {
        rc = ensure_direction(tool, pin, GPIO_MODE_OUTPUT);
        if (rc.code != CC_OK) goto io_error;
        int next = gpio_get_level((gpio_num_t)pin) ? 0 : 1;
        esp_err_t err = gpio_set_level((gpio_num_t)pin, next);
        if (err != ESP_OK) {
            rc = cc_result_error(CC_ERR_IO, esp_err_to_name(err));
            goto io_error;
        }
        char buffer[96];
        snprintf(buffer, sizeof(buffer), "{\"pin\":%d,\"level\":%d}", pin, next);
        out_result->ok = 1;
        out_result->content = strdup(buffer);
    } else {
        set_tool_error(out_result, "Unknown GPIO operation");
    }

    cc_json_destroy(args);
    return cc_result_ok();

io_error:
    set_tool_error(out_result, rc.message ? rc.message : "GPIO operation failed");
    cc_result_free(&rc);
    cc_json_destroy(args);
    return cc_result_ok();
}

/* 学习注释：gpio_tool_destroy 是本文件内部辅助函数。
 * 阅读时先看它服务哪个 public API，再看它如何处理边界条件和资源释放。 */
static void gpio_tool_destroy(void *self)
{
    free(self);
}

static cc_tool_vtable_t gpio_tool_vtable = {
    gpio_tool_name,
    gpio_tool_description,
    gpio_tool_schema_json,
    gpio_tool_call,
    gpio_tool_destroy
};

/* 学习注释：cc_esp32_gpio_tool_create 是对外可见或跨模块调用的入口。
 * 阅读时重点确认参数校验、所有权转移、错误码和清理路径是否成对出现。 */
cc_result_t cc_esp32_gpio_tool_create(cc_tool_t *out_tool)
{
    cc_esp32_gpio_tool_t *self = calloc(1, sizeof(*self));
    if (!self) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create ESP32 GPIO tool");
    out_tool->self = self;
    out_tool->vtable = &gpio_tool_vtable;
    return cc_result_ok();
}
