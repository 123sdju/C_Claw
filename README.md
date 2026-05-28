# C-Claw SDK

C-Claw SDK 是一个纯 C Agent Runtime 基础库。当前分支只保留 SDK 源码、构建
脚本、测试和中文接口文档；下游应用通过源码子目录复用：

```cmake
add_subdirectory(cclaw)
target_link_libraries(my_agent PRIVATE c_claw_runtime)
```

安装后也可以作为 CMake package 复用：

```cmake
find_package(CClaw CONFIG REQUIRED)
target_link_libraries(my_agent PRIVATE CClaw::runtime)
```

SDK 不提供固定 gateway。CLI、Web、UART、HTTP 服务、硬件采集和 UI 展示由应用层实现。

## 默认行为

- 默认只支持文本输入和文本输出。
- 多模态必须由应用配置显式开启；未配置 `multimodal` 段时按文本模式运行。
- 运行期有效能力 = 编译期 profile 开关 ∩ 应用配置 ∩ provider 能力。
- 运行期配置不能打开未编译进 SDK 的能力，`cc_config_validate()` 会返回 `CC_ERR_UNSUPPORTED`。
- SDK 只管理媒体 artifact 的元数据、引用、base64 字段和校验，不做图片/音频/视频编解码。
- SDK 不提供 examples、CLI、Web、UART 或业务 gateway，只提供核心 runtime、端口契约和默认 adapter。
- public API 可从聚合头 `#include <cc/cclaw.h>` 进入；版本信息在 `cc/core/cc_version.h`。
- 文件工具强制 workspace 边界检查；`http.request` 默认 deny，必须配置 network allowlist。
- logger 和 event bus 会对常见密钥字段做基础 redaction。

## 目录结构

```text
cclaw/core/       Agent 主循环、消息模型、工具结果、多模态 artifact、builder、queue、skills
cclaw/ports/      可替换端口接口：LLM、tool、storage、memory、artifact、platform
cclaw/adapters/   内置适配器：文件/HTTP 工具、provider、policy、session store、memory store
cclaw/platforms/  POSIX、Windows、ESP32、FreeRTOS 平台端口实现
cclaw/profiles/   编译期能力裁剪 profile
cclaw/tests/      SDK 行为测试
docs/             SDK 接口与学习文档
```

## 构建和测试

```bash
cmake --preset core-minimal
cmake --build --preset core-minimal
ctest --preset core-minimal
```

所有本地 profile 都有 test preset：

```bash
ctest --preset desktop-agent
ctest --preset multimodal-full
ctest --preset mcu-text
ctest --preset mcu-mm-lite
```

Sanitizer 构建：

```bash
cmake --preset core-minimal-sanitizers
cmake --build --preset core-minimal-sanitizers
ctest --preset core-minimal-sanitizers
```

当前 preset 关闭 LeakSanitizer leak 检测以避开本地 ptrace 限制；AddressSanitizer 和
UndefinedBehaviorSanitizer 仍启用。

可用 profile：

- `core-minimal`：文本默认模式，最小 SDK 验证构建。
- `multimodal-full`：开启 image/audio/video/file 输入输出、inline base64、artifact store。
- `desktop-agent`：桌面 Agent 常用能力，含工具、memory、skills、run queue、tool pool、多模态和 HTTP provider。
- `mcu-text`：极简文本模式，关闭文件工具、SQLite、skills、MCP、plugin、多 agent、多模态。
- `mcu-mm-lite`：轻量多模态，仅保留 image/file artifact 引用，不编译 audio/video/inline base64/artifact store。

`build/` 是本地构建产物目录，不应提交。

## 下游应用负责

- 提供 `cc_runtime_feature_set_t`，声明要注册的 provider、工具、存储、policy、sandbox、plugin/MCP loader。
- 选择并加载配置文件，管理 API key 和模型参数。
- 实现 gateway，把用户输入传给 `cc_agent_runtime_t` 或 `cc_agent_manager_t`。
- 实现平台私有工具、MCP transport、媒体采集/渲染、硬件资源管理。

## 文档入口

- [工程总览与逐文件阅读顺序](docs/learning-overview.md)
- [SDK 接口总览](docs/sdk-api.md)
- [Runtime 学习文档](docs/learning-runtime.md)
- [多模态学习文档](docs/learning-multimodal.md)
- [Provider 学习文档](docs/learning-provider.md)
- [工具学习文档](docs/learning-tools.md)
- [存储学习文档](docs/learning-storage.md)
- [Profile 学习文档](docs/learning-profiles.md)
- [移植学习文档](docs/learning-porting.md)

## License

Apache-2.0
