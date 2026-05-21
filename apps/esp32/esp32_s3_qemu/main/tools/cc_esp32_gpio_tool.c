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

/**
 * cc_esp32_gpio_tool — ESP32 GPIO 工具私有状态，记录已配置方向的引脚，避免重复配置。
 *
 * 资源约定：结构体内的堆字符串或数组由本模块 cleanup/free 函数释放；外部借用指针不在这里释放。
 */
typedef struct cc_esp32_gpio_tool {
    int configured[GPIO_NUM_MAX];
} cc_esp32_gpio_tool_t;

/**
 * gpio_allowed — 板级 GPIO 白名单。
 *
 * QEMU 示例故意只暴露一小组安全引脚，避免 agent 误操作 UART、flash 或启动相关
 * 引脚。真实板级应用应根据原理图重新定义 allowlist，而不是把平台层做成通用
 * “任意 GPIO 执行器”。
 */
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

/**
 * gpio_tool_name — 返回端口、工具或协议的静态名称字符串，用于注册和日志。
 *
 * @param self vtable 私有上下文；生命周期由创建该端口的实现管理。
 * @return 返回借用或静态只读字符串；调用方不得释放。
 */
static const char *gpio_tool_name(void *self)
{
    (void)self;
    return "gpio";
}

/**
 * gpio_tool_description — 返回工具或组件的静态说明字符串，供模型理解工具用途。
 *
 * @param self vtable 私有上下文；生命周期由创建该端口的实现管理。
 * @return 返回借用或静态只读字符串；调用方不得释放。
 */
static const char *gpio_tool_description(void *self)
{
    (void)self;
    return "Read, write, or toggle an allowed ESP32 GPIO pin";
}

/**
 * gpio_tool_schema_json — 返回工具参数 schema 的静态 JSON 字符串，供工具注册给 LLM。
 *
 * @param self vtable 私有上下文；生命周期由创建该端口的实现管理。
 * @return 返回借用或静态只读字符串；调用方不得释放。
 */
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

/**
 * set_tool_error — 更新对象内部字段或输出结构，同时维护旧值释放规则。
 *
 * @param out_result 输出参数；调用方传入有效指针，成功后接收结果。
 * @param message 借用的对象；函数不释放该对象本身。
 */
static void set_tool_error(cc_tool_result_t *out_result, const char *message)
{
    out_result->ok = 0;
    out_result->error = strdup(message ? message : "GPIO tool error");
}

/**
 * ensure_direction — 将引脚切到本次操作需要的输入/输出模式。
 *
 * ESP-IDF 的 GPIO 配置是板级资源状态，不属于 core SDK。工具每次 read/write/toggle
 * 前都显式 reset + set_direction，牺牲一点性能换取 QEMU smoke 和交互 demo 的
 * 可预测性；configured[] 记录最近一次成功配置，便于后续扩展时做缓存策略。
 */
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

/**
 * gpio_tool_call — 参与工具注册、工具调用或工具结果写回流程。
 *
 * @param self vtable 私有上下文；生命周期由创建该端口的实现管理。
 * @param args_json 借用的只读字符串；函数不会释放该指针。
 * @param ctx 调用上下文；只在本次函数执行期间借用。
 * @param out_result 输出参数；调用方传入有效指针，成功后接收结果。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
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

/**
 * gpio_tool_destroy — 释放、停止或复位该组件拥有的资源，防止失败路径泄漏。
 *
 * @param self vtable 私有上下文；生命周期由创建该端口的实现管理。
 */
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

/**
 * cc_esp32_gpio_tool_create — 完成对应初始化步骤，失败时返回 cc_result_t 错误。
 *
 * @param out_tool 输出参数；调用方传入有效指针，成功后接收结果。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
cc_result_t cc_esp32_gpio_tool_create(cc_tool_t *out_tool)
{
    cc_esp32_gpio_tool_t *self = calloc(1, sizeof(*self));
    if (!self) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create ESP32 GPIO tool");
    out_tool->self = self;
    out_tool->vtable = &gpio_tool_vtable;
    return cc_result_ok();
}
