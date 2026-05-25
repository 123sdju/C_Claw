/**
 * cc_media.h — 多模态媒体产物和 content part helper。
 */

#ifndef CC_MEDIA_H
#define CC_MEDIA_H

#include "cc/core/cc_result.h"
#include <stddef.h>

typedef struct cc_media_artifact {
    const char *id;
    const char *kind;
    const char *mime;
    const char *path;
    /*
     * Base64 is an app-provided transport field. The SDK does not read media
     * files or run codecs; board/app layers decide whether and how to encode
     * bytes, while providers only consume this normalized value.
     */
    const char *data_base64;
    size_t bytes;
    int width;
    int height;
    int duration_ms;
    const char *created_at;
} cc_media_artifact_t;

/**
 * 将单个 artifact 转成 provider-neutral content part JSON 字符串。
 * 调用方负责 free(*out_part_json)。
 */
cc_result_t cc_media_artifact_to_content_part_json(
    const cc_media_artifact_t *artifact,
    char **out_part_json
);

/**
 * 从 artifacts JSON 数组构建 content parts JSON 数组。
 * 会在数组开头插入 text part，便于模型理解这些媒体来自工具观察。
 */
cc_result_t cc_content_parts_build_text_image_audio(
    const char *text,
    const char *artifacts_json,
    char **out_content_parts_json
);

/**
 * 生成去除大 base64 的 artifact 摘要文本，用于历史压缩和 fallback provider。
 * 调用方负责 free(*out_summary)。
 */
cc_result_t cc_media_artifacts_summarize(
    const char *artifacts_json,
    char **out_summary
);

#endif
