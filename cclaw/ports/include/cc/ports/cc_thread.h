/**
 * 学习导读：cclaw/ports/include/cc/ports/cc_thread.h
 *
 * 所属层次：端口层。
 * 阅读重点：这里定义可替换接口，阅读时重点看 struct + vtable + void *self 如何表达多态和依赖注入。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * cc_thread.h — 线程与互斥锁平台抽象层
 *
 * @file    cc/ports/cc_thread.h
 * @brief   提供跨平台的线程创建和互斥锁操作接口。
 *
 * 本模块封装了不同操作系统（POSIX pthread、Windows、ESP32 FreeRTOS）
 * 的线程原语，为上层提供统一的线程管理和同步接口。
 * 上层模块（如 cc_agent_runtime_t）通过互斥锁保护共享的可变状态。
 *
 * ─── 接口契约 ─────────────────────────────────────────────────────────
 *
 *   - 线程生命周期：create → join（等待线程结束）
 *   - 互斥锁生命周期：create → lock/unlock → destroy
 *   - cc_thread_t 和 cc_mutex_t 是不透明类型（void *），由平台实现
 *   - 所有创建函数通过 cc_result_t 返回错误信息
 *
 * ─── 线程模型 ─────────────────────────────────────────────────────────
 *
 *   ┌──────────────────┐    ┌──────────────────┐
 *   │  cc_thread_create │    │  cc_mutex_create  │
 *   │        │          │    │        │          │
 *   │   fn(arg) 线程    │    │   lock()/unlock() │
 *   │        │          │    │        │          │
 *   │  cc_thread_join   │    │  cc_mutex_destroy │
 *   └──────────────────┘    └──────────────────┘
 *
 * ─── 平台适配 ─────────────────────────────────────────────────────────
 *
 *   POSIX:      pthread_create / pthread_mutex_t
 *   Windows:    CreateThread     / CRITICAL_SECTION 或 SRWLOCK
 *   ESP32:      xTaskCreate      / SemaphoreHandle_t（互斥信号量）
 *
 * ─── 依赖 ─────────────────────────────────────────────────────────────
 *
 *   依赖 cc/core/cc_result.h 和 <stddef.h>（size_t 定义）。
 */

#ifndef CC_THREAD_H
#define CC_THREAD_H

#include "cc/core/cc_result.h"
#include <stddef.h>

/**
 * cc_thread_fn_t — 线程入口函数签名
 *
 * 定义线程启动时需要执行的函数类型。参数 arg 由 cc_thread_create
 * 传递，返回值由 cc_thread_join 获取（通常返回 NULL）。
 */
typedef void *(*cc_thread_fn_t)(void *arg);

/**
 * cc_thread_t — 线程句柄（不透明类型）
 *
 * 封装底层平台的线程句柄。在 POSIX 上为 pthread_t*，
 * Windows 上为 HANDLE，ESP32 上为 TaskHandle_t。
 * 上层代码不感知具体类型，仅通过 create/join 管理生命周期。
 */
typedef void *cc_thread_t;

/**
 * cc_mutex_t — 互斥锁句柄（不透明类型）
 *
 * 封装底层平台的互斥锁句柄。用于保护多线程共享的可变数据。
 * 遵循标准的 lock/unlock 配对使用模式。
 */
typedef void *cc_mutex_t;

/**
 * cc_cond_t — 条件变量句柄（不透明类型）
 *
 * 条件变量用于“有工作才唤醒”的队列模型。和互斥锁配合使用：
 * 调用方先持有 mutex，发现队列为空后调用 cc_cond_wait；等待期间平台层会
 * 原子地释放 mutex，收到 signal/broadcast 后重新持有 mutex 并返回。
 *
 * 为什么放在 port 层：
 *   run queue、plugin worker pool、MCP runtime sweep 都需要阻塞等待能力。
 *   如果只用轮询 sleep，会在桌面浪费 CPU，也会让 ESP 这类设备更耗电。
 */
typedef void *cc_cond_t;

/**
 * cc_thread_create — 创建并启动新线程
 *
 * 在新线程中执行 fn(arg)，立即返回。新线程独立调度。
 * 调用者需要 cc_thread_join() 等待线程结束并回收资源。
 *
 * @param fn         线程入口函数（不可为 NULL）
 * @param arg        传递给 fn 的参数（可为 NULL，fn 自行处理）
 * @param out_thread 输出：新线程的句柄（调用者负责 cc_thread_join）
 * @return           CC_OK 表示线程创建成功
 */
cc_result_t cc_thread_create(cc_thread_fn_t fn, void *arg, cc_thread_t *out_thread);

/**
 * cc_thread_join — 等待线程结束并回收资源
 *
 * 阻塞调用线程，直到 target 线程执行完毕。类似 pthread_join。
 * join 后线程句柄失效，不可再次使用。
 *
 * @param thread  待等待的线程句柄（由 cc_thread_create 返回）
 * @return        CC_OK 表示线程已正常结束
 */
cc_result_t cc_thread_join(cc_thread_t thread);

/**
 * cc_mutex_create — 创建互斥锁对象
 *
 * 分配并初始化一个平台相关的互斥锁。初始状态为未锁定。
 *
 * @param out_mutex  输出：新互斥锁的句柄（调用者负责 cc_mutex_destroy）
 * @return           CC_OK 表示互斥锁创建成功
 */
cc_result_t cc_mutex_create(cc_mutex_t *out_mutex);

/**
 * cc_mutex_destroy — 销毁互斥锁并释放资源
 *
 * 销毁前必须确保锁处于未锁定状态，否则行为未定义。
 *
 * @param mutex  待销毁的互斥锁句柄
 */
void cc_mutex_destroy(cc_mutex_t mutex);

/**
 * cc_mutex_lock — 获取互斥锁（阻塞等待）
 *
 * 如果锁已被其他线程持有，当前线程阻塞直到锁可用。
 * 成功返回后，调用者独占临界区，操作完成后必须调用 cc_mutex_unlock()。
 *
 * @param mutex  互斥锁句柄
 */
void cc_mutex_lock(cc_mutex_t mutex);

/**
 * cc_mutex_unlock — 释放互斥锁
 *
 * 将锁标记为可用，允许其他等待的线程获取。
 * 必须在 cc_mutex_lock 配对使用，每次 lock 对应一次 unlock。
 *
 * @param mutex  互斥锁句柄
 */
void cc_mutex_unlock(cc_mutex_t mutex);

/**
 * cc_cond_create — 创建条件变量对象。
 *
 * @param out_cond 输出：新条件变量句柄（调用者负责 cc_cond_destroy）
 * @return         CC_OK 表示成功
 */
cc_result_t cc_cond_create(cc_cond_t *out_cond);

/**
 * cc_cond_destroy — 销毁条件变量。
 *
 * 调用前必须确保没有线程仍在等待该条件变量。
 *
 * @param cond 待销毁的条件变量句柄
 */
void cc_cond_destroy(cc_cond_t cond);

/**
 * cc_cond_wait — 等待条件变量被唤醒。
 *
 * 调用方必须在进入此函数前持有 mutex。函数返回时仍然持有 mutex。
 * 由于所有平台都可能发生“伪唤醒”，调用方必须在 while 循环中检查条件。
 *
 * @param cond  条件变量句柄
 * @param mutex 与条件变量配对使用的互斥锁
 */
void cc_cond_wait(cc_cond_t cond, cc_mutex_t mutex);

/**
 * cc_cond_timedwait — 带超时等待条件变量。
 *
 * 调用方必须在进入此函数前持有 mutex，函数返回时仍然持有 mutex。
 * 返回 1 表示被 signal/broadcast 唤醒，返回 0 表示超时或平台无法区分的
 * 非致命等待结束。调用方仍然必须用 while 循环重新检查自己的条件。
 *
 * 这个接口主要用于协作式取消：core 队列或 tool pool 可以每隔几十毫秒醒来
 * 查询 cc_cancel_token，而不需要平台层知道上层取消令牌的具体语义。
 *
 * @param cond       条件变量句柄
 * @param mutex      与条件变量配对使用的互斥锁
 * @param timeout_ms 最大等待时间；<=0 时等价于一次普通 wait
 * @return           1 表示被唤醒，0 表示超时/未唤醒
 */
int cc_cond_timedwait(cc_cond_t cond, cc_mutex_t mutex, int timeout_ms);

/**
 * cc_cond_signal — 唤醒一个等待线程。
 *
 * @param cond 条件变量句柄
 */
void cc_cond_signal(cc_cond_t cond);

/**
 * cc_cond_broadcast — 唤醒全部等待线程。
 *
 * @param cond 条件变量句柄
 */
void cc_cond_broadcast(cc_cond_t cond);

#endif
