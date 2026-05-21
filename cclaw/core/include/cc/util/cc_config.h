/**
 * 学习导读：cclaw/core/include/cc/util/cc_config.h
 *
 * 所属层次：核心层。
 * 阅读重点：这里定义 Agent 运行时的数据模型、主循环和通用工具，阅读时重点看所有权、错误返回和 ReAct 数据流。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * cc_config.h — 系统配置数据模型与加载模块
 *
 * @file    cc/util/cc_config.h
 * @brief   定义 c-claw 的全局配置结构体及其序列化/反序列化接口。
 *
 * 配置文件 config.json 是系统的"构造蓝图"，所有组件的初始化参数
 * 都在这里集中定义。本模块负责将 JSON 配置解析为类型安全的 C 结构体。
 *
 * ─── 接口契约 ─────────────────────────────────────────────────────────
 *
 *   - cc_config_load() 从 JSON 文件读取配置，失败时返回错误但可降级
 *   - cc_config_load_default() 不读文件，使用硬编码的默认值
 *   - cc_config_destroy() 释放 config 中的所有动态分配字段
 *   - 调用方不需要手动释放字段，全部委托给 cc_config_destroy()
 *
 * ─── 配置入口 ─────────────────────────────────────────────────────────
 *
 *   config.json 是唯一主配置入口；未写入 config.json 的字段走
 *   cc_config_load_default() 中的 profile 默认值。loader 解析后会做统一语义
 *   校验，例如 MCP transport/url/command、plugin workers、timeout 等。
 *
 * ─── 依赖 ─────────────────────────────────────────────────────────────
 *
 *   依赖 cc/core/cc_result.h（错误传递）。
 */

#ifndef CC_CONFIG_H
#define CC_CONFIG_H

#include "cc/core/cc_result.h"
#include <stddef.h>

/**
 * cc_config_string_list_t — 配置中的字符串数组。
 *
 * 所有 items[i] 都由 cc_config_t 拥有，调用方只借用读取。
 * 这个小结构会被 agents/skills/tools/MCP 多处复用，避免每个段都手写
 * `char ** + count` 字段并增加释放遗漏风险。
 */
typedef struct cc_config_string_list {
    char **items;
    size_t count;
} cc_config_string_list_t;

typedef struct cc_config_agent_profile {
    char *id;
    char *model;
    char *workspace;
    char *agent_dir;
    char *system_prompt_file;
    cc_config_string_list_t skills;
} cc_config_agent_profile_t;

typedef struct cc_config_agents {
    cc_config_agent_profile_t defaults;
    cc_config_agent_profile_t *profiles;
    size_t profile_count;
} cc_config_agents_t;

typedef struct cc_config_queue {
    int main_concurrency;
    int subagent_concurrency;
    int plugin_concurrency;
    int mcp_concurrency;
    int per_session_concurrency;
    char *mode;
    int debounce_ms;
    int max_pending_per_session;
} cc_config_queue_t;

typedef struct cc_config_tool_policy {
    char *name;
    int concurrency;
    int timeout_ms;
} cc_config_tool_policy_t;

typedef struct cc_config_tools {
    cc_config_string_list_t enabled;
    int default_timeout_ms;
    cc_config_tool_policy_t *policies;
    size_t policy_count;
} cc_config_tools_t;

typedef struct cc_config_plugin_tool {
    char *name;
    char *description;
    char *parameters_json;
} cc_config_plugin_tool_t;

typedef struct cc_config_plugin_entry {
    char *id;
    int enabled;
    char *command;
    char **args;
    size_t arg_count;
    int workers;
    int timeout_ms;
    int max_in_flight;
    int restart_on_crash;
    cc_config_plugin_tool_t *tools;
    size_t tool_count;
    cc_config_string_list_t skill_dirs;
} cc_config_plugin_entry_t;

typedef struct cc_config_plugins {
    int hot_reload;
    int reload_debounce_ms;
    cc_config_plugin_entry_t *entries;
    size_t entry_count;
} cc_config_plugins_t;

typedef struct cc_config_skills {
    int watch;
    int watch_debounce_ms;
    cc_config_string_list_t extra_dirs;
} cc_config_skills_t;

typedef struct cc_config_mcp_server {
    char *name;
    char *command;
    char **args;
    size_t arg_count;
    char *cwd;
    char *url;
    char *transport;
    int connection_timeout_ms;
} cc_config_mcp_server_t;

typedef struct cc_config_mcp {
    int enabled;
    int session_idle_ttl_ms;
    cc_config_mcp_server_t *servers;
    size_t server_count;
} cc_config_mcp_t;

/**
 * cc_config_t — 系统全局配置结构体
 *
 * 从 config.json 反序列化得到，包含控制所有组件行为的参数。
 * 所有字符串字段由内部 malloc 分配，必须通过 cc_config_destroy() 释放。
 */
typedef struct cc_config {
    char *provider;              /**< LLM 后端类型：
                                  *   "openai" = OpenAI 兼容 API
                                  *   "anthropic" = Anthropic Messages API
                                  *   "ollama" = 本地 Ollama 服务
                                  *   默认值：按编译 profile 选择首个可用 provider */
    char *model;                 /**< LLM 模型名称，如 "gpt-4o-mini"、"claude-3-5-haiku-latest"。
                                  *   默认值：按 provider 选择 */
    char *base_url;              /**< LLM API 基础地址：
                                  *   Ollama: "http://localhost:11434"
                                  *   OpenAI: "https://api.openai.com"
                                  *   Anthropic: "https://api.anthropic.com"
                                  *   默认值：按 provider 选择 */
    char *api_key;               /**< API 密钥（远程 provider 需要，Ollama 可为 NULL）。
                                  *   默认值：NULL */
    char *storage_type;          /**< 存储后端类型："json" 或 "sqlite"。
                                  *   默认值："json" */
    char *data_dir;              /**< 运行时数据目录。
                                  *   默认值：由平台 profile 的 CC_DEFAULT_DATA_DIR 提供 */
    char *storage_path;          /**< 存储数据目录路径。
                                  *   默认值：由平台 profile 的 CC_DEFAULT_STORAGE_PATH 提供 */
    char *workspace_path;        /**< Agent 工作区路径，所有文件工具操作限定在此。
                                  *   默认值：由平台 profile 的 CC_DEFAULT_WORKSPACE_PATH 提供 */
    char *sandbox_type;          /**< 沙箱类型："local"。
                                  *   默认值："local" */
    int shell_requires_approval; /**< Shell 工具是否需要用户审批：
                                  *   1 = 交互式审批（每次执行前询问）
                                  *   0 = 自动批准（CI/非交互模式）
                                  *   默认值：1 */
    int sandbox_timeout_ms;      /**< 沙箱命令执行超时（毫秒）。
                                  *   超过此时间子进程被 SIGKILL 终止。
                                  *   默认值：30000（30 秒） */
    int max_steps;               /**< Agent 最大推理步数。
                                  *   每步 = 一次 LLM 推理 + 一次工具执行。
                                  *   超过此值 Agent 强制停止，防止无限循环。
                                  *   默认值：10 */
    int thinking_mode;           /**< 是否启用 LLM 思维链模式（CoT/thinking）：
                                  *   1 = 启用（LLM 输出思考过程 + 最终回答）
                                  *   0 = 禁用（仅输出最终回答）
                                  *   默认值：0 */
    int max_tokens;              /**< 主对话单次回复最大 token 数。
                                  *   默认值：4096 */
    double temperature;           /**< 主对话生成温度（0.0-2.0）。
                                  *   0.0 表示最确定输出。
                                  *   默认值：0.7 */
    int stream_mode;             /**< 是否默认使用 LLM 流式请求：
                                  *   1 = 默认调用流式 LLM API
                                  *   0 = 默认调用同步 LLM API，运行时可由 gateway 命令切换
                                  *   默认值：0 */
    int debug_mode;              /**< CLI / gateway 是否默认开启调试输出：
                                  *   1 = 启动时开启内部调试输出
                                  *   0 = 默认关闭，运行时可由 gateway 命令切换
                                  *   默认值：0 */
    char *memory_backend;        /**< 长期记忆后端类型："json_file" | "sqlite" | "noop"。
                                  *   "json_file" = JSON 文件存储
                                  *   "sqlite"    = SQLite 数据库存储
                                  *   "noop"      = 禁用记忆（嵌入式平台裁剪）
                                  *   默认值："json_file" */
    char *memory_path;           /**< 长期记忆存储路径。
                                  *   json_file 后端：JSON 文件路径（默认 profile memory path）
                                  *   sqlite 后端：数据库文件路径（默认 profile memory path）
                                  *   默认值：由平台 profile 的 CC_DEFAULT_MEMORY_PATH 提供 */
    int active_memory_enabled;   /**< 是否在 run 结束后自动写入可检索摘要。
                                  *   只有 CC_ENABLE_ACTIVE_MEMORY=ON 时 runtime 才会编译执行路径。 */
    int active_memory_write_summary; /**< 非 0 表示 run 后写入 user/assistant 摘要。 */
    int active_memory_max_value_chars; /**< 单条 active memory 最大字符数，防止长回答撑爆设备存储。 */
    char *active_memory_category; /**< active memory 写入分类，默认 "active_summary"。 */
    char *system_prompt;         /**< 显式系统提示词覆盖。
                                  *   优先级最高：如果设置，则忽略 soul.md / user.md。
                                  *   默认值：NULL（使用 soul.md + user.md 组合） */
    char *soul_file;             /**< Agent 人格定义文件路径。
                                  *   定义 Agent 的核心身份、行为准则、角色定位。
                                  *   启动时自动读取并注入到 system prompt 头部。
                                  *   默认值："soul.md" */
    char *user_file;             /**< 用户偏好文件路径。
                                  *   定义用户的个性化上下文、偏好风格、长期规则。
                                  *   启动时自动读取并注入到 system prompt 中。
                                  *   默认值："user.md" */
    char **enabled_tools;        /**< 启用的工具名称列表（过滤用）。
                                  *   为 NULL 时表示启用所有已注册工具。
                                  *   默认值：NULL */
    size_t enabled_tools_count;  /**< enabled_tools 数组的长度 */
    char **plugin_commands;      /**< 插件启动命令列表。
                                  *   每个元素是完整的命令行（如 "python3 plugin.py"）。
                                  *   默认值：NULL（不加载任何插件） */
    size_t plugin_count;         /**< plugin_commands 数组的长度 */
    int context_window_tokens;   /**< LLM 上下文窗口 token 预算。
                                  *   用于动态截断历史消息，防止超出窗口限制。
                                  *   默认值：8192（兼容 8K 窗口模型）。
                                  *   设置为 0 表示不限制（关闭动态截断）。 */
    double context_compress_threshold; /**< 上下文压缩触发阈值（0.0-1.0）。
                                  *   当已用 token > context_window_tokens * threshold 时触发压缩。
                                  *   默认值：0.8（窗口使用率 80% 时开始压缩旧消息）。
                                  *   设置为 0 表示不压缩，仅做硬截断。 */
    int context_keep_recent;     /**< 压缩时保留最近 N 条原始消息不被压缩。
                                  *   这些消息保留完整细节，确保最近对话的连贯性。
                                  *   默认值：20 */
    int summary_max_tokens;      /**< 上下文摘要压缩请求的最大生成 token 数。
                                  *   默认值：1024 */
    double summary_temperature;   /**< 上下文摘要压缩请求的生成温度。
                                  *   默认值：0.3 */

    /** 分段运行时配置：这些字段是主配置模型，平铺字段仅作为 runtime 便捷缓存。 */
    cc_config_agents_t agents;
    cc_config_queue_t queue;
    cc_config_tools_t tools;
    cc_config_plugins_t plugins;
    cc_config_skills_t skills;
    cc_config_mcp_t mcp;
} cc_config_t;

/**
 * cc_config_load — 从 JSON 文件加载配置
 *
 * 解析指定路径的 JSON 配置文件并填充 cc_config_t。
 * 文件中未指定的字段将使用默认值。
 * 文件不存在或格式错误时返回错误，但调用方可选择降级运行。
 *
 * @param path       配置文件路径（通常为 "config.json"）
 * @param out_config 输出：填充好的配置结构体（调用方负责 cc_config_destroy）
 * @return           CC_OK 表示加载成功
 */
cc_result_t cc_config_load(const char *path, cc_config_t *out_config);

/**
 * cc_config_load_default — 加载硬编码的默认配置
 *
 * 不读取任何文件，使用内置默认值初始化配置。
 * 适用于"零配置启动"场景——用户下载后即可运行，无需手动创建配置文件。
 *
 * @param out_config  输出：填充默认值的配置结构体（调用方负责 cc_config_destroy）
 * @return            CC_OK 表示成功
 */
cc_result_t cc_config_load_default(cc_config_t *out_config);

/**
 * cc_config_destroy — 释放配置中的所有动态资源
 *
 * 释放所有字符串字段和数组的内存。config 指针本身不会被释放
 * （它通常在栈上或作为另一个结构体的成员）。
 * 传入 NULL 是安全的（无操作）。
 *
 * @param config  要释放的配置结构体指针
 */
void cc_config_destroy(cc_config_t *config);

/**
 * cc_config_build_system_prompt — 从配置文件构建系统提示词
 *
 * 按优先级组合系统提示词源：
 *   1. 如果 config.system_prompt 非空 → 直接使用（显式覆盖）
 *   2. 否则读取 soul.md 和 user.md 文件并合并
 *   3. 如果两者都不可用 → 返回硬编码默认值
 *
 * soul.md 追加到 system prompt 头部（定义 Agent 人格），
 * user.md 追加到 system prompt 末尾（定义用户偏好）。
 * 如果文件不存在或读取失败，静默跳过。
 *
 * @param config     配置结构体指针（不可为 NULL）
 * @param out_prompt 输出：构建好的系统提示词字符串（调用者负责 free）
 * @return           CC_OK 表示成功
 */
cc_result_t cc_config_build_system_prompt(const cc_config_t *config, char **out_prompt);

#endif
