/**
 * cc_skill_catalog.h — AgentSkills 风格 SKILL.md 目录索引。
 *
 * 所属层次：核心 SDK。
 *
 * catalog 只负责“把一组目录里的 SKILL.md 读成可注入 prompt 的快照”：
 *   - 不启动 watcher，不访问平台专用 API，因此 ESP 可以裁剪 watcher 后保留静态 skills。
 *   - 同名 skill 后加载覆盖先加载，用确定性顺序表达优先级。
 *   - prompt 构建时可以传 allowlist，让每个 agent 只看到自己允许的 skill。
 */

#ifndef CC_SKILL_CATALOG_H
#define CC_SKILL_CATALOG_H

#include "cc/core/cc_result.h"
#include "cc/ports/cc_filesystem.h"
#include "cc/util/cc_config.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cc_skill_catalog cc_skill_catalog_t;

cc_result_t cc_skill_catalog_create(cc_skill_catalog_t **out_catalog);

void cc_skill_catalog_destroy(cc_skill_catalog_t *catalog);

cc_result_t cc_skill_catalog_load_from_config(
    cc_skill_catalog_t *catalog,
    cc_filesystem_t *filesystem,
    const cc_config_t *config
);

cc_result_t cc_skill_catalog_build_prompt(
    cc_skill_catalog_t *catalog,
    const cc_config_string_list_t *allowlist,
    char **out_prompt
);

cc_result_t cc_skill_catalog_list_names(
    cc_skill_catalog_t *catalog,
    char ***out_names,
    size_t *out_count
);

#ifdef __cplusplus
}
#endif

#endif
