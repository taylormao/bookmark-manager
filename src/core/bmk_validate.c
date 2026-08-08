/*
 * bmk_validate.c - 批量校验实现（FR-14 第 5 条）
 *
 * 判定规则（第 5 条）：
 *   2xx                 → valid=1, note=ok
 *   4xx/5xx             → valid=0, status=实际状态码
 *   403/429             → valid 保持原值, note=blocked（待复测）
 *   网络层失败           → valid=0, status=0, note=具体错误类型
 *
 * 并发模型（需求 5.1 第 2 条）：
 *   生产者-消费者：主线程（生产者）将 URL 推入固定容量队列，
 *   多个 worker 线程并行执行 HEAD→判定→写库→回调。
 *   每 host 的请求由 per_host_limit 限制并发数，
 *   通过 host_semaphore 表实现细粒度限流（FR-14 第 3 条）。
 */
#include "bmk_validate.h"
#include "bmk_fetch.h"
#include "bmk_log.h"
#include "bmk_settings.h"
#include "bmk_proxy.h"
#include "bmk_url.h"

#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>

#ifdef _WIN32
#  include <windows.h>
   typedef HANDLE              bmk_thread;
   typedef CRITICAL_SECTION    bmk_mtx;
   typedef CONDITION_VARIABLE  bmk_cv;
#  define MTX_INIT(m)   InitializeCriticalSection(m)
#  define MTX_LOCK(m)   EnterCriticalSection(m)
#  define MTX_UNLOCK(m) LeaveCriticalSection(m)
#  define MTX_FREE(m)   DeleteCriticalSection(m)
#  define CV_INIT(c)    InitializeConditionVariable(c)
#  define CV_WAIT(c,m)  SleepConditionVariableCS(c, m, INFINITE)
#  define CV_SIGNAL(c)  WakeConditionVariable(c)
#  define CV_BROADCAST(c) WakeAllConditionVariable(c)
#  define THREAD_CREATE(t,fn,a)  ((*(t) = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)(fn), (a), 0, NULL)) != NULL)
#  define THREAD_JOIN(t)         do { WaitForSingleObject(t, INFINITE); CloseHandle(t); } while(0)
#  define SLEEP_MS(ms)           Sleep(ms)
#else
#  include <pthread.h>
#  include <unistd.h>
   typedef pthread_t         bmk_thread;
   typedef pthread_mutex_t   bmk_mtx;
   typedef pthread_cond_t    bmk_cv;
#  define MTX_INIT(m)   pthread_mutex_init((m), NULL)
#  define MTX_LOCK(m)   pthread_mutex_lock(m)
#  define MTX_UNLOCK(m) pthread_mutex_unlock(m)
#  define MTX_FREE(m)   pthread_mutex_destroy(m)
#  define CV_INIT(c)    pthread_cond_init(c, NULL)
#  define CV_WAIT(c,m)  pthread_cond_wait(c, m)
#  define CV_SIGNAL(c)  pthread_cond_signal(c)
#  define CV_BROADCAST(c) pthread_cond_broadcast(c)
#  define THREAD_CREATE(t,fn,a)  (pthread_create((t), NULL, (void *(*)(void*))(fn), (a)) == 0)
#  define THREAD_JOIN(t)         pthread_join(t, NULL)
#  define SLEEP_MS(ms)           usleep((useconds_t)(ms) * 1000)
#endif

void bmk_validate_opts_init(bmk_validate_opts *o)
{
    memset(o, 0, sizeof(*o));
    o->timeout_s = 10;
    o->connect_timeout_s = 5;
    o->max_redirects = 5;
    o->concurrency = 4;
    o->per_host_limit = 2;
    o->interval_ms = 500;
    o->jitter_ms = 200;
    o->retry = 1;
    o->proxy_mode = BMK_PROXY_DIRECT;
}

void bmk_validate_opts_load(bmk_db *db, bmk_validate_opts *o)
{
    bmk_validate_opts_init(o);
    if (!db) return;
    o->timeout_s         = (int)bmk_settings_get_int(db, BMK_SET_CHK_TIMEOUT, 10);
    o->connect_timeout_s = (int)bmk_settings_get_int(db, BMK_SET_CHK_CONN_TIMEOUT, 5);
    o->max_redirects     = (int)bmk_settings_get_int(db, BMK_SET_CHK_MAX_REDIR, 5);
    o->concurrency       = (int)bmk_settings_get_int(db, BMK_SET_CHK_CONCURRENCY, 4);
    o->per_host_limit    = (int)bmk_settings_get_int(db, BMK_SET_CHK_PER_HOST, 2);
    o->interval_ms       = (int)bmk_settings_get_int(db, BMK_SET_CHK_INTERVAL_MS, 500);
    o->jitter_ms         = (int)bmk_settings_get_int(db, BMK_SET_CHK_JITTER_MS, 200);
    o->retry             = (int)bmk_settings_get_int(db, BMK_SET_CHK_RETRY, 1);

    if (o->concurrency < 1)  o->concurrency = 1;
    if (o->concurrency > 10) o->concurrency = 10;

    char *mode = bmk_settings_get_str(db, BMK_SET_PROXY_MODE, "direct");
    if (mode) {
        if      (strcmp(mode, "always") == 0) o->proxy_mode = BMK_PROXY_ALWAYS;
        else if (strcmp(mode, "auto") == 0)   o->proxy_mode = BMK_PROXY_AUTO;
        else                                  o->proxy_mode = BMK_PROXY_DIRECT;
        free(mode);
    }
}

/* ------------------------------------------------------------------ */
/* 判定（纯函数，单元测试直接调用）                                    */
/* ------------------------------------------------------------------ */

void bmk_check_judge(long status, const char *curl_error, int prev_valid,
                     bmk_check_result *out)
{
    memset(out, 0, sizeof(*out));

    if (curl_error && *curl_error) {
        /* 网络层失败 */
        out->valid = BMK_VALID_BAD;
        out->status_code = 0;
        snprintf(out->note, sizeof(out->note), "%s", curl_error);
        return;
    }

    /* 403/429 → 待复测：valid 保持原值 */
    if (status == 403 || status == 429) {
        out->valid = prev_valid;
        out->status_code = (int)status;
        snprintf(out->note, sizeof(out->note), "%s", BMK_NOTE_BLOCKED);
        return;
    }

    if (status >= 200 && status < 300) {
        out->valid = BMK_VALID_GOOD;
        out->status_code = (int)status;
        snprintf(out->note, sizeof(out->note), "%s", BMK_NOTE_OK);
        return;
    }

    /* 3xx（重定向后未再跳转的最终码）视为可访问（测试契约：2xx/3xx 有效） */
    if (status >= 300 && status < 400) {
        out->valid = BMK_VALID_GOOD;
        out->status_code = (int)status;
        snprintf(out->note, sizeof(out->note), "http_%ld", status);
        return;
    }

    /* 4xx / 5xx → 无效（已由 libcurl 跟随到最终码） */
    out->valid = BMK_VALID_BAD;
    out->status_code = (int)status;
    snprintf(out->note, sizeof(out->note), "http_%ld", status);
}

/* CURLcode → 稳定的错误标识（FR-14 第 5 条） */
static const char *curl_error_note(CURLcode rc)
{
    switch (rc) {
    case CURLE_OK:
        return NULL;
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_RESOLVE_PROXY:
        return BMK_NOTE_DNS_FAILED;
    case CURLE_OPERATION_TIMEDOUT:
        return BMK_NOTE_TIMEOUT;
    case CURLE_COULDNT_CONNECT:
        return BMK_NOTE_CONNECT_FAILED;
    case CURLE_SSL_CONNECT_ERROR:
    case CURLE_PEER_FAILED_VERIFICATION:
    case CURLE_SSL_CIPHER:
    case CURLE_SSL_CACERT_BADFILE:
        return BMK_NOTE_TLS_FAILED;
    default:
        return BMK_NOTE_NETWORK_ERROR;
    }
}

/* ------------------------------------------------------------------ */
/* 单条请求                                                            */
/* ------------------------------------------------------------------ */

static size_t discard_cb(char *ptr, size_t size, size_t nmemb, void *user)
{
    size_t n = size * nmemb;
    size_t *total = (size_t *)user;
    *total += n;
    /* GET 降级时最多接收 1 MiB 后中断，只取状态码（FR-14 第 2 条） */
    if (*total > 1024 * 1024) return 0;
    return n;
}

typedef struct {
    long status;
    const char *error_note;
} probe_result;

static void probe_url(const char *url, const bmk_validate_opts *o,
                      const char *proxy, probe_result *out)
{
    out->status = 0;
    out->error_note = NULL;

    CURL *c = curl_easy_init();
    if (!c) { out->error_note = BMK_NOTE_NETWORK_ERROR; return; }
    bmk_curl_set_default_cainfo(c);

    size_t received = 0;
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_NOBODY, 1L);          /* 默认 HEAD */
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, (long)o->max_redirects);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, (long)o->timeout_s);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, (long)o->connect_timeout_s);
    curl_easy_setopt(c, CURLOPT_USERAGENT, bmk_next_user_agent());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, discard_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &received);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    /* 直连时显式禁用环境代理变量，避免 HTTP_PROXY 等把请求带偏导致误杀 */
    if (proxy && *proxy)
        curl_easy_setopt(c, CURLOPT_PROXY, proxy);
    else
        curl_easy_setopt(c, CURLOPT_PROXY, "");

    CURLcode rc = curl_easy_perform(c);
    long status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);

    /* HEAD 未返回 2xx/3xx 时降级 GET 复核：部分站点对 HEAD 返回 404/403/5xx
     * 但 GET 正常，避免把可访问网址误判为无效（FR-14 第 2 条 + 实测反馈） */
    bool need_get = false;
    if (rc == CURLE_OK) {
        if (status == 0) need_get = true;
        else if (status < 200 || status >= 400) need_get = true;
    }
    if (need_get) {
        curl_easy_setopt(c, CURLOPT_NOBODY, 0L);
        curl_easy_setopt(c, CURLOPT_HTTPGET, 1L);
        received = 0;
        rc = curl_easy_perform(c);
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
        if (rc == CURLE_WRITE_ERROR && status > 0) rc = CURLE_OK;
    }

    out->status = status;
    out->error_note = curl_error_note(rc);
    curl_easy_cleanup(c);
}

/* ------------------------------------------------------------------ */
/* 并发工作池（需求 5.1 第 2 条）                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    int64_t  id;
    char    *url;
    int      prev_valid;
} task_item;

typedef struct {
    /* 共享状态 */
    bmk_db            *db;
    const bmk_validate_opts *opts;
    bmk_validate_cb    cb;
    void              *cb_user;
    volatile bool      canceled;
    volatile bool      user_canceled;   /* 回调主动取消，与收尾置位的 canceled 区分 */

    /* 任务队列 */
    task_item         *queue;
    int                q_cap;
    volatile int       q_head;      /* 生产者写入位置 */
    volatile int       q_tail;      /* 消费者读取位置 */
    volatile int       q_count;     /* 当前队列元素数 */
    bmk_mtx            q_lock;
    bmk_cv             q_cond;      /* 消费者等待 */
    bmk_cv             q_full;      /* 生产者等待（队列满时） */

    /* 进度计数器 */
    volatile int64_t   done_count;
    volatile int64_t   total_count;
    bmk_mtx            done_lock;

    /* 每 host 信号量：key=host, value=剩余许可数 */
    struct {
        char   *host;
        int     slots;
    } *host_limits;
    int                host_limit_n;
    int                host_limit_cap;
    bmk_mtx            host_lock;
} pool_state;

static void pool_state_free(pool_state *ps)
{
    if (!ps) return;
    free(ps->queue);
    for (int i = 0; i < ps->host_limit_n; i++) free(ps->host_limits[i].host);
    free(ps->host_limits);
    MTX_FREE(&ps->q_lock);
    MTX_FREE(&ps->done_lock);
    MTX_FREE(&ps->host_lock);
    free(ps);
}

/* 查找或创建 host 信号量槽 */
static int host_slot_index(pool_state *ps, const char *host)
{
    if (!host) return -1;
    for (int i = 0; i < ps->host_limit_n; i++)
        if (strcmp(ps->host_limits[i].host, host) == 0) return i;

    /* 创建新 slot */
    if (ps->host_limit_n == ps->host_limit_cap) {
        ps->host_limit_cap = ps->host_limit_cap ? ps->host_limit_cap * 2 : 16;
        void *p = realloc(ps->host_limits,
                          (size_t)ps->host_limit_cap * sizeof(ps->host_limits[0]));
        if (!p) return -1;
        ps->host_limits = (void *)p;
    }
    ps->host_limits[ps->host_limit_n].host  = bmk_strdup(host);
    ps->host_limits[ps->host_limit_n].slots = ps->opts->per_host_limit;
    return ps->host_limit_n++;
}

/* 获取 host 许可（阻塞直到有空位），返回 slot 索引 */
static int host_acquire(pool_state *ps, const char *host)
{
    MTX_LOCK(&ps->host_lock);
    int idx = host_slot_index(ps, host);
    if (idx < 0) { MTX_UNLOCK(&ps->host_lock); return -1; }

    while (ps->host_limits[idx].slots <= 0 && !ps->canceled) {
        /* 简化：自旋等待 50ms */
        MTX_UNLOCK(&ps->host_lock);
        SLEEP_MS(50);
        MTX_LOCK(&ps->host_lock);
    }
    if (ps->canceled) { MTX_UNLOCK(&ps->host_lock); return -1; }
    ps->host_limits[idx].slots--;
    MTX_UNLOCK(&ps->host_lock);
    return idx;
}

static void host_release(pool_state *ps, int slot_idx)
{
    if (slot_idx < 0) return;
    MTX_LOCK(&ps->host_lock);
    ps->host_limits[slot_idx].slots++;
    MTX_UNLOCK(&ps->host_lock);
}

/* worker 线程 */
static void worker_thread(pool_state *ps)
{
    for (;;) {
        MTX_LOCK(&ps->q_lock);
        while (ps->q_count == 0 && !ps->canceled)
            CV_WAIT(&ps->q_cond, &ps->q_lock);
        if (ps->q_count == 0 || ps->canceled) {
            MTX_UNLOCK(&ps->q_lock);
            break;
        }
        /* 取任务 */
        task_item t = ps->queue[ps->q_tail];
        ps->q_tail = (ps->q_tail + 1) % ps->q_cap;
        ps->q_count--;
        CV_SIGNAL(&ps->q_full);
        MTX_UNLOCK(&ps->q_lock);

        /* host 限流 */
        char *host = bmk_url_host(t.url);
        int host_idx = host_acquire(ps, host);

        /* 实际校验 */
        const char *proxy = NULL;
        char *proxy_owned = NULL;
        if (ps->opts->proxy_mode == BMK_PROXY_ALWAYS) {
            proxy_owned = bmk_proxy_next_endpoint(t.url);
            proxy = proxy_owned;
        }

        probe_result pr;
        probe_url(t.url, ps->opts, proxy, &pr);

        /* 失败重试 */
        for (int attempt = 0; attempt < ps->opts->retry; attempt++) {
            bool retryable = (pr.error_note != NULL) ||
                             (pr.status >= 500 && pr.status < 600);
            if (!retryable) break;
            SLEEP_MS(ps->opts->interval_ms);
            probe_url(t.url, ps->opts, proxy, &pr);
        }

        /* 自动代理回退 */
        if (ps->opts->proxy_mode == BMK_PROXY_AUTO &&
            (pr.error_note != NULL || pr.status == 403 || pr.status == 429)) {
            char *alt = bmk_proxy_next_endpoint(t.url);
            if (alt) {
                probe_result pr2;
                probe_url(t.url, ps->opts, alt, &pr2);
                if (pr2.error_note == NULL &&
                    !(pr2.status == 403 || pr2.status == 429))
                    pr = pr2;
                free(alt);
            }
        }
        free(proxy_owned);

        host_release(ps, host_idx);
        free(host);

        bmk_check_result res;
        bmk_check_judge(pr.status, pr.error_note, t.prev_valid, &res);

        /* 写库（bmk_bookmark_set_check_result 自行加锁） */
        int64_t now = bmk_now();
        bmk_bookmark_set_check_result(ps->db, t.id, res.valid,
                                      res.status_code, res.note, now);

        /* 进度 */
        MTX_LOCK(&ps->done_lock);
        int64_t done = ++ps->done_count;
        int64_t total = ps->total_count;
        MTX_UNLOCK(&ps->done_lock);

        /* 回调：返回 false 则取消 */
        if (ps->cb) {
            /* 临时构造书签仅用于回调展示（id/url/valid 足够） */
            bmk_bookmark b;
            memset(&b, 0, sizeof b);
            b.id = t.id;
            b.url = t.url;
            b.valid = t.prev_valid;
            if (!ps->cb(done, total, &b, &res, ps->cb_user)) {
                ps->canceled = true;
                ps->user_canceled = true;
            }
        }

        free(t.url);

        /* 间隔 + 抖动（仅在有后续任务时） */
        if (ps->done_count < total && ps->opts->interval_ms > 0) {
            int jitter = ps->opts->jitter_ms > 0
                         ? (rand() % (2 * ps->opts->jitter_ms + 1)) - ps->opts->jitter_ms
                         : 0;
            int delay = ps->opts->interval_ms + jitter;
            if (delay > 0) SLEEP_MS(delay);
        }
    }
}

bmk_err bmk_validate_run(bmk_db *db, const int64_t *ids, size_t n,
                         const bmk_validate_opts *opts,
                         bmk_validate_cb cb, void *user)
{
    if (!db || !ids) return BMK_ERR_INVALID_ARG;

    bmk_validate_opts defo;
    if (!opts) { bmk_validate_opts_init(&defo); opts = &defo; }

    int workers = opts->concurrency;
    if (workers < 1) workers = 1;
    if (workers > 10) workers = 10;

    pool_state *ps = (pool_state *)calloc(1, sizeof(*ps));
    if (!ps) return BMK_ERR_NOMEM;

    ps->db = db;
    ps->opts = opts;
    ps->cb = cb;
    ps->cb_user = user;
    ps->total_count = (int64_t)n;  /* Bug#3: 下文生产者循环结束后修正为实际入队数 */

    /* 队列容量 = workers * 4 */
    ps->q_cap = workers * 4;
    if ((int)n < ps->q_cap) ps->q_cap = (int)n;
    if (ps->q_cap < 4) ps->q_cap = 4;
    ps->queue = (task_item *)calloc((size_t)ps->q_cap, sizeof(task_item));
    if (!ps->queue) { pool_state_free(ps); return BMK_ERR_NOMEM; }

    MTX_INIT(&ps->q_lock);
    MTX_INIT(&ps->done_lock);
    MTX_INIT(&ps->host_lock);

    /* 启动 worker 线程 */
    bmk_thread *threads = (bmk_thread *)calloc((size_t)workers, sizeof(bmk_thread));
    if (!threads) { pool_state_free(ps); return BMK_ERR_NOMEM; }

    for (int i = 0; i < workers; i++) {
        if (!THREAD_CREATE(&threads[i], worker_thread, ps)) {
            /* 创建失败：标记取消，让已创建的线程退出 */
            ps->canceled = true;
            CV_BROADCAST(&ps->q_cond);
            for (int j = 0; j < i; j++) THREAD_JOIN(threads[j]);
            free(threads);
            pool_state_free(ps);
            return BMK_ERR_DB;
        }
    }

    int64_t qcount = 0;
    /* 生产者：逐个载入书签并入队 */
    for (size_t i = 0; i < n && !ps->canceled; i++) {
        bmk_bookmark *b = NULL;
        if (bmk_bookmark_get(db, ids[i], &b) != BMK_OK || !b) {
            BMK_WARN("validate: skip deleted/invalid bookmark id=%lld", (long long)ids[i]);
            continue;
        }

        /* 等待队列有空位 */
        MTX_LOCK(&ps->q_lock);
        while (ps->q_count >= ps->q_cap && !ps->canceled)
            CV_WAIT(&ps->q_full, &ps->q_lock);
        if (ps->canceled) {
            MTX_UNLOCK(&ps->q_lock);
            bmk_bookmark_free(b);
            free(b);
            break;
        }

        ps->queue[ps->q_head].url        = bmk_strdup(b->url);
        ps->queue[ps->q_head].id         = b->id;
        ps->queue[ps->q_head].prev_valid = b->valid;
        ps->q_head = (ps->q_head + 1) % ps->q_cap;
        ps->q_count++;
        CV_SIGNAL(&ps->q_cond);
        qcount++;
        MTX_UNLOCK(&ps->q_lock);

        bmk_bookmark_free(b);
        free(b);
    }
    /* Bug#3: 根据实际入队数修正 total_count */
    MTX_LOCK(&ps->done_lock);
    ps->total_count = qcount;
    MTX_UNLOCK(&ps->done_lock);

    /* 生产完毕：等待队列清空 */
    for (;;) {
        MTX_LOCK(&ps->q_lock);
        if (ps->q_count == 0 || ps->canceled) { MTX_UNLOCK(&ps->q_lock); break; }
        MTX_UNLOCK(&ps->q_lock);
        SLEEP_MS(10);
    }

    /* 通知 worker 退出 */
    ps->canceled = true;
    CV_BROADCAST(&ps->q_cond);

    /* 等待所有线程 */
    for (int i = 0; i < workers; i++) THREAD_JOIN(threads[i]);

    /* 取消时队列中可能残留未处理项（worker 直接退出不会消费），
     * 释放其 URL 避免泄漏。线程已全部退出，此处无竞争。 */
    while (ps->q_count > 0) {
        free(ps->queue[ps->q_tail].url);
        ps->q_tail = (ps->q_tail + 1) % ps->q_cap;
        ps->q_count--;
    }

    free(threads);

    bmk_err result = BMK_OK;
    if (ps->user_canceled) result = BMK_ERR_CANCELED;

    pool_state_free(ps);
    return result;
}
