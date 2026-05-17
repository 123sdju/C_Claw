/**
 * 学习导读：cclaw/ports/include/cc/ports/cc_path.h
 *
 * 所属层次：端口层。
 * 阅读重点：这里定义可替换接口，阅读时重点看 struct + vtable + void *self 如何表达多态和依赖注入。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * cc_path.h — 路径操作工具模块
 *
 * @file    cc/ports/cc_path.h
 * @brief   提供跨平台的路径拼接、规范化、安全检查等功能。
 *
 * 路径操作是 Agent 安全的关键防线。所有文件工具在执行前
 * 都通过 cc_path_is_within() 检查目标路径是否在允许的工作区范围内，
 * 防止目录遍历攻击（如 ../../../etc/passwd）。
 *
 * ─── 接口契约 ─────────────────────────────────────────────────────────
 *
 *   - 所有返回字符串的函数，调用者负责 free()
 *   - cc_path_is_within() 是防止路径逃逸的关键安全函数
 *   - cc_path_canonical() 解析符号链接和相对路径为绝对路径
 *   - 路径分隔符始终为 '/'，兼容 Windows（内部转换）
 *
 * ─── 安全原则 ─────────────────────────────────────────────────────────
 *
 *   决不允许 Agent 访问工作区之外的任何路径。cc_path_is_within()
 *   是执行这一安全策略的核心函数。在每次文件操作前调用它：
 *
 *     if (!cc_path_is_within(workspace_dir, request_path)) {
 *         return CC_ERR_PERMISSION_DENIED;
 *     }
 *
 * ─── 依赖 ─────────────────────────────────────────────────────────────
 *
 *   仅依赖 cc/core/cc_result.h。
 */

#ifndef CC_PATH_H
#define CC_PATH_H

#include "cc/core/cc_result.h"

/**
 * cc_path_join — 拼接路径
 *
 * 将 base 和 child 用 '/' 连接，处理边界情况（多余斜杠）。
 * 例如: cc_path_join("/workspace", "src/main.c") → "/workspace/src/main.c"
 *
 * @param base   基础路径（不可为 NULL）
 * @param child  子路径（不可为 NULL）
 * @return       拼接后的路径字符串（调用者负责 free），失败返回 NULL
 */
char *cc_path_join(const char *base, const char *child);

/**
 * cc_path_canonical — 获取路径的规范化绝对形式
 *
 * 使用 realpath() 解析所有符号链接、解析 "." 和 ".."、获取绝对路径。
 * 这是防御目录遍历攻击（如 ../../../etc/passwd）的关键步骤。
 *
 * @param path  原始路径（可为相对路径或含符号链接）
 * @return      规范化的绝对路径字符串（调用者负责 free），失败返回 NULL
 */
char *cc_path_canonical(const char *path);

/**
 * cc_path_is_within — 检查路径是否在指定的基础目录内
 *
 * 这是 c-claw 最重要的安全函数之一。它确保 Agent 不能通过
 * 相对路径或符号链接逃逸工作区。原理：
 *   1. 将 base_dir 和 path 都转为规范化的绝对路径
 *   2. 从头比较两路径的前缀
 *   3. 如果 path 不以 base_dir 开头 → 拒绝访问
 *
 * @param base_dir  基础目录（工作区路径，不可为 NULL）
 * @param path      要检查的目标路径（不可为 NULL）
 * @return          1 = 路径在基础目录内（安全），0 = 路径逃逸（危险）
 */
int cc_path_is_within(const char *base_dir, const char *path);

/**
 * cc_path_dirname — 获取路径的父目录
 *
 * 返回路径中去掉最后一个组成部分后的部分。
 * 例如: cc_path_dirname("/workspace/src/main.c") → "/workspace/src"
 * 返回的字符串由 malloc 分配，调用者负责 free()。
 *
 * @param path  路径字符串（不可为 NULL）
 * @return      父目录路径（调用者负责 free），失败返回 NULL
 */
char *cc_path_dirname(const char *path);

/**
 * cc_path_exists — 检查路径是否存在
 *
 * 使用 access(F_OK) 检查路径（文件或目录）是否存在。
 * 比 cc_filesystem.exists() 更轻量，适合快速检查。
 *
 * @param path  要检查的路径（不可为 NULL）
 * @return      1 = 存在, 0 = 不存在
 */
int cc_path_exists(const char *path);

#endif
