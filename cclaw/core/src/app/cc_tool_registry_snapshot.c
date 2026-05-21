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
