/**
 * 学习导读：cclaw/adapters/src/storage/cc_memory_store_factory.c
 *
 * 所属层次：适配器层。
 * 阅读重点：这里把端口接口落到具体后端，阅读时重点看协议转换、资源释放和失败降级。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/*
 * cc_memory_store_factory.c — 记忆存储后端选择工厂
 *
 * 模块说明：
 *   基于配置文件中的 backend 字符串，动态选择并创建对应的记忆存储后端实例。
 *   支持四种后端：inmem（内存）、json_file（JSON 文件）、sqlite（SQLite 数据库）、
 *   noop（空操作，存储禁用）。
 *
 * 设计模式：简单工厂模式
 *   根据输入参数路由到不同的具体工厂函数（cc_memory_store_create_*），
 *   调用者只需指定后端名称和路径，无需了解每种后端的创建细节。
 *
 * 支持的后端：
 *   - inmem    ：纯内存存储（进程退出数据丢失）
 *   - json_file：JSON 文件持久化（适合小数据量，默认文件 memory.json）
 *   - sqlite   ：SQLite3 数据库持久化（适合大数据量，默认文件 memory.db）
 *   - noop     ：空操作，所有操作直接返回错误（用于禁用记忆功能的平台）
 */

#include "cc/ports/cc_memory_store.h"
#include "cc/ports/cc_platform.h"
#include <stdlib.h>
#include <string.h>

#ifndef CC_DEFAULT_MEMORY_PATH
#define CC_DEFAULT_MEMORY_PATH "runtime/data/memory.json"
#endif

/*
 * cc_memory_store_create_noop — 创建空操作记忆存储
 *
 * 功能：当平台不支持或配置禁用记忆存储时使用此实现。
 *       直接返回 CC_ERR_PLATFORM 错误，不创建实际存储实例。
 *
 * @param out_store 输出参数（未使用，仅保持接口签名一致）
 * @return 始终返回 CC_ERR_PLATFORM
 */
cc_result_t cc_memory_store_create_noop(cc_memory_store_t *out_store)
{
    (void)out_store;
    return cc_result_error(CC_ERR_PLATFORM, "Memory store disabled on this platform");
}

cc_result_t cc_memory_store_create_inmem(cc_memory_store_t *out_store);
cc_result_t cc_memory_store_create_json_file(cc_memory_store_t *out_store, const char *file_path);
#if CC_STORAGE_SQLITE
cc_result_t cc_memory_store_create_sqlite(cc_memory_store_t *out_store, const char *db_path);
#endif

/*
 * cc_memory_store_factory_create — 记忆存储工厂主入口
 *
 * 功能：根据 backend 参数路由到具体的存储后端创建函数：
 *       - "inmem"     → cc_memory_store_create_inmem()
 *       - "json_file" → cc_memory_store_create_json_file(默认路径 "memory.json")
 *       - "sqlite"    → cc_memory_store_create_sqlite(默认路径 "memory.db")
 *       - "noop"      → cc_memory_store_create_noop()
 *
 * 参数：
 *   out_store — 输出参数，填充创建好的存储实例
 *   backend   — 后端类型字符串（"inmem"/"json_file"/"sqlite"/"noop"）
 *   path      — 持久化文件/数据库路径（仅 json_file 和 sqlite 使用，可为 NULL 使用默认值）
 *
 * @return cc_result_t
 *   - CC_OK：创建成功
 *   - CC_ERR_INVALID_ARGUMENT：out_store 或 backend 为 NULL，或 backend 不识别
 */
cc_result_t cc_memory_store_factory_create(
    cc_memory_store_t *out_store,
    const char *backend,
    const char *path
)
{
    if (!out_store || !backend)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid factory arguments");

    if (strcmp(backend, "inmem") == 0)
        return cc_memory_store_create_inmem(out_store);
    if (strcmp(backend, "json_file") == 0)
        return cc_memory_store_create_json_file(out_store, path ? path : CC_DEFAULT_MEMORY_PATH);
    if (strcmp(backend, "sqlite") == 0) {
#if CC_STORAGE_SQLITE
        return cc_memory_store_create_sqlite(out_store, path ? path : CC_DEFAULT_MEMORY_PATH);
#else
        return cc_result_error(CC_ERR_PLATFORM, "SQLite memory backend disabled in this build");
#endif
    }
    if (strcmp(backend, "noop") == 0 || strcmp(backend, "none") == 0)
        return cc_memory_store_create_noop(out_store);

    return cc_result_errf(CC_ERR_INVALID_ARGUMENT, "Unknown memory backend: %s", backend);
}
