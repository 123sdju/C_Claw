/**
 * 学习导读：cclaw/core/include/cc/util/cc_token_counter.h
 *
 * 所属层次：核心层。
 * 阅读重点：这里声明 token 估算器，重点看近似计数用于上下文裁剪而非计费。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * cc_token_counter.h — Token 估算工具模块
 *
 * @file    cc/util/cc_token_counter.h
 * @brief   提供与 tokenizer 无关的 token 数量估算功能，用于上下文窗口预算管理。
 *
 * 本模块不依赖任何具体的 tokenizer 实现（如 tiktoken），而是使用基于
 * 字符类型的启发式估算。估算结果精度约 ±30%，但足以满足上下文截断和
 * 压缩决策的需求——我们只需要知道"是否接近窗口限制"，不需要精确计数。
 *
 * ─── 估算策略 ─────────────────────────────────────────────────────────
 *
 *   ASCII 文本（英文为主）：约 4 个字符 = 1 token
 *   CJK 文本（中文/日文/韩文）：约 1.5 个字符 = 1 token
 *   混合文本：自动按字符类型分别计数后汇总
 *
 * ─── 接口契约 ─────────────────────────────────────────────────────────
 *
 *   - cc_token_estimate() 估算单段文本的 token 数
 *   - 所有函数返回整数，无错误码（估算永不失败）
 *   - 线程安全：无共享状态
 *
 * ─── 依赖 ─────────────────────────────────────────────────────────────
 *
 *   无任何外部依赖（纯标准 C 和 UTF-8 位运算）。
 */

#ifndef CC_TOKEN_COUNTER_H
#define CC_TOKEN_COUNTER_H

/**
 * cc_token_estimate — 估算文本的 token 数量
 *
 * 遍历 UTF-8 编码的文本字符串，根据字符类型分别估算 token 数：
 *   - ASCII 字符（0x00-0x7F）：每 4 个计 1 token
 *   - 多字节 UTF-8 字符（CJK 等）：每个计 1 token
 *
 * @param text  UTF-8 编码的文本（可为 NULL）
 * @return      估算的 token 数量（>= 0），text 为 NULL 时返回 0
 */
int cc_token_estimate(const char *text);

/**
 * cc_token_estimate_json_messages — 估算 JSON messages 数组的 token 数
 *
 * 对完整的 LLM messages JSON 字符串进行 token 估算。
 * 与 cc_token_estimate 的区别：JSON 中的转义字符（如 \"、\\n）会导致
 * 估算偏高，但这对上下文管理是安全的——高估只会让我们更早触发压缩，
 * 而不会导致超出窗口限制。
 *
 * @param messages_json  JSON 格式的 messages 数组字符串（可为 NULL）
 * @return               估算的 token 数量（>= 0）
 */
int cc_token_estimate_json_messages(const char *messages_json);

#endif
