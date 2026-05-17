/**
 * 学习导读：cclaw/adapters/src/tools/common/cc_memory_tool.c
 *
 * 所属层次：适配器层。
 * 阅读重点：这里把端口接口落到具体后端，阅读时重点看协议转换、资源释放和失败降级。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/*
 * cc_memory_tool.c — memory 工具适配器
 *
 * 模块说明：
 *   本文件实现了 "memory" 工具的适配器（Adapter）。
 *   将底层 cc_memory_store_t 持久化存储接口适配为 cc_tool vtable 接口，
 *   使 LLM 可通过统一工具接口进行长期记忆的增删改查（CRUD）操作。
 *
 * 设计模式：Adapter（适配器）模式
 *   将记忆存储能力适配为标准工具接口，LLM 调用时感知为普通的 function calling。
 *
 * 实现接口：
 *   - cc_tool_vtable_t（5 个虚拟方法：name / description / schema_json / call / destroy）
 *
 * 支持的操作：
 *   - store：存储一条记忆（key + value + category）
 *   - recall：模糊搜索记忆（按 query 匹配 key/value/category）
 *   - forget：按 key 删除单条记忆
 *   - list：列出所有记忆（可按 category 过滤）
 *
 * 安全约束：
 *   - key 最长 256 字符，value 最长 4096 字符，防止内存溢出
 *   - 依赖注入的 cc_memory_store_t 管理实际持久化策略
 */

#include "cc/ports/cc_tool.h"
#include "cc/ports/cc_memory_store.h"
#include "cc/core/cc_tool_call.h"
#include "cc/util/cc_json.h"
#include "cc/util/cc_string_builder.h"
#include <stdlib.h>
#include <string.h>

/*
 * cc_memory_tool_t — memory 工具的内部数据结构
 *
 * 字段说明：
 *   store — 底层记忆存储后端实例（依赖注入），
 *           可以是 SQLite、JSON 文件或内存后端
 */
typedef struct {
    cc_memory_store_t *store;
} cc_memory_tool_t;

/*
 * memory_name — 返回工具名称
 *
 * 功能：返回该工具在工具注册表中的唯一标识名称。
 * 参数：self — 工具实例指针（本函数未使用）
 * 返回值：工具名称字符串 "memory"
 */
static const char *memory_name(void *self) { (void)self; return "memory"; }

/*
 * memory_description — 返回工具描述
 *
 * 功能：返回工具的自然语言描述，供 LLM 理解工具用途。
 *       明确列出四种操作及其用法，帮助模型正确构造 tool call 参数。
 * 参数：self — 工具实例指针（本函数未使用）
 * 返回值：工具描述字符串，列出 store/recall/forget/list 操作
 */
static const char *memory_description(void *self)
{
    (void)self;
    return "Long-term memory storage. Use to remember important facts across sessions. "
           "Operations: store(key, value, category), recall(query), forget(key), list()";
}

/*
 * memory_schema_json — 返回工具参数的 JSON Schema
 *
 * 功能：定义工具调用时必须/可选的参数及其类型，符合 JSON Schema 规范。
 *       LLM 根据此 Schema 知道如何构造参数 JSON。
 *
 * 参数说明：
 *   - operation（必填）：枚举值 store/recall/forget/list，决定执行何种操作
 *   - key：记忆键名（store/forget 时必填）
 *   - value：记忆值（store 时必填）
 *   - category：分类标签（store 时可选），建议使用 user_pref/project_info/lesson/rule
 *   - query：搜索关键词（recall 时使用，为空则返回所有）
 */
static const char *memory_schema_json(void *self)
{
    (void)self;
    return "{"
        "\"type\":\"object\","
        "\"properties\":{"
            "\"operation\":{\"type\":\"string\",\"enum\":[\"store\",\"recall\",\"forget\",\"list\"],"
                "\"description\":\"store=save fact, recall=search, forget=delete, list=show all\"},"
            "\"key\":{\"type\":\"string\",\"description\":\"Memory key (for store/forget)\"},"
            "\"value\":{\"type\":\"string\",\"description\":\"Memory value (for store)\"},"
            "\"category\":{\"type\":\"string\",\"description\":\"Category: user_pref, project_info, lesson, rule\"},"
            "\"query\":{\"type\":\"string\",\"description\":\"Search query (for recall)\"}"
        "},"
        "\"required\":[\"operation\"]"
    "}";
}

/*
 * memory_call — 执行记忆操作（CRUD 核心入口）
 *
 * 功能：根据 operation 参数路由到不同的记忆操作分支：
 *   1. 解析 JSON 参数，提取 operation 字段
 *   2. 根据 operation 分发到 store/recall/forget/list 分支
 *   3. 格式化结果并填充 out_result
 *
 * 各操作的详细逻辑：
 *   store  —— 校验 key/value 长度限制（256/4096 字符），
 *            调用 cc_memory_store_set 持久化存储，
 *            自动关联当前 session_id
 *   recall —— 调用 cc_memory_store_search 进行 LIKE 模糊匹配，
 *            最多返回 20 条结果，格式化输出 key/value/category
 *   forget —— 按 key 精确匹配并删除，调用 cc_memory_store_delete
 *   list   —— 按 category 过滤（NULL 则全部），最多返回 50 条，
 *            格式化输出所有匹配的记忆
 *
 * 参数：
 *   self      — 工具实例指针
 *   args_json — JSON 格式的调用参数（必须包含 "operation" 字段）
 *   ctx       — 工具上下文，包含 session_id（store 时自动关联）
 *   out_result— 输出结果结构体，包含 ok/content/error 字段
 *
 * 返回值：cc_result_t，函数层面始终返回 OK
 *         （操作失败通过 out_result->ok=0 表达，不中断上层流程）
 */
static cc_result_t memory_call(void *self, const char *args_json,
                                const cc_tool_context_t *ctx,
                                cc_tool_result_t *out_result)
{
    cc_memory_tool_t *tool = (cc_memory_tool_t *)self;

    cc_json_value_t *params = NULL;
    cc_result_t rc = cc_json_parse(args_json, &params);
    if (rc.code != CC_OK || !params) {
        out_result->ok = 0;
        out_result->error = strdup("Failed to parse arguments JSON");
        cc_result_free(&rc);
        return cc_result_ok();
    }

    const char *op = cc_json_string_value(cc_json_object_get(params, "operation"));

    if (!op) {
        out_result->ok = 0;
        out_result->error = strdup("Missing required field: 'operation'");
        cc_json_destroy(params);
        return cc_result_ok();
    }

    cc_string_builder_t sb;
    cc_string_builder_init(&sb);

    if (strcmp(op, "store") == 0) {
        const char *key = cc_json_string_value(cc_json_object_get(params, "key"));
        const char *value = cc_json_string_value(cc_json_object_get(params, "value"));
        const char *category = cc_json_string_value(cc_json_object_get(params, "category"));

        if (!key || !value) {
            out_result->ok = 0;
            out_result->error = strdup("'key' and 'value' are required for store");
            cc_json_destroy(params);
            return cc_result_ok();
        }

        if (strlen(key) > 256 || strlen(value) > 4096) {
            out_result->ok = 0;
            out_result->error = strdup("key max 256 chars, value max 4096 chars");
            cc_json_destroy(params);
            return cc_result_ok();
        }

        cc_result_t set_rc = cc_memory_store_set(tool->store, key, value, category,
                                                   ctx->session_id);
        if (set_rc.code == CC_OK)
            cc_string_builder_appendf(&sb, "Memory stored: %s = %s", key, value);
        else
            cc_string_builder_appendf(&sb, "Failed to store memory: %s", set_rc.message);
        cc_result_free(&set_rc);

    } else if (strcmp(op, "recall") == 0) {
        const char *query = cc_json_string_value(cc_json_object_get(params, "query"));
        if (!query) query = "";

        cc_memory_entry_t *entries = NULL;
        size_t count = 0;
        cc_result_t search_rc = cc_memory_store_search(tool->store, query, 20, &entries, &count);
        if (search_rc.code != CC_OK || count == 0) {
            cc_string_builder_append(&sb, "No matching memories found.");
        } else {
            cc_string_builder_appendf(&sb, "Found %zu memories:\n", count);
            for (size_t i = 0; i < count; i++) {
                cc_string_builder_appendf(&sb, "- %s: %s", entries[i].key, entries[i].value);
                if (entries[i].category)
                    cc_string_builder_appendf(&sb, " [%s]", entries[i].category);
                cc_string_builder_append(&sb, "\n");
            }
            cc_memory_entry_free_array(entries, count);
        }
        cc_result_free(&search_rc);

    } else if (strcmp(op, "forget") == 0) {
        const char *key = cc_json_string_value(cc_json_object_get(params, "key"));
        if (!key) {
            out_result->ok = 0;
            out_result->error = strdup("'key' is required for forget");
            cc_json_destroy(params);
            return cc_result_ok();
        }

        cc_result_t del_rc = cc_memory_store_delete(tool->store, key);
        if (del_rc.code == CC_OK)
            cc_string_builder_appendf(&sb, "Memory forgotten: %s", key);
        else
            cc_string_builder_appendf(&sb, "No memory found with key: %s", key);
        cc_result_free(&del_rc);

    } else if (strcmp(op, "list") == 0) {
        const char *category = cc_json_string_value(cc_json_object_get(params, "category"));

        cc_memory_entry_t *entries = NULL;
        size_t count = 0;
        cc_result_t list_rc = cc_memory_store_list(tool->store, category, 50, &entries, &count);
        if (list_rc.code != CC_OK || count == 0) {
            cc_string_builder_append(&sb, "No memories found.");
        } else {
            cc_string_builder_appendf(&sb, "All memories (%zu total):\n", count);
            for (size_t i = 0; i < count; i++) {
                cc_string_builder_appendf(&sb, "- %s: %s", entries[i].key, entries[i].value);
                if (entries[i].category)
                    cc_string_builder_appendf(&sb, " [%s]", entries[i].category);
                cc_string_builder_append(&sb, "\n");
            }
            cc_memory_entry_free_array(entries, count);
        }
        cc_result_free(&list_rc);

    } else {
        cc_string_builder_appendf(&sb, "Unknown operation: %s. Use store/recall/forget/list", op);
    }

    cc_json_destroy(params);
    out_result->ok = 1;
    out_result->content = cc_string_builder_take(&sb);
    return cc_result_ok();
}

/*
 * memory_destroy — 销毁 memory 工具实例
 *
 * 功能：释放工具实例内存。注意不释放 store（store 生命周期由外部管理）。
 * 参数：self — 工具实例指针
 */
static void memory_destroy(void *self)
{
    cc_memory_tool_t *tool = (cc_memory_tool_t *)self;
    if (!tool) return;
    free(tool);
}

/*
 * cc_memory_tool_create — 创建 memory 工具实例（工厂函数）
 *
 * 功能：
 *   1. 分配并零初始化 cc_memory_tool_t 结构体
 *   2. 通过依赖注入绑定 cc_memory_store_t 后端
 *   3. 在函数内构造 vtable（静态局部变量，所有实例共享同一份 vtable）
 *   4. 填充 cc_tool_t 输出参数
 *
 * 参数：
 *   store    — 底层记忆存储后端（依赖注入，生命期由调用者管理）
 *   out_tool — 输出参数，创建成功后包含工具 self 指针和 vtable
 *
 * 返回值：cc_result_t，成功返回 CC_OK，内存不足返回 CC_ERR_OUT_OF_MEMORY
 *
 * 设计决策：
 *   vtable 使用 static 局部变量实现，确保所有 memory 工具实例共享同一虚函数表，
 *   减少每个实例的内存占用，同时保持 vtable 地址稳定（不会被销毁）。
 */
cc_result_t cc_memory_tool_create(cc_memory_store_t *store, cc_tool_t *out_tool)
{
    cc_memory_tool_t *tool = calloc(1, sizeof(cc_memory_tool_t));
    if (!tool) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate memory tool");

    tool->store = store;

    static cc_tool_vtable_t vtable;
    vtable.name = memory_name;
    vtable.description = memory_description;
    vtable.schema_json = memory_schema_json;
    vtable.call = memory_call;
    vtable.destroy = memory_destroy;

    out_tool->self = tool;
    out_tool->vtable = &vtable;
    return cc_result_ok();
}