/**
 * 学习导读：cclaw/ports/include/cc/ports/cc_tool_registry.h
 *
 * 所属层次：端口层。
 * 阅读重点：这里定义可替换接口，阅读时重点看 struct + vtable + void *self 如何表达多态和依赖注入。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * cc_tool_registry.h — 工具注册表端口（Port）
 *
 * @file    cc/ports/cc_tool_registry.h
 * @brief   提供工具的注册、查找、汇总功能，作为 Agent 可调用工具的中央索引。
 *
 * 工具注册表采用"注册表模式"（Registry Pattern），是所有工具的统一入口。
 * LLM 通过 build_schema_json() 获知可用工具列表，通过 find() 查找具体工具
 * 并调用其 vtable->call() 执行。
 *
 * ─── 接口契约 ─────────────────────────────────────────────────────────
 *
 *   - 注册表在堆上分配，由 create/destroy 管理生命周期
 *   - 注册的工具以值拷贝方式存储（cc_tool_t 结构体的浅拷贝）
 *   - 工具名称唯一：同名工具 add 会覆盖旧的
 *   - build_schema_json 生成的 JSON 是符合 OpenAI function calling 格式的数组
 *
 * ─── 使用模式 ─────────────────────────────────────────────────────────
 *
 *   cc_tool_registry_t *reg = NULL;
 *   cc_tool_registry_create(&reg);
 *   cc_tool_registry_add(reg, some_tool);
 *   char *schema;
 *   cc_tool_registry_build_schema_json(reg, &schema);
 *   // 将 schema 传给 LLM...
 *   free(schema);
 *   cc_tool_registry_destroy(reg);
 *
 * ─── 依赖 ─────────────────────────────────────────────────────────────
 *
 *   依赖 cc/core/cc_result.h 和 cc/ports/cc_tool.h。
 */

#ifndef CC_TOOL_REGISTRY_H
#define CC_TOOL_REGISTRY_H

#include "cc/core/cc_result.h"
#include "cc/ports/cc_tool.h"

/**
 * cc_tool_registry_t — 工具注册表（不透明类型）
 *
 * 内部维护一个工具列表（动态数组），提供注册、查找和汇总功能。
 * 具体实现在 .c 文件中定义，对调用方透明。
 */
typedef struct cc_tool_registry cc_tool_registry_t;

/**
 * cc_tool_registry_create — 创建空的工具注册表
 *
 * 在堆上分配注册表，初始状态不含任何工具。
 *
 * @param out_registry  输出：指向新注册表的指针（调用者负责 cc_tool_registry_destroy）
 * @return              CC_OK 表示成功
 */
cc_result_t cc_tool_registry_create(cc_tool_registry_t **out_registry);

/**
 * cc_tool_registry_destroy — 销毁注册表及其中的所有工具
 *
 * 依次调用每个注册工具的 vtable->destroy()（如果存在），
 * 然后释放注册表本身。传入 NULL 是安全的。
 *
 * @param registry  要销毁的注册表指针
 */
void cc_tool_registry_destroy(cc_tool_registry_t *registry);

/**
 * cc_tool_registry_add — 向注册表添加一个工具
 *
 * 以值拷贝方式存储 tool（浅拷贝 self + vtable 指针）。
 * 如果已存在同名工具，旧工具被覆盖（旧工具的 destroy 被调用）。
 *
 * @param registry  注册表（不可为 NULL）
 * @param tool      要注册的工具实例（值拷贝）
 * @return          CC_OK 表示成功
 */
cc_result_t cc_tool_registry_add(
    cc_tool_registry_t *registry,
    cc_tool_t tool
);

/**
 * cc_tool_registry_freeze — 冻结注册表，禁止后续添加工具
 *
 * 冻结后注册表进入运行期只读状态，find/list/build_schema/count 可并发调用。
 *
 * @param registry  注册表（不可为 NULL）
 * @return          CC_OK 表示成功
 */
cc_result_t cc_tool_registry_freeze(cc_tool_registry_t *registry);

/**
 * cc_tool_registry_is_frozen — 查询注册表是否已冻结
 *
 * @param registry  注册表（可为 NULL）
 * @return          1 = 已冻结，0 = 未冻结或 registry 为 NULL
 */
int cc_tool_registry_is_frozen(cc_tool_registry_t *registry);

/**
 * cc_tool_registry_find — 按名称查找工具
 *
 * 在注册表中查找指定名称的工具，拷贝到 out_tool（浅拷贝）。
 *
 * @param registry  注册表（不可为 NULL）
 * @param name      要查找的工具名称（不可为 NULL）
 * @param out_tool  输出：找到的工具实例（值拷贝），未找到时 vtable 为 NULL
 * @return          CC_OK 表示找到，CC_ERR_NOT_FOUND 表示不存在
 */
cc_result_t cc_tool_registry_find(
    cc_tool_registry_t *registry,
    const char *name,
    cc_tool_t *out_tool
);

/**
 * cc_tool_registry_build_schema_json — 生成所有工具的 JSON Schema 数组
 *
 * 遍历注册表中所有工具，根据每个工具的 name()、description()、
 * schema_json() 构建一个符合 OpenAI function calling 格式的 JSON 数组。
 * 此数组可直接放入 LLM 的 tools 参数。
 *
 * @param registry       注册表（不可为 NULL）
 * @param out_tools_json 输出：JSON Schema 数组字符串（调用者负责 free）
 * @return               CC_OK 表示成功
 */
cc_result_t cc_tool_registry_build_schema_json(
    cc_tool_registry_t *registry,
    char **out_tools_json
);

/**
 * cc_tool_registry_count — 获取注册表中工具的数量
 *
 * @param registry  注册表（不可为 NULL）
 * @return          已注册的工具数量
 */
size_t cc_tool_registry_count(cc_tool_registry_t *registry);

/**
 * cc_tool_registry_list_names — 获取所有工具的名称列表
 *
 * 分配一个字符串数组，包含所有已注册工具的名称。
 * 用于 UI 展示和调试。
 *
 * @param registry   注册表（不可为 NULL）
 * @param out_names  输出：工具名称数组（调用者负责分别 free 每个元素和数组本身）
 * @param out_count  输出：名称数组的长度
 * @return           CC_OK 表示成功
 */
cc_result_t cc_tool_registry_list_names(
    cc_tool_registry_t *registry,
    char ***out_names,
    size_t *out_count
);

#endif
