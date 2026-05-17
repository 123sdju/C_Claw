/**
 * 学习导读：cclaw/platforms/windows/src/cc_windows_thread.c
 *
 * 所属层次：平台层。
 * 阅读重点：这里隐藏 POSIX、Windows、ESP32 的系统 API 差异，阅读时重点看同名端口函数如何按平台实现。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * cc_windows_thread.c — Windows 线程与互斥锁封装
 *
 * 在整体架构中的角色和层次：
 *   本模块位于 Platform 层的 Windows 平台实现子层。
 *   Platform 层是整个系统的最底层，负责封装操作系统差异。
 *   本文件是 cc_thread.h 接口在 Windows（Win32）平台的具体实现，
 *   提供轻量级的线程创建/等待和互斥锁（Mutex）机制。
 *   上层代码（如 Agent 并行执行、Sandbox 并发控制）通过统一的
 *   cc_thread_t / cc_mutex_t 类型操作，无需关心底层是 CreateThread
 *   还是 POSIX pthread。
 *
 * Windows 线程 API 封装：
 *   线程管理：
 *     - cc_thread_create   → CreateThread()        创建新线程
 *     - cc_thread_join     → WaitForSingleObject() + CloseHandle()
 *                           阻塞等待线程结束并释放句柄资源
 *     - 线程函数签名统一为 cc_thread_fn_t（DWORD WINAPI (*)(void*)）
 *
 *   互斥锁管理：
 *     - cc_mutex_create    → InitializeCriticalSection() 初始化临界区
 *     - cc_mutex_destroy   → DeleteCriticalSection() + free() 销毁并释放
 *     - cc_mutex_lock      → EnterCriticalSection() 获取锁（阻塞）
 *     - cc_mutex_unlock    → LeaveCriticalSection() 释放锁
 *
 * CRITICAL_SECTION 的特点（为何选择它而非 Mutex/SRWLock）：
 *   - 递归锁：同一线程可以多次 EnterCriticalSection 而不会死锁，
 *     只需匹配相同次数的 LeaveCriticalSection 即可释放。
 *     这使得在递归调用链中使用锁非常方便，无需担心重入问题。
 *   - 轻量级：CRITICAL_SECTION 是用户态对象，仅在发生竞争时才
 *     进入内核态等待。相比之下，Mutex/HANDLE 始终涉及内核态操作，
 *     性能开销更大。对于进程内多线程同步，CRITICAL_SECTION 更高效。
 *   - 无需跨进程：CRITICAL_SECTION 仅用于同一进程内线程同步，
 *     而本项目的互斥锁使用场景恰好限定在进程内部，符合其设计目标。
 *   - 不可等待超时：CRITICAL_SECTION 不支持带超时的加锁（TryEnterCriticalSection
 *     是非阻塞尝试，但不支持超时）。当前 API 设计中 lock 为阻塞调用，
 *     不需要超时支持，因此此限制不影响使用。
 *
 * 设计决策：
 *   - cc_thread_t 直接映射到 HANDLE（void*），避免额外的包装结构体
 *   - cc_mutex_t 为堆分配的 CRITICAL_SECTION*，因为 CRITICAL_SECTION
 *     是不透明结构且不能值拷贝，堆分配可安全传递指针
 *   - cc_thread_create 不暴露线程 ID、栈大小、创建标志等高级参数，
 *     保持接口简洁，仅覆盖常见使用场景
 *   - cc_thread_join 在等待完成后自动 CloseHandle，RAII 风格的资源管理
 *
 * 平台依赖（Windows 特有，不可移植到 POSIX）：
 *   - CreateThread — 线程创建
 *   - WaitForSingleObject — 线程等待
 *   - CloseHandle — 线程句柄释放
 *   - InitializeCriticalSection / DeleteCriticalSection — 临界区生命周期
 *   - EnterCriticalSection / LeaveCriticalSection — 临界区加锁/解锁
 */

#include "cc/ports/cc_thread.h"

#ifdef _WIN32
#include <windows.h>
#include <stdlib.h>

/*
 * cc_thread_create — 创建新线程
 *
 * 使用 Windows CreateThread API 创建一个新线程运行指定的函数。
 * 线程以默认栈大小和默认安全属性创建，立即开始执行。
 *
 * 参数：
 *   fn         — 线程入口函数，签名为 DWORD WINAPI fn(void*)
 *   arg        — 传递给线程函数的参数指针
 *   out_thread — 输出参数，指向新创建的线程句柄（HANDLE）
 *
 * 返回值：
 *   成功返回 cc_result_ok()，失败返回 CC_ERR_INVALID_ARGUMENT 或 CC_ERR_PLATFORM
 *
 * 平台注意事项：
 *   - 第 4 个参数为 NULL（默认安全属性），句柄不可被子进程继承
 *   - 第 5 个参数为 NULL（默认栈大小），使用与主线程相同的栈大小（通常 1MB）
 *   - 第 6 个参数为 0，线程创建后立即运行
 *   - 线程 ID（最后一个参数为 NULL）不对外暴露
 *   - 创建失败通常意味着系统资源不足或栈空间不足
 */
cc_result_t cc_thread_create(cc_thread_fn_t fn, void *arg, cc_thread_t *out_thread)
{
    if (!fn || !out_thread) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid thread argument");
    }
    HANDLE h = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)fn, arg, 0, NULL);
    if (!h) {
        return cc_result_error(CC_ERR_PLATFORM, "Failed to create thread");
    }
    *out_thread = h;
    return cc_result_ok();
}

/*
 * cc_thread_join — 等待线程结束并释放句柄
 *
 * 阻塞当前线程直到目标线程结束，然后自动关闭线程句柄。
 * 与 POSIX pthread_join 不同，此函数不获取线程返回值。
 *
 * 参数：
 *   thread — 要等待的线程句柄（NULL 时返回错误）
 *
 * 返回值：
 *   成功返回 cc_result_ok()，失败返回 CC_ERR_INVALID_ARGUMENT
 *
 * 平台注意事项：
 *   - 使用 INFINITE 超时，无限等待直到线程结束
 *   - 调用 CloseHandle 后 thread 句柄变为无效，不可重复 join
 *   - 不获取线程返回值（CreateThread 的返回值是 DWORD 退出码）
 */
cc_result_t cc_thread_join(cc_thread_t thread)
{
    if (!thread) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null thread");
    }
    WaitForSingleObject((HANDLE)thread, INFINITE);
    CloseHandle((HANDLE)thread);
    return cc_result_ok();
}

/*
 * cc_mutex_create — 创建互斥锁
 *
 * 分配并初始化一个 Windows CRITICAL_SECTION 作为互斥锁。
 * CRITICAL_SECTION 是递归的、轻量级的用户态锁，仅用于进程内同步。
 *
 * 参数：
 *   out_mutex — 输出参数，指向新创建的互斥锁
 *
 * 返回值：
 *   成功返回 cc_result_ok()，失败返回 CC_ERR_INVALID_ARGUMENT 或 CC_ERR_OUT_OF_MEMORY
 *
 * 实现说明：
 *   - CRITICAL_SECTION 在堆上分配，因为它是不可拷贝的不透明结构
 *   - InitializeCriticalSection 不会失败（内部使用异常处理），无需错误检查
 *   - 返回的 cc_mutex_t 可安全在多线程间传递（指针语义）
 */
cc_result_t cc_mutex_create(cc_mutex_t *out_mutex)
{
    if (!out_mutex) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null mutex output");
    }
    CRITICAL_SECTION *cs = malloc(sizeof(CRITICAL_SECTION));
    if (!cs) {
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate mutex");
    }
    InitializeCriticalSection(cs);
    *out_mutex = cs;
    return cc_result_ok();
}

/*
 * cc_mutex_destroy — 销毁互斥锁
 *
 * 删除 CRITICAL_SECTION 对象并释放其内存。
 * 对 NULL 指针安全（无操作）。
 *
 * 参数：
 *   mutex — 要销毁的互斥锁（可为 NULL）
 *
 * 注意事项：
 *   - 调用前必须确保没有线程持有该锁，否则行为未定义
 *   - DeleteCriticalSection 后不可再使用该锁
 *   - 不能在锁被持有时销毁（EnterCriticalSection 后未 LeaveCriticalSection）
 */
void cc_mutex_destroy(cc_mutex_t mutex)
{
    if (!mutex) return;
    DeleteCriticalSection((CRITICAL_SECTION *)mutex);
    free(mutex);
}

/*
 * cc_mutex_lock — 获取互斥锁（阻塞）
 *
 * 进入临界区，如果锁已被其他线程持有，则阻塞当前线程直到锁可用。
 * 由于 CRITICAL_SECTION 是递归锁，同一线程可多次获取而不会死锁。
 *
 * 参数：
 *   mutex — 要获取的互斥锁（NULL 时无操作）
 *
 * 注意事项：
 *   - 每次 lock 必须匹配一次 unlock
 *   - 递归获取 N 次后需要释放 N 次才能真正释放锁
 */
void cc_mutex_lock(cc_mutex_t mutex)
{
    if (!mutex) return;
    EnterCriticalSection((CRITICAL_SECTION *)mutex);
}

/*
 * cc_mutex_unlock — 释放互斥锁
 *
 * 离开临界区，释放当前线程持有的锁。
 * 如果锁被递归获取多次，仅当释放次数等于获取次数时才真正释放。
 *
 * 参数：
 *   mutex — 要释放的互斥锁（NULL 时无操作）
 *
 * 注意事项：
 *   - 只能由持有锁的线程调用，其他线程调用会导致未定义行为
 *   - 释放次数超过获取次数会导致未定义行为
 */
void cc_mutex_unlock(cc_mutex_t mutex)
{
    if (!mutex) return;
    LeaveCriticalSection((CRITICAL_SECTION *)mutex);
}

#endif