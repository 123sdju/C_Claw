/**
 * cc_tool_registry_snapshot.h — 工具注册表快照。
 *
 * 所属层次：核心 SDK。
 *
 * 热重载的关键不是“直接改全局 registry”，而是发布一个新的 generation：
 *   - 新 run acquire 最新 snapshot。
 *   - 已经开始的 run 持有旧 snapshot 的引用。
 *   - reload 失败时不替换 snapshot，旧 generation 继续服务。
 *
 * 本模块只做引用计数和 generation 标识，不负责启动 plugin worker 或 watcher。
 * 那些能力依赖进程/文件系统，属于 POSIX/Windows app 层；ESP 可以完全不编译。
 */

#ifndef CC_TOOL_REGISTRY_SNAPSHOT_H
#define CC_TOOL_REGISTRY_SNAPSHOT_H

#include "cc/core/cc_result.h"
#include "cc/ports/cc_tool_registry.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cc_tool_registry_snapshot cc_tool_registry_snapshot_t;

cc_result_t cc_tool_registry_snapshot_create(
    cc_tool_registry_t *registry,
    unsigned long generation,
    int owns_registry,
    cc_tool_registry_snapshot_t **out_snapshot
);

cc_tool_registry_snapshot_t *cc_tool_registry_snapshot_acquire(
    cc_tool_registry_snapshot_t *snapshot
);

void cc_tool_registry_snapshot_release(cc_tool_registry_snapshot_t *snapshot);

cc_tool_registry_t *cc_tool_registry_snapshot_registry(
    cc_tool_registry_snapshot_t *snapshot
);

unsigned long cc_tool_registry_snapshot_generation(
    cc_tool_registry_snapshot_t *snapshot
);

#ifdef __cplusplus
}
#endif

#endif
