# 平台移植学习文档

移植 C-Claw SDK 时优先实现 ports，而不是改 core。

## 必要平台端口

- `cc_thread.h`：线程、互斥锁、条件变量。
- `cc_filesystem.h`：文件系统访问。
- `cc_path.h`：路径规范化和拼接。
- `cc_http_client.h`：只有 HTTP 工具或远程 provider 需要。
- `cc_process.h`：只有 shell、plugin、stdio MCP 需要。

## 安全要求

- `cc_path_canonical()` 必须对不存在的目标路径也做 `.` / `..` 归一化；文件写入前要同时校验
  目标路径和 parent dir 都在 workspace 内。
- 如果平台支持 symlink，parent dir 的 canonical 结果必须解析 symlink，不能只做字符串前缀判断。
- shell/process 端口默认视为高风险能力；profile 未显式启用时不编译，启用后仍需要 policy/approval。
- HTTP 端口只提供传输能力；`http.request` 工具默认 deny，应用或配置必须提供 network allowlist。
- 如果 HTTP client 自动跟随 redirect，adapter 必须能拿到最终 URL 并重新执行 network policy；
  否则应禁用自动 redirect，把跳转交给上层显式处理。

## MCU 建议

- 从 `mcu-text` 开始，只保留文本 runtime 和 memory session store。
- 确认 RAM/栈预算后再开启 `mcu-mm-lite`。
- MCU 多模态优先传 path/uri/id 等引用，不启用 inline base64。
- 摄像头、麦克风、屏幕、音频播放由应用层或 BSP 完成，SDK 不绑定具体硬件。

## 下游接入顺序

1. 选择 profile 并完成 configure/build。
2. 实现或选择平台端口。
3. 提供 `cc_runtime_feature_set_t`。
4. 实现 gateway。
5. 增加平台私有工具和 provider。

plugin、MCP、subagent 都是 extension point。SDK core 只提供端口和协议 helper，不提供业务
gateway、UI、API key 分发或硬件采集入口。

## 嵌入式面试关注点

移植文档可以用来回答“你怎么做跨平台”和“你了解 RTOS/Linux 差异吗”：

- Linux 上优先讲 pthread、realpath、curl、fork/exec 和文件权限。
- RTOS 上优先讲 task、mutex/semaphore、flash 文件系统、lwIP/mbedTLS 和无进程模型。
- core 不直接调用平台 API，所有差异收敛到 ports/platforms。
- profile 是编译期裁剪，不是把所有功能都带到固件里再运行时关闭。
- path、network、shell/process 是最容易出安全问题的移植点。

常见追问：

- 如果平台没有文件系统？
  - 实现 memory session store，或把 store port 映射到 flash KV；文件工具不编译。
- 如果平台没有动态加载？
  - plugin/MCP 是扩展点，不是 core 必需能力；MCU profile 可以关闭。
- 如果平台 HTTP client 不支持 redirect target？
  - 禁用自动 redirect，或由 adapter 暴露最终 URL 后重新执行 allowlist 校验。
- 为什么不把 UART 放进 SDK？
  - UART 是设备业务入口，属于应用/BSP；SDK 只提供 runtime 和 port，不绑定具体硬件。
