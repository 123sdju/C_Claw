# 存储学习文档

SDK 把存储分成多个端口，原因是桌面、服务端和 MCU 的持久化能力差异很大。core 只依赖
抽象接口，不直接依赖 SQLite、文件系统或某个数据库。

## 1. Session Store

`cc_session_store_t` 保存 Agent 对话历史：

- session 基本信息。
- typed messages。
- tool call 审计记录。
- tool result 审计记录。

消息在内存中是 `cc_message_t`。存储实现可以把它序列化成 JSON 或数据库字段，但应用层
不需要手写这些内部 JSON。

内置后端：

- memory session store：适合测试、临时会话、MCU 轻量模式。
- JSON file session store：适合单用户本地持久化。
- SQLite session store：适合桌面或服务端，支持更好的查询和事务。

## 2. Tool Result 存储

工具结果现在包含 typed artifact list。存储层通常会：

- 保存 `ok`。
- 保存 `text`。
- 保存 `error`。
- 保存 `metadata`。
- 把 `artifacts` 序列化为内部 JSON 数组。

这不是 public raw JSON API，而是存储后端自己的持久化格式。

## 3. Memory Store

`cc_memory_store_t` 用于长期记忆。它和 session store 的区别：

- session store 保存完整对话历史。
- memory store 保存可检索事实、偏好、摘要或应用自己的长期数据。

memory tool 可以让 LLM 写入或查询 memory store。active memory 可以在一轮对话结束后
自动写入摘要。

## 4. Artifact Store

`cc_artifact_store_t` 是多模态 artifact 的存储端口。

它保存的是：

- artifact id；
- kind；
- MIME；
- path 或 URI；
- base64 字段；
- 尺寸、时长、字节数；
- metadata。

它不负责：

- 摄像头采集；
- 麦克风采集；
- 图片压缩；
- 音频播放；
- 视频渲染；
- 文件上传下载。

这些由应用层或平台层做。

## 5. 选择后端的建议

- MCU 文本模式：memory session store。
- MCU 轻量多模态：memory session store + artifact 引用，不启用 inline base64。
- 桌面单用户：JSON file session store。
- 桌面完整 Agent：SQLite session store + memory store + artifact store。
- 服务端：建议应用层实现自己的 session/memory/artifact store，接入 ports。

## 6. 释放规则

从 store 加载出的 `cc_message_t *messages` 通常是数组：

```c
cc_message_t *messages = NULL;
size_t count = 0;
store.vtable->load_messages(store.self, session_id, 500, &messages, &count);

for (size_t i = 0; i < count; i++) {
    cc_message_cleanup(&messages[i]);
}
free(messages);
```

端口对象本身由创建者释放。通过 runtime builder 创建的 store 会由 builder 统一销毁。

## 7. 嵌入式面试关注点

存储层最适合用来讲“端口抽象”和“资源裁剪”：

- Linux/桌面可以用 SQLite，MCU 可以用 memory store、flash KV 或应用自定义 store。
- core 不直接依赖数据库 API，只依赖 `cc_session_store_t`、`cc_memory_store_t`、`cc_artifact_store_t`。
- store 返回的数组和结构体必须有明确释放函数，避免 C SDK 常见的所有权混乱。
- 多模态 artifact 尽量存引用，不在 MCU profile 里保存大块 base64。

面试表达：

> 我把会话历史、长期记忆和 artifact 拆成三个 port。这样 Linux 可以用 SQLite，
> MCU 可以换成轻量 KV 或内存 ring buffer，core 的 agent 主循环不需要改。

常见追问：

- 为什么 session store 和 memory store 要分开？
  - session 是完整对话历史，memory 是可检索长期事实，生命周期和查询方式不同。
- SQLite 适合 MCU 吗？
  - 通常不适合低资源 MCU，所以通过 profile 裁剪掉，保留 memory/json 或自定义 store。
- artifact store 为什么不负责摄像头？
  - 采集属于 BSP/应用层，SDK 只保存模型能引用和理解的 artifact 元数据。
