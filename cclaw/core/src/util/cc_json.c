/**
 * 学习导读：cclaw/core/src/util/cc_json.c
 *
 * 所属层次：核心层。
 * 阅读重点：这里封装 cJSON，重点看 cc_json_value_t 的所有权、对象/数组访问
 *           和解析错误如何转换成 cc_result_t。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/*
 * cc_json.c — JSON 操作封装
 *
 * 在整体架构中的角色：
 *   本模块是 Util 层的 JSON 基础设施，对 cJSON 库进行类型安全的封装。
 *   它将 cJSON 的内部类型（cJSON *）对外隐藏为不透明指针 cc_json_value_t *，
 *   使得上层模块不直接依赖 cJSON，降低了库耦合度。
 *   主要服务于配置解析（cc_config）、LLM API 请求/响应构建等场景。
 *
 * cJSON 包装策略（Wrapping Strategy）：
 * ─────────────────────────────────────
 *   本模块采用"薄包装"（Thin Wrapper）模式封装 cJSON：
 *     - cc_json_value_t 是 typedef void 的不透明指针（Opaque Pointer）
 *     - 内部通过 (cJSON *) 强制转换在 cJSON 和 cc_json_value_t 之间切换
 *     - 每个函数（创建/解析/查询/销毁）内部调用对应的 cJSON 函数
 *
 *   包装模式的优势：
 *     1. 不透明指针隐藏了 cJSON 的内部结构细节，上层模块不需要
 *        #include "cJSON.h"，减少了编译依赖和编译时间。
 *     2. 如果未来需要替换 JSON 库（如从 cJSON 切换到 jansson 或 yyjson），
 *        只需修改本模块的内部实现，所有上层模块无需改动。
 *     3. 可以添加额外的类型安全检查（如 cc_json_string_value 自动验证
 *        cJSON_IsString），而这些检查在直接使用 cJSON 时容易被遗漏。
 *     4. 统一错误处理——所有函数通过 cc_result_t 返回错误码，
 *        与项目整体错误处理体系统一。
 *
 *   包装模式的代价：
 *     - 额外的函数调用层（通常可被编译器内联优化抵消）
 *     - 并非所有 cJSON 功能都被暴露（只封装了项目实际需要的 API）
 *     - 需要维护本模块的代码（约 500 行，成本可控）
 *
 * 类型擦除（Type Erasure）的好处：
 * ──────────────────────────────
 *   typedef void cc_json_value_t 是一种 C 语言中的"类型擦除"技术。
 *
 *   BENEFIT 1 — 编译隔离（Compilation Isolation）：
 *     上层模块只需 #include "cc_json.h"（声明了 cc_json_value_t 为 void），
 *     不需要 #include "cJSON.h"。如果 cJSON 的头文件发生变化，
 *     只有 cc_json.c 需要重新编译，而不是所有使用 JSON 的模块。
 *     在大型项目中，这显著减少了增量编译时间。
 *
 *   BENEFIT 2 — API 稳定性（API Stability）：
 *     如果直接暴露 cJSON *，上层模块可能会依赖 cJSON 特定的 API
 *     （如直接访问 cJSON->valuestring 字段），导致库替换困难。
 *     不透明指针强制上层模块只能使用本模块提供的封装函数。
 *
 *   BENEFIT 3 — 类型安全包装（Type-Safe Wrapper）：
 *     本模块可以添加 cJSON 原生不具备的安全检查。例如：
 *       - cc_json_string_value() 先检查 cJSON_IsString() 再取值
 *       - cc_json_object_get() 对 NULL 参数返回 NULL 而非崩溃
 *     直接使用 cJSON 时，这些安全检查容易被遗漏。
 *
 * 核心设计：
 *   - 类型擦除：使用 typedef void cc_json_value_t 将 cJSON 内部类型隐藏
 *   - 读取接口：提供类型安全的取值函数（string/int/bool），自动进行 NULL 检查和类型校验
 *   - 构造接口：create 函数封装 cJSON_Create* 系列，统一错误处理
 *   - 文件解析：cc_json_parse_from_file 一次性完成文件读取和 JSON 解析
 *   - 统一结果：所有可能失败的操作通过 cc_result_t 返回，与项目错误处理体系一致
 *
 * 依赖：
 *   - cJSON.h：底层 JSON 解析和构造库
 *   - cc/core/cc_result.h：统一结果类型
 *   - 标准 C 库：stdlib（malloc/free）、string（strdup）、stdio（fopen/fread/fclose）
 */

#include "cc/util/cc_json.h"
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/*
 * cc_json_parse — 解析 JSON 字符串
 *
 * 功能：将 C 字符串解析为内部 JSON 值树。
 *       解析失败时通过 cc_result_t 返回详细错误信息。
 *
 * 参数：
 *   text      — 待解析的 JSON 文本（C 字符串，不可为 NULL）
 *   out_value — 输出参数，解析成功时指向根 JSON 值
 *
 * 返回值：
 *   cc_result_ok() — 解析成功，*out_value 指向 JSON 树
 *   cc_result_error(CC_ERR_INVALID_ARGUMENT) — text 为 NULL
 *   cc_result_error(CC_ERR_JSON) — JSON 语法错误，错误消息包含解析失败位置
 *
 * 设计决策：使用 cJSON_Parse 而非 cJSON_ParseWithOpts，
 *           因为本项目不需要自定义解析选项，保持接口简洁。
 */
cc_result_t cc_json_parse(const char *text, cc_json_value_t **out_value)
{
    if (!text) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null JSON text");
    cJSON *val = cJSON_Parse(text);
    if (!val) {
        const char *err = cJSON_GetErrorPtr();
        return cc_result_error(CC_ERR_JSON, err ? err : "JSON parse error");
    }
    *out_value = (cc_json_value_t *)val;
    return cc_result_ok();
}

/*
 * cc_json_parse_from_file — 从文件读取并解析 JSON
 *
 * 功能：一次性完成文件打开、内容读取和 JSON 解析三个步骤。
 *       读取后的缓冲区在解析完成后立即释放，不会泄漏。
 *       文件读取失败或 JSON 解析失败时均返回对应错误码。
 *
 * 参数：
 *   path      — JSON 文件的路径
 *   out_value — 输出参数，解析成功时指向根 JSON 值
 *
 * 返回值：
 *   cc_result_ok() — 解析成功
 *   cc_result_error(CC_ERR_IO) — 文件无法打开
 *   cc_result_error(CC_ERR_OUT_OF_MEMORY) — 读取缓冲区分配失败
 *   cc_result_error(CC_ERR_JSON) — JSON 语法错误
 *
 * 设计决策：在同一个函数内完成 fopen/fread/fclose 和 JSON 解析，
 *           简化调用者代码，并确保中间缓冲区在所有路径上都被正确释放。
 */
cc_result_t cc_json_parse_from_file(const char *path, cc_json_value_t **out_value)
{
    FILE *f = fopen(path, "rb");
    if (!f) return cc_result_error(CC_ERR_IO, "Cannot open JSON file");

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = malloc(size + 1);
    if (!buf) {
        fclose(f);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate buffer");
    }

    size_t read_size = fread(buf, 1, size, f);
    fclose(f);
    buf[read_size] = '\0';

    cc_result_t rc = cc_json_parse(buf, out_value);
    free(buf);
    return rc;
}

/*
 * cc_json_stringify — 将 JSON 值序列化为字符串
 *
 * 功能：将内部 JSON 树转换为格式化的 JSON 字符串（带缩进）。
 *       返回的字符串由 cJSON_Print 动态分配，调用者需要调用 free 释放。
 *       如果 value 为 NULL，返回字符串 "null" 而非 NULL。
 *
 * 参数：
 *   value — 待序列化的 JSON 值（可为 NULL）
 *
 * 返回值：
 *   成功 — 返回 JSON 字符串（调用者负责释放）
 *   value 为 NULL — 返回 strdup("null") 的结果
 *
 * 设计决策：使用 cJSON_Print（格式化输出）而非 cJSON_PrintUnformatted，
 *           因为本项目以配置文件读写和 API 调试为主要场景，
 *           可读性优先于传输效率。
 */
char *cc_json_stringify(const cc_json_value_t *value)
{
    if (!value) return strdup("null");
    return cJSON_Print((cJSON *)value);
}

/*
 * cc_json_stringify_unformatted — 序列化为紧凑单行 JSON
 *
 * 功能：将 JSON 值树序列化为无缩进、无换行的紧凑 JSON 字符串。
 *       内部调用 cJSON_PrintUnformatted。
 *
 * 与 cc_json_stringify 的区别：
 *   cc_json_stringify 使用 cJSON_Print，输出带缩进和换行的可读格式，
 *   适合人类阅读和调试输出。
 *   cc_json_stringify_unformatted 使用 cJSON_PrintUnformatted，
 *   输出紧凑单行格式，适合机器间通信（如 JSON-RPC 行协议、
 *   HTTP 请求体等），避免换行符破坏行协议。
 *
 * @param value 待序列化的 JSON 值（可为 NULL）
 * @return      紧凑 JSON 字符串（调用者负责 free），value 为 NULL 时返回 "null"
 */
char *cc_json_stringify_unformatted(const cc_json_value_t *value)
{
    if (!value) return strdup("null");
    return cJSON_PrintUnformatted((cJSON *)value);
}

/*
 * cc_json_destroy — 释放 JSON 值树的所有内存
 *
 * 功能：递归释放 JSON 值树占用的所有内存。
 *       调用后 value 指针失效，不可再次使用。
 *       如果 value 为 NULL，则静默跳过。
 *
 * 参数：
 *   value — 待销毁的 JSON 值（可为 NULL）
 *
 * 返回值：无
 *
 * 注意事项：此函数会递归释放整棵树，包括所有子节点。
 *           对于深层嵌套的大型 JSON，注意栈深度限制。
 */
void cc_json_destroy(cc_json_value_t *value)
{
    if (value) cJSON_Delete((cJSON *)value);
}

/*
 * cc_json_object_get — 从 JSON 对象中按键获取子值
 *
 * 功能：在 JSON 对象中查找指定 key 对应的值。
 *       如果 obj 不是对象类型，或 key 不存在，或任一参数为 NULL，均返回 NULL。
 *
 * 参数：
 *   obj — JSON 对象指针（不可为 NULL）
 *   key — 要查找的键名（不可为 NULL）
 *
 * 返回值：
 *   找到 — 返回对应值的指针（生命周期与父对象绑定）
 *   未找到或参数无效 — 返回 NULL
 *
 * 注意事项：返回的指针是借出引用（borrowed reference），
 *           其生命周期由父 JSON 对象管理，调用者不应单独释放。
 */
cc_json_value_t *cc_json_object_get(const cc_json_value_t *obj, const char *key)
{
    if (!obj || !key) return NULL;
    return (cc_json_value_t *)cJSON_GetObjectItem((cJSON *)obj, key);
}

int cc_json_object_size(const cc_json_value_t *obj)
{
    if (!obj || !cJSON_IsObject((cJSON *)obj)) return 0;
    int count = 0;
    for (cJSON *child = ((cJSON *)obj)->child; child; child = child->next) count++;
    return count;
}

const char *cc_json_object_key_at(const cc_json_value_t *obj, int index)
{
    if (!obj || !cJSON_IsObject((cJSON *)obj) || index < 0) return NULL;
    int i = 0;
    for (cJSON *child = ((cJSON *)obj)->child; child; child = child->next, i++) {
        if (i == index) return child->string;
    }
    return NULL;
}

cc_json_value_t *cc_json_object_value_at(const cc_json_value_t *obj, int index)
{
    if (!obj || !cJSON_IsObject((cJSON *)obj) || index < 0) return NULL;
    int i = 0;
    for (cJSON *child = ((cJSON *)obj)->child; child; child = child->next, i++) {
        if (i == index) return (cc_json_value_t *)child;
    }
    return NULL;
}

/*
 * cc_json_string_value — 提取 JSON 字符串值
 *
 * 功能：安全地从 JSON 值中提取字符串内容。
 *       自动进行 NULL 检查和类型校验（必须是 JSON 字符串类型）。
 *
 * 参数：
 *   value — JSON 值指针（可为 NULL）
 *
 * 返回值：
 *   成功 — 返回字符串内容的 const char * 指针
 *   value 为 NULL 或不是字符串类型 — 返回 NULL
 */
const char *cc_json_string_value(const cc_json_value_t *value)
{
    if (!value || !cJSON_IsString((cJSON *)value)) return NULL;
    return cJSON_GetStringValue((cJSON *)value);
}

/*
 * cc_json_int_value — 提取 JSON 整数值
 *
 * 功能：安全地从 JSON 值中提取整数值。
 *       自动进行 NULL 检查和类型校验（必须是 JSON 数字类型）。
 *
 * 参数：
 *   value — JSON 值指针（可为 NULL）
 *
 * 返回值：
 *   成功 — 返回整数值
 *   value 为 NULL 或不是数字类型 — 返回 0
 *
 * 注意事项：返回 0 在语义上可能产生歧义（0 可以是合法的 JSON 数字值），
 *           调用者应先通过 cc_json_is_* 系列函数进行类型预检查。
 */
int cc_json_int_value(const cc_json_value_t *value)
{
    if (!value || !cJSON_IsNumber((cJSON *)value)) return 0;
    return ((cJSON *)value)->valueint;
}

/*
 * cc_json_number_value — 提取 JSON 数值
 *
 * 与 cc_json_int_value 类似，但保留浮点精度，供 temperature 等配置项使用。
 */
double cc_json_number_value(const cc_json_value_t *value)
{
    if (!value || !cJSON_IsNumber((cJSON *)value)) return 0.0;
    return ((cJSON *)value)->valuedouble;
}

/*
 * cc_json_bool_value — 提取 JSON 布尔值
 *
 * 功能：安全地从 JSON 值中提取布尔值（返回 0 或 1）。
 *       NULL 值被视为 false。
 *
 * 参数：
 *   value — JSON 值指针（可为 NULL）
 *
 * 返回值：
 *   JSON true  — 返回 1
 *   JSON false、NULL 或其他类型 — 返回 0
 */
int cc_json_bool_value(const cc_json_value_t *value)
{
    if (!value) return 0;
    return cJSON_IsTrue((cJSON *)value) ? 1 : 0;
}

/*
 * cc_json_is_object — 判断 JSON 值是否为对象类型
 *
 * 功能：检查 value 是否为 JSON 对象类型（即 {} 包裹的键值对集合）。
 *
 * 参数：
 *   value — JSON 值指针（可为 NULL）
 *
 * 返回值：
 *   是对象类型 — 返回非零值（真）
 *   不是对象类型或 value 为 NULL — 返回 0（假）
 */
int cc_json_is_object(const cc_json_value_t *value)
{
    return value && cJSON_IsObject((cJSON *)value);
}

/*
 * cc_json_is_array — 判断 JSON 值是否为数组类型
 *
 * 功能：检查 value 是否为 JSON 数组类型（即 [] 包裹的元素列表）。
 *
 * 参数：
 *   value — JSON 值指针（可为 NULL）
 *
 * 返回值：
 *   是数组类型 — 返回非零值（真）
 *   不是数组类型或 value 为 NULL — 返回 0（假）
 */
int cc_json_is_array(const cc_json_value_t *value)
{
    return value && cJSON_IsArray((cJSON *)value);
}

/*
 * cc_json_is_string — 判断 JSON 值是否为字符串类型
 *
 * 功能：检查 value 是否为 JSON 字符串类型。
 *
 * 参数：
 *   value — JSON 值指针（可为 NULL）
 *
 * 返回值：
 *   是字符串类型 — 返回非零值（真）
 *   不是字符串类型或 value 为 NULL — 返回 0（假）
 */
int cc_json_is_string(const cc_json_value_t *value)
{
    return value && cJSON_IsString((cJSON *)value);
}

/*
 * cc_json_create_object — 创建空的 JSON 对象
 *
 * 功能：创建一个空的 JSON 对象 {}。
 *       返回值由 cJSON 分配，需要调用 cc_json_destroy 或将其作为子节点添加到父节点后自动管理。
 *
 * 参数：无
 *
 * 返回值：
 *   成功 — 返回新创建的 JSON 对象
 *   内存不足 — 返回 NULL（cJSON 内部已处理 OOM）
 */
cc_json_value_t *cc_json_create_object(void)
{
    return (cc_json_value_t *)cJSON_CreateObject();
}

/*
 * cc_json_create_array — 创建空的 JSON 数组
 *
 * 功能：创建一个空的 JSON 数组 []。
 *       返回值由 cJSON 分配，需要调用 cc_json_destroy 或使用 cc_json_array_append 将其添加到父节点。
 *
 * 参数：无
 *
 * 返回值：
 *   成功 — 返回新创建的 JSON 数组
 *   内存不足 — 返回 NULL
 */
cc_json_value_t *cc_json_create_array(void)
{
    return (cc_json_value_t *)cJSON_CreateArray();
}

/*
 * cc_json_create_string — 创建 JSON 字符串值
 *
 * 功能：创建一个 JSON 字符串值。
 *       如果 value 为 NULL，则创建空字符串 "" 而非 NULL 值。
 *
 * 参数：
 *   value — 字符串内容（可为 NULL，NULL 时使用空字符串）
 *
 * 返回值：
 *   成功 — 返回新创建的 JSON 字符串
 *   内存不足 — 返回 NULL
 */
cc_json_value_t *cc_json_create_string(const char *value)
{
    return (cc_json_value_t *)cJSON_CreateString(value ? value : "");
}

/*
 * cc_json_create_number — 创建 JSON 数值
 *
 * 功能：创建一个 JSON 数值（double 类型，兼容整数和浮点数）。
 *
 * 参数：
 *   value — 数值（double）
 *
 * 返回值：
 *   成功 — 返回新创建的 JSON 数值
 *   内存不足 — 返回 NULL
 */
cc_json_value_t *cc_json_create_number(double value)
{
    return (cc_json_value_t *)cJSON_CreateNumber(value);
}

/*
 * cc_json_create_bool — 创建 JSON 布尔值
 *
 * 功能：创建一个 JSON 布尔值。
 *
 * 参数：
 *   value — 布尔值（非零为 true，0 为 false）
 *
 * 返回值：
 *   成功 — 返回新创建的 JSON 布尔值
 *   内存不足 — 返回 NULL
 */
cc_json_value_t *cc_json_create_bool(int value)
{
    return (cc_json_value_t *)cJSON_CreateBool(value);
}

/**
 * cc_json_create_null — 创建 JSON null 值
 *
 * 功能：创建一个表示 null 的 JSON 值。
 *       在 LLM API 中，assistant 消息在有 tool_calls 时 content 必须为 null，
 *       此函数专门用于构造这种情况。
 *
 * 参数：无
 *
 * 返回值：
 *   成功 — 返回新创建的 JSON null 值
 *   内存不足 — 返回 NULL（极少发生，cJSON_CreateNull 只是一个常量对象）
 *
 * 典型使用场景：
 *   // 构造 {"role":"assistant","content":null,"tool_calls":[...]}
 *   cc_json_object_set(msg, "content", cc_json_create_null());
 *   对应 OpenAI API 中 assistant 消息伴随 tool_calls 时的标准格式。
 */
cc_json_value_t *cc_json_create_null(void)
{
    return (cc_json_value_t *)cJSON_CreateNull();
}

/*
 * cc_json_object_set — 向 JSON 对象中添加键值对
 *
 * 功能：向 JSON 对象中设置指定 key 的值。
 *       value 的所有权转移给 obj 管理，调用者不应再单独释放 value。
 *       如果任一参数为 NULL，则静默跳过。
 *
 * 参数：
 *   obj   — 目标 JSON 对象
 *   key   — 键名字符串
 *   value — 要设置的 JSON 值（所有权转移）
 *
 * 返回值：无
 *
 * 注意事项：value 的所有权转移给 obj 后，obj 销毁时会自动释放 value。
 *           不要在调用此函数后再对 value 调用 cc_json_destroy。
 */
void cc_json_object_set(cc_json_value_t *obj, const char *key, cc_json_value_t *value)
{
    if (!obj || !key || !value) return;
    cJSON *object = (cJSON *)obj;
    if (cJSON_GetObjectItemCaseSensitive(object, key)) {
        cJSON_ReplaceItemInObjectCaseSensitive(object, key, (cJSON *)value);
    } else {
        cJSON_AddItemToObject(object, key, (cJSON *)value);
    }
}

/*
 * cc_json_array_append — 向 JSON 数组中追加元素
 *
 * 功能：向 JSON 数组末尾追加一个值。
 *       value 的所有权转移给 arr 管理。
 *       如果任一参数为 NULL，则静默跳过。
 *
 * 参数：
 *   arr   — 目标 JSON 数组
 *   value — 要追加的 JSON 值（所有权转移）
 *
 * 返回值：无
 *
 * 注意事项：与 cc_json_object_set 类似，value 的所有权转移给 arr。
 */
void cc_json_array_append(cc_json_value_t *arr, cc_json_value_t *value)
{
    if (arr && value)
        cJSON_AddItemToArray((cJSON *)arr, (cJSON *)value);
}

/*
 * cc_json_array_size — 获取 JSON 数组的元素个数
 *
 * 功能：返回 JSON 数组中的元素数量。
 *       自动进行 NULL 检查和类型校验。
 *
 * 参数：
 *   arr — JSON 数组指针（可为 NULL）
 *
 * 返回值：
 *   成功 — 返回数组元素个数
 *   arr 为 NULL 或不是数组类型 — 返回 0
 */
int cc_json_array_size(const cc_json_value_t *arr)
{
    if (!arr || !cJSON_IsArray((cJSON *)arr)) return 0;
    return cJSON_GetArraySize((cJSON *)arr);
}

/*
 * cc_json_array_get — 按索引获取 JSON 数组中的元素
 *
 * 功能：返回 JSON 数组中指定索引位置的元素。
 *       自动进行 NULL 检查和类型校验。
 *
 * 参数：
 *   arr   — JSON 数组指针
 *   index — 元素索引（从 0 开始）
 *
 * 返回值：
 *   成功 — 返回对应元素的指针（借出引用）
 *   索引越界、arr 为 NULL 或不是数组类型 — 返回 NULL
 *
 * 注意事项：返回的指针是借出引用，生命周期由父数组管理。
 */
cc_json_value_t *cc_json_array_get(const cc_json_value_t *arr, int index)
{
    if (!arr || !cJSON_IsArray((cJSON *)arr)) return NULL;
    return (cc_json_value_t *)cJSON_GetArrayItem((cJSON *)arr, index);
}
