/**
 * 学习导读：cclaw/ports/include/cc/ports/cc_sandbox.h
 *
 * 所属层次：端口层。
 * 阅读重点：这里定义可替换接口，阅读时重点看 struct + vtable + void *self 如何表达多态和依赖注入。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * cc_sandbox.h — 命令执行沙箱端口（Port）
 *
 * @file    cc/ports/cc_sandbox.h
 * @brief   定义在受控隔离环境中执行系统命令的抽象接口。采用 vtable 多态模式。
 *
 * 沙箱（cc_sandbox_t）是整个 c-claw 项目中安全执行外部命令的关键抽象。
 * 当 Agent 需要执行 bash 命令、运行脚本或调用外部程序时，所有调用都
 * 必须经过沙箱层。沙箱负责：
 *
 *   1. 环境隔离：限制命令可访问的文件系统和网络资源
 *   2. 超时控制：防止命令无限期运行
 *   3. 输出捕获：收集 stdout 和 stderr 用于返回给 LLM
 *   4. 执行策略：可在此层集成安全检查（与 cc_policy_engine_t 配合）
 *
 * ─── 架构定位 ─────────────────────────────────────────────────────────
 *
 * 在端口-适配器（Ports & Adapters）架构中：
 *   - 本模块是"端口"（Port）：定义了命令执行的抽象合约
 *   - 具体沙箱实现（Docker、chroot、本地进程等）是"适配器"
 *   - 上层工具（如 bash 工具）只依赖本端口，不感知隔离方式
 *
 * ─── 安全模型 ─────────────────────────────────────────────────────────
 *
 *   沙箱不是可选的——每个命令执行都应通过沙箱。
 *   适配器层的安全等级从低到高：
 *     - 本地进程（最不安全，仅限开发环境）
 *     - Docker 容器（推荐：提供文件系统和网络隔离）
 *     - 虚拟机（最高：完全系统隔离）
 *
 *   无论哪种适配器，超时机制必须可靠：即使适配器层实现有缺陷，
 *   也必须保证命令不会无限运行（可使用 alarm/setrlimit + SIGKILL）。
 *
 * ─── 接口契约 ─────────────────────────────────────────────────────────
 *
 *   每个 cc_sandbox_t 由两个部分组成：
 *     - self    : 指向具体沙箱实现的私有数据（容器 ID、进程句柄等）
 *     - vtable  : 指向虚函数表，定义了 run() 和 destroy()
 *
 *   实现者必须填充 vtable 中的所有函数指针。
 *   调用者只依赖 vtable，不感知具体沙箱实现。
 *
 * ─── 使用模式 ─────────────────────────────────────────────────────────
 *
 *   // 1. 构建命令
 *   cc_sandbox_command_t cmd = {
 *       .command     = "ls -la /workspace",
 *       .working_dir = "/workspace",
 *       .env         = NULL,
 *       .timeout_ms  = 30000
 *   };
 *
 *   // 2. 通过沙箱执行
 *   cc_sandbox_result_t result;
 *   cc_result_t rc = sandbox->vtable->run(sandbox->self, &cmd, &result);
 *
 *   // 3. 检查结果
 *   if (rc.code == CC_OK) {
 *       printf("exit: %d, stdout: %s\n", result.exit_code, result.stdout_text);
 *       if (result.timed_out) {
 *           printf("命令超时！\n");
 *       }
 *   }
 *
 *   // 4. 释放结果
 *   cc_sandbox_result_free(&result);
 *
 * ─── 依赖 ─────────────────────────────────────────────────────────────
 *
 *   仅依赖 cc/core/cc_result.h（统一错误传递）。
 *   不依赖任何其他 cc_* 模块。
 */

#ifndef CC_SANDBOX_H
#define CC_SANDBOX_H

#include "cc/core/cc_result.h"

/**
 * cc_sandbox_command_t — 沙箱执行命令描述
 *
 * 封装一条待执行命令的所有参数。调用方在栈上构造此结构体，
 * 然后通过沙箱的 run() 方法提交执行。所有字符串字段均在
 * 调用期间保持有效即可，沙箱实现不会持有这些指针。
 *
 * 安全考量：command 字段中的内容可能来自 LLM 生成的代码，
 * 因此沙箱适配器必须妥善处理特殊字符和潜在的注入攻击。
 */
typedef struct cc_sandbox_command {
    char *command;      /**< 要执行的命令字符串（shell 格式）。
                         *   例如 "gcc -o hello hello.c" 或 "ls -la | grep foo"。
                         *   该字符串会传递给系统的 shell 解释执行。
                         *   适配器层应考虑 shell 注入风险，必要时对参数做转义。 */
    char *working_dir;  /**< 命令执行的工作目录（绝对路径）。
                         *   如果为 NULL，使用默认工作目录（沙箱实现定义）。
                         *   建议始终显式设置以限制文件访问范围。 */
    char **env;         /**< 环境变量数组，以 NULL 结尾。
                         *   格式：{"KEY=VALUE", "PATH=/bin", NULL}。
                         *   如果为 NULL，沙箱实现应提供最小安全环境变量集合。
                         *   注意：应清除危险的 env（如 LD_PRELOAD、IFS 等）。 */
    int timeout_ms;     /**< 命令执行的超时时间（毫秒）。
                         *   设为 0 表示使用沙箱默认超时（建议不低于 30 秒）。
                         *   设为负数表示无超时（危险，仅限受信任的命令）。
                         *   超时后命令进程将被强制终止（SIGKILL）。 */
} cc_sandbox_command_t;

/**
 * cc_sandbox_result_t — 沙箱命令执行结果
 *
 * 封装命令执行完毕后的返回信息。无论命令本身成功还是失败，
 * 只要沙箱正常完成了执行过程，run() 都应返回 CC_OK，
 * 而具体的成功/失败通过 exit_code 判断。
 *
 * 所有动态分配的字符串字段（stdout_text、stderr_text）必须通过
 * cc_sandbox_result_free() 统一释放。
 */
typedef struct cc_sandbox_result {
    int exit_code;      /**< 命令的退出码。
                         *   0 通常表示成功，非零表示某种错误。
                         *   如果命令被信号终止，此值由沙箱实现定义
                         *   （通常为 128 + 信号编号，与 shell 约定一致）。 */
    char *stdout_text;  /**< 命令的标准输出内容（UTF-8 文本）。
                         *   可能为空字符串，但不会为 NULL（除非分配失败）。
                         *   注意：输出可能非常大（如 cat 大文件），
                         *   沙箱实现应考虑对输出大小做截断限制。 */
    char *stderr_text;  /**< 命令的标准错误输出内容（UTF-8 文本）。
                         *   可能为空字符串，但不会为 NULL（除非分配失败）。
                         *   错误输出通常用于诊断和返回给 LLM 分析。 */
    int timed_out;      /**< 是否因超时被终止。
                         *   1 表示命令超时（此时 exit_code 可能无效），
                         *   0 表示正常结束（无论成功或失败）。 */
} cc_sandbox_result_t;

/* ── 前向声明 ───────────────────────────────────────────────────────── */

typedef struct cc_sandbox_vtable cc_sandbox_vtable_t;
typedef struct cc_sandbox cc_sandbox_t;

/**
 * cc_sandbox_t — 沙箱实例（多态句柄）
 *
 * 这是一个值语义的结构体，通过 self + vtable 实现多态。
 * 可以直接按值传递和拷贝，浅拷贝后两个实例指向同一个底层沙箱。
 *
 * @note 浅拷贝后的实例共享同一个 self，不要在其中一个上调用 destroy，
 *       否则另一个实例将成为悬空指针。
 */
struct cc_sandbox {
    void *self;                       /**< 指向具体沙箱实现的私有数据。
                                       *   如 Docker 沙箱可能存储容器 ID、
                                       *   本地沙箱可能存储进程控制信息。 */
    const cc_sandbox_vtable_t *vtable; /**< 虚函数表指针，定义了 run 和 destroy */
};

/**
 * cc_sandbox_vtable_t — 沙箱虚函数表
 *
 * 定义命令执行沙箱的抽象接口。每个具体的隔离策略
 * （本地进程、Docker、chroot 等）都通过实现此表来提供沙箱服务。
 */
struct cc_sandbox_vtable {
    /**
     * run — 在沙箱中执行一条命令
     *
     * 该函数是沙箱的核心操作。它接收一个命令描述，在隔离环境中
     * 执行该命令，并将执行结果填充到 out_result 中。
     *
     * 实现要求：
     *   - 命令必须在隔离环境中执行（如独立进程、容器等）
     *   - 必须捕获 stdout 和 stderr
     *   - 必须遵守 timeout_ms 超时设置
     *   - 如果沙箱自身出错（无法创建进程等），返回非 CC_OK 的错误码
     *   - 命令执行超时时，设置 out_result->timed_out = 1
     *   - 命令正常执行完毕（成功或失败）时，返回 CC_OK，
     *     通过 out_result->exit_code 判断命令自身是否成功
     *
     * @param self       沙箱私有数据
     * @param command    待执行的命令描述（不可为 NULL）
     * @param out_result 输出：命令执行结果。调用方必须在用完后
     *                   调用 cc_sandbox_result_free() 释放。
     * @return           CC_OK 表示沙箱成功执行了命令（不表示命令成功），
     *                   非 CC_OK 表示沙箱自身遇到错误
     */
    cc_result_t (*run)(
        void *self,
        const cc_sandbox_command_t *command,
        cc_sandbox_result_t *out_result
    );

    /**
     * destroy — 销毁沙箱实例
     *
     * 释放 self 指向的私有数据。如果是 Docker 沙箱，可能包括
     * 停止并删除容器；如果是本地进程沙箱，可能包括终止守护进程。
     * 传入 NULL self 是安全的（无操作）。
     *
     * @param self  沙箱私有数据（可为 NULL）
     */
    void (*destroy)(void *self);
};

/**
 * cc_sandbox_result_free — 释放沙箱执行结果中的动态内存
 *
 * 释放 stdout_text 和 stderr_text 字符串的内存。
 * 不释放 result 指针本身（它通常在栈上）。
 * 传入 NULL 是安全的。重复调用也是安全的。
 *
 * @param result  要释放的执行结果指针（可为 NULL）
 */
void cc_sandbox_result_free(cc_sandbox_result_t *result);

#endif