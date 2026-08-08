/*
 * bmk_fetch.h - HTTP 抓取：标题与图标（FR-03 / FR-04）
 */
#ifndef BMK_FETCH_H
#define BMK_FETCH_H

#include "bmk_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BMK_FETCH_MAX_BODY   (2 * 1024 * 1024)  /* 2 MiB（FR-03 第 1 条） */
#define BMK_FETCH_MAX_ICON   (512 * 1024)       /* 512 KiB（FR-03 第 4 条） */
#define BMK_FETCH_TIMEOUT_S  10
#define BMK_FETCH_MAX_REDIR  5

typedef struct {
    char          *title;        /* 可为 NULL */
    unsigned char *icon;         /* 可为 NULL */
    size_t         icon_len;
    char          *icon_mime;    /* 可为 NULL */
    char          *final_url;    /* 跟随重定向后的最终 URL */
    long           first_status; /* 首个响应状态码，用于 301/308 判定 */
    bool           permanent_redirect; /* 原响应为 301/308 且最终 URL 不同 */
} bmk_fetch_result;

void bmk_fetch_result_free(bmk_fetch_result *r);

typedef struct {
    bool  use_icon_service;  /* 外部图标服务开关（FR-03 第 3c 条） */
    const char *proxy;       /* 形如 socks5h://127.0.0.1:1080；NULL 为直连 */
    int   timeout_s;
    const char *user_agent;  /* NULL 时由 UA 池轮换 */
} bmk_fetch_opts;

void bmk_fetch_opts_init(bmk_fetch_opts *o);

/*
 * 抓取标题与图标。网络失败不视为致命错误：返回 BMK_OK 但字段可能为 NULL
 * （FR-03 第 6 条：抓取失败不报错弹窗）。URL 非法时返回 BMK_ERR_INVALID_URL。
 */
bmk_err bmk_fetch_page(const char *url, const bmk_fetch_opts *opts,
                       bmk_fetch_result *out);

/* UA 池（FR-14 第 4 条）：内置 5 个常见浏览器 UA，按调用轮换。 */
const char *bmk_next_user_agent(void);
size_t      bmk_user_agent_count(void);
const char *bmk_user_agent_at(size_t i);

/*
 * 内置默认图标（FR-04 第 2 条）。
 * 返回 64x64 PNG 数据，界面在无自定义图标时使用。
 * len 输出字节数，mime 固定为 "image/png"。调用者不释放。
 */
const unsigned char *bmk_default_icon(size_t *len);
const char          *bmk_default_icon_mime(void);

/*
 * 便携版 CA 证书：Windows 下若 exe 同目录存在 ca-bundle.crt 则显式指定，
 * 避免发布环境缺系统证书导致 https 全部误判 tls_failed。其他平台或文件缺失时
 * 保持 libcurl 默认行为。handle 为 CURL*。
 */
void bmk_curl_set_default_cainfo(void *curl_handle);

/* 全局 curl 初始化/清理（进程级，各入口调用一次）。 */
bmk_err bmk_net_global_init(void);
void    bmk_net_global_cleanup(void);

#ifdef __cplusplus
}
#endif
#endif /* BMK_FETCH_H */
