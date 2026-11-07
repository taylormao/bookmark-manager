#include "bmk_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifdef _WIN32
#  include <windows.h>
static CRITICAL_SECTION g_lock;
static int g_lock_ready = 0;
static void log_lock(void)   { if (g_lock_ready) EnterCriticalSection(&g_lock); }
static void log_unlock(void) { if (g_lock_ready) LeaveCriticalSection(&g_lock); }
static void log_lock_init(void)
{
    if (!g_lock_ready) { InitializeCriticalSection(&g_lock); g_lock_ready = 1; }
}
#else
#  include <pthread.h>
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static void log_lock(void)      { pthread_mutex_lock(&g_lock); }
static void log_unlock(void)    { pthread_mutex_unlock(&g_lock); }
static void log_lock_init(void) { }
#endif

#define LOG_MAX_BYTES (5 * 1024 * 1024)  /* 5MB（需求 2.5） */
#define LOG_MAX_FILES 3

static FILE *g_fp = NULL;
static char *g_path = NULL;
static bmk_log_level g_level = BMK_LOG_INFO;

static const char *level_name(bmk_log_level l)
{
    switch (l) {
    case BMK_LOG_DEBUG: return "DEBUG";
    case BMK_LOG_INFO:  return "INFO";
    case BMK_LOG_WARN:  return "WARN";
    case BMK_LOG_ERROR: return "ERROR";
    }
    return "?";
}

bmk_err bmk_log_init(const char *log_dir, bmk_log_level level)
{
    log_lock_init();
    log_lock();
    g_level = level;
    if (g_fp) { fclose(g_fp); g_fp = NULL; }
    free(g_path);
    g_path = NULL;

    if (log_dir && *log_dir) {
        if (bmk_mkdirs(log_dir) != BMK_OK) { log_unlock(); return BMK_ERR_IO; }
        g_path = bmk_path_join(log_dir, "app.log");
        if (!g_path) { log_unlock(); return BMK_ERR_NOMEM; }
        g_fp = fopen(g_path, "ab");
        if (!g_fp) { log_unlock(); return BMK_ERR_IO; }
    }
    log_unlock();
    return BMK_OK;
}

void bmk_log_shutdown(void)
{
    log_lock();
    if (g_fp) { fclose(g_fp); g_fp = NULL; }
    free(g_path);
    g_path = NULL;
    log_unlock();
}

void bmk_log_set_level(bmk_log_level level) { g_level = level; }
bmk_log_level bmk_log_get_level(void)       { return g_level; }

/* 滚动：app.log -> app.log.1 -> app.log.2 ...（调用时须持锁） */
static void rotate_if_needed(void)
{
    if (!g_fp || !g_path) return;
    long sz = ftell(g_fp);
    if (sz < LOG_MAX_BYTES) return;

    fclose(g_fp);
    g_fp = NULL;

    char from[1024], to[1024];
    /* 删除最老的一份 */
    snprintf(to, sizeof(to), "%s.%d", g_path, LOG_MAX_FILES - 1);
    remove(to);
    for (int i = LOG_MAX_FILES - 2; i >= 1; i--) {
        snprintf(from, sizeof(from), "%s.%d", g_path, i);
        snprintf(to, sizeof(to), "%s.%d", g_path, i + 1);
        rename(from, to);
    }
    snprintf(to, sizeof(to), "%s.1", g_path);
    rename(g_path, to);

    g_fp = fopen(g_path, "ab");
}

void bmk_logf(bmk_log_level level, const char *fmt, ...)
{
    if (level < g_level) return;

    char tbuf[32];
    time_t t = time(NULL);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &tmv);

    log_lock();
    FILE *out = g_fp ? g_fp : stderr;
    fprintf(out, "[%s] %-5s ", tbuf, level_name(level));
    va_list ap;
    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);
    fputc('\n', out);
    fflush(out);
    if (g_fp) rotate_if_needed();
    log_unlock();
}

char *bmk_log_redact(const char *secret)
{
    if (!secret) return bmk_strdup("(null)");
    size_t n = strlen(secret);
    if (n == 0) return bmk_strdup("");
    if (n <= 4) {
        char *p = (char *)malloc(n + 1);
        if (!p) return NULL;
        memset(p, '*', n);
        p[n] = '\0';
        return p;
    }
    char *p = (char *)malloc(n + 1);
    if (!p) return NULL;
    p[0] = secret[0];
    p[1] = secret[1];
    memset(p + 2, '*', n - 4);
    p[n - 2] = secret[n - 2];
    p[n - 1] = secret[n - 1];
    p[n] = '\0';
    return p;
}
