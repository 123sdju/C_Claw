/**
 * 学习导读：cclaw/core/src/app/cc_tool_registry_snapshot.c
 *
 * 所属层次：核心层。
 * 阅读重点：热重载工具集在读侧的并发保护——通过 generation（版本号）和
 *          ref_count（引用计数）实现"新 run 拿最新 snapshot，老 run 继续用
 *          旧 snapshot"。理解 acquire/release 对 ref_count 的原子管理以及
 *          owns_registry 标志对 registry 生命周期所有权的区分。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * cc_tool_registry_snapshot.c — 工具注册表热重载读侧保护模块
 *
 * 本模块在整体架构中的角色：
 * ─────────────────────────────
 * 位于 Core 层（与平台无关），为工具注册表的热重载提供读侧并发安全。
 * 热重载的关键不是"直接改全局 registry"，而是发布一个新的 generation：
 *   - 新 run 通过 acquire() 获取最新 snapshot
 *   - 已经开始的 run 持有旧 snapshot 的引用，不受 reload 影响
 *   - reload 失败时不替换 snapshot，旧 generation 继续服务
 *
 * 本模块只做引用计数和 generation 标识，不负责启动 plugin worker 或
 * watcher——那些能力依赖进程/文件系统，属于 POSIX/Windows app 层。
 *
 * 上游调用方：
 *   - app 层的 runtime_builder（如 cc_agent_runtime）—— 成功构建新
 *     registry 后创建 snapshot 并发布；每个 run 开始时 acquire 当前
 *     snapshot，结束时 release
 *
 * 下游依赖模块：
 *   - cc_tool_registry（cc/ports/cc_tool_registry.h）—— 工具注册表抽象，
 *     当 owns_registry=1 时由 snapshot 负责销毁
 *   - cc_thread.c（cc_mutex_t）—— 保护 ref_count 的并发修改
 *
 * ─── 结构体字段说明 ────────────────────────────────────────────────────
 *
 *   cc_tool_registry_snapshot 内部字段：
 *     registry      —— 本 generation 对应的工具注册表指针
 *     generation    —— 无符号版本号，每次 reload 成功递增
 *     owns_registry —— 是否拥有 registry 生命周期（release 到 ref_count==0
 *                      时，若此标志为真则调用 cc_tool_registry_destroy）
 *     ref_count     —— 引用计数，受 mutex 保护。当前正在使用此 snapshot 的
 *                      run 数量。创建时为 1（发布者持有首引用）
 *     mutex         —— 保护 ref_count 读写，避免 reload/destroy 与 run
 *                      acquire/release 并发竞争
 *
 * ─── 引用计数生命周期 ──────────────────────────────────────────────────
 *
 *   create():           ref_count = 1  ← 发布者持有首引用
 *   run begin: acquire() → ref_count++
 *   run end:   release() → ref_count--
 *   reload:    发布者创建新 snapshot，然后 release 旧 snapshot
 *              若此时没有 run 持有旧 snapshot（ref_count == 0），销毁
 *
 * ─── 关键设计决策 ──────────────────────────────────────────────────────
 *
 *   - 为什么用 generation 而不是指针比较？
 *     指针比较只能判断"是不是同一个对象"，无法判断"哪个更新"。
 *     generation 是一个单调递增的版本号，让调用方可以轻易判断
 *     "当前 run 用的工具集是否已经过期"。
 *
 *   - owns_registry 为什么要区分？
 *     某些场景下 registry 的生命周期不由 snapshot 管理（例如 ESP32
 *     静态编译的工具注册表），此时 owns_registry=0，release 到
 *     ref_count==0 时只销毁 snapshot 自身。
 */

#include "cc/app/cc_tool_registry_snapshot.h"
#include "cc/ports/cc_thread.h"

#include <stdlib.h>

/*
 * Snapshot 是热重载的读侧保护。runtime_builder 成功构建新 registry 后发布新
 * generation；已经开始的 run acquire 旧 snapshot 并在 run 结束 release。
 *
 * ref_count 只保护 snapshot 对象和它拥有的 registry 生命周期，不让 app 层
 * plugin reload 直接影响正在运行的工具调用。
 */
struct cc_tool_registry_snapshot {
    cc_tool_registry_t *registry;
    unsigned long generation;
    int owns_registry;
    size_t ref_count;
    cc_mutex_t mutex;
};

cc_result_t cc_tool_registry_snapshot_create(
    cc_tool_registry_t *registry,
    unsigned long generation,
    int owns_registry,
    cc_tool_registry_snapshot_t **out_snapshot
)
{
    if (!registry || !out_snapshot) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid tool registry snapshot");
    }
    cc_tool_registry_snapshot_t *snapshot = calloc(1, sizeof(cc_tool_registry_snapshot_t));
    if (!snapshot) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create registry snapshot");
    snapshot->registry = registry;
    snapshot->generation = generation;
    snapshot->owns_registry = owns_registry ? 1 : 0;
    snapshot->ref_count = 1;
    cc_result_t rc = cc_mutex_create(&snapshot->mutex);
    if (rc.code != CC_OK) {
        free(snapshot);
        return rc;
    }
    *out_snapshot = snapshot;
    return cc_result_ok();
}

cc_tool_registry_snapshot_t *cc_tool_registry_snapshot_acquire(
    cc_tool_registry_snapshot_t *snapshot
)
{
    if (!snapshot) return NULL;
    /* ref_count 由 mutex 保护，避免 reload/destroy 与 run acquire/release 竞争。 */
    cc_mutex_lock(snapshot->mutex);
    snapshot->ref_count++;
    cc_mutex_unlock(snapshot->mutex);
    return snapshot;
}

void cc_tool_registry_snapshot_release(cc_tool_registry_snapshot_t *snapshot)
{
    if (!snapshot) return;
    int destroy = 0;
    cc_mutex_lock(snapshot->mutex);
    if (snapshot->ref_count > 0) snapshot->ref_count--;
    destroy = snapshot->ref_count == 0;
    cc_mutex_unlock(snapshot->mutex);
    if (!destroy) return;

    if (snapshot->owns_registry) {
        cc_tool_registry_destroy(snapshot->registry);
    }
    cc_mutex_destroy(snapshot->mutex);
    free(snapshot);
}

cc_tool_registry_t *cc_tool_registry_snapshot_registry(
    cc_tool_registry_snapshot_t *snapshot
)
{
    return snapshot ? snapshot->registry : NULL;
}

unsigned long cc_tool_registry_snapshot_generation(
    cc_tool_registry_snapshot_t *snapshot
)
{
    return snapshot ? snapshot->generation : 0;
}
