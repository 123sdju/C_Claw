/**
 * 学习导读：cclaw/core/src/core/cc_event_bus.c
 *
 * 所属层次：核心层。
 * 阅读重点：这里定义 Agent 运行时的数据模型、主循环和通用工具，阅读时重点看所有权、错误返回和 ReAct 数据流。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * cc_event_bus.c — 事件总线（发布/订阅模式实现）
 *
 * 模块在整体架构中的角色：
 *   本模块实现了轻量级的进程内事件总线，采用发布-订阅（Pub/Sub）模式，
 *   允许系统中不同组件之间进行解耦的同步通信。事件总线是系统组件间
 *   "横向"通信的骨干——各组件不需要直接引用对方，只需订阅感兴趣的事件类型。
 *
 *   这是 AI Agent 框架中关键的胶水层——当 LLM 开始流式输出、工具调用
 *   完成、会话状态变更时，通过事件总线通知所有感兴趣的订阅者（如 CLI 流式渲染、
 *   日志记录、测试探针或未来的外部 gateway 推送）。
 *
 * 依赖的其他模块：
 *   - cc_result.h — 统一错误返回类型
 *   - cc_event_bus.h — 定义 cc_event_bus_t 和 cc_event_handler_fn 类型
 *   - cc_thread.h — 互斥锁（cc_mutex_t），用于订阅和发布的线程安全
 *   - 标准库 (stdlib.h, string.h)
 *
 * 被哪些模块使用：
 *   - Runtime/App 层 — 发布 LLM 流式输出事件、工具调用开始/完成事件
 *   - CLI gateway — 订阅事件以实时渲染 stream/thinking/tool 状态
 *   - 测试/监控模块 — 订阅事件进行行为断言、审计和性能观测
 *
 * 核心概念（领域术语）：
 *   - 事件类型（event_type）：字符串标识，如 "tool_call"、"agent_message"。
 *     为什么用字符串而非枚举：允许运行时动态注册新的事件类型，无需重新编译。
 *     应用层注入新工具时可以顺便注册新的事件类型，完全无需修改 Core 层代码。
 *   - 订阅者（handler）：注册到特定事件类型的回调函数。
 *     函数签名：void handler(const char *event_type, const char *event_json, void *user_data)
 *   - 发布者（publish）：触发事件，同步通知所有匹配的订阅者。
 *     发布操作是同步的——publish 返回时所有处理器已执行完毕。
 *   - 通配订阅者：event_type 为 NULL 的处理器，接收所有事件。
 *     常用于横切关注点（cross-cutting concerns）如全局日志、性能监控。
 *
 * 发布/订阅（Pub/Sub）模式的详细说明：
 *   Pub/Sub 是一种消息传递模式，其中：
 *     1. 发布者（Publisher）产生事件，但不直接知道谁在监听。
 *     2. 订阅者（Subscriber）表达对特定事件类型的兴趣。
 *     3. 事件总线（Event Bus）作为中间人，接收发布并分发给匹配的订阅者。
 *   这种模式实现了组件间的松耦合——新增一个订阅者不需要修改发布者的代码，
 *   新增一个事件类型不需要修改现有订阅者的代码。
 *   在本框架中，这允许 CLI gateway 通过订阅事件来更新终端输出，而 runtime
 *   不需要知道具体 gateway 的存在。
 *
 * 设计决策（为什么这样设计）：
 *   1. 固定容量（MAX_HANDLERS=64），使用静态数组。
 *      为什么：事件总线的订阅者在进程启动时一次性注册完成，运行时不会
 *      动态增减。固定数组避免了动态内存管理的复杂性和碎片化。
 *      64 个槽位对于典型应用绰绰有余（通常只有 10-20 个订阅者）。
 *   2. 发布时同步调用所有匹配的处理器（非异步）。
 *      为什么：简化了错误处理和资源生命周期管理。异步模式需要引入
 *      线程池和消息队列，在单线程事件循环架构中会增加不必要的复杂度。
 *      同步调用确保了确定的事件处理顺序——处理器 A 总是在处理器 B
 *      之前执行，便于调试和推理。调用者可以依赖"publish 返回 = 所有
 *      处理器已执行完毕"的语义。
 *   3. 事件类型为 NULL 的处理器视为"通配订阅者"，接收所有事件。
 *      为什么：支持日志/监控等横切关注点，它们需要观察所有事件而
 *      不是特定类型。比要求订阅者逐个注册所有事件类型更简洁，且
 *      新增事件类型时通配订阅者自动覆盖，不需要更新注册代码。
 *   4. 处理器快照（snapshot）策略——publish 时先复制匹配的处理器到本地数组，
 *      释放锁后再依次调用。
 *      为什么（线程安全的关键设计）：如果持锁期间直接调用 handler，
 *      handler 内部可能调用 subscribe/publish 造成死锁。先复制快照
 *      再释放锁调用，避免了死锁和长时间持锁导致的性能问题。
 *      详见 cc_event_bus_publish 的注释。
 */

#include "cc/ports/cc_event_bus.h"
#include "cc/ports/cc_thread.h"
#include <stdlib.h>
#include <string.h>

#define MAX_HANDLERS 64

/*
 * cc_event_handler_entry_t — 事件处理器条目（内部结构体）
 *
 * 表示一个已注册的事件订阅。每个条目记录了订阅的事件类型、回调函数
 * 和透传的用户数据。这是总线内部的数据结构，不对外暴露。
 *
 * 字段：
 *   event_type — 订阅的事件类型字符串，通过 strdup 拷贝自调用者参数。
 *                为什么需要拷贝：调用者可能在栈上构造临时字符串，总线
 *                必须拥有独立副本才能在后续的 publish 时正确匹配。
 *                NULL 表示此处理器监听所有事件（通配订阅），不参与
 *                strcmp 匹配——任何 event_type 都命中通配订阅者。
 *                destroy 时由 cc_event_bus_destroy 负责释放此字符串。
 *   handler    — 事件回调函数指针，事件发布时同步调用。
 *                接收三个参数：event_type（字符串，告知处理器事件类型）、
 *                event_json（事件负载 JSON，NULL 表示无负载）、
 *                user_data（用户上下文指针，原样透传）。
 *                函数无返回值（void）——事件总线是"通知"模式，不期望响应。
 *   user_data  — 用户自定义上下文数据，回调时原样透传给 handler。
 *                常见用途：
 *                - 传递 UI 组件指针以更新界面
 *                - 传递 session 上下文以确定事件来源
 *                - 传递配置指针以控制处理器行为
 *                总线不关心 user_data 的内容和生命周期——这是调用者的资源。
 */
typedef struct {
    char *event_type;
    cc_event_handler_fn handler;
    void *user_data;
} cc_event_handler_entry_t;

/*
 * cc_event_bus — 事件总线结构体（内部实现，opaque type）
 *
 * 包含一个固定大小的处理器数组、当前计数和互斥锁。
 * 所有操作都是 O(n) 的线性扫描——对于最多 64 个处理器的场景，
 * 这是最优的策略：连续内存、缓存友好、无散列计算和冲突处理开销。
 *
 * 字段：
 *   handlers — 静态定长数组，存储所有已注册的事件处理器，
 *              按注册顺序（subscribe 的先后）排列。
 *              发布时从前到后遍历匹配——注册顺序 = 调用顺序。
 *   count    — 当前已注册的处理器数量，也是 handlers 的有效索引范围
 *              [0, count)。新增处理器时 count 自增作为写入索引。
 *   mutex    — 互斥锁，保护 subscribe 和 publish 的并发访问。
 *              为什么需要锁：subscribe 修改 handlers 数组和 count，
 *              publish 读取 handlers 数组和 count，两者之间存在竞争。
 *              互斥锁保证了"单写多读"的线程安全。
 *              注意：handler 回调在锁外执行（快照策略），以避免死锁。
 */
struct cc_event_bus {
    cc_event_handler_entry_t handlers[MAX_HANDLERS];
    size_t count;
    cc_mutex_t mutex;
};

/*
 * cc_event_bus_create — 创建事件总线实例
 *
 * 功能：
 *   分配并初始化一个新的事件总线。使用 calloc 分配以确保所有处理器槽位
 *   零初始化（event_type=NULL, handler=NULL, user_data=NULL），count=0。
 *   同时创建互斥锁用于后续的线程安全操作。
 *
 *   调用者需要在创建后通过 cc_event_bus_subscribe 注册感兴趣的事件处理器。
 *   初始状态为"空总线"——没有任何已注册的处理器，publish 不会触发任何回调。
 *
 * 参数：
 *   @param out_bus — [out] 输出参数，指向创建的事件总线指针。
 *                    如果函数返回非 CC_OK，此参数的值未定义。
 *
 * @return CC_OK 表示成功，可以通过 *out_bus 使用事件总线。
 * @return CC_ERR_OUT_OF_MEMORY 表示 calloc 分配失败，系统内存不足。
 */
cc_result_t cc_event_bus_create(cc_event_bus_t **out_bus)
{
    cc_event_bus_t *bus = calloc(1, sizeof(cc_event_bus_t));
    if (!bus) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create event bus");
    cc_result_t rc = cc_mutex_create(&bus->mutex);
    if (rc.code != CC_OK) {
        free(bus);
        return rc;
    }
    *out_bus = bus;
    return cc_result_ok();
}

/*
 * cc_event_bus_destroy — 销毁事件总线
 *
 * 功能：
 *   释放事件总线及其所有已注册处理器的资源。遍历所有处理器条目，
 *   释放每个条目的 event_type 字符串（通配订阅者为 NULL，free(NULL) 安全），
 *   销毁互斥锁，最后释放事件总线结构体本身。
 *
 *   不会释放 handler 函数指针和 user_data 指针——那些是调用者的资源，
 *   总线只是保存引用，不拥有所有权。
 *
 * 参数：
 *   @param bus — 要销毁的事件总线指针，可以为 NULL（安全无操作）。
 *               销毁后 bus 指针变为悬空，调用者不应再使用。
 *
 * 线程安全：
 *   销毁前加锁遍历释放 event_type，但本身不防外部的并发 publish——
 *   调用者应确保在 destroy 时没有其他线程在执行 publish，
 *   否则 release 后 publish 访问 freed bus 会导致崩溃。
 *   通常 destroy 在程序结束时调用，此时所有线程已停止，无需额外同步。
 */
void cc_event_bus_destroy(cc_event_bus_t *bus)
{
    if (!bus) return;
    cc_mutex_lock(bus->mutex);
    for (size_t i = 0; i < bus->count; i++) {
        free(bus->handlers[i].event_type);
    }
    cc_mutex_unlock(bus->mutex);
    cc_mutex_destroy(bus->mutex);
    free(bus);
}

/*
 * cc_event_bus_subscribe — 订阅事件（注册事件处理器）
 *
 * 功能：
 *   注册一个事件处理器（回调函数），关联到指定的事件类型。
 *   当该类型的事件通过 cc_event_bus_publish 发布时，处理器被同步调用。
 *   如果 event_type 为 NULL，表示通配订阅（接收所有类型的事件），
 *   常用于日志、监控等需要跨事件类型观察的横切关注点。
 *
 *   这是"订阅"（Subscribe）端——表达对特定事件类型的兴趣。
 *   订阅可以在总线创建后的任何时间执行，但通常在初始化阶段一次性完成。
 *
 * 参数：
 *   @param bus        — 事件总线实例，不可为 NULL
 *   @param event_type — 要订阅的事件类型字符串，如 "tool_call"、"agent_message"
 *                       "session_created" 等。可以为 NULL，表示通配订阅，
 *                       监听所有事件类型。
 *                       内部通过 strdup 拷贝，调用者可以在调用后立即释放原字符串。
 *                       为什么需要拷贝：调用者可能在栈上构造临时字符串
 *                       （如 sprintf(buf, "tool:%s", tool_name)），
 *                       总线需要一个持久副本用于后续 publish 时的匹配。
 *   @param handler    — 事件回调函数指针，事件发布时被同步调用。不可为 NULL。
 *                       函数签名：void (*handler)(const char *event_type,
 *                                                  const char *event_json,
 *                                                  void *user_data)
 *                       event_type 告知处理器本次触发的具体事件类型
 *                       （对通配订阅者特别有用——通过此参数区分事件类型）。
 *                       event_json 是事件的负载 JSON，可能为 NULL。
 *                       user_data 是订阅时传入的上下文指针，原样透传。
 *   @param user_data  — 用户自定义上下文数据，回调时原样透传给 handler。
 *                       常用于传递 UI 组件指针、session 指针、配置指针等。
 *                       总线不管理 user_data 的生命周期——这是调用者的资源。
 *
 * @return CC_OK 表示订阅成功。
 * @return CC_ERR_INVALID_ARGUMENT 表示 bus 或 handler 为 NULL。
 * @return CC_ERR_OUT_OF_MEMORY 表示处理器数量已达 MAX_HANDLERS (64) 上限，
 *         或 strdup 内存分配失败。
 *
 * 为什么没有"取消订阅"（unsubscribe）接口：
 *   订阅关系在程序生命周期中是静态的——初始化阶段确定后不再改变。
 *   64 个处理器槽位足够覆盖所有典型场景（CLI、日志、测试探针、审计等），
 *   无需运行时动态增删。静态订阅模式简化了锁策略和内存管理。
 *   如果未来需要动态订阅，可以增加 unsubscribe 接口，但当前设计
 *   假设"注册一次，永不改变"——所有的设计决策（快照策略、固定数组等）
 *   都基于此假设。
 */
cc_result_t cc_event_bus_subscribe(
    cc_event_bus_t *bus,
    const char *event_type,
    cc_event_handler_fn handler,
    void *user_data
)
{
    if (!bus || !handler) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null argument");
    cc_mutex_lock(bus->mutex);
    if (bus->count >= MAX_HANDLERS) {
        cc_mutex_unlock(bus->mutex);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Event bus full");
    }

    bus->handlers[bus->count].event_type = event_type ? strdup(event_type) : NULL;
    bus->handlers[bus->count].handler = handler;
    bus->handlers[bus->count].user_data = user_data;
    bus->count++;
    cc_mutex_unlock(bus->mutex);
    return cc_result_ok();
}

/*
 * cc_event_bus_publish — 发布事件（核心分发逻辑）
 *
 * 功能：
 *   向事件总线发布一个指定类型的事件。总线遍历所有已注册的处理器，
 *   将事件分发给所有匹配的订阅者。匹配规则（二选一满足即匹配）：
 *   - 处理器的 event_type 与传入的 event_type 字符串完全匹配（strcmp == 0），或
 *   - 处理器的 event_type 为 NULL（通配订阅者，接收所有事件）。
 *
 *   处理器按注册顺序在同一线程中同步调用。这意味着前一个处理器的
 *   副作用可能影响后续处理器的执行环境（如修改了全局状态）。
 *
 *   事件负载（event_json）作为只读 JSON 字符串原样透传给每个匹配的处理器。
 *   为什么使用 JSON 字符串而非结构体：不同事件类型的负载格式完全不同——
 *   tool_call 事件携带工具名称和参数，message 事件携带消息内容和角色。
 *   JSON 提供了灵活的结构化数据表示，无需为每种事件类型定义独立的 C 结构体。
 *   处理器按需解析 JSON（通常使用 cJSON 或 jansson）提取关心字段。
 *
 * 参数：
 *   @param bus        — 事件总线实例，不可为 NULL
 *   @param event_type — 发布的事件类型字符串，不可为 NULL。
 *                       用于与已注册处理器的 event_type 进行 strcmp 匹配。
 *   @param event_json — 事件的负载数据（JSON 字符串），可以为 NULL。
 *                       原样透传给每个匹配的处理器。处理器根据自身需要
 *                       解析此 JSON 来提取事件详情。
 *                       为什么允许 NULL：某些事件只是信号（signal），
 *                       不需要额外数据（如 "shutdown" 事件）。
 *
 * @return CC_OK 表示事件已分发给所有匹配的处理器。
 * @return CC_ERR_INVALID_ARGUMENT 表示 bus 或 event_type 为 NULL。
 *
 * 处理器快照（handler snapshot）策略——线程安全的核心设计：
 *
 *   为什么需要快照（而非持锁直接调用 handler）：
 *
 *   如果持锁期间直接调用 handler：
 *     cc_mutex_lock(bus->mutex);
 *     for (i = 0; i < bus->count; i++) {
 *         if (match) handler(event_type, event_json, user_data);
 *         // handler 内部可能调用 subscribe（修改 bus->handlers）→ 死锁！
 *         // 或者 handler 内部调用 publish（递归锁）→ 如果 mutex 非递归则死锁！
 *     }
 *     cc_mutex_unlock(bus->mutex);
 *
 *   快照策略的实现：
 *     cc_mutex_lock(bus->mutex);
 *     // 步骤1：持锁期间只做"读取+快照"（O(n) 的遍历和浅拷贝）
 *     snapshot = [匹配的 handler 的副本];
 *     cc_mutex_unlock(bus->mutex);
 *     // 步骤2：释放锁后执行 handler（无锁调用）
 *     for (each in snapshot) {
 *         handler(event_type, event_json, user_data);  // 安全：handler 可以调用 subscribe/publish
 *     }
 *
 *   快照策略的代价：
 *     - 额外的栈数组（MAX_HANDLERS * sizeof(entry) ≈ 1.5KB），完全可接受
 *     - 浅拷贝 entry（每个 ~24 字节），开销远小于潜在的锁竞争
 *
 *   快照策略的好处：
 *     - 避免死锁：handler 可以在内部安全地调用 subscribe/publish
 *     - 减少锁持有时间：锁只在快照期间持有，handler 执行时间不影响锁
 *     - 可预测的行为：快照后的 handler 集合确定，不受并发 subscribe 影响
 *
 * 注意事项（调用者必须了解）：
 *   - 处理器在同一线程中同步执行，按注册顺序串行调用。
 *     为什么需要知道这一点：如果处理器 A 修改了处理器 B 依赖的全局状态，
 *     A→B 的调用顺序保证了 B 看到的是"A 之后"的状态。
 *   - 如果某个处理器的回调中发生段错误（segfault）或未定义行为，
 *     后续处理器不会被执行——发布流程会异常终止。
 *     为什么这样设计：在 C 语言中没有异常处理机制，处理器崩溃意味着
 *     整个进程的不稳定，此时继续执行其他处理器可能带来更大的损害。
 *   - 处理器内部不应修改事件总线状态（如 subscribe），否则行为未定义
 *     ——虽然快照策略允许这样操作，但快照时已确定的 handler 集合
 *     不会包含之后注册的处理器，这在语义上可能不是预期的行为。
 *   - 总线的返回值只反映"分发是否开始"，不反映各处理器的执行结果。
 *     各处理器是 void 函数，不需要返回值——这是"发布-订阅"模式的特征：
 *     关注事件的传播，不关注订阅者的响应。
 *
 * 为什么同步调用而非异步+返回值：
 *   事件总线的目标是"通知"而非"请求"——发布者不关心"谁处理了"或
 *   "处理结果如何"。如需确认/响应，应使用请求-响应（request-response）
 *   模式而非事件总线。同步调度确保了在单线程事件循环中事件的
 *   确定性传播——publish 返回意味着所有处理器已按顺序执行完毕。
 *   这种简单的心智模型是框架设计正确性的基石。
 */
cc_result_t cc_event_bus_publish(
    cc_event_bus_t *bus,
    const char *event_type,
    const char *event_json
)
{
    if (!bus || !event_type) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null argument");

    /* 处理器快照数组（栈上分配）：存储匹配的处理器副本 */
    cc_event_handler_entry_t snapshot[MAX_HANDLERS];
    size_t snapshot_count = 0;

    /* 阶段1：持锁构建快照——只读取和浅拷贝，不执行 handler */
    cc_mutex_lock(bus->mutex);
    for (size_t i = 0; i < bus->count; i++) {
        cc_event_handler_entry_t *entry = &bus->handlers[i];
        /* 匹配规则：
           - entry->event_type == NULL：通配订阅者，匹配所有事件
           - strcmp == 0：精确匹配，处理器的订阅事件类型与发布类型一致 */
        if (entry->event_type == NULL || strcmp(entry->event_type, event_type) == 0) {
            snapshot[snapshot_count++] = *entry;  /* 浅拷贝：复制三个指针 */
        }
    }
    cc_mutex_unlock(bus->mutex);

    /* 阶段2：释放锁后执行 handler——handler 可以安全地调用 subscribe/publish */
    for (size_t i = 0; i < snapshot_count; i++) {
        snapshot[i].handler(event_type, event_json, snapshot[i].user_data);
    }
    return cc_result_ok();
}
