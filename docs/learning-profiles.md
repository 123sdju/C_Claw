# Profile 学习文档

Profile 是编译期裁剪入口。profile 文件只设置默认值，命令行 `-DCC_ENABLE_*=ON/OFF`
可以覆盖。

## 主要 profile

- `core-minimal`：文本默认模式，最小 SDK 验证。
- `multimodal-full`：多模态全开，用于验证 image/audio/video/file 和 artifact。
- `desktop-agent`：桌面 Agent 常用能力。
- `mcu-text`：MCU 极简文本模式。
- `mcu-mm-lite`：MCU 轻量多模态，只保留 image/file artifact 引用。

## 常用裁剪开关

- `CC_ENABLE_MULTIMODAL`
- `CC_ENABLE_MEDIA_IMAGE`
- `CC_ENABLE_MEDIA_AUDIO`
- `CC_ENABLE_MEDIA_VIDEO`
- `CC_ENABLE_MEDIA_FILE`
- `CC_ENABLE_MEDIA_OUTPUT`
- `CC_ENABLE_INLINE_BASE64`
- `CC_ENABLE_ARTIFACT_STORE`
- `CC_ENABLE_STREAMING`
- `CC_ENABLE_SESSION_STORE_MEMORY`
- `CC_ENABLE_SESSION_STORE_JSON_FILE`
- `CC_ENABLE_SESSION_STORE_SQLITE`

运行期 config 不能突破编译期裁剪。配置启用了未编译能力时，`cc_config_validate()`
返回 `CC_ERR_UNSUPPORTED`。
