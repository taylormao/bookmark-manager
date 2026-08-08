/*
 * bmk_fetch.c - 标题与图标抓取实现（FR-03）
 *
 * 图标提取顺序（逐级尝试，任一成功即结束）：
 *   a. HTML 中的 <link rel="icon"|"shortcut icon"|"apple-touch-icon">
 *   b. <scheme>://<host>/favicon.ico
 *   c. 外部图标服务（DuckDuckGo → Google），可在设置关闭
 *   d. 全部失败 → 由界面使用默认图标（FR-04），数据库保持 NULL
 */
#include "bmk_fetch.h"
#include "bmk_log.h"
#include "bmk_url.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>

#ifdef _WIN32
#  include <windows.h>
#endif

#ifndef BMK_HAVE_GUMBO
#  define BMK_HAVE_GUMBO 0
#endif
#if BMK_HAVE_GUMBO
#  include <gumbo.h>
#endif

/* ------------------------------------------------------------------ */
/* UA 池（FR-14 第 4 条）                                              */
/* ------------------------------------------------------------------ */

static const char *UA_POOL[] = {
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/138.0.0.0 Safari/537.36",
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.4 Safari/605.1.15",
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:138.0) Gecko/20100101 Firefox/138.0",
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/137.0.0.0 Safari/537.36",
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/138.0.0.0 Safari/537.36 Edg/138.0.0.0",
};
#define UA_COUNT (sizeof(UA_POOL) / sizeof(UA_POOL[0]))

static unsigned g_ua_cursor = 0;

/* 便携版 CA 证书：Windows 下优先加载 exe 同目录的 ca-bundle.crt（见头文件注释） */
void bmk_curl_set_default_cainfo(void *curl_handle)
{
    CURL *c = (CURL *)curl_handle;
    if (!c) return;
#ifdef _WIN32
    static char cafile[1024];
    static int  cafile_ready = 0;
    if (!cafile_ready) {
        cafile_ready = 1;
        HMODULE h = GetModuleHandleA(NULL);
        if (h && GetModuleFileNameA(h, cafile, (DWORD)sizeof(cafile))) {
            char *slash = strrchr(cafile, '\\');
            if (slash) {
                *slash = '\0';
                strncat(cafile, "\\ca-bundle.crt", sizeof(cafile) - strlen(cafile) - 1);
            }
        } else {
            cafile[0] = '\0';
        }
    }
    if (cafile[0] && bmk_file_exists(cafile))
        curl_easy_setopt(c, CURLOPT_CAINFO, cafile);
#else
    (void)c;
#endif
}

const char *bmk_next_user_agent(void)
{
    /* 轮换即可，无需严格线程安全 */
    unsigned i = g_ua_cursor++;
    return UA_POOL[i % UA_COUNT];
}

size_t bmk_user_agent_count(void) { return UA_COUNT; }

const char *bmk_user_agent_at(size_t i)
{
    return (i < UA_COUNT) ? UA_POOL[i] : NULL;
}

/* ------------------------------------------------------------------ */

bmk_err bmk_net_global_init(void)
{
    return (curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK) ? BMK_OK : BMK_ERR_NETWORK;
}

void bmk_net_global_cleanup(void) { curl_global_cleanup(); }

/* 内置默认图标：一个最小合法 1×1 透明 PNG（FR-04 第 2 条）。
 * 当 fetch 找不到任何图标时，界面使用此占位图。 */
static const unsigned char g_default_png[] = {
    /* PNG signature */
    0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A,
    /* IHDR chunk: 1x1, 8-bit RGBA */
    0x00,0x00,0x00,0x0D,0x49,0x48,0x44,0x52,
    0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,
    0x08,0x06,0x00,0x00,0x00,0x1F,0x15,0xC4,0x89,
    /* CRC of IHDR */
    0x00,0x00,0x00,0x04,0x67,0x41,0x4D,0x41,
    /* sRGB chunk */
    0x00,0x00,0xB1,0x8F,0x0B,0xFC,0x61,0x05,
    0x00,0x00,0x00,0x01,0x73,0x52,0x47,0x42,
    0x00,0xAE,0xCE,0x1C,0xE9,0x00,0x00,0x00,
    /* IDAT chunk: single pixel, alpha=0 (transparent) */
    0x0C,0x49,0x44,0x41,0x54,0x08,0xD7,0x63,
    0x60,0x60,0x60,0x00,0x00,0x00,0x04,0x00,
    0x01,0x27,0x34,0x27,0x0A,0x00,0x00,0x00,
    /* IEND */
    0x00,0x49,0x45,0x4E,0x44,0xAE,0x42,0x60,0x82
};

const unsigned char *bmk_default_icon(size_t *len)
{
    if (len) *len = sizeof(g_default_png);
    return g_default_png;
}

const char *bmk_default_icon_mime(void)
{
    return "image/png";
}

void bmk_fetch_opts_init(bmk_fetch_opts *o)
{
    memset(o, 0, sizeof(*o));
    o->use_icon_service = true;   /* 默认开启（FR-03 第 3c 条） */
    o->timeout_s = BMK_FETCH_TIMEOUT_S;
}

void bmk_fetch_result_free(bmk_fetch_result *r)
{
    if (!r) return;
    free(r->title);
    free(r->icon);
    free(r->icon_mime);
    free(r->final_url);
    memset(r, 0, sizeof(*r));
}

/* ------------------------------------------------------------------ */
/* HTTP 下载                                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    bmk_buf buf;
    size_t  limit;
    bool    exceeded;
} dl_sink;

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    dl_sink *s = (dl_sink *)userdata;
    size_t n = size * nmemb;
    if (s->buf.len + n > s->limit) {
        s->exceeded = true;
        return 0;  /* 中断传输 */
    }
    if (!bmk_buf_append(&s->buf, ptr, n)) return 0;
    return n;
}

typedef struct {
    char  *data;
    size_t len;
    char  *content_type;
    long   status;
    long   first_status;   /* 首个响应状态码（跟随重定向前） */
    char  *effective_url;
} http_response;

static void http_response_free(http_response *r)
{
    if (!r) return;
    free(r->data);
    free(r->content_type);
    free(r->effective_url);
    memset(r, 0, sizeof(*r));
}

/* 头部回调：记录首个响应行（HTTP/1.x NNN ...）的状态码。
 * FR-03 第 7 条要求“原响应为 301/308”，即跟随重定向之前的状态码，
 * 而 CURLINFO_RESPONSE_CODE 返回的是最终状态码，二者不同，需单独捕获。 */
typedef struct {
    long first_status;
    bool seen_first;
} header_state;

static size_t header_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    header_state *h = (header_state *)userdata;
    size_t n = size * nmemb;
    if (!h->seen_first && n >= 12 && strncmp(ptr, "HTTP/", 5) == 0) {
        long code = strtol(ptr + 9, NULL, 10);  /* "HTTP/1.1 200" → 200 */
        if (code > 0) { h->first_status = code; h->seen_first = true; }
    }
    return n;
}

/*
 * 通用 GET。limit 为最大接收字节数，超过即中断（视为失败）。
 */
static bmk_err http_get(const char *url, const bmk_fetch_opts *opts,
                        size_t limit, http_response *out)
{
    memset(out, 0, sizeof(*out));
    CURL *c = curl_easy_init();
    if (!c) return BMK_ERR_NOMEM;
    bmk_curl_set_default_cainfo(c);

    dl_sink sink;
    bmk_buf_init(&sink.buf);
    sink.limit = limit;
    sink.exceeded = false;

    header_state hdr;
    memset(&hdr, 0, sizeof(hdr));

    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, (long)BMK_FETCH_MAX_REDIR);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, (long)(opts->timeout_s > 0 ? opts->timeout_s
                                                                    : BMK_FETCH_TIMEOUT_S));
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &hdr);
    curl_easy_setopt(c, CURLOPT_USERAGENT,
                     opts->user_agent ? opts->user_agent : bmk_next_user_agent());
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    /* 直连时显式禁用环境代理变量（与校验一致） */
    if (opts->proxy && *opts->proxy)
        curl_easy_setopt(c, CURLOPT_PROXY, opts->proxy);
    else
        curl_easy_setopt(c, CURLOPT_PROXY, "");

    CURLcode rc = curl_easy_perform(c);
    /* 因超出大小上限而中断：视为失败 */
    if (rc != CURLE_OK && !(rc == CURLE_WRITE_ERROR && sink.exceeded)) {
        bmk_buf_free(&sink.buf);
        curl_easy_cleanup(c);
        return BMK_ERR_NETWORK;
    }
    if (sink.exceeded) {
        bmk_buf_free(&sink.buf);
        curl_easy_cleanup(c);
        return BMK_ERR_NETWORK;
    }

    long status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    char *ctype = NULL, *eurl = NULL;
    curl_easy_getinfo(c, CURLINFO_CONTENT_TYPE, &ctype);
    curl_easy_getinfo(c, CURLINFO_EFFECTIVE_URL, &eurl);

    out->status = status;
    out->first_status = hdr.first_status ? hdr.first_status : status;
    out->content_type = ctype ? bmk_strdup(ctype) : NULL;
    out->effective_url = eurl ? bmk_strdup(eurl) : NULL;
    out->len = sink.buf.len;
    out->data = bmk_buf_steal(&sink.buf);

    curl_easy_cleanup(c);
    return BMK_OK;
}

/* ------------------------------------------------------------------ */
/* HTML 解析（Gumbo）                                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    char *og_title;
    char *twitter_title;
    char *title;
    char *icon_href;          /* 最佳候选 */
    int   icon_priority;      /* 越小越优先 */
} html_info;

#if BMK_HAVE_GUMBO

static const char *node_attr(const GumboNode *node, const char *name)
{
    if (node->type != GUMBO_NODE_ELEMENT) return NULL;
    GumboAttribute *a = gumbo_get_attribute(&node->v.element.attributes, name);
    return a ? a->value : NULL;
}

/* 取元素的纯文本内容 */
static void collect_text(const GumboNode *node, bmk_buf *out)
{
    if (node->type == GUMBO_NODE_TEXT || node->type == GUMBO_NODE_WHITESPACE) {
        bmk_buf_append_str(out, node->v.text.text);
        return;
    }
    if (node->type != GUMBO_NODE_ELEMENT) return;
    const GumboVector *ch = &node->v.element.children;
    for (unsigned i = 0; i < ch->length; i++)
        collect_text((const GumboNode *)ch->data[i], out);
}

static void scan_html(const GumboNode *node, html_info *info)
{
    if (node->type != GUMBO_NODE_ELEMENT) return;

    GumboTag tag = node->v.element.tag;

    if (tag == GUMBO_TAG_META) {
        const char *prop = node_attr(node, "property");
        const char *name = node_attr(node, "name");
        const char *content = node_attr(node, "content");
        if (content && *content) {
            if (prop && bmk_strcasecmp(prop, "og:title") == 0 && !info->og_title)
                info->og_title = bmk_strdup(content);
            else if (name && bmk_strcasecmp(name, "twitter:title") == 0 && !info->twitter_title)
                info->twitter_title = bmk_strdup(content);
        }
    } else if (tag == GUMBO_TAG_TITLE) {
        if (!info->title) {
            bmk_buf b;
            bmk_buf_init(&b);
            collect_text(node, &b);
            info->title = bmk_buf_steal(&b);
        }
    } else if (tag == GUMBO_TAG_LINK) {
        const char *rel = node_attr(node, "rel");
        const char *href = node_attr(node, "href");
        if (rel && href && *href) {
            int prio = -1;
            /* FR-03 第 3a 条列出的三类 rel */
            if (bmk_strcasecmp(rel, "icon") == 0)               prio = 0;
            else if (bmk_strcasecmp(rel, "shortcut icon") == 0) prio = 1;
            else if (bmk_strcasecmp(rel, "apple-touch-icon") == 0) prio = 2;
            if (prio >= 0 && (!info->icon_href || prio < info->icon_priority)) {
                free(info->icon_href);
                info->icon_href = bmk_strdup(href);
                info->icon_priority = prio;
            }
        }
    }

    const GumboVector *ch = &node->v.element.children;
    for (unsigned i = 0; i < ch->length; i++)
        scan_html((const GumboNode *)ch->data[i], info);
}

#endif /* BMK_HAVE_GUMBO */

/* ------------------------------------------------------------------ */
/* 图标获取                                                            */
/* ------------------------------------------------------------------ */

/* 校验图标响应是否合法（FR-03 第 4 条） */
static bool icon_response_ok(const http_response *r)
{
    if (r->status < 200 || r->status >= 300) return false;
    if (!r->data || r->len == 0) return false;
    if (r->len > BMK_FETCH_MAX_ICON) return false;
    if (!r->content_type) return false;
    return strncmp(r->content_type, "image/", 6) == 0;
}

static bool try_icon_url(const char *url, const bmk_fetch_opts *opts,
                         bmk_fetch_result *out)
{
    http_response r;
    if (http_get(url, opts, BMK_FETCH_MAX_ICON + 1, &r) != BMK_OK) return false;
    if (!icon_response_ok(&r)) { http_response_free(&r); return false; }

    out->icon = (unsigned char *)malloc(r.len);
    if (!out->icon) { http_response_free(&r); return false; }
    memcpy(out->icon, r.data, r.len);
    out->icon_len = r.len;

    /* Content-Type 可能带参数，如 "image/png; charset=..." */
    char *mime = bmk_strdup(r.content_type);
    if (mime) {
        char *semi = strchr(mime, ';');
        if (semi) *semi = '\0';
        bmk_str_trim(mime);
    }
    out->icon_mime = mime;
    http_response_free(&r);
    return true;
}

/* data: URI 图标直接解码入库（FR-03 边界） */
static bool try_icon_data_uri(const char *uri, bmk_fetch_result *out)
{
    if (strncmp(uri, "data:", 5) != 0) return false;
    const char *comma = strchr(uri, ',');
    if (!comma) return false;
    const char *meta = uri + 5;
    const char *semi = strchr(meta, ';');
    if (!semi || semi > comma) return false;
    if (strncmp(semi + 1, "base64", 6) != 0) return false;

    size_t dlen = 0;
    unsigned char *data = bmk_base64_decode(comma + 1, &dlen);
    if (!data) return false;
    if (dlen == 0 || dlen > BMK_FETCH_MAX_ICON) { free(data); return false; }

    out->icon = data;
    out->icon_len = dlen;
    out->icon_mime = bmk_strndup(meta, (size_t)(semi - meta));
    return true;
}

bmk_err bmk_fetch_page(const char *url, const bmk_fetch_opts *opts,
                       bmk_fetch_result *out)
{
    if (!url || !out) return BMK_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));

    bmk_fetch_opts defo;
    if (!opts) { bmk_fetch_opts_init(&defo); opts = &defo; }

    if (!bmk_url_is_valid_input(url, NULL)) return BMK_ERR_INVALID_URL;

    /* --- 1. 抓取页面 --- */
    http_response page;
    bmk_err e = http_get(url, opts, BMK_FETCH_MAX_BODY, &page);
    if (e != BMK_OK) {
        /* 抓取失败不报错，仅记 WARN（FR-03 第 6 条） */
        BMK_WARN("fetch failed for %s", url);
        return BMK_OK;
    }

    out->first_status = page.first_status;
    out->final_url = page.effective_url ? bmk_strdup(page.effective_url) : NULL;
    /* 301/308 且最终 URL 不同 → 由界面询问是否改用最终网址（FR-03 第 7 条）。
     * 用首个响应状态码判定（跟随重定向前），而非 CURLINFO 的最终码。 */
    if (out->final_url && strcmp(out->final_url, url) != 0 &&
        (page.first_status == 301 || page.first_status == 308))
        out->permanent_redirect = true;

    html_info info;
    memset(&info, 0, sizeof(info));

#if BMK_HAVE_GUMBO
    if (page.data && page.len > 0) {
        GumboOutput *g = gumbo_parse_with_options(&kGumboDefaultOptions, page.data, page.len);
        if (g) {
            scan_html(g->root, &info);
            gumbo_destroy_output(&kGumboDefaultOptions, g);
        }
    }
#endif

    /* --- 2. 标题：og:title → twitter:title → <title> --- */
    char *title = info.og_title ? info.og_title
                : info.twitter_title ? info.twitter_title
                : info.title;
    if (title && *title) {
        char *t = bmk_strdup(title);
        if (t) {
            bmk_str_collapse_ws(t);
            bmk_str_truncate_utf8(t, 300);   /* 超过 300 字符截断 */
            if (*t) out->title = t;
            else free(t);
        }
    }

    /* --- 3. 图标 --- */
    char *host = bmk_url_host(url);
    const char *base = out->final_url ? out->final_url : url;

    /* a. HTML 中的 <link rel=icon> */
    if (info.icon_href) {
        if (strncmp(info.icon_href, "data:", 5) == 0) {
            try_icon_data_uri(info.icon_href, out);
        } else {
            char *abs = bmk_url_resolve(base, info.icon_href);
            if (abs) {
                try_icon_url(abs, opts, out);
                free(abs);
            }
        }
    }

    /* b. /favicon.ico —— 使用页面实际 scheme（FR-03 第 3b 条：
     *    `<scheme>://<host>/favicon.ico`；原先固定 https 会导致纯 http 站点取不到） */
    if (!out->icon && host) {
        const char *scheme = (strncmp(base, "http://", 7) == 0) ? "http" : "https";
        char fav[512];
        snprintf(fav, sizeof(fav), "%s://%s/favicon.ico", scheme, host);
        try_icon_url(fav, opts, out);
    }

    /* c. 外部图标服务（默认开启，可关闭；关闭后不得发起外部请求） */
    if (!out->icon && host && opts->use_icon_service) {
        char svc[512];
        snprintf(svc, sizeof(svc), "https://icons.duckduckgo.com/ip3/%s.ico", host);
        if (!try_icon_url(svc, opts, out)) {
            snprintf(svc, sizeof(svc),
                     "https://www.google.com/s2/favicons?domain=%s&sz=64", host);
            try_icon_url(svc, opts, out);
        }
    }

    /* d. 仍无图标 → 保持 NULL，界面用默认图标（FR-04 第 2 条） */

    free(host);
    free(info.og_title);
    free(info.twitter_title);
    free(info.title);
    free(info.icon_href);
    http_response_free(&page);
    return BMK_OK;
}
