/**
 * 学习导读：apps/windows/cli/src/plugin/cc_plugin_manager.c
 *
 * 所属层次：Windows CLI 应用层。
 * 阅读重点：这里镜像桌面 CLI 能力但使用 Windows 平台实现，阅读时重点比较与 POSIX 版本的差异。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/******************************************************************************
 * cc_plugin_manager.c — 插件管理器
 *
 * 本模块负责外部插件的完整生命周期管理。通过解析 JSON 配置文件，
 * 为每个插件启动独立的子进程，并将其暴露的工具注册到工具注册表中。
 *
 * 架构角色：
 *   cc_plugin_manager 是插件系统的"门面"（Facade），对外提供统一的
 *   加载接口。main.c 只需调用 cc_plugin_manager_load_plugins() 即可
 *   加载所有外部插件，无需关心内部实现细节。
 *
 * 插件配置格式（plugins.json）：
 *   {
 *     "plugins": [
 *       {
 *         "name": "weather",
 *         "command": "python3",
 *         "args": ["apps/windows/cli/plugins/weather_tool.py"],
 *         "tools": [
 *           {
 *             "name": "weather_query",
 *             "description": "查询城市天气",
 *             "parameters": { ...JSON Schema... }
 *           }
 *         ]
 *       }
 *     ]
 *   }
 *
 * 一个插件进程可以暴露多个工具，通过不同的 method 名称区分。
 * 所有工具共享同一个子进程和管道。
 *
 * 错误处理：
 *   - 插件进程启动失败：跳过该插件，继续加载其他插件
 *   - 工具注册失败：跳过该工具，继续处理同插件的其他工具
 *****************************************************************************/

#include "cc/plugin/cc_plugin_manager.h"
#include "cc/plugin/cc_plugin_process.h"
#include "cc/util/cc_json.h"
#include "cc/ports/cc_tool.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/**
 * cc_plugin_entry_t — 单个插件条目，拥有插件名并记录该插件注册出的工具数量。
 *
 * 资源约定：动态缓冲区由该结构拥有；借用指针只在所属调用链有效，count/capacity 字段必须同步维护。
 */
typedef struct {
    char *name;
    cc_plugin_process_t *process;
    int tool_count;
} cc_plugin_entry_t;

/**
 * cc_plugin_manager — 插件管理器状态，拥有插件进程、插件工具条目和注册时产生的资源。
 *
 * 资源约定：动态缓冲区由该结构拥有；借用指针只在所属调用链有效，count/capacity 字段必须同步维护。
 */
struct cc_plugin_manager {
    cc_plugin_entry_t *entries;
    int entry_count;
};

/**
 * cc_plugin_manager_create — 创建、启动或加载组件资源，并把错误统一传播给调用方。
 *
 * 位置：插件/JSON-RPC 子系统。注释重点说明当前函数的输入输出、资源边界和错误传播。
 *
 * @param out_manager 输出参数；成功时写入有效结果，失败时保持为 NULL 或未定义状态。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
cc_result_t cc_plugin_manager_create(cc_plugin_manager_t **out_manager)
{
    cc_plugin_manager_t *manager = calloc(1, sizeof(cc_plugin_manager_t));
    if (!manager) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create plugin manager");
    *out_manager = manager;
    return cc_result_ok();
}

/**
 * cc_plugin_tool_create_full — 创建、启动或加载组件资源，并把错误统一传播给调用方。
 *
 * 位置：插件/JSON-RPC 子系统。注释重点说明当前函数的输入输出、资源边界和错误传播。
 *
 * @param plugin_name 借用的只读字符串；函数不会释放该指针。
 * @param tool_name 借用的只读字符串；函数不会释放该指针。
 * @param tool_description 借用的只读字符串；函数不会释放该指针。
 * @param tool_schema_json 借用的只读字符串；函数不会释放该指针。
 * @param process 借用的指针参数；若需要长期保存内容，函数会复制。
 * @param out_tool 输出参数；成功时写入有效结果，失败时保持为 NULL 或未定义状态。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
extern cc_result_t cc_plugin_tool_create_full(
    const char *plugin_name,
    const char *tool_name,
    const char *tool_description,
    const char *tool_schema_json,
    cc_plugin_process_t *process,
    cc_tool_t *out_tool
);

/**
 * manager_add_entry — 向动态数组、字符串缓冲或结果集合追加内容，必要时扩容。
 *
 * 位置：插件/JSON-RPC 子系统。注释重点说明当前函数的输入输出、资源边界和错误传播。
 *
 * @param manager 借用的对象；函数不释放该对象本身。
 * @param entry 按值传入，用于控制本次操作。
 * 无返回值；副作用体现在对象状态、输出缓冲区或资源释放上。
 */
static void manager_add_entry(cc_plugin_manager_t *manager, cc_plugin_entry_t entry)
{
    manager->entries = realloc(manager->entries,
        (manager->entry_count + 1) * sizeof(cc_plugin_entry_t));
    manager->entries[manager->entry_count++] = entry;
}

/**
 * load_single_plugin — 创建、启动或加载组件资源，并把错误统一传播给调用方。
 *
 * 位置：插件/JSON-RPC 子系统。注释重点说明当前函数的输入输出、资源边界和错误传播。
 *
 * @param manager 借用的对象；函数不释放该对象本身。
 * @param plugin_json 借用的指针参数；若需要长期保存内容，函数会复制。
 * @param registry 借用的对象；函数不释放该对象本身。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
static cc_result_t load_single_plugin(
    cc_plugin_manager_t *manager,
    cc_json_value_t *plugin_json,
    cc_tool_registry_t *registry
)
{
    const char *plugin_name = cc_json_string_value(
        cc_json_object_get(plugin_json, "name"));
    const char *command = cc_json_string_value(
        cc_json_object_get(plugin_json, "command"));
    cc_json_value_t *args_arr = cc_json_object_get(plugin_json, "args");
    cc_json_value_t *tools_arr = cc_json_object_get(plugin_json, "tools");

    if (!plugin_name || !command || !args_arr || !tools_arr) {
        return cc_result_error(CC_ERR_MODEL, "Invalid plugin config: missing name/command/args/tools");
    }

    int argc = cc_json_is_array(args_arr) ? cc_json_array_size(args_arr) : 0;
    char **argv = calloc(argc + 2, sizeof(char *));
    if (!argv) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate argv");

    argv[0] = strdup(command);
    for (int i = 0; i < argc; i++) {
        const char *arg = cc_json_string_value(cc_json_array_get(args_arr, i));
        argv[i + 1] = arg ? strdup(arg) : strdup("");
    }
    argv[argc + 1] = NULL;

    cc_plugin_process_t *process = NULL;
    cc_result_t rc = cc_plugin_process_start(command, argv, &process);

    for (int i = 0; i < argc + 1; i++) free(argv[i]);
    free(argv);

    if (rc.code != CC_OK) {
        return rc;
    }

    int tool_count = cc_json_is_array(tools_arr) ? cc_json_array_size(tools_arr) : 0;
    int registered = 0;
    for (int i = 0; i < tool_count; i++) {
        cc_json_value_t *def = cc_json_array_get(tools_arr, i);
        const char *tool_name = cc_json_string_value(cc_json_object_get(def, "name"));
        const char *desc = cc_json_string_value(cc_json_object_get(def, "description"));
        cc_json_value_t *params = cc_json_object_get(def, "parameters");

        if (!tool_name) continue;

        char *schema_json = params ? cc_json_stringify(params) : NULL;

        cc_tool_t tool;
        memset(&tool, 0, sizeof(tool));
        rc = cc_plugin_tool_create_full(plugin_name, tool_name,
            desc ? desc : "Plugin tool",
            schema_json ? schema_json : "{\"type\":\"object\",\"properties\":{}}",
            process, &tool);

        free(schema_json);

        if (rc.code != CC_OK) continue;

        rc = cc_tool_registry_add(registry, tool);
        if (rc.code == CC_OK) {
            registered++;
        } else {
            if (tool.vtable && tool.vtable->destroy) {
                tool.vtable->destroy(tool.self);
            }
        }
    }

    if (registered == 0) {
        cc_plugin_process_destroy(process);
        return cc_result_error(CC_ERR_MODEL, "No tools could be registered for plugin");
    }

    cc_plugin_entry_t entry;
    entry.name = strdup(plugin_name);
    entry.process = process;
    entry.tool_count = registered;
    manager_add_entry(manager, entry);

    return cc_result_ok();
}

/**
 * cc_plugin_manager_load_plugins — 创建、启动或加载组件资源，并把错误统一传播给调用方。
 *
 * 位置：插件/JSON-RPC 子系统。注释重点说明当前函数的输入输出、资源边界和错误传播。
 *
 * @param manager 借用的对象；函数不释放该对象本身。
 * @param config_json 借用的只读字符串；函数不会释放该指针。
 * @param registry 借用的对象；函数不释放该对象本身。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
cc_result_t cc_plugin_manager_load_plugins(
    cc_plugin_manager_t *manager,
    const char *config_json,
    cc_tool_registry_t *registry
)
{
    if (!config_json || !registry) return cc_result_ok();

    cc_json_value_t *root = NULL;
    cc_result_t rc = cc_json_parse(config_json, &root);
    if (rc.code != CC_OK) return cc_result_ok();

    cc_json_value_t *plugins_arr = cc_json_object_get(root, "plugins");
    if (!plugins_arr || !cc_json_is_array(plugins_arr)) {
        cc_json_destroy(root);
        return cc_result_ok();
    }

    int count = cc_json_array_size(plugins_arr);
    for (int i = 0; i < count; i++) {
        cc_json_value_t *plugin_json = cc_json_array_get(plugins_arr, i);
        cc_result_t prc = load_single_plugin(manager, plugin_json, registry);
        if (prc.code != CC_OK) {
            fprintf(stderr, "Plugin load warning: %s\n",
                prc.message ? prc.message : "unknown error");
            cc_result_free(&prc);
        }
    }

    cc_json_destroy(root);
    return cc_result_ok();
}

/**
 * cc_plugin_manager_destroy — 释放、停止或复位该组件拥有的资源，防止失败路径泄漏。
 *
 * 位置：插件/JSON-RPC 子系统。注释重点说明当前函数的输入输出、资源边界和错误传播。
 *
 * @param manager 借用的对象；函数不释放该对象本身。
 * 无返回值；副作用体现在对象状态、输出缓冲区或资源释放上。
 */
void cc_plugin_manager_destroy(cc_plugin_manager_t *manager)
{
    if (!manager) return;
    for (int i = 0; i < manager->entry_count; i++) {
        cc_plugin_entry_t *entry = &manager->entries[i];
        cc_plugin_process_destroy(entry->process);
        free(entry->name);
    }
    free(manager->entries);
    free(manager);
}
