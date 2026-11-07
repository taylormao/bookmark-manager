/*
 * bmk_url.c - URL 归一化实现（附录 B）
 *
 * 管线（严格按附录 B 顺序）：
 *   1. libcurl URL API 解析（失败=非法）
 *   2. scheme 转小写
 *   3. host 转小写并剥离前缀 "www."
 *   4. 删除默认端口
 *   5. 删除 fragment
 *   6. path 点段归一 + 百分号解码一次 + 去尾斜杠
 *   7. query 拆分并各解码一次
 *   8. 白名单过滤（bilibili 留 p，youtube 留 v，其余全删）
 *   9. 保留参数按 (name, value) 升序（UTF-8 字节序）
 *  10. 拼接
 */
#include "bmk_url.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>

/* ------------------------------------------------------------------ */
/* 百分号解码                                                          */
/* ------------------------------------------------------------------ */

static int hexval(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

char *bmk_url_percent_decode(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s);
    char *out = (char *)malloc(n + 1);
    if (!out) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '%' && i + 2 < n) {
            int hi = hexval((unsigned char)s[i + 1]);
            int lo = hexval((unsigned char)s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out[o++] = (char)((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        out[o++] = s[i];
    }
    out[o] = '\0';
    return out;
}

/* ------------------------------------------------------------------ */
/* 点段归一（RFC 3986 5.2.4）                                          */
/* ------------------------------------------------------------------ */

char *bmk_url_remove_dot_segments(const char *path)
{
    if (!path) return NULL;
    size_t n = strlen(path);
    /* 输出段栈：记录每个段在 out 中的起始偏移 */
    char *out = (char *)malloc(n + 2);
    if (!out) return NULL;
    size_t *starts = (size_t *)malloc((n + 2) * sizeof(size_t));
    if (!starts) { free(out); return NULL; }

    size_t olen = 0, nseg = 0;
    bool absolute = (n > 0 && path[0] == '/');
    bool trailing_slash = false;

    size_t i = 0;
    if (absolute) i = 1;

    while (i <= n) {
        /* 取出一个段 [i, j) */
        size_t j = i;
        while (j < n && path[j] != '/') j++;
        size_t seglen = j - i;
        const char *seg = path + i;

        if (seglen == 1 && seg[0] == '.') {
            trailing_slash = true;
        } else if (seglen == 2 && seg[0] == '.' && seg[1] == '.') {
            if (nseg > 0) {
                nseg--;
                olen = starts[nseg];
            }
            /* `..` 越界时保留原样不报错（附录 B 第 6 条）：即忽略 */
            trailing_slash = true;
        } else if (seglen == 0) {
            /* 连续斜杠或结尾斜杠 */
            if (j >= n) trailing_slash = true;
        } else {
            starts[nseg++] = olen;
            out[olen++] = '/';
            memcpy(out + olen, seg, seglen);
            olen += seglen;
            trailing_slash = false;
        }

        if (j >= n) break;
        i = j + 1;
        if (i > n) break;
    }

    /* 保留末尾斜杠：/a/b/ 与 /a/b 是不同路径（RFC 3986 5.2.4） */
    if (trailing_slash && olen > 0 && out[olen - 1] != '/')
        out[olen++] = '/';
    out[olen] = '\0';
    free(starts);

    if (olen == 0) {
        /* 空 path 视为 "/" */
        out[0] = '/';
        out[1] = '\0';
    }
    return out;
}

/* ------------------------------------------------------------------ */
/* query 参数                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    char *name;
    char *value;   /* 可为 NULL（无 '=' 时） */
} qparam;

static int qparam_cmp(const void *a, const void *b)
{
    const qparam *x = (const qparam *)a;
    const qparam *y = (const qparam *)b;
    int c = strcmp(x->name ? x->name : "", y->name ? y->name : "");
    if (c != 0) return c;
    return strcmp(x->value ? x->value : "", y->value ? y->value : "");
}

/* host 是否命中白名单域（等于 domain 或以 .domain 结尾） */
static bool host_matches(const char *host, const char *domain)
{
    if (!host || !domain) return false;
    size_t hl = strlen(host), dl = strlen(domain);
    if (hl == dl) return strcmp(host, domain) == 0;
    if (hl > dl) return host[hl - dl - 1] == '.' &&
                        strcmp(host + hl - dl, domain) == 0;
    return false;
}

/* 返回该 host 下应保留的参数名；NULL 表示删除全部参数 */
static const char *whitelist_param_for_host(const char *host)
{
    if (host_matches(host, "bilibili.com")) return "p";
    if (host_matches(host, "youtube.com"))  return "v";
    return NULL;
}

/* ------------------------------------------------------------------ */
/* 主流程                                                              */
/* ------------------------------------------------------------------ */

/* 取 URL 的 scheme（小写，新分配）；无 scheme 返回 NULL */
static char *extract_scheme(const char *url)
{
    if (!url) return NULL;
    const char *p = url;
    if (!isalpha((unsigned char)*p)) return NULL;
    while (*p && (isalnum((unsigned char)*p) || *p == '+' || *p == '-' || *p == '.')) p++;
    if (*p != ':') return NULL;
    char *s = bmk_strndup(url, (size_t)(p - url));
    if (!s) return NULL;
    for (char *q = s; *q; q++) *q = (char)tolower((unsigned char)*q);
    return s;
}

/* 非 http(s)：去掉 fragment 后的原文 */
static bmk_err normalize_other_scheme(const char *url, char **out_key)
{
    const char *hash = strchr(url, '#');
    char *key = hash ? bmk_strndup(url, (size_t)(hash - url)) : bmk_strdup(url);
    if (!key) return BMK_ERR_NOMEM;
    *out_key = key;
    return BMK_OK;
}

bmk_err bmk_url_normalize(const char *url, char **out_key)
{
    if (!url || !out_key) return BMK_ERR_INVALID_ARG;
    *out_key = NULL;

    char *scheme = extract_scheme(url);
    bool is_http = scheme && (strcmp(scheme, "http") == 0 || strcmp(scheme, "https") == 0);
    if (!is_http) {
        bmk_err e = normalize_other_scheme(url, out_key);
        free(scheme);
        return e;
    }
    free(scheme);

    CURLU *h = curl_url();
    if (!h) return BMK_ERR_NOMEM;
    if (curl_url_set(h, CURLUPART_URL, url, 0) != CURLUE_OK) {
        curl_url_cleanup(h);
        return BMK_ERR_INVALID_URL;
    }

    bmk_err rc = BMK_ERR_NOMEM;
    char *c_scheme = NULL, *c_host = NULL, *c_port = NULL,
         *c_path = NULL, *c_query = NULL;
    char *host = NULL, *path_dec = NULL, *path_norm = NULL;
    qparam *params = NULL;
    size_t nparams = 0;
    bmk_buf out;
    bmk_buf_init(&out);

    curl_url_get(h, CURLUPART_SCHEME, &c_scheme, 0);
    if (curl_url_get(h, CURLUPART_HOST, &c_host, CURLU_PUNYCODE) != CURLUE_OK || !c_host) {
        /* 无 punycode 支持时退回普通 host */
        curl_url_get(h, CURLUPART_HOST, &c_host, 0);
    }
    curl_url_get(h, CURLUPART_PORT, &c_port, 0);
    curl_url_get(h, CURLUPART_PATH, &c_path, 0);
    curl_url_get(h, CURLUPART_QUERY, &c_query, 0);

    if (!c_scheme || !c_host || !*c_host) { rc = BMK_ERR_INVALID_URL; goto done; }

    /* 2. scheme 小写；附录 B 用例 2：http 与 https 归一为同一 key
     *    （用例 2 期望 http://example.com/abc -> https://example.com/abc） */
    for (char *q = c_scheme; *q; q++) *q = (char)tolower((unsigned char)*q);

    /* 3. host 小写 + 剥离 www. */
    host = bmk_strdup(c_host);
    if (!host) goto done;
    for (char *q = host; *q; q++) *q = (char)tolower((unsigned char)*q);
    if (strncmp(host, "www.", 4) == 0) {
        memmove(host, host + 4, strlen(host + 4) + 1);
    }
    if (!*host) { rc = BMK_ERR_INVALID_URL; goto done; }

    /* 6. path：百分号解码一次 → 点段归一 → 去尾斜杠 */
    {
        const char *raw_path = (c_path && *c_path) ? c_path : "/";
        path_dec = bmk_url_percent_decode(raw_path);
        if (!path_dec) goto done;
        path_norm = bmk_url_remove_dot_segments(path_dec);
        if (!path_norm) goto done;
        size_t pl = strlen(path_norm);
        if (pl > 1 && path_norm[pl - 1] == '/') path_norm[pl - 1] = '\0';
        if (!*path_norm) { path_norm[0] = '/'; path_norm[1] = '\0'; }
    }

    /* 7~9. query 拆分、解码、白名单过滤、排序 */
    const char *keep = whitelist_param_for_host(host);
    if (keep && c_query && *c_query) {
        size_t cap = 8;
        params = (qparam *)calloc(cap, sizeof(qparam));
        if (!params) goto done;

        const char *p = c_query;
        while (*p) {
            const char *amp = strchr(p, '&');
            size_t itemlen = amp ? (size_t)(amp - p) : strlen(p);
            if (itemlen > 0) {
                char *item = bmk_strndup(p, itemlen);
                if (!item) goto done;
                char *eq = strchr(item, '=');
                char *rawname, *rawval = NULL;
                if (eq) {
                    *eq = '\0';
                    rawname = item;
                    rawval = eq + 1;
                } else {
                    rawname = item;
                }
                char *name = bmk_url_percent_decode(rawname);
                char *val = rawval ? bmk_url_percent_decode(rawval) : NULL;
                free(item);
                if (!name) { free(val); goto done; }

                if (strcmp(name, keep) == 0) {
                    if (nparams == cap) {
                        cap *= 2;
                        qparam *np = (qparam *)realloc(params, cap * sizeof(qparam));
                        if (!np) { free(name); free(val); goto done; }
                        params = np;
                    }
                    params[nparams].name = name;
                    params[nparams].value = val;
                    nparams++;
                } else {
                    free(name);
                    free(val);
                }
            }
            if (!amp) break;
            p = amp + 1;
        }
        if (nparams > 1) qsort(params, nparams, sizeof(qparam), qparam_cmp);
    }

    /* 10. 拼接。附录 B 用例 2 要求 http 归一到 https。 */
    if (!bmk_buf_append_str(&out, "https")) goto done;
    if (!bmk_buf_append_str(&out, "://")) goto done;
    if (!bmk_buf_append_str(&out, host)) goto done;
    /* 非默认端口追加到 key（Bug#1：端口冲突检测）。
     * 按原始 scheme 判定默认端口：http=80、https=443；二者都会被去掉。 */
    if (c_port && *c_port) {
        int p = atoi(c_port);
        bool is_default_port = (strcmp(c_scheme, "http") == 0 && p == 80) ||
                               (strcmp(c_scheme, "https") == 0 && p == 443);
        if (p > 0 && !is_default_port) {
            if (!bmk_buf_append_char(&out, ':')) goto done;
            if (!bmk_buf_append_str(&out, c_port)) goto done;
        }
    }
    if (!bmk_buf_append_str(&out, path_norm)) goto done;
    for (size_t i = 0; i < nparams; i++) {
        if (!bmk_buf_append_char(&out, i == 0 ? '?' : '&')) goto done;
        if (!bmk_buf_append_str(&out, params[i].name)) goto done;
        if (params[i].value) {
            if (!bmk_buf_append_char(&out, '=')) goto done;
            if (!bmk_buf_append_str(&out, params[i].value)) goto done;
        }
    }

    *out_key = bmk_buf_steal(&out);
    rc = BMK_OK;

done:
    bmk_buf_free(&out);
    for (size_t i = 0; i < nparams; i++) {
        free(params[i].name);
        free(params[i].value);
    }
    free(params);
    free(host);
    free(path_dec);
    free(path_norm);
    curl_free(c_scheme);
    curl_free(c_host);
    curl_free(c_port);
    curl_free(c_path);
    curl_free(c_query);
    curl_url_cleanup(h);
    return rc;
}

/* ------------------------------------------------------------------ */

bool bmk_url_is_valid_input(const char *url, const char **reason)
{
    const char *dummy = NULL;
    if (!reason) reason = &dummy;
    *reason = NULL;

    if (!url || !*url) { *reason = "url_empty"; return false; }

    /* 非法字符：空格与控制字符（FR-02 第 2 条） */
    for (const unsigned char *p = (const unsigned char *)url; *p; p++) {
        if (*p == ' ' || *p < 0x20 || *p == 0x7F) {
            *reason = "url_has_illegal_char";
            return false;
        }
    }

    char *scheme = extract_scheme(url);
    if (!scheme) { *reason = "url_scheme_missing"; return false; }
    bool ok_scheme = (strcmp(scheme, "http") == 0 || strcmp(scheme, "https") == 0);
    free(scheme);
    if (!ok_scheme) { *reason = "url_scheme_unsupported"; return false; }

    CURLU *h = curl_url();
    if (!h) { *reason = "nomem"; return false; }
    bool ok = false;
    if (curl_url_set(h, CURLUPART_URL, url, 0) == CURLUE_OK) {
        char *host = NULL;
        if (curl_url_get(h, CURLUPART_HOST, &host, 0) == CURLUE_OK && host && *host) {
            ok = true;
            curl_free(host);
        } else {
            *reason = "url_host_empty";
            if (host) curl_free(host);
        }
    } else {
        *reason = "url_parse_failed";
    }
    curl_url_cleanup(h);
    return ok;
}

char *bmk_url_host(const char *url)
{
    if (!url) return NULL;
    CURLU *h = curl_url();
    if (!h) return NULL;
    char *result = NULL;
    if (curl_url_set(h, CURLUPART_URL, url, 0) == CURLUE_OK) {
        char *host = NULL;
        if (curl_url_get(h, CURLUPART_HOST, &host, CURLU_PUNYCODE) != CURLUE_OK || !host)
            curl_url_get(h, CURLUPART_HOST, &host, 0);
        if (host) {
            result = bmk_strdup(host);
            curl_free(host);
            if (result) {
                for (char *q = result; *q; q++) *q = (char)tolower((unsigned char)*q);
                if (strncmp(result, "www.", 4) == 0)
                    memmove(result, result + 4, strlen(result + 4) + 1);
            }
        }
    }
    curl_url_cleanup(h);
    return result;
}

char *bmk_url_resolve(const char *base, const char *relative)
{
    if (!relative) return NULL;
    if (!base) return bmk_strdup(relative);

    CURLU *h = curl_url();
    if (!h) return NULL;
    char *result = NULL;
    if (curl_url_set(h, CURLUPART_URL, base, 0) == CURLUE_OK &&
        curl_url_set(h, CURLUPART_URL, relative, 0) == CURLUE_OK) {
        char *full = NULL;
        if (curl_url_get(h, CURLUPART_URL, &full, 0) == CURLUE_OK && full) {
            result = bmk_strdup(full);
            curl_free(full);
        }
    }
    curl_url_cleanup(h);
    return result;
}
