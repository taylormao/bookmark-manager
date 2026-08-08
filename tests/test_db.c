/*
 * test_db.c - 数据层、排序（FR-09）与筛选（FR-12/附录 E）单元测试
 */
#include "test_util.h"
#include "bmk_db.h"
#include "bmk_settings.h"

#include <stdio.h>

static const char *TMP_DB = "test_db.sqlite";

static bmk_db *fresh_db(void)
{
    remove(TMP_DB);
    remove("test_db.sqlite-wal");
    remove("test_db.sqlite-shm");
    bmk_db *db = NULL;
    if (bmk_db_open(TMP_DB, &db) != BMK_OK) {
        fprintf(stderr, "无法创建测试数据库\n");
        exit(1);
    }
    return db;
}

static void drop_db(bmk_db *db)
{
    bmk_db_close(db);
    remove(TMP_DB);
    remove("test_db.sqlite-wal");
    remove("test_db.sqlite-shm");
}

static int64_t add(bmk_db *db, const char *title, const char *url,
                   int64_t cat, int valid, int status, int64_t created)
{
    bmk_bookmark b;
    memset(&b, 0, sizeof b);
    b.title = bmk_strdup(title);
    b.url   = bmk_strdup(url);
    b.category_id = cat;
    b.valid = valid;
    b.status_code = status;
    b.created_at = created;
    b.updated_at = created;
    bmk_err e = bmk_bookmark_insert(db, &b);
    int64_t id = (e == BMK_OK) ? b.id : 0;
    bmk_bookmark_free(&b);
    CHECK(id > 0, "插入书签 %s 应成功（%s）", url, bmk_err_str(e));
    return id;
}

/* ---------------- 分类（FR-10） ---------------- */

static void test_categories(void)
{
    bmk_db *db = fresh_db();

    int64_t root = 0, child = 0, grand = 0;
    CHECK_EQ(bmk_category_create(db, "技术", 0, &root), BMK_OK, "创建根分类");
    CHECK_EQ(bmk_category_create(db, "前端", root, &child), BMK_OK, "创建子分类");
    CHECK_EQ(bmk_category_create(db, "CSS", child, &grand), BMK_OK, "创建孙分类");

    /* 同级同名不区分大小写唯一（FR-10 第 3 条） */
    int64_t dup = 0;
    CHECK_EQ(bmk_category_create(db, "前端", root, &dup), BMK_ERR_CONFLICT,
             "同级同名应冲突");
    CHECK_EQ(bmk_category_create(db, "前端", root, &dup), BMK_ERR_CONFLICT,
             "同级同名（大小写变体）应冲突");

    /* 不同父级允许同名 */
    int64_t other = 0;
    CHECK_EQ(bmk_category_create(db, "前端", 0, &other), BMK_OK, "不同父级允许同名");

    /* 递归计数（FR-10 第 5 条） */
    add(db, "a", "https://a.com/1", root,  BMK_VALID_UNKNOWN, -1, 100);
    add(db, "b", "https://b.com/1", child, BMK_VALID_UNKNOWN, -1, 101);
    add(db, "c", "https://c.com/1", grand, BMK_VALID_UNKNOWN, -1, 102);
    CHECK_EQ(bmk_category_count_recursive(db, root), 3, "根分类含子孙计数");
    CHECK_EQ(bmk_category_count_recursive(db, child), 2, "子分类含子孙计数");
    CHECK_EQ(bmk_category_count_recursive(db, grand), 1, "叶分类计数");

    /* 禁止移动到自身或子孙 */
    CHECK(bmk_category_is_descendant(db, grand, root), "grand 应是 root 的子孙");
    CHECK(!bmk_category_is_descendant(db, root, grand), "root 不是 grand 的子孙");
    CHECK(bmk_category_move(db, root, grand) != BMK_OK, "禁止移动到自己的子孙");
    CHECK(bmk_category_move(db, root, root) != BMK_OK, "禁止移动到自身");

    /* 删除父分类：子分类递归消失，网址归入未分类（FR-10 第 4 条） */
    CHECK_EQ(bmk_category_affected_bookmarks(db, root), 3, "删除前受影响网址数");
    CHECK_EQ(bmk_category_delete(db, root), BMK_OK, "删除父分类");

    bmk_category **cats = NULL;
    size_t n = 0;
    bmk_category_list(db, &cats, &n);
    for (size_t i = 0; i < n; i++)
        CHECK(cats[i]->id != child && cats[i]->id != grand, "子孙分类应被递归删除");
    bmk_category_list_free(cats, n);

    bmk_filter f;
    bmk_filter_init(&f);
    f.category_id = BMK_CATEGORY_UNCATEGORIZED;
    bmk_bookmark **list = NULL;
    size_t ln = 0;
    bmk_bookmark_query(db, &f, BMK_SORT_CREATED_AT, true, &list, &ln);
    CHECK_EQ(ln, 3, "原分类下网址应归入未分类");
    bmk_bookmark_list_free(list, ln);
    bmk_filter_free(&f);

    drop_db(db);
}

/* ---------------- 标签（FR-11） ---------------- */

static void test_tags(void)
{
    bmk_db *db = fresh_db();

    int64_t t1 = 0, t2 = 0;
    bmk_tag_get_or_create(db, "Dev", &t1);
    bmk_tag_get_or_create(db, "dev", &t2);
    CHECK_EQ(t1, t2, "大小写变体应视为同一标签");

    /* 去首尾空格 */
    int64_t t3 = 0;
    bmk_tag_get_or_create(db, "  DEV  ", &t3);
    CHECK_EQ(t3, t1, "去空格后应命中同一标签");

    bmk_tag **tags = NULL;
    size_t n = 0;
    bmk_tag_list(db, &tags, &n);
    CHECK_EQ(n, 1, "应只有一个标签");
    if (n) CHECK_STR(tags[0]->name, "Dev", "保留先创建者的大小写形式");
    bmk_tag_list_free(tags, n);

    int64_t id = add(db, "a", "https://a.com/1", 0, BMK_VALID_UNKNOWN, -1, 100);
    const char *tv[] = { "Dev", "C语言" };
    CHECK_EQ(bmk_bookmark_set_tags(db, id, tv, 2), BMK_OK, "设置标签");

    bmk_bookmark *b = NULL;
    bmk_bookmark_get(db, id, &b);
    CHECK_EQ(b ? b->tag_count : 0, 2, "读回两个标签");
    /* 需求 3.3：标签变更也刷新 updated_at（创建时间为 100，刷新后应远大于它） */
    CHECK(b && b->updated_at > 100, "设置标签后应刷新 updated_at");
    if (b) { bmk_bookmark_free(b); free(b); }

    /* 合并：Dev 并入 C语言 */
    int64_t tc = 0;
    bmk_tag_get_or_create(db, "C语言", &tc);
    CHECK_EQ(bmk_tag_merge(db, t1, tc), BMK_OK, "标签合并");

    bmk_bookmark_get(db, id, &b);
    CHECK_EQ(b ? b->tag_count : 0, 1, "合并后只剩一个标签");
    if (b) { bmk_bookmark_free(b); free(b); }

    /* 删除标签只解除关联 */
    CHECK_EQ(bmk_tag_delete(db, tc), BMK_OK, "删除标签");
    bmk_bookmark_get(db, id, &b);
    CHECK(b != NULL, "删除标签不应删除网址");
    if (b) { CHECK_EQ(b->tag_count, 0, "关联已解除"); bmk_bookmark_free(b); free(b); }

    drop_db(db);
}

/* ---------------- 排序（FR-09） ---------------- */

static void query_ids(bmk_db *db, bmk_sort_key k, bool asc, int64_t *out, size_t max,
                      size_t *out_n)
{
    bmk_filter f;
    bmk_filter_init(&f);
    bmk_bookmark **list = NULL;
    size_t n = 0;
    bmk_bookmark_query(db, &f, k, asc, &list, &n);
    bmk_filter_free(&f);
    *out_n = n < max ? n : max;
    for (size_t i = 0; i < *out_n; i++) out[i] = list[i]->id;
    bmk_bookmark_list_free(list, n);
}

static void test_sort(void)
{
    bmk_db *db = fresh_db();

    /* valid: 1=GOOD 0=BAD -1=NULL；status: -1 表示 NULL */
    int64_t good = add(db, "banana", "https://b.com/", 0, BMK_VALID_GOOD,    200, 300);
    int64_t bad  = add(db, "Apple",  "https://a.com/", 0, BMK_VALID_BAD,     404, 100);
    int64_t unk  = add(db, "cherry", "https://c.com/", 0, BMK_VALID_UNKNOWN,  -1, 200);

    int64_t ids[8];
    size_t n = 0;

    /* 标题 NOCASE 升序：Apple < banana < cherry */
    query_ids(db, BMK_SORT_TITLE, true, ids, 8, &n);
    CHECK_EQ(n, 3, "标题排序条数");
    CHECK_EQ(ids[0], bad,  "标题升序第 1 条应为 Apple（NOCASE）");
    CHECK_EQ(ids[1], good, "标题升序第 2 条应为 banana");
    CHECK_EQ(ids[2], unk,  "标题升序第 3 条应为 cherry");

    /* 添加时间升序 */
    query_ids(db, BMK_SORT_CREATED_AT, true, ids, 8, &n);
    CHECK_EQ(ids[0], bad,  "时间升序最早");
    CHECK_EQ(ids[2], good, "时间升序最晚");

    /* 有效性升序：0 -> NULL -> 1 */
    query_ids(db, BMK_SORT_VALID, true, ids, 8, &n);
    CHECK_EQ(ids[0], bad,  "有效性升序: 0 在最前");
    CHECK_EQ(ids[1], unk,  "有效性升序: NULL 居中");
    CHECK_EQ(ids[2], good, "有效性升序: 1 在最后");

    /* 有效性降序：1 -> NULL -> 0 */
    query_ids(db, BMK_SORT_VALID, false, ids, 8, &n);
    CHECK_EQ(ids[0], good, "有效性降序: 1 在最前");
    CHECK_EQ(ids[1], unk,  "有效性降序: NULL 居中");
    CHECK_EQ(ids[2], bad,  "有效性降序: 0 在最后");

    /* 状态码：NULL 在升序与降序中都排最后 */
    query_ids(db, BMK_SORT_STATUS_CODE, true, ids, 8, &n);
    CHECK_EQ(ids[0], good, "状态码升序 200 在前");
    CHECK_EQ(ids[1], bad,  "状态码升序 404 居中");
    CHECK_EQ(ids[2], unk,  "状态码升序 NULL 最后");

    query_ids(db, BMK_SORT_STATUS_CODE, false, ids, 8, &n);
    CHECK_EQ(ids[0], bad,  "状态码降序 404 在前");
    CHECK_EQ(ids[1], good, "状态码降序 200 居中");
    CHECK_EQ(ids[2], unk,  "状态码降序 NULL 仍排最后");

    /* 最近校验时间：均为 NULL 时按 id ASC 稳定 */
    query_ids(db, BMK_SORT_LAST_CHECKED, true, ids, 8, &n);
    CHECK(ids[0] < ids[1] && ids[1] < ids[2], "全 NULL 时按 id ASC 稳定");

    /* 分类：NULL（未分类）排最后 */
    int64_t cat = 0;
    bmk_category_create(db, "分类A", 0, &cat);
    int64_t inCat = add(db, "dog", "https://d.com/", cat, BMK_VALID_UNKNOWN, -1, 400);
    query_ids(db, BMK_SORT_CATEGORY, true, ids, 8, &n);
    CHECK_EQ(n, 4, "分类排序条数");
    CHECK_EQ(ids[0], inCat, "有分类的排在前，未分类排最后");

    drop_db(db);
}

/* ---------------- 筛选（FR-12 / 附录 E） ---------------- */

static size_t count_with(bmk_db *db, bmk_filter *f)
{
    bmk_bookmark **list = NULL;
    size_t n = 0;
    bmk_bookmark_query(db, f, BMK_SORT_CREATED_AT, true, &list, &n);
    bmk_bookmark_list_free(list, n);
    return n;
}

static void test_filter(void)
{
    bmk_db *db = fresh_db();

    int64_t cat = 0, sub = 0;
    bmk_category_create(db, "技术", 0, &cat);
    bmk_category_create(db, "前端", cat, &sub);

    int64_t a = add(db, "GitHub 首页", "https://github.com/",      cat, BMK_VALID_GOOD, 200, 100);
    int64_t b = add(db, "MDN 文档",   "https://developer.mozilla.org/zh-CN/", sub, BMK_VALID_BAD, 404, 200);
    int64_t c = add(db, "示例站点",   "https://example.com/blog",   0,  BMK_VALID_UNKNOWN, -1, 300);
    (void)a;

    /* 被屏蔽（403 但保持原有 valid） */
    int64_t d = add(db, "被屏蔽站",   "https://blocked.com/",       0,  BMK_VALID_UNKNOWN, -1, 400);
    bmk_bookmark_set_check_result(db, d, BMK_VALID_UNKNOWN, 403, BMK_NOTE_BLOCKED, 500);

    const char *tv[] = { "文档" };
    bmk_bookmark_set_tags(db, b, tv, 1);

    bmk_filter f;

    /* 关键词：title 或 url 子串，不区分大小写 */
    bmk_filter_init(&f);
    f.keyword = bmk_strdup("github");
    CHECK_EQ(count_with(db, &f), 1, "关键词匹配 url");
    bmk_filter_free(&f);

    bmk_filter_init(&f);
    f.keyword = bmk_strdup("文档");
    CHECK_EQ(count_with(db, &f), 1, "关键词匹配中文标题");
    bmk_filter_free(&f);

    /* 网址包含（独立维度，与关键词 AND） */
    bmk_filter_init(&f);
    f.keyword = bmk_strdup("示例");
    f.url_contains = bmk_strdup("github");
    CHECK_EQ(count_with(db, &f), 0, "维度之间为 AND");
    bmk_filter_free(&f);

    /* 域名 */
    bmk_filter_init(&f);
    f.domain = bmk_strdup("example.com");
    CHECK_EQ(count_with(db, &f), 1, "域名筛选");
    bmk_filter_free(&f);

    /* 分类：包含全部子孙 */
    bmk_filter_init(&f);
    f.category_id = cat;
    CHECK_EQ(count_with(db, &f), 2, "分类筛选应包含子孙");
    bmk_filter_free(&f);

    bmk_filter_init(&f);
    f.category_id = BMK_CATEGORY_UNCATEGORIZED;
    CHECK_EQ(count_with(db, &f), 2, "未分类筛选");
    bmk_filter_free(&f);

    /* 有效性 */
    bmk_filter_init(&f); f.valid_filter = BMK_VF_VALID;
    CHECK_EQ(count_with(db, &f), 1, "有效筛选");
    bmk_filter_free(&f);

    bmk_filter_init(&f); f.valid_filter = BMK_VF_INVALID;
    CHECK_EQ(count_with(db, &f), 1, "无效筛选");
    bmk_filter_free(&f);

    bmk_filter_init(&f); f.valid_filter = BMK_VF_UNCHECKED;
    CHECK_EQ(count_with(db, &f), 2, "未校验筛选（含 403 保持 NULL 的条目）");
    bmk_filter_free(&f);

    bmk_filter_init(&f); f.valid_filter = BMK_VF_BLOCKED;
    CHECK_EQ(count_with(db, &f), 1, "待复测筛选");
    bmk_filter_free(&f);

    /* 状态码区间 400-499 */
    bmk_filter_init(&f);
    f.status_min = 400; f.status_max = 499;
    CHECK_EQ(count_with(db, &f), 2, "状态码区间筛选");
    bmk_filter_free(&f);

    /* 标签 */
    bmk_tag **tags = NULL;
    size_t tn = 0;
    bmk_tag_list(db, &tags, &tn);
    CHECK_EQ(tn, 1, "标签数量");
    if (tn) {
        bmk_filter_init(&f);
        f.tag_ids = (int64_t *)malloc(sizeof(int64_t));
        f.tag_ids[0] = tags[0]->id;
        f.tag_count = 1;
        CHECK_EQ(count_with(db, &f), 1, "标签筛选");
        bmk_filter_free(&f);
    }
    bmk_tag_list_free(tags, tn);

    /* 组合：分类 AND 状态码 */
    bmk_filter_init(&f);
    f.category_id = cat;
    f.status_min = 400; f.status_max = 499;
    CHECK_EQ(count_with(db, &f), 1, "分类 AND 状态码组合");
    bmk_filter_free(&f);

    /* 统计（FR-20 第 2 条） */
    bmk_stats st;
    CHECK_EQ(bmk_db_stats(db, &st), BMK_OK, "统计查询");
    CHECK_EQ(st.total, 4, "统计总数");
    CHECK_EQ(st.invalid, 1, "统计失效数");
    CHECK_EQ(st.blocked, 1, "统计待复测数");
    CHECK_EQ(st.unchecked, 2, "统计未校验数");

    (void)c;
    drop_db(db);
}

/* ---------------- 设置 ---------------- */

static void test_settings(void)
{
    bmk_db *db = fresh_db();

    CHECK_EQ(bmk_settings_get_int(db, BMK_SET_CHK_TIMEOUT, 10), 10, "默认值");
    CHECK_EQ(bmk_settings_set_int(db, BMK_SET_CHK_TIMEOUT, 25), BMK_OK, "写入 int");
    CHECK_EQ(bmk_settings_get_int(db, BMK_SET_CHK_TIMEOUT, 10), 25, "读回 int");

    CHECK_EQ(bmk_settings_set_bool(db, BMK_SET_ICON_SERVICE, false), BMK_OK, "写入 bool");
    CHECK(!bmk_settings_get_bool(db, BMK_SET_ICON_SERVICE, true), "读回 bool");

    CHECK_EQ(bmk_settings_set_str(db, BMK_SET_LANGUAGE, "en"), BMK_OK, "写入 str");
    char *v = bmk_settings_get_str(db, BMK_SET_LANGUAGE, "zh");
    CHECK_STR(v, "en", "读回 str");
    free(v);

    /* 含引号与中文的值需正确 JSON 转义 */
    CHECK_EQ(bmk_settings_set_str(db, "custom", "带\"引号\"的中文值"), BMK_OK, "写入特殊字符");
    v = bmk_settings_get_str(db, "custom", NULL);
    CHECK_STR(v, "带\"引号\"的中文值", "读回特殊字符");
    free(v);

    drop_db(db);
}

/* ---------------- 校验结果与查重 ---------------- */

static void test_check_and_dedupe(void)
{
    bmk_db *db = fresh_db();

    /* 同一 url_key 的不同写法应归为一组（附录 B #1/#2/#3） */
    add(db, "A", "https://example.com/abc?utm_source=x&id=1", 0, BMK_VALID_UNKNOWN, -1, 100);
    add(db, "B", "http://example.com/abc?utm_source=y",       0, BMK_VALID_UNKNOWN, -1, 200);
    add(db, "C", "https://www.example.com/abc/",              0, BMK_VALID_UNKNOWN, -1, 300);
    add(db, "D", "https://other.com/",                        0, BMK_VALID_UNKNOWN, -1, 400);

    bmk_dup_group **g = NULL;
    size_t n = 0;
    CHECK_EQ(bmk_dedupe_find(db, &g, &n), BMK_OK, "查重执行");
    CHECK_EQ(n, 1, "应有 1 组重复");
    if (n) {
        CHECK_EQ(g[0]->count, 3, "组内 3 条");
        CHECK_STR(g[0]->url_key, "https://example.com/abc", "组 url_key");
    }
    bmk_dup_group_list_free(g, n);

    /* count_by_key */
    char *title = NULL;
    CHECK_EQ(bmk_bookmark_count_by_key(db, "https://example.com/abc", &title), 3,
             "按 url_key 计数");
    free(title);

    drop_db(db);
}

int main(void)
{
    test_categories();
    test_tags();
    test_sort();
    test_filter();
    test_settings();
    test_check_and_dedupe();
    return test_report("数据层 / 排序 / 筛选");
}
