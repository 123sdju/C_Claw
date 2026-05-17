/**
 * 学习导读：apps/windows/cli/include/cc/plugin/cc_plugin_manager.h
 *
 * 所属层次：Windows CLI 应用层。
 * 阅读重点：这里镜像桌面 CLI 能力但使用 Windows 平台实现，阅读时重点比较与 POSIX 版本的差异。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/******************************************************************************
 * cc_plugin_manager.h — 插件管理器接口
 *
 * 本头文件声明了插件管理器的公共 API。插件管理器是插件系统的
 * 顶层入口（Facade），负责插件配置加载、进程启动和工具注册。
 *
 * ── 使用方式 ──
 *
 *   1. cc_plugin_manager_create(&manager)         — 创建管理器
 *   2. cc_plugin_manager_load_plugins(manager, json_config, registry)
 *        — 从 JSON 配置加载所有插件，启动进程，注册工具到 ToolRegistry
 *   3. ... (Agent 运行期间，插件工具自动通过管道通信) ...
 *   4. cc_plugin_manager_destroy(manager)         — 清理所有插件进程
 *
 * ── 配置格式 ──
 *
 *   插件配置是一个 JSON 字符串（通常来自 plugins.json 文件）：
 *
 *   {
 *     "plugins": [
 *       {
 *         "name": "插件人类可读名称",
 *         "command": "可执行文件路径（如 python3）",
 *         "args": ["参数1", "参数2", ...],
 *         "tools": [
 *           {
 *             "name": "工具名（LLM 可见）",
 *             "description": "工具功能描述（LLM 用于判断何时调用）",
 *             "parameters": { JSON Schema 对象 }
 *           }
 *         ]
 *       }
 *     ]
 *   }
 *
 *   一个插件进程可以暴露多个工具，通过不同的 method 名称区分。
 *   所有工具共享同一个子进程和管道连接。
 *
 * ── 错误处理 ──
 *
 *   加载策略采用"尽力而为"（best-effort）：
 *   - 某个插件配置格式错误 → 跳过该插件，继续加载下一个
 *   - 某个工具注册失败 → 跳过该工具，继续处理同插件的其他工具
 *   - 插件进程启动失败 → 跳过该插件
 *   - 没有 plugins.json 文件 → 静默跳过，不报错
 *
 * ── 依赖 ──
 *
 *   cc/core/cc_result.h       — 统一错误处理类型
 *   cc/ports/cc_tool_registry.h — 工具注册表接口
 *****************************************************************************/

#ifndef CC_PLUGIN_MANAGER_H
#define CC_PLUGIN_MANAGER_H

#include "cc/core/cc_result.h"
#include "cc/ports/cc_tool_registry.h"

/* opaque pointer — 实现细节隐藏在 .c 文件中 */
typedef struct cc_plugin_manager cc_plugin_manager_t;

/**
 * 创建插件管理器。
 *
 * @param out_manager 输出：创建的插件管理器对象
 * @return            CC_OK 或 CC_ERR_OUT_OF_MEMORY
 */
cc_result_t cc_plugin_manager_create(cc_plugin_manager_t **out_manager);

/**
 * 销毁插件管理器。
 *
 * 终止所有已加载的插件子进程，释放所有相关内存。
 * 包括：停止子进程、关闭管道、释放工具对象、释放插件配置数据。
 * manager 可以为 NULL。
 *
 * @param manager  要销毁的插件管理器
 */
void cc_plugin_manager_destroy(cc_plugin_manager_t *manager);

/**
 * 从 JSON 配置加载所有插件。
 *
 * 解析配置 JSON，为每个插件启动子进程，将其工具注册到指定的
 * ToolRegistry 中。采用 best-effort 策略，单个插件失败不影响其他。
 *
 * @param manager     插件管理器
 * @param config_json 插件配置 JSON 字符串（如从 plugins.json 读取的内容）
 * @param registry    工具注册表，插件工具将注册到这里
 * @return            CC_OK（始终返回成功，内部跳过失败的插件）
 */
cc_result_t cc_plugin_manager_load_plugins(
    cc_plugin_manager_t *manager,
    const char *config_json,
    cc_tool_registry_t *registry
);

#endif