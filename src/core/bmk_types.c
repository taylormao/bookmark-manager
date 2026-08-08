#include "bmk_types.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
#  include <direct.h>
#  define BMK_MKDIR(p) _mkdir(p)
#  define BMK_PATH_SEP '\\'
#else
#  include <unistd.h>
#  define BMK_MKDIR(p) mkdir((p), 0700)
#  define BMK_PATH_SEP '/'
#endif

const char *bmk_err_str(bmk_err e)
{
    switch (e) {
    case BMK_OK:               return "ok";
    case BMK_ERR_INVALID_ARG:  return "invalid_arg";
    case BMK_ERR_NOMEM:        return "nomem";
    case BMK_ERR_IO:           return "io_error";
    case BMK_ERR_DB:           return "db_error";
    case BMK_ERR_DB_CORRUPT:   return "db_corrupt";
    case BMK_ERR_NOT_FOUND:    return "not_found";
    case BMK_ERR_CONFLICT:     return "conflict";
    case BMK_ERR_INVALID_URL:  return "invalid_url";
    case BMK_ERR_PARSE:        return "parse_error";
    case BMK_ERR_NETWORK:      return "network_error";
    case BMK_ERR_CANCELED:     return "canceled";
    case BMK_ERR_SUBPROCESS:   return "subprocess_error";
    case BMK_ERR_AUTH:         return "auth_error";
    case BMK_ERR_UNSUPPORTED:  return "unsupported";
    case BMK_ERR_PERMISSION:   return "permission_denied";
    }
    return "unknown";
}

/* ---------------- bmk_buf ---------------- */

void bmk_buf_init(bmk_buf *b)
{
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void bmk_buf_free(bmk_buf *b)
{
    if (!b) return;
    free(b->data);
    bmk_buf_init(b);
}

void bmk_buf_clear(bmk_buf *b)
{
    b->len = 0;
    if (b->data) b->data[0] = '\0';
}

bool bmk_buf_reserve(bmk_buf *b, size_t need)
{
    /* need = 期望可容纳的字符数（不含结尾 '\0'） */
    if (b->cap > need) return true;
    size_t cap = b->cap ? b->cap : 64;
    while (cap <= need) {
        if (cap > (size_t)-1 / 2) return false;
        cap *= 2;
    }
    char *p = (char *)realloc(b->data, cap);
    if (!p) return false;
    b->data = p;
    b->cap = cap;
    if (b->len == 0) b->data[0] = '\0';
    return true;
}

bool bmk_buf_append(bmk_buf *b, const char *data, size_t n)
{
    if (n == 0) {
        /* 仍需保证 data 非空且以 '\0' 结尾 */
        if (!b->data && !bmk_buf_reserve(b, 0)) return false;
        return true;
    }
    if (!bmk_buf_reserve(b, b->len + n)) return false;
    memcpy(b->data + b->len, data, n);
    b->len += n;
    b->data[b->len] = '\0';
    return true;
}

bool bmk_buf_append_str(bmk_buf *b, const char *s)
{
    if (!s) return true;
    return bmk_buf_append(b, s, strlen(s));
}

bool bmk_buf_append_char(bmk_buf *b, char c)
{
    return bmk_buf_append(b, &c, 1);
}

bool bmk_buf_printf(bmk_buf *b, const char *fmt, ...)
{
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) { va_end(ap2); return false; }
    if (!bmk_buf_reserve(b, b->len + (size_t)n)) { va_end(ap2); return false; }
    vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    b->len += (size_t)n;
    return true;
}

char *bmk_buf_steal(bmk_buf *b)
{
    char *p = b->data;
    if (!p) p = bmk_strdup("");
    bmk_buf_init(b);
    return p;
}

/* ---------------- 字符串 ---------------- */

char *bmk_strdup(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s);
    char *p = (char *)malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n + 1);
    return p;
}

char *bmk_strndup(const char *s, size_t n)
{
    if (!s) return NULL;
    size_t l = 0;
    while (l < n && s[l]) l++;
    char *p = (char *)malloc(l + 1);
    if (!p) return NULL;
    memcpy(p, s, l);
    p[l] = '\0';
    return p;
}

int bmk_strcasecmp(const char *a, const char *b)
{
    if (a == b) return 0;
    if (!a) return -1;
    if (!b) return 1;
    while (*a && *b) {
        int ca = tolower((unsigned char)*a);
        int cb = tolower((unsigned char)*b);
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

char *bmk_str_trim(char *s)
{
    if (!s) return NULL;
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
    return s;
}

char *bmk_str_collapse_ws(char *s)
{
    if (!s) return NULL;
    char *w = s;
    const char *r = s;
    bool in_ws = false;
    /* 跳过前导空白 */
    while (*r && isspace((unsigned char)*r)) r++;
    for (; *r; r++) {
        if (isspace((unsigned char)*r)) {
            in_ws = true;
            continue;
        }
        if (in_ws) { *w++ = ' '; in_ws = false; }
        *w++ = *r;
    }
    *w = '\0';
    return s;
}

const char *bmk_str_casestr(const char *hay, const char *needle)
{
    if (!hay || !needle) return NULL;
    if (!*needle) return hay;
    size_t nl = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < nl && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i]))
            i++;
        if (i == nl) return p;
    }
    return NULL;
}

void bmk_str_truncate_utf8(char *s, size_t max_chars)
{
    if (!s) return;
    size_t chars = 0;
    size_t i = 0;
    while (s[i]) {
        unsigned char c = (unsigned char)s[i];
        size_t adv = 1;
        if      (c >= 0xF0) adv = 4;
        else if (c >= 0xE0) adv = 3;
        else if (c >= 0xC0) adv = 2;
        /* 防止越过结尾 */
        for (size_t k = 1; k < adv; k++) {
            if (!s[i + k]) { adv = k; break; }
        }
        if (chars == max_chars) { s[i] = '\0'; return; }
        chars++;
        i += adv;
    }
}

/* ---------------- 时间 ---------------- */

int64_t bmk_now(void)
{
    return (int64_t)time(NULL);
}

void bmk_timestamp_str(int64_t ts, char *out, size_t outsz)
{
    time_t t = (time_t)ts;
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    strftime(out, outsz, "%Y%m%d_%H%M%S", &tmv);
}

void bmk_iso8601(int64_t ts, char *out, size_t outsz)
{
    time_t t = (time_t)ts;
    struct tm tmv;
#ifdef _WIN32
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    strftime(out, outsz, "%Y-%m-%dT%H:%M:%SZ", &tmv);
}

/* ---------------- 路径与文件 ---------------- */

char *bmk_path_join(const char *a, const char *b)
{
    if (!a || !*a) return bmk_strdup(b);
    if (!b || !*b) return bmk_strdup(a);
    size_t la = strlen(a);
    bool has_sep = (a[la - 1] == '/' || a[la - 1] == '\\');
    size_t need = la + (has_sep ? 0 : 1) + strlen(b) + 1;
    char *p = (char *)malloc(need);
    if (!p) return NULL;
    if (has_sep) snprintf(p, need, "%s%s", a, b);
    else         snprintf(p, need, "%s%c%s", a, BMK_PATH_SEP, b);
    return p;
}

bool bmk_file_exists(const char *path)
{
    if (!path) return false;
    struct stat st;
    return stat(path, &st) == 0;
}

bmk_err bmk_mkdirs(const char *path)
{
    if (!path || !*path) return BMK_ERR_INVALID_ARG;
    char *tmp = bmk_strdup(path);
    if (!tmp) return BMK_ERR_NOMEM;
    size_t n = strlen(tmp);
    /* 去掉末尾分隔符 */
    while (n > 1 && (tmp[n - 1] == '/' || tmp[n - 1] == '\\')) tmp[--n] = '\0';

    for (size_t i = 1; i < n; i++) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            char save = tmp[i];
            tmp[i] = '\0';
            /* 跳过 Windows 盘符根，如 "C:" */
            if (!(i == 2 && tmp[1] == ':')) {
                struct stat st;
                if (stat(tmp, &st) != 0 && BMK_MKDIR(tmp) != 0) {
                    /* 并发创建时可能已存在，再判断一次 */
                    if (stat(tmp, &st) != 0) { free(tmp); return BMK_ERR_IO; }
                }
            }
            tmp[i] = save;
        }
    }
    struct stat st;
    if (stat(tmp, &st) != 0 && BMK_MKDIR(tmp) != 0) {
        if (stat(tmp, &st) != 0) { free(tmp); return BMK_ERR_IO; }
    }
    free(tmp);
    return BMK_OK;
}

bmk_err bmk_read_file(const char *path, char **out, size_t *out_len)
{
    if (!path || !out) return BMK_ERR_INVALID_ARG;
    *out = NULL;
    if (out_len) *out_len = 0;

    FILE *f = fopen(path, "rb");
    if (!f) return BMK_ERR_IO;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return BMK_ERR_IO; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return BMK_ERR_IO; }
    rewind(f);

    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return BMK_ERR_NOMEM; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = '\0';
    *out = buf;
    if (out_len) *out_len = got;
    return BMK_OK;
}

bmk_err bmk_write_file(const char *path, const void *data, size_t len)
{
    if (!path) return BMK_ERR_INVALID_ARG;
    FILE *f = fopen(path, "wb");
    if (!f) return BMK_ERR_IO;
    if (len > 0 && fwrite(data, 1, len, f) != len) {
        fclose(f);
        return BMK_ERR_IO;
    }
    if (fclose(f) != 0) return BMK_ERR_IO;
    return BMK_OK;
}

/* ---------------- Base64 ---------------- */

static const char B64_ALPHA[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

char *bmk_base64_encode(const unsigned char *data, size_t len)
{
    if (!data && len) return NULL;
    size_t olen = 4 * ((len + 2) / 3);
    char *out = (char *)malloc(olen + 1);
    if (!out) return NULL;

    size_t i = 0, o = 0;
    while (i + 2 < len) {
        uint32_t v = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8) | data[i + 2];
        out[o++] = B64_ALPHA[(v >> 18) & 0x3F];
        out[o++] = B64_ALPHA[(v >> 12) & 0x3F];
        out[o++] = B64_ALPHA[(v >> 6) & 0x3F];
        out[o++] = B64_ALPHA[v & 0x3F];
        i += 3;
    }
    if (i < len) {
        uint32_t v = (uint32_t)data[i] << 16;
        bool two = (i + 1 < len);
        if (two) v |= (uint32_t)data[i + 1] << 8;
        out[o++] = B64_ALPHA[(v >> 18) & 0x3F];
        out[o++] = B64_ALPHA[(v >> 12) & 0x3F];
        out[o++] = two ? B64_ALPHA[(v >> 6) & 0x3F] : '=';
        out[o++] = '=';
    }
    out[o] = '\0';
    return out;
}

static int b64_val(unsigned char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

unsigned char *bmk_base64_decode(const char *text, size_t *out_len)
{
    if (!text) return NULL;
    size_t tlen = strlen(text);
    unsigned char *out = (unsigned char *)malloc(tlen / 4 * 3 + 4);
    if (!out) return NULL;

    uint32_t acc = 0;
    int nbits = 0;
    size_t o = 0;
    for (size_t i = 0; i < tlen; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == '=' ) break;
        if (isspace(c)) continue;
        int v = b64_val(c);
        if (v < 0) { free(out); return NULL; }
        acc = (acc << 6) | (uint32_t)v;
        nbits += 6;
        if (nbits >= 8) {
            nbits -= 8;
            out[o++] = (unsigned char)((acc >> nbits) & 0xFF);
        }
    }
    if (out_len) *out_len = o;
    return out;
}
