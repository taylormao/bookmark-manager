/*
 * bmk_netscape.c - Netscape 书签导入导出实现（附录 A）
 *
 * 解析策略：Netscape 书签是半结构化的 HTML（多数导出不闭合 <DT>/<p>），
 * 通用 HTML DOM 解析反而难以还原层级。这里采用面向该格式的流式扫描：
 *   <DL>  → 入栈一层分类
 *   </DL> → 出栈
 *   <H3>  → 在当前层创建/复用分类，成为后续 <DL> 的父级
 *   <A>   → 在当前层创建书签
 * 该策略与 Chrome/Firefox/Edge 的导出格式完全兼容。
 */
#include "bmk_netscape.h"
#include "bmk_log.h"
#include "bmk_url.h"
#include "bmk_settings.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <errno.h>
#  include <iconv.h>
#endif

#define MAX_DEPTH 64          /* FR-10 要求至少支持 32 层，留一倍余量 */
#define MAX_ICON_BYTES (512 * 1024)

/* ------------------------------------------------------------------ */
/* 编码                                                                */
/* ------------------------------------------------------------------ */

bool bmk_is_valid_utf8(const char *buf, size_t len)
{
    const unsigned char *p = (const unsigned char *)buf;
    size_t i = 0;
    while (i < len) {
        unsigned char c = p[i];
        size_t extra;
        if (c < 0x80)              { i++; continue; }
        else if ((c & 0xE0) == 0xC0) { extra = 1; if (c < 0xC2) return false; }
        else if ((c & 0xF0) == 0xE0) { extra = 2; }
        else if ((c & 0xF8) == 0xF0) { extra = 3; if (c > 0xF4) return false; }
        else return false;

        if (i + extra >= len) return false;
        for (size_t k = 1; k <= extra; k++) {
            if (i + k >= len) return false;
            if ((p[i + k] & 0xC0) != 0x80) return false;
        }
        i += extra + 1;
    }
    return true;
}

char *bmk_gbk_to_utf8(const char *gbk, size_t len)
{
#ifdef _WIN32
    /* Windows 用系统 API（936 = GBK） */
    int wlen = MultiByteToWideChar(936, 0, gbk, (int)len, NULL, 0);
    if (wlen <= 0) return NULL;
    wchar_t *wbuf = (wchar_t *)malloc((size_t)wlen * sizeof(wchar_t));
    if (!wbuf) return NULL;
    MultiByteToWideChar(936, 0, gbk, (int)len, wbuf, wlen);

    int ulen = WideCharToMultiByte(CP_UTF8, 0, wbuf, wlen, NULL, 0, NULL, NULL);
    if (ulen <= 0) { free(wbuf); return NULL; }
    char *out = (char *)malloc((size_t)ulen + 1);
    if (!out) { free(wbuf); return NULL; }
    WideCharToMultiByte(CP_UTF8, 0, wbuf, wlen, out, ulen, NULL, NULL);
    out[ulen] = '\0';
    free(wbuf);
    return out;
#else
    /* Linux 用 iconv */
    iconv_t cd = iconv_open("UTF-8", "GBK");
    if (cd == (iconv_t)-1) return NULL;

    size_t inleft = len;
    char *inbuf = (char *)gbk;
    size_t outcap = len * 4 + 4;
    char *out = (char *)malloc(outcap);
    if (!out) { iconv_close(cd); return NULL; }
    char *outbuf = out;
    size_t outleft = outcap - 1;

    while (inleft > 0) {
        size_t r = iconv(cd, &inbuf, &inleft, &outbuf, &outleft);
        if (r == (size_t)-1) {
            if (errno == EILSEQ || errno == EINVAL) {
                /* 跳过非法字节，尽量保留其余内容 */
                inbuf++; inleft--;
                if (outleft > 0) { *outbuf++ = '?'; outleft--; }
                continue;
            }
            free(out);
            iconv_close(cd);
            return NULL;
        }
    }
    *outbuf = '\0';
    iconv_close(cd);
    return out;
#endif
}

/* ------------------------------------------------------------------ */
/* HTML 实体                                                           */
/* ------------------------------------------------------------------ */

char *bmk_html_escape(const char *s)
{
    if (!s) return bmk_strdup("");
    bmk_buf b;
    bmk_buf_init(&b);
    for (const char *p = s; *p; p++) {
        switch (*p) {
        case '&':  bmk_buf_append_str(&b, "&amp;");  break;
        case '<':  bmk_buf_append_str(&b, "&lt;");   break;
        case '>':  bmk_buf_append_str(&b, "&gt;");   break;
        case '"':  bmk_buf_append_str(&b, "&quot;"); break;
        case '\'': bmk_buf_append_str(&b, "&#39;");  break;
        default:   bmk_buf_append_char(&b, *p);      break;
        }
    }
    return bmk_buf_steal(&b);
}

/* 将 Unicode 码点以 UTF-8 追加 */
static void append_utf8(bmk_buf *b, unsigned long cp)
{
    if (cp < 0x80) {
        bmk_buf_append_char(b, (char)cp);
    } else if (cp < 0x800) {
        bmk_buf_append_char(b, (char)(0xC0 | (cp >> 6)));
        bmk_buf_append_char(b, (char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        bmk_buf_append_char(b, (char)(0xE0 | (cp >> 12)));
        bmk_buf_append_char(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
        bmk_buf_append_char(b, (char)(0x80 | (cp & 0x3F)));
    } else {
        bmk_buf_append_char(b, (char)(0xF0 | (cp >> 18)));
        bmk_buf_append_char(b, (char)(0x80 | ((cp >> 12) & 0x3F)));
        bmk_buf_append_char(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
        bmk_buf_append_char(b, (char)(0x80 | (cp & 0x3F)));
    }
}

char *bmk_html_unescape(const char *s)
{
    if (!s) return bmk_strdup("");
    bmk_buf b;
    bmk_buf_init(&b);
    for (const char *p = s; *p; ) {
        if (*p != '&') { bmk_buf_append_char(&b, *p++); continue; }

        const char *semi = strchr(p, ';');
        if (!semi || semi - p > 10) { bmk_buf_append_char(&b, *p++); continue; }

        size_t elen = (size_t)(semi - p - 1);
        char ent[16];
        memcpy(ent, p + 1, elen);
        ent[elen] = '\0';

        if      (strcmp(ent, "amp") == 0)  bmk_buf_append_char(&b, '&');
        else if (strcmp(ent, "lt") == 0)   bmk_buf_append_char(&b, '<');
        else if (strcmp(ent, "gt") == 0)   bmk_buf_append_char(&b, '>');
        else if (strcmp(ent, "quot") == 0) bmk_buf_append_char(&b, '"');
        else if (strcmp(ent, "apos") == 0) bmk_buf_append_char(&b, '\'');
        else if (strcmp(ent, "nbsp") == 0) bmk_buf_append_char(&b, ' ');
        else if (ent[0] == '#') {
            unsigned long cp = (ent[1] == 'x' || ent[1] == 'X')
                               ? strtoul(ent + 2, NULL, 16)
                               : strtoul(ent + 1, NULL, 10);
            if (cp > 0) append_utf8(&b, cp);
        } else {
            /* 未知实体原样保留 */
            bmk_buf_append_char(&b, '&');
            bmk_buf_append_str(&b, ent);
            bmk_buf_append_char(&b, ';');
        }
        p = semi + 1;
    }
    return bmk_buf_steal(&b);
}

/* ------------------------------------------------------------------ */
/* 报告                                                                */
/* ------------------------------------------------------------------ */

void bmk_import_report_free(bmk_import_report *r)
{
    if (!r) return;
    for (size_t i = 0; i < r->skipped_count; i++) free(r->skipped_titles[i]);
    free(r->skipped_titles);
    for (size_t i = 0; i < r->failed_count; i++) free(r->failed_reasons[i]);
    free(r->failed_reasons);
    memset(r, 0, sizeof(*r));
}

static void report_add(char ***arr, size_t *n, const char *text)
{
    char **na = (char **)realloc(*arr, (*n + 1) * sizeof(char *));
    if (!na) return;
    *arr = na;
    (*arr)[*n] = bmk_strdup(text);
    (*n)++;
}

void bmk_import_opts_init(bmk_import_opts *o)
{
    memset(o, 0, sizeof(*o));
    o->encoding = BMK_ENC_AUTO;
    o->dup_policy = BMK_DUP_SKIP;   /* 设置默认“跳过”（FR-07 第 3 条） */
}

/* ------------------------------------------------------------------ */
/* 标签属性解析                                                        */
/* ------------------------------------------------------------------ */

/*
 * 从形如 `A HREF="x" ADD_DATE="1"` 的标签内部文本中取出属性值。
 * 属性名不区分大小写，值可用单/双引号或无引号。
 * 返回新分配字符串（已 HTML 反转义）；未找到返回 NULL。
 */
static char *tag_attr(const char *tag, const char *name)
{
    size_t nlen = strlen(name);
    const char *p = tag;
    while (*p) {
        /* 定位候选属性名 */
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;

        const char *nstart = p;
        while (*p && *p != '=' && !isspace((unsigned char)*p) && *p != '>') p++;
        size_t curlen = (size_t)(p - nstart);

        while (*p && isspace((unsigned char)*p)) p++;
        if (*p != '=') {
            /* 无值属性，继续 */
            if (!*p) break;
            continue;
        }
        p++; /* 跳过 '=' */
        while (*p && isspace((unsigned char)*p)) p++;

        const char *vstart;
        size_t vlen;
        if (*p == '"' || *p == '\'') {
            char q = *p++;
            vstart = p;
            while (*p && *p != q) p++;
            vlen = (size_t)(p - vstart);
            if (*p) p++;
        } else {
            vstart = p;
            while (*p && !isspace((unsigned char)*p) && *p != '>') p++;
            vlen = (size_t)(p - vstart);
        }

        if (curlen == nlen) {
            bool match = true;
            for (size_t i = 0; i < nlen; i++) {
                if (tolower((unsigned char)nstart[i]) != tolower((unsigned char)name[i])) {
                    match = false;
                    break;
                }
            }
            if (match) {
                char *raw = bmk_strndup(vstart, vlen);
                if (!raw) return NULL;
                char *dec = bmk_html_unescape(raw);
                free(raw);
                return dec;
            }
        }
    }
    return NULL;
}

/* 判断标签名（tag 指向 '<' 之后的内容） */
static bool tag_is(const char *tag, const char *name)
{
    size_t n = strlen(name);
    for (size_t i = 0; i < n; i++) {
        if (tolower((unsigned char)tag[i]) != tolower((unsigned char)name[i])) return false;
    }
    char after = tag[n];
    return after == '\0' || isspace((unsigned char)after) || after == '>' || after == '/';
}

/* ------------------------------------------------------------------ */
/* 导入                                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    bmk_db *db;
    const bmk_import_opts *opts;
    bmk_import_report *report;
    int64_t stack[MAX_DEPTH];   /* 分类 id 栈；0 = 根/未分类 */
    int     depth;
    int64_t pending_category;   /* 最近 <H3> 创建的分类，等待其 <DL> */
    bool    canceled;
} import_ctx;

/* 解析 ICON="data:image/png;base64,xxx" */
static bool parse_data_uri_icon(const char *icon_attr,
                                unsigned char **out, size_t *out_len, char **out_mime)
{
    *out = NULL; *out_len = 0; *out_mime = NULL;
    if (!icon_attr) return false;
    if (strncmp(icon_attr, "data:", 5) != 0) return false;

    const char *comma = strchr(icon_attr, ',');
    if (!comma) return false;
    const char *meta = icon_attr + 5;
    /* meta 形如 image/png;base64 */
    const char *semi = strchr(meta, ';');
    size_t mlen = semi ? (size_t)(semi - meta) : (size_t)(comma - meta);
    if (mlen == 0) return false;
    if (!semi || strncmp(semi + 1, "base64", 6) != 0) return false;

    size_t dlen = 0;
    unsigned char *data = bmk_base64_decode(comma + 1, &dlen);
    if (!data) return false;
    if (dlen == 0 || dlen > MAX_ICON_BYTES) { free(data); return false; }

    *out = data;
    *out_len = dlen;
    *out_mime = bmk_strndup(meta, mlen);
    return true;
}

static void import_handle_anchor(import_ctx *ctx, const char *tagbody, const char *text)
{
    bmk_import_report *rep = ctx->report;

    char *href = tag_attr(tagbody, "HREF");
    if (!href || !*href) {
        free(href);
        if (rep) {
            rep->failed++;
            report_add(&rep->failed_reasons, &rep->failed_count, "missing HREF");
        }
        return;
    }

    /* url_key：非 http(s) 也照常导入（FR-07 第 2 条第 3 点） */
    char *url_key = NULL;
    if (bmk_url_normalize(href, &url_key) != BMK_OK || !url_key) {
        if (rep) {
            rep->failed++;
            char msg[512];
            snprintf(msg, sizeof(msg), "%s - invalid url", href);
            report_add(&rep->failed_reasons, &rep->failed_count, msg);
        }
        free(href);
        return;
    }

    /* 重复处理（FR-07 第 3 条） */
    if (ctx->opts->dup_policy == BMK_DUP_SKIP) {
        if (bmk_bookmark_count_by_key(ctx->db, url_key, NULL) > 0) {
            if (rep) {
                rep->skipped++;
                char msg[768];
                snprintf(msg, sizeof(msg), "%s - %s",
                         (text && *text) ? text : "(no title)", href);
                report_add(&rep->skipped_titles, &rep->skipped_count, msg);
            }
            free(href);
            free(url_key);
            return;
        }
    }

    bmk_bookmark b;
    memset(&b, 0, sizeof(b));
    b.url = href;
    b.url_key = url_key;
    b.valid = BMK_VALID_UNKNOWN;
    b.status_code = -1;

    /* 标题：<A> 文本；为空时用 host 兜底 */
    char *title = NULL;
    if (text && *text) {
        title = bmk_strdup(text);
    } else {
        char *host = bmk_url_host(href);
        title = host ? host : bmk_strdup(href);
    }
    bmk_str_collapse_ws(title);
    b.title = title;

    /* ADD_DATE → created_at，缺失用导入时间 */
    char *add_date = tag_attr(tagbody, "ADD_DATE");
    b.created_at = (add_date && *add_date) ? (int64_t)strtoll(add_date, NULL, 10) : bmk_now();
    if (b.created_at <= 0) b.created_at = bmk_now();
    free(add_date);

    char *last_mod = tag_attr(tagbody, "LAST_MODIFIED");
    free(last_mod);  /* insert 内部按当前时间刷新 updated_at */

    /* ICON：仅 data URI 解码入库，URL 形式不下载（FR-07 第 2 条） */
    char *icon_attr = tag_attr(tagbody, "ICON");
    if (icon_attr) {
        unsigned char *idata = NULL;
        size_t ilen = 0;
        char *imime = NULL;
        if (parse_data_uri_icon(icon_attr, &idata, &ilen, &imime)) {
            b.icon = idata;
            b.icon_len = ilen;
            b.icon_mime = imime;
        }
        free(icon_attr);
    }

    /* TAGS（Firefox）按逗号拆分 */
    char *tags_attr = tag_attr(tagbody, "TAGS");
    char **tags = NULL;
    size_t ntags = 0;
    if (tags_attr && *tags_attr) {
        char *save = tags_attr;
        char *tok = save;
        while (tok) {
            char *comma = strchr(tok, ',');
            if (comma) *comma = '\0';
            char *t = bmk_strdup(tok);
            if (t) {
                bmk_str_trim(t);
                /* Bug#5: 解码 %2C 为逗号（round-trip） */
                for (char *p = t; *p; p++) {
                    if (strncmp(p, "%2C", 3) == 0 || strncmp(p, "%2c", 3) == 0) {
                        *p = ',';
                        memmove(p + 1, p + 3, strlen(p + 3) + 1);
                    }
                }
                if (*t) {
                    char **nt = (char **)realloc(tags, (ntags + 1) * sizeof(char *));
                    if (nt) { tags = nt; tags[ntags++] = t; }
                    else free(t);
                } else {
                    free(t);
                }
            }
            tok = comma ? comma + 1 : NULL;
        }
    }
    free(tags_attr);
    b.tags = tags;
    b.tag_count = ntags;

    b.category_id = ctx->stack[ctx->depth];

    bmk_err e = bmk_bookmark_insert(ctx->db, &b);
    if (e == BMK_OK) {
        if (rep) rep->added++;
    } else if (rep) {
        rep->failed++;
        char msg[512];
        snprintf(msg, sizeof(msg), "%s - %s", href, bmk_err_str(e));
        report_add(&rep->failed_reasons, &rep->failed_count, msg);
    }

    /* 释放（b 的字段均为本函数分配） */
    free(b.title);
    free(b.url);
    free(b.url_key);
    free(b.icon);
    free(b.icon_mime);
    for (size_t i = 0; i < ntags; i++) free(tags[i]);
    free(tags);
}

bmk_err bmk_import_html_buffer(bmk_db *db, const char *buf, size_t len,
                               const bmk_import_opts *opts,
                               bmk_import_report *report)
{
    if (!db || !buf) return BMK_ERR_INVALID_ARG;

    bmk_import_opts defo;
    if (!opts) { bmk_import_opts_init(&defo); opts = &defo; }

    /* 校验是否为书签文件（FR-07 边界）：必须含 <DL */
    if (!bmk_str_casestr(buf, "<dl")) {
        BMK_WARN("import: no <DL> structure found");
        return BMK_ERR_PARSE;
    }

    import_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.db = db;
    ctx.opts = opts;
    ctx.report = report;
    ctx.depth = 0;
    ctx.stack[0] = opts->root_category_id;
    ctx.pending_category = 0;

    bmk_err e = bmk_db_begin(db);
    if (e != BMK_OK) return e;

    int64_t processed = 0;
    const char *p = buf;
    const char *end = buf + len;

    while (p < end && *p) {
        const char *lt = strchr(p, '<');
        if (!lt) break;
        const char *gt = strchr(lt, '>');
        if (!gt) break;

        char *tagbody = bmk_strndup(lt + 1, (size_t)(gt - lt - 1));
        if (!tagbody) { bmk_db_rollback(db); return BMK_ERR_NOMEM; }

        if (tag_is(tagbody, "DL")) {
            /* 进入一层：父级为最近的 <H3> 分类（若有） */
            if (ctx.depth + 1 < MAX_DEPTH) {
                ctx.depth++;
                ctx.stack[ctx.depth] = ctx.pending_category
                                       ? ctx.pending_category
                                       : ctx.stack[ctx.depth - 1];
            }
            ctx.pending_category = 0;
        } else if (tag_is(tagbody, "/DL")) {
            if (ctx.depth > 0) ctx.depth--;
            ctx.pending_category = 0;
        } else if (tag_is(tagbody, "H3")) {
            /* 取 <H3>...</H3> 之间的文本作为分类名 */
            const char *tclose = strstr(gt + 1, "</");
            const char *tend = tclose ? tclose : gt + 1;
            char *raw = bmk_strndup(gt + 1, (size_t)(tend - gt - 1));
            char *name = raw ? bmk_html_unescape(raw) : NULL;
            free(raw);
            if (name) {
                bmk_str_collapse_ws(name);
                if (*name) {
                    int64_t parent = ctx.stack[ctx.depth];
                    /* 同名同级复用（FR-07 第 2 条） */
                    int64_t cid = bmk_category_find(db, name, parent);
                    if (!cid) {
                        bmk_err ce = bmk_category_create(db, name, parent, &cid);
                        if (ce != BMK_OK && ce != BMK_ERR_CONFLICT) cid = 0;
                    }
                    ctx.pending_category = cid;
                }
                free(name);
            }
        } else if (tag_is(tagbody, "A")) {
            const char *tclose = strstr(gt + 1, "</");
            const char *tend = tclose ? tclose : gt + 1;
            char *raw = bmk_strndup(gt + 1, (size_t)(tend - gt - 1));
            char *text = raw ? bmk_html_unescape(raw) : NULL;
            free(raw);
            import_handle_anchor(&ctx, tagbody, text ? text : "");
            free(text);

            processed++;
            /* 每条 anchor 都检查进度回调，小文件也能及时取消（FR-07 第 4 条） */
            if (opts->progress) {
                if (!opts->progress(processed, -1, opts->progress_user)) {
                    free(tagbody);
                    bmk_db_rollback(db);   /* 取消 = 回滚（FR-07 第 4 条） */
                    return BMK_ERR_CANCELED;
                }
            }
        }

        free(tagbody);
        p = gt + 1;
    }

    e = bmk_db_commit(db);
    if (e != BMK_OK) {
        bmk_db_rollback(db);
        return e;
    }
    if (opts->progress) opts->progress(processed, processed, opts->progress_user);
    BMK_INFO("import finished: added=%lld skipped=%lld failed=%lld",
             report ? (long long)report->added : 0,
             report ? (long long)report->skipped : 0,
             report ? (long long)report->failed : 0);
    return BMK_OK;
}

bmk_err bmk_import_html_file(bmk_db *db, const char *path,
                             const bmk_import_opts *opts,
                             bmk_import_report *report)
{
    if (!db || !path) return BMK_ERR_INVALID_ARG;

    char *raw = NULL;
    size_t len = 0;
    bmk_err e = bmk_read_file(path, &raw, &len);
    if (e != BMK_OK) return e;

    bmk_import_opts defo;
    if (!opts) { bmk_import_opts_init(&defo); opts = &defo; }

    /* 编码识别（FR-07 第 1 条） */
    const char *content = raw;
    size_t clen = len;
    char *converted = NULL;

    /* UTF-8 BOM */
    if (len >= 3 && (unsigned char)raw[0] == 0xEF &&
        (unsigned char)raw[1] == 0xBB && (unsigned char)raw[2] == 0xBF) {
        content = raw + 3;
        clen = len - 3;
    } else if (opts->encoding == BMK_ENC_GBK) {
        converted = bmk_gbk_to_utf8(raw, len);
        if (converted) { content = converted; clen = strlen(converted); }
    } else if (opts->encoding == BMK_ENC_AUTO) {
        /* 无 BOM：先按 UTF-8 严格解码，失败按 GBK */
        if (!bmk_is_valid_utf8(raw, len)) {
            converted = bmk_gbk_to_utf8(raw, len);
            if (converted) { content = converted; clen = strlen(converted); }
        }
    }

    e = bmk_import_html_buffer(db, content, clen, opts, report);
    free(converted);
    free(raw);
    return e;
}

/* ------------------------------------------------------------------ */
/* 导出                                                                */
/* ------------------------------------------------------------------ */

static void export_indent(bmk_buf *b, int depth)
{
    for (int i = 0; i < depth; i++) bmk_buf_append_str(b, "    ");
}

/* 输出单条 <DT><A ...> */
static void export_anchor(bmk_buf *out, const bmk_bookmark *b, int depth)
{
    export_indent(out, depth);
    char *eurl = bmk_html_escape(b->url);
    char *etitle = bmk_html_escape(b->title);

    bmk_buf_printf(out, "<DT><A HREF=\"%s\" ADD_DATE=\"%lld\" LAST_MODIFIED=\"%lld\"",
                   eurl ? eurl : "",
                   (long long)b->created_at, (long long)b->updated_at);

    /* TAGS 逗号分隔（FR-08 第 2 条） */
    if (b->tag_count > 0) {
        bmk_buf tb;
        bmk_buf_init(&tb);
        for (size_t i = 0; i < b->tag_count; i++) {
            if (i) bmk_buf_append_char(&tb, ',');
            /* Bug#5: 将 tag 中的逗号编码为 %2C */
            const char *tp = b->tags[i];
            while (*tp) {
                if (*tp == ',') bmk_buf_append_str(&tb, "%2C");
                else bmk_buf_append_char(&tb, *tp);
                tp++;
            }
        }
        char *etags = bmk_html_escape(tb.data ? tb.data : "");
        bmk_buf_printf(out, " TAGS=\"%s\"", etags ? etags : "");
        free(etags);
        bmk_buf_free(&tb);
    }

    /* ICON 为 data URI（icon 存在时） */
    if (b->icon && b->icon_len > 0) {
        char *b64 = bmk_base64_encode(b->icon, b->icon_len);
        if (b64) {
            bmk_buf_printf(out, " ICON=\"data:%s;base64,%s\"",
                           b->icon_mime ? b->icon_mime : "image/png", b64);
            free(b64);
        }
    }

    bmk_buf_printf(out, ">%s</A>\n", etitle ? etitle : "");
    free(eurl);
    free(etitle);
}

/* 递归导出分类树 */
static void export_category(bmk_buf *out, bmk_db *db,
                            bmk_category **cats, size_t ncats,
                            bmk_bookmark **all, size_t nall,
                            int64_t parent_id, int depth)
{
    /* 先输出本层的子分类 */
    for (size_t i = 0; i < ncats; i++) {
        if (cats[i]->parent_id != parent_id) continue;

        export_indent(out, depth);
        char *ename = bmk_html_escape(cats[i]->name);
        bmk_buf_printf(out, "<DT><H3>%s</H3>\n", ename ? ename : "");
        free(ename);
        export_indent(out, depth);
        bmk_buf_append_str(out, "<DL><p>\n");

        export_category(out, db, cats, ncats, all, nall, cats[i]->id, depth + 1);

        export_indent(out, depth);
        bmk_buf_append_str(out, "</DL><p>\n");
    }

    /* 再输出属于本层的书签 */
    for (size_t i = 0; i < nall; i++) {
        if (all[i]->category_id != parent_id) continue;
        export_anchor(out, all[i], depth);
    }
}

bmk_err bmk_export_html_string(bmk_db *db, bmk_bookmark **list, size_t n, char **out)
{
    if (!db || !out) return BMK_ERR_INVALID_ARG;
    *out = NULL;

    bmk_buf b;
    bmk_buf_init(&b);
    /* 附录 A 格式要点；UTF-8、LF 换行 */
    bmk_buf_append_str(&b, "<!DOCTYPE NETSCAPE-Bookmark-file-1>\n");
    bmk_buf_append_str(&b, "<META HTTP-EQUIV=\"Content-Type\" CONTENT=\"text/html; charset=UTF-8\">\n");
    bmk_buf_append_str(&b, "<TITLE>Bookmarks</TITLE>\n");
    bmk_buf_append_str(&b, "<H1>Bookmarks</H1>\n");
    bmk_buf_append_str(&b, "<DL><p>\n");

    bmk_bookmark **owned = NULL;
    size_t nowned = 0;

    if (list) {
        /*
         * 指定了列表（当前筛选结果）：按给定顺序平铺导出，保持排序顺序
         * （FR-08 第 1 条）。
         */
        for (size_t i = 0; i < n; i++) export_anchor(&b, list[i], 1);
    } else {
        /* 全部：按分类树嵌套导出，未分类放根 <DL>（FR-08 第 3 条） */
        bmk_err e = bmk_bookmark_query(db, NULL, BMK_SORT_CREATED_AT, true, &owned, &nowned);
        if (e != BMK_OK) { bmk_buf_free(&b); return e; }

        bmk_category **cats = NULL;
        size_t ncats = 0;
        e = bmk_category_list(db, &cats, &ncats);
        if (e != BMK_OK) {
            bmk_bookmark_list_free(owned, nowned);
            bmk_buf_free(&b);
            return e;
        }
        /* 根层分类的 parent_id 在模型中为 0 */
        export_category(&b, db, cats, ncats, owned, nowned, 0, 1);

        bmk_category_list_free(cats, ncats);
        bmk_bookmark_list_free(owned, nowned);
    }

    bmk_buf_append_str(&b, "</DL><p>\n");
    *out = bmk_buf_steal(&b);
    return BMK_OK;
}

bmk_err bmk_export_html_file(bmk_db *db, const char *path,
                             bmk_bookmark **list, size_t n)
{
    if (!db || !path) return BMK_ERR_INVALID_ARG;
    char *html = NULL;
    bmk_err e = bmk_export_html_string(db, list, n, &html);
    if (e != BMK_OK) return e;
    e = bmk_write_file(path, html, strlen(html));
    free(html);
    if (e == BMK_OK) BMK_INFO("exported bookmarks to %s", path);
    return e;
}
