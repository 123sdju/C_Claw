/**
 * 学习导读：cclaw/adapters/src/tools/common/cc_file_write_tool.c
 *
 * 所属层次：适配器层。
 * 阅读重点：这里把端口接口落到具体后端，阅读时重点看协议转换、资源释放和失败降级。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/*
 * cc_file_write_tool.c — file_write 工具适配器
 *
 * 模块说明：
 *   本文件实现了 "file_write" 工具的适配器（Adapter）。
 *   设计模式：Adapter（适配器）模式 —— 将底层 cc_filesystem_t 文件系统接口
 *   适配为 cc_tool vtable 接口，使 LLM 可通过统一工具接口写入文件。
 *
 * 实现接口：
 *   - cc_tool_vtable_t（5 个虚拟方法：name / description / schema_json / call / destroy）
 *
 * 安全约束：
 *   - 写入路径限制在 workspace 范围内，防止路径穿越攻击
 *   - 强制解析 JSON 参数并校验 path、content 必填
 */

#include "cc/ports/cc_tool.h"
#include "cc/ports/cc_filesystem.h"
#include "cc/ports/cc_path.h"
#include "cc/util/cc_json.h"
#include <stdlib.h>
#include <string.h>

/*
 * cc_file_write_tool_t — file_write 工具的内部数据结构
 *
 * 字段说明：
 *   fs — 底层文件系统实例（cc_filesystem_t），
 *        提供 write_text 等方法用于实际写入文件
 */
typedef struct {
    cc_filesystem_t fs;
} cc_file_write_tool_t;

/*
 * file_write_name — 返回工具名称
 *
 * 功能：返回该工具在工具注册表中的唯一标识名称。
 * 参数：self — 工具实例指针（本函数未使用）
 * 返回值：工具名称字符串 "file_write"
 */
static const char *file_write_name(void *self)
{
    (void)self;
    return "file_write";
}

/*
 * file_write_description — 返回工具描述
 *
 * 功能：返回工具的自然语言描述，供 LLM 理解工具用途。
 * 参数：self — 工具实例指针（本函数未使用）
 * 返回值：工具描述字符串 "Write content to a file"
 */
static const char *file_write_description(void *self)
{
    (void)self;
    return "Write content to a file";
}

/*
 * file_write_schema_json — 返回工具参数的 JSON Schema
 *
 * 功能：定义工具调用时必须/可选的参数及其类型，符合 JSON Schema 规范。
 * 参数：self — 工具实例指针（本函数未使用）
 * 返回值：JSON Schema 字符串，定义了 path（string，必填）和 content（string，必填）参数
 */
static const char *file_write_schema_json(void *self)
{
    (void)self;
    return "{"
        "\"type\":\"object\","
        "\"properties\":{"
            "\"path\":{\"type\":\"string\",\"description\":\"Path to the file to write\"},"
            "\"content\":{\"type\":\"string\",\"description\":\"Content to write\"}"
        "},"
        "\"required\":[\"path\",\"content\"]"
    "}";
}

/*
 * file_write_call — 执行文件写入操作
 *
 * 功能：
 *   1. 解析 JSON 参数，提取目标路径和写入内容
 *   2. 安全校验：将相对路径拼接到 workspace 根目录，检查是否越界
 *   3. 调用底层文件系统 write_text 写入文件内容
 *   4. 将结果填充到 out_result 中
 *
 * 参数：
 *   self      — 工具实例指针
 *   args_json — JSON 格式的调用参数（必须包含 "path" 和 "content" 字段）
 *   ctx       — 工具上下文，包含 workspace_dir（workspace 根目录）
 *   out_result— 输出结果结构体，包含 ok/content/error 字段
 *
 * 返回值：cc_result_t，始终返回 OK（业务错误通过 out_result->ok 标识）
 */
static cc_result_t file_write_call(
    void *self,
    const char *args_json,
    const cc_tool_context_t *ctx,
    cc_tool_result_t *out_result
)
{
    cc_file_write_tool_t *tool = (cc_file_write_tool_t *)self;

    memset(out_result, 0, sizeof(cc_tool_result_t));

    cc_json_value_t *args = NULL;
    cc_result_t rc = cc_json_parse(args_json, &args);
    if (rc.code != CC_OK) {
        out_result->ok = 0;
        out_result->error = strdup("Failed to parse arguments JSON");
        return cc_result_ok();
    }

    cc_json_value_t *path_val = cc_json_object_get(args, "path");
    cc_json_value_t *content_val = cc_json_object_get(args, "content");
    const char *path = cc_json_string_value(path_val);
    const char *content = cc_json_string_value(content_val);

    if (!path || !content) {
        out_result->ok = 0;
        out_result->error = strdup("Missing required parameters: path and content");
        cc_json_destroy(args);
        return cc_result_ok();
    }

    char *full_path = cc_path_join(ctx->workspace_dir, path);
    if (!cc_path_is_within(ctx->workspace_dir, full_path)) {
        out_result->ok = 0;
        out_result->error = strdup("Access denied: path is outside workspace");
        free(full_path);
        cc_json_destroy(args);
        return cc_result_ok();
    }

    rc = tool->fs.vtable->write_text(tool->fs.self, full_path, content);
    free(full_path);
    cc_json_destroy(args);

    if (rc.code != CC_OK) {
        out_result->ok = 0;
        out_result->error = strdup(rc.message ? rc.message : "Failed to write file");
        cc_result_free(&rc);
        return cc_result_ok();
    }

    out_result->ok = 1;
    out_result->content = strdup("File written successfully");
    return cc_result_ok();
}

/*
 * file_write_destroy — 销毁 file_write 工具实例
 *
 * 功能：释放工具实例占用的内存。
 * 参数：self — 工具实例指针
 * 返回值：无
 */
static void file_write_destroy(void *self)
{
    free(self);
}

/*
 * file_write_vtable — file_write 工具的虚拟方法表
 *
 * 说明：将 5 个静态函数绑定为 cc_tool_vtable_t 接口的实现，
 *       使用 Adapter 模式将文件系统功能适配为标准工具接口。
 */
static cc_tool_vtable_t file_write_vtable = {
    file_write_name,
    file_write_description,
    file_write_schema_json,
    file_write_call,
    file_write_destroy
};

/*
 * cc_file_write_tool_create — 创建 file_write 工具实例（工厂函数） 
 *
 * 功能：
 *   1. 分配并初始化 cc_file_write_tool_t 结构体
 *   2. 注入底层文件系统依赖
 *   3. 填充 cc_tool_t 输出参数，返回工厂模式创建的实例
 *
 * 参数：
 *   fs       — 底层文件系统实例（依赖注入）
 *   out_tool — 输出参数，创建成功后包含工具 self 指针和 vtable
 *
 * 返回值：cc_result_t，成功返回 CC_OK，内存不足返回 CC_ERR_OUT_OF_MEMORY
 */
cc_result_t cc_file_write_tool_create(cc_filesystem_t fs, cc_tool_t *out_tool)
{
    cc_file_write_tool_t *self = calloc(1, sizeof(cc_file_write_tool_t));
    if (!self) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create file write tool");

    self->fs = fs;
    out_tool->self = self;
    out_tool->vtable = &file_write_vtable;
    return cc_result_ok();
}