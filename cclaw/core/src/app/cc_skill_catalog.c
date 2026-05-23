/**
 * 学习导读：cclaw/core/src/app/cc_skill_catalog.c
 *
 * 所属层次：核心层。
 * 阅读重点：SKILL.md 文件解析与 prompt 快照生成，重点看多目录加载顺序
 *          决定的同名覆盖语义、allowlist 过滤以及 entry 的字符串所有权。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * cc_skill_catalog.c — AgentSkills 风格 SKILL.md 目录索引模块
 *
 * 本模块在整体架构中的角色：
 * ─────────────────────────────
 * 位于 App 层（业务逻辑层），只负责"把一组目录里的 SKILL.md 读成可注入
 * prompt 的快照"。不启动文件 watcher、不访问平台专用 API、不关心 skill
 * 目录的发现策略——这些由 app/builder 层决定何时调用 load_from_config。
 *
 * 上游调用方：
 *   - cc_context_builder.c —— 通过 cc_skill_catalog_build_prompt 获取
 *     skill prompt 快照，注入到 LLM 上下文的 system prompt 中
 *   - app/builder 层 —— 在配置解析后调用 load_from_config 加载 skill
 *
 * 下游依赖模块：
 *   - cc_filesystem.c —— 遍历目录、读取文件内容（通过 vtable）
 *   - cc_string_builder.c —— 拼接 prompt 文本
 *   - cc_config.c —— 读取 skills.extra_dirs、plugin skill_dirs、workspace_path
 *
 * ─── 内部数据结构 ───────────────────────────────────────────────────
 *
 *   cc_skill_entry_t：
 *     单个 skill 条目。持有三个字符串（catalog 拥有所有权）：
 *     - name：skill 名称，即 SKILL.md 所在目录名
 *     - source_path：SKILL.md 文件的完整路径（用于调试）
 *     - content：SKILL.md 的文本内容
 *
 *   cc_skill_catalog（主结构体）：
 *     持有 entries 动态数组、count 和 capacity。
 *
 * ─── 同名覆盖规则 ───────────────────────────────────────────────────
 *
 *   upsert_skill 实现"后加载覆盖先加载"语义：如果 name 已存在，释放旧的
 *   source_path 和 content 并写入新值，保留旧的 name 字符串不变（避免
 *   重复分配）。如果 name 不存在，追加新 entry。
 *
 *   加载顺序从低到高优先级为：
 *     1. ~/.cclaw/skills（用户家目录，优先级最低）
 *     2. agent_dir/skills（agent 个人目录）
 *     3. plugin skill_dirs（各 plugin 配置的 skill 目录）
 *     4. config->skills.extra_dirs（配置文件额外指定）
 *     5. workspace_path/.agents/skills（项目工作区，优先级最高）
 *
 *   这个顺序确保项目级 skill 能覆盖 plugin 或用户级的同名定义。
 *
 * ─── SKILL.md 解析流程 ──────────────────────────────────────────────
 *
 *   load_skill_dir：
 *     1. 检查目录是否存在（通过 filesystem->exists）
 *     2. list_dir 获取子目录名列表
 *     3. 对每个子目录，拼接 "<子目录>/SKILL.md" 路径
 *     4. read_text 读取文件内容
 *     5. 若非空，调用 upsert_skill(name=子目录名, path=SKILL.md路径, content=内容)
 *
 * ─── Prompt Snapshot 生成 ────────────────────────────────────────────
 *
 *   build_prompt：
 *     1. 若 catalog 为空或无 entry，直接返回 OK（out_prompt=NULL）
 *     2. 用 string_builder 拼接 "[Skills]\n" 头部和说明文字
 *     3. 遍历所有 entry，用 allowlist 过滤（allowlist 为空则不过滤）
 *     4. 每个 skill 格式为 "\n## <name>\n<content>"，确保末尾有换行
 *     5. 若结果仅包含头部无实际 skill 内容，返回 NULL 而非空 prompt
 *     6. 通过 cc_string_builder_take 转移字符串所有权给调用方
 *
 * ─── 设计决策 ───────────────────────────────────────────────────────
 *
 *   为什么 catalog 拥有 name/source_path/content 三个字符串？
 *     catalog 是 skill 数据的唯一所有者。加载后原始文件和目录可能变化，
 *     catalog 的快照语义要求它持有的数据在生命周期内不变。调用方通过
 *     build_prompt 获取的 prompt 字符串由调用方负责 free。
 *
 *   为什么同名覆盖不替换 name 字符串？
 *     upsert 时如果 name 相同，保留旧的 name 指针并更新 source_path 和
 *     content。这减少了一次 free+malloc 开销，且 name 内容本就相同。
 *
 *   为什么空 content 的 SKILL.md 被视为无 skill？
 *     load_skill_dir 中 read_text 返回后检查 content 非空才调用 upsert。
 *     空文件不产生 entry，避免在 prompt 中注入无效内容。
 *
 *   为什么 allowlist 为空时不过滤？
 *     allow_skill 在 allowlist==NULL 或 count==0 时返回 1（允许全部）。
 *     这允许调用方省略 allowlist 参数以获得完整的 skill prompt。
 */

#include "cc/app/cc_skill_catalog.h"
#include "cc/util/cc_string_builder.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/*
 * Skill catalog 只实现"目录 -> SKILL.md -> prompt snapshot"的可移植语义。
 * 文件 watcher、个人目录发现策略、plugin skill dir 注入时机由 app/builder 负责。
 *
 * 同名覆盖规则通过加载顺序表达：后加载的 entry 覆盖先加载的 entry，最终 prompt
 * 只输出一个同名 skill。catalog 拥有 name/source_path/content 三个字符串。
 */
typedef struct cc_skill_entry {
    char *name;
    char *source_path;
    char *content;
} cc_skill_entry_t;

struct cc_skill_catalog {
    cc_skill_entry_t *entries;
    size_t count;
    size_t capacity;
};

static void free_entry(cc_skill_entry_t *entry)
{
    if (!entry) return;
    free(entry->name);
    free(entry->source_path);
    free(entry->content);
    memset(entry, 0, sizeof(*entry));
}

cc_result_t cc_skill_catalog_create(cc_skill_catalog_t **out_catalog)
{
    if (!out_catalog) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null skill catalog output");
    }
    cc_skill_catalog_t *catalog = calloc(1, sizeof(*catalog));
    if (!catalog) {
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create skill catalog");
    }
    *out_catalog = catalog;
    return cc_result_ok();
}

void cc_skill_catalog_destroy(cc_skill_catalog_t *catalog)
{
    if (!catalog) return;
    for (size_t i = 0; i < catalog->count; i++) {
        free_entry(&catalog->entries[i]);
    }
    free(catalog->entries);
    free(catalog);
}

static int ensure_capacity(cc_skill_catalog_t *catalog)
{
    if (catalog->count < catalog->capacity) return 1;
    size_t next_capacity = catalog->capacity ? catalog->capacity * 2 : 8;
    cc_skill_entry_t *next = realloc(catalog->entries, next_capacity * sizeof(*next));
    if (!next) return 0;
    memset(next + catalog->capacity, 0, (next_capacity - catalog->capacity) * sizeof(*next));
    catalog->entries = next;
    catalog->capacity = next_capacity;
    return 1;
}

static long find_skill(cc_skill_catalog_t *catalog, const char *name)
{
    for (size_t i = 0; i < catalog->count; i++) {
        if (catalog->entries[i].name && strcmp(catalog->entries[i].name, name) == 0) {
            return (long)i;
        }
    }
    return -1;
}

static char *join_path2(const char *a, const char *b)
{
    if (!a || !b) return NULL;
    size_t alen = strlen(a);
    size_t blen = strlen(b);
    int need_slash = alen > 0 && a[alen - 1] != '/';
    char *out = malloc(alen + (need_slash ? 1 : 0) + blen + 1);
    if (!out) return NULL;
    memcpy(out, a, alen);
    size_t pos = alen;
    if (need_slash) out[pos++] = '/';
    memcpy(out + pos, b, blen);
    out[pos + blen] = '\0';
    return out;
}

static char *home_skill_dir(void)
{
    const char *home = getenv("HOME");
    if (!home || !home[0]) return NULL;
    return join_path2(home, ".cclaw/skills");
}

static cc_result_t upsert_skill(
    cc_skill_catalog_t *catalog,
    const char *name,
    const char *source_path,
    const char *content
)
{
    if (!catalog || !name || !content) return cc_result_ok();
    long index = find_skill(catalog, name);
    cc_skill_entry_t *entry = NULL;
    if (index >= 0) {
        entry = &catalog->entries[index];
        free(entry->source_path);
        free(entry->content);
        entry->source_path = NULL;
        entry->content = NULL;
    } else {
        if (!ensure_capacity(catalog)) {
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to grow skill catalog");
        }
        entry = &catalog->entries[catalog->count++];
        entry->name = strdup(name);
        if (!entry->name) {
            catalog->count--;
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy skill name");
        }
    }

    entry->source_path = source_path ? strdup(source_path) : strdup("");
    entry->content = strdup(content);
    if (!entry->source_path || !entry->content) {
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy skill content");
    }
    return cc_result_ok();
}

static cc_result_t load_skill_dir(
    cc_skill_catalog_t *catalog,
    cc_filesystem_t *fs,
    const char *dir
)
{
    if (!catalog || !fs || !fs->vtable || !dir || !dir[0]) return cc_result_ok();
    if (!fs->vtable->list_dir || !fs->vtable->read_text) return cc_result_ok();

    int exists = 0;
    cc_result_t rc = fs->vtable->exists ?
        fs->vtable->exists(fs->self, dir, &exists) : cc_result_ok();
    if (rc.code != CC_OK || !exists) {
        cc_result_free(&rc);
        return cc_result_ok();
    }
    cc_result_free(&rc);

    char **items = NULL;
    size_t count = 0;
    rc = fs->vtable->list_dir(fs->self, dir, &items, &count);
    if (rc.code != CC_OK) {
        cc_result_free(&rc);
        return cc_result_ok();
    }

    for (size_t i = 0; i < count; i++) {
        char *skill_dir = join_path2(dir, items[i]);
        char *skill_md = skill_dir ? join_path2(skill_dir, "SKILL.md") : NULL;
        char *content = NULL;
        if (skill_md) {
            cc_result_t read_rc = fs->vtable->read_text(fs->self, skill_md, &content);
            if (read_rc.code == CC_OK && content && content[0]) {
                cc_result_t add_rc = upsert_skill(catalog, items[i], skill_md, content);
                if (add_rc.code != CC_OK) {
                    for (size_t j = 0; j < count; j++) free(items[j]);
                    free(items);
                    free(skill_dir);
                    free(skill_md);
                    free(content);
                    return add_rc;
                }
            }
            cc_result_free(&read_rc);
        }
        free(content);
        free(skill_dir);
        free(skill_md);
        free(items[i]);
    }
    free(items);
    return cc_result_ok();
}

static cc_result_t load_plugin_skill_dirs(
    cc_skill_catalog_t *catalog,
    cc_filesystem_t *fs,
    const cc_config_t *config
)
{
    if (!config) return cc_result_ok();
    for (size_t i = 0; i < config->plugins.entry_count; i++) {
        const cc_config_plugin_entry_t *entry = &config->plugins.entries[i];
        for (size_t j = 0; j < entry->skill_dirs.count; j++) {
            cc_result_t rc = load_skill_dir(catalog, fs, entry->skill_dirs.items[j]);
            if (rc.code != CC_OK) return rc;
        }
    }
    return cc_result_ok();
}

cc_result_t cc_skill_catalog_load_from_config(
    cc_skill_catalog_t *catalog,
    cc_filesystem_t *filesystem,
    const cc_config_t *config
)
{
    if (!catalog || !filesystem || !config) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid skill catalog load request");
    }

    /*
     * 加载顺序从低优先级到高优先级。upsert 语义保证后加载的同名 skill
     * 覆盖先加载内容，所以项目/workspace 能覆盖用户级或 plugin 级定义。
     */
    char *home_dir = home_skill_dir();
    cc_result_t rc = load_skill_dir(catalog, filesystem, home_dir);
    free(home_dir);
    if (rc.code != CC_OK) return rc;

    if (config->agents.defaults.agent_dir) {
        char *personal = join_path2(config->agents.defaults.agent_dir, "skills");
        rc = load_skill_dir(catalog, filesystem, personal);
        free(personal);
        if (rc.code != CC_OK) return rc;
    }

    rc = load_plugin_skill_dirs(catalog, filesystem, config);
    if (rc.code != CC_OK) return rc;

    for (size_t i = 0; i < config->skills.extra_dirs.count; i++) {
        rc = load_skill_dir(catalog, filesystem, config->skills.extra_dirs.items[i]);
        if (rc.code != CC_OK) return rc;
    }

    if (config->workspace_path) {
        char *agents = join_path2(config->workspace_path, ".agents/skills");
        rc = load_skill_dir(catalog, filesystem, agents);
        free(agents);
        if (rc.code != CC_OK) return rc;
    }
    return cc_result_ok();
}

static int allow_skill(const cc_config_string_list_t *allowlist, const char *name)
{
    if (!allowlist || allowlist->count == 0) return 1;
    for (size_t i = 0; i < allowlist->count; i++) {
        if (allowlist->items[i] && name && strcmp(allowlist->items[i], name) == 0) return 1;
    }
    return 0;
}

cc_result_t cc_skill_catalog_build_prompt(
    cc_skill_catalog_t *catalog,
    const cc_config_string_list_t *allowlist,
    char **out_prompt
)
{
    if (!out_prompt) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null skill prompt output");
    }
    *out_prompt = NULL;
    if (!catalog || catalog->count == 0) return cc_result_ok();

    cc_string_builder_t sb;
    cc_string_builder_init(&sb);
    cc_string_builder_append(&sb, "\n\n[Skills]\n");
    cc_string_builder_append(&sb,
        "The following SKILL.md files are available for this agent. "
        "Use them as task-specific operating instructions when relevant.\n");

    for (size_t i = 0; i < catalog->count; i++) {
        cc_skill_entry_t *entry = &catalog->entries[i];
        if (!allow_skill(allowlist, entry->name)) continue;
        cc_string_builder_appendf(&sb, "\n## %s\n", entry->name ? entry->name : "skill");
        cc_string_builder_append(&sb, entry->content ? entry->content : "");
        if (entry->content && entry->content[strlen(entry->content) - 1] != '\n') {
            cc_string_builder_append(&sb, "\n");
        }
    }

    const char *text = cc_string_builder_cstr(&sb);
    if (!text || strlen(text) <= strlen("\n\n[Skills]\n")) {
        cc_string_builder_deinit(&sb);
        return cc_result_ok();
    }
    *out_prompt = cc_string_builder_take(&sb);
    return cc_result_ok();
}

cc_result_t cc_skill_catalog_list_names(
    cc_skill_catalog_t *catalog,
    char ***out_names,
    size_t *out_count
)
{
    if (!out_names || !out_count) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid skill list output");
    }
    *out_names = NULL;
    *out_count = 0;
    if (!catalog || catalog->count == 0) return cc_result_ok();
    char **names = calloc(catalog->count, sizeof(char *));
    if (!names) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate skill name list");
    for (size_t i = 0; i < catalog->count; i++) {
        names[i] = strdup(catalog->entries[i].name ? catalog->entries[i].name : "");
        if (!names[i]) {
            for (size_t j = 0; j < i; j++) free(names[j]);
            free(names);
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy skill name");
        }
    }
    *out_names = names;
    *out_count = catalog->count;
    return cc_result_ok();
}
