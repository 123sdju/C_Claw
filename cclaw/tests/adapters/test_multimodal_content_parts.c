#include "cc/app/cc_agent_runtime.h"
#include "cc/app/cc_context_builder.h"
#include "cc/core/cc_media.h"
#include "cc/core/cc_message.h"
#include "cc/core/cc_tool_call.h"
#include "cc/ports/cc_session_store.h"
#include "cc/ports/cc_tool_registry.h"

#include <stdlib.h>
#include <string.h>

extern cc_result_t cc_memory_session_store_create(cc_session_store_t *out_store);

static cc_result_t fake_chat(void *self, const cc_llm_chat_request_t *request, cc_llm_response_t *out)
{
    (void)self;
    (void)request;
    memset(out, 0, sizeof(*out));
    out->has_text = 1;
    out->finished = 1;
    out->text = strdup("ok");
    return cc_result_ok();
}

static cc_llm_provider_vtable_t fake_llm_vtable = {
    fake_chat,
    NULL,
    NULL
};

int main(void)
{
    cc_media_artifact_t image = {
        .id = "img_1",
        .kind = "image",
        .mime = "image/png",
        .path = "/tmp/image.png",
        .data_base64 = "aW1hZ2U=",
        .bytes = 5,
        .width = 2,
        .height = 3,
        .created_at = "2026-05-25T00:00:00"
    };
    char *part_json = NULL;
    cc_result_t rc = cc_media_artifact_to_content_part_json(&image, &part_json);
    if (rc.code != CC_OK || !part_json || !strstr(part_json, "\"type\":\"image\"")) return 2;
    cc_result_free(&rc);

    char *parts_json = NULL;
    rc = cc_content_parts_build_text_image_audio(
        "observe image",
        "[{\"id\":\"img_1\",\"kind\":\"image\",\"mime\":\"image/png\",\"data_base64\":\"aW1hZ2U=\",\"width\":2,\"height\":3}]",
        &parts_json);
    if (rc.code != CC_OK || !parts_json || !strstr(parts_json, "\"data_base64\":\"aW1hZ2U=\"")) return 3;
    cc_result_free(&rc);

    cc_message_t *msg = NULL;
    cc_message_create("m1", "ses_mm", CC_ROLE_USER, "observe image", NULL, &msg);
    cc_message_set_content_parts_json(msg, parts_json);
    cc_message_t copied;
    if (cc_message_copy(msg, &copied).code != CC_OK ||
        !copied.content_parts_json ||
        strcmp(copied.content_parts_json, parts_json) != 0) {
        return 4;
    }
    cc_message_cleanup(&copied);

    cc_tool_result_t tr = {0};
    cc_tool_result_set_artifacts_json(&tr, "[{\"kind\":\"image\"}]");
    if (!tr.artifacts_json || !strstr(tr.artifacts_json, "image")) return 5;
    free(tr.artifacts_json);

    cc_tool_registry_t *registry = NULL;
    cc_session_store_t store = {0};
    cc_agent_runtime_t *runtime = NULL;
    if (cc_tool_registry_create(&registry).code != CC_OK) return 1;
    cc_tool_registry_freeze(registry);
    if (cc_memory_session_store_create(&store).code != CC_OK) return 1;
    store.vtable->create_session(store.self, "ses_mm", ".");
    store.vtable->append_message(store.self, msg);

    cc_llm_provider_t llm = { NULL, &fake_llm_vtable };
    cc_agent_runtime_deps_t deps = {0};
    deps.llm = llm;
    deps.tool_registry = registry;
    deps.store = store;

    cc_agent_runtime_options_t options = {0};
    options.config.max_steps = 1;
    options.config.system_prompt = "system";
    options.config.workspace_dir = ".";
    options.config.model = "fake";
    if (cc_agent_runtime_create(&deps, &options, &runtime).code != CC_OK) return 1;

    char *messages_json = NULL;
    rc = cc_context_builder_build_messages(runtime, "ses_mm", "system", &messages_json);
    if (rc.code != CC_OK || !messages_json ||
        !strstr(messages_json, "\"content\"") ||
        !strstr(messages_json, "image") ||
        !strstr(messages_json, "data_base64")) {
        return 6;
    }

    free(messages_json);
    cc_result_free(&rc);
    cc_agent_runtime_destroy(runtime);
    store.vtable->destroy(store.self);
    cc_tool_registry_destroy(registry);
    cc_message_destroy(msg);
    free(part_json);
    free(parts_json);
    return 0;
}
