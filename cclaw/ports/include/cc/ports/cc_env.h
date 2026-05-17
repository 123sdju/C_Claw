/**
 * 学习导读：cclaw/ports/include/cc/ports/cc_env.h
 *
 * 所属层次：端口层。
 * 阅读重点：这里定义可替换接口，阅读时重点看 struct + vtable + void *self 如何表达多态和依赖注入。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * cc_env.h — 环境变量访问模块
 *
 * @file    cc/ports/cc_env.h
 * @brief   提供跨平台的环境变量读取、写入和类型转换接口。
 *
 * 本模块封装了操作系统环境变量的访问操作。通过统一的 API，
 * 上层代码无需关心底层是 POSIX 的 getenv/setenv、Windows 的
 * GetEnvironmentVariable/SetEnvironmentVariable，还是 ESP32 的
 * 自定义配置存储。
 *
 * ─── 接口契约 ─────────────────────────────────────────────────────────
 *
 *   - cc_env_get() 读取字符串类型环境变量
 *   - cc_env_get_int() 读取整数类型环境变量（带默认值）
 *   - cc_env_set() 设置环境变量
 *   - 返回的字符串指针可能指向静态缓冲区或内部缓存，调用者不应 free
 *
 * ─── 使用场景 ─────────────────────────────────────────────────────────
 *
 *   本模块在项目中主要用于读取运行时配置，例如：
 *     - API 密钥（如 OPENAI_API_KEY）
 *     - 模型名称（如 CC_MODEL）
 *     - 调试开关（如 CC_DEBUG）
 *     - 超时设置（如 CC_TIMEOUT_SECONDS）
 *
 * ─── 依赖 ─────────────────────────────────────────────────────────────
 *
 *   无外部依赖。纯 C 标准库函数封装，不依赖项目内部任何模块。
 */

#ifndef CC_ENV_H
#define CC_ENV_H

/**
 * cc_env_get — 读取环境变量的字符串值
 *
 * 从操作系统环境变量表中查找 key 对应的值。如果环境变量不存在，
 * 返回 NULL。返回的指针指向内部缓冲区，调用者不应修改或释放。
 *
 * @param key  环境变量名称（不可为 NULL，大小写敏感取决于平台）
 * @return     环境变量的字符串值，不存在时返回 NULL
 */
const char *cc_env_get(const char *key);

/**
 * cc_env_get_int — 读取环境变量的整数值
 *
 * 将环境变量的字符串值解析为整数。无法解析或变量不存在时，
 * 返回 default_value。适用于数值型配置参数（如端口号、超时秒数）。
 *
 * @param key            环境变量名称（不可为 NULL）
 * @param default_value  变量不存在或无法解析时的默认返回值
 * @return               解析后的整数值，或 default_value
 */
int cc_env_get_int(const char *key, int default_value);

/**
 * cc_env_set — 设置环境变量
 *
 * 将 key=value 写入当前进程的环境变量表。如果 key 已存在则覆盖。
 * 设置的变量仅对当前进程及其子进程可见。
 *
 * @param key    环境变量名称（不可为 NULL）
 * @param value  环境变量值（不可为 NULL）
 */
void cc_env_set(const char *key, const char *value);

#endif