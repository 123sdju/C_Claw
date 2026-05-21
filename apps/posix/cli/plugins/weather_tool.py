#!/usr/bin/env python3
# 学习导读：apps/posix/cli/plugins/weather_tool.py
# 所属层次：示例插件。
# 阅读重点：这里演示 JSON-RPC 插件协议，重点看 stdin/stdout 如何承载工具调用。
# 注释说明：如果注释与代码冲突，以代码行为和测试为准。
"""
weather_tool.py — c-claw 插件工具示例（天气查询）

本文件演示了如何用 Python 编写一个符合 c-claw 插件协议的
外部工具。任何实现了从 stdin 读 JSON-RPC 请求、向 stdout 写
JSON-RPC 响应的可执行文件都可以作为 c-claw 插件。

── 通信协议 ──

标准输入 (stdin):
  每行一个完整的 JSON-RPC 2.0 请求，格式：
  {"jsonrpc":"2.0","id":"请求ID","method":"方法名","params":{...}}

标准输出 (stdout):
  每行一个完整的 JSON-RPC 2.0 响应，格式：
  成功: {"jsonrpc":"2.0","id":"请求ID","result":{...}}
  失败: {"jsonrpc":"2.0","id":"请求ID","error":{"code":-1,"message":"错误描述"}}

重要：JSON 必须是紧凑单行格式，不能有换行或缩进！
      每个 JSON 后跟一个 '\n'，用 sys.stdout.flush() 确保立即发送。

── 运行方式 ──

独立测试：
  echo '{"jsonrpc":"2.0","id":"1","method":"weather_query","params":{"city":"Beijing"}}' | python3 weather_tool.py

── 配置方式 (config.json plugins.entries) ──

  {
    "name": "weather",
    "command": "python3",
    "args": ["apps/posix/cli/plugins/weather_tool.py"],
    "tools": [{
      "name": "weather_query",
      "description": "查询指定城市的天气信息，返回温度和天气状况",
      "parameters": {
        "type": "object",
        "properties": {
          "city": {"type": "string", "description": "城市名称，如 Beijing, Shanghai"}
        },
        "required": ["city"]
      }
    }]
  }

── 错误码参考 ──

  -32700  Parse error       — JSON 解析失败
  -32601  Method not found  — 请求的方法名未注册
  -32602  Invalid params    — 参数格式或值不正确
"""

import sys
import json


def handle_request(request):
    """
    处理单个 JSON-RPC 请求，返回响应字典。

    通过 request["method"] 区分不同的工具调用。
    每个 method 对应一个工具函数。

    Args:
        request: JSON-RPC 请求字典，包含 method, params, id 字段

    Returns:
        响应字典，包含 jsonrpc, id 和 result 或 error
    """
    method = request.get("method", "")
    params = request.get("params", {})
    req_id = request.get("id", "")

    if method == "weather_query":
        return _handle_weather_query(params, req_id)
    else:
        return {
            "jsonrpc": "2.0",
            "id": req_id,
            "error": {
                "code": -32601,
                "message": f"Method not found: {method}"
            }
        }


def _handle_weather_query(params, req_id):
    """
    处理 weather_query 工具调用。

    实际应用中，这里会调用天气 API（如 OpenWeatherMap）。
    本示例返回模拟数据作为演示。

    Args:
        params: 包含 city 字段的参数字典
        req_id: 请求 ID

    Returns:
        成功响应包含城市天气数据
    """
    city = params.get("city", "Unknown")

    # 模拟天气数据（实际项目中替换为 API 调用）
    result = {
        "city": city,
        "temperature": 22,
        "unit": "celsius",
        "condition": "Sunny",
        "humidity": "45%",
        "wind": "5 km/h"
    }

    return {"jsonrpc": "2.0", "id": req_id, "result": result}


def main():
    """
    主循环：从 stdin 逐行读取 JSON-RPC 请求，处理后写入 stdout。

    协议要点：
    - 每行一个完整的 JSON（不能跨行）
    - JSON 是紧凑格式（无缩进换行）
    - 处理完立即 flush，确保主进程能及时收到响应
    - stdin 关闭时退出循环
    """
    for line in sys.stdin:
        line = line.strip()
        if not line:
            break

        try:
            request = json.loads(line)
            response = handle_request(request)
        except json.JSONDecodeError:
            response = {
                "jsonrpc": "2.0",
                "id": None,
                "error": {"code": -32700, "message": "Parse error"}
            }

        # 写入响应并立即 flush
        sys.stdout.write(json.dumps(response, ensure_ascii=False) + "\n")
        sys.stdout.flush()


if __name__ == "__main__":
    main()
