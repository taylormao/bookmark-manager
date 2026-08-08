/*
 * bmk_sync.c - 云同步实现
 *
 * 安全：PAT 与 secret_key 不出现在日志、界面与同步内容中（需求 5.3 第 1 条）。
 */
#include "bmk_sync.h"
#include "bmk_backup.h"
#include "bmk_log.h"
#include "bmk_netscape.h"
#include "bmk_process.h"
#include "bmk_fetch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>
#include <sqlite3.h>

static void emit(bmk_sync_log_cb cb, void *user, const char *fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    BMK_INFO("sync: %s", buf);
    if (cb) cb(buf, user);
}

/* ------------------------------------------------------------------ */
/* git 输出脱敏（需求 5.3 第 1 条：PAT 不得出现在日志与界面）            */
/* ------------------------------------------------------------------ */

/* 同步期间待脱敏的密钥（PAT）。git 错误输出可能回显带 PAT 的远端 URL
 * （如 "fatal: repository 'https://<pat>@github.com/...' not found"），
 * 展示/落日志前必须抹除。仅在同步流程内使用（单线程），结束后清空。 */
static const char *g_redact_secret = NULL;

static void redact_set(const char *secret)
{
    /* 太短的串（如 <8 字符）不参与替换，避免把普通文本误伤成星号 */
    g_redact_secret = (secret && strlen(secret) >= 8) ? secret : NULL;
}

static void redact_clear(void)
{
    g_redact_secret = NULL;
}

static void redact_in_place(char *text)
{
    if (!text || !g_redact_secret || !*g_redact_secret) return;
    size_t sl = strlen(g_redact_secret);
    char *p = text;
    while ((p = strstr(p, g_redact_secret)) != NULL) {
        for (size_t i = 0; i < sl; i++) p[i] = '*';
        p += sl;
    }
}

static bmk_err git_run(const char *git, const char *cwd, bmk_sync_log_cb log,
                       void *user, char **out, const char *const *args)
{
    /* args[0] 起为 git 子命令 */
    const char *argv[24];
    size_t i = 0;
    argv[i++] = git;
    for (size_t k = 0; args[k] && i < 23; k++) argv[i++] = args[k];
    argv[i] = NULL;

    /*
     * 固定 git 输出语言为英文（LC_ALL=C / LANG=C）。
     * commit_is_noop 依赖 "nothing to commit" 等英文关键词，中文本地化的
     * git 会输出“无文件要提交”导致误判（no-op 被当成真实失败→push 误中止）。
     * bmk_proc_run 用 popen 继承进程环境，故在此临时设置、调用后恢复；
     * 同步流程单线程（bmk_proc_run 本身也会切换进程 cwd），此处安全。
     */
    char *saved_lc = getenv("LC_ALL") ? bmk_strdup(getenv("LC_ALL")) : NULL;
#ifdef _WIN32
    _putenv("LC_ALL=C");
    _putenv("LANG=C");
#else
    setenv("LC_ALL", "C", 1);
    setenv("LANG", "C", 1);
#endif

    char *text = NULL;
    int code = -1;
    bmk_err e = bmk_proc_run(argv, cwd, &text, &code);

#ifdef _WIN32
    if (saved_lc) {
        char buf[4096];
        snprintf(buf, sizeof(buf), "LC_ALL=%s", saved_lc);
        _putenv(buf);
        free(saved_lc);
    } else {
        _putenv("LC_ALL=");
    }
    _putenv("LANG=");
#else
    if (saved_lc) { setenv("LC_ALL", saved_lc, 1); free(saved_lc); }
    else          unsetenv("LC_ALL");
    unsetenv("LANG");
#endif

    if (text && *text) {
        redact_in_place(text);
        if (log) emit(log, user, "%s", text);
    }
    if (out) *out = text;
    else     free(text);
    return e;
}

/* ------------------------------------------------------------------ */
/* sync_meta                                                           */
/* ------------------------------------------------------------------ */

char *bmk_sync_meta_get(bmk_db *db, const char *key)
{
    if (!db || !key) return NULL;
    sqlite3 *h = bmk_db_handle(db);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(h, "SELECT value FROM sync_meta WHERE key=?;",
                           -1, &st, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_text(st, 1, key, -1, SQLITE_TRANSIENT);
    char *v = NULL;
    if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) != SQLITE_NULL)
        v = bmk_strdup((const char *)sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
    return v;
}

bmk_err bmk_sync_meta_set(bmk_db *db, const char *key, const char *value)
{
    if (!db || !key) return BMK_ERR_INVALID_ARG;
    sqlite3 *h = bmk_db_handle(db);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(h,
            "INSERT INTO sync_meta(key,value) VALUES(?,?) "
            "ON CONFLICT(key) DO UPDATE SET value=excluded.value;",
            -1, &st, NULL) != SQLITE_OK) return BMK_ERR_DB;
    sqlite3_bind_text(st, 1, key, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, value ? value : "", -1, SQLITE_TRANSIENT);
    bmk_err e = (sqlite3_step(st) == SQLITE_DONE) ? BMK_OK : BMK_ERR_DB;
    sqlite3_finalize(st);
    return e;
}

/* ------------------------------------------------------------------ */
/* GitHub                                                              */
/* ------------------------------------------------------------------ */

/* 构造单次注入的 HTTP 认证头（git -c http.extraheader=...）。
 * 返回 "http.extraheader=AUTHORIZATION: basic <base64>"（堆内存），
 * SSH 或无 PAT 时返回 NULL。凭据仅作用于当次 git 进程，绝不写入
 * .git/config（避免 PAT 明文持久化到本地第二份拷贝）。 */
static char *build_auth_header(const bmk_github_cfg *cfg)
{
    if (cfg->use_ssh || !cfg->pat || !*cfg->pat) return NULL;

    /* GitHub 接受 x-access-token 作为 basic auth 用户名，PAT 为密码 */
    static const char user_prefix[] = "x-access-token:";
    size_t n = sizeof(user_prefix) - 1 + strlen(cfg->pat);
    char *plain = (char *)malloc(n + 1);
    if (!plain) return NULL;
    memcpy(plain, user_prefix, sizeof(user_prefix) - 1);
    memcpy(plain + sizeof(user_prefix) - 1, cfg->pat, strlen(cfg->pat) + 1);

    char *b64 = bmk_base64_encode((const unsigned char *)plain, n);
    free(plain);
    if (!b64) return NULL;

    static const char head_prefix[] = "http.extraheader=AUTHORIZATION: basic ";
    char *hdr = (char *)malloc(sizeof(head_prefix) - 1 + strlen(b64) + 1);
    if (!hdr) { free(b64); return NULL; }
    memcpy(hdr, head_prefix, sizeof(head_prefix) - 1);
    strcpy(hdr + sizeof(head_prefix) - 1, b64);
    free(b64);
    return hdr;
}

/* 带凭据注入的 git 调用：在子命令前插入 -c http.extraheader=...，
 * 凭据仅作用于当次进程（-c 不写配置文件），auth 为 NULL 时等同 git_run。 */
static bmk_err git_run_cred(const char *git, const char *cwd, bmk_sync_log_cb log,
                            void *user, char **out, const char *auth,
                            const char *const *args)
{
    if (!auth) return git_run(git, cwd, log, user, out, args);
    const char *argv[26];
    size_t i = 0;
    argv[i++] = "-c";
    argv[i++] = auth;
    for (size_t k = 0; args[k] && i < 24; k++) argv[i++] = args[k];
    argv[i] = NULL;
    return git_run(git, cwd, log, user, out, argv);
}

/* 探测默认分支：优先 main，其次 master */
static char *detect_branch(const char *git, const char *cwd)
{
    char *out = NULL;
    const char *args[] = { "rev-parse", "--abbrev-ref", "HEAD", NULL };
    if (git_run(git, cwd, NULL, NULL, &out, args) == BMK_OK && out) {
        bmk_str_trim(out);
        if (*out && strcmp(out, "HEAD") != 0) return out;
    }
    free(out);
    return bmk_strdup("main");
}

/* git commit 的“无变更”输出识别：此时 exit code 非 0，属正常情况；
 * 其余非 0 退出（如缺 user.name/email、仓库状态异常）为真实失败，
 * 必须中止流程，否则后续 push 会报 up-to-date 而远端未更新（静默丢同步）。 */
static bool commit_is_noop(const char *out)
{
    if (!out) return false;
    if (strstr(out, "nothing to commit") != NULL)  return true;  /* EN 现代 git */
    if (strstr(out, "No changes") != NULL)         return true;  /* 旧版 git */
    if (strstr(out, "nothing added to commit") != NULL) return true;
    return false;
}

bmk_err bmk_sync_github_push(bmk_db *db, const bmk_github_cfg *cfg,
                             bmk_sync_log_cb log, void *user)
{
    if (!db || !cfg || !cfg->repo || !*cfg->repo || !cfg->sync_dir)
        return BMK_ERR_INVALID_ARG;

    /* git 可执行路径：配置优先，否则 PATH 探测 */
    char *git_owned = NULL;
    const char *git = cfg->git_path;
    if (!git || !*git) {
        git_owned = bmk_which("git");
        git = git_owned;
    }
    if (!git) {
        emit(log, user, "git not found in PATH; please install git or set its path");
        return BMK_ERR_NOT_FOUND;
    }

    bmk_err e = bmk_mkdirs(cfg->sync_dir);
    if (e != BMK_OK) { free(git_owned); return e; }

    char *auth = build_auth_header(cfg);
    /* 从 git 输出中抹除 PAT（可能回显在远端 URL 里） */
    redact_set(cfg->pat);
    char *git_dir = bmk_path_join(cfg->sync_dir, ".git");
    bool cloned = git_dir && bmk_file_exists(git_dir);
    free(git_dir);

    /* 首次执行 clone，后续复用（FR-18 第 3 条） */
    if (!cloned) {
        emit(log, user, "cloning repository into sync directory");
        const char *args[] = { "clone", cfg->repo, ".", NULL };
        e = git_run_cred(git, cfg->sync_dir, log, user, NULL, auth, args);
        if (e != BMK_OK) {
            emit(log, user, "clone failed; verify the repository exists and credentials are valid");
            free(auth); free(git_owned);
            redact_clear();
            return BMK_ERR_AUTH;
        }
    } else {
        /* 把 remote 恢复为不含 PAT 的干净地址（兼容旧版本曾把凭据 URL 写入
         * .git/config 的情况）；认证改由 http.extraheader 单次注入。 */
        const char *args[] = { "remote", "set-url", "origin", cfg->repo, NULL };
        git_run(git, cfg->sync_dir, NULL, NULL, NULL, args);
    }

    char *branch_owned = NULL;
    const char *branch = cfg->branch;
    if (!branch || !*branch) {
        branch_owned = detect_branch(git, cfg->sync_dir);
        branch = branch_owned;
    }

    /* a. git fetch origin */
    {
        const char *args[] = { "fetch", "origin", NULL };
        if (git_run_cred(git, cfg->sync_dir, log, user, NULL, auth, args) != BMK_OK)
            emit(log, user, "fetch failed; continuing with local state");
    }

    /* 判断本地与远端关系 */
    bool diverged = false;
    {
        char ref[256];
        snprintf(ref, sizeof(ref), "origin/%s", branch);
        char *out = NULL;
        const char *args[] = { "rev-list", "--left-right", "--count", NULL, NULL };
        char range[512];
        snprintf(range, sizeof(range), "HEAD...%s", ref);
        args[3] = range;
        if (git_run(git, cfg->sync_dir, NULL, NULL, &out, args) == BMK_OK && out) {
            long ahead = 0, behind = 0;
            if (sscanf(out, "%ld %ld", &ahead, &behind) == 2) {
                if (behind > 0 && ahead == 0) {
                    /* b. 可快进 */
                    emit(log, user, "local is behind remote; fast-forwarding");
                    const char *ff[] = { "merge", "--ff-only", ref, NULL };
                    git_run(git, cfg->sync_dir, log, user, NULL, ff);
                } else if (behind > 0 && ahead > 0) {
                    diverged = true;
                }
            }
        }
        free(out);
    }

    /* c. 分叉：先备份远端版本 */
    if (diverged) {
        emit(log, user, "local and remote have diverged; backing up remote copy");
        char spec[512];
        snprintf(spec, sizeof(spec), "origin/%s:bookmarks.html", branch);
        char *out = NULL;
        const char *args[] = { "show", spec, NULL };
        if (git_run(git, cfg->sync_dir, NULL, NULL, &out, args) == BMK_OK &&
            out && *out) {
            char *path = NULL;
            if (bmk_backup_write_raw("remote", "html", out, strlen(out), &path) == BMK_OK) {
                emit(log, user, "remote version saved to %s", path);
                free(path);
            }
        }
        free(out);
    }

    /* d. 用本地导出覆盖工作区 */
    {
        char *html = NULL;
        e = bmk_export_html_string(db, NULL, 0, &html);
        if (e != BMK_OK) {
            free(auth); free(branch_owned); free(git_owned);
            redact_clear();
            return e;
        }
        char *dst = bmk_path_join(cfg->sync_dir, "bookmarks.html");
        e = dst ? bmk_write_file(dst, html, strlen(html)) : BMK_ERR_NOMEM;
        free(html);
        free(dst);
        if (e != BMK_OK) {
            free(auth); free(branch_owned); free(git_owned);
            redact_clear();
            return e;
        }
    }
    if (cfg->push_db && cfg->db_path && bmk_file_exists(cfg->db_path)) {
        char *data = NULL;
        size_t len = 0;
        if (bmk_read_file(cfg->db_path, &data, &len) == BMK_OK) {
            char ts[32];
            bmk_timestamp_str(bmk_now(), ts, sizeof(ts));
            char name[128];
            snprintf(name, sizeof(name), "bookmarks-%s.db", ts);
            char *dst = bmk_path_join(cfg->sync_dir, name);
            if (dst) { bmk_write_file(dst, data, len); free(dst); }
            free(data);
        }
    }

    /* e. add + commit */
    {
        const char *add[] = { "add", "-A", NULL };
        git_run(git, cfg->sync_dir, log, user, NULL, add);

        char iso[32];
        bmk_iso8601(bmk_now(), iso, sizeof(iso));
        char msg[128];
        snprintf(msg, sizeof(msg), "sync %s", iso);
        const char *ci[] = { "commit", "-m", msg, NULL };
        char *co = NULL;
        bmk_err ce = git_run(git, cfg->sync_dir, log, user, &co, ci);
        if (ce != BMK_OK) {
            /* 区分“无变更”（正常）与真实失败（如缺 user.name/email）：
             * 真实失败必须中止，否则 push 报 up-to-date 而远端未更新。 */
            if (!commit_is_noop(co)) {
                emit(log, user,
                     "commit failed; check git identity (user.name/user.email) "
                     "and repository state");
                free(co);
                free(auth); free(branch_owned); free(git_owned);
                redact_clear();
                return BMK_ERR_SUBPROCESS;
            }
            emit(log, user, "no changes to commit");
        }
        free(co);
    }

    /* f. push（分叉时用 --force-with-lease） */
    {
        bmk_err pe;
        if (diverged) {
            emit(log, user, "pushing with --force-with-lease (local wins)");
            const char *args[] = { "push", "--force-with-lease", "origin", branch, NULL };
            pe = git_run_cred(git, cfg->sync_dir, log, user, NULL, auth, args);
        } else {
            const char *args[] = { "push", "origin", branch, NULL };
            pe = git_run_cred(git, cfg->sync_dir, log, user, NULL, auth, args);
        }
        if (pe != BMK_OK) {
            emit(log, user, "push failed; check network and credentials");
            free(auth); free(branch_owned); free(git_owned);
            redact_clear();
            return BMK_ERR_NETWORK;
        }
    }

    /* g. 写 sync_meta */
    {
        char iso[32];
        bmk_iso8601(bmk_now(), iso, sizeof(iso));
        bmk_sync_meta_set(db, BMK_SYNC_LAST_SYNC, iso);
        bmk_sync_meta_set(db, BMK_SYNC_GITHUB_REMOTE, cfg->repo);
    }
    emit(log, user, "sync completed");

    free(auth);
    free(branch_owned);
    free(git_owned);
    redact_clear();
    return BMK_OK;
}

bmk_err bmk_sync_github_restore(bmk_db *db, const bmk_github_cfg *cfg,
                                bmk_sync_log_cb log, void *user)
{
    if (!db || !cfg || !cfg->sync_dir || !cfg->repo || !*cfg->repo)
        return BMK_ERR_INVALID_ARG;

    char *git_owned = NULL;
    const char *git = cfg->git_path;
    if (!git || !*git) { git_owned = bmk_which("git"); git = git_owned; }
    if (!git) return BMK_ERR_NOT_FOUND;

    bmk_err e = bmk_mkdirs(cfg->sync_dir);
    if (e != BMK_OK) { free(git_owned); return e; }

    /* 首次恢复：sync_dir 尚无 .git 时先 clone（与 push 逻辑一致），
     * 否则直接 pull 在非 git 目录失败（FR-18 第 3 条 / QA 第 3 项）。 */
    redact_set(cfg->pat);
    char *git_dir = bmk_path_join(cfg->sync_dir, ".git");
    bool cloned = git_dir && bmk_file_exists(git_dir);
    free(git_dir);

    if (!cloned) {
        char *auth = build_auth_header(cfg);
        emit(log, user, "cloning repository into sync directory");
        const char *args[] = { "clone", cfg->repo, ".", NULL };
        bmk_err ce = git_run_cred(git, cfg->sync_dir, log, user, NULL, auth, args);
        free(auth);
        if (ce != BMK_OK) {
            emit(log, user, "clone failed; verify the repository exists and credentials are valid");
            free(git_owned);
            redact_clear();
            return BMK_ERR_AUTH;
        }
    } else {
        /* 把 remote 恢复为不含 PAT 的干净地址（兼容旧版本曾写入 .git/config
         * 的凭据 URL）；认证由 http.extraheader 单次注入。 */
        const char *su[] = { "remote", "set-url", "origin", cfg->repo, NULL };
        git_run(git, cfg->sync_dir, NULL, NULL, NULL, su);
        char *auth = build_auth_header(cfg);
        const char *args[] = { "pull", "--ff-only", NULL };
        bmk_err pe = git_run_cred(git, cfg->sync_dir, log, user, NULL, auth, args);
        free(auth);
        if (pe != BMK_OK) {
            emit(log, user, "pull failed; check network and credentials");
            free(git_owned);
            redact_clear();
            return BMK_ERR_NETWORK;
        }
    }
    free(git_owned);

    char *path = bmk_path_join(cfg->sync_dir, "bookmarks.html");
    if (!path) { redact_clear(); return BMK_ERR_NOMEM; }
    if (!bmk_file_exists(path)) {
        free(path);
        redact_clear();
        emit(log, user, "remote bookmarks.html not found");
        return BMK_ERR_NOT_FOUND;
    }

    /* 走 FR-07 导入流程（默认跳过重复） */
    bmk_import_opts opts;
    bmk_import_opts_init(&opts);
    bmk_import_report rep;
    memset(&rep, 0, sizeof(rep));
    e = bmk_import_html_file(db, path, &opts, &rep);
    emit(log, user, "restore: added=%lld skipped=%lld failed=%lld",
         (long long)rep.added, (long long)rep.skipped, (long long)rep.failed);
    bmk_import_report_free(&rep);
    free(path);
    redact_clear();
    return e;
}

/* ------------------------------------------------------------------ */
/* S3（附录 D）                                                        */
/* ------------------------------------------------------------------ */

typedef struct { bmk_buf buf; } s3_sink;

static size_t s3_write_cb(char *ptr, size_t size, size_t nmemb, void *user)
{
    s3_sink *s = (s3_sink *)user;
    size_t n = size * nmemb;
    if (!bmk_buf_append(&s->buf, ptr, n)) return 0;
    return n;
}

typedef struct { const char *data; size_t len; size_t pos; } s3_source;

static size_t s3_read_cb(char *ptr, size_t size, size_t nmemb, void *user)
{
    s3_source *s = (s3_source *)user;
    size_t want = size * nmemb;
    size_t left = s->len - s->pos;
    size_t n = want < left ? want : left;
    memcpy(ptr, s->data + s->pos, n);
    s->pos += n;
    return n;
}

/* 构造对象 URL：path-style = https://<endpoint>/<bucket>/<key> */
static char *s3_object_url(const bmk_s3_cfg *cfg, const char *key)
{
    /* 默认 endpoint 含 region（FR-19 第 1 条：https://s3.<region>.amazonaws.com） */
    char def_endpoint[256];
    const char *endpoint = (cfg->endpoint && *cfg->endpoint) ? cfg->endpoint : NULL;
    if (!endpoint) {
        const char *region = (cfg->region && *cfg->region) ? cfg->region : "us-east-1";
        snprintf(def_endpoint, sizeof(def_endpoint), "https://s3.%s.amazonaws.com", region);
        endpoint = def_endpoint;
    }
    bmk_buf b;
    bmk_buf_init(&b);

    if (cfg->path_style) {
        bmk_buf_append_str(&b, endpoint);
        if (b.len && b.data[b.len - 1] == '/') b.data[--b.len] = '\0';
        bmk_buf_append_char(&b, '/');
        bmk_buf_append_str(&b, cfg->bucket);
    } else {
        /* virtual-host: https://<bucket>.<host>/ */
        const char *scheme_end = strstr(endpoint, "://");
        if (scheme_end) {
            bmk_buf_append(&b, endpoint, (size_t)(scheme_end - endpoint) + 3);
            bmk_buf_append_str(&b, cfg->bucket);
            bmk_buf_append_char(&b, '.');
            bmk_buf_append_str(&b, scheme_end + 3);
        } else {
            bmk_buf_append_str(&b, endpoint);
        }
        if (b.len && b.data[b.len - 1] == '/') b.data[--b.len] = '\0';
    }

    if (cfg->prefix && *cfg->prefix) {
        if (cfg->prefix[0] != '/') bmk_buf_append_char(&b, '/');
        bmk_buf_append_str(&b, cfg->prefix);
        size_t l = b.len;
        if (l && b.data[l - 1] == '/') b.data[--b.len] = '\0';
    }
    bmk_buf_append_char(&b, '/');
    bmk_buf_append_str(&b, key);
    return bmk_buf_steal(&b);
}

/* 统一设置 SigV4 认证（附录 D） */
static void s3_auth(CURL *c, const bmk_s3_cfg *cfg, char *sigv4_buf, size_t bufsz,
                    char *userpwd_buf, size_t pwdsz)
{
    const char *region = (cfg->region && *cfg->region) ? cfg->region : "us-east-1";
    snprintf(sigv4_buf, bufsz, "aws:amz:%s:s3", region);
    snprintf(userpwd_buf, pwdsz, "%s:%s",
             cfg->access_key ? cfg->access_key : "",
             cfg->secret_key ? cfg->secret_key : "");
    curl_easy_setopt(c, CURLOPT_HTTPAUTH, (long)CURLAUTH_AWS_SIGV4);
    curl_easy_setopt(c, CURLOPT_AWS_SIGV4, sigv4_buf);
    curl_easy_setopt(c, CURLOPT_USERPWD, userpwd_buf);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 60L);
}

/* HEAD：404 视为不存在（合法） */
static bmk_err s3_head(const bmk_s3_cfg *cfg, const char *url, long *out_status)
{
    CURL *c = curl_easy_init();
    if (!c) return BMK_ERR_NOMEM;
    bmk_curl_set_default_cainfo(c);
    char sig[128], pwd[512];
    s3_auth(c, cfg, sig, sizeof(sig), pwd, sizeof(pwd));
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_NOBODY, 1L);

    CURLcode rc = curl_easy_perform(c);
    long status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(c);
    memset(pwd, 0, sizeof(pwd));

    if (out_status) *out_status = status;
    return (rc == CURLE_OK) ? BMK_OK : BMK_ERR_NETWORK;
}

static bmk_err s3_get(const bmk_s3_cfg *cfg, const char *url,
                      char **out, size_t *out_len, long *out_status)
{
    CURL *c = curl_easy_init();
    if (!c) return BMK_ERR_NOMEM;
    bmk_curl_set_default_cainfo(c);
    char sig[128], pwd[512];
    s3_auth(c, cfg, sig, sizeof(sig), pwd, sizeof(pwd));

    s3_sink sink;
    bmk_buf_init(&sink.buf);
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, s3_write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &sink);

    CURLcode rc = curl_easy_perform(c);
    long status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(c);
    memset(pwd, 0, sizeof(pwd));

    if (out_status) *out_status = status;
    if (rc != CURLE_OK) { bmk_buf_free(&sink.buf); return BMK_ERR_NETWORK; }
    if (out_len) *out_len = sink.buf.len;
    *out = bmk_buf_steal(&sink.buf);
    return BMK_OK;
}

static bmk_err s3_put(const bmk_s3_cfg *cfg, const char *url,
                      const char *data, size_t len, long *out_status)
{
    CURL *c = curl_easy_init();
    if (!c) return BMK_ERR_NOMEM;
    bmk_curl_set_default_cainfo(c);
    char sig[128], pwd[512];
    s3_auth(c, cfg, sig, sizeof(sig), pwd, sizeof(pwd));

    s3_source src = { data, len, 0 };
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(c, CURLOPT_READFUNCTION, s3_read_cb);
    curl_easy_setopt(c, CURLOPT_READDATA, &src);
    curl_easy_setopt(c, CURLOPT_INFILESIZE_LARGE, (curl_off_t)len);

    CURLcode rc = curl_easy_perform(c);
    long status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(c);
    memset(pwd, 0, sizeof(pwd));

    if (out_status) *out_status = status;
    if (rc != CURLE_OK) return BMK_ERR_NETWORK;
    return (status >= 200 && status < 300) ? BMK_OK : BMK_ERR_NETWORK;
}

bmk_err bmk_sync_s3_push(bmk_db *db, const bmk_s3_cfg *cfg,
                         bmk_sync_log_cb log, void *user)
{
    if (!db || !cfg || !cfg->bucket || !*cfg->bucket) return BMK_ERR_INVALID_ARG;

    char *url = s3_object_url(cfg, "bookmarks.html");
    if (!url) return BMK_ERR_NOMEM;

    /* a. 凭据校验：HEAD（404 视为合法） */
    long status = 0;
    bmk_err e = s3_head(cfg, url, &status);
    if (e != BMK_OK) {
        emit(log, user, "endpoint unreachable");
        free(url);
        return BMK_ERR_NETWORK;
    }
    if (status == 403) {
        emit(log, user, "credentials rejected by the server");
        free(url);
        return BMK_ERR_AUTH;
    }
    if (status == 404) {
        emit(log, user, "remote object does not exist yet (first push)");
    } else if (status >= 400) {
        emit(log, user, "unexpected status %ld; verify bucket exists", status);
        free(url);
        return BMK_ERR_NETWORK;
    }

    /* b. 远端存在 → 先下载备份 */
    if (status == 200) {
        char *old = NULL;
        size_t olen = 0;
        long gstatus = 0;
        if (s3_get(cfg, url, &old, &olen, &gstatus) == BMK_OK && old && olen) {
            char *path = NULL;
            if (bmk_backup_write_raw("remote", "html", old, olen, &path) == BMK_OK) {
                emit(log, user, "remote version saved to %s", path);
                free(path);
            }
        }
        free(old);
    }

    /* c. 上传本地导出 */
    char *html = NULL;
    e = bmk_export_html_string(db, NULL, 0, &html);
    if (e != BMK_OK) { free(url); return e; }

    long pstatus = 0;
    e = s3_put(cfg, url, html, strlen(html), &pstatus);
    free(html);
    if (e != BMK_OK) {
        emit(log, user, "upload failed (status %ld)", pstatus);
        free(url);
        return e;
    }
    emit(log, user, "bookmarks.html uploaded");
    free(url);

    /* 可选数据库快照 */
    if (cfg->push_db && cfg->db_path && bmk_file_exists(cfg->db_path)) {
        char *data = NULL;
        size_t len = 0;
        if (bmk_read_file(cfg->db_path, &data, &len) == BMK_OK) {
            char ts[32];
            bmk_timestamp_str(bmk_now(), ts, sizeof(ts));
            char key[128];
            snprintf(key, sizeof(key), "bookmarks-%s.db", ts);
            char *durl = s3_object_url(cfg, key);
            if (durl) {
                long ds = 0;
                if (s3_put(cfg, durl, data, len, &ds) == BMK_OK)
                    emit(log, user, "database snapshot uploaded");
                free(durl);
            }
            free(data);
        }
    }

    /* d. 写 sync_meta */
    char iso[32];
    bmk_iso8601(bmk_now(), iso, sizeof(iso));
    bmk_sync_meta_set(db, BMK_SYNC_LAST_SYNC, iso);
    bmk_sync_meta_set(db, BMK_SYNC_S3_TARGET, cfg->bucket);
    emit(log, user, "sync completed");
    return BMK_OK;
}

bmk_err bmk_sync_s3_restore(bmk_db *db, const bmk_s3_cfg *cfg,
                            bmk_sync_log_cb log, void *user)
{
    if (!db || !cfg) return BMK_ERR_INVALID_ARG;

    char *url = s3_object_url(cfg, "bookmarks.html");
    if (!url) return BMK_ERR_NOMEM;

    char *data = NULL;
    size_t len = 0;
    long status = 0;
    bmk_err e = s3_get(cfg, url, &data, &len, &status);
    free(url);
    if (e != BMK_OK || status != 200 || !data || !len) {
        free(data);
        emit(log, user, "download failed (status %ld)", status);
        return BMK_ERR_NETWORK;
    }

    bmk_import_opts opts;
    bmk_import_opts_init(&opts);
    bmk_import_report rep;
    memset(&rep, 0, sizeof(rep));
    e = bmk_import_html_buffer(db, data, len, &opts, &rep);
    emit(log, user, "restore: added=%lld skipped=%lld failed=%lld",
         (long long)rep.added, (long long)rep.skipped, (long long)rep.failed);
    bmk_import_report_free(&rep);
    free(data);
    return e;
}
