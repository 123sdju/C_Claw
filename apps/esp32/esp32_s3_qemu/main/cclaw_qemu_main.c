/**
 * 学习导读：apps/esp32/esp32_s3_qemu/main/cclaw_qemu_main.c
 *
 * 所属层次：ESP32 应用层。
 * 阅读重点：这里展示设备 profile 的能力裁剪和 QEMU 示例，阅读时重点看哪些桌面能力被禁用或替换。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

#include "cc/app/cc_agent_runtime.h"
#include "cc/app/cc_app_features.h"
#include "cc/app/cc_runtime_builder.h"
#include "cc/core/cc_message.h"
#include "cc/core/cc_result.h"
#include "cc/ports/cc_llm_provider.h"
#include "cc/ports/cc_tool_registry.h"
#include "cc/ports/cc_session_store.h"
#include "cc/ports/cc_platform.h"
#include "cc/ports/cc_thread.h"
#include "cc/util/cc_json.h"
#include "cc/util/cc_config.h"
#include "cc/util/cc_memory.h"
#include "esp_vfs_fat.h"
#include "esp_log.h"
#if CCLAW_QEMU_REAL_LLM
#include "esp_event.h"
#include "esp_eth.h"
#include "esp_eth_driver.h"
#include "esp_netif.h"
#include "esp_netif_types.h"
#include "nvs_flash.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wear_levelling.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "cclaw_qemu";
static const char *CHAT_SESSION_ID = "qemu-chat";
static const char *QEMU_SDCARD_MOUNT = "/sdcard";
static const char *QEMU_CCLAW_DIR = "/sdcard/cclaw";
static const char *QEMU_WORKSPACE_DIR = "/sdcard/cclaw/workspace";
static const char *QEMU_DATA_DIR = "/sdcard/cclaw/data";
static wl_handle_t s_sdcard_wl_handle = WL_INVALID_HANDLE;

extern cc_result_t cc_memory_session_store_create(cc_session_store_t *out_store);
extern cc_result_t cc_esp32_gpio_tool_create(cc_tool_t *out_tool);

#ifndef CCLAW_QEMU_REAL_LLM
#define CCLAW_QEMU_REAL_LLM 0
#endif

#ifndef CCLAW_QEMU_LLM_API_KEY
#define CCLAW_QEMU_LLM_API_KEY ""
#endif

#ifndef CCLAW_QEMU_LLM_BASE_URL
#define CCLAW_QEMU_LLM_BASE_URL ""
#endif

#ifndef CCLAW_QEMU_LLM_MODEL
#define CCLAW_QEMU_LLM_MODEL ""
#endif

#if CCLAW_QEMU_REAL_LLM
static volatile int s_qemu_got_ip = 0;
static esp_eth_handle_t s_eth_handle = NULL;
static esp_eth_netif_glue_handle_t s_eth_glue = NULL;
static esp_eth_mac_t *s_eth_mac = NULL;
static esp_eth_phy_t *s_eth_phy = NULL;

/* 学习注释：on_eth_got_ip 是本文件内部辅助函数。
 * 阅读时按“解析参数 → 加载配置 → 构建 runtime → 交给 gateway → cleanup”的顺序看资源生命周期。 */
static void on_eth_got_ip(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_id;
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "QEMU Ethernet got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    s_qemu_got_ip = 1;
}

/* 学习注释：qemu_network_start 是本文件内部辅助函数。
 * 阅读时按“解析参数 → 加载配置 → 构建 runtime → 交给 gateway → cleanup”的顺序看资源生命周期。 */
static cc_result_t qemu_network_start(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) return cc_result_error(CC_ERR_NETWORK, esp_err_to_name(err));

    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return cc_result_error(CC_ERR_NETWORK, esp_err_to_name(err));
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return cc_result_error(CC_ERR_NETWORK, esp_err_to_name(err));
    }

    esp_netif_inherent_config_t inherent = ESP_NETIF_INHERENT_DEFAULT_ETH();
    inherent.if_desc = "qemu_eth";
    inherent.route_prio = 64;
    esp_netif_config_t netif_config = {
        .base = &inherent,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH
    };
    esp_netif_t *netif = esp_netif_new(&netif_config);
    if (!netif) return cc_result_error(CC_ERR_NETWORK, "Failed to create Ethernet netif");

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    mac_config.rx_task_stack_size = 4096;
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = 1;
    phy_config.reset_gpio_num = -1;
    phy_config.autonego_timeout_ms = 100;

    s_eth_mac = esp_eth_mac_new_openeth(&mac_config);
    s_eth_phy = esp_eth_phy_new_dp83848(&phy_config);
    if (!s_eth_mac || !s_eth_phy) {
        return cc_result_error(CC_ERR_NETWORK, "Failed to create OpenETH driver");
    }

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(s_eth_mac, s_eth_phy);
    err = esp_eth_driver_install(&eth_config, &s_eth_handle);
    if (err != ESP_OK) return cc_result_error(CC_ERR_NETWORK, esp_err_to_name(err));

    s_eth_glue = esp_eth_new_netif_glue(s_eth_handle);
    if (!s_eth_glue) return cc_result_error(CC_ERR_NETWORK, "Failed to create Ethernet netif glue");
    err = esp_netif_attach(netif, s_eth_glue);
    if (err != ESP_OK) return cc_result_error(CC_ERR_NETWORK, esp_err_to_name(err));

    err = esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &on_eth_got_ip, NULL);
    if (err != ESP_OK) return cc_result_error(CC_ERR_NETWORK, esp_err_to_name(err));

    err = esp_eth_start(s_eth_handle);
    if (err != ESP_OK) return cc_result_error(CC_ERR_NETWORK, esp_err_to_name(err));

    for (int i = 0; i < 100 && !s_qemu_got_ip; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!s_qemu_got_ip) return cc_result_error(CC_ERR_TIMEOUT, "Timed out waiting for QEMU Ethernet IP");
    return cc_result_ok();
}
#endif

typedef struct thread_probe {
    cc_mutex_t mutex;
    int value;
} thread_probe_t;

#if CCLAW_QEMU_REAL_LLM
/* 学习注释：replace_config_string 是本文件内部辅助函数。
 * 阅读时按“解析参数 → 加载配置 → 构建 runtime → 交给 gateway → cleanup”的顺序看资源生命周期。 */
static int replace_config_string(char **field, const char *value)
{
    if (!value || !value[0]) return 1;
    char *copy = strdup(value);
    if (!copy) return 0;
    free(*field);
    *field = copy;
    return 1;
}
#endif

/* 学习注释：worker_increment 是本文件内部辅助函数。
 * 阅读时按“解析参数 → 加载配置 → 构建 runtime → 交给 gateway → cleanup”的顺序看资源生命周期。 */
static void *worker_increment(void *arg)
{
    thread_probe_t *probe = (thread_probe_t *)arg;
    cc_mutex_lock(probe->mutex);
    probe->value += 1;
    cc_mutex_unlock(probe->mutex);
    return NULL;
}

/* 学习注释：require_ok 是本文件内部辅助函数。
 * 阅读时按“解析参数 → 加载配置 → 构建 runtime → 交给 gateway → cleanup”的顺序看资源生命周期。 */
static void require_ok(const char *name, cc_result_t rc)
{
    if (rc.code == CC_OK) {
        ESP_LOGI(TAG, "%s: ok", name);
        return;
    }
    ESP_LOGE(TAG, "%s: %s", name, rc.message ? rc.message : cc_error_string(rc.code));
    cc_result_free(&rc);
    abort();
}

/* 学习注释：require_true 是本文件内部辅助函数。
 * 阅读时按“解析参数 → 加载配置 → 构建 runtime → 交给 gateway → cleanup”的顺序看资源生命周期。 */
static void require_true(const char *name, int condition)
{
    if (condition) {
        ESP_LOGI(TAG, "%s: ok", name);
        return;
    }
    ESP_LOGE(TAG, "%s: failed", name);
    abort();
}

/* 学习注释：test_json 是本文件内部辅助函数。
 * 阅读时按“解析参数 → 加载配置 → 构建 runtime → 交给 gateway → cleanup”的顺序看资源生命周期。 */
static void test_json(void)
{
    cc_json_value_t *root = NULL;
    require_ok("json_parse", cc_json_parse("{\"hello\":\"esp32s3\",\"n\":3}", &root));
    require_true("json_field", strcmp(cc_json_string_value(cc_json_object_get(root, "hello")), "esp32s3") == 0);
    require_true("json_number", cc_json_int_value(cc_json_object_get(root, "n")) == 3);
    char *text = cc_json_stringify_unformatted(root);
    require_true("json_stringify", text && strstr(text, "esp32s3"));
    free(text);
    cc_json_destroy(root);
}

/* 学习注释：test_message_copy 是本文件内部辅助函数。
 * 阅读时按“解析参数 → 加载配置 → 构建 runtime → 交给 gateway → cleanup”的顺序看资源生命周期。 */
static void test_message_copy(void)
{
    cc_message_t message;
    memset(&message, 0, sizeof(message));
    message.role = CC_ROLE_ASSISTANT;
    message.content = cc_strdup("calling tool");
    cc_result_t rc = cc_message_set_tool_calls_json(
        &message,
        "[{\"id\":\"call_1\",\"type\":\"function\",\"function\":{\"name\":\"memory\",\"arguments\":\"{}\"}}]");
    require_ok("message_tool_calls", rc);
    rc = cc_message_set_reasoning_content(&message, "short thought");
    require_ok("message_reasoning", rc);

    cc_message_t copy;
    rc = cc_message_copy(&message, &copy);
    require_ok("message_copy", rc);
    require_true("message_copy_content", copy.content && strcmp(copy.content, "calling tool") == 0);
    require_true("message_copy_tool_calls", copy.tool_calls_json && strstr(copy.tool_calls_json, "call_1"));
    require_true("message_copy_reasoning", copy.reasoning_content && strcmp(copy.reasoning_content, "short thought") == 0);

    cc_message_cleanup(&copy);
    cc_message_cleanup(&message);
}

/* 学习注释：test_thread_mutex 是本文件内部辅助函数。
 * 阅读时按“解析参数 → 加载配置 → 构建 runtime → 交给 gateway → cleanup”的顺序看资源生命周期。 */
static void test_thread_mutex(void)
{
    thread_probe_t probe;
    memset(&probe, 0, sizeof(probe));
    require_ok("mutex_create", cc_mutex_create(&probe.mutex));

    cc_thread_t t1;
    cc_thread_t t2;
    require_ok("thread_create_1", cc_thread_create(worker_increment, &probe, &t1));
    require_ok("thread_create_2", cc_thread_create(worker_increment, &probe, &t2));
    require_ok("thread_join_1", cc_thread_join(t1));
    require_ok("thread_join_2", cc_thread_join(t2));

    require_true("thread_mutex_value", probe.value == 2);
    cc_mutex_destroy(probe.mutex);
}

/* 学习注释：test_tool_registry 是本文件内部辅助函数。
 * 阅读时按“解析参数 → 加载配置 → 构建 runtime → 交给 gateway → cleanup”的顺序看资源生命周期。 */
static void test_tool_registry(void)
{
    cc_tool_registry_t *registry = NULL;
    require_ok("tool_registry_create", cc_tool_registry_create(&registry));
    require_ok("tool_registry_freeze", cc_tool_registry_freeze(registry));
    cc_tool_registry_destroy(registry);
}

/* 学习注释：test_gpio_tool 是本文件内部辅助函数。
 * 阅读时按“解析参数 → 加载配置 → 构建 runtime → 交给 gateway → cleanup”的顺序看资源生命周期。 */
static void test_gpio_tool(void)
{
    cc_tool_t tool;
    memset(&tool, 0, sizeof(tool));
    require_ok("gpio_tool_create", cc_esp32_gpio_tool_create(&tool));

    cc_tool_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.session_id = "qemu-gpio-test";
    ctx.workspace_dir = QEMU_WORKSPACE_DIR;

    cc_tool_result_t result;
    memset(&result, 0, sizeof(result));
    require_ok("gpio_tool_write", tool.vtable->call(
        tool.self,
        "{\"operation\":\"write\",\"pin\":2,\"level\":1}",
        &ctx,
        &result));
    require_true("gpio_tool_write_result", result.ok && result.content && strstr(result.content, "\"level\":1"));
    free(result.content);
    free(result.error);
    free(result.metadata_json);

    memset(&result, 0, sizeof(result));
    require_ok("gpio_tool_read", tool.vtable->call(
        tool.self,
        "{\"operation\":\"read\",\"pin\":2}",
        &ctx,
        &result));
    require_true("gpio_tool_read_result", result.ok && result.content && strstr(result.content, "\"pin\":2"));
    free(result.content);
    free(result.error);
    free(result.metadata_json);

    if (tool.vtable && tool.vtable->destroy) {
        tool.vtable->destroy(tool.self);
    }
}

/* 学习注释：mount_qemu_sdcard 是本文件内部辅助函数。
 * 阅读时按“解析参数 → 加载配置 → 构建 runtime → 交给 gateway → cleanup”的顺序看资源生命周期。 */
static void mount_qemu_sdcard(void)
{
    const esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = true,
        .max_files = 8,
        .allocation_unit_size = 4096
    };
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(
        QEMU_SDCARD_MOUNT,
        "sdcard",
        &mount_config,
        &s_sdcard_wl_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sdcard_mount: %s", esp_err_to_name(err));
        abort();
    }

    FILE *f = fopen("/sdcard/cclaw_qemu.txt", "w");
    if (!f) {
        ESP_LOGE(TAG, "sdcard_write_probe: failed");
        abort();
    }
    fputs("c-claw qemu sdcard ok\n", f);
    fclose(f);

    f = fopen("/sdcard/cclaw_qemu.txt", "r");
    if (!f) {
        ESP_LOGE(TAG, "sdcard_read_probe: failed");
        abort();
    }
    char probe[64];
    char *line = fgets(probe, sizeof(probe), f);
    fclose(f);
    require_true("sdcard_probe", line && strstr(probe, "qemu sdcard ok"));
    mkdir(QEMU_CCLAW_DIR, 0775);
    mkdir(QEMU_WORKSPACE_DIR, 0775);
    mkdir(QEMU_DATA_DIR, 0775);
    ESP_LOGI(TAG, "sdcard_mount: ok at %s", QEMU_SDCARD_MOUNT);
}

#if !CCLAW_QEMU_REAL_LLM
/* 学习注释：last_user_content_from_messages 是本文件内部辅助函数。
 * 阅读时按“解析参数 → 加载配置 → 构建 runtime → 交给 gateway → cleanup”的顺序看资源生命周期。 */
static const char *last_user_content_from_messages(const char *messages_json)
{
    static char content[512];
    content[0] = '\0';
    if (!messages_json) return "";

    cc_json_value_t *messages = NULL;
    cc_result_t rc = cc_json_parse(messages_json, &messages);
    if (rc.code != CC_OK || !messages) {
        cc_result_free(&rc);
        return "";
    }

    int count = cc_json_array_size(messages);
    for (int i = count - 1; i >= 0; i--) {
        cc_json_value_t *msg = cc_json_array_get(messages, i);
        const char *role = cc_json_string_value(cc_json_object_get(msg, "role"));
        if (!role || strcmp(role, "user") != 0) continue;
        const char *text = cc_json_string_value(cc_json_object_get(msg, "content"));
        if (text) {
            snprintf(content, sizeof(content), "%s", text);
        }
        break;
    }

    cc_json_destroy(messages);
    return content;
}

/* 学习注释：qemu_mock_chat 是本文件内部辅助函数。
 * 阅读时按“解析参数 → 加载配置 → 构建 runtime → 交给 gateway → cleanup”的顺序看资源生命周期。 */
static cc_result_t qemu_mock_chat(void *self, const cc_llm_chat_request_t *request, cc_llm_response_t *out)
{
    (void)self;
    memset(out, 0, sizeof(*out));

    const char *user = last_user_content_from_messages(request ? request->messages_json : NULL);
    char reply[768];
    if (strcmp(user, "/ping") == 0) {
        snprintf(reply, sizeof(reply), "pong from ESP32-S3 QEMU");
    } else if (strcmp(user, "/whoami") == 0) {
        snprintf(reply, sizeof(reply), "I am c-claw running inside an ESP32-S3 QEMU UART gateway.");
    } else {
        snprintf(reply, sizeof(reply), "ESP32 mock agent heard: %s", user[0] ? user : "(empty)");
    }

    out->has_text = 1;
    out->finished = 1;
    out->text = cc_strdup(reply);
    if (!out->text) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate mock response");
    return cc_result_ok();
}

/* 学习注释：qemu_mock_destroy 是本文件内部辅助函数。
 * 阅读时按“解析参数 → 加载配置 → 构建 runtime → 交给 gateway → cleanup”的顺序看资源生命周期。 */
static void qemu_mock_destroy(void *self)
{
    (void)self;
}

static cc_llm_provider_vtable_t qemu_mock_llm_vtable = {
    qemu_mock_chat,
    NULL,
    qemu_mock_destroy
};
#endif

/* 学习注释：trim_line 是本文件内部辅助函数。
 * 阅读时按“解析参数 → 加载配置 → 构建 runtime → 交给 gateway → cleanup”的顺序看资源生命周期。 */
static void trim_line(char *line)
{
    if (!line) return;
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        line[--len] = '\0';
    }
}

/* 学习注释：run_uart_chat 是本文件内部辅助函数。
 * 阅读时按“解析参数 → 加载配置 → 构建 runtime → 交给 gateway → cleanup”的顺序看资源生命周期。 */
static void run_uart_chat(void)
{
#if CCLAW_QEMU_REAL_LLM
    cc_config_t config;
    cc_runtime_builder_t *builder = NULL;
    cc_agent_runtime_t *runtime = NULL;
    memset(&config, 0, sizeof(config));

    cc_result_t rc = qemu_network_start();
    require_ok("chat_network_start", rc);
    rc = cc_config_load(CC_DEFAULT_CONFIG_PATH, &config);
    if (rc.code != CC_OK) {
        cc_result_free(&rc);
    }
    require_true("chat_config_api_key",
        replace_config_string(&config.api_key, CCLAW_QEMU_LLM_API_KEY));
    require_true("chat_config_base_url",
        replace_config_string(&config.base_url, CCLAW_QEMU_LLM_BASE_URL));
    require_true("chat_config_model",
        replace_config_string(&config.model, CCLAW_QEMU_LLM_MODEL));
    require_true("chat_config_workspace",
        replace_config_string(&config.workspace_path, QEMU_WORKSPACE_DIR));
    require_true("chat_config_data_dir",
        replace_config_string(&config.data_dir, QEMU_DATA_DIR));

    rc = cc_runtime_builder_create(&config, cc_app_default_features(), &builder);
    require_ok("chat_runtime_builder_create", rc);
    runtime = cc_runtime_builder_runtime(builder);
    rc = cc_agent_runtime_create_session(runtime, CHAT_SESSION_ID, QEMU_WORKSPACE_DIR);
    require_ok("chat_session_create", rc);
#else
    cc_tool_registry_t *registry = NULL;
    cc_session_store_t store;
    cc_llm_provider_t llm;
    cc_agent_runtime_t *runtime = NULL;
    memset(&store, 0, sizeof(store));
    memset(&llm, 0, sizeof(llm));

    cc_result_t rc = cc_tool_registry_create(&registry);
    require_ok("chat_tool_registry_create", rc);
    rc = cc_tool_registry_freeze(registry);
    require_ok("chat_tool_registry_freeze", rc);
    rc = cc_memory_session_store_create(&store);
    require_ok("chat_store_create", rc);

    llm.self = NULL;
    llm.vtable = &qemu_mock_llm_vtable;

    cc_agent_runtime_config_t config = {
        .max_steps = 2,
        .system_prompt =
            "You are c-claw running on ESP32-S3 QEMU. Keep replies concise. "
            "Treat only user messages beginning with '/' as local commands handled by the UART shell. "
            "For all other input, including Chinese text, answer as a normal chat assistant. "
            "The writable workspace is /sdcard/cclaw/workspace.",
        .workspace_dir = (char *)QEMU_WORKSPACE_DIR,
        .model = CCLAW_QEMU_LLM_MODEL[0] ? CCLAW_QEMU_LLM_MODEL : "qemu-mock"
    };

    cc_agent_runtime_deps_t deps;
    memset(&deps, 0, sizeof(deps));
    deps.llm = llm;
    deps.tool_registry = registry;
    deps.store = store;

    cc_agent_runtime_options_t options;
    memset(&options, 0, sizeof(options));
    options.config = config;

    rc = cc_agent_runtime_create(&deps, &options, &runtime);
    require_ok("chat_runtime_create", rc);
    rc = cc_agent_runtime_create_session(runtime, CHAT_SESSION_ID, QEMU_WORKSPACE_DIR);
    require_ok("chat_session_create", rc);
#endif

    printf("\nC-Claw ESP32-S3 interactive chat ready (%s).\n",
#if CCLAW_QEMU_REAL_LLM
        "real OpenAI-compatible LLM"
#else
        "local mock LLM"
#endif
    );
    printf("Workspace: %s\n", QEMU_WORKSPACE_DIR);
    printf("Type text and press Enter. Commands: /ping, /whoami, /help, /exit\n");
    printf("To quit QEMU from a terminal, press Ctrl-A then X.\n\n");
    fflush(stdout);

    char line[512];
    int prompt_pending = 1;
    while (1) {
        if (prompt_pending) {
            printf("you> ");
            fflush(stdout);
            prompt_pending = 0;
        }
        if (!fgets(line, sizeof(line), stdin)) {
            vTaskDelay(pdMS_TO_TICKS(100));
            clearerr(stdin);
            continue;
        }
        prompt_pending = 1;
        trim_line(line);
        if (line[0] == '\0') continue;
        if (strcmp(line, "/help") == 0) {
            printf("agent> Commands: /ping, /whoami, /help, /exit\n");
            continue;
        }
        if (strcmp(line, "/exit") == 0) {
            printf("agent> chat loop stopped. Use Ctrl-A then X to close QEMU.\n");
            break;
        }

        char *response = NULL;
        rc = cc_agent_runtime_handle_message(runtime, CHAT_SESSION_ID, line, &response);
        if (rc.code != CC_OK) {
            printf("agent error> %s\n", rc.message ? rc.message : cc_error_string(rc.code));
            cc_result_free(&rc);
            free(response);
            continue;
        }
        printf("agent> %s\n", response ? response : "");
        free(response);
    }

#if CCLAW_QEMU_REAL_LLM
    cc_runtime_builder_destroy(builder);
    cc_config_destroy(&config);
#else
    cc_agent_runtime_destroy(runtime);
    if (llm.vtable && llm.vtable->destroy) llm.vtable->destroy(llm.self);
    if (store.vtable && store.vtable->destroy) store.vtable->destroy(store.self);
    cc_tool_registry_destroy(registry);
#endif
}

/* 学习注释：app_main 是 ESP-IDF 固件入口。
 * 阅读时按“解析参数 → 加载配置 → 构建 runtime → 交给 gateway → cleanup”的顺序看资源生命周期。 */
void app_main(void)
{
    setvbuf(stdin, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);

    ESP_LOGI(TAG, "starting c-claw ESP32-S3 QEMU smoke test");
    ESP_LOGI(TAG, "CC_PLATFORM=%d", CC_PLATFORM);
    require_true("platform_is_esp32", CC_PLATFORM == CC_PLATFORM_ESP32);

    test_json();
    test_message_copy();
    test_thread_mutex();
    test_tool_registry();
    mount_qemu_sdcard();
    test_gpio_tool();

    printf("CCLAW_QEMU_PASS\n");
    fflush(stdout);

    run_uart_chat();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
