#include "cc/app/cc_agent_runtime.h"
#include "cc/app/cc_context_builder.h"
#include "cc/core/cc_message.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

extern cc_result_t cc_memory_session_store_create(cc_session_store_t *out_store);

typedef struct {
    int sync_seen;
    int sync_max_tokens;
    double sync_temperature;
    int stream_seen;
    int stream_max_tokens;
    double stream_temperature;
    int summary_seen;
    int summary_max_tokens;
    double summary_temperature;
} fake_llm_state_t;

static int double_eq(double a, double b)
{
    double d = a - b;
    if (d < 0.0) d = -d;
    return d < 0.000001;
}

static cc_result_t fake_chat(void *self, const cc_llm_chat_request_t *request, cc_llm_response_t *out)
{
    fake_llm_state_t *state = (fake_llm_state_t *)self;
    memset(out, 0, sizeof(*out));

    if (request && request->messages_json &&
        strstr(request->messages_json, "Summarize the following conversation")) {
        state->summary_seen = 1;
        state->summary_max_tokens = request->max_tokens;
        state->summary_temperature = request->temperature;
        out->text = strdup("summary-ok");
    } else {
        state->sync_seen = 1;
        state->sync_max_tokens = request ? request->max_tokens : -1;
        state->sync_temperature = request ? request->temperature : -1.0;
        out->text = strdup("sync-ok");
    }

    out->has_text = 1;
    out->finished = 1;
    return out->text ? cc_result_ok() :
        cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate fake response");
}

static cc_result_t fake_chat_stream(
    void *self,
    const cc_llm_chat_request_t *request,
    cc_llm_stream_callback_fn on_chunk,
    void *user_data
)
{
    fake_llm_state_t *state = (fake_llm_state_t *)self;
    state->stream_seen = 1;
    state->stream_max_tokens = request ? request->max_tokens : -1;
    state->stream_temperature = request ? request->temperature : -1.0;

    cc_stream_chunk_t text = { CC_STREAM_CHUNK_TEXT, "stream-ok", NULL, NULL };
    cc_stream_chunk_t done = { CC_STREAM_CHUNK_FINISHED, NULL, NULL, NULL };
    on_chunk(&text, user_data);
    on_chunk(&done, user_data);
    return cc_result_ok();
}

static cc_llm_provider_vtable_t fake_vtable = {
    fake_chat,
    fake_chat_stream,
    NULL
};

static int create_runtime(
    fake_llm_state_t *state,
    cc_agent_runtime_config_t config,
    cc_tool_registry_t **out_registry,
    cc_session_store_t *out_store,
    cc_agent_runtime_t **out_runtime
)
{
    if (cc_tool_registry_create(out_registry).code != CC_OK) return 1;
    if (cc_tool_registry_freeze(*out_registry).code != CC_OK) return 1;
    if (cc_memory_session_store_create(out_store).code != CC_OK) return 1;

    cc_agent_runtime_deps_t deps = {0};
    deps.llm.self = state;
    deps.llm.vtable = &fake_vtable;
    deps.tool_registry = *out_registry;
    deps.store = *out_store;

    cc_agent_runtime_options_t options = {0};
    options.config = config;
    return cc_agent_runtime_create(&deps, &options, out_runtime).code == CC_OK ? 0 : 1;
}

static void destroy_runtime(
    cc_tool_registry_t *registry,
    cc_session_store_t *store,
    cc_agent_runtime_t *runtime
)
{
    cc_agent_runtime_destroy(runtime);
    if (store->vtable && store->vtable->destroy) store->vtable->destroy(store->self);
    cc_tool_registry_destroy(registry);
}

static int append_user_message(cc_session_store_t *store, const char *session_id, const char *id, const char *content)
{
    cc_message_t *msg = NULL;
    cc_result_t rc = cc_message_create(id, session_id, CC_ROLE_USER, content, NULL, &msg);
    if (rc.code != CC_OK) return 1;
    rc = store->vtable->append_message(store->self, msg);
    cc_message_destroy(msg);
    int failed = rc.code != CC_OK;
    cc_result_free(&rc);
    return failed;
}

int main(void)
{
    int failed = 0;
    fake_llm_state_t state = {0};
    cc_tool_registry_t *registry = NULL;
    cc_session_store_t store = {0};
    cc_agent_runtime_t *runtime = NULL;

    cc_agent_runtime_config_t request_config = {
        .max_steps = 1,
        .system_prompt = "system",
        .workspace_dir = ".",
        .model = "fake",
        .max_tokens = 1234,
        .temperature = 0.25
    };
    if (create_runtime(&state, request_config, &registry, &store, &runtime)) return 1;

    store.vtable->create_session(store.self, "sync", ".");
    char *response = NULL;
    cc_result_t rc = cc_agent_runtime_handle_message(runtime, "sync", "hello", &response);
    if (rc.code != CC_OK || !response || strcmp(response, "sync-ok") != 0) failed = 1;
    if (!state.sync_seen || state.sync_max_tokens != 1234 ||
        !double_eq(state.sync_temperature, 0.25)) failed = 1;
    cc_result_free(&rc);
    free(response);

    store.vtable->create_session(store.self, "stream", ".");
    response = NULL;
    rc = cc_agent_runtime_handle_message_stream(runtime, "stream", "hello", &response);
    if (rc.code != CC_OK || !response || strcmp(response, "stream-ok") != 0) failed = 1;
    if (!state.stream_seen || state.stream_max_tokens != 1234 ||
        !double_eq(state.stream_temperature, 0.25)) failed = 1;
    cc_result_free(&rc);
    free(response);
    destroy_runtime(registry, &store, runtime);

    memset(&state, 0, sizeof(state));
    registry = NULL;
    memset(&store, 0, sizeof(store));
    runtime = NULL;
    cc_agent_runtime_config_t summary_config = {
        .max_steps = 1,
        .system_prompt = "system",
        .workspace_dir = ".",
        .model = "fake",
        .context_window_tokens = 80,
        .context_compress_threshold = 0.8,
        .context_keep_recent = 1,
        .summary_max_tokens = 321,
        .summary_temperature = 0.15
    };
    if (create_runtime(&state, summary_config, &registry, &store, &runtime)) return 1;

    store.vtable->create_session(store.self, "summary", ".");
    const char *long_text =
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    for (int i = 0; i < 8; i++) {
        char id[16];
        snprintf(id, sizeof(id), "m%d", i);
        if (append_user_message(&store, "summary", id, long_text)) failed = 1;
    }

    char *messages_json = NULL;
    rc = cc_context_builder_build_messages(runtime, "summary", "system", &messages_json);
    if (rc.code != CC_OK || !messages_json) failed = 1;
    if (!state.summary_seen || state.summary_max_tokens != 321 ||
        !double_eq(state.summary_temperature, 0.15)) failed = 1;
    if (messages_json && !strstr(messages_json, "summary-ok")) failed = 1;
    free(messages_json);
    cc_result_free(&rc);
    destroy_runtime(registry, &store, runtime);

    return failed ? 1 : 0;
}
