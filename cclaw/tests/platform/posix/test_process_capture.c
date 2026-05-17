/**
 * 学习导读：cclaw/tests/platform/posix/test_process_capture.c
 *
 * 所属层次：测试层。
 * 阅读重点：这里用小型 Given/When/Then 场景固定行为，阅读时重点看每个断言防止哪类回归。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/*
 * test_process_capture.c
 *
 * 测试目标：验证进程输出捕获功能对大批量输出（80KB）的完整性和正确性。
 *
 * 测试方法：
 * - 通过 sh -c 执行 shell 脚本，循环 5000 次，
 *   每次打印固定字符串 "0123456789abcdef"（16 字节），
 *   总输出量 = 5000 × 16 = 80000 字节（约 80KB）。
 * - 使用 cc_process_run 同步运行子进程，配置：
 *   a) capture_stdout = 1：捕获标准输出
 *   b) capture_stderr = 1：捕获标准错误
 *   c) timeout_ms = 2000：2 秒超时保护
 *
 * 边界条件与验证点：
 * - 大输出量：80KB 输出可能超过默认缓冲区大小（如 4KB/64KB），
 *   验证捕获逻辑是否支持多轮读取和动态扩容。
 * - 超时保护：确认 2 秒超时足以让 shell 循环完成 5000 次迭代，
 *   不会误触发 timed_out。
 * - 精确字节计数：验证 stdout 长度精确等于 80000，
 *   无截断、无多余换行/空格。
 * - 退出码检查：子进程正常结束，exit_code 必须为 0。
 *
 * 通过标准：exit_code == 0，timed_out == false，stdout_len == 80000。
 */

#include "cc/ports/cc_process.h"

#include <stdio.h>
#include <string.h>

int main(void)
{
    /* 构造 shell 命令：循环 5000 次打印 16 字节固定字符串 */
    char *args[] = {
        "sh",
        "-c",
        "i=0; while [ $i -lt 5000 ]; do printf 0123456789abcdef; i=$((i+1)); done",
        NULL
    };

    /* 配置进程运行选项 */
    cc_process_options_t options;
    memset(&options, 0, sizeof(options));
    options.command = "sh";
    options.args = args;
    options.timeout_ms = 2000;     /* 2 秒超时保护，防止进程卡死 */
    options.capture_stdout = 1;    /* 捕获标准输出到内存 */
    options.capture_stderr = 1;    /* 捕获标准错误到内存 */

    /* 同步运行子进程并捕获输出 */
    cc_process_result_t result;
    cc_result_t rc = cc_process_run(&options, &result);
    if (rc.code != CC_OK) {
        fprintf(stderr, "cc_process_run failed: %s\n", rc.message ? rc.message : "unknown");
        cc_result_free(&rc);
        return 1;
    }

    /* 验证输出完整性：5000 次 × 16 字节 = 80000 字节 */
    size_t stdout_len = result.stdout_text ? strlen(result.stdout_text) : 0;
    int ok = result.exit_code == 0 && !result.timed_out && stdout_len == 80000;

    if (!ok) {
        fprintf(stderr, "unexpected result: exit=%d timed_out=%d stdout_len=%zu\n",
            result.exit_code, result.timed_out, stdout_len);
    }

    cc_process_result_free(&result);
    return ok ? 0 : 1;
}
