/*
 * test_merge.c - 合并规则（FR-16）与校验判定（FR-14）单元测试
 */
#include "test_util.h"
#include "bmk_db.h"
#include "bmk_backup.h"
#include "bmk_validate.h"

#include <stdio.h>

static const char *TMP_DB = "test_merge.sqlite";

static bmk_db *fresh_db(void)
{
    remove(TMP_DB);
    remove("test_merge.sqlite-wal");
    remove("test_merge.sqlite-shm");
    bmk_db *db = NULL;
    if (bmk_db_open(TMP_DB, &db) != BMK_OK) { fprintf(stderr, "建库失败\n"); exit(1); }
    return db;
}

static void drop_db(bmk_db *db)
{
    bmk_db_close(db);
    remove(TMP_DB);
    remove("test_merge.sqlite-wal");
    remove("test_merge.sqlite-shm");
}

static int64_t add_full(bmk_db *db, const char *title, const char *url,
                        int64_t cat, int64_t created, const char *icon)
{
    bmk_bookmark b;
    memset(&b, 0, sizeof b);
    b.title = bmk_strdup(title);
    b.url   = bmk_strdup(url);
    b.category_id = cat;
    b.created_at  = created;
    b.updated_at  = created;
    b.valid = BMK_VALID_UNKNOWN;
    b.status_code = -1;
    if (icon) {
        b.icon_len = strlen(icon);
        b.icon = (unsigned char *)malloc(b.icon_len);
        memcpy(b.icon, icon, b.icon_len);
        b.icon_mime = bmk_strdup("image/png");
    }
    bmk_bookmark_insert(db, &b);
    int64_t id = b.id;
    bmk_bookmark_free(&b);
    return id;
}

/* FR-16：主记录取 created_at 最早者；标签取并集；
 * 分类/备注/图标以主记录优先，主记录为空时取其他记录第一个非空值。 */
static void test_merge_rules(void)
{
    bmk_db *db = fresh_db();

    int64_t catA = 0, catB = 0;
    bmk_category_create(db, "A", 0, &catA);
    bmk_category_create(db, "B", 0, &catB);

    /* 主记录（最早）无分类、无图标；后两条分别提供分类与图标 */
    int64_t first  = add_full(db, "第一条", "https://example.com/x?a=1", 0,     100, NULL);
    int64_t second = add_full(db, "第二条", "https://example.com/x?b=2", catA,  200, NULL);
    int64_t third  = add_full(db, "第三条", "https://www.example.com/x/", catB, 300, "ICON");

    const char *t1[] = { "tag1" };
    const char *t2[] = { "tag2", "共享" };
    const char *t3[] = { "共享", "tag3" };
    bmk_bookmark_set_tags(db, first,  t1, 1);
    bmk_bookmark_set_tags(db, second, t2, 2);
    bmk_bookmark_set_tags(db, third,  t3, 2);

    /* 校验字段：应取组内 last_checked_at 最新者（此处为 third） */
    bmk_bookmark_set_check_result(db, second, BMK_VALID_GOOD, 200, NULL, 1000);
    bmk_bookmark_set_check_result(db, third,  BMK_VALID_BAD,  404, "not found", 2000);

    int64_t ids[3] = { third, first, second };   /* 顺序打乱，验证按时间选主记录 */
    int64_t primary = 0;
    CHECK_EQ(bmk_merge_group(db, ids, 3, &primary), BMK_OK, "合并应成功");
    CHECK_EQ(primary, first, "主记录应为 created_at 最早者");

    bmk_bookmark *b = NULL;
    CHECK_EQ(bmk_bookmark_get(db, primary, &b), BMK_OK, "读取主记录");
    if (b) {
        CHECK_STR(b->title, "第一条", "title 取主记录");
        CHECK_EQ(b->tag_count, 4, "标签取并集且去重（tag1/tag2/共享/tag3）");
        CHECK_EQ(b->category_id, catA, "主记录分类为 NULL 时取组内最早的非 NULL");
        CHECK(b->icon_len > 0, "主记录图标为 NULL 时取组内第一条非 NULL");
        CHECK_EQ(b->valid, BMK_VALID_BAD, "校验字段取 last_checked_at 最新者");
        CHECK_EQ(b->status_code, 404, "状态码取最新校验者");
        CHECK_EQ(b->last_checked_at, 2000, "最近校验时间取最新者");
        CHECK_EQ(b->created_at, 100, "created_at 保持主记录");
        bmk_bookmark_free(b);
        free(b);
    }

    /* 其余记录已删除 */
    CHECK(bmk_bookmark_get(db, second, &b) != BMK_OK, "被合并记录应删除");
    CHECK(bmk_bookmark_get(db, third, &b) != BMK_OK, "被合并记录应删除");

    drop_db(db);
}

static void test_merge_primary_wins(void)
{
    bmk_db *db = fresh_db();

    int64_t catA = 0, catB = 0;
    bmk_category_create(db, "A", 0, &catA);
    bmk_category_create(db, "B", 0, &catB);

    /* 主记录本身有分类与图标，应保持不变 */
    int64_t first  = add_full(db, "主", "https://e.com/p", catA, 100, "MAIN");
    int64_t second = add_full(db, "副", "https://e.com/p/", catB, 200, "OTHER");

    int64_t ids[2] = { first, second };
    int64_t primary = 0;
    bmk_merge_group(db, ids, 2, &primary);
    CHECK_EQ(primary, first, "主记录选取");

    bmk_bookmark *b = NULL;
    bmk_bookmark_get(db, primary, &b);
    if (b) {
        CHECK_EQ(b->category_id, catA, "主记录分类优先");
        CHECK(b->icon_len == 4 && memcmp(b->icon, "MAIN", 4) == 0, "主记录图标优先");
        bmk_bookmark_free(b);
        free(b);
    }
    drop_db(db);
}

static void test_merge_guards(void)
{
    bmk_db *db = fresh_db();
    int64_t one = add_full(db, "只有一条", "https://only.com/", 0, 100, NULL);
    int64_t ids[1] = { one };
    int64_t primary = 0;
    CHECK(bmk_merge_group(db, ids, 1, &primary) != BMK_OK, "少于两条不应合并");
    CHECK(bmk_merge_group(db, NULL, 0, &primary) != BMK_OK, "空输入应拒绝");
    drop_db(db);
}

/* FR-14 判定规则：2xx/3xx 有效；4xx/5xx 失效；
 * 403/429 视为可能被反爬，保持原有 valid 并标注 blocked；超时/DNS 失败为失效。 */
static void test_check_judge(void)
{
    bmk_check_result r;

    bmk_check_judge(200, NULL, BMK_VALID_UNKNOWN, &r);
    CHECK_EQ(r.valid, BMK_VALID_GOOD, "200 有效");
    CHECK_EQ(r.status_code, 200, "状态码回填");

    bmk_check_judge(204, NULL, BMK_VALID_UNKNOWN, &r);
    CHECK_EQ(r.valid, BMK_VALID_GOOD, "204 有效");

    bmk_check_judge(301, NULL, BMK_VALID_UNKNOWN, &r);
    CHECK_EQ(r.valid, BMK_VALID_GOOD, "301 有效");

    bmk_check_judge(399, NULL, BMK_VALID_UNKNOWN, &r);
    CHECK_EQ(r.valid, BMK_VALID_GOOD, "3xx 有效");

    bmk_check_judge(404, NULL, BMK_VALID_GOOD, &r);
    CHECK_EQ(r.valid, BMK_VALID_BAD, "404 失效");

    bmk_check_judge(500, NULL, BMK_VALID_GOOD, &r);
    CHECK_EQ(r.valid, BMK_VALID_BAD, "500 失效");

    /* 403 / 429：保持原有 valid，不翻转（附录 F） */
    bmk_check_judge(403, NULL, BMK_VALID_GOOD, &r);
    CHECK_EQ(r.valid, BMK_VALID_GOOD, "403 保持原有 valid=1");
    CHECK_STR(r.note, BMK_NOTE_BLOCKED, "403 标注 blocked");

    bmk_check_judge(403, NULL, BMK_VALID_UNKNOWN, &r);
    CHECK_EQ(r.valid, BMK_VALID_UNKNOWN, "403 保持原有 valid=NULL");
    CHECK_STR(r.note, BMK_NOTE_BLOCKED, "403 标注 blocked");

    bmk_check_judge(429, NULL, BMK_VALID_BAD, &r);
    CHECK_EQ(r.valid, BMK_VALID_BAD, "429 保持原有 valid=0");
    CHECK_STR(r.note, BMK_NOTE_BLOCKED, "429 标注 blocked");

    /* 网络层错误：status=0，note 由调用方给出 */
    bmk_check_judge(0, BMK_NOTE_TIMEOUT, BMK_VALID_GOOD, &r);
    CHECK_EQ(r.valid, BMK_VALID_BAD, "超时判失效");
    CHECK_STR(r.note, BMK_NOTE_TIMEOUT, "超时标注");
    CHECK_EQ(r.status_code, 0, "网络层失败状态码为 0");

    bmk_check_judge(0, BMK_NOTE_DNS_FAILED, BMK_VALID_GOOD, &r);
    CHECK_EQ(r.valid, BMK_VALID_BAD, "DNS 失败判失效");
    CHECK_STR(r.note, BMK_NOTE_DNS_FAILED, "DNS 标注");

    bmk_check_judge(0, BMK_NOTE_TLS_FAILED, BMK_VALID_GOOD, &r);
    CHECK_EQ(r.valid, BMK_VALID_BAD, "TLS 失败判失效");
    CHECK_STR(r.note, BMK_NOTE_TLS_FAILED, "TLS 标注");

    bmk_check_judge(0, BMK_NOTE_CONNECT_FAILED, BMK_VALID_GOOD, &r);
    CHECK_EQ(r.valid, BMK_VALID_BAD, "连接失败判失效");
}

int main(void)
{
    /* 合并前会生成备份文件（FR-16 第 4 条），需先指定备份目录。 */
    bmk_mkdirs("test_backups");
    bmk_backup_set_dir("test_backups");

    test_merge_rules();
    test_merge_primary_wins();
    test_merge_guards();
    test_check_judge();
    return test_report("合并规则 / 校验判定");
}
