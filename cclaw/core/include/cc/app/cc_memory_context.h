/**
 * 学习导读：cclaw/core/include/cc/app/cc_memory_context.h
 *
 * 所属层次：核心层。
 * 阅读重点：这里声明 memory prompt 注入入口，重点看 memory store 借用和关闭
 *           memory feature 后的裁剪边界。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * cc_memory_context.h — 长期记忆上下文注入模块
 *
 * @file    cc/app/cc_memory_context.h
 * @brief   将长期记忆存储中的相关知识注入到 LLM 上下文。
 *
 * 本模块负责在每次 LLM 调用前，从记忆存储（cc_memory_store_t）中
 * 检索与当前会话文本语义相关的记忆条目，并将其格式化为一段
 * 可注入 LLM 上下文的记忆文本块。
 *
 * ─── 接口契约 ─────────────────────────────────────────────────────────
 *
 *   - cc_memory_context_inject() 是唯一对外接口
 *   - 传入 store（记忆存储实例）和 session_text（当前会话文本）
 *   - 内部通过 store.search() 检索相关记忆
 *   - 输出一段格式化后的记忆文本块，可直接拼接到 LLM messages 中
 *
 * ─── 工作流程 ─────────────────────────────────────────────────────────
 *
 *   1. 将当前会话文本作为检索 query，调用 store.search()
 *   2. 对命中条目按关联度排序
 *   3. 将条目格式化为"记忆片段"文本块
 *   4. 返回格式化后的文本，供 cc_context_builder 注入 LLM 上下文
 *
 * ─── 依赖 ─────────────────────────────────────────────────────────────
 *
 *   依赖 cc/core/cc_result.h（错误传递）和
 *        cc/ports/cc_memory_store.h（记忆存储端口接口）。
 */

#ifndef CC_MEMORY_CONTEXT_H
#define CC_MEMORY_CONTEXT_H

#include "cc/core/cc_result.h"
#include "cc/ports/cc_memory_store.h"

/**
 * cc_memory_context_inject — 从长期记忆中检索并生成上下文文本块
 *
 * 以当前会话文本作为语义检索 query，在记忆存储中搜索相关条目，
 * 将其格式化为一整段可注入 LLM 上下文的文本。检索不到任何
 * 相关记忆时，out_memory_block 返回 NULL（不报错）。
 *
 * 典型的输出格式类似于：
 *   "[LONG-TERM MEMORY]\n- 用户偏好: Python\n- 上次项目: /home/user/foo\n..."
 *
 * @param store            记忆存储实例（不可为 NULL）
 * @param session_text     当前会话的文本内容（不可为 NULL），用作检索 query
 * @param out_memory_block 输出：格式化后的记忆文本块（调用者负责 free）
 *                         无相关记忆时为 NULL
 * @return                 CC_OK 表示操作完成（即使无命中也是 CC_OK）
 */
cc_result_t cc_memory_context_inject(
    cc_memory_store_t *store,
    const char *session_text,
    char **out_memory_block
);

#endif
