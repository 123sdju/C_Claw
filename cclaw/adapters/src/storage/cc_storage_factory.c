/**
 * 学习导读：cclaw/adapters/src/storage/cc_storage_factory.c
 *
 * 所属层次：适配器层。
 * 阅读重点：这里把端口接口落到具体后端，阅读时重点看协议转换、资源释放和失败降级。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * cc_storage_factory.c — 存储工厂（根据配置选择存储后端）
 *
 * 模块在整体架构中的角色：
 *   本模块实现了会话存储后端的工厂模式（Factory Pattern）。它在程序启动时
 *   读取配置中的 storage_type 字段，创建对应的 cc_session_store_t 实例。
 *   这是存储层的入口点——上层代码不需要知道具体使用了哪个存储后端，
 *   只需通过 cc_session_store_t 虚表接口操作数据。
 *
 *   存储工厂扮演了"配置驱动的依赖注入"角色：
 *   - 开发/测试阶段使用 memory 后端（快速、无持久化需求，CI 友好）
 *   - 单用户桌面应用使用 JSON 文件后端（持久化、零外部依赖）
 *   - 多用户服务端使用 SQLite 后端（支持索引查询、事务、并发控制）
 *   切换后端只需修改配置文件中的一行字符串，无需重新编译任何代码。
 *   这就是策略模式（Strategy Pattern）的核心价值——算法与客户解耦。
 *
 * 依赖的其他模块：
 *   - cc_result.h             — 统一错误返回类型
 *   - cc_config.h             — 配置结构体，提供 storage_type 和 storage_path
 *   - cc_storage_factory.h    — 工厂接口定义和 cc_config_t 声明
 *   - 三个后端具体实现（编译时链接，通过构造函数声明引入）:
 *     - cc_memory_session_store_create   — 纯内存存储（哈希表）
 *     - cc_json_file_store_create        — JSON 文件存储（单文件序列化）
 *     - cc_sqlite_session_store_create   — SQLite 数据库存储（关系型）
 *   - 标准库 (string.h, stdio.h)
 *
 * 被哪些模块使用：
 *   - App feature/runtime_builder — 在初始化流程中调用，获取存储实例。
 *     具体应用负责读取配置、调用工厂，并将返回的 store 注入 runtime。
 *   - 所有下游模块通过 cc_session_store_t 接口间接使用（无需知道工厂存在）。
 *     这体现了依赖倒置原则（DIP）：高层模块不依赖低层模块，两者都依赖抽象。
 *
 * 支持的存储后端及其适用场景：
 *   - "memory"    — 纯内存存储（进程内哈希表）。
 *                   进程退出后数据全部丢失。
 *                   适用场景：单元测试、CI 流水线、临时 Demo、性能基准测试。
 *                   为什么需要它：单元测试不应依赖文件系统（速度慢、需要清理），
 *                   内存后端的启动延迟为零且无状态残留。
 *   - "local_file" — 本地 JSON 文件存储，所有会话持久化到单个 JSON 文件。
 *                   文件格式为 {"sessions": [...]}，简单可读、易于手动编辑和调试。
 *                   适用场景：单用户桌面应用、简单部署、数据量较小的场景。
 *                   优点：零外部依赖、数据可读、备份简单。
 *                   缺点：无索引（全量加载）、不支持并发写入、大数据量时性能下降。
 *   - "json"       — 与 local_file 相同的 JSON 文件存储（别名）。
 *                   同时支持 "local_file" 和 "json" 两个名称以兼容不同用户习惯。
 *   - "sqlite"     — SQLite 数据库存储，支持 SQL 索引查询和 ACID 事务。
 *                   适用场景：多用户服务端、需要复杂查询的场景（如"按日期范围
 *                   搜索会话"）、对数据一致性有严格要求的场景。
 *                   优点：支持并发读取、事务保证、索引加速查询。
 *                   缺点：需要 sqlite3 库（编译时链接依赖）。
 *   - 未指定/未知 — 默认使用 JSON 文件存储（"local_file" 同逻辑）。
 *                   为什么选择 JSON 而非 memory：生产环境中数据丢失是
 *                   不可接受的。JSON 文件存储是最低配置的持久化方案——
 *                   不需要额外库、不需要配置、立即可用。
 *                   路径使用默认值 profile storage path。
 *
 * 降级策略（优雅降级——容错设计的核心）：
 *   当配置选择 SQLite 但初始化失败时（可能原因：sqlite3 库未链接、磁盘满、
 *   权限问题、sqlite3_open 返回错误），自动降级到 JSON 文件存储，
 *   并通过 stderr 输出警告信息告知运维人员。
 *   为什么自动降级而非报错退出：
 *   1. 对于已部署的应用，降级到 JSON 文件存储至少保证了基本可用性
 *      ——用户可以继续对话，不会因为存储后端不可用而完全无法使用。
 *   2. 退出进程是最糟糕的用户体验——用户看到的可能是"程序闪退"而非
 *      有用的错误信息。降级 + 警告的组合让用户既能继续使用，又能
 *      通过日志了解问题并修复。
 *   3. 运维人员可以通过 stderr 中的 "[storage] SQLite failed" 关键字
 *      搜索到降级事件，及时修复 SQLite 依赖问题。
 *   注意：如果 JSON 文件初始化也失败（如磁盘满），则无处可降级——调用者
 *   收到错误返回值后需要自行决定如何处理（通常是不可能的，应该终止）。
 *
 * 默认存储路径：profile storage path
 *   选择当前目录下的 data 子目录，而非项目根目录或系统临时目录。
 *   为什么在 data/ 子目录：
 *   - 避免污染程序根目录（根目录应只有代码和配置）
 *   方便 gitignore（一个 `data/` 规则即可忽略所有运行时数据）
 *   - 方便 Docker 卷挂载（只挂载 data/ 目录实现数据持久化）
 */

#include "cc/ports/cc_storage_factory.h"
#include "cc/ports/cc_platform.h"
#include <string.h>
#include <stdio.h>

#ifndef CC_DEFAULT_STORAGE_PATH
#define CC_DEFAULT_STORAGE_PATH "runtime/data/sessions.json"
#endif

/*
 * 三个存储后端的构造函数声明（外部链接，编译时由链接器链接到具体实现）。
 * 每个函数接收不同的参数（路径或无参数），但都输出统一的
 * cc_session_store_t 虚表结构体。这是实现"策略模式"的关键——
 * 工厂只需要知道这些函数签名，不需要知道实现细节。
 *
 * cc_sqlite_session_store_create(const char *db_path, cc_session_store_t *out_store)
 *   - 创建 SQLite 存储后端
 *   - db_path: SQLite 数据库文件路径，如 profile SQLite path
 *   - 如果 SQLite 库未链接，此函数仍存在但返回错误（链接时或运行时失败）
 *
 * cc_json_file_store_create(const char *file_path, cc_session_store_t *out_store)
 *   - 创建 JSON 文件存储后端
 *   - file_path: JSON 数据文件路径，如 profile storage path
 *   - 首次使用时创建文件，后续追加
 *
 * cc_memory_session_store_create(cc_session_store_t *out_store)
 *   - 创建纯内存存储后端
 *   - 无需路径参数——数据只存在于进程内存中
 *   - 每次调用 create 都返回一个全新的空存储
 */
#if CC_STORAGE_SQLITE
cc_result_t cc_sqlite_session_store_create(const char *db_path, cc_session_store_t *out_store);
#endif
cc_result_t cc_json_file_store_create(const char *file_path, cc_session_store_t *out_store);
cc_result_t cc_memory_session_store_create(cc_session_store_t *out_store);

/*
 * cc_storage_factory_create_store — 根据配置创建合适的会话存储实例
 *
 * 功能：
 *   工厂方法（Factory Method）的核心入口。读取 config->storage_type
 *   决定使用哪个存储后端，调用对应的构造函数创建 cc_session_store_t
 *   虚表实例。支持自动降级——如果 SQLite 初始化失败，自动回退到
 *   JSON 文件存储。
 *
 * 参数：
 *   @param config    — 系统配置结构体，从中读取 storage_type 和 storage_path。
 *                      不可为 NULL。
 *      config->storage_type — 字符串，决定使用哪个存储后端：
 *         "memory"     → 纯内存存储（测试/临时场景）
 *         "local_file" → JSON 文件存储（轻量持久化）
 *         "json"       → JSON 文件存储（与 local_file 等价）
 *         "sqlite"     → SQLite 数据库（带降级到 JSON 文件）
 *         NULL 或其他  → 默认 JSON 文件存储（最安全的默认值）
 *         storage_type 的比较使用 strcmp 精确匹配（区分大小写）。
 *      config->storage_path — 存储文件路径：
 *         对 JSON 后端：JSON 文件的路径
 *         对 SQLite 后端：数据库文件路径
 *         为 NULL 时使用默认路径 profile storage path
 *   @param out_store — [out] 输出参数，指向调用者分配的 cc_session_store_t
 *                      （通常在栈上）。函数通过 memset 清零后填充虚表指针
 *                      （vtable 和 self）。
 *                      为什么由调用者分配：允许在栈上分配 cc_session_store_t，
 *                      避免工厂内部的堆分配。调用者是存储实例的唯一所有者——
 *                      工厂创建完毕后立即返回，不持有 store 引用。
 *                      cc_session_store_t 是两个指针大小（~16 字节），栈分配无开销。
 *
 * @return CC_OK 表示存储实例创建成功（可能是原始选择或降级后的后端）。
 * @return CC_ERR_INVALID_ARGUMENT 表示 config 或 out_store 为 NULL。
 * @return 其他错误码由后端构造函数返回（如文件系统错误）。
 *
 * 存储后端选择逻辑（完整决策树）：
 *
 *   ┌─ config->storage_type == "memory" ──────────────────────────────┐
 *   │  → 创建纯内存存储，不依赖文件系统                                │
 *   │  用途：测试/CI 环境，不需要持久化，最快启动                        │
 *   │  为什么直接 return：memory 永远不会降级——它实际上几乎不会失败      │
 *   └────────────────────────────────────────────────────────────────┘
 *
 *   ┌─ config->storage_type == "local_file" ──────────────────────────┐
 *   │  → 创建 JSON 文件存储，路径为 config->storage_path 或默认路径     │
 *   │  用途：单用户桌面应用，需求简单持久化                             │
 *   └────────────────────────────────────────────────────────────────┘
 *
 *   ┌─ config->storage_type == "json" ────────────────────────────────┐
 *   │  → 与 "local_file" 完全相同的逻辑                                │
 *   │  用途：兼容不同用户的配置命名习惯                                  │
 *   └────────────────────────────────────────────────────────────────┘
 *
 *   ┌─ config->storage_type == "sqlite" ──────────────────────────────┐
 *   │  → 尝试创建 SQLite 存储（传递 config->storage_path 作为 db 路径）  │
 *   │  ├─ 成功（rc.code == CC_OK）：直接返回 SQLite store             │
 *   │  │  为什么在这层判断而非递归调用：成功时不需要降级，直接返回最简    │
 *   │  └─ 失败：自动降级到 JSON 文件存储 + stderr 警告                  │
 *   │     降级路径与 "local_file" 相同                                  │
 *   │     为什么 falback 不尝试 memory：生产环境数据持久化是必需的，      │
 *   │     memory 降级会导致数据丢失，不符合期望                           │
 *   └────────────────────────────────────────────────────────────────┘
 *
 *   ┌─ config->storage_type 为 NULL 或其他值 ─────────────────────────┐
 *   │  → 默认 JSON 文件存储（最通用、最安全的默认值）                     │
 *   │  路径：config->storage_path 或默认 profile storage path        │
 *   │  为什么 JSON 是默认：不依赖外部库，任何平台都可用，数据持久化       │
 *   └────────────────────────────────────────────────────────────────┘
 *
 * 平台注意事项：
 *   - SQLite 后端需要编译时链接 sqlite3 库，否则构造函数返回错误。
 *     如果项目需要零外部依赖，只需在编译时不链接 sqlite3，配置中
 *     不选择 sqlite 即可。工厂不会因为缺少 sqlite3 而编译失败。
 *   - JSON 文件后端依赖文件系统权限和 cc_filesystem 接口。
 *     需要确保 data/ 目录存在且有写权限。如果目录不存在，JSON 后端
 *     的 create 函数通常会尝试创建目录（取决于 cc_filesystem 实现）。
 *   - 内存后端不持久化，进程退出后所有数据全部丢失。
 *     选择它意味着接受数据丢失的风险。不要在配置为 memory 时
 *     期望"重启后还能恢复对话"。
 *
 * 为什么使用 memset 清零 out_store：
 *   cc_session_store_t 可能在栈上分配，包含不确定的随机值。
 *   memset 清零确保：
 *   1. 未使用的虚表字段为 NULL（如果 store 只有部分操作被支持）
 *   2. 后端构造函数可以安全地通过指针赋值设置 vtable 和 self
 *   3. 如果后端构造函数提前失败返回错误，out_store 的状态是
 *      明确的（全零），调用者不会误用随机值
 */
cc_result_t cc_storage_factory_create_store(
    const cc_config_t *config,
    cc_session_store_t *out_store
)
{
    if (!config || !out_store)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null config or out_store");

    /* 清零 out_store——确保所有字段在初始状态，栈上的随机值被清除 */
    memset(out_store, 0, sizeof(cc_session_store_t));

    /* 分支 1: 纯内存存储——用于测试和临时场景 */
    if (config->storage_type && strcmp(config->storage_type, "memory") == 0) {
        return cc_memory_session_store_create(out_store);
    }

    /* 分支 2: JSON 文件存储——本地文件持久化 */
    if (config->storage_type && strcmp(config->storage_type, "local_file") == 0) {
        const char *path = config->storage_path ? config->storage_path : CC_DEFAULT_STORAGE_PATH;
        return cc_json_file_store_create(path, out_store);
    }

    /* 分支 3: JSON 文件存储（别名）——与 local_file 完全相同的逻辑 */
    if (config->storage_type && strcmp(config->storage_type, "json") == 0) {
        const char *path = config->storage_path ? config->storage_path : CC_DEFAULT_STORAGE_PATH;
        return cc_json_file_store_create(path, out_store);
    }

    /* 分支 4: SQLite 数据库存储——带自动降级到 JSON 文件 */
    if (config->storage_type && strcmp(config->storage_type, "sqlite") == 0) {
#if CC_STORAGE_SQLITE
        /* 尝试创建 SQLite 存储后端 */
        cc_result_t rc = cc_sqlite_session_store_create(config->storage_path, out_store);
        if (rc.code == CC_OK) return rc;  /* SQLite 成功，直接返回 */

        /* SQLite 失败——输出警告信息到 stderr，然后降级到 JSON 文件存储。
           stderr 输出是开发/运维人员发现问题的唯一途径——
           生产环境中应通过 cc_log 系统记录，但在工厂创建阶段日志系统
           可能尚未初始化，因此退回到原始 stderr。 */
        fprintf(stderr, "[storage] SQLite failed (%s), falling back to JSON\n",
            rc.message ? rc.message : "unknown");
        cc_result_free(&rc);  /* 释放 SQLite 错误消息——不再需要传播 */
        /* 继续执行到默认分支（不 return，不 break——C 语言中无 fallthrough 到 else） */
#else
        fprintf(stderr, "[storage] SQLite disabled in this build, falling back to JSON\n");
#endif
    }

    /* 分支 5（默认）: JSON 文件存储——最安全、最通用的默认降级目标。
       以下情况会走到这里：
       - config->storage_type == NULL（未指定）
       - config->storage_type 是未知值（不是上面任一已知值）
       - config->storage_type == "sqlite" 但 SQLite 初始化失败（降级） */
    return cc_json_file_store_create(
        config->storage_path ? config->storage_path : CC_DEFAULT_STORAGE_PATH,
        out_store
    );
}
