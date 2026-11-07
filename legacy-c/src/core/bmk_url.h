/*
 * bmk_url.h - URL 校验与归一化（附录 B）
 *
 * 纯函数、无全局状态，可直接单元测试（需求 5.5 第 1 条）。
 */
#ifndef BMK_URL_H
#define BMK_URL_H

#include "bmk_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 计算去重键 url_key（附录 B 管线）。
 *
 * - http/https 走完整归一化管线；
 * - 其他 scheme：url_key = 去掉 fragment 后的完整 URL 原文（附录 B 第 9 条/用例 17）。
 *
 * 成功时 *out_key 为新分配字符串，调用者负责 free。
 * URL 无法解析时返回 BMK_ERR_INVALID_URL。
 */
bmk_err bmk_url_normalize(const char *url, char **out_key);

/*
 * 校验用户输入的 URL 是否可保存（FR-02 第 2 条）：
 * 仅接受 http/https，host 非空，不含空格与控制字符。
 * reason 可为 NULL；否则返回稳定的原因标识（不需释放，为静态串）。
 */
bool bmk_url_is_valid_input(const char *url, const char **reason);

/*
 * 提取归一化 host（小写、剥离 www.、IDN 转 punycode）。
 * 失败返回 NULL；成功返回新分配字符串。
 */
char *bmk_url_host(const char *url);

/* 将相对 URL 按 base 绝对化（FR-03 第 3a 条）。失败返回 NULL。 */
char *bmk_url_resolve(const char *base, const char *relative);

/* 百分号解码一次（就地生成新串）。失败返回 NULL。 */
char *bmk_url_percent_decode(const char *s);

/* RFC 3986 点段归一（`/a/./b`→`/a/b`）。返回新分配字符串。 */
char *bmk_url_remove_dot_segments(const char *path);

#ifdef __cplusplus
}
#endif
#endif /* BMK_URL_H */
