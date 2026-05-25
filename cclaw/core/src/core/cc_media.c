#include "cc/core/cc_media.h"

#include "cc/util/cc_json.h"
#include "cc/util/cc_memory.h"
#include "cc/util/cc_string_builder.h"
#include <stdlib.h>
#include <string.h>

static const char *safe_kind(const char *kind)
{
    if (!kind || !*kind) return "file";
    if (strcmp(kind, "image") == 0 ||
        strcmp(kind, "audio") == 0 ||
        strcmp(kind, "video") == 0 ||
        strcmp(kind, "file") == 0) {
        return kind;
    }
    return "file";
}

static void json_set_string_if_present(cc_json_value_t *obj, const char *key, const char *value)
{
    if (value && *value) {
        cc_json_object_set(obj, key, cc_json_create_string(value));
    }
}

static void json_set_number_if_positive(cc_json_value_t *obj, const char *key, double value)
{
    if (value > 0) {
        cc_json_object_set(obj, key, cc_json_create_number(value));
    }
}

static cc_json_value_t *artifact_object_to_content_part(const cc_json_value_t *artifact)
{
    if (!artifact || !cc_json_is_object(artifact)) return NULL;

    const char *kind = safe_kind(cc_json_string_value(cc_json_object_get(artifact, "kind")));
    cc_json_value_t *part = cc_json_create_object();
    if (!part) return NULL;

    cc_json_object_set(part, "type", cc_json_create_string(kind));
    json_set_string_if_present(part, "id", cc_json_string_value(cc_json_object_get(artifact, "id")));
    json_set_string_if_present(part, "mime", cc_json_string_value(cc_json_object_get(artifact, "mime")));
    json_set_string_if_present(part, "path", cc_json_string_value(cc_json_object_get(artifact, "path")));
    json_set_string_if_present(part, "data_base64", cc_json_string_value(cc_json_object_get(artifact, "data_base64")));
    json_set_string_if_present(part, "created_at", cc_json_string_value(cc_json_object_get(artifact, "created_at")));

    if (cc_json_object_get(artifact, "bytes")) {
        cc_json_object_set(part, "bytes",
            cc_json_create_number(cc_json_number_value(cc_json_object_get(artifact, "bytes"))));
    }
    if (cc_json_object_get(artifact, "width")) {
        cc_json_object_set(part, "width",
            cc_json_create_number(cc_json_number_value(cc_json_object_get(artifact, "width"))));
    }
    if (cc_json_object_get(artifact, "height")) {
        cc_json_object_set(part, "height",
            cc_json_create_number(cc_json_number_value(cc_json_object_get(artifact, "height"))));
    }
    if (cc_json_object_get(artifact, "duration_ms")) {
        cc_json_object_set(part, "duration_ms",
            cc_json_create_number(cc_json_number_value(cc_json_object_get(artifact, "duration_ms"))));
    }

    return part;
}

static cc_result_t append_artifact_summary(cc_string_builder_t *sb, const cc_json_value_t *artifact)
{
    if (!artifact || !cc_json_is_object(artifact)) return cc_result_ok();

    const char *id = cc_json_string_value(cc_json_object_get(artifact, "id"));
    const char *kind = safe_kind(cc_json_string_value(cc_json_object_get(artifact, "kind")));
    const char *mime = cc_json_string_value(cc_json_object_get(artifact, "mime"));
    const char *path = cc_json_string_value(cc_json_object_get(artifact, "path"));
    double bytes = cc_json_number_value(cc_json_object_get(artifact, "bytes"));
    double width = cc_json_number_value(cc_json_object_get(artifact, "width"));
    double height = cc_json_number_value(cc_json_object_get(artifact, "height"));
    double duration_ms = cc_json_number_value(cc_json_object_get(artifact, "duration_ms"));

    cc_result_t rc = cc_string_builder_appendf(sb, "- %s", kind);
    if (rc.code != CC_OK) return rc;
    if (id && *id) {
        rc = cc_string_builder_appendf(sb, " id=%s", id);
        if (rc.code != CC_OK) return rc;
    }
    if (mime && *mime) {
        rc = cc_string_builder_appendf(sb, " mime=%s", mime);
        if (rc.code != CC_OK) return rc;
    }
    if (path && *path) {
        rc = cc_string_builder_appendf(sb, " path=%s", path);
        if (rc.code != CC_OK) return rc;
    }
    if (bytes > 0) {
        rc = cc_string_builder_appendf(sb, " bytes=%.0f", bytes);
        if (rc.code != CC_OK) return rc;
    }
    if (width > 0 && height > 0) {
        rc = cc_string_builder_appendf(sb, " size=%.0fx%.0f", width, height);
        if (rc.code != CC_OK) return rc;
    }
    if (duration_ms > 0) {
        rc = cc_string_builder_appendf(sb, " duration_ms=%.0f", duration_ms);
        if (rc.code != CC_OK) return rc;
    }
    return cc_string_builder_append(sb, "\n");
}

cc_result_t cc_media_artifact_to_content_part_json(
    const cc_media_artifact_t *artifact,
    char **out_content_part_json
)
{
    if (!artifact || !out_content_part_json) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null media artifact argument");
    }
    *out_content_part_json = NULL;

    cc_json_value_t *part = cc_json_create_object();
    if (!part) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate content part");

    cc_json_object_set(part, "type", cc_json_create_string(safe_kind(artifact->kind)));
    json_set_string_if_present(part, "id", artifact->id);
    json_set_string_if_present(part, "mime", artifact->mime);
    json_set_string_if_present(part, "path", artifact->path);
    json_set_string_if_present(part, "data_base64", artifact->data_base64);
    json_set_string_if_present(part, "created_at", artifact->created_at);
    json_set_number_if_positive(part, "bytes", (double)artifact->bytes);
    json_set_number_if_positive(part, "width", (double)artifact->width);
    json_set_number_if_positive(part, "height", (double)artifact->height);
    json_set_number_if_positive(part, "duration_ms", (double)artifact->duration_ms);

    char *json = cc_json_stringify_unformatted(part);
    cc_json_destroy(part);
    if (!json) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to serialize content part");

    *out_content_part_json = json;
    return cc_result_ok();
}

cc_result_t cc_content_parts_build_text_image_audio(
    const char *text,
    const char *artifacts_json,
    char **out_content_parts_json
)
{
    if (!out_content_parts_json) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null content parts output");
    }
    *out_content_parts_json = NULL;

    cc_json_value_t *arr = cc_json_create_array();
    if (!arr) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate content parts");

    cc_json_value_t *text_part = cc_json_create_object();
    if (!text_part) {
        cc_json_destroy(arr);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate text part");
    }
    cc_json_object_set(text_part, "type", cc_json_create_string("text"));
    cc_json_object_set(text_part, "text", cc_json_create_string(text ? text : ""));
    cc_json_array_append(arr, text_part);

    if (artifacts_json && *artifacts_json) {
        cc_json_value_t *artifacts = NULL;
        cc_result_t rc = cc_json_parse(artifacts_json, &artifacts);
        if (rc.code != CC_OK) {
            cc_json_destroy(arr);
            return rc;
        }

        if (cc_json_is_array(artifacts)) {
            int n = cc_json_array_size(artifacts);
            for (int i = 0; i < n; ++i) {
                cc_json_value_t *part = artifact_object_to_content_part(cc_json_array_get(artifacts, i));
                if (part) cc_json_array_append(arr, part);
            }
        } else if (cc_json_is_object(artifacts)) {
            cc_json_value_t *part = artifact_object_to_content_part(artifacts);
            if (part) cc_json_array_append(arr, part);
        }
        cc_json_destroy(artifacts);
    }

    char *json = cc_json_stringify_unformatted(arr);
    cc_json_destroy(arr);
    if (!json) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to serialize content parts");

    *out_content_parts_json = json;
    return cc_result_ok();
}

cc_result_t cc_media_artifacts_summarize(
    const char *artifacts_json,
    char **out_summary
)
{
    if (!out_summary) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null artifact summary output");
    }
    *out_summary = NULL;

    if (!artifacts_json || !*artifacts_json) {
        *out_summary = cc_strdup("");
        return *out_summary ? cc_result_ok()
                            : cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate summary");
    }

    cc_json_value_t *artifacts = NULL;
    cc_result_t rc = cc_json_parse(artifacts_json, &artifacts);
    if (rc.code != CC_OK) {
        *out_summary = cc_strdup("[multimodal artifacts present]");
        return *out_summary ? cc_result_ok() : rc;
    }

    cc_string_builder_t sb;
    rc = cc_string_builder_init(&sb);
    if (rc.code != CC_OK) {
        cc_json_destroy(artifacts);
        return rc;
    }

    rc = cc_string_builder_append(&sb, "Multimodal artifacts:\n");
    if (rc.code == CC_OK && cc_json_is_array(artifacts)) {
        int n = cc_json_array_size(artifacts);
        for (int i = 0; i < n && rc.code == CC_OK; ++i) {
            rc = append_artifact_summary(&sb, cc_json_array_get(artifacts, i));
        }
    } else if (rc.code == CC_OK && cc_json_is_object(artifacts)) {
        rc = append_artifact_summary(&sb, artifacts);
    }

    cc_json_destroy(artifacts);
    if (rc.code != CC_OK) {
        cc_string_builder_deinit(&sb);
        return rc;
    }

    *out_summary = cc_string_builder_take(&sb);
    if (!*out_summary) {
        cc_string_builder_deinit(&sb);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate summary");
    }
    return cc_result_ok();
}
