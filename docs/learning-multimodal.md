# 多模态学习文档

本文用于配合阅读 `cclaw/core/include/cc/core/cc_media.h` 和
`cclaw/core/src/core/cc_media.c`。这一层是 SDK 多模态能力的基础：它不调用模型、
不访问文件系统、不做图片/音频/视频编解码，只定义“如何在 SDK 内部表达媒体内容”。

## 1. 为什么要 typed model

旧方式把多模态内容当 JSON 字符串透传，应用层需要自己拼 provider 协议，问题很多：

- 应用会直接依赖 OpenAI、Anthropic、Ollama 的具体 JSON 格式。
- 字符串转义和内存释放容易出错。
- SDK 无法在发送请求前统一检查 MIME、base64 大小、artifact 数量。
- MCU 这类资源受限平台很难裁剪掉不需要的能力。

现在的规则是：

```text
应用层构造 typed message
        ↓
SDK runtime 做能力和限制检查
        ↓
provider adapter 转成具体模型协议
```

所以应用只接触 `cc_message_t`、`cc_content_part_t`、`cc_media_artifact_t` 等结构。

## 2. 默认文本模式

没有配置 `multimodal` 时，SDK 只支持文本：

- 文本输入永远启用。
- 文本输出永远启用。
- 图片、音频、视频、文件输入默认关闭。
- 图片、音频、视频、文件输出默认关闭。
- inline base64 默认关闭。

运行期有效能力来自三层相交：

```text
编译期 CC_ENABLE_* ∩ 应用 config ∩ provider capability
```

例子：即使 config 写了 `input.image=true`，如果当前 profile 没有编译
`CC_ENABLE_MEDIA_IMAGE`，`cc_config_validate()` 也会返回 `CC_ERR_UNSUPPORTED`。

## 3. 核心结构

### cc_media_kind_t

```c
typedef enum cc_media_kind {
    CC_MEDIA_TEXT,
    CC_MEDIA_IMAGE,
    CC_MEDIA_AUDIO,
    CC_MEDIA_VIDEO,
    CC_MEDIA_FILE
} cc_media_kind_t;
```

`TEXT` 是 content part，不应该作为 artifact 校验。非文本内容都通过 artifact 描述。

### cc_media_artifact_t

artifact 是媒体对象的元数据和值对象：

```c
typedef struct cc_media_artifact {
    char *id;
    cc_media_kind_t kind;
    char *mime;
    char *path;
    char *uri;
    char *data_base64;
    size_t bytes;
    int width;
    int height;
    int duration_ms;
    char *created_at;
    char *metadata;
} cc_media_artifact_t;
```

字段含义：

- `id`：应用或 SDK 分配的 artifact 标识，用于日志、store、provider 输出关联。
- `kind`：媒体类型，不能为 `CC_MEDIA_TEXT`。
- `mime`：媒体 MIME，例如 `image/png`、`audio/wav`。
- `path`：本地路径引用，适合桌面或设备本地文件。
- `uri`：远程或应用私有 URI。
- `data_base64`：inline base64 内容，只在 config 和编译期开启后使用。
- `bytes`：媒体字节数，SDK 用它做限制检查。
- `width` / `height`：图片或视频尺寸。
- `duration_ms`：音频或视频时长。
- `created_at`：创建时间字符串。
- `metadata`：扩展元数据字符串，建议使用 JSON，但 SDK 不解析它。

所有 `char *` 字段由 artifact 拥有。调用 `cc_media_artifact_cleanup()` 会释放这些字段。

### cc_content_part_t

content part 是消息内容的最小单元：

```c
typedef struct cc_content_part {
    cc_media_kind_t kind;
    cc_content_part_direction_t direction;
    char *text;
    cc_media_artifact_t artifact;
} cc_content_part_t;
```

规则：

- `kind == CC_MEDIA_TEXT` 时使用 `text`。
- `kind != CC_MEDIA_TEXT` 时使用 `artifact`。
- `direction` 表示该 part 是输入还是输出，方便 provider 和 UI 区分来源。

### cc_content_parts_t

`cc_content_parts_t` 是动态数组。一个 message 可以包含多个 part：

```text
用户消息:
  part[0] text  = "请解释这张图"
  part[1] image = artifact(id=img_1, mime=image/png, path=...)
```

## 4. 典型调用顺序

### 构造纯文本消息

```c
cc_message_t *msg = NULL;
cc_result_t rc = cc_message_create_text(
    "msg_1",
    "session_1",
    CC_ROLE_USER,
    "hello",
    NULL,
    &msg
);
if (rc.code != CC_OK) {
    cc_result_free(&rc);
    return;
}

/* 使用 msg */

cc_message_destroy(msg);
```

### 构造图片输入消息

```c
cc_media_artifact_t image;
cc_media_artifact_init(&image);
image.id = strdup("img_1");
image.kind = CC_MEDIA_IMAGE;
image.mime = strdup("image/png");
image.path = strdup("/tmp/input.png");
image.bytes = 1024;
image.width = 640;
image.height = 480;

cc_content_parts_t parts;
cc_content_parts_init(&parts);
cc_content_parts_append_text(&parts, "请描述这张图片", CC_CONTENT_PART_INPUT);
cc_content_parts_append_artifact(&parts, &image, CC_CONTENT_PART_INPUT);

cc_message_t *msg = NULL;
cc_message_create_parts("msg_2", "session_1", CC_ROLE_USER, &parts, NULL, &msg);

cc_message_destroy(msg);
cc_content_parts_cleanup(&parts);
cc_media_artifact_cleanup(&image);
```

注意：`cc_content_parts_append_artifact()` 会深拷贝 artifact，所以 `image` 可以在 append 后清理。

## 5. 校验规则

`cc_media_artifact_validate()` 会检查：

- artifact 不能为 NULL。
- artifact kind 不能是 `CC_MEDIA_TEXT`。
- MIME 必须非空。
- MIME 必须匹配 `allowed_mime_prefixes`，除非白名单为空。
- `path` 不能包含 `..`。
- `bytes` 不能超过 `max_artifact_bytes`。
- 如果使用 `data_base64`，必须允许 inline base64。
- base64 字符串长度不能超过 `max_base64_bytes`。

SDK 不会做：

- 打开文件检查真实大小。
- 解码 base64。
- 探测 URI 是否可访问。
- 判断图片格式是否合法。

这些工作属于应用层或平台层。

## 6. 序列化和摘要

`cc_content_parts_to_json()`、`cc_media_artifact_list_to_json()` 主要给 session store 和
provider adapter 使用。它们不是旧 raw JSON API 的兼容层。

`cc_media_artifact_list_summarize()` 和 `cc_content_parts_text_summary()` 用于降级：

- provider 不支持图片时，可以把图片变成文本摘要。
- 历史压缩时，只保留 id、mime、path、uri、尺寸、时长。
- 摘要不会写入 `data_base64`。

## 7. 释放规则速查

- 栈上 `cc_media_artifact_t`：先 `cc_media_artifact_init()`，不用时 `cc_media_artifact_cleanup()`。
- `cc_media_artifact_list_t`：先 `cc_media_artifact_list_init()`，不用时 `cc_media_artifact_list_cleanup()`。
- `cc_content_parts_t`：先 `cc_content_parts_init()`，不用时 `cc_content_parts_cleanup()`。
- `cc_message_t *`：通过 create 函数创建后，用 `cc_message_destroy()`。
- `char *out_json` / `char *out_summary`：成功返回后调用方 `free()`。

失败路径可以统一 cleanup，因为这些 cleanup 函数都支持零初始化对象。
