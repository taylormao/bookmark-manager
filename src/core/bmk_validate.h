/*
 * bmk_validate.h - 批量校验（FR-14）
 */
#ifndef BMK_VALIDATE_H
#define BMK_VALIDATE_H

#include "bmk_types.h"
#include "bmk_db.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BMK_PROXY_DIRECT = 0,  /* 直连（默认） */
    BMK_PROXY_ALWAYS,      /* 全部走代理 */
    BMK_PROXY_AUTO,        /* 直连失败或 blocked 时换节点重试 1 次 */
} bmk_proxy_mode;

typedef struct {
    int timeout_s;          /* 默认 10 */
    int connect_timeout_s;  /* 默认 5 */
    int max_redirects;      /* 默认 5 */
    int concurrency;        /* 1-10，默认 4 */
    int per_host_limit;     /* 默认 2 */
    int interval_ms;        /* 默认 500 */
    int jitter_ms;          /* 默认 ±200 */
    int retry;              /* 默认 1（仅网络错误与 5xx） */
    bmk_proxy_mode proxy_mode;
} bmk_validate_opts;

void bmk_validate_opts_init(bmk_validate_opts *o);
/* 从 settings 载入校验参数。 */
void bmk_validate_opts_load(bmk_db *db, bmk_validate_opts *o);

/* 单条校验的判定结果（不落库，便于单元测试）。 */
typedef struct {
    int  valid;         /* BMK_VALID_* */
    int  status_code;   /* 网络层失败为 0 */
    char note[32];      /* BMK_NOTE_* */
} bmk_check_result;

/*
 * 依据 HTTP 结果做判定（FR-14 第 5 条）。纯函数，便于单元测试。
 *
 * @param status       最终 HTTP 状态码；网络层失败传 0
 * @param curl_error   网络层错误标识（BMK_NOTE_*），无错误传 NULL
 * @param prev_valid   条目原有 valid（403/429 时保持不变）
 */
void bmk_check_judge(long status, const char *curl_error, int prev_valid,
                     bmk_check_result *out);

/* 进度回调；返回 false 表示取消。 */
typedef bool (*bmk_validate_cb)(int64_t done, int64_t total,
                                const bmk_bookmark *b,
                                const bmk_check_result *res,
                                void *user);

/*
 * 批量校验给定 id 集合。内部使用线程池并发，结果逐条写库。
 * 取消后已写入结果保留（FR-14 第 8 条）。
 */
bmk_err bmk_validate_run(bmk_db *db, const int64_t *ids, size_t n,
                         const bmk_validate_opts *opts,
                         bmk_validate_cb cb, void *user);

#ifdef __cplusplus
}
#endif
#endif /* BMK_VALIDATE_H */
