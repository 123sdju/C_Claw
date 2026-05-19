/**
 * 学习导读：cclaw/ports/include/cc/ports/cc_tool.h
 *
 * 所属层次：端口层。
 * 阅读重点：这里定义可替换接口，阅读时重点看 struct + vtable + void *self 如何表达多态和依赖注入。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * cc_tool.h — 工具抽象端口（Port）
 *
 * @file    cc/ports/cc_tool.h
 * @brief   定义 Agent 可调用工具的抽象接口和上下文。采用 vtable 多态模式。
 *
 * 在 c-claw 的端口-适配器架构中，本模块定义的是"端口"（Port）：
 * 它规定了工具行为的抽象合约，但不提供具体实现。具体工具的创建
 * 通过工厂函数返回填充了 vtable 的 cc_tool_t 实例。
 *
 * ─── 接口契约 ─────────────────────────────────────────────────────────
 *
 *   每个 cc_tool_t 由两个部分组成：
 *     - self      : 指向具体工具实现的私有数据
 *     - vtable    : 指向虚函数表，定义了工具的公共接口
 *
 *   实现者必须填充 vtable 中的所有函数指针，并提供 name/description/schema
 *   等元数据。调用者只依赖 vtable，不感知具体工具类型。
 *
 * ─── 依赖 ─────────────────────────────────────────────────────────────
 *
 *   依赖 cc/core/cc_result.h（错误传递）和 cc/core/cc_tool_call.h（数据模型）。
 */

#ifndef CC_TOOL_H
#define CC_TOOL_H

#include "cc/core/cc_result.h"
#include "cc/core/cc_tool_call.h"

/**
 * cc_event_bus_t — 前向声明的端口/服务句柄类型，具体字段在本文件后文或对应端口中定义。
 */
typedef struct cc_event_bus cc_event_bus_t;
/**
 * cc_logger_t — 前向声明的端口/服务句柄类型，具体字段在本文件后文或对应端口中定义。
 */
typedef struct cc_logger cc_logger_t;
/**
 * cc_memory_store — 长期记忆端口的前向声明。
 *
 * 工具上下文只借用 memory store 指针，具体结构定义在 memory store 端口头文件中。
 */
typedef struct cc_memory_store cc_memory_store_t;

/**
 * cc_tool_approval_fn — 工具调用人工审批回调。
 *
 * policy engine 判定需要人工确认时，tool executor 会调用该函数。回调只读取
 * tool_name/arguments_json/reason；user_data 由 gateway 注入并负责生命周期。
 *
 * @return 非 0 表示允许继续执行工具，0 表示拒绝。
 */
typedef int (*cc_tool_approval_fn)(
    const char *tool_name,
    const char *arguments_json,
    const char *reason,
    void *user_data
);

/**
 * cc_runtime_services — 暴露给工具的受限 runtime 服务集合，避免工具直接持有完整 runtime。
 *
 * 该结构体不拥有任何字段。runtime 在执行工具时临时提供它，让工具可以发布事件、
 * 写日志、访问长期记忆或请求人工审批，同时避免工具拿到完整 runtime。
 */
typedef struct cc_runtime_services {
    /** 可选事件总线；工具可用它发布细粒度进度事件。 */
    cc_event_bus_t *event_bus;
    /** 可选 logger；工具可用它输出诊断日志。 */
    cc_logger_t *logger;
    /** 可选长期记忆存储；为 NULL 表示当前 profile 未启用记忆工具链。 */
    cc_memory_store_t *memory_store;
    /** 可选人工审批回调；高风险工具可在执行前调用。 */
    cc_tool_approval_fn approve_tool_call;
    /** 传给 approve_tool_call 的调用方上下文。 */
    void *approval_user_data;
} cc_runtime_services_t;

/**
 * cc_tool_context_t — 工具调用上下文
 *
 * 在工具执行时传入，提供工具需要的环境信息。
 * 只要某个操作需要知道"谁在什么会话、什么目录下调用的我"，
 * 就可以从这个结构中获取。
 */
typedef struct cc_tool_context {
    const char *session_id;   /**< 当前会话 ID，用于关联存储操作 */
    const char *workspace_dir; /**< 当前工作区目录，文件操作应限定在此路径下 */
    const char *user_id;      /**< 调用用户的标识符（留作将来扩展） */
    const cc_runtime_services_t *services; /**< 受限运行时服务，避免工具拿到完整 runtime */
} cc_tool_context_t;

/* ── 前向声明 ───────────────────────────────────────────────────────── */

typedef struct cc_tool_vtable cc_tool_vtable_t;
/**
 * cc_tool_t — 前向声明的端口/服务句柄类型，具体字段在本文件后文或对应端口中定义。
 */
typedef struct cc_tool cc_tool_t;

/**
 * cc_tool_t — 工具实例（多态句柄）
 *
 * 这是一个值语义的结构体，通过 self + vtable 实现多态。
 * 可以直接按值传递和拷贝，浅拷贝后两个实例指向同一个底层工具对象。
 * 生命周期由创建者管理，销毁时通过 vtable->destroy 释放 self。
 */
struct cc_tool {
    void *self;                    /**< 指向具体工具实现的私有数据。
                                   *   由工厂函数分配，类型因具体工具而异 */
    const cc_tool_vtable_t *vtable; /**< 虚函数表指针，定义了工具的所有行为 */
};

/**
 * cc_tool_vtable_t — 工具虚函数表
 *
 * 定义工具的抽象接口。每个具体的工具实现必须填充此表。
 * 类似于面向对象语言中的接口/抽象类。
 */
struct cc_tool_vtable {
    /**
     * name — 获取工具名称
     *
     * 用于工具注册表的索引和 LLM 的工具选择。
     * 名称应对 LLM 友好，使用下划线分隔的小写英文（如 "file_read"）。
     *
     * @param self  工具私有数据
     * @return      工具名称字符串（静态常量，不需要释放）
     */
    const char *(*name)(void *self);

    /**
     * description — 获取工具描述
     *
     * 用于生成发送给 LLM 的工具 schema。描述应说明工具的功能、
     * 使用场景和注意事项，帮助 LLM 正确选择工具。
     *
     * @param self  工具私有数据
     * @return      工具描述字符串（静态常量，不需要释放）
     */
    const char *(*description)(void *self);

    /**
     * schema_json — 获取工具参数 JSON Schema
     *
     * 返回符合 JSON Schema 规范的参数字符串，定义工具接受的参数
     * 名称、类型、是否必填、默认值等。LLM 据此生成合法的调用参数。
     *
     * @param self  工具私有数据
     * @return      JSON Schema 字符串（静态常量，不需要释放）
     */
    const char *(*schema_json)(void *self);

    /**
     * call — 执行工具调用
     *
     * 工具调用的核心函数。解析 args_json 参数，执行工具逻辑，
     * 将结果写入 out_result。
     *
     * @param self        工具私有数据
     * @param args_json   调用参数（JSON 格式字符串），格式由 schema_json 定义
     * @param ctx         调用上下文（会话 ID、工作区等环境信息）
     * @param out_result  输出：工具执行结果（调用者负责 cc_tool_result_destroy）
     * @return            CC_OK 表示执行成功
     */
    cc_result_t (*call)(
        void *self,
        const char *args_json,
        const cc_tool_context_t *ctx,
        cc_tool_result_t *out_result
    );

    /**
     * destroy — 销毁工具实例
     *
     * 释放 self 指向的私有数据及工具持有的所有资源。
     * 如果工具没有需要释放的资源（如纯函数工具），此字段可为 NULL。
     *
     * @param self  工具私有数据
     */
    void (*destroy)(void *self);
};

#endif
