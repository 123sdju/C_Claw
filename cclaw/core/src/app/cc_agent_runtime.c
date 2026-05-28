



#include "cc_agent_runtime_internal.h"
#include "cc/app/cc_context_builder.h"
#include "cc/app/cc_tool_executor.h"
#include "cc/core/cc_media.h"
#include "cc/core/cc_observability.h"
#include "cc/util/cc_string_builder.h"
#include "cc/util/cc_json.h"
#include "cc/ports/cc_thread.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

static cc_mutex_t g_id_mutex = NULL;
static unsigned long g_id_counter = 0;

/* 释放配置里的字符串列表；runtime 创建时会深拷贝，销毁时必须成对清理。 */
static void runtime_string_list_cleanup(cc_config_string_list_t *list)
{
    if (!list) return;
    for (size_t i = 0; i < list->count; i++) free(list->items[i]);
    free(list->items);
    memset(list, 0, sizeof(*list));
}

/* 深拷贝字符串列表，保证 runtime 生命周期不依赖外部 config 内存。 */
static cc_result_t runtime_string_list_copy(
    const cc_config_string_list_t *src,
    cc_config_string_list_t *dst
)
{
    if (!dst) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null string list copy output");
    }
    memset(dst, 0, sizeof(*dst));
    if (!src || src->count == 0) return cc_result_ok();

    dst->items = calloc(src->count, sizeof(char *));
    if (!dst->items) {
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy string list");
    }
    dst->count = src->count;
    for (size_t i = 0; i < src->count; i++) {
        if (!src->items[i]) continue;
        dst->items[i] = strdup(src->items[i]);
        if (!dst->items[i]) {
            runtime_string_list_cleanup(dst);
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy string list item");
        }
    }
    return cc_result_ok();
}

/* 把配置层的多模态限制转换成 provider 请求使用的只读 limits 视图。 */
static void runtime_media_limits_from_config(
    const cc_multimodal_config_t *config,
    cc_media_limits_t *out_limits
)
{
    if (!out_limits) return;
    memset(out_limits, 0, sizeof(*out_limits));
    if (!config) {
        cc_media_limits_text_only(out_limits);
        return;
    }
    out_limits->max_artifacts = config->limits.max_artifacts;
    out_limits->max_artifact_bytes = config->limits.max_artifact_bytes;
    out_limits->max_base64_bytes = config->limits.max_base64_bytes;
    out_limits->allow_inline_base64 = config->limits.allow_inline_base64;
    out_limits->allowed_mime_prefixes =
        (const char **)config->limits.allowed_mime_prefixes.items;
    out_limits->allowed_mime_prefix_count =
        config->limits.allowed_mime_prefixes.count;
}

/*
 * Runtime 业务路径统一从这里发布观测事件。
 *
 * event bus 是底层传输，runtime 不再直接拼 payload 调底层发布函数。
 * attributes_json 必须是 object 文本；调用方只在需要补充事件私有字段时传入。
 */
static void runtime_publish_observability(
    cc_agent_runtime_t *runtime,
    const char *event_name,
    const char *session_id,
    int step,
    const char *status,
    const char *message,
    const cc_result_t *error,
    const char *attributes_json
)
{
    if (!runtime || !runtime->event_bus || !event_name) return;

    cc_observability_event_t event;
    memset(&event, 0, sizeof(event));
    event.size = sizeof(event);
    event.event = event_name;
    event.session_id = session_id;
    event.step = step;
    event.status = status ? status : "";
    event.message = message;
    event.error = error;
    event.attributes_json = attributes_json;

    cc_result_t rc = cc_observability_publish(runtime->event_bus, &event);
    cc_result_free(&rc);
}

/*
 * Stream 文本类事件都带 content attribute。这样 text/thinking/delta/error 在
 * 下游保持同一个读取位置，不再依赖旧版裸字符串 payload。
 */
static void runtime_publish_stream_content(
    cc_agent_runtime_t *runtime,
    const char *event_name,
    const char *session_id,
    int step,
    const char *status,
    const char *content
)
{
    if (!runtime || !runtime->event_bus || !event_name) return;

    cc_json_value_t *attrs = cc_json_create_object();
    if (!attrs) return;
    cc_json_object_set(attrs, "content",
        cc_json_create_string(content ? content : ""));
    char *attrs_json = cc_json_stringify_unformatted(attrs);
    cc_json_destroy(attrs);
    if (!attrs_json) return;

    runtime_publish_observability(runtime, event_name, session_id, step,
        status, content, NULL, attrs_json);
    free(attrs_json);
}


/* ID 生成器需要跨线程安全，因为 run queue 或多 session 场景可能并发创建消息。 */
static void ensure_id_mutex(void)
{
    if (!g_id_mutex) {
        cc_mutex_create(&g_id_mutex);
    }
}


/* 生成 SDK 内部消息 ID；当前只要求进程内唯一，不作为持久化全局 ID。 */
static char *generate_id(void)
{
    char buf[64];
    ensure_id_mutex();
    unsigned long next = 0;
    if (g_id_mutex) {
        cc_mutex_lock(g_id_mutex);
        next = ++g_id_counter;
        cc_mutex_unlock(g_id_mutex);
    } else {


        next = ++g_id_counter;
    }
    snprintf(buf, sizeof(buf), "msg_%ld_%lu", (long)time(NULL), next);
    return strdup(buf);
}


/*
 * 主动记忆写入钩子。
 *
 * 它只在最终 assistant 回复完成后写入，不在 stream partial、取消或错误路径写入，
 * 避免把不完整响应固化到长期记忆。
 */
static void active_memory_after_run(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const char *user_input,
    const char *assistant_text
)
{
#if CC_ENABLE_ACTIVE_MEMORY
    if (!runtime || !runtime->memory_store || !runtime->memory_store->vtable) return;
    if (!runtime->config.active_memory_enabled ||
        !runtime->config.active_memory_write_summary) return;
    if ((!user_input || !user_input[0]) && (!assistant_text || !assistant_text[0])) return;

    cc_string_builder_t sb;
    if (cc_string_builder_init(&sb).code != CC_OK) return;
    cc_string_builder_append(&sb, "User: ");
    cc_string_builder_append(&sb, user_input ? user_input : "");
    cc_string_builder_append(&sb, "\nAssistant: ");
    cc_string_builder_append(&sb, assistant_text ? assistant_text : "");
    char *value = cc_string_builder_take(&sb);
    if (!value) {
        cc_string_builder_deinit(&sb);
        return;
    }

    int max_chars = runtime->config.active_memory_max_value_chars > 0 ?
        runtime->config.active_memory_max_value_chars : 1600;
    if ((int)strlen(value) > max_chars) value[max_chars] = '\0';

    char *id = generate_id();
    if (!id) {
        free(value);
        return;
    }
    size_t key_len = strlen("active.") + strlen(id) + 1;
    char *key = malloc(key_len);
    if (!key) {
        free(id);
        free(value);
        return;
    }
    snprintf(key, key_len, "active.%s", id);
    free(id);

    cc_result_t rc = cc_memory_store_set(
        runtime->memory_store,
        key,
        value,
        runtime->config.active_memory_category ?
            runtime->config.active_memory_category : "active_summary",
        session_id
    );
    cc_result_free(&rc);
    free(key);
    free(value);
#else
    (void)runtime;
    (void)session_id;
    (void)user_input;
    (void)assistant_text;
#endif
}

/* 从同步 run options 中取取消 token；NULL 表示调用方没有启用取消。 */
static cc_cancel_token_t *run_cancel_token(const cc_agent_runtime_run_options_t *options)
{
    return options ? options->cancel_token : NULL;
}

/* 统一把取消状态映射成 CC_ERR_CANCELLED，便于上层做恢复或 UI 状态更新。 */
static cc_result_t check_run_cancelled(const cc_agent_runtime_run_options_t *options, const char *message)
{
    if (cc_cancel_token_is_cancelled(run_cancel_token(options))) {
        return cc_result_error(CC_ERR_CANCELLED, message ? message : "Agent run cancelled");
    }
    return cc_result_ok();
}

/* stream options 有独立 token；stream callback API 可以不依赖普通 run options。 */
static cc_cancel_token_t *stream_cancel_token(const cc_agent_runtime_stream_options_t *options)
{
    return options ? options->cancel_token : NULL;
}

/* max_steps 同时支持旧字段和统一 limits 字段；limits 优先表达新的资源模型。 */
static int runtime_effective_max_steps(const cc_agent_runtime_t *runtime)
{
    if (!runtime) return 1;
    if (runtime->config.limits.max_steps > 0) return runtime->config.limits.max_steps;
    if (runtime->config.max_steps > 0) return runtime->config.max_steps;
    return 1;
}

/* 输入字节限制在写入 session 之前执行，避免超大请求进入历史上下文。 */
static cc_result_t check_input_limit(
    const cc_agent_runtime_t *runtime,
    const char *input
)
{
    size_t max_bytes = runtime ? runtime->config.limits.max_input_bytes : 0;
    if (max_bytes > 0 && input && strlen(input) > max_bytes) {
        return cc_result_errf(
            CC_ERR_LIMIT_EXCEEDED,
            "Input exceeds max_input_bytes (%zu)",
            max_bytes);
    }
    return cc_result_ok();
}

/* 输出字节限制在落库之前执行，超限响应不会写入 assistant final。 */
static cc_result_t check_output_limit(
    const cc_agent_runtime_t *runtime,
    const char *output,
    const char *label
)
{
    size_t max_bytes = runtime ? runtime->config.limits.max_output_bytes : 0;
    if (max_bytes > 0 && output && strlen(output) > max_bytes) {
        return cc_result_errf(
            CC_ERR_LIMIT_EXCEEDED,
            "%s exceeds max_output_bytes (%zu)",
            label ? label : "Output",
            max_bytes);
    }
    return cc_result_ok();
}

/*
 * Runtime 创建阶段的 provider 能力协商。
 *
 * 这里选择 fail-fast：如果配置启用了 tool、stream 或多模态能力，但 provider 不声明支持，
 * runtime 创建直接失败，避免运行时静默降级。
 */
static cc_result_t validate_provider_capabilities(
    const cc_agent_runtime_deps_t *deps,
    const cc_agent_runtime_config_t *config
)
{
    if (!deps || !deps->llm.vtable || !deps->llm.vtable->capabilities) {
        return cc_result_ok();
    }

    cc_llm_provider_capabilities_t caps;
    memset(&caps, 0, sizeof(caps));
    cc_result_t rc = deps->llm.vtable->capabilities(deps->llm.self, &caps);
    if (rc.code != CC_OK) return rc;

    if (!caps.text_input || !caps.text_output) {
        return cc_result_error(CC_ERR_UNSUPPORTED,
            "Provider must support text input and output");
    }

    if (deps->tool_registry && cc_tool_registry_count(deps->tool_registry) > 0 &&
        !caps.tool_calling) {
        return cc_result_error(CC_ERR_UNSUPPORTED,
            "Configured tools require provider tool-calling support");
    }

    if (!config) return cc_result_ok();
    if (config->multimodal.input.image && !caps.image_input)
        return cc_result_error(CC_ERR_UNSUPPORTED, "Provider does not support image input");
    if (config->multimodal.input.audio && !caps.audio_input)
        return cc_result_error(CC_ERR_UNSUPPORTED, "Provider does not support audio input");
    if (config->multimodal.input.video && !caps.video_input)
        return cc_result_error(CC_ERR_UNSUPPORTED, "Provider does not support video input");
    if (config->multimodal.input.file && !caps.file_input)
        return cc_result_error(CC_ERR_UNSUPPORTED, "Provider does not support file input");
    if (config->multimodal.output.image && !caps.image_output)
        return cc_result_error(CC_ERR_UNSUPPORTED, "Provider does not support image output");
    if (config->multimodal.output.audio && !caps.audio_output)
        return cc_result_error(CC_ERR_UNSUPPORTED, "Provider does not support audio output");
    if (config->multimodal.output.video && !caps.video_output)
        return cc_result_error(CC_ERR_UNSUPPORTED, "Provider does not support video output");
    if (config->multimodal.output.file && !caps.file_output)
        return cc_result_error(CC_ERR_UNSUPPORTED, "Provider does not support file output");

    return cc_result_ok();
}

/*
 * provider 没有真正 streaming 时的降级路径。
 *
 * 它用同步回复模拟 text chunk，但仍通过 stream callback 和统一 observability schema 输出。
 * 这条路径只在调用方没有强制要求 provider 原生 stream 时使用。
 */
static cc_result_t cc_agent_runtime_handle_stream_fallback(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const char *user_input,
    const cc_agent_runtime_run_options_t *options,
    const cc_agent_runtime_stream_options_t *stream_options,
    char **out_response
)
{
    char *sync_response = NULL;
    cc_result_t rc = cc_agent_runtime_handle_message_with_options(
        runtime, session_id, user_input, options, &sync_response);



    if (sync_response && (runtime->event_bus ||
        (stream_options && stream_options->on_chunk))) {
        const char *p = sync_response;
        const char *start = p;
        size_t emitted = 0;
        while (*p) {
            if (*p == ' ' || *p == '\n') {
                if (p > start) {
                    char word[256] = {0};
                    size_t wlen = (size_t)(p - start);
                    if (wlen > 255) wlen = 255;
                    strncpy(word, start, wlen);
                    word[wlen] = *p;
                    word[wlen + 1] = '\0';
                    emitted += strlen(word);
                    if (stream_options && stream_options->on_chunk) {
                        cc_stream_chunk_t chunk = { CC_STREAM_CHUNK_TEXT, word, NULL, NULL };
                        stream_options->on_chunk(&chunk, stream_options->user_data);
                    }
                    runtime_publish_stream_content(runtime, CC_OBS_EVENT_STREAM_TEXT,
                        session_id, 0, "delta", word);
                }
                start = p + 1;
            }
            p++;
        }
        if (p > start) {
            char word[256] = {0};
            size_t wlen = (size_t)(p - start);
            if (wlen > 255) wlen = 255;
            strncpy(word, start, wlen);
            word[wlen] = '\0';
            emitted += strlen(word);
            if (stream_options && stream_options->on_chunk) {
                cc_stream_chunk_t chunk = { CC_STREAM_CHUNK_TEXT, word, NULL, NULL };
                stream_options->on_chunk(&chunk, stream_options->user_data);
            }
            runtime_publish_stream_content(runtime, CC_OBS_EVENT_STREAM_TEXT,
                session_id, 0, "delta", word);
        }
        if (runtime->config.limits.max_stream_bytes > 0 &&
            emitted > runtime->config.limits.max_stream_bytes) {
            free(sync_response);
            return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Stream exceeds max_stream_bytes");
        }
    }

    if (sync_response) {
        *out_response = sync_response;
    }
    runtime_publish_observability(runtime, CC_OBS_EVENT_STREAM_FINISHED,
        session_id, 0, "finished", NULL, NULL, NULL);
    if (stream_options && stream_options->on_chunk) {
        cc_stream_chunk_t done = { CC_STREAM_CHUNK_FINISHED, NULL, NULL, NULL };
        stream_options->on_chunk(&done, stream_options->user_data);
    }
    return rc;
}


/*
 * 创建 runtime 并接管一份配置副本。
 *
 * provider/store/registry/event_bus 等端口仍由外部持有；runtime 只保存 vtable/self 引用。
 * 这种依赖注入方式是 C 语言里实现可测试、可移植 SDK 的核心设计。
 */
cc_result_t cc_agent_runtime_create(
    const cc_agent_runtime_deps_t *deps,
    const cc_agent_runtime_options_t *options,
    cc_agent_runtime_t **out_runtime
)
{
    if (!deps || !options || !out_runtime) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null runtime create argument");
    }
    cc_result_t caps_rc = validate_provider_capabilities(deps, &options->config);
    if (caps_rc.code != CC_OK) return caps_rc;

    ensure_id_mutex();
    cc_agent_runtime_config_t config = options->config;
    cc_agent_runtime_t *runtime = calloc(1, sizeof(cc_agent_runtime_t));
    if (!runtime) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create agent runtime");

    cc_result_t mutex_rc = cc_mutex_create(&runtime->mutex);
    if (mutex_rc.code != CC_OK) {
        free(runtime);
        return mutex_rc;
    }


    cc_config_string_list_t allowed_mime_prefixes =
        config.multimodal.limits.allowed_mime_prefixes;
    runtime->config = config;
    memset(&runtime->config.multimodal.limits.allowed_mime_prefixes, 0,
        sizeof(runtime->config.multimodal.limits.allowed_mime_prefixes));
    cc_result_t copy_rc = runtime_string_list_copy(
        &allowed_mime_prefixes,
        &runtime->config.multimodal.limits.allowed_mime_prefixes);
    if (copy_rc.code != CC_OK) {
        cc_mutex_destroy(runtime->mutex);
        free(runtime);
        return copy_rc;
    }
    if (runtime->config.system_prompt)
        runtime->config.system_prompt = strdup(config.system_prompt);
    if (runtime->config.workspace_dir)
        runtime->config.workspace_dir = strdup(config.workspace_dir);
    if (runtime->config.model)
        runtime->config.model = strdup(config.model);
    if (runtime->config.active_memory_category)
        runtime->config.active_memory_category = strdup(config.active_memory_category);


    runtime->llm = deps->llm;
    runtime->tool_registry = deps->tool_registry;
    runtime->store = deps->store;
    runtime->policy = deps->policy;
    runtime->sandbox = deps->sandbox;
    runtime->event_bus = deps->event_bus;
    runtime->logger = deps->logger;
    runtime->memory_store = deps->memory_store;
    runtime->tool_pool = deps->tool_pool;
    runtime->thinking_mode = options->thinking_mode;
    runtime->services.event_bus = deps->event_bus;
    runtime->services.logger = deps->logger;
    runtime->services.memory_store = deps->memory_store;
    runtime->services.tool_pool = deps->tool_pool;
    runtime->services.approve_tool_call = deps->approve_tool_call;
    runtime->services.approval_user_data = deps->approval_user_data;

    *out_runtime = runtime;
    return cc_result_ok();
}


/* 写入最终 assistant 文本；只有完整 final response 才调用，partial stream 不落库。 */
cc_result_t cc_agent_runtime_store_assistant_text(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const char *text,
    const char *reasoning_content
)
{
    if (!runtime || !session_id) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null assistant text argument");
    }
    cc_message_t *assistant_msg = NULL;
    char *aid = generate_id();
    cc_result_t rc = cc_message_create_text(
        aid, session_id, CC_ROLE_ASSISTANT, text ? text : "", NULL, &assistant_msg);
    free(aid);
    if (rc.code != CC_OK) return rc;
    if (reasoning_content && reasoning_content[0]) {
        rc = cc_message_set_reasoning_content(assistant_msg, reasoning_content);
        if (rc.code != CC_OK) {
            cc_message_destroy(assistant_msg);
            return rc;
        }
    }
    if (runtime->store.vtable && runtime->store.vtable->append_message) {
        rc = runtime->store.vtable->append_message(runtime->store.self, assistant_msg);
    }
    cc_message_destroy(assistant_msg);
    return rc;
}

/* 把工具产生的 artifact 摘要追加为 observation，让下一轮模型能理解工具产物。 */
static cc_result_t cc_agent_runtime_append_artifact_observation(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const cc_tool_result_t *tool_result
)
{
    if (!runtime || !session_id || !tool_result ||
        tool_result->artifacts.count == 0) {
        return cc_result_ok();
    }
    if (!runtime->store.vtable || !runtime->store.vtable->append_message) {
        return cc_result_ok();
    }

    char *summary = NULL;
    cc_result_t rc = cc_media_artifact_list_summarize(&tool_result->artifacts, &summary);
    if (rc.code != CC_OK) return rc;

    cc_string_builder_t sb;
    rc = cc_string_builder_init(&sb);
    if (rc.code == CC_OK) {
        rc = cc_string_builder_append(&sb, "Tool produced multimodal artifacts.\n");
    }
    if (rc.code == CC_OK && summary && summary[0]) {
        rc = cc_string_builder_append(&sb, summary);
    }
    free(summary);
    if (rc.code != CC_OK) {
        cc_string_builder_deinit(&sb);
        return rc;
    }

    char *content = cc_string_builder_take(&sb);
    if (!content) {
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to build artifact observation");
    }

    cc_content_parts_t parts;
    cc_content_parts_init(&parts);
    rc = cc_content_parts_append_text(&parts, content, CC_CONTENT_PART_INPUT);
    for (size_t i = 0; rc.code == CC_OK && i < tool_result->artifacts.count; i++) {
        rc = cc_content_parts_append_artifact(
            &parts, &tool_result->artifacts.items[i], CC_CONTENT_PART_INPUT);
    }
    if (rc.code != CC_OK) {
        cc_content_parts_cleanup(&parts);
        free(content);
        return rc;
    }
    cc_message_t *msg = NULL;
    char *id = generate_id();
    rc = cc_message_create_parts(id, session_id, CC_ROLE_USER, &parts, NULL, &msg);
    free(id);
    cc_content_parts_cleanup(&parts);
    free(content);
    if (rc.code == CC_OK) {
        rc = runtime->store.vtable->append_message(runtime->store.self, msg);
    }
    cc_message_destroy(msg);
    return rc;
}


/* 同步 run 的工具步骤：执行工具、审计 tool call/result，并把 tool 消息写回历史。 */
cc_result_t cc_agent_runtime_execute_tool_step(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const cc_tool_call_t *call,
    const char *reasoning_content,
    cc_cancel_token_t *cancel_token
)
{
    if (!runtime || !session_id || !call) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null tool step argument");
    }

    cc_message_t *asst_msg = NULL;
    char *aid = generate_id();
    cc_result_t rc = cc_message_create_text(aid, session_id, CC_ROLE_ASSISTANT,
        NULL, call->id, &asst_msg);
    free(aid);
    if (rc.code == CC_OK) {
        rc = cc_message_add_tool_call(asst_msg, call);
    }
    if (rc.code == CC_OK && reasoning_content && reasoning_content[0]) {
        rc = cc_message_set_reasoning_content(asst_msg, reasoning_content);
    }
    if (rc.code != CC_OK) {
        cc_message_destroy(asst_msg);
        return rc;
    }
    if (runtime->store.vtable && runtime->store.vtable->append_message) {
        runtime->store.vtable->append_message(runtime->store.self, asst_msg);
    }
    cc_message_destroy(asst_msg);

    cc_tool_result_t tool_result;
    memset(&tool_result, 0, sizeof(tool_result));
    cc_tool_executor_options_t exec_options;
    memset(&exec_options, 0, sizeof(exec_options));
    exec_options.cancel_token = cancel_token;
    rc = cc_tool_executor_execute_with_options(
        runtime, session_id, call, &exec_options, &tool_result);
    if (rc.code != CC_OK) {
        free(tool_result.text);
        free(tool_result.error);
        free(tool_result.metadata);
        cc_media_artifact_list_cleanup(&tool_result.artifacts);
        return rc;
    }

    if (runtime->store.vtable && runtime->store.vtable->append_tool_call) {
        runtime->store.vtable->append_tool_call(runtime->store.self, session_id, call);
    }
    if (runtime->store.vtable && runtime->store.vtable->append_tool_result) {
        runtime->store.vtable->append_tool_result(
            runtime->store.self, session_id, call->id, &tool_result);
    }

    cc_message_t *tool_msg = NULL;
    char *tid = generate_id();
    rc = cc_message_create_text(tid, session_id, CC_ROLE_TOOL,
        tool_result.ok ? tool_result.text : tool_result.error,
        call->id, &tool_msg);
    free(tid);
    if (rc.code == CC_OK && runtime->store.vtable && runtime->store.vtable->append_message) {
        runtime->store.vtable->append_message(runtime->store.self, tool_msg);
    }
    cc_message_destroy(tool_msg);
    if (rc.code == CC_OK) {
        rc = cc_agent_runtime_append_artifact_observation(
            runtime, session_id, &tool_result);
    }
    free(tool_result.text);
    free(tool_result.error);
    free(tool_result.metadata);
    cc_media_artifact_list_cleanup(&tool_result.artifacts);
    return rc.code == CC_OK ? cc_result_ok() : rc;
}

/*
 * 流式主循环的临时状态。
 *
 * 这个结构体只在一次 stream run 的栈帧中存在，不跨线程共享；其中 runtime/session_id
 * 是借用引用，builder 和 cur_tool_* 是本循环拥有并负责释放的资源。把这些状态集中起来，
 * 可以让 provider callback 在没有全局变量的情况下累积 text、thinking 和 tool 参数，
 * 这也是嵌入式 C 中常见的“显式上下文指针”写法。
 */
typedef struct {
    cc_agent_runtime_t *runtime;
    const char *session_id;
    int step;
    int chunk_count;
    cc_string_builder_t text_builder;
    cc_string_builder_t thinking_builder;
    cc_string_builder_t args_builder;
    char *cur_tool_name;
    char *cur_tool_id;
    int has_tool_call;
    int finished;
    int cancelled;
    int limit_exceeded;
    cc_cancel_token_t *cancel_token;
    cc_agent_runtime_stream_callback_fn on_chunk;
    void *chunk_user_data;
    size_t emitted_stream_bytes;
    size_t max_stream_bytes;
    char *response_text;
} stream_loop_ctx_t;




/*
 * stream provider 可能把一个 tool call 拆成 start/delta/end 多个 chunk。
 *
 * 这个函数在 end 到达时把累积的 arguments 组装成 cc_tool_call_t，调用统一工具执行器，
 * 再把 tool result 写回 session store。这样同步和流式工具执行共享同一套安全策略、
 * 参数校验、approval 和资源限制。函数内部对 cur_tool_name/cur_tool_id 拥有释放责任，
 * 调用完成后会清空当前工具状态，避免下一段 chunk 误复用上一次工具调用。
 */
static void execute_pending_tool(stream_loop_ctx_t *ctx)
{
    if (!ctx->cur_tool_name) return;
    if (cc_cancel_token_is_cancelled(ctx->cancel_token)) {
        ctx->cancelled = 1;
        return;
    }

    const char *arguments = cc_string_builder_cstr(&ctx->args_builder);
    if (!arguments || strlen(arguments) == 0) {
        arguments = "{}";
    }


    cc_message_t *tc_msg = NULL;
    char *tid = generate_id();
    cc_message_create_text(tid, ctx->session_id, CC_ROLE_ASSISTANT,
        NULL, ctx->cur_tool_id ? ctx->cur_tool_id : "", &tc_msg);
    free(tid);
    cc_tool_call_t stored_call;
    memset(&stored_call, 0, sizeof(stored_call));
    stored_call.id = ctx->cur_tool_id;
    stored_call.name = ctx->cur_tool_name;
    stored_call.arguments_json = (char *)arguments;
    cc_message_add_tool_call(tc_msg, &stored_call);
    const char *thinking = cc_string_builder_cstr(&ctx->thinking_builder);
    if (thinking && strlen(thinking) > 0) {
        cc_message_set_reasoning_content(tc_msg, thinking);
    }

    if (ctx->runtime->store.vtable && ctx->runtime->store.vtable->append_message) {
        ctx->runtime->store.vtable->append_message(ctx->runtime->store.self, tc_msg);
    }
    cc_message_destroy(tc_msg);



    cc_tool_call_t call;
    memset(&call, 0, sizeof(call));
    call.name = ctx->cur_tool_name;
    call.arguments_json = (char *)arguments;
    if (ctx->cur_tool_id) {
        call.id = ctx->cur_tool_id;
    }

    cc_tool_result_t tres = {0};
    cc_tool_executor_options_t exec_options;
    memset(&exec_options, 0, sizeof(exec_options));
    exec_options.cancel_token = ctx->cancel_token;
    cc_result_t exec_rc = cc_tool_executor_execute_with_options(
        ctx->runtime, ctx->session_id, &call, &exec_options, &tres);
    if (exec_rc.code != CC_OK) {
        ctx->cancelled = exec_rc.code == CC_ERR_CANCELLED;
        cc_result_free(&exec_rc);
        free(ctx->cur_tool_name);
        ctx->cur_tool_name = NULL;
        free(ctx->cur_tool_id);
        ctx->cur_tool_id = NULL;
        cc_string_builder_clear(&ctx->args_builder);
        return;
    }

    if (ctx->runtime->store.vtable && ctx->runtime->store.vtable->append_tool_call) {
        ctx->runtime->store.vtable->append_tool_call(
            ctx->runtime->store.self, ctx->session_id, &call);
    }
    if (ctx->runtime->store.vtable && ctx->runtime->store.vtable->append_tool_result) {
        ctx->runtime->store.vtable->append_tool_result(
            ctx->runtime->store.self, ctx->session_id,
            ctx->cur_tool_id ? ctx->cur_tool_id : "", &tres);
    }

    char *tool_result_content = NULL;
    if (tres.ok) {
        cc_json_value_t *res_json = cc_json_create_object();
        cc_json_object_set(res_json, "success", cc_json_create_bool(1));
        cc_json_object_set(res_json, "result",
            cc_json_create_string(tres.text ? tres.text : ""));
        tool_result_content = cc_json_stringify_unformatted(res_json);
        cc_json_destroy(res_json);
    } else {
        cc_json_value_t *res_json = cc_json_create_object();
        cc_json_object_set(res_json, "success", cc_json_create_bool(0));
        cc_json_object_set(res_json, "error",
            cc_json_create_string(tres.error ? tres.error : "unknown error"));
        tool_result_content = cc_json_stringify_unformatted(res_json);
        cc_json_destroy(res_json);
    }

    cc_message_t *tool_msg = NULL;
    char *tcid = ctx->cur_tool_id ? strdup(ctx->cur_tool_id) : NULL;
    tid = generate_id();
    cc_message_create_text(tid, ctx->session_id, CC_ROLE_TOOL,
        tool_result_content ? tool_result_content : "{}", tcid, &tool_msg);
    free(tid);
    free(tcid);

    if (ctx->runtime->store.vtable && ctx->runtime->store.vtable->append_message) {
        ctx->runtime->store.vtable->append_message(ctx->runtime->store.self, tool_msg);
    }
    cc_message_destroy(tool_msg);
    cc_agent_runtime_append_artifact_observation(ctx->runtime, ctx->session_id, &tres);


    cc_json_value_t *payload_json = cc_json_create_object();
    if (payload_json) {
        cc_json_object_set(payload_json, "tool_name",
            cc_json_create_string(ctx->cur_tool_name));
        cc_json_object_set(payload_json, "tool_id",
            cc_json_create_string(ctx->cur_tool_id ? ctx->cur_tool_id : ""));
        cc_json_object_set(payload_json, "arguments",
            cc_json_create_string(arguments));
        cc_json_object_set(payload_json, "ok", cc_json_create_bool(tres.ok));
        if (tres.ok) {
            cc_json_object_set(payload_json, "result",
                cc_json_create_string(tres.text ? tres.text : ""));
        } else {
            cc_json_object_set(payload_json, "error",
                cc_json_create_string(tres.error ? tres.error : "unknown error"));
        }
        char *payload = cc_json_stringify_unformatted(payload_json);
        cc_json_destroy(payload_json);
        if (payload) {
            runtime_publish_observability(ctx->runtime, CC_OBS_EVENT_STREAM_TOOL_END,
                ctx->session_id, ctx->step, tres.ok ? "ok" : "error",
                tres.ok ? tres.text : tres.error, NULL, payload);
            free(payload);
        }
    }
    free(tool_result_content);
    free(tres.text);
    free(tres.error);
    free(tres.metadata);
    cc_media_artifact_list_cleanup(&tres.artifacts);


    free(ctx->cur_tool_name);
    ctx->cur_tool_name = NULL;
    free(ctx->cur_tool_id);
    ctx->cur_tool_id = NULL;
    cc_string_builder_clear(&ctx->args_builder);
}


/*
 * 通过 stream callback 把 runtime 本地错误作为 error chunk 交给实时 UI。
 *
 * 注意这里不直接写 session store；stream 错误、取消和超限都属于 partial response，
 * 默认不落库，避免下游把半截 assistant 回复当成稳定上下文。
 */
static void stream_loop_emit_error(stream_loop_ctx_t *ctx, const char *message)
{
    if (!ctx || !ctx->on_chunk) return;
    cc_stream_chunk_t error_chunk;
    memset(&error_chunk, 0, sizeof(error_chunk));
    error_chunk.type = CC_STREAM_CHUNK_ERROR;
    error_chunk.content = (char *)(message ? message : "stream error");
    ctx->on_chunk(&error_chunk, ctx->chunk_user_data);
}


/*
 * 转发 provider chunk，同时执行 stream 字节限制，超限后停止继续处理。
 *
 * 字节预算按实际发给应用 callback 的 content 计算；一旦超过 max_stream_bytes，
 * 设置 limit_exceeded/finished 状态并发送 error chunk，主循环随后统一返回
 * CC_ERR_LIMIT_EXCEEDED。这样 UI 能立刻看到错误，业务层也能拿到稳定错误码。
 */
static void stream_loop_forward_chunk(
    stream_loop_ctx_t *ctx,
    const cc_stream_chunk_t *chunk
)
{
    if (!ctx || !chunk || !ctx->on_chunk || ctx->limit_exceeded) return;

    if (ctx->max_stream_bytes > 0 && chunk->content) {
        size_t len = strlen(chunk->content);
        if (ctx->emitted_stream_bytes + len > ctx->max_stream_bytes) {
            ctx->limit_exceeded = 1;
            ctx->finished = 1;
            stream_loop_emit_error(ctx, "Stream exceeds max_stream_bytes");
            return;
        }
        ctx->emitted_stream_bytes += len;
    }

    ctx->on_chunk(chunk, ctx->chunk_user_data);
}


/*
 * provider stream callback 的核心状态机。
 *
 * 它同时负责三件事：把 chunk 透传给应用 callback、累积最终文本/思考/tool 参数、
 * 发布统一 observability 事件。工具调用在这里被识别并延迟到 tool_end 后执行。
 *
 * 设计上 callback 不拥有 chunk 内存，只在回调期间读取；需要跨 chunk 保存的内容都复制
 * 到 string builder 或 strdup 的字段里。这样 provider 可以使用栈上 chunk，也不会造成
 * 悬空指针问题。
 */
static void stream_loop_callback(const cc_stream_chunk_t *chunk, void *user_data)
{
    stream_loop_ctx_t *ctx = (stream_loop_ctx_t *)user_data;
    if (!ctx || !ctx->runtime) return;
    if (!chunk) return;
    if (cc_cancel_token_is_cancelled(ctx->cancel_token)) {
        ctx->cancelled = 1;
        ctx->finished = 1;
        return;
    }

    ctx->chunk_count++;
    stream_loop_forward_chunk(ctx, chunk);

    if (ctx->step >= 2 && getenv("CCLAW_DEBUG")) {
        static const char *cnames[] = {
            "TEXT","THINKING","TOOL_START","TOOL_DELTA","TOOL_END","FINISHED",
            "ARTIFACT","PROVIDER_WARNING","ERROR"
        };
        fprintf(stderr, "[CB] step=%d chunk=%d/%s\n",
            ctx->step, chunk->type,
            (chunk->type >= 0 && chunk->type < 9) ? cnames[chunk->type] : "?");
    }

    switch (chunk->type) {

    case CC_STREAM_CHUNK_THINKING:


        if (chunk->content && strlen(chunk->content) > 0) {
            cc_string_builder_append(&ctx->thinking_builder, chunk->content);
            runtime_publish_stream_content(ctx->runtime, CC_OBS_EVENT_STREAM_THINKING,
                ctx->session_id, ctx->step, "delta", chunk->content);
        }
        break;

    case CC_STREAM_CHUNK_TEXT:


        if (chunk->content && strlen(chunk->content) > 0) {
            cc_string_builder_append(&ctx->text_builder, chunk->content);
            runtime_publish_stream_content(ctx->runtime, CC_OBS_EVENT_STREAM_TEXT,
                ctx->session_id, ctx->step, "delta", chunk->content);
        }
        break;

    case CC_STREAM_CHUNK_TOOL_START:


        if (ctx->cur_tool_name != NULL) {
            execute_pending_tool(ctx);
        }
        ctx->has_tool_call = 1;
        ctx->cur_tool_name = chunk->tool_name ? strdup(chunk->tool_name) : NULL;
        ctx->cur_tool_id = chunk->tool_id ? strdup(chunk->tool_id) : NULL;
        cc_string_builder_clear(&ctx->args_builder);

        {
            cc_json_value_t *attrs = cc_json_create_object();
            if (attrs) {
                cc_json_object_set(attrs, "tool_name",
                    cc_json_create_string(ctx->cur_tool_name ? ctx->cur_tool_name : ""));
                cc_json_object_set(attrs, "tool_id",
                    cc_json_create_string(ctx->cur_tool_id ? ctx->cur_tool_id : ""));
                char *attrs_json = cc_json_stringify_unformatted(attrs);
                cc_json_destroy(attrs);
                if (attrs_json) {
                    runtime_publish_observability(ctx->runtime,
                        CC_OBS_EVENT_STREAM_TOOL_START, ctx->session_id,
                        ctx->step, "started", NULL, NULL, attrs_json);
                    free(attrs_json);
                }
            }
        }
        break;

    case CC_STREAM_CHUNK_TOOL_DELTA:


        if (chunk->content && strlen(chunk->content) > 0) {
            cc_string_builder_append(&ctx->args_builder, chunk->content);
            runtime_publish_stream_content(ctx->runtime, CC_OBS_EVENT_STREAM_TOOL_DELTA,
                ctx->session_id, ctx->step, "delta", chunk->content);
        }
        break;

    case CC_STREAM_CHUNK_TOOL_END:


        execute_pending_tool(ctx);
        break;

    case CC_STREAM_CHUNK_FINISHED:
        ctx->finished = 1;
        break;

    case CC_STREAM_CHUNK_ARTIFACT:
        if (chunk->content) {
            runtime_publish_stream_content(ctx->runtime, CC_OBS_EVENT_STREAM_ARTIFACT,
                ctx->session_id, ctx->step, "artifact", chunk->content);
        }
        break;

    case CC_STREAM_CHUNK_PROVIDER_WARNING:
        if (chunk->content) {
            runtime_publish_stream_content(ctx->runtime,
                CC_OBS_EVENT_STREAM_PROVIDER_WARNING, ctx->session_id,
                ctx->step, "warning", chunk->content);
        }
        break;

    case CC_STREAM_CHUNK_ERROR:
        if (chunk->content) {
            runtime_publish_stream_content(ctx->runtime, CC_OBS_EVENT_STREAM_ERROR,
                ctx->session_id, ctx->step, "error", chunk->content);
        }
        break;
    }
}


/*
 * 流式 agent 主循环。
 *
 * 每一步构造上下文 -> 调 provider stream -> 根据是否出现 tool call 决定继续循环或落库。
 * 取消、超限和 provider 错误不会写入不完整 assistant final。
 *
 * out_response 由调用方释放；内部 builder 在每一步复用，只有在确认没有 tool call 且
 * 输出限制通过后，才把最终 assistant 文本写入 session store 和 active memory。
 * 这条路径是“实时输出 callback”和“持久化最终响应”的分界点，面试时可以强调它避免
 * partial chunk 污染会话历史。
 */
static cc_result_t cc_agent_runtime_handle_message_stream_internal(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const char *user_input,
    const cc_agent_runtime_run_options_t *options,
    const cc_agent_runtime_stream_options_t *stream_options,
    char **out_response
)
{
    if (!runtime || !session_id || !user_input || !out_response) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "NULL argument");
    }
    *out_response = NULL;
    cc_result_t limit_rc = check_input_limit(runtime, user_input);
    if (limit_rc.code != CC_OK) return limit_rc;
    cc_result_t cancel_rc = check_run_cancelled(options, "Agent stream run cancelled before start");
    if (cancel_rc.code != CC_OK) return cancel_rc;
    if (cc_cancel_token_is_cancelled(stream_cancel_token(stream_options))) {
        return cc_result_error(CC_ERR_CANCELLED, "Agent stream run cancelled before start");
    }

    if (!runtime->llm.vtable || !runtime->llm.vtable->chat_stream) {
        if (stream_options && stream_options->on_chunk) {
            return cc_result_error(CC_ERR_UNSUPPORTED,
                "Provider does not support streaming callbacks");
        }
        return cc_agent_runtime_handle_stream_fallback(
            runtime, session_id, user_input, options, stream_options, out_response);
    }



    cc_message_t *user_msg = NULL;
    char *msg_id = generate_id();
    cc_result_t rc = cc_message_create_text(msg_id, session_id, CC_ROLE_USER, user_input, NULL, &user_msg);
    free(msg_id);

    if (rc.code != CC_OK) return rc;

    if (runtime->store.vtable && runtime->store.vtable->append_message) {
        rc = runtime->store.vtable->append_message(runtime->store.self, user_msg);
        if (rc.code != CC_OK) {
            cc_message_destroy(user_msg);
            return rc;
        }
    }
    cc_message_destroy(user_msg);



    stream_loop_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.runtime = runtime;
    ctx.session_id = session_id;
    ctx.cancel_token = run_cancel_token(options);
    if (!ctx.cancel_token) ctx.cancel_token = stream_cancel_token(stream_options);
    if (stream_options) {
        ctx.on_chunk = stream_options->on_chunk;
        ctx.chunk_user_data = stream_options->user_data;
    }
    ctx.max_stream_bytes = runtime->config.limits.max_stream_bytes;
    cc_string_builder_init(&ctx.text_builder);
    cc_string_builder_init(&ctx.thinking_builder);
    cc_string_builder_init(&ctx.args_builder);

    for (int step = 0; step < runtime_effective_max_steps(runtime); step++) {
        rc = check_run_cancelled(options, "Agent stream run cancelled");
        if (rc.code == CC_OK && cc_cancel_token_is_cancelled(ctx.cancel_token)) {
            rc = cc_result_error(CC_ERR_CANCELLED, "Agent stream run cancelled");
        }
        if (rc.code != CC_OK) {
            free(ctx.cur_tool_name);
            free(ctx.cur_tool_id);
            cc_string_builder_deinit(&ctx.text_builder);
            cc_string_builder_deinit(&ctx.thinking_builder);
            cc_string_builder_deinit(&ctx.args_builder);
            return rc;
        }
        ctx.step = step + 1;
        ctx.finished = 0;
        ctx.has_tool_call = 0;
        ctx.chunk_count = 0;
        cc_string_builder_clear(&ctx.text_builder);
        cc_string_builder_clear(&ctx.thinking_builder);
        cc_string_builder_clear(&ctx.args_builder);
        free(ctx.cur_tool_name);
        ctx.cur_tool_name = NULL;
        free(ctx.cur_tool_id);
        ctx.cur_tool_id = NULL;



        cc_message_t *messages = NULL;
        size_t message_count = 0;
        cc_context_builder_build_messages(runtime, session_id,
            runtime->config.system_prompt, &messages, &message_count);



        char *tools_json = NULL;
        cc_tool_registry_build_schema_json(runtime->tool_registry, &tools_json);



        runtime_publish_observability(runtime, CC_OBS_EVENT_LLM_REQUEST_START,
            session_id, step, "started", NULL, NULL, NULL);



        cc_llm_chat_request_t req;
        memset(&req, 0, sizeof(req));
        cc_media_limits_t media_limits;
        runtime_media_limits_from_config(&runtime->config.multimodal, &media_limits);
        req.model = runtime->config.model;
        req.messages = messages;
        req.message_count = message_count;
        req.media_limits = &media_limits;
        req.max_tokens = runtime->config.max_tokens;
        req.temperature = runtime->config.temperature;
        req.stream = 1;
        req.thinking_mode = cc_agent_runtime_get_thinking_mode(runtime);
        req.cancel_token = ctx.cancel_token;
        req.timeout_ms = runtime->config.limits.provider_timeout_ms;

        if (tools_json && strlen(tools_json) > 2) {
            req.tools_json = tools_json;
        }

        if (getenv("CCLAW_DEBUG")) {
        fprintf(stderr, "[DEBUG] step=%d calling chat_stream, message_count=%zu tools_json_len=%zu\n",
            step,
            message_count,
            tools_json ? strlen(tools_json) : 0);
        }

        cc_result_t rc = runtime->llm.vtable->chat_stream(
            runtime->llm.self, &req, stream_loop_callback, &ctx);

        for (size_t mi = 0; mi < message_count; mi++) cc_message_cleanup(&messages[mi]);
        free(messages);
        free(tools_json);

        if (getenv("CCLAW_DEBUG")) {
        fprintf(stderr, "[DEBUG] step=%d chunks=%d has_tool_call=%d finished=%d "
            "text_len=%zu rc=%d\n",
            step, ctx.chunk_count, ctx.has_tool_call, ctx.finished,
            strlen(cc_string_builder_cstr(&ctx.text_builder)), rc.code);
        }

        runtime_publish_observability(runtime, CC_OBS_EVENT_LLM_RESPONSE_FINISH,
            session_id, step, rc.code == CC_OK ? "ok" : "error",
            rc.message, rc.code == CC_OK ? NULL : &rc, NULL);

        if (ctx.limit_exceeded) {
            free(ctx.cur_tool_name);
            free(ctx.cur_tool_id);
            cc_string_builder_deinit(&ctx.text_builder);
            cc_string_builder_deinit(&ctx.thinking_builder);
            cc_string_builder_deinit(&ctx.args_builder);
            cc_result_free(&rc);
            return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Stream exceeds max_stream_bytes");
        }

        if (rc.code != CC_OK) {
            free(ctx.cur_tool_name);
            free(ctx.cur_tool_id);
            cc_string_builder_deinit(&ctx.text_builder);
            cc_string_builder_deinit(&ctx.thinking_builder);
            cc_string_builder_deinit(&ctx.args_builder);
            *out_response = strdup("Streaming error");
            return rc;
        }
        if (ctx.cancelled || cc_cancel_token_is_cancelled(run_cancel_token(options))) {
            free(ctx.cur_tool_name);
            free(ctx.cur_tool_id);
            cc_string_builder_deinit(&ctx.text_builder);
            cc_string_builder_deinit(&ctx.thinking_builder);
            cc_string_builder_deinit(&ctx.args_builder);
            return cc_result_error(CC_ERR_CANCELLED, "Agent stream run cancelled");
        }



        if (!ctx.has_tool_call) {
            const char *final_text = cc_string_builder_cstr(&ctx.text_builder);
            const char *thinking = cc_string_builder_cstr(&ctx.thinking_builder);

            rc = check_output_limit(runtime, final_text, "Stream response");
            if (rc.code != CC_OK) {
                free(ctx.cur_tool_name);
                free(ctx.cur_tool_id);
                cc_string_builder_deinit(&ctx.text_builder);
                cc_string_builder_deinit(&ctx.thinking_builder);
                cc_string_builder_deinit(&ctx.args_builder);
                return rc;
            }

            cc_agent_runtime_store_assistant_text(
                runtime, session_id, final_text, thinking);
            active_memory_after_run(runtime, session_id, user_input, final_text);

            *out_response = final_text ? strdup(final_text) : strdup("");
            free(ctx.cur_tool_name);
            free(ctx.cur_tool_id);
            cc_string_builder_deinit(&ctx.text_builder);
            cc_string_builder_deinit(&ctx.thinking_builder);
            cc_string_builder_deinit(&ctx.args_builder);

            runtime_publish_observability(runtime, CC_OBS_EVENT_STREAM_FINISHED,
                session_id, step, "finished", NULL, NULL, NULL);
            runtime_publish_observability(runtime, CC_OBS_EVENT_RUN_FINISHED,
                session_id, step, "ok", NULL, NULL, NULL);
            return cc_result_ok();
        }



    }



    free(ctx.cur_tool_name);
    free(ctx.cur_tool_id);
    cc_string_builder_deinit(&ctx.text_builder);
    cc_string_builder_deinit(&ctx.thinking_builder);
    cc_string_builder_deinit(&ctx.args_builder);

    *out_response = strdup("Agent stopped: max steps reached.");
    runtime_publish_observability(runtime, CC_OBS_EVENT_STREAM_FINISHED,
        session_id, runtime_effective_max_steps(runtime), "finished", NULL, NULL, NULL);
    runtime_publish_observability(runtime, CC_OBS_EVENT_RUN_FINISHED,
        session_id, runtime_effective_max_steps(runtime), "max_steps_reached",
        "max_steps_reached", NULL, "{\"reason\":\"max_steps_reached\"}");
    return cc_result_ok();
}

/* 旧 stream API 的 options 版本：保留 out_response，但不直接暴露 chunk callback。 */
cc_result_t cc_agent_runtime_handle_message_stream_with_options(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const char *user_input,
    const cc_agent_runtime_run_options_t *options,
    char **out_response
)
{
    return cc_agent_runtime_handle_message_stream_internal(
        runtime, session_id, user_input, options, NULL, out_response);
}

/* 正式 stream callback API：实时输出走 callback，event bus 只作为观测通道。 */
cc_result_t cc_agent_runtime_handle_message_stream_cb(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const char *user_input,
    const cc_agent_runtime_stream_options_t *options,
    char **out_response
)
{
    cc_agent_runtime_run_options_t run_options;
    memset(&run_options, 0, sizeof(run_options));
    if (options) run_options.cancel_token = options->cancel_token;
    return cc_agent_runtime_handle_message_stream_internal(
        runtime, session_id, user_input,
        options ? &run_options : NULL,
        options,
        out_response);
}

/* 兼容入口：使用默认 run options 执行流式处理。 */
cc_result_t cc_agent_runtime_handle_message_stream(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const char *user_input,
    char **out_response
)
{
    return cc_agent_runtime_handle_message_stream_with_options(
        runtime, session_id, user_input, NULL, out_response);
}


/* 销毁 runtime 自有配置内存；注入的端口对象由创建者或 builder 负责销毁。 */
void cc_agent_runtime_destroy(cc_agent_runtime_t *runtime)
{
    if (!runtime) return;
    cc_mutex_lock(runtime->mutex);
    free(runtime->config.system_prompt);
    free(runtime->config.workspace_dir);
    free(runtime->config.model);
    free(runtime->config.active_memory_category);
    runtime_string_list_cleanup(&runtime->config.multimodal.limits.allowed_mime_prefixes);
    cc_mutex_unlock(runtime->mutex);
    cc_mutex_destroy(runtime->mutex);
    free(runtime);
}


/* 运行时切换 thinking mode，最终会透传到 provider request。 */
void cc_agent_runtime_set_thinking_mode(cc_agent_runtime_t *runtime, int enabled)
{
    if (!runtime) return;
    cc_mutex_lock(runtime->mutex);
    runtime->thinking_mode = enabled ? 1 : 0;
    cc_mutex_unlock(runtime->mutex);
}


/* 注入人工审批回调；高风险工具 require_approval 时会调用它。 */
void cc_agent_runtime_set_tool_approval(
    cc_agent_runtime_t *runtime,
    cc_tool_approval_fn approve_tool_call,
    void *user_data
)
{
    if (!runtime) return;
    cc_mutex_lock(runtime->mutex);
    runtime->services.approve_tool_call = approve_tool_call;
    runtime->services.approval_user_data = user_data;
    cc_mutex_unlock(runtime->mutex);
}


/* 查询当前 thinking mode，provider request 构造时使用这个值。 */
int cc_agent_runtime_get_thinking_mode(cc_agent_runtime_t *runtime)
{
    if (!runtime) return 0;
    cc_mutex_lock(runtime->mutex);
    int enabled = runtime->thinking_mode;
    cc_mutex_unlock(runtime->mutex);
    return enabled;
}

/*
 * 暴露 runtime 内部 event bus 的借用指针。
 *
 * 调用方不能销毁返回值；它通常用于测试、调试 UI 或上层日志模块订阅统一
 * observability 事件。业务路径发布事件仍应走 cc_observability_publish 封装。
 */
cc_event_bus_t *cc_agent_runtime_event_bus(cc_agent_runtime_t *runtime)
{
    return runtime ? runtime->event_bus : NULL;
}

/*
 * 暴露工具注册表的借用指针。
 *
 * 下游应用通过它注册工具，runtime 销毁时统一释放 registry；多线程注册与执行的并发
 * 语义由 tool registry 自身的锁保护，调用方不应缓存内部数组指针。
 */
cc_tool_registry_t *cc_agent_runtime_tool_registry(cc_agent_runtime_t *runtime)
{
    return runtime ? runtime->tool_registry : NULL;
}

/*
 * 暴露 session store 端口的借用视图。
 *
 * 返回的是 runtime 内嵌端口结构地址，便于测试或上层做只读查询；端口 self/vtable 的
 * 生命周期仍由 runtime/builder 注入约定管理，调用方不能 free 这个指针。
 */
cc_session_store_t *cc_agent_runtime_session_store(cc_agent_runtime_t *runtime)
{
    return runtime ? &runtime->store : NULL;
}


/* 判断当前 provider 是否实现原生 stream vtable。 */
int cc_agent_runtime_supports_stream(cc_agent_runtime_t *runtime)
{
    return runtime && runtime->llm.vtable && runtime->llm.vtable->chat_stream;
}


/*
 * 创建会话并记录 workspace 边界。
 *
 * 如果 store 没有实现 create_session，runtime 选择 no-op 成功，保持最小嵌入式配置可用；
 * 一旦实现了 session store，workspace_dir 会优先使用调用方传入值，否则回退到 runtime
 * config。文件工具后续会依赖这个 workspace 做路径归一化和越界检查。
 */
cc_result_t cc_agent_runtime_create_session(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const char *workspace_dir
)
{
    if (!runtime || !session_id) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null session create argument");
    }
    if (!runtime->store.vtable || !runtime->store.vtable->create_session) {
        return cc_result_ok();
    }
    return runtime->store.vtable->create_session(
        runtime->store.self,
        session_id,
        workspace_dir ? workspace_dir : runtime->config.workspace_dir
    );
}




/*
 * 同步 agent 主循环。
 *
 * 同步路径和 stream 路径共享 context builder、tool executor、limits 和 active memory。
 * 区别是 provider 一次性返回 cc_llm_response_t，而不是逐 chunk 回调。
 */
cc_result_t cc_agent_runtime_handle_message_with_options(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const char *user_input,
    const cc_agent_runtime_run_options_t *options,
    char **out_response
)
{
    if (!runtime || !session_id || !user_input || !out_response) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "NULL argument");
    }
    *out_response = NULL;
    cc_result_t limit_rc = check_input_limit(runtime, user_input);
    if (limit_rc.code != CC_OK) return limit_rc;
    cc_result_t cancel_rc = check_run_cancelled(options, "Agent run cancelled before start");
    if (cancel_rc.code != CC_OK) return cancel_rc;

    cc_message_t *user_msg = NULL;
    char *msg_id = generate_id();
    cc_result_t rc = cc_message_create_text(msg_id, session_id, CC_ROLE_USER, user_input, NULL, &user_msg);
    free(msg_id);

    if (rc.code != CC_OK) return rc;


    if (runtime->store.vtable && runtime->store.vtable->append_message) {
        rc = runtime->store.vtable->append_message(runtime->store.self, user_msg);
        if (rc.code != CC_OK) {
            cc_message_destroy(user_msg);
            return rc;
        }
    }
    cc_message_destroy(user_msg);



    for (int step = 0; step < runtime_effective_max_steps(runtime); ++step) {
        rc = check_run_cancelled(options, "Agent run cancelled");
        if (rc.code != CC_OK) return rc;
        cc_message_t *messages = NULL;
        size_t message_count = 0;
        char *tools_json = NULL;



        rc = cc_context_builder_build_messages(
            runtime, session_id,
            runtime->config.system_prompt,
            &messages,
            &message_count
        );
        if (rc.code != CC_OK) return rc;



        rc = cc_tool_registry_build_schema_json(
            runtime->tool_registry,
            &tools_json
        );
        if (rc.code != CC_OK) {
            for (size_t mi = 0; mi < message_count; mi++) cc_message_cleanup(&messages[mi]);
            free(messages);
            return rc;
        }



        cc_llm_chat_request_t request;
        memset(&request, 0, sizeof(request));
        cc_media_limits_t media_limits;
        runtime_media_limits_from_config(&runtime->config.multimodal, &media_limits);
        request.messages = messages;
        request.message_count = message_count;
        request.media_limits = &media_limits;
        request.tools_json = tools_json;
        request.model = runtime->config.model;
        request.stream = 0;
        request.max_tokens = runtime->config.max_tokens;
        request.temperature = runtime->config.temperature;
        request.thinking_mode = cc_agent_runtime_get_thinking_mode(runtime);
        request.cancel_token = run_cancel_token(options);
        request.timeout_ms = runtime->config.limits.provider_timeout_ms;


        runtime_publish_observability(runtime, CC_OBS_EVENT_LLM_REQUEST_START,
            session_id, step, "started", NULL, NULL, NULL);



        cc_llm_response_t response;
        memset(&response, 0, sizeof(response));
        rc = runtime->llm.vtable->chat(
            runtime->llm.self,
            &request,
            &response
        );

        for (size_t mi = 0; mi < message_count; mi++) cc_message_cleanup(&messages[mi]);
        free(messages);
        free(tools_json);


        runtime_publish_observability(runtime, CC_OBS_EVENT_LLM_RESPONSE_FINISH,
            session_id, step, rc.code == CC_OK ? "ok" : "error",
            rc.message, rc.code == CC_OK ? NULL : &rc, NULL);


        if (rc.code != CC_OK) {
            cc_llm_response_free(&response);
            return rc;
        }
        rc = check_run_cancelled(options, "Agent run cancelled after LLM response");
        if (rc.code != CC_OK) {
            cc_llm_response_free(&response);
            return rc;
        }



        if (response.tool_calls.count > 0) {
            for (size_t ti = 0; ti < response.tool_calls.count; ti++) {
                rc = cc_agent_runtime_execute_tool_step(
                    runtime, session_id, &response.tool_calls.items[ti], response.reasoning_content,
                    run_cancel_token(options));
                if (rc.code != CC_OK) break;
            }
            cc_llm_response_free(&response);
            if (rc.code != CC_OK) return rc;
            continue;
        }



        if (response.has_text) {
            rc = check_output_limit(runtime, response.text, "Response");
            if (rc.code != CC_OK) {
                cc_llm_response_free(&response);
                return rc;
            }
            rc = cc_agent_runtime_store_assistant_text(
                runtime, session_id, response.text, response.reasoning_content);
            if (rc.code != CC_OK) {
                cc_llm_response_free(&response);
                return rc;
            }
            active_memory_after_run(runtime, session_id, user_input, response.text);
            *out_response = strdup(response.text ? response.text : "");
            cc_llm_response_free(&response);
            runtime_publish_observability(runtime, CC_OBS_EVENT_RUN_FINISHED,
                session_id, step, "ok", NULL, NULL, NULL);
            return cc_result_ok();
        }



        cc_llm_response_free(&response);
        break;
    }



    *out_response = strdup("Agent stopped: max steps reached.");
    runtime_publish_observability(runtime, CC_OBS_EVENT_RUN_FINISHED,
        session_id, runtime_effective_max_steps(runtime), "max_steps_reached",
        "max_steps_reached", NULL, "{\"reason\":\"max_steps_reached\"}");
    return cc_result_ok();
}

/* 默认同步入口：不带取消 token，返回完整 assistant 文本。 */
cc_result_t cc_agent_runtime_handle_message(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const char *user_input,
    char **out_response
)
{
    return cc_agent_runtime_handle_message_with_options(
        runtime, session_id, user_input, NULL, out_response);
}
