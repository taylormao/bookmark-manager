/*
 * test_netscape.c - 导入导出往返测试（FR-07 / FR-08 / 附录 A）
 */
#include "test_util.h"
#include "bmk_netscape.h"
#include "bmk_db.h"

#include <stdio.h>

static const char *TMP_DB = "test_ns.sqlite";

static bmk_db *fresh_db(void)
{
    remove(TMP_DB);
    remove("test_ns.sqlite-wal");
    remove("test_ns.sqlite-shm");
    bmk_db *db = NULL;
    if (bmk_db_open(TMP_DB, &db) != BMK_OK) { fprintf(stderr, "建库失败\n"); exit(1); }
    return db;
}

static void drop_db(bmk_db *db)
{
    bmk_db_close(db);
    remove(TMP_DB);
    remove("test_ns.sqlite-wal");
    remove("test_ns.sqlite-shm");
}

static const char *SAMPLE =
"<!DOCTYPE NETSCAPE-Bookmark-file-1>\n"
"<META HTTP-EQUIV=\"Content-Type\" CONTENT=\"text/html; charset=UTF-8\">\n"
"<TITLE>Bookmarks</TITLE>\n"
"<H1>Bookmarks</H1>\n"
"<DL><p>\n"
"    <DT><H3 ADD_DATE=\"1710000000\">技术</H3>\n"
"    <DL><p>\n"
"        <DT><A HREF=\"https://github.com/\" ADD_DATE=\"1710000001\" TAGS=\"code,git\">GitHub</A>\n"
"        <DT><H3>前端</H3>\n"
"        <DL><p>\n"
"            <DT><A HREF=\"https://developer.mozilla.org/\" ADD_DATE=\"1710000002\">MDN &amp; Docs</A>\n"
"        </DL><p>\n"
"    </DL><p>\n"
"    <DT><A HREF=\"https://example.com/?a=1\" ADD_DATE=\"1710000003\">示例 &lt;站点&gt;</A>\n"
"    <DT><A HREF=\"javascript:alert(1)\">脚本书签</A>\n"
"</DL><p>\n";

static size_t total_count(bmk_db *db)
{
    bmk_filter f;
    bmk_filter_init(&f);
    bmk_bookmark **list = NULL;
    size_t n = 0;
    bmk_bookmark_query(db, &f, BMK_SORT_CREATED_AT, true, &list, &n);
    bmk_bookmark_list_free(list, n);
    bmk_filter_free(&f);
    return n;
}

static void test_import_basic(void)
{
    bmk_db *db = fresh_db();

    bmk_import_opts o;
    bmk_import_opts_init(&o);
    bmk_import_report r;
    memset(&r, 0, sizeof r);

    CHECK_EQ(bmk_import_html_buffer(db, SAMPLE, strlen(SAMPLE), &o, &r), BMK_OK,
             "导入应成功");
    CHECK_EQ(r.added, 4, "应导入 4 条（含 javascript: 书签）");
    CHECK_EQ(r.skipped, 0, "首次导入无跳过");
    bmk_import_report_free(&r);

    CHECK_EQ(total_count(db), 4, "库内条数");

    /* 分类层级还原（附录 A：H3 建立分类） */
    int64_t tech = bmk_category_find(db, "技术", 0);
    CHECK(tech > 0, "应建立分类 技术");
    int64_t fe = bmk_category_find(db, "前端", tech);
    CHECK(fe > 0, "应建立子分类 前端");
    CHECK_EQ(bmk_category_count_recursive(db, tech), 2, "技术分类含子孙 2 条");

    /* HTML 实体反转义 */
    bmk_filter f;
    bmk_filter_init(&f);
    f.keyword = bmk_strdup("MDN");
    bmk_bookmark **list = NULL;
    size_t n = 0;
    bmk_bookmark_query(db, &f, BMK_SORT_CREATED_AT, true, &list, &n);
    CHECK_EQ(n, 1, "找到 MDN");
    if (n) CHECK_STR(list[0]->title, "MDN & Docs", "实体应被反转义");
    bmk_bookmark_list_free(list, n);
    bmk_filter_free(&f);

    /* TAGS 属性解析 */
    bmk_filter_init(&f);
    f.keyword = bmk_strdup("GitHub");
    bmk_bookmark_query(db, &f, BMK_SORT_CREATED_AT, true, &list, &n);
    if (n) CHECK_EQ(list[0]->tag_count, 2, "TAGS 应解析为 2 个标签");
    bmk_bookmark_list_free(list, n);
    bmk_filter_free(&f);

    /* 非 http(s) 的 url_key（附录 B 第 9 条） */
    bmk_filter_init(&f);
    f.keyword = bmk_strdup("脚本书签");
    bmk_bookmark_query(db, &f, BMK_SORT_CREATED_AT, true, &list, &n);
    CHECK_EQ(n, 1, "javascript: 书签应被导入");
    if (n) CHECK_STR(list[0]->url_key, "javascript:alert(1)", "非 http(s) 原样为 key");
    bmk_bookmark_list_free(list, n);
    bmk_filter_free(&f);

    drop_db(db);
}

static void test_import_duplicate_policy(void)
{
    /* 默认跳过重复 */
    bmk_db *db = fresh_db();
    bmk_import_opts o;
    bmk_import_opts_init(&o);
    bmk_import_report r;

    memset(&r, 0, sizeof r);
    bmk_import_html_buffer(db, SAMPLE, strlen(SAMPLE), &o, &r);
    bmk_import_report_free(&r);

    memset(&r, 0, sizeof r);
    CHECK_EQ(bmk_import_html_buffer(db, SAMPLE, strlen(SAMPLE), &o, &r), BMK_OK,
             "二次导入应成功");
    CHECK_EQ(r.added, 0, "重复应全部跳过");
    CHECK_EQ(r.skipped, 4, "跳过计数");
    CHECK_EQ(total_count(db), 4, "库内条数不变");
    bmk_import_report_free(&r);
    drop_db(db);

    /* 导入并标记，不合并 */
    db = fresh_db();
    bmk_import_opts_init(&o);
    memset(&r, 0, sizeof r);
    bmk_import_html_buffer(db, SAMPLE, strlen(SAMPLE), &o, &r);
    bmk_import_report_free(&r);

    o.dup_policy = BMK_DUP_IMPORT;
    memset(&r, 0, sizeof r);
    bmk_import_html_buffer(db, SAMPLE, strlen(SAMPLE), &o, &r);
    CHECK_EQ(r.added, 4, "策略为导入时不跳过");
    CHECK_EQ(total_count(db), 8, "库内条数翻倍");
    bmk_import_report_free(&r);
    drop_db(db);
}

static bool cancel_at_two(int64_t done, int64_t total, void *user)
{
    (void)total; (void)user;
    return done < 2;
}

static void test_import_cancel_rollback(void)
{
    bmk_db *db = fresh_db();
    bmk_import_opts o;
    bmk_import_opts_init(&o);
    o.progress = cancel_at_two;

    bmk_import_report r;
    memset(&r, 0, sizeof r);
    bmk_err e = bmk_import_html_buffer(db, SAMPLE, strlen(SAMPLE), &o, &r);
    CHECK_EQ(e, BMK_ERR_CANCELED, "取消应返回 canceled");
    CHECK_EQ(total_count(db), 0, "取消导入必须整体回滚（FR-07 第 4 条）");
    bmk_import_report_free(&r);

    drop_db(db);
}

static void test_round_trip(void)
{
    bmk_db *db1 = fresh_db();
    bmk_import_opts o;
    bmk_import_opts_init(&o);
    bmk_import_report r;
    memset(&r, 0, sizeof r);
    bmk_import_html_buffer(db1, SAMPLE, strlen(SAMPLE), &o, &r);
    bmk_import_report_free(&r);

    char *html = NULL;
    CHECK_EQ(bmk_export_html_string(db1, NULL, 0, &html), BMK_OK, "导出应成功");
    CHECK(html && strstr(html, "<!DOCTYPE NETSCAPE-Bookmark-file-1>"),
          "导出应含标准 DOCTYPE");
    CHECK(html && strstr(html, "&amp;"), "导出应转义 &");
    CHECK(html && strstr(html, "javascript:alert(1)"), "导出应含非 http 书签");
    bmk_db_close(db1);
    remove(TMP_DB); remove("test_ns.sqlite-wal"); remove("test_ns.sqlite-shm");

    /* 往返：导出结果再导入，条数与分类结构应一致 */
    bmk_db *db2 = fresh_db();
    memset(&r, 0, sizeof r);
    CHECK_EQ(bmk_import_html_buffer(db2, html, strlen(html), &o, &r), BMK_OK,
             "往返导入应成功");
    CHECK_EQ(r.added, 4, "往返后条数一致");
    bmk_import_report_free(&r);

    int64_t tech = bmk_category_find(db2, "技术", 0);
    CHECK(tech > 0, "往返后分类保留");
    CHECK(bmk_category_find(db2, "前端", tech) > 0, "往返后子分类保留");

    free(html);
    drop_db(db2);
}

static void test_escape(void)
{
    char *s = bmk_html_escape("a<b>&\"c\"");
    CHECK_STR(s, "a&lt;b&gt;&amp;&quot;c&quot;", "HTML 转义");
    free(s);

    s = bmk_html_unescape("a&lt;b&gt;&amp;&quot;c&quot;&#39;&nbsp;");
    CHECK(s && strstr(s, "a<b>&\"c\"'") == s, "HTML 反转义");
    free(s);

    /* 未知实体保持原样 */
    s = bmk_html_unescape("100&percnt;&unknown;");
    CHECK(s && strstr(s, "&unknown;") != NULL, "未知实体保留原样");
    free(s);
}

static void test_utf8_check(void)
{
    CHECK(bmk_is_valid_utf8("hello", 5), "ASCII 是合法 UTF-8");
    CHECK(bmk_is_valid_utf8("中文", strlen("中文")), "中文是合法 UTF-8");
    CHECK(!bmk_is_valid_utf8("\xC3", 1), "截断的多字节序列非法");
    CHECK(!bmk_is_valid_utf8("\xFF\xFE", 2), "非法首字节");
    CHECK(bmk_is_valid_utf8("", 0), "空串合法");
}

int main(void)
{
    test_import_basic();
    test_import_duplicate_policy();
    test_import_cancel_rollback();
    test_round_trip();
    test_escape();
    test_utf8_check();
    return test_report("导入导出（附录 A）");
}
