/*
 * bmk_log.h - 日志（需求 2.5）
 *
 * 文件 <数据目录>/logs/app.log，滚动上限 5MB x 3，默认级别 INFO。
 * 敏感信息（PAT、密钥、完整 vless 链接）不得写入，调用方应先用
 * bmk_log_redact() 脱敏。
 */
#ifndef BMK_LOG_H
#define BMK_LOG_H

#include "bmk_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BMK_LOG_DEBUG = 0,
    BMK_LOG_INFO  = 1,
    BMK_LOG_WARN  = 2,
    BMK_LOG_ERROR = 3,
} bmk_log_level;

/* 初始化日志；log_dir 为空则只输出到 stderr。 */
bmk_err bmk_log_init(const char *log_dir, bmk_log_level level);
void    bmk_log_shutdown(void);
void    bmk_log_set_level(bmk_log_level level);
bmk_log_level bmk_log_get_level(void);

void bmk_logf(bmk_log_level level, const char *fmt, ...);

#define BMK_DEBUG(...) bmk_logf(BMK_LOG_DEBUG, __VA_ARGS__)
#define BMK_INFO(...)  bmk_logf(BMK_LOG_INFO,  __VA_ARGS__)
#define BMK_WARN(...)  bmk_logf(BMK_LOG_WARN,  __VA_ARGS__)
#define BMK_ERROR(...) bmk_logf(BMK_LOG_ERROR, __VA_ARGS__)

/*
 * 对密钥类文本脱敏：保留前 2 位与后 2 位，中间用 '*' 代替。
 * 返回新分配字符串，调用者负责 free。
 */
char *bmk_log_redact(const char *secret);

#ifdef __cplusplus
}
#endif
#endif /* BMK_LOG_H */
