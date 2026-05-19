/**
 * 学习导读：cclaw/core/src/util/cc_config.c
 *
 * 所属层次：核心层。
 * 阅读重点：这里定义 Agent 运行时的数据模型、主循环和通用工具，阅读时重点看所有权、错误返回和 ReAct 数据流。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * cc_config.c — 配置加载与管理模块
 *
 * 在整体架构中的角色和层次：
 *   本模块位于 Util 层，是应用程序的配置管理基础设施。
 *   Util 层位于 Platform 层之上、Core/Business 层之下，提供通用工具能力。
 *   本模块负责加载和管理整个应用程序的运行参数，涵盖 LLM 模型、存储后端、
 *   工作区、沙箱、工具集、插件等所有可配置项。它是程序启动时的第一个初始化步骤：
 *   所有其他模块的初始化都依赖于本模块提供的配置参数。
 *
 * 配置机制（双层 fallback）：
 *   第一层：从 JSON 配置文件读取（通过 cc_json_parse_from_file 解析）
 *   第二层：JSON 文件读取失败或字段缺失时，回退到硬编码的默认值
 *   这种设计确保了程序的"开箱即用"体验——即使没有配置文件也能正常运行。
 *
 * 核心设计：
 *   - 分层 fallback：JSON 文件读取失败 → 字段缺失（或类型不匹配）→ 硬编码默认值
 *   - 容错优先（fail-safe）：配置文件不存在或损坏时不会报错，静默使用默认配置
 *     这是刻意为之的设计选择，优先保证程序可启动，而非严格校验配置
 *   - 段式解析：将配置分为 model/storage/workspace/sandbox/tools 五个逻辑段，
 *     每段独立解析和 fallback，一个段缺失或损坏不影响其他段
 *   - 内存管理：所有字符串字段通过 strdup 在堆上分配，
 *     cc_config_destroy 统一释放，确保无内存泄漏
 *   - 字段语义：所有字段均设计为可选，任何字段缺失都不会导致解析失败
 *
 * JSON 配置结构（完整示例）：
 *   {
 *     "model": {
 *       "provider":       "ollama"     // LLM 提供商标识（ollama/openai/anthropic）
 *       "model":          "qwen2.5-coder:7b" // 模型名称
 *       "base_url":       "http://localhost:11434" // API 端点地址
 *       "api_key":        "sk-xxxx"    // API 密钥（可选，本地模型不需要）
 *       "thinking_mode":  0            // 是否启用思考模式（TRAE 扩展）
 *     },
 *     "storage": {
 *       "type":           "sqlite"     // 存储后端类型（sqlite/file/memory）
 *       "path":           profile SQLite path // 数据库文件路径
 *     },
 *     "workspace": {
 *       "path":           profile workspace path // Agent 工作目录（创建临时文件的位置）
 *     },
 *     "sandbox": {
 *       "type":                   "local"    // 沙箱类型（local/docker）
 *       "shell_requires_approval": true      // shell 命令是否需要用户确认
 *       "timeout_ms":             30000      // 命令执行超时（毫秒）
 *     },
 *     "tools": {
 *       "enabled": ["read", "write", "shell"] // 启用的工具列表
 *     }
 *   }
 *
 * 依赖：
 *   - cc/util/cc_json.h — JSON 解析和读取基础设施
 *   - 标准 C 库 — stdlib（malloc/free/calloc）、string（strdup/memset）
 */

#include "cc/util/cc_config.h"
#include "cc/ports/cc_platform.h"
#include "cc/util/cc_json.h"
#include "cc/util/cc_string_builder.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#if CC_LLM_OPENAI
#define CC_DEFAULT_PROVIDER "openai"
#define CC_DEFAULT_MODEL "gpt-4o-mini"
#define CC_DEFAULT_BASE_URL "https://api.openai.com"
#elif CC_LLM_ANTHROPIC
#define CC_DEFAULT_PROVIDER "anthropic"
#define CC_DEFAULT_MODEL "claude-3-5-haiku-latest"
#define CC_DEFAULT_BASE_URL "https://api.anthropic.com"
#elif CC_LLM_OLLAMA
#define CC_DEFAULT_PROVIDER "ollama"
#define CC_DEFAULT_MODEL "qwen2.5-coder:7b"
#define CC_DEFAULT_BASE_URL "http://localhost:11434"
#else
#define CC_DEFAULT_PROVIDER "none"
#define CC_DEFAULT_MODEL "none"
#define CC_DEFAULT_BASE_URL ""
#endif

#if CC_STORAGE_SQLITE
#define CC_DEFAULT_STORAGE_TYPE "sqlite"
#elif CC_STORAGE_JSON_FILE
#define CC_DEFAULT_STORAGE_TYPE "json"
#else
#define CC_DEFAULT_STORAGE_TYPE "memory"
#endif

#ifndef CC_DEFAULT_DATA_DIR
#define CC_DEFAULT_DATA_DIR "runtime/data"
#endif

#ifndef CC_DEFAULT_WORKSPACE_PATH
#define CC_DEFAULT_WORKSPACE_PATH "runtime/workspace"
#endif

#ifndef CC_DEFAULT_STORAGE_PATH
#if CC_STORAGE_SQLITE
#define CC_DEFAULT_STORAGE_PATH "runtime/data/c-claw.db"
#else
#define CC_DEFAULT_STORAGE_PATH "runtime/data/sessions.json"
#endif
#endif

#ifndef CC_DEFAULT_MEMORY_PATH
#define CC_DEFAULT_MEMORY_PATH "runtime/data/memory.json"
#endif

#ifndef CC_DEFAULT_PLUGIN_CONFIG_PATH
#define CC_DEFAULT_PLUGIN_CONFIG_PATH ""
#endif

#ifndef CC_DEFAULT_SOUL_FILE
#define CC_DEFAULT_SOUL_FILE "soul.md"
#endif

#ifndef CC_DEFAULT_USER_FILE
#define CC_DEFAULT_USER_FILE "user.md"
#endif

#if CC_HAS_MEMORY
#define CC_DEFAULT_MEMORY_BACKEND "json_file"
#else
#define CC_DEFAULT_MEMORY_BACKEND "noop"
#endif

/**
 * read_text_file — 读取文本文件的全部内容到堆内存
 *
 * 功能：
 *   将指定路径的文本文件完整读入内存，返回以 '\0' 结尾的 C 字符串。
 *   调用方负责 free 返回的缓冲区。
 *
 * @param path 文件路径（不可为 NULL）
 * @return 堆上分配的字符串（调用方负责 free），NULL 表示读取失败
 *
 * 设计决策：
 *   - 使用 fseek + ftell 获取文件大小再一次性读取，而非逐行读取。
 *     WHY：逐行读取需要多次 realloc 来扩展缓冲区，而一次性读取
 *     只需一次 malloc。对于典型的配置文件（几 KB 到几十 KB），
 *     一次性分配的效率更高。
 *
 *   - 使用 "rb" 模式打开文件（二进制读取模式）。
 *     WHY：避免 Windows 下 \r\n 到 \n 的自动转换影响文件大小计算。
 *     且对于 UTF-8 编码的 soul.md/user.md 文件，二进制读取更安全。
 *
 *   - 文件读取失败时不设置 errno 以外的错误信息。
 *     WHY：上层调用方（cc_config_build_system_prompt）对文件读取失败
 *     采用静默忽略策略，文件不存在是正常情况。
 *
 * 典型调用场景：
 *   char *content = read_text_file("soul.md");
 *   if (content) { ... 使用 content ... }
 *   free(content);
 */
static char *read_text_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) { fclose(f); return NULL; }
    char *buf = malloc(len + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, len, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

/**
 * replace_string — 用新分配字符串替换配置字段；分配失败时保留旧值并返回错误。
 *
 * 位置：核心数据模型层。注释重点说明当前函数的输入输出、资源边界和错误传播。
 *
 * @param field 输出参数；成功时写入有效结果，失败时保持为 NULL 或未定义状态。
 * @param value 借用的只读字符串；函数不会释放该指针。
 * @return 返回整数状态、计数或断言结果，供当前调用链判断下一步。
 */
static int replace_string(char **field, const char *value)
{
    if (!value) return 1;
    char *copy = strdup(value);
    if (!copy) return 0;
    free(*field);
    *field = copy;
    return 1;
}

/**
 * json_string_field — 在结构体与 JSON/文本之间转换，并负责字段校验和临时内存。
 *
 * 位置：核心数据模型层。注释重点说明当前函数的输入输出、资源边界和错误传播。
 *
 * @param obj 借用的指针参数；若需要长期保存内容，函数会复制。
 * @param key 借用的只读字符串；函数不会释放该指针。
 * @return 返回借用或静态只读字符串；调用方不得释放。
 */
static const char *json_string_field(cc_json_value_t *obj, const char *key)
{
    if (!obj || !key) return NULL;
    return cc_json_string_value(cc_json_object_get(obj, key));
}

/**
 * json_boolish_value — 在结构体与 JSON/文本之间转换，并负责字段校验和临时内存。
 *
 * 位置：核心数据模型层。注释重点说明当前函数的输入输出、资源边界和错误传播。
 *
 * @param value 借用的指针参数；若需要长期保存内容，函数会复制。
 * @return 返回整数状态、计数或断言结果，供当前调用链判断下一步。
 */
static int json_boolish_value(cc_json_value_t *value)
{
    const char *s = cc_json_string_value(value);
    if (s) {
        return strcmp(s, "true") == 0 || strcmp(s, "1") == 0 ||
               strcmp(s, "on") == 0 || strcmp(s, "yes") == 0;
    }
    return cc_json_bool_value(value) || cc_json_int_value(value) != 0;
}

/**
 * config_required_allocs_ok — 检查默认配置填充后的关键字符串字段是否都已成功分配。
 *
 * 位置：核心数据模型层。注释重点说明当前函数的输入输出、资源边界和错误传播。
 *
 * @param config 只读配置对象；函数读取字段但不保存 config 指针。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
static cc_result_t config_required_allocs_ok(const cc_config_t *config)
{
    if (!config->provider || !config->model || !config->base_url ||
        !config->storage_type || !config->data_dir || !config->storage_path ||
        !config->workspace_path || !config->sandbox_type ||
        !config->memory_backend || !config->memory_path ||
        !config->plugin_config_path ||
        !config->soul_file || !config->user_file) {
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate config defaults");
    }
    return cc_result_ok();
}

/*
 * cc_config_load_default — 加载默认配置
 *
 * 功能：用硬编码的默认值填充 cc_config_t 结构体。
 *       所有字符串字段通过 strdup 在堆上分配，确保与 cc_config_load 的内存管理策略一致。
 *
 * 参数：
 *   out_config — 输出参数，指向待填充的 cc_config_t 结构体
 *
 * 返回值：
 *   cc_result_ok() — 填充成功（默认配置总是成功，不会失败）
 *
 * 默认值概要：
 *   - AI Provider: 按编译 profile 优先选择 openai / anthropic / ollama
 *   - Storage: sqlite / ./data/c-claw.db
 *   - Workspace: ./workspace（Agent 执行任务的临时文件目录）
 *   - Sandbox: local / shell_requires_approval=true / timeout=30000ms
 *   - Agent: max_steps=10（每轮对话最大工具调用步数）
 */
cc_result_t cc_config_load_default(cc_config_t *out_config)
{
    memset(out_config, 0, sizeof(cc_config_t));
    out_config->provider = strdup(CC_DEFAULT_PROVIDER);
    out_config->model = strdup(CC_DEFAULT_MODEL);
    out_config->base_url = strdup(CC_DEFAULT_BASE_URL);
    out_config->storage_type = strdup(CC_DEFAULT_STORAGE_TYPE);
    out_config->data_dir = strdup(CC_DEFAULT_DATA_DIR);
    out_config->storage_path = strdup(CC_DEFAULT_STORAGE_PATH);
    out_config->workspace_path = strdup(CC_DEFAULT_WORKSPACE_PATH);
    out_config->sandbox_type = strdup("local");
    out_config->shell_requires_approval = 1;
    out_config->sandbox_timeout_ms = 30000;
    out_config->max_steps = 10;
    out_config->stream_mode = 0;
    out_config->debug_mode = 0;
    out_config->memory_backend = strdup(CC_DEFAULT_MEMORY_BACKEND);
    out_config->memory_path = strdup(CC_DEFAULT_MEMORY_PATH);
    out_config->plugin_config_path = strdup(CC_DEFAULT_PLUGIN_CONFIG_PATH);
    out_config->soul_file = strdup(CC_DEFAULT_SOUL_FILE);
    out_config->user_file = strdup(CC_DEFAULT_USER_FILE);
    out_config->context_window_tokens = 8192;
    out_config->context_compress_threshold = 0.8;
    out_config->context_keep_recent = 20;
    cc_result_t rc = config_required_allocs_ok(out_config);
    if (rc.code != CC_OK) {
        cc_config_destroy(out_config);
        return rc;
    }
    return cc_result_ok();
}

/*
 * cc_config_load — 从 JSON 文件加载配置（带默认值 fallback）
 *
 * 功能：尝试从指定路径的 JSON 文件读取配置。如果文件读取失败（如文件不存在）
 *       或 JSON 解析失败，则静默回退到默认配置。配置文件中的每个字段都是可选的，
 *       缺失的字段使用对应的默认值。
 *
 * 配置 JSON 结构示例：
 *   {
 *     "model":    { "provider": "...", "model": "...", "base_url": "...", "api_key": "..." },
 *     "storage":  { "type": "...", "path": "..." },
 *     "workspace":{ "path": "..." },
 *     "sandbox":  { "type": "...", "shell_requires_approval": true, "timeout_ms": 30000 },
 *     "tools":    { "enabled": ["tool_a", "tool_b"] }
 *   }
 *
 * 参数：
 *   path       — JSON 配置文件的路径（若为 NULL，直接使用默认配置）
 *   out_config — 输出参数，指向待填充的 cc_config_t 结构体
 *
 * 返回值：
 *   始终返回 cc_result_ok()（容错设计，不因配置问题中断程序启动）
 *
 * 设计决策：
 *   - 容错优先（fail-safe）：配置文件损坏或缺失时程序仍可运行，方便开发调试
 *   - 段式解析：按 model/storage/workspace/sandbox/tools 分段处理，
 *     每段独立 fallback，一个段缺失不影响其他段
 *   - tools.enabled 数组：支持按名称动态启用/禁用工具，解析为字符串数组
 *
 * 逐字段解析逻辑详解：
 *
 * 【model 段】— LLM/AI 模型配置
 *   - provider:    JSON "model.provider" 字符串 → out_config->provider
 *                  默认值: 按编译 profile 选择首个可用 provider
 *   - model:       JSON "model.model" 字符串 → out_config->model
 *                  默认值: 随 provider 变化
 *   - base_url:    JSON "model.base_url" 字符串 → out_config->base_url
 *                  默认值: 随 provider 变化
 *   - api_key:     JSON "model.api_key" 字符串 → out_config->api_key
 *                  默认值: NULL（本地模型无需密钥；远程模型需要配置）
 *   - thinking_mode: JSON "model.thinking_mode" 整数 → out_config->thinking_mode
 *                  默认值: 0（禁用思考模式）
 *   解析逻辑：先尝试 cc_json_object_get(root, "model")，若 model 段存在，
 *   则逐字段读取；若字段不存在或类型不匹配，使用默认值。若整个 model 段
 *   不存在，全部字段设为默认值，且 api_key 和 thinking_mode 不设置（保持 NULL/0）。
 *
 * 【storage 段】— 存储后端配置
 *   - type:        JSON "storage.type" 字符串 → out_config->storage_type
 *                  默认值: "sqlite"（SQLite 本地数据库）
 *   - path:        JSON "storage.path" 字符串 → out_config->storage_path
 *                  默认值: profile SQLite path
 *   解析逻辑：同 model 段，段不存在时全部设为默认值。
 *
 * 【workspace 段】— 工作区配置
 *   - path:        JSON "workspace.path" 字符串 → out_config->workspace_path
 *                  默认值: profile workspace path
 *                  用途：Agent 执行任务时的临时文件存放目录
 *   解析逻辑：同 model 段，单字段段，段不存在时使用默认值。
 *
 * 【sandbox 段】— 沙箱执行环境配置
 *   - type:                   JSON "sandbox.type" 字符串 → out_config->sandbox_type
 *                             默认值: "local"（本地沙箱模式）
 *   - shell_requires_approval: JSON "sandbox.shell_requires_approval" 布尔 → out_config->shell_requires_approval
 *                             默认值: 1（需要用户确认，安全优先）
 *   - timeout_ms:             JSON "sandbox.timeout_ms" 整数 → out_config->sandbox_timeout_ms
 *                             默认值: 30000（30 秒超时）
 *   解析逻辑：布尔值通过 cc_json_bool_value() 读取，整数值通过 cc_json_int_value() 读取。
 *   类型不匹配时函数返回 0，因此字段缺失时使用默认值 1/30000。
 *
 * 【tools 段】— 工具启用/禁用配置
 *   - enabled:     JSON "tools.enabled" 字符串数组 → out_config->enabled_tools + enabled_tools_count
 *                  默认值: NULL 数组，count=0（全部工具禁用，或按模块内部默认逻辑）
 *   解析逻辑：先检查 "tools.enabled" 是否为 JSON 数组类型（cc_json_is_array），
 *   若为数组，通过 cc_json_array_size 获取长度，遍历每个元素，
 *   通过 cc_json_array_get + cc_json_string_value 获取工具名称并通过 strdup 复制。
 *   若 tools 段或 enabled 字段不存在或不是数组，enabled_tools 保持为 NULL。
 */
cc_result_t cc_config_load(const char *path, cc_config_t *out_config)
{
    cc_result_t rc = cc_config_load_default(out_config);
    if (rc.code != CC_OK || !path) return rc;

    cc_json_value_t *root = NULL;
    rc = cc_json_parse_from_file(path, &root);
    if (rc.code != CC_OK) {
        return rc;
    }

    cc_json_value_t *model = cc_json_object_get(root, "model");
    if (model) {
        const char *s = json_string_field(model, "provider");
        if (s && !replace_string(&out_config->provider, s)) goto oom;
        s = json_string_field(model, "model");
        if (s && !replace_string(&out_config->model, s)) goto oom;
        s = json_string_field(model, "base_url");
        if (s && !replace_string(&out_config->base_url, s)) goto oom;
        s = json_string_field(model, "api_key");
        if (s && !replace_string(&out_config->api_key, s)) goto oom;
        cc_json_value_t *tm = cc_json_object_get(model, "thinking_mode");
        out_config->thinking_mode = tm ? cc_json_int_value(tm) : 0;
        cc_json_value_t *sm = cc_json_object_get(model, "stream_mode");
        if (sm) out_config->stream_mode = json_boolish_value(sm);
    }

    cc_json_value_t *storage = cc_json_object_get(root, "storage");
    if (storage) {
        const char *s = json_string_field(storage, "type");
        if (s && !replace_string(&out_config->storage_type, s)) goto oom;
        s = json_string_field(storage, "data_dir");
        if (s && !replace_string(&out_config->data_dir, s)) goto oom;
        s = json_string_field(storage, "path");
        if (s && !replace_string(&out_config->storage_path, s)) goto oom;
    }

    cc_json_value_t *workspace = cc_json_object_get(root, "workspace");
    if (workspace) {
        const char *s = json_string_field(workspace, "path");
        if (s && !replace_string(&out_config->workspace_path, s)) goto oom;
    }

    cc_json_value_t *sandbox = cc_json_object_get(root, "sandbox");
    if (sandbox) {
        const char *s = json_string_field(sandbox, "type");
        if (s && !replace_string(&out_config->sandbox_type, s)) goto oom;
        cc_json_value_t *v = cc_json_object_get(sandbox, "shell_requires_approval");
        out_config->shell_requires_approval = v ? cc_json_bool_value(v) : 1;
        v = cc_json_object_get(sandbox, "timeout_ms");
        out_config->sandbox_timeout_ms = v ? cc_json_int_value(v) : 30000;
    }

    /*
     * ────────────────────────────────────────────────────────────
     * 【memory 段】— 长期记忆后端配置
     * ────────────────────────────────────────────────────────────
     *
     * 这部分配置控制长期记忆（Long-Term Memory）子系统的存储后端。
     * 长期记忆独立于会话存储（session store），用于跨会话持久化用户偏好、
     * 项目上下文、重要事实等。
     *
     * 解析逻辑：
     *   如果 JSON 中存在 "memory" 段，从中读取 backend 和 path 字段。
     *   如果段不存在或字段缺失，使用默认值：
     *     - memory_backend: "json_file"（JSON 文件后端，简单可靠）
     *     - memory_path: profile memory path（记忆数据的持久化文件路径）
     *
     * Memory backend 的可选值：
     *   - "json_file":  基于 JSON 文件的简单后端，所有记忆序列化为一个 JSON 文件
     *   - "sqlite":     基于 SQLite 的后端，支持更复杂的查询和更大的记忆数量
     *   - "none":       禁用长期记忆功能
     *
     * 为什么默认使用 JSON 文件后端：
     *   JSON 文件是最简单的持久化方案——无需数据库依赖，用户可以直接查看和
     *   编辑记忆文件。适合记忆数量较少（< 100 条）的场景。对于更大规模的
     *   记忆管理，可通过配置切换到 SQLite 后端。
     */
    cc_json_value_t *memory = cc_json_object_get(root, "memory");
    if (memory) {
        const char *s = json_string_field(memory, "backend");
        if (s && !replace_string(&out_config->memory_backend, s)) goto oom;
        s = json_string_field(memory, "path");
        if (s && !replace_string(&out_config->memory_path, s)) goto oom;
    }

    cc_json_value_t *tools = cc_json_object_get(root, "tools");
    if (tools) {
        const char *plugins_path = json_string_field(tools, "plugin_config_path");
        if (plugins_path && !replace_string(&out_config->plugin_config_path, plugins_path)) goto oom;

        cc_json_value_t *enabled = cc_json_object_get(tools, "enabled");
        if (enabled && cc_json_is_array(enabled)) {
            int count = cc_json_array_size(enabled);
            size_t string_count = 0;
            for (int i = 0; i < count; i++) {
                cc_json_value_t *item = cc_json_array_get(enabled, i);
                const char *name = cc_json_string_value(item);
                if (name) string_count++;
            }
            char **tools_list = string_count ? calloc(string_count, sizeof(char *)) : NULL;
            if (string_count && !tools_list) goto oom;

            size_t write_i = 0;
            for (int i = 0; i < count; i++) {
                const char *name = cc_json_string_value(cc_json_array_get(enabled, i));
                if (!name) continue;
                tools_list[write_i] = strdup(name);
                if (!tools_list[write_i]) {
                    for (size_t j = 0; j < write_i; j++) free(tools_list[j]);
                    free(tools_list);
                    goto oom;
                }
                write_i++;
            }
            for (size_t i = 0; i < out_config->enabled_tools_count; i++)
                free(out_config->enabled_tools[i]);
            free(out_config->enabled_tools);
            out_config->enabled_tools = tools_list;
            out_config->enabled_tools_count = string_count;
        }
    }

    cc_json_value_t *system = cc_json_object_get(root, "system");
    if (system) {
        const char *s = json_string_field(system, "soul_file");
        if (s && !replace_string(&out_config->soul_file, s)) goto oom;
        s = json_string_field(system, "user_file");
        if (s && !replace_string(&out_config->user_file, s)) goto oom;
        s = json_string_field(system, "system_prompt");
        if (s && !replace_string(&out_config->system_prompt, s)) goto oom;
        cc_json_value_t *v = cc_json_object_get(system, "max_steps");
        out_config->max_steps = v ? cc_json_int_value(v) : 10;

        v = cc_json_object_get(system, "debug");
        if (v) out_config->debug_mode = json_boolish_value(v);
        v = cc_json_object_get(system, "debug_mode");
        if (v) out_config->debug_mode = json_boolish_value(v);

        /*
         * ────────────────────────────────────────────────────────────
         * 上下文窗口管理配置（用于 cc_context_builder 动态截断/压缩）
         * ────────────────────────────────────────────────────────────
         *
         * context_window_tokens:
         *   LLM 上下文窗口的总 token 预算。
         *   默认 8192，兼容 8K 窗口的模型（如多数开源模型）。
         *   对于 32K/128K 窗口模型可调大此值。
         *   设为 0 则关闭动态截断，退化为加载全部历史消息。
         *
         * context_compress_threshold:
         *   JSON 中存储为整数（如 80 表示 80%），解析时除以 100 转为 0.8。
         *   当上下文使用率超过此阈值时，触发 LLM 摘要压缩旧消息。
         *   默认 0.8（80%），设为 0 则禁用压缩，仅做硬截断。
         *
         * context_keep_recent:
         *   压缩时保留最近 N 条原始消息不被压缩。
         *   默认 20 条，确保最近对话的完整连贯性。
         */
        v = cc_json_object_get(system, "context_window_tokens");
        out_config->context_window_tokens = v ? cc_json_int_value(v) : 8192;
        v = cc_json_object_get(system, "context_compress_threshold");
        out_config->context_compress_threshold = v ? (double)cc_json_int_value(v) / 100.0 : 0.8;
        v = cc_json_object_get(system, "context_keep_recent");
        out_config->context_keep_recent = v ? cc_json_int_value(v) : 20;
    }

    cc_json_value_t *cli = cc_json_object_get(root, "cli");
    if (cli) {
        cc_json_value_t *v = cc_json_object_get(cli, "debug_mode");
        if (v) out_config->debug_mode = json_boolish_value(v);
        v = cc_json_object_get(cli, "debug");
        if (v) out_config->debug_mode = json_boolish_value(v);
    }

    if (out_config->thinking_mode) {
        out_config->stream_mode = 1;
    }

    cc_json_destroy(root);
    return cc_result_ok();

oom:
    cc_json_destroy(root);
    return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate config field");
}

/*
 * cc_config_destroy — 释放配置结构体的所有动态内存
 *
 * 功能：释放 cc_config_t 中所有通过 strdup/calloc 动态分配的字段，
 *       包括字符串字段、enabled_tools 数组和 plugin_commands 数组。
 *       释放后将结构体清零（memset 为 0），防止悬垂指针。
 *
 * 参数：
 *   config — 指向待销毁的 cc_config_t 结构体（可为 NULL，为 NULL 时静默返回）
 *
 * 返回值：无
 *
 * 注意事项：
 *   - 此函数释放所有字段但不会释放 config 指针本身（config 通常为栈变量）。
 *   - 调用此函数后不可再使用 config 中的任何字段值。
 *   - plugin_commands 字段由外部加载逻辑（非 cc_config_load）填充，此处一并清理。
 */
void cc_config_destroy(cc_config_t *config)
{
    if (!config) return;
    free(config->provider);
    free(config->model);
    free(config->base_url);
    free(config->api_key);
    free(config->storage_type);
    free(config->data_dir);
    free(config->storage_path);
    free(config->workspace_path);
    free(config->sandbox_type);
    free(config->memory_backend);
    free(config->memory_path);
    free(config->plugin_config_path);
    free(config->system_prompt);
    free(config->soul_file);
    free(config->user_file);
    for (size_t i = 0; i < config->enabled_tools_count; i++) {
        free(config->enabled_tools[i]);
    }
    free(config->enabled_tools);
    for (size_t i = 0; i < config->plugin_count; i++) {
        free(config->plugin_commands[i]);
    }
    free(config->plugin_commands);
    memset(config, 0, sizeof(cc_config_t));
}

/**
 * cc_config_build_system_prompt — 构建 Agent 的完整 system prompt
 *
 * 功能：
 *   根据配置中的 system_prompt、soul_file 和 user_file 字段，
 *   按优先级构建 Agent 的系统提示词文本。
 *
 * 构建优先级（三级 fallback）：
 * ──────────────────────────
 *   Level 1: 如果 config->system_prompt 在 JSON 中直接指定（非空），
 *            直接返回该字符串（不再读取 soul.md 和 user.md）。
 *            WHY：JSON 中直接指定的 system_prompt 是用户明确配置的，
 *            具有最高优先级，应覆盖文件方式。
 *
 *   Level 2: 如果 config->system_prompt 为空（未在 JSON 中配置），
 *            则从以下两个文件中读取内容并拼接：
 *               - soul.md：定义 Agent 的"灵魂"/角色/身份
 *                 （如"你是一个专业的 C 语言编程助手"）
 *               - user.md：定义用户偏好和个性化设定
 *                 （如"用户喜欢简洁的回答，用中文回复"）
 *            拼接顺序：soul.md + "\n\n" + user.md + "\n\n"
 *            WHY soul.md 在前 user.md 在后：
 *              soul.md 定义 Agent 的核心身份（类似于"你是谁"），
 *              user.md 定义辅助偏好（类似于"你该怎么服务"），
 *              先确定身份再确定服务方式，更符合逻辑层次。
 *
 *   Level 3: 如果 soul.md 和 user.md 都不存在或为空，
 *            返回硬编码的默认 prompt：
 *            "You are a helpful AI assistant. Use tools to help the user."
 *
 * soul.md 与 user.md 的设计哲学：
 * ────────────────────────────────
 *   soul.md（灵魂文件）：
 *     - 定义 Agent 的核心身份和行为准则
 *     - 类似于 LLM 的"角色系统提示词"
 *     - 示例内容：
 *       "你是一位资深的 Linux 系统管理员助手。
 *        你的职责是帮助用户诊断和解决系统问题。
 *        你应该先分析问题根因，再提出解决方案。"
 *
 *   user.md（用户文件）：
 *     - 定义用户特定的偏好和约束
 *     - 类似于 LLM 的"用户个性化提示词"
 *     - 与 soul.md 分离的好处：用户可以在不修改 Agent 角色的情况下
 *       只调整个性化设置
 *     - 示例内容：
 *       "请始终用中文回复。
 *        如果涉及系统命令，请先解释命令的作用再执行。"
 *
 * 为什么采用文件方式（soul.md / user.md）：
 * ──────────────────────────────────────────
 *   1. 编辑便利性：Markdown 文件比嵌入在 JSON 中的字符串更容易编辑，
 *      支持编辑器的语法高亮和多行编辑。
 *
 *   2. 关注点分离：soul.md 和 user.md 分开管理，Agent 开发者可以只修改
 *      soul.md 来调整 Agent 行为，终端用户可以只修改 user.md 来定制体验。
 *
 *   3. 版本控制友好：Markdown 文件的 diff 比 JSON 中的长字符串更清晰，
 *      便于代码审查和版本追踪。
 *
 * 内存管理：
 *   - 如果直接使用 config->system_prompt，通过 strdup 深拷贝
 *     （调用方负责 free *out_prompt）
 *   - 如果从文件读取，通过 cc_string_builder_take 转移所有权
 *   - 如果使用默认 prompt，通过 strdup 分配
 *   - 所有路径都确保 *out_prompt 是调用方可 free 的堆内存
 *
 * 错误处理：
 *   所有路径都保证返回一个有效的 system prompt。
 *   即使 soul.md 和 user.md 都不存在，也会返回默认的 prompt。
 *   这是"开箱即用"设计的体现——用户不需要创建任何文件就能启动 Agent。
 *
 * @param config     配置结构体，包含 system_prompt/soul_file/user_file 字段
 * @param out_prompt 输出参数，构建完成的 system prompt 字符串（调用方负责 free）
 * @return CC_OK 始终成功（总是能构建一个有效的 prompt）
 */
cc_result_t cc_config_build_system_prompt(const cc_config_t *config, char **out_prompt)
{
    *out_prompt = NULL;

    /*
     * Level 1: JSON 中直接指定了 system_prompt
     *
     * 这是最高优先级的 prompt 来源。
     * 用户可以在配置文件中直接写 system_prompt 字段来完全控制 Agent 的行为，
     * 此时忽略 soul.md 和 user.md 文件。
     *
     * 使用 strdup 深拷贝以确保调用方获得独立的内存块。
     */
    if (config->system_prompt && strlen(config->system_prompt) > 0) {
        *out_prompt = strdup(config->system_prompt);
        return cc_result_ok();
    }

    /*
     * Level 2: 从 soul.md 和 user.md 文件构建 prompt
     *
     * 使用 cc_string_builder 高效拼接两个文件的内容。
     * 每个文件通过 read_text_file 读取，如果文件不存在则返回 NULL 并被跳过。
     *
     * 拼接格式：
     *   <soul.md 内容>
     *
     *   <user.md 内容>
     *
     * 两个文件之间用 "\n\n" 分隔，确保内容独立成段。
     */
    cc_string_builder_t sb;
    cc_string_builder_init(&sb);

    char *soul_content = read_text_file(config->soul_file);
    if (soul_content && strlen(soul_content) > 0) {
        cc_string_builder_append(&sb, soul_content);
        cc_string_builder_append(&sb, "\n\n");
    }
    free(soul_content);

    char *user_content = read_text_file(config->user_file);
    if (user_content && strlen(user_content) > 0) {
        cc_string_builder_append(&sb, user_content);
        cc_string_builder_append(&sb, "\n\n");
    }
    free(user_content);

    /*
     * 检查文件拼接结果
     *
     * 如果至少有一个文件提供了内容，cc_string_builder_take 会将
     * 拼接后的完整字符串转移给 *out_prompt。
     *
     * 零拷贝语义：take() 直接返回 sb 的内部缓冲区指针，
     * 不产生额外的 strdup/memcpy 开销。
     */
    char *combined = cc_string_builder_take(&sb);
    if (combined && strlen(combined) > 0) {
        *out_prompt = combined;
        return cc_result_ok();
    }
    free(combined);

    /*
     * Level 3: 硬编码默认 prompt
     *
     * 当所有文件都不存在或为空时，使用此预设的默认 prompt。
     * 这是最后的 fallback，确保 Agent 始终有一个最小的系统提示词。
     *
     * 默认 prompt 的设计意图：
     *   - "helpful AI assistant"：明确 Agent 的角色是"有帮助的助手"
     *   - "Use tools to help the user"：引导 LLM 使用注册的工具来完成任务
     *     这句话对于 tool-use Agent 很重要，如果不提示可能 LLM 默认不会
     *     主动使用工具。
     */
    *out_prompt = strdup("You are a helpful AI assistant. Use tools to help the user.");
    return cc_result_ok();
}
