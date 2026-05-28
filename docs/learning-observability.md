# Observability 学习文档

Observability 是 SDK 的观测层，用来把 runtime、LLM、tool、approval、memory、stream 和 error
事件统一发布给日志、指标或调试 UI。

## 为什么需要统一 schema

旧做法容易出现两个问题：

- 不同事件 payload 形状不一致，下游解析复杂。
- 有些事件直接发布裸字符串，难以携带 session、step、status 和 error 信息。

现在所有业务事件都通过 `cc_observability_publish()` 构造统一 JSON：

```json
{
  "schema_version": 1,
  "event": "stream.text",
  "session_id": "s1",
  "run_id": "",
  "step": 0,
  "status": "delta",
  "message": "hello",
  "attributes": {
    "content": "hello"
  }
}
```

## 事件 family

- `run.*`：一轮 agent run 的完成状态。
- `llm.*`：provider 请求和响应。
- `tool.*`：工具开始、完成、错误。
- `approval.*`：审批 required、approved、denied。
- `memory.*`：长期记忆查询和写入。
- `stream.*`：text、thinking、tool delta、artifact、warning、error、finished。
- `error.*`：无法归类的 runtime 错误。

## 与 stream callback 的区别

stream callback 是业务输出通道：

- 适合 UI 实时显示文本。
- 顺序和延迟更重要。
- 回调数据只在调用期间有效。

event bus 是观测通道：

- 适合日志、指标、调试 UI。
- payload 是统一 JSON。
- 可以同步或异步投递。
- 底层会做 redaction。

面试表达：

> 我把实时输出和观测事件分开。UI 不需要解析日志 JSON，日志系统也不影响 callback 的实时性。
> 这是嵌入式里常见的控制面和观测面分离思路。

## 错误字段

当事件带 `cc_result_t` 错误时，payload 会包含：

- `error.code`
- `error.message`
- `error.http_status`
- `error.retry_after_ms`
- `error.recoverable`
- `error.provider_error_code`
- `error.raw_redacted_body`

provider 429、5xx、timeout、cancel、JSON parse 都能被结构化记录，下游应用再决定是否 retry。

## attributes

`attributes_json` 必须是 JSON object。它用于放事件私有字段：

- stream text：`content`
- tool event：`tool`、`tool_call_id`、`args`、`ok`
- approval event：`tool`、`reason`
- run finished：`reason`

这样顶层 schema 稳定，业务字段又能扩展。

## 测试关注点

- 每个 family 都能生成固定 schema。
- attributes 必须是 object。
- event bus 发布后 secret 会被 redaction。
- runtime/tool 业务路径不能直接调用底层 event bus 发布函数。
- 旧事件名不再保留 alias。
