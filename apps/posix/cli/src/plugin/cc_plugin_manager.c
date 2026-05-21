/**
 * 学习导读：apps/posix/cli/src/plugin/cc_plugin_manager.c
 *
 * 所属层次：POSIX CLI 应用层。
 * 阅读重点：这里组装桌面 CLI、工具、插件和 sandbox，阅读时重点看 main 到 runtime builder 的组合流程。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/******************************************************************************
 * cc_plugin_manager.c — 插件管理器
 *
 * 本模块负责外部插件的完整生命周期管理。主入口读取 config.json 的
 * plugins.entries，为每个插件启动一个或多个 worker 子进程，并将其暴露的工具
 * 注册到工具注册表中。
 *
 * 架构角色：
 *   cc_plugin_manager 是插件系统的"门面"（Facade），对外提供统一的
 *   加载接口。runtime builder 只需调用 cc_plugin_manager_load_config() 即可
 *   加载所有外部插件，无需关心进程、管道和工具注册的细节。
 *
 * 插件配置格式：
 *   {
 *     "plugins": {
 *       "entries": {
 *         "weather": {
 *           "enabled": true,
 *           "workers": 2,
 *           "command": "python3",
 *           "args": ["apps/posix/cli/plugins/weather_tool.py"],
 *           "tools": [
 *             {
 *               "name": "weather_query",
 *               "description": "查询城市天气",
 *               "parameters": { ...JSON Schema... }
 *             }
 *           ]
 *         }
 *       }
 *     }
 *   }
 *
 * 一个插件条目可以暴露多个工具。单 worker 内部由管道串行化；多 worker 时，
 * plugin tool 会 round-robin 选择子进程，从而允许同一插件并发处理多个调用。
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
    cc_plugin_process_t **processes;
    int process_count;
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
 * cc_plugin_manager_create — 创建插件 supervisor 状态。
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
 * cc_plugin_tool_create_full — 把一个插件进程包装成标准 tool。
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

extern cc_result_t cc_plugin_tool_create_pool(
    const char *plugin_name,
    const char *tool_name,
    const char *tool_description,
    const char *tool_schema_json,
    cc_plugin_process_t **processes,
    size_t process_count,
    cc_tool_t *out_tool
);

/**
 * manager_add_entry — 向动态数组、字符串缓冲或结果集合追加内容，必要时扩容。
 *
 * 位置：插件/JSON-RPC 子系统。注释重点说明当前函数的输入输出、资源边界和错误传播。
 *
 * @param manager 借用的对象；函数不释放该对象本身。
 * @param entry 按值传入，用于控制本次操作。
 * @return CC_OK 表示已接管 entry；失败时调用方仍拥有 entry，需要自行释放。
 */
static cc_result_t manager_add_entry(cc_plugin_manager_t *manager, cc_plugin_entry_t entry)
{
    cc_plugin_entry_t *next = realloc(manager->entries,
        (manager->entry_count + 1) * sizeof(cc_plugin_entry_t));
    if (!next) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to grow plugin entries");
    manager->entries = next;
    manager->entries[manager->entry_count++] = entry;
    return cc_result_ok();
}

/**
 * load_single_plugin — 从旧 JSON 测试格式启动一个插件条目。
 *
 * @param manager 借用的对象；函数不释放该对象本身。
 * @param plugin_json 借用的指针参数；若需要长期保存内容，函数会复制。
 * @param registry 借用的对象；函数不释放该对象本身。
 * @return 成功表示该插件至少完成了必要资源处理；失败由上层跳过该插件。
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
    int workers = cc_json_int_value(cc_json_object_get(plugin_json, "workers"));
    int timeout_ms = cc_json_int_value(cc_json_object_get(plugin_json, "timeoutMs"));
    if (timeout_ms <= 0) timeout_ms = cc_json_int_value(cc_json_object_get(plugin_json, "timeout_ms"));
    int restart_on_crash = cc_json_bool_value(cc_json_object_get(plugin_json, "restartOnCrash"));
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

    if (workers <= 0) workers = 1;
    cc_plugin_process_t **processes = calloc((size_t)workers, sizeof(*processes));
    if (!processes) {
        for (int i = 0; i < argc + 1; i++) free(argv[i]);
        free(argv);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate plugin workers");
    }

    cc_result_t rc = cc_result_ok();
    int started = 0;
    for (int i = 0; i < workers; i++) {
        rc = cc_plugin_process_start_with_options(
            command, argv, restart_on_crash, timeout_ms, &processes[started]);
        if (rc.code != CC_OK) break;
        started++;
    }

    for (int i = 0; i < argc + 1; i++) free(argv[i]);
    free(argv);

    if (rc.code != CC_OK) {
        for (int i = 0; i < started; i++) cc_plugin_process_destroy(processes[i]);
        free(processes);
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
        rc = cc_plugin_tool_create_pool(plugin_name, tool_name,
            desc ? desc : "Plugin tool",
            schema_json ? schema_json : "{\"type\":\"object\",\"properties\":{}}",
            processes, (size_t)started, &tool);

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
        for (int i = 0; i < started; i++) cc_plugin_process_destroy(processes[i]);
        free(processes);
        return cc_result_error(CC_ERR_MODEL, "No tools could be registered for plugin");
    }

    cc_plugin_entry_t entry;
    entry.name = strdup(plugin_name);
    entry.processes = processes;
    entry.process_count = started;
    entry.tool_count = registered;
    if (!entry.name) {
        for (int i = 0; i < started; i++) cc_plugin_process_destroy(processes[i]);
        free(processes);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy plugin name");
    }
    rc = manager_add_entry(manager, entry);
    if (rc.code != CC_OK) {
        free(entry.name);
        for (int i = 0; i < started; i++) cc_plugin_process_destroy(processes[i]);
        free(processes);
        return rc;
    }

    return cc_result_ok();
}

static cc_result_t load_single_plugin_config(
    cc_plugin_manager_t *manager,
    const cc_config_plugin_entry_t *entry_config,
    cc_tool_registry_t *registry,
    cc_runtime_diagnostics_t *diagnostics
)
{
    if (!entry_config || !entry_config->enabled) return cc_result_ok();
    if (!entry_config->id || !entry_config->command || entry_config->tool_count == 0) {
        return cc_result_error(CC_ERR_MODEL, "Invalid plugin config: missing id/command/tools");
    }

    int argc = (int)entry_config->arg_count;
    char **argv = calloc((size_t)argc + 2, sizeof(char *));
    if (!argv) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate argv");
    argv[0] = strdup(entry_config->command);
    for (int i = 0; i < argc; i++) {
        argv[i + 1] = strdup(entry_config->args[i] ? entry_config->args[i] : "");
    }
    argv[argc + 1] = NULL;

    int workers = entry_config->workers > 0 ? entry_config->workers : 1;
    cc_plugin_process_t **processes = calloc((size_t)workers, sizeof(*processes));
    if (!processes) {
        for (int i = 0; i < argc + 1; i++) free(argv[i]);
        free(argv);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate plugin workers");
    }

    cc_result_t rc = cc_result_ok();
    int started = 0;
    for (int i = 0; i < workers; i++) {
        rc = cc_plugin_process_start_with_options(
            entry_config->command,
            argv,
            entry_config->restart_on_crash,
            entry_config->timeout_ms,
            &processes[started]);
        if (rc.code != CC_OK) break;
        started++;
    }
    for (int i = 0; i < argc + 1; i++) free(argv[i]);
    free(argv);
    if (rc.code != CC_OK) {
        for (int i = 0; i < started; i++) cc_plugin_process_destroy(processes[i]);
        free(processes);
        return rc;
    }

    int registered = 0;
    for (size_t i = 0; i < entry_config->tool_count; i++) {
        const cc_config_plugin_tool_t *tool_cfg = &entry_config->tools[i];
        if (!tool_cfg->name) continue;
        cc_tool_t tool = {0};
        rc = cc_plugin_tool_create_pool(
            entry_config->id,
            tool_cfg->name,
            tool_cfg->description ? tool_cfg->description : "Plugin tool",
            tool_cfg->parameters_json ? tool_cfg->parameters_json : "{\"type\":\"object\",\"properties\":{}}",
            processes,
            (size_t)started,
            &tool);
        if (rc.code != CC_OK) {
            cc_runtime_diagnostics_add(
                diagnostics,
                "plugin_tool",
                tool_cfg->name ? tool_cfg->name : entry_config->id,
                rc.message ? rc.message : "failed to create plugin tool");
            cc_result_free(&rc);
            continue;
        }
        rc = cc_tool_registry_add(registry, tool);
        if (rc.code == CC_OK) {
            registered++;
        } else if (tool.vtable && tool.vtable->destroy) {
            cc_runtime_diagnostics_add(
                diagnostics,
                "plugin_tool",
                tool_cfg->name ? tool_cfg->name : entry_config->id,
                rc.message ? rc.message : "failed to register plugin tool");
            tool.vtable->destroy(tool.self);
            cc_result_free(&rc);
        }
    }

    if (registered == 0) {
        for (int i = 0; i < started; i++) cc_plugin_process_destroy(processes[i]);
        free(processes);
        return cc_result_error(CC_ERR_MODEL, "No tools could be registered for plugin");
    }

    cc_plugin_entry_t entry = {0};
    entry.name = strdup(entry_config->id);
    if (!entry.name) {
        for (int i = 0; i < started; i++) cc_plugin_process_destroy(processes[i]);
        free(processes);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy plugin id");
    }
    entry.processes = processes;
    entry.process_count = started;
    entry.tool_count = registered;
    rc = manager_add_entry(manager, entry);
    if (rc.code != CC_OK) {
        free(entry.name);
        for (int i = 0; i < started; i++) cc_plugin_process_destroy(processes[i]);
        free(processes);
        return rc;
    }
    return cc_result_ok();
}

cc_result_t cc_plugin_manager_load_config(
    cc_plugin_manager_t *manager,
    const cc_config_t *config,
    cc_tool_registry_t *registry,
    cc_runtime_diagnostics_t *diagnostics
)
{
    if (!manager || !config || !registry) return cc_result_ok();
    for (size_t i = 0; i < config->plugins.entry_count; i++) {
        const cc_config_plugin_entry_t *entry = &config->plugins.entries[i];
        cc_result_t prc = load_single_plugin_config(manager, entry, registry, diagnostics);
        if (prc.code != CC_OK) {
            cc_runtime_diagnostics_add(
                diagnostics,
                "plugin",
                entry && entry->id ? entry->id : "(unknown)",
                prc.message ? prc.message : "plugin unavailable");
            cc_result_free(&prc);
        }
    }
    return cc_result_ok();
}

/**
 * cc_plugin_manager_load_plugins — 旧测试入口：尽力加载 JSON 字符串中的插件。
 *
 * 桌面 CLI 主路径使用 cc_plugin_manager_load_config() 读取 config.json。
 * 本函数保留给低层测试和简单嵌入场景：解析失败或单个插件失败会跳过，
 * 不让整个进程退出，也不会注册不可用工具。
 *
 * @param manager 借用的对象；函数不释放该对象本身。
 * @param config_json 借用的只读字符串；函数不会释放该指针。
 * @param registry 借用的对象；函数不释放该对象本身。
 * @return 当前语义始终返回 CC_OK；失败条目被跳过。
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
        for (int j = 0; j < entry->process_count; j++) {
            cc_plugin_process_destroy(entry->processes[j]);
        }
        free(entry->processes);
        free(entry->name);
    }
    free(manager->entries);
    free(manager);
}
