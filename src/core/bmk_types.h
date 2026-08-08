/*
 * bmk_types.h - 基础类型、错误码与通用工具（libbookmark_core 公共头）
 *
 * 本头文件不依赖 GTK，可被 CLI 与单元测试直接使用（需求 2.2 约束 1）。
 */
#ifndef BMK_TYPES_H
#define BMK_TYPES_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* 错误码：数值稳定，文案可翻译（需求 5.6 第 2 条）                    */
/* ------------------------------------------------------------------ */
typedef enum {
    BMK_OK = 0,
    BMK_ERR_INVALID_ARG = 1,
    BMK_ERR_NOMEM = 2,
    BMK_ERR_IO = 3,
    BMK_ERR_DB = 4,
    BMK_ERR_DB_CORRUPT = 5,
    BMK_ERR_NOT_FOUND = 6,
    BMK_ERR_CONFLICT = 7,       /* 唯一约束冲突，如同级同名分类 */
    BMK_ERR_INVALID_URL = 8,
    BMK_ERR_PARSE = 9,          /* 文件/链接解析失败 */
    BMK_ERR_NETWORK = 10,
    BMK_ERR_CANCELED = 11,
    BMK_ERR_SUBPROCESS = 12,
    BMK_ERR_AUTH = 13,
    BMK_ERR_UNSUPPORTED = 14,
    BMK_ERR_PERMISSION = 15,
} bmk_err;

/* 返回错误码对应的稳定英文标识（用于日志/测试断言）。 */
const char *bmk_err_str(bmk_err e);

/* ------------------------------------------------------------------ */
/* 三态有效性（需求 3.3 bookmarks.valid）                              */
/* ------------------------------------------------------------------ */
#define BMK_VALID_UNKNOWN (-1) /* 数据库中为 NULL */
#define BMK_VALID_BAD     0
#define BMK_VALID_GOOD    1

/* check_note 的稳定取值（需求 3.3 / FR-14 第 5 条） */
#define BMK_NOTE_OK             "ok"
#define BMK_NOTE_BLOCKED        "blocked"
#define BMK_NOTE_DNS_FAILED     "dns_failed"
#define BMK_NOTE_TIMEOUT        "timeout"
#define BMK_NOTE_CONNECT_FAILED "connect_failed"
#define BMK_NOTE_TLS_FAILED     "tls_failed"
#define BMK_NOTE_NETWORK_ERROR  "network_error"

/* ------------------------------------------------------------------ */
/* 动态字符串缓冲（导入导出与 URL 归一化大量使用）                     */
/* ------------------------------------------------------------------ */
typedef struct {
    char  *data;  /* 始终以 '\0' 结尾（len 不含结尾符） */
    size_t len;
    size_t cap;
} bmk_buf;

void  bmk_buf_init(bmk_buf *b);
void  bmk_buf_free(bmk_buf *b);
bool  bmk_buf_reserve(bmk_buf *b, size_t need);
bool  bmk_buf_append(bmk_buf *b, const char *data, size_t n);
bool  bmk_buf_append_str(bmk_buf *b, const char *s);
bool  bmk_buf_append_char(bmk_buf *b, char c);
bool  bmk_buf_printf(bmk_buf *b, const char *fmt, ...);
void  bmk_buf_clear(bmk_buf *b);
/* 取出缓冲区所有权，调用后 b 被重置为空。 */
char *bmk_buf_steal(bmk_buf *b);

/* ------------------------------------------------------------------ */
/* 字符串工具                                                          */
/* ------------------------------------------------------------------ */
char *bmk_strdup(const char *s);            /* s 为 NULL 时返回 NULL */
char *bmk_strndup(const char *s, size_t n);
int   bmk_strcasecmp(const char *a, const char *b);
/* 去首尾空白（原地），返回 s。 */
char *bmk_str_trim(char *s);
/* 连续空白（含换行/制表）压缩为单个空格并去首尾（FR-03 第 2 条）。 */
char *bmk_str_collapse_ws(char *s);
/* 不区分大小写的子串查找（ASCII）。 */
const char *bmk_str_casestr(const char *hay, const char *needle);
/* 按 UTF-8 字符边界安全截断到最多 max_chars 个字符（原地）。 */
void  bmk_str_truncate_utf8(char *s, size_t max_chars);

/* ------------------------------------------------------------------ */
/* 时间与路径                                                          */
/* ------------------------------------------------------------------ */
int64_t bmk_now(void);  /* Unix 秒（UTC） */
/* 生成 yyyyMMdd_HHmmss 形式的时间戳（本地时区），写入 out（≥16 字节）。 */
void bmk_timestamp_str(int64_t ts, char *out, size_t outsz);
/* 生成 ISO-8601 时间串，如 2026-08-07T10:00:00Z。 */
void bmk_iso8601(int64_t ts, char *out, size_t outsz);

/* 路径拼接，返回新分配的字符串。 */
char *bmk_path_join(const char *a, const char *b);
/* 递归创建目录。 */
bmk_err bmk_mkdirs(const char *path);
bool bmk_file_exists(const char *path);
/* 读取整个文件到内存（追加 '\0'）。*out_len 不含结尾符。 */
bmk_err bmk_read_file(const char *path, char **out, size_t *out_len);
bmk_err bmk_write_file(const char *path, const void *data, size_t len);

/* Base64 */
char *bmk_base64_encode(const unsigned char *data, size_t len);
/* 解码失败返回 NULL。*out_len 为解码字节数。 */
unsigned char *bmk_base64_decode(const char *text, size_t *out_len);

#ifdef __cplusplus
}
#endif
#endif /* BMK_TYPES_H */
