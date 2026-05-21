/**
 * 学习导读：cclaw/core/include/cc/util/cc_json.h
 *
 * 所属层次：核心层。
 * 阅读重点：这里定义 Agent 运行时的数据模型、主循环和通用工具，阅读时重点看所有权、错误返回和 ReAct 数据流。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * cc_json.h — JSON 解析与序列化工具模块
 *
 * @file    cc/util/cc_json.h
 * @brief   提供 JSON 文档的解析、查询、创建和序列化功能。
 *
 * 本模块是对 cJSON 库的轻量级封装，使用不透明指针（cc_json_value_t）
 * 隐藏底层实现细节。调用方不需要直接引入 cJSON.h，降低了耦合度。
 *
 * ─── 接口契约 ─────────────────────────────────────────────────────────
 *
 *   - 解析函数返回 cc_result_t，失败时包含错误信息
 *   - 所有创建的 cc_json_value_t 必须通过 cc_json_destroy() 释放
 *   - 查询函数在键不存在或类型不匹配时返回 NULL/0（防御式设计）
 *   - 线程安全：无共享状态，每次调用独立操作
 *
 * ─── 依赖 ─────────────────────────────────────────────────────────────
 *
 *   依赖 cc/core/cc_result.h（错误传递）。
 *   底层使用 cJSON 库（通过 include/cJSON.h）。
 */

#ifndef CC_JSON_H
#define CC_JSON_H

#include "cc/core/cc_result.h"

/**
 * cc_json_value_t — JSON 值的不透明类型
 *
 * 封装底层 cJSON 对象。调用方不应访问其内部成员，
 * 而是通过本模块提供的函数进行操作。这样可以避免
 * 直接绑定 cJSON 的 ABI，便于未来替换底层实现。
 */
typedef void cc_json_value_t;

/**
 * cc_json_parse — 解析 JSON 文本字符串
 *
 * 将 C 字符串解析为 cc_json_value_t 树结构。
 * 解析失败时返回 CC_ERR_JSON 错误码，out_value 设为 NULL。
 *
 * @param text       要解析的 JSON 文本（不可为 NULL）
 * @param out_value  输出：解析后的 JSON 根节点（调用方负责 cc_json_destroy）
 * @return           CC_OK 表示解析成功
 */
cc_result_t cc_json_parse(const char *text, cc_json_value_t **out_value);

/**
 * cc_json_parse_from_file — 从文件读取并解析 JSON
 *
 * 等价于 read_file + cc_json_parse，但由内部实现统一处理。
 * 文件不存在时返回 CC_ERR_NOT_FOUND，格式错误时返回 CC_ERR_JSON。
 *
 * @param path       文件路径（不可为 NULL）
 * @param out_value  输出：解析后的 JSON 根节点（调用方负责 cc_json_destroy）
 * @return           CC_OK 表示解析成功
 */
cc_result_t cc_json_parse_from_file(const char *path, cc_json_value_t **out_value);

/**
 * cc_json_stringify — 将 JSON 值序列化为格式化的字符串
 *
 * 生成的 JSON 字符串带缩进和换行，适合人类阅读和配置文件输出。
 * 返回的字符串由 malloc 分配，调用者负责 free()。
 *
 * @param value  JSON 值（不可为 NULL）
 * @return       格式化的 JSON 字符串（需要调用者 free），失败返回 NULL
 */
char *cc_json_stringify(const cc_json_value_t *value);

/**
 * cc_json_stringify_unformatted — 将 JSON 值序列化为紧凑的字符串
 *
 * 生成的 JSON 字符串不含缩进和换行，适合网络传输和日志单行输出。
 * 返回的字符串由 malloc 分配，调用者负责 free()。
 *
 * @param value  JSON 值（不可为 NULL）
 * @return       紧凑的 JSON 字符串（需要调用者 free），失败返回 NULL
 */
char *cc_json_stringify_unformatted(const cc_json_value_t *value);

/**
 * cc_json_destroy — 释放 JSON 值树
 *
 * 递归释放整个 JSON 文档树的所有节点。
 * 传入 NULL 是安全的（无操作）。
 *
 * @param value  要释放的 JSON 根节点
 */
void cc_json_destroy(cc_json_value_t *value);

/**
 * cc_json_object_get — 从 JSON 对象中获取指定键的值
 *
 * 如果 obj 不是对象类型，或键不存在，返回 NULL。
 * 返回的指针指向树内部的节点，生命周期与 root 相同，不需要单独释放。
 *
 * @param obj  JSON 对象（不可为 NULL）
 * @param key  要获取的键名（不可为 NULL）
 * @return     对应键的值节点，不存在则返回 NULL
 */
cc_json_value_t *cc_json_object_get(const cc_json_value_t *obj, const char *key);

/**
 * cc_json_object_size — 获取对象字段数量。
 *
 * @param obj JSON 对象
 * @return    字段数量，不是对象时为 0
 */
int cc_json_object_size(const cc_json_value_t *obj);

/**
 * cc_json_object_key_at — 获取对象第 index 个字段名。
 *
 * 返回值是借用指针，生命周期跟随 obj。
 * 这个接口主要服务 config.json 中 entries 这种“以 id 为 key 的对象”。
 *
 * @param obj   JSON 对象
 * @param index 从 0 开始的字段索引
 * @return      字段名，不存在时返回 NULL
 */
const char *cc_json_object_key_at(const cc_json_value_t *obj, int index);

/**
 * cc_json_object_value_at — 获取对象第 index 个字段值。
 *
 * @param obj   JSON 对象
 * @param index 从 0 开始的字段索引
 * @return      字段值借用指针，不存在时返回 NULL
 */
cc_json_value_t *cc_json_object_value_at(const cc_json_value_t *obj, int index);

/**
 * cc_json_string_value — 从 JSON 值中提取字符串
 *
 * 如果 value 不是字符串类型，返回 NULL。
 * 返回的指针指向内部数据，不需要释放。
 *
 * @param value  JSON 值（不可为 NULL）
 * @return       字符串内容，不是字符串类型则返回 NULL
 */
const char *cc_json_string_value(const cc_json_value_t *value);

/**
 * cc_json_int_value — 从 JSON 值中提取整数
 *
 * 如果 value 不是数值类型，返回 0。
 * 注意：JSON 本身不区分整数和浮点数，此函数返回截断后的整数部分。
 *
 * @param value  JSON 值（不可为 NULL）
 * @return       整数值，不是数值类型则返回 0
 */
int cc_json_int_value(const cc_json_value_t *value);

/**
 * cc_json_number_value — 从 JSON 值中提取浮点数
 *
 * 如果 value 不是数值类型，返回 0.0。
 *
 * @param value  JSON 值（不可为 NULL）
 * @return       double 数值，不是数值类型则返回 0.0
 */
double cc_json_number_value(const cc_json_value_t *value);

/**
 * cc_json_bool_value — 从 JSON 值中提取布尔值
 *
 * 如果 value 不是布尔类型，返回 0（即 false）。
 *
 * @param value  JSON 值（不可为 NULL）
 * @return       1 = true, 0 = false 或不是布尔类型
 */
int cc_json_bool_value(const cc_json_value_t *value);

/* ── 类型查询函数 ──────────────────────────────────────────────────── */

/**
 * cc_json_is_object — 判断 JSON 值是否为对象类型
 *
 * @param value  JSON 值
 * @return       1 = 是对象, 0 = 不是或 value 为 NULL
 */
int cc_json_is_object(const cc_json_value_t *value);

/**
 * cc_json_is_array — 判断 JSON 值是否为数组类型
 *
 * @param value  JSON 值
 * @return       1 = 是数组, 0 = 不是或 value 为 NULL
 */
int cc_json_is_array(const cc_json_value_t *value);

/**
 * cc_json_is_string — 判断 JSON 值是否为字符串类型
 *
 * @param value  JSON 值
 * @return       1 = 是字符串, 0 = 不是或 value 为 NULL
 */
int cc_json_is_string(const cc_json_value_t *value);

/* ── JSON 构造函数 ─────────────────────────────────────────────────── */

/**
 * cc_json_create_object — 创建空的 JSON 对象
 *
 * @return  新建的 JSON 对象（需要 cc_json_destroy），失败返回 NULL
 */
cc_json_value_t *cc_json_create_object(void);

/**
 * cc_json_create_array — 创建空的 JSON 数组
 *
 * @return  新建的 JSON 数组（需要 cc_json_destroy），失败返回 NULL
 */
cc_json_value_t *cc_json_create_array(void);

/**
 * cc_json_create_string — 创建 JSON 字符串节点
 *
 * value 会被内部拷贝。
 *
 * @param value  字符串内容（不可为 NULL）
 * @return       新建的 JSON 字符串节点（需要 cc_json_destroy），失败返回 NULL
 */
cc_json_value_t *cc_json_create_string(const char *value);

/**
 * cc_json_create_number — 创建 JSON 数值节点
 *
 * @param value  数值（double 精度）
 * @return       新建的 JSON 数值节点（需要 cc_json_destroy），失败返回 NULL
 */
cc_json_value_t *cc_json_create_number(double value);

/**
 * cc_json_create_bool — 创建 JSON 布尔节点
 *
 * @param value  0 = false, 非零 = true
 * @return       新建的 JSON 布尔节点（需要 cc_json_destroy），失败返回 NULL
 */
cc_json_value_t *cc_json_create_bool(int value);

/**
 * cc_json_create_null — 创建 JSON null 节点
 *
 * @return  新建的 JSON null 节点（需要 cc_json_destroy），失败返回 NULL
 */
cc_json_value_t *cc_json_create_null(void);

/* ── JSON 修改函数 ─────────────────────────────────────────────────── */

/**
 * cc_json_object_set — 向 JSON 对象中添加或替换键值对
 *
 * 如果 key 已存在，旧值被释放；新 value 的所有权转移给 obj，
 * 调用方不应再访问或释放 value。
 *
 * @param obj   目标 JSON 对象（不可为 NULL）
 * @param key   键名（不可为 NULL）
 * @param value 要设置的值（不可为 NULL，所有权转移）
 */
void cc_json_object_set(cc_json_value_t *obj, const char *key, cc_json_value_t *value);

/**
 * cc_json_array_append — 向 JSON 数组末尾追加元素
 *
 * value 的所有权转移给 arr，调用方不应再访问或释放 value。
 *
 * @param arr   目标 JSON 数组（不可为 NULL）
 * @param value 要追加的元素（不可为 NULL，所有权转移）
 */
void cc_json_array_append(cc_json_value_t *arr, cc_json_value_t *value);

/**
 * cc_json_array_size — 获取 JSON 数组的元素个数
 *
 * @param arr  JSON 数组（不可为 NULL）
 * @return     元素个数，不是数组类型则返回 0
 */
int cc_json_array_size(const cc_json_value_t *arr);

/**
 * cc_json_array_get — 获取 JSON 数组中指定索引的元素
 *
 * 返回的指针指向内部节点，生命周期与 arr 相同，不需要单独释放。
 *
 * @param arr    JSON 数组（不可为 NULL）
 * @param index  索引位置（从 0 开始）
 * @return       对应位置的 JSON 值，越界或类型不对则返回 NULL
 */
cc_json_value_t *cc_json_array_get(const cc_json_value_t *arr, int index);

#endif
