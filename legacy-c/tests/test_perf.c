/*
 * test_perf.c - 10,000 条数据规模基准（需求 §5.1）
 *
 * 自动化可测部分：
 *   - 逐条插入 10,000 条（需求无阈值，记录耗时供回归对比）
 *   - 关键词筛选 + 标题排序 < 200ms              （§5.1 第 1 条）
 *   - 导入 10,000 条 Netscape HTML < 10s         （§5.1 第 3 条）
 * 启动时间与 UI 滚动流畅度需人工/界面观察，不在本用例范围。
 */
#include "test_util.h"
#include "bmk_db.h"
#include "bmk_netscape.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <windows.h>
#  include <process.h>
static double now_ms(void) { return (double)GetTickCount64(); }
static int perf_pid(void) { return (int)_getpid(); }
#else
#  include <unistd.h>
#  include <time.h>
static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}
static int perf_pid(void) { return (int)getpid(); }
#endif

#define PERF_N  10000
#define PERF_BUF 1024

static void perf_temp_dir(char *buf, size_t sz, const char *tag)
{
    const char *base = getenv("TEMP");
    if (!base || !*base) base = "/tmp";
#ifdef _WIN32
    snprintf(buf, sz, "%s\\bmk_perf_%s_%d", base, tag, (int)getpid());
#else
    snprintf(buf, sz, "%s/bmk_perf_%s_%d", base, tag, (int)getpid());
#endif
}

static void perf_db_path(char *buf, size_t sz, const char *dir)
{
#ifdef _WIN32
    snprintf(buf, sz, "%s\\bookmarks.db", dir);
#else
    snprintf(buf, sz, "%s/bookmarks.db", dir);
#endif
}

static void perf_cleanup(const char *dir)
{
    char p[PERF_BUF];
#ifdef _WIN32
    snprintf(p, sizeof p, "%s\\bookmarks.db", dir);   remove(p);
    snprintf(p, sizeof p, "%s\\bookmarks.db-wal", dir); remove(p);
    snprintf(p, sizeof p, "%s\\bookmarks.db-shm", dir); remove(p);
#else
    snprintf(p, sizeof p, "%s/bookmarks.db", dir);     remove(p);
    snprintf(p, sizeof p, "%s/bookmarks.db-wal", dir); remove(p);
    snprintf(p, sizeof p, "%s/bookmarks.db-shm", dir); remove(p);
#endif
}

/* 逐条插入 PERF_N 条 + 在其上验证筛选排序阈值（§5.1 第 1 条） */
static void test_insert_and_query(void)
{
    char dir[PERF_BUF], dbp[PERF_BUF];
    perf_temp_dir(dir, sizeof dir, "ins");
    perf_db_path(dbp, sizeof dbp, dir);
    bmk_mkdirs(dir);


    bmk_db *db = NULL;
    CHECK_EQ(bmk_db_open(dbp, &db), BMK_OK, "打开基准库");
    if (!db) { perf_cleanup(dir); return; }

    double t0 = now_ms();
    for (int i = 0; i < PERF_N; i++) {
        bmk_bookmark b;
        memset(&b, 0, sizeof b);
        char url[128], title[128];
        snprintf(url,   sizeof url,   "https://site%d.example.com/page%d", i, i);
        snprintf(title, sizeof title, "Site %d", i);
        b.url   = bmk_strdup(url);
        b.title = bmk_strdup(title);
        b.valid = BMK_VALID_UNKNOWN;
        b.status_code = -1;
        bmk_err e = bmk_bookmark_insert(db, &b);
        if (e != BMK_OK) {
            CHECK(0, "插入第 %d 条失败（%s）", i, bmk_err_str(e));
            break;
        }
        bmk_bookmark_free(&b);
    }
    double ins_ms = now_ms() - t0;
    printf("[基准] 逐条插入 %d 条: %.0f ms（%.1f 条/s）\n",
           PERF_N, ins_ms, ins_ms > 0 ? PERF_N * 1000.0 / ins_ms : 0);
    CHECK(ins_ms < 60000.0, "逐条插入 10k 应远低于 60s，实际 %.0f ms", ins_ms);

    /* §5.1 第 1 条：排序与筛选响应 < 200ms */
    bmk_filter f;
    bmk_filter_init(&f);
    char kw[] = "site5";
    f.keyword = kw;
    double q0 = now_ms();
    bmk_bookmark **list = NULL;
    size_t n = 0;
    bmk_err e = bmk_bookmark_query(db, &f, BMK_SORT_TITLE, false, &list, &n);
    double q_ms = now_ms() - q0;
    CHECK_EQ(e, BMK_OK, "筛选+排序查询执行");
    CHECK(q_ms < 200.0, "10k 条筛选+排序 < 200ms（§5.1.1），实际 %.0f ms", q_ms);
    CHECK(n > 0, "关键词筛选有结果");
    bmk_bookmark_list_free(list, n);

    /* 无筛选、按标题排序全量列出（列表页最重场景） */
    q0 = now_ms();
    list = NULL; n = 0;
    e = bmk_bookmark_query(db, NULL, BMK_SORT_TITLE, true, &list, &n);
    q_ms = now_ms() - q0;
    CHECK_EQ(e, BMK_OK, "全量查询执行");
    CHECK(q_ms < 200.0, "10k 条全量排序 < 200ms（§5.1.1），实际 %.0f ms", q_ms);
    bmk_bookmark_list_free(list, n);

    bmk_db_close(db);
    perf_cleanup(dir);
}

/* 导入 10,000 条 Netscape HTML（§5.1 第 3 条：< 10s） */
static void test_import_10k(void)
{
    char dir[PERF_BUF], dbp[PERF_BUF];
    perf_temp_dir(dir, sizeof dir, "imp");
    perf_db_path(dbp, sizeof dbp, dir);
    bmk_mkdirs(dir);

    /* 生成 10k 条标准 HTML（bmk_buf：测试不链接 glib，用项目自带缓冲） */
    bmk_buf s;
    bmk_buf_init(&s);
    bmk_buf_append_str(&s,
        "<!DOCTYPE NETSCAPE-Bookmark-file-1>\n"
        "<META HTTP-EQUIV=\"Content-Type\" CONTENT=\"text/html; charset=UTF-8\">\n"
        "<TITLE>Bookmarks</TITLE>\n<H1>Bookmarks</H1>\n<DL><p>\n");
    for (int i = 0; i < PERF_N; i++) {
        bmk_buf_printf(&s,
            "    <DT><A HREF=\"https://imp%d.example.com/page%d\" "
            "ADD_DATE=\"1700000000\">Import %d</A>\n", i, i, i);
    }
    bmk_buf_append_str(&s, "</DL><p>\n");

    bmk_db *db = NULL;
    CHECK_EQ(bmk_db_open(dbp, &db), BMK_OK, "打开导入基准库");
    if (!db) {
        bmk_buf_free(&s);
        perf_cleanup(dir);
        return;
    }

    bmk_import_opts o;
    bmk_import_opts_init(&o);
    bmk_import_report rep;
    memset(&rep, 0, sizeof rep);

    double t0 = now_ms();
    bmk_err e = bmk_import_html_buffer(db, s.data, s.len, &o, &rep);
    double ms = now_ms() - t0;
    printf("[基准] 导入 %d 条: %.0f ms\n", PERF_N, ms);
    CHECK_EQ(e, BMK_OK, "导入执行成功");
    CHECK_EQ((long long)rep.added, (long long)PERF_N, "10k 条全部导入");
    CHECK(ms < 10000.0, "导入 10k < 10s（§5.1.3），实际 %.0f ms", ms);

    bmk_db_close(db);
    bmk_buf_free(&s);
    perf_cleanup(dir);
}

int main(void)
{
    test_insert_and_query();
    test_import_10k();
    return test_report("性能基准（10k，§5.1）");
}
