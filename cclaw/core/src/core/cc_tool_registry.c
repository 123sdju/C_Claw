/**
 * 学习导读：cclaw/core/src/core/cc_tool_registry.c
 *
 * 所属层次：核心层。
 * 阅读重点：这里定义 Agent 运行时的数据模型、主循环和通用工具，阅读时重点看所有权、错误返回和 ReAct 数据流。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * cc_tool_registry.c — 工具注册表实现
 *
 * 模块在整体架构中的角色：
 *   本模块是 AI Agent 的"工具箱"管理器——它维护一个已注册工具的集合，
 *   提供按名称查找、生成 JSON Schema 等功能。当 LLM 决定调用某个工具时，
 *   框架通过注册表查找对应工具的实现并执行。
 *
 *   这是连接 LLM 模型能力与系统实际能力的"桥梁"——
 *   LLM 只能输出 function call 的文本描述（函数名 + JSON 参数），
 *   注册表将这些文本描述映射到实际的 C 函数调用（通过虚表 vtable），
 *   使 Agent 能够产生真正的系统副作用（读文件、查数据库、发网络请求等）。
 *
 *   如果没有注册表，LLM 的工具调用只是一串无意义的文本。
 *   有了注册表，这些文本变成了对真实系统能力的引用。
 *
 * 依赖的其他模块：
 *   - cc_result.h — 统一错误返回类型
 *   - cc_tool_registry.h — 定义 cc_tool_t、cc_tool_registry_t 和虚表接口
 *   - cc_string_builder.h — 高效的字符串拼接，用于构造 JSON Schema 数组
 *   - cc_thread.h — 互斥锁，保护并发访问
 *   - 标准库 (stdlib.h, string.h)
 *
 * 被哪些模块使用：
 *   - App feature/runtime_builder — 在初始化时注册所有可用工具（file_read、
 *     shell_run、memory、http.request 等）
 *   - Agent runtime — 调用 build_schema_json 生成 tools 参数 JSON，
 *     随请求交给 LLM provider，告诉模型有哪些工具可用
 *   - Tool Executor 层 — 通过 find 查找工具后调用 vtable->call
 *
 * 注册表模式（Registry Pattern）详解：
 *   注册表是一种设计模式，用于集中管理一组"可发现"的对象。在本框架中：
 *   - 注册阶段：应用 feature 工厂创建工具实例并调用 add 注册到注册表
 *   - 发现阶段：Tool Executor 通过 name 调用 find 查找工具
 *   - 描述阶段：LLM Adapter 通过 build_schema_json 生成工具的 JSON Schema 描述
 *   注册表不关心工具的具体实现——它只管理 cc_tool_t 条目（vtable + self 指针），
 *   所有具体工作委托给虚表方法。这实现了"控制反转"（IoC）：
 *   注册表定义了"什么是工具"，各工具模块定义了"工具怎么做"。
 *
 * 设计决策（为什么这样设计）：
 *   1. 固定容量（MAX_TOOLS=64），使用静态数组。
 *      为什么：工具数量在程序启动时由 runtime_builder 注册确定，运行时不变。
 *      静态数组避免了动态数组的 malloc/realloc 开销和内存碎片。
 *      64 个槽位对于典型 AI Agent 场景绰绰有余（通常 10-30 个工具）。
 *   2. 每个工具通过虚表（vtable）提供名称、描述、Schema 和执行函数。
 *      为什么：虚表模式是 C 语言中实现多态的标准方式——不同工具的实现
 *      完全不同（文件读取 vs HTTP 请求 vs 长期记忆），但注册表可以以
 *      统一接口管理它们，无需知道具体类型。新工具只需实现虚表接口，
 *      注册表代码完全不需要修改。
 *   3. 支持按名称线性查找（O(n) 扫描）。
 *      为什么：工具数量通常不超过 20-30 个，线性扫描的常数因子极低
 *      （连续内存、缓存友好）。对于这个规模，散列表的散列计算开销 +
 *      额外的内存间接访问反而更慢。且线性扫描的代码更简单、更易维护。
 *   4. Schema 通过 cc_string_builder 动态构建 JSON 数组。
 *      为什么：每个工具的 Schema JSON 可能很大（含复杂的 parameters 定义，
 *      可能有嵌套对象、数组、枚举等），且总量无法预知。
 *      cc_string_builder 通过动态扩容（按需 realloc）避免了预分配过大
 *      缓冲区或多次 strcat 的低效。最终通过 cc_string_builder_take
 *      提取完整字符串并转移所有权。
 *   5. 冻结机制（freeze）——注册完成后调用 freeze 禁止后续添加。
 *      为什么：工具的 Schema JSON 被发送给 LLM，形成"能力契约"。
 *      如果在对话中途添加新工具，LLM 可能会收到不一致的工具列表，
 *      导致它调用已存在但未在当前请求中声明的工具。冻结确保了一致性。
 *
 * 工具生命周期：
 *   注册表创建 → 各工具 create + add 注册 → freeze（可选）→
 *   find/build_schema 使用 → destroy 销毁（级联销毁所有工具）
 */

#include "cc/ports/cc_tool_registry.h"
#include "cc/util/cc_string_builder.h"
#include "cc/ports/cc_thread.h"
#include <stdlib.h>
#include <string.h>

#define MAX_TOOLS 64

/*
 * cc_tool_registry — 工具注册表结构体（内部实现，opaque type）
 *
 * 持有所有已注册工具的数组、当前计数、冻结标志和互斥锁。
 * 所有查询操作通过线性扫描完成——对于最多 64 个工具的场景，
 * 这是最优策略：连续内存、CPU 缓存友好、无散列计算和冲突处理开销。
 *
 * 字段：
 *   tools  — 静态定长数组，存储所有已注册的工具（cc_tool_t 结构体）。
 *            按注册顺序（add 的顺序）排列——注册顺序决定了
 *            build_schema_json 输出中的工具顺序。
 *            每个条目是 cc_tool_t 的浅拷贝（包含 self 和 vtable 两个指针）。
 *            实际数据（工具实现）由 self 指向，生命周期由注册表管理
 *            （destroy 时通过 vtable->destroy(self) 释放）。
 *   count  — 当前已注册的工具数量，也是 tools 的有效索引范围 [0, count)。
 *            新增工具时 count 自增作为写入索引。destroy 时遍历 [0, count)
 *            释放每个工具。
 *   frozen — 冻结标志：0 表示可以添加工具，1 表示冻结（禁止添加）。
 *            为什么需要冻结：确保工具列表在整个对话期间保持一致——
 *            LLM 收到的工具 Schema 与实际可调用的工具集必须匹配。
 *            冻结后尝试 add 会返回 CC_ERR_INVALID_ARGUMENT 错误。
 *   mutex  — 互斥锁，保护 tools 数组、count 和 frozen 标志的并发访问。
 *            为什么需要锁：add 和 find/build_schema 可能在不同线程中
 *            执行（例如应用启动阶段注册，runtime 在工作线程
 *            构建 Schema）。互斥锁保证了并发安全。
 */
struct cc_tool_registry {
    cc_tool_t tools[MAX_TOOLS];
    size_t count;
    int frozen;
    cc_mutex_t mutex;
};

/*
 * cc_tool_registry_create — 创建工具注册表实例
 *
 * 功能：
 *   分配并初始化一个空的工具注册表。使用 calloc 分配确保所有工具槽位
 *   零初始化（vtable=NULL, self=NULL），count=0，frozen=0。
 *   同时创建互斥锁用于线程安全操作。
 *
 *   初始状态是一个"空工具箱"——没有任何工具可用，LLM 不会收到
 *   任何 function call 能力。调用者在创建后需要通过 cc_tool_registry_add
 *   逐个注册工具，最后调用 cc_tool_registry_freeze 冻结注册表。
 *
 * 参数：
 *   @param out_registry — [out] 输出参数，指向创建的注册表指针。
 *                         如果函数返回非 CC_OK，此参数的值未定义。
 *
 * @return CC_OK 表示成功。
 * @return CC_ERR_OUT_OF_MEMORY 表示 calloc 分配失败，系统内存不足。
 */
cc_result_t cc_tool_registry_create(cc_tool_registry_t **out_registry)
{
    cc_tool_registry_t *registry = calloc(1, sizeof(cc_tool_registry_t));
    if (!registry) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create tool registry");
    registry->count = 0;
    cc_result_t rc = cc_mutex_create(&registry->mutex);
    if (rc.code != CC_OK) {
        free(registry);
        return rc;
    }
    *out_registry = registry;
    return cc_result_ok();
}

/*
 * cc_tool_registry_destroy — 销毁工具注册表
 *
 * 功能：
 *   遍历所有已注册的工具，调用每个工具虚表中的 destroy 函数释放其
 *   资源，销毁互斥锁，最后释放注册表结构体本身。
 *
 *   这是注册表的"级联销毁"——注册表销毁时，它管理的所有工具也被销毁。
 *   这种"聚合根负责清理子对象"的模式是所有权管理的最佳实践。
 *
 * 参数：
 *   @param registry — 要销毁的注册表指针，可以为 NULL（安全无操作）。
 *
 * 行为细节（防御性编程）：
 *   对每个已注册工具，检查三个条件全部满足后才调用 destroy：
 *     1. tool->vtable != NULL           — 工具虚表存在
 *     2. tool->vtable->destroy != NULL  — destroy 函数指针存在（非纯虚函数）
 *     3. tool->self != NULL             — 工具实例数据存在
 *   如果任何一个条件不满足，跳过该工具。
 *   为什么这么严格：注册表中可能存在"幽灵条目"——部分初始化的工具
 *   （如 add 过程中因错误而未完全填充的条目）。三级检查确保了
 *   对每个条目只调用合法的 destroy，防止空指针崩溃。
 *
 * 线程安全：
 *   销毁前加锁遍历释放工具，但本身不防外部的并发操作——
 *   调用者应确保在 destroy 时没有其他线程在使用注册表。
 *   通常 destroy 在程序结束时调用，此时所有线程已停止。
 *
 * 注意事项：
 *   - 销毁后 registry 指针变为悬空，调用者不应再使用。
 *   - 工具自身的生命周期由注册表完全管理——注册表销毁时所有工具也销毁。
 *     调用者不需要（也不应该）单独销毁已注册的工具。
 */
void cc_tool_registry_destroy(cc_tool_registry_t *registry)
{
    if (!registry) return;
    cc_mutex_lock(registry->mutex);
    for (size_t i = 0; i < registry->count; i++) {
        cc_tool_t *tool = &registry->tools[i];
        if (tool->vtable && tool->vtable->destroy && tool->self) {
            tool->vtable->destroy(tool->self);
        }
    }
    cc_mutex_unlock(registry->mutex);
    cc_mutex_destroy(registry->mutex);
    free(registry);
}

/*
 * cc_tool_registry_add — 向注册表中添加工具
 *
 * 功能：
 *   将一个工具追加到注册表的工具列表中。工具按值传递（浅拷贝 cc_tool_t），
 *   这意味着 vtable 指针和 self 指针被复制，但工具实现本身的数据
 *   （self 指向的堆内存）不会复制。注册表接管了该工具的所有权——
 *   destroy 时会调用 vtable->destroy(self) 释放资源。
 *
 *   如果注册表已被 freeze，此函数返回错误——冻结后不允许添加新工具。
 *
 * 参数：
 *   @param registry — 目标注册表，不可为 NULL
 *   @param tool     — 要注册的工具（按值传递，包含 self 和 vtable 两个指针）。
 *                     当函数返回 CC_OK 时，工具的所有权已转移给注册表。
 *                     调用者不应再单独销毁此工具——注册表的 destroy 会负责。
 *                     调用者可以安全地覆写/释放自己的本地变量。
 *
 * @return CC_OK 表示注册成功。
 * @return CC_ERR_INVALID_ARGUMENT 表示 registry 为 NULL，或注册表已被冻结。
 * @return CC_ERR_OUT_OF_MEMORY 表示注册表已满（达到 MAX_TOOLS=64）。
 *
 * 为什么按值传递而非指针：
 *   按值传递将工具的浅拷贝所有权转移给注册表。调用者可以在 add 后
 *   立即释放/覆写本地变量，不会影响注册表中的条目。
 *   cc_tool_t 只有两个指针（~16 字节），按值传递的成本为零（寄存器传递）。
 *
 * 为什么添加后不自动 freeze：
 *   freeze 是独立操作——某些场景下注册表可能需要分阶段添加工具：
 *   - 第一阶段：注册 Core 层基础工具（文件读写、Shell 执行等）
 *   - 第二阶段：应用 feature 注册平台/产品特定工具（shell、plugin、GPIO 等）
 *   在全部注册完成后由 runtime_builder 显式调用 freeze。
 *   这让不同模块可以独立添加工具，互不干扰。
 */
cc_result_t cc_tool_registry_add(
    cc_tool_registry_t *registry,
    cc_tool_t tool
)
{
    if (!registry) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null registry");
    cc_mutex_lock(registry->mutex);
    if (registry->frozen) {
        cc_mutex_unlock(registry->mutex);
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Tool registry is frozen");
    }
    if (registry->count >= MAX_TOOLS) {
        cc_mutex_unlock(registry->mutex);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Tool registry full");
    }
    registry->tools[registry->count++] = tool;
    cc_mutex_unlock(registry->mutex);
    return cc_result_ok();
}

/*
 * cc_tool_registry_freeze — 冻结工具注册表
 *
 * 功能：
 *   将注册表标记为冻结状态，此后不能再添加新工具。冻结是单向操作——
 *   一旦冻结，不能解冻。这是确保工具列表一致性的关键机制。
 *
 * 为什么需要冻结：
 *   工具注册表是 LLM 与系统之间的"能力契约"——LLM 收到的工具 Schema
 *   必须与实际可调用的工具集完全一致。如果在对话中途添加新工具，
 *   可能出现以下问题：
 *     - LLM 在上一轮请求中收到的 Schema 不包含新工具
 *     - 但本轮注册表已包含新工具
 *     - LLM 可能调用它不知道的工具（因为上一轮没收到 Schema）
 *     - 或 LLM 知道某个工具存在但它被删除了
 *   冻结确保整个对话周期内工具集不变，避免了 Schema 不一致问题。
 *
 * 参数：
 *   @param registry — 注册表指针，不可为 NULL
 *
 * @return CC_OK 表示冻结成功。
 * @return CC_ERR_INVALID_ARGUMENT 表示 registry 为 NULL。
 *
 * 典型使用流程：
 *   // 初始化阶段
 *   cc_tool_registry_t *reg;
 *   cc_tool_registry_create(&reg);
 *   cc_tool_registry_add(reg, create_file_tool());
 *   cc_tool_registry_add(reg, create_search_tool());
 *   cc_tool_registry_add(reg, create_db_tool());
 *   cc_tool_registry_freeze(reg);  // 注册完成，冻结！
 *   // 后续只能 find/build_schema，不能再 add
 */
cc_result_t cc_tool_registry_freeze(cc_tool_registry_t *registry)
{
    if (!registry) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null registry");
    cc_mutex_lock(registry->mutex);
    registry->frozen = 1;
    cc_mutex_unlock(registry->mutex);
    return cc_result_ok();
}

/*
 * cc_tool_registry_is_frozen — 查询注册表是否已冻结
 *
 * 功能：
 *   返回注册表的冻结状态。主要用于调试和诊断——确认注册表在
 *   预期的时间点已被冻结。
 *
 * 参数：
 *   @param registry — 注册表指针，可以为 NULL（返回 0，视为未冻结）
 *
 * @return 1 表示已冻结，0 表示未冻结（或 registry 为 NULL）。
 *
 * 典型使用：
 *   if (!cc_tool_registry_is_frozen(reg)) {
 *       cc_log_warn("Tool registry not frozen before LLM request");
 *   }
 */
int cc_tool_registry_is_frozen(cc_tool_registry_t *registry)
{
    if (!registry) return 0;
    cc_mutex_lock(registry->mutex);
    int frozen = registry->frozen;
    cc_mutex_unlock(registry->mutex);
    return frozen;
}

/*
 * cc_tool_registry_find — 按名称查找工具
 *
 * 功能：
 *   在注册表中线性搜索与给定名称匹配的工具。对于每个已注册工具，
 *   通过 vtable->name(tool->self) 获取工具的名称字符串，与传入的
 *   name 参数进行 strcmp 精确匹配。
 *
 *   这是工具调用流程中的关键步骤——LLM 返回 {name: "file_read"}，
 *   框架通过 find("file_read") 获取对应的工具实现，然后调用
 *   tool.vtable->execute(tool.self, ...) 执行。
 *
 * 参数：
 *   @param registry — 注册表，不可为 NULL
 *   @param name     — 要查找的工具名称，不可为 NULL。
 *                     这是 LLM 在 function call 中指定的工具名。
 *   @param out_tool — [out] 输出参数，指向找到的工具的副本（浅拷贝）。
 *                     如果函数返回非 CC_OK，此参数的值未定义。
 *
 * @return CC_OK 表示找到，*out_tool 包含工具的完整副本（vtable + self）。
 * @return CC_ERR_INVALID_ARGUMENT 表示 registry 或 name 为 NULL。
 * @return CC_ERR_NOT_FOUND 表示没有找到匹配名称的工具。
 *
 * 返回浅拷贝（而非指针）的原因：
 *   返回 cc_tool_t 的副本（两个指针：vtable 和 self）让调用者可以
 *   直接通过 out_tool->vtable->execute(out_tool->self, ...) 调用工具，
 *   无需持有注册表的锁。如果在锁内返回指针，调用者在锁外使用该指针
 *   时注册表可能被并发修改。返回副本消除了这种竞态条件——调用者持有
 *   自己的 tool 副本，不依赖注册表的内部状态。
 *
 * 为什么线性搜索而非哈希表：
 *   工具数量通常 <= 30，线性扫描的常数因子极低。
 *   连续内存布局使得 CPU 缓存命中率高（一次缓存行加载可覆盖多个条目）。
 *   对于这个规模，哈希表需要：
 *     - 散列函数计算（至少数十个 CPU 周期）
 *     - 额外的内存间接访问（哈希表指针跳转）
 *     - 冲突处理逻辑（开放寻址或链表遍历）
 *   这些开销加起来可能超过线性扫描的简单比较（30 次 strcmp 也非常快）。
 */
cc_result_t cc_tool_registry_find(
    cc_tool_registry_t *registry,
    const char *name,
    cc_tool_t *out_tool
)
{
    if (!registry || !name) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null argument");
    cc_mutex_lock(registry->mutex);
    for (size_t i = 0; i < registry->count; i++) {
        cc_tool_t *tool = &registry->tools[i];
        /* 防御性检查：确保工具的 vtable 和 name 函数存在。
           已注册的工具应该有完整的虚表，但加检查避免因部分初始化
           导致崩溃。如果某项检查失败，静默跳过该条目。 */
        if (tool->vtable && tool->vtable->name) {
            const char *tool_name = tool->vtable->name(tool->self);
            if (tool_name && strcmp(tool_name, name) == 0) {
                *out_tool = *tool;  /* 浅拷贝：复制 vtable 和 self 两个指针 */
                cc_mutex_unlock(registry->mutex);
                return cc_result_ok();
            }
        }
    }
    cc_mutex_unlock(registry->mutex);
    return cc_result_errf(CC_ERR_NOT_FOUND, "Tool not found: %s", name);
}

/*
 * cc_tool_registry_build_schema_json — 构建工具的 JSON Schema 列表
 *
 * 功能：
 *   遍历所有已注册的工具，为每个工具调用其 vtable->schema_json 获取
 *   工具的 JSON Schema 定义，以及 vtable->name/vtable->description
 *   获取名称和描述，拼装生成符合 OpenAI Function Calling 格式的
 *   完整 tools JSON 数组字符串。
 *
 * 生成的 JSON 格式（OpenAI / Anthropic 兼容）：
 *   [
 *     {
 *       "type": "function",
 *       "function": {
 *         "name": "file_read",                    ← 工具名称
 *         "description": "Read a file from disk", ← 工具描述（可选）
 *         "parameters": {                         ← 工具的 JSON Schema 定义
 *           "type": "object",
 *           "properties": {
 *             "path": {"type": "string", "description": "File path"}
 *           },
 *           "required": ["path"]
 *         }
 *       }
 *     },
 *     { ... 第二个工具 ... }
 *   ]
 *
 *   这个 JSON 数组被 LLM Adapter 嵌入到 API 请求的 "tools" 字段中，
 *   告诉 LLM 有哪些工具可用（名称、用途、参数格式）。
 *   LLM 根据这些 Schema 决定是否以及如何使用工具。
 *
 * 参数：
 *   @param registry       — 注册表，不可为 NULL
 *   @param out_tools_json  — [out] 输出参数，指向新分配的 JSON 字符串。
 *                            调用者负责通过 free(*out_tools_json) 释放。
 *                            如果注册表为空返回 "[]"，如果所有工具跳过了也返回 "[]"。
 *
 * @return CC_OK 表示构建成功，*out_tools_json 指向完整 JSON 数组。
 * @return CC_ERR_INVALID_ARGUMENT 表示 registry 为 NULL。
 *         注意：即使内存分配失败也不会返回错误——cc_string_builder
 *         内部处理 OOM 情况，最终 take 返回 NULL 或部分字符串。
 *
 * 实现细节：
 *   - 跳过没有 schema_json 虚函数的工具（静默忽略，不报错）。
 *     为什么静默忽略：某些工具可能只是注册占位，不具备实际的 LLM 调用能力，
 *     不出现在 Schema 中是合理的。如果报错，调用者需要逐个检查每个工具，
 *     增加了不必要的负担。
 *   - 跳过 schema_json 返回 NULL 的工具（同上，静默忽略）。
 *   - 跳过没有 vtable 的工具（静默忽略，防止空指针崩溃）。
 *   - 通过 cc_string_builder 增量构建 JSON——先 append "["，
 *     逐个工具追加 JSON 对象（逗号分隔），最后 append "]"。
 *   - 使用 first 标志控制逗号分隔：首个元素前不加逗号，后续元素前加逗号。
 *     这种方式避免了"尾部多余逗号"或"首部多余逗号"的问题，且代码简洁。
 *   - cc_string_builder_take 提取最终字符串并将所有权转移给调用者，
 *     同时释放 cc_string_builder 的内部缓冲区。
 */
cc_result_t cc_tool_registry_build_schema_json(
    cc_tool_registry_t *registry,
    char **out_tools_json
)
{
    if (!registry) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null registry");

    cc_string_builder_t sb;
    cc_string_builder_init(&sb);
    cc_string_builder_append(&sb, "[");

    int first = 1;  /* 逗号分隔标志：首个元素前不需要逗号 */
    cc_mutex_lock(registry->mutex);
    for (size_t i = 0; i < registry->count; i++) {
        cc_tool_t *tool = &registry->tools[i];

        /* 跳过没有 schema_json 虚函数的工具——它们不会出现在 LLM 的工具列表中 */
        if (!tool->vtable || !tool->vtable->schema_json) continue;

        const char *schema = tool->vtable->schema_json(tool->self);
        if (!schema) continue;  /* schema_json 返回 NULL，跳过 */

        const char *name = tool->vtable->name ? tool->vtable->name(tool->self) : NULL;
        const char *desc = tool->vtable->description ? tool->vtable->description(tool->self) : NULL;

        if (!first) cc_string_builder_append(&sb, ",");
        first = 0;

        /* 构建单个工具的 OpenAI function 定义对象 */
        cc_string_builder_append(&sb, "{");
        cc_string_builder_appendf(&sb, "\"type\":\"function\",");
        cc_string_builder_appendf(&sb, "\"function\":{");
        cc_string_builder_appendf(&sb, "\"name\":\"%s\"", name ? name : "unknown");
        if (desc) cc_string_builder_appendf(&sb, ",\"description\":\"%s\"", desc);
        cc_string_builder_appendf(&sb, ",\"parameters\":%s", schema);
        cc_string_builder_append(&sb, "}}");
    }
    cc_mutex_unlock(registry->mutex);

    cc_string_builder_append(&sb, "]");

    /* take 将内部缓冲区的所有权转移给调用者，同时释放 sb 自身。
       调用者获得一个 malloc 分配的字符串，需要最终调用 free 释放。 */
    *out_tools_json = cc_string_builder_take(&sb);
    return cc_result_ok();
}

/*
 * cc_tool_registry_count — 获取已注册的工具数量
 *
 * 功能：
 *   返回注册表中当前已注册的工具数量。实现为简单的 O(1) 成员读取。
 *   主要用于调试、日志输出和 UI 展示（如"已加载 12 个工具"）。
 *
 * 参数：
 *   @param registry — 注册表指针，可以为 NULL
 *
 * @return 已注册的工具数量。如果 registry 为 NULL，返回 0
 *         （安全的空注册表行为，调用者无需额外的 NULL 检查）。
 *
 * 线程安全：
 *   加锁读取 count，确保在并发环境中获取一致的值。
 */
size_t cc_tool_registry_count(cc_tool_registry_t *registry)
{
    if (!registry) return 0;
    cc_mutex_lock(registry->mutex);
    size_t count = registry->count;
    cc_mutex_unlock(registry->mutex);
    return count;
}

/*
 * cc_tool_registry_list_names — 获取所有已注册工具的名称列表
 *
 * 功能：
 *   遍历注册表，为每个工具调用 vtable->name 获取名称字符串（通过
 *   strdup 拷贝），将所有名称收集到动态分配的字符串数组中。
 *   输出包含两个部分：一个 char* 数组（每个元素是 strdup 的名称），
 *   和一个 count 值指示数组长度。
 *
 *   此函数与 build_schema_json 的区别：
 *   - build_schema_json：生成 JSON Schema（重量级，用于 LLM API 请求）
 *   - list_names：只获取名称列表（轻量级，用于 UI 展示、调试输出）
 *   获取名称列表不需要生成完整的 JSON Schema，效率更高。
 *
 * 参数：
 *   @param registry  — 注册表，不可为 NULL
 *   @param out_names — [out] 输出参数，指向字符串数组。
 *                      调用者负责释放：
 *                        1. 先遍历 free(names[i])（每个 strdup 的元素）
 *                        2. 再 free(names)（数组本身）
 *                      注意：未设置名称的工具在数组中对应位置为 NULL，
 *                      free(NULL) 是安全的。
 *   @param out_count — [out] 输出参数，指向数组长度（即注册表中的工具数量）。
 *
 * @return CC_OK 表示成功。
 * @return CC_ERR_INVALID_ARGUMENT 表示 registry 为 NULL。
 * @return CC_ERR_OUT_OF_MEMORY 表示 calloc 或 strdup 分配失败。
 *
 * 典型使用场景：
 *   size_t count = 0;
 *   char **names = NULL;
 *   cc_tool_registry_list_names(reg, &names, &count);
 *   printf("Available tools (%zu):\n", count);
 *   for (size_t i = 0; i < count; i++) {
 *       printf("  - %s\n", names[i] ? names[i] : "(unnamed)");
 *   }
 *   // 释放
 *   for (size_t i = 0; i < count; i++) free(names[i]);
 *   free(names);
 */
cc_result_t cc_tool_registry_list_names(
    cc_tool_registry_t *registry,
    char ***out_names,
    size_t *out_count
)
{
    if (!registry) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null registry");

    cc_mutex_lock(registry->mutex);
    size_t count = registry->count;

    /* calloc 分配 char* 数组，零初始化确保未设置名称的工具对应 NULL */
    char **names = calloc(count, sizeof(char *));
    if (!names) {
        cc_mutex_unlock(registry->mutex);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate names");
    }

    for (size_t i = 0; i < count; i++) {
        cc_tool_t *tool = &registry->tools[i];
        /* 防御性检查：确保 vtable 和 name 函数存在后才获取名称 */
        if (tool->vtable && tool->vtable->name) {
            names[i] = strdup(tool->vtable->name(tool->self));
            /* 如果 strdup 返回 NULL（OOM），names[i] 保持 calloc 的 NULL，
               调用者需要在释放时处理 NULL 条目 */
        }
    }
    cc_mutex_unlock(registry->mutex);

    *out_names = names;
    *out_count = count;
    return cc_result_ok();
}
