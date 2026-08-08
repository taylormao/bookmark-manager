/*
 * bmk_db.c - SQLite 数据层实现
 *
 * DDL 严格遵循需求第 3.2 节原文。
 */
#include "bmk_db.h"
#include "bmk_log.h"
#include "bmk_url.h"
#include "bmk_settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sqlite3.h>

#ifdef _WIN32
#  include <windows.h>
typedef CRITICAL_SECTION bmk_mutex;
#  define MTX_INIT(m)   InitializeCriticalSection(m)
#  define MTX_LOCK(m)   EnterCriticalSection(m)
#  define MTX_UNLOCK(m) LeaveCriticalSection(m)
#  define MTX_FREE(m)   DeleteCriticalSection(m)
#else
#  include <pthread.h>
typedef pthread_mutex_t bmk_mutex;
#  define MTX_INIT(m)   pthread_mutex_init((m), NULL)
#  define MTX_LOCK(m)   pthread_mutex_lock(m)
#  define MTX_UNLOCK(m) pthread_mutex_unlock(m)
#  define MTX_FREE(m)   pthread_mutex_destroy(m)
#endif

#define BMK_SCHEMA_VERSION 1

struct bmk_db {
    sqlite3   *h;
    bmk_mutex  lock;
    int        lock_depth;  /* Bug#9: 可重入锁深度 */
    char      *last_error;
};

sqlite3 *bmk_db_handle(bmk_db *db) { return db ? db->h : NULL; }
void bmk_db_lock(bmk_db *db) {
    if (!db) return;
    if (db->lock_depth > 0) { db->lock_depth++; return; }
    MTX_LOCK(&db->lock);
    db->lock_depth = 1;
}
void bmk_db_unlock(bmk_db *db) {
    if (!db) return;
    if (db->lock_depth > 1) { db->lock_depth--; return; }
    db->lock_depth = 0;
    MTX_UNLOCK(&db->lock);
}

/* 为所有公开的数据库操作加锁/解锁（需求 2.4）。
 * 开锁/关锁/begin/commit/rollback 本身不加锁（调用者自行配对），
 * 其余读写函数均自保护。 */
#define BMK_DB_GUARD(db)                                              \
    do {                                                              \
        if (!(db)) return BMK_ERR_INVALID_ARG;                        \
        bmk_db_lock(db);                                              \
    } while (0)
#define BMK_DB_GUARD_GOTO(db, label)                                  \
    do {                                                              \
        if (!(db)) { e = BMK_ERR_INVALID_ARG; goto label; }           \
        bmk_db_lock(db);                                              \
    } while (0)
#define BMK_DB_GUARD_RET(db, ret)                                     \
    do {                                                              \
        if (!(db)) return (ret);                                      \
        bmk_db_lock(db);                                              \
    } while (0)
#define BMK_DB_UNLOCK(db)                                             \
    do {                                                              \
        bmk_db_unlock(db);                                            \
    } while (0)

static void set_error(bmk_db *db, const char *msg)
{
    if (!db) return;
    free(db->last_error);
    db->last_error = bmk_strdup(msg ? msg : "");
}

const char *bmk_db_last_error(bmk_db *db)
{
    return (db && db->last_error) ? db->last_error : "";
}

static bmk_err exec_sql(bmk_db *db, const char *sql)
{
    char *err = NULL;
    if (sqlite3_exec(db->h, sql, NULL, NULL, &err) != SQLITE_OK) {
        set_error(db, err);
        BMK_ERROR("sql failed: %s (%s)", err ? err : "?", sql);
        sqlite3_free(err);
        return BMK_ERR_DB;
    }
    return BMK_OK;
}

/* ------------------------------------------------------------------ */
/* 模型释放                                                            */
/* ------------------------------------------------------------------ */

void bmk_bookmark_free(bmk_bookmark *b)
{
    if (!b) return;
    free(b->title);
    free(b->url);
    free(b->url_key);
    free(b->icon);
    free(b->icon_mime);
    free(b->category_name);
    free(b->check_note);
    for (size_t i = 0; i < b->tag_count; i++) free(b->tags[i]);
    free(b->tags);
}

void bmk_bookmark_list_free(bmk_bookmark **list, size_t n)
{
    if (!list) return;
    for (size_t i = 0; i < n; i++) {
        bmk_bookmark_free(list[i]);
        free(list[i]);
    }
    free(list);
}

void bmk_category_free(bmk_category *c)
{
    if (!c) return;
    free(c->name);
    free(c);
}

void bmk_category_list_free(bmk_category **list, size_t n)
{
    if (!list) return;
    for (size_t i = 0; i < n; i++) bmk_category_free(list[i]);
    free(list);
}

void bmk_tag_free(bmk_tag *t)
{
    if (!t) return;
    free(t->name);
    free(t);
}

void bmk_tag_list_free(bmk_tag **list, size_t n)
{
    if (!list) return;
    for (size_t i = 0; i < n; i++) bmk_tag_free(list[i]);
    free(list);
}

void bmk_filter_init(bmk_filter *f)
{
    memset(f, 0, sizeof(*f));
    f->category_id = BMK_CATEGORY_ALL;
    f->valid_filter = BMK_VF_ALL;
    f->status_min = -1;
    f->status_max = -1;
}

void bmk_filter_free(bmk_filter *f)
{
    if (!f) return;
    free(f->keyword);
    free(f->url_contains);
    free(f->domain);
    free(f->tag_ids);
    memset(f, 0, sizeof(*f));
}

/* ------------------------------------------------------------------ */
/* 建库与迁移                                                          */
/* ------------------------------------------------------------------ */

static const char *SCHEMA_SQL =
"CREATE TABLE IF NOT EXISTS categories ("
"  id        INTEGER PRIMARY KEY AUTOINCREMENT,"
"  name      TEXT NOT NULL,"
"  parent_id INTEGER REFERENCES categories(id) ON DELETE CASCADE,"
"  UNIQUE (parent_id, name COLLATE NOCASE)"
");"
"CREATE TABLE IF NOT EXISTS tags ("
"  id   INTEGER PRIMARY KEY AUTOINCREMENT,"
"  name TEXT NOT NULL UNIQUE COLLATE NOCASE"
");"
"CREATE TABLE IF NOT EXISTS bookmarks ("
"  id              INTEGER PRIMARY KEY AUTOINCREMENT,"
"  title           TEXT NOT NULL,"
"  url             TEXT NOT NULL,"
"  url_key         TEXT NOT NULL,"
"  icon            BLOB,"
"  icon_mime       TEXT,"
"  category_id     INTEGER REFERENCES categories(id) ON DELETE SET NULL,"
"  valid           INTEGER CHECK (valid IN (0,1) OR valid IS NULL),"
"  status_code     INTEGER,"
"  last_checked_at INTEGER,"
"  check_note      TEXT,"
"  created_at      INTEGER NOT NULL,"
"  updated_at      INTEGER NOT NULL"
");"
"CREATE INDEX IF NOT EXISTS idx_bookmarks_url_key  ON bookmarks(url_key);"
"CREATE INDEX IF NOT EXISTS idx_bookmarks_category ON bookmarks(category_id);"
"CREATE INDEX IF NOT EXISTS idx_bookmarks_valid    ON bookmarks(valid);"
"CREATE TABLE IF NOT EXISTS bookmark_tags ("
"  bookmark_id INTEGER NOT NULL REFERENCES bookmarks(id) ON DELETE CASCADE,"
"  tag_id      INTEGER NOT NULL REFERENCES tags(id) ON DELETE CASCADE,"
"  PRIMARY KEY (bookmark_id, tag_id)"
");"
"CREATE TABLE IF NOT EXISTS deleted_log ("
"  id          INTEGER PRIMARY KEY AUTOINCREMENT,"
"  bookmark_id INTEGER NOT NULL,"
"  title       TEXT,"
"  url         TEXT,"
"  url_key     TEXT,"
"  category_id INTEGER,"
"  tags        TEXT,"
"  deleted_at  INTEGER NOT NULL,"
"  backup_file TEXT"
");"
"CREATE TABLE IF NOT EXISTS sync_meta ("
"  key   TEXT PRIMARY KEY,"
"  value TEXT"
");"
"CREATE TABLE IF NOT EXISTS settings ("
"  key   TEXT PRIMARY KEY,"
"  value TEXT"
");";

bool bmk_db_integrity_ok(bmk_db *db)
{
    sqlite3_stmt *st = NULL;
    bool ok = false;
    if (sqlite3_prepare_v2(db->h, "PRAGMA integrity_check;", -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) {
            const unsigned char *v = sqlite3_column_text(st, 0);
            ok = v && strcmp((const char *)v, "ok") == 0;
        }
    }
    sqlite3_finalize(st);
    return ok;
}

/* schema 迁移：以 settings.schema_version 为准（需求 3.4 第 4 条）。 */
static bmk_err migrate(bmk_db *db)
{
    int64_t ver = bmk_settings_get_int(db, BMK_SET_SCHEMA_VERSION, 0);
    if (ver == BMK_SCHEMA_VERSION) return BMK_OK;

    if (ver == 0) {
        /* 全新库或首次引入版本号 */
        bmk_err e = bmk_settings_set_int(db, BMK_SET_SCHEMA_VERSION, BMK_SCHEMA_VERSION);
        if (e != BMK_OK) return e;
        BMK_INFO("schema initialized at version %d", BMK_SCHEMA_VERSION);
        return BMK_OK;
    }
    if (ver > BMK_SCHEMA_VERSION) {
        BMK_ERROR("database schema version %lld is newer than supported %d",
                  (long long)ver, BMK_SCHEMA_VERSION);
        set_error(db, "database created by a newer version");
        return BMK_ERR_UNSUPPORTED;
    }
    /* 未来版本在此追加增量迁移分支 */
    return bmk_settings_set_int(db, BMK_SET_SCHEMA_VERSION, BMK_SCHEMA_VERSION);
}

bmk_err bmk_db_open(const char *db_path, bmk_db **out)
{
    if (!db_path || !out) return BMK_ERR_INVALID_ARG;
    *out = NULL;

    bmk_db *db = (bmk_db *)calloc(1, sizeof(bmk_db));
    if (!db) return BMK_ERR_NOMEM;
    MTX_INIT(&db->lock);

    int rc = sqlite3_open_v2(db_path, &db->h,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                             SQLITE_OPEN_FULLMUTEX, NULL);
    if (rc != SQLITE_OK) {
        BMK_ERROR("cannot open database %s: %s", db_path,
                  db->h ? sqlite3_errmsg(db->h) : "?");
        bmk_err e = (rc == SQLITE_CORRUPT) ? BMK_ERR_DB_CORRUPT :
                    (rc == SQLITE_PERM || rc == SQLITE_CANTOPEN) ? BMK_ERR_PERMISSION
                                                                 : BMK_ERR_DB;
        if (db->h) sqlite3_close(db->h);
        MTX_FREE(&db->lock);
        free(db);
        return e;
    }

    /* 需求 3.1：建库即执行 WAL 与外键 */
    exec_sql(db, "PRAGMA journal_mode=WAL;");
    exec_sql(db, "PRAGMA foreign_keys=ON;");
    exec_sql(db, "PRAGMA busy_timeout=5000;");

    if (!bmk_db_integrity_ok(db)) {
        BMK_ERROR("integrity_check failed for %s", db_path);
        sqlite3_close(db->h);
        MTX_FREE(&db->lock);
        free(db->last_error);
        free(db);
        return BMK_ERR_DB_CORRUPT;
    }

    if (exec_sql(db, SCHEMA_SQL) != BMK_OK) {
        bmk_db_close(db);
        return BMK_ERR_DB;
    }

    bmk_err e = migrate(db);
    if (e != BMK_OK) {
        bmk_db_close(db);
        return e;
    }

    *out = db;
    return BMK_OK;
}

void bmk_db_close(bmk_db *db)
{
    if (!db) return;
    if (db->h) sqlite3_close(db->h);
    MTX_FREE(&db->lock);
    free(db->last_error);
    free(db);
}

bmk_err bmk_db_begin(bmk_db *db)    { return exec_sql(db, "BEGIN IMMEDIATE;"); }
bmk_err bmk_db_commit(bmk_db *db)   { return exec_sql(db, "COMMIT;"); }
bmk_err bmk_db_rollback(bmk_db *db) { return exec_sql(db, "ROLLBACK;"); }

/* ------------------------------------------------------------------ */
/* 分类                                                                */
/* ------------------------------------------------------------------ */

static void bind_parent(sqlite3_stmt *st, int idx, int64_t parent_id)
{
    if (parent_id > 0) sqlite3_bind_int64(st, idx, parent_id);
    else               sqlite3_bind_null(st, idx);
}

int64_t bmk_category_find(bmk_db *db, const char *name, int64_t parent_id)
{
    if (!db || !name) return 0;
    BMK_DB_GUARD_RET(db, 0);
    const char *sql = parent_id > 0
        ? "SELECT id FROM categories WHERE parent_id=? AND name=? COLLATE NOCASE;"
        : "SELECT id FROM categories WHERE parent_id IS NULL AND name=? COLLATE NOCASE;";
    sqlite3_stmt *st = NULL;
    int64_t id = 0;
    if (sqlite3_prepare_v2(db->h, sql, -1, &st, NULL) != SQLITE_OK) goto done;
    int i = 1;
    if (parent_id > 0) sqlite3_bind_int64(st, i++, parent_id);
    sqlite3_bind_text(st, i, name, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) id = sqlite3_column_int64(st, 0);
done:
    sqlite3_finalize(st);
    BMK_DB_UNLOCK(db);
    return id;
}

bmk_err bmk_category_create(bmk_db *db, const char *name, int64_t parent_id, int64_t *out_id)
{
    if (!db || !name || !*name) return BMK_ERR_INVALID_ARG;

    /* 锁外检测同名（bmk_category_find 自身加锁），随后加锁再 INSERT。
     * 竞态极小（同父同名冲突被 UNIQUE 约束兜底），先判定可省事务开销。 */
    int64_t exist = bmk_category_find(db, name, parent_id);
    if (exist) {
        if (out_id) *out_id = exist;
        return BMK_ERR_CONFLICT;
    }

    BMK_DB_GUARD(db);
    bmk_err e = BMK_OK;
    sqlite3_stmt *st = NULL;
    const char *sql = "INSERT INTO categories(name, parent_id) VALUES(?, ?);";
    if (sqlite3_prepare_v2(db->h, sql, -1, &st, NULL) != SQLITE_OK) {
        set_error(db, sqlite3_errmsg(db->h));
        e = BMK_ERR_DB; goto done;
    }
    sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
    bind_parent(st, 2, parent_id);
    if (sqlite3_step(st) != SQLITE_DONE) {
        set_error(db, sqlite3_errmsg(db->h));
        e = BMK_ERR_DB; goto done;
    }
    if (out_id) *out_id = sqlite3_last_insert_rowid(db->h);
done:
    sqlite3_finalize(st);
    BMK_DB_UNLOCK(db);
    return e;
}

bmk_err bmk_category_rename(bmk_db *db, int64_t id, const char *new_name)
{
    if (!db || id <= 0 || !new_name || !*new_name) return BMK_ERR_INVALID_ARG;
    BMK_DB_GUARD(db);

    bmk_err e = BMK_OK;
    sqlite3_stmt *st = NULL;
    int64_t parent = 0;
    if (sqlite3_prepare_v2(db->h, "SELECT parent_id FROM categories WHERE id=?;",
                           -1, &st, NULL) != SQLITE_OK) { e = BMK_ERR_DB; goto done; }
    sqlite3_bind_int64(st, 1, id);
    if (sqlite3_step(st) != SQLITE_ROW) { e = BMK_ERR_NOT_FOUND; goto done; }
    parent = (sqlite3_column_type(st, 0) == SQLITE_NULL) ? 0 : sqlite3_column_int64(st, 0);
    sqlite3_finalize(st); st = NULL;

    /* 解锁后查重（bmk_category_find 自行加锁），查完再锁回来 */
    BMK_DB_UNLOCK(db);
    int64_t exist = bmk_category_find(db, new_name, parent);
    bmk_db_lock(db);
    if (exist && exist != id) { e = BMK_ERR_CONFLICT; goto done; }

    if (sqlite3_prepare_v2(db->h, "UPDATE categories SET name=? WHERE id=?;",
                           -1, &st, NULL) != SQLITE_OK) { e = BMK_ERR_DB; goto done; }
    sqlite3_bind_text(st, 1, new_name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, id);
    e = (sqlite3_step(st) == SQLITE_DONE) ? BMK_OK : BMK_ERR_DB;
    if (e != BMK_OK) set_error(db, sqlite3_errmsg(db->h));
done:
    sqlite3_finalize(st);
    BMK_DB_UNLOCK(db);
    return e;
}

bool bmk_category_is_descendant(bmk_db *db, int64_t id, int64_t ancestor)
{
    if (id <= 0 || ancestor <= 0) return false;
    if (id == ancestor) return true;
    BMK_DB_GUARD_RET(db, false);

    bool result = false;
    /* 自 id 向上回溯（层级有限，最多 32 层的约束下安全；此处设 256 上限防环） */
    int64_t cur = id;
    for (int depth = 0; depth < 256 && cur > 0; depth++) {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(db->h, "SELECT parent_id FROM categories WHERE id=?;",
                               -1, &st, NULL) != SQLITE_OK) break;
        sqlite3_bind_int64(st, 1, cur);
        int64_t parent = 0;
        if (sqlite3_step(st) == SQLITE_ROW)
            parent = (sqlite3_column_type(st, 0) == SQLITE_NULL) ? 0
                                                                 : sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
        if (parent == ancestor) { result = true; break; }
        cur = parent;
    }
    BMK_DB_UNLOCK(db);
    return result;
}

bmk_err bmk_category_move(bmk_db *db, int64_t id, int64_t new_parent_id)
{
    if (!db || id <= 0) return BMK_ERR_INVALID_ARG;
    /* 禁止移动到自身或子孙（FR-10 第 2 条）—— is_descendant 自身加锁，此处无锁调用 */
    if (new_parent_id > 0 && bmk_category_is_descendant(db, new_parent_id, id))
        return BMK_ERR_INVALID_ARG;
    if (new_parent_id == id) return BMK_ERR_INVALID_ARG;

    BMK_DB_GUARD(db);
    bmk_err e = BMK_OK;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->h, "SELECT name FROM categories WHERE id=?;",
                           -1, &st, NULL) != SQLITE_OK) { e = BMK_ERR_DB; goto done; }
    sqlite3_bind_int64(st, 1, id);
    char *name = NULL;
    if (sqlite3_step(st) == SQLITE_ROW)
        name = bmk_strdup((const char *)sqlite3_column_text(st, 0));
    sqlite3_finalize(st); st = NULL;
    if (!name) { e = BMK_ERR_NOT_FOUND; goto done; }

    /* 同名检测（bmk_category_find 自行加锁） */
    BMK_DB_UNLOCK(db);
    int64_t exist = bmk_category_find(db, name, new_parent_id);
    free(name);
    bmk_db_lock(db);
    if (exist && exist != id) { e = BMK_ERR_CONFLICT; goto done; }

    if (sqlite3_prepare_v2(db->h, "UPDATE categories SET parent_id=? WHERE id=?;",
                           -1, &st, NULL) != SQLITE_OK) { e = BMK_ERR_DB; goto done; }
    bind_parent(st, 1, new_parent_id);
    sqlite3_bind_int64(st, 2, id);
    e = (sqlite3_step(st) == SQLITE_DONE) ? BMK_OK : BMK_ERR_DB;
done:
    sqlite3_finalize(st);
    BMK_DB_UNLOCK(db);
    return e;
}

bmk_err bmk_category_delete(bmk_db *db, int64_t id)
{
    if (!db || id <= 0) return BMK_ERR_INVALID_ARG;
    BMK_DB_GUARD(db);
    /* 外键 ON DELETE CASCADE 递归删子分类；bookmarks.category_id 自动置 NULL */
    sqlite3_stmt *st = NULL;
    bmk_err e = BMK_OK;
    if (sqlite3_prepare_v2(db->h, "DELETE FROM categories WHERE id=?;",
                           -1, &st, NULL) != SQLITE_OK) { e = BMK_ERR_DB; goto done; }
    sqlite3_bind_int64(st, 1, id);
    e = (sqlite3_step(st) == SQLITE_DONE) ? BMK_OK : BMK_ERR_DB;
    if (e != BMK_OK) set_error(db, sqlite3_errmsg(db->h));
done:
    sqlite3_finalize(st);
    BMK_DB_UNLOCK(db);
    return e;
}

bmk_err bmk_category_list(bmk_db *db, bmk_category ***out, size_t *out_n)
{
    if (!db || !out || !out_n) return BMK_ERR_INVALID_ARG;
    BMK_DB_GUARD(db);
    *out = NULL; *out_n = 0;

    const char *sql =
        "SELECT id, name, COALESCE(parent_id,0) FROM categories "
        "ORDER BY COALESCE(parent_id,0), name COLLATE NOCASE;";
    sqlite3_stmt *st = NULL;
    bmk_err e = BMK_OK;
    if (sqlite3_prepare_v2(db->h, sql, -1, &st, NULL) != SQLITE_OK) { e = BMK_ERR_DB; goto done; }

    size_t cap = 16, n = 0;
    bmk_category **list = (bmk_category **)calloc(cap, sizeof(*list));
    if (!list) { e = BMK_ERR_NOMEM; goto done; }

    while (sqlite3_step(st) == SQLITE_ROW) {
        if (n == cap) {
            cap *= 2;
            bmk_category **nl = (bmk_category **)realloc(list, cap * sizeof(*list));
            if (!nl) { bmk_category_list_free(list, n); e = BMK_ERR_NOMEM; goto done; }
            list = nl;
        }
        bmk_category *c = (bmk_category *)calloc(1, sizeof(*c));
        if (!c) { bmk_category_list_free(list, n); e = BMK_ERR_NOMEM; goto done; }
        c->id = sqlite3_column_int64(st, 0);
        c->name = bmk_strdup((const char *)sqlite3_column_text(st, 1));
        c->parent_id = sqlite3_column_int64(st, 2);
        list[n++] = c;
    }
    *out = list;
    *out_n = n;
    list = NULL;   /* 转移所有权，避免 done 处重复释放 */
done:
    sqlite3_finalize(st);
    BMK_DB_UNLOCK(db);
    return e;
}

/* 递归 CTE：分类及其全部子孙 id */
#define CATEGORY_TREE_CTE                                       \
    "WITH RECURSIVE tree(id) AS ("                              \
    "  SELECT id FROM categories WHERE id=?"                    \
    "  UNION ALL"                                               \
    "  SELECT c.id FROM categories c JOIN tree t ON c.parent_id=t.id"  \
    ")"

int64_t bmk_category_count_recursive(bmk_db *db, int64_t id)
{
    if (!db || id <= 0) return 0;
    BMK_DB_GUARD_RET(db, 0);
    const char *sql = CATEGORY_TREE_CTE
        " SELECT COUNT(*) FROM bookmarks WHERE category_id IN (SELECT id FROM tree);";
    sqlite3_stmt *st = NULL;
    int64_t c = 0;
    if (sqlite3_prepare_v2(db->h, sql, -1, &st, NULL) != SQLITE_OK) goto done;
    sqlite3_bind_int64(st, 1, id);
    if (sqlite3_step(st) == SQLITE_ROW) c = sqlite3_column_int64(st, 0);
done:
    sqlite3_finalize(st);
    BMK_DB_UNLOCK(db);
    return c;
}

int64_t bmk_category_affected_bookmarks(bmk_db *db, int64_t id)
{
    return bmk_category_count_recursive(db, id);
}

int64_t bmk_category_count_uncategorized(bmk_db *db)
{
    if (!db) return 0;
    BMK_DB_GUARD_RET(db, 0);
    sqlite3_stmt *st = NULL;
    int64_t c = 0;
    if (sqlite3_prepare_v2(db->h,
            "SELECT COUNT(*) FROM bookmarks WHERE category_id IS NULL;",
            -1, &st, NULL) != SQLITE_OK) goto done;
    if (sqlite3_step(st) == SQLITE_ROW) c = sqlite3_column_int64(st, 0);
done:
    sqlite3_finalize(st);
    BMK_DB_UNLOCK(db);
    return c;
}

/* ------------------------------------------------------------------ */
/* 标签                                                                */
/* ------------------------------------------------------------------ */

bmk_err bmk_tag_get_or_create(bmk_db *db, const char *name, int64_t *out_id)
{
    if (!db || !name) return BMK_ERR_INVALID_ARG;
    char *trimmed = bmk_strdup(name);
    if (!trimmed) return BMK_ERR_NOMEM;
    bmk_str_trim(trimmed);
    if (!*trimmed) { free(trimmed); return BMK_ERR_INVALID_ARG; }

    BMK_DB_GUARD(db);
    bmk_err e = BMK_OK;
    sqlite3_stmt *st = NULL;
    /* 不区分大小写查找（tags.name 为 UNIQUE COLLATE NOCASE） */
    if (sqlite3_prepare_v2(db->h, "SELECT id FROM tags WHERE name=? COLLATE NOCASE;",
                           -1, &st, NULL) != SQLITE_OK) { e = BMK_ERR_DB; goto done; }
    sqlite3_bind_text(st, 1, trimmed, -1, SQLITE_TRANSIENT);
    int64_t id = 0;
    if (sqlite3_step(st) == SQLITE_ROW) id = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st); st = NULL;

    if (id) {  /* 已存在：保留先创建者的大小写形式（FR-11 第 1 条） */
        if (out_id) *out_id = id;
        goto done;
    }

    if (sqlite3_prepare_v2(db->h, "INSERT INTO tags(name) VALUES(?);",
                           -1, &st, NULL) != SQLITE_OK) { e = BMK_ERR_DB; goto done; }
    sqlite3_bind_text(st, 1, trimmed, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) != SQLITE_DONE) {
        set_error(db, sqlite3_errmsg(db->h));
        e = BMK_ERR_DB;
    } else if (out_id) {
        *out_id = sqlite3_last_insert_rowid(db->h);
    }
done:
    sqlite3_finalize(st);
    BMK_DB_UNLOCK(db);
    free(trimmed);
    return e;
}

bmk_err bmk_tag_list(bmk_db *db, bmk_tag ***out, size_t *out_n)
{
    if (!db || !out || !out_n) return BMK_ERR_INVALID_ARG;
    BMK_DB_GUARD(db);
    *out = NULL; *out_n = 0;

    const char *sql =
        "SELECT t.id, t.name, (SELECT COUNT(*) FROM bookmark_tags bt WHERE bt.tag_id=t.id) "
        "FROM tags t ORDER BY t.name COLLATE NOCASE;";
    sqlite3_stmt *st = NULL;
    bmk_err e = BMK_OK;
    if (sqlite3_prepare_v2(db->h, sql, -1, &st, NULL) != SQLITE_OK) { e = BMK_ERR_DB; goto done; }

    size_t cap = 16, n = 0;
    bmk_tag **list = (bmk_tag **)calloc(cap, sizeof(*list));
    if (!list) { e = BMK_ERR_NOMEM; goto done; }

    while (sqlite3_step(st) == SQLITE_ROW) {
        if (n == cap) {
            cap *= 2;
            bmk_tag **nl = (bmk_tag **)realloc(list, cap * sizeof(*list));
            if (!nl) { bmk_tag_list_free(list, n); e = BMK_ERR_NOMEM; goto done; }
            list = nl;
        }
        bmk_tag *t = (bmk_tag *)calloc(1, sizeof(*t));
        if (!t) { bmk_tag_list_free(list, n); e = BMK_ERR_NOMEM; goto done; }
        t->id = sqlite3_column_int64(st, 0);
        t->name = bmk_strdup((const char *)sqlite3_column_text(st, 1));
        t->usage_count = sqlite3_column_int64(st, 2);
        list[n++] = t;
    }
    *out = list;
    *out_n = n;
    list = NULL;
done:
    sqlite3_finalize(st);
    BMK_DB_UNLOCK(db);
    return e;
}

bmk_err bmk_tag_rename(bmk_db *db, int64_t id, const char *new_name)
{
    if (!db || id <= 0 || !new_name) return BMK_ERR_INVALID_ARG;
    char *trimmed = bmk_strdup(new_name);
    if (!trimmed) return BMK_ERR_NOMEM;
    bmk_str_trim(trimmed);
    if (!*trimmed) { free(trimmed); return BMK_ERR_INVALID_ARG; }

    BMK_DB_GUARD(db);
    bmk_err e = BMK_OK;
    sqlite3_stmt *st = NULL;
    /* 目标名已存在（不区分大小写）且不是自己 → 视为合并 */
    if (sqlite3_prepare_v2(db->h, "SELECT id FROM tags WHERE name=? COLLATE NOCASE;",
                           -1, &st, NULL) != SQLITE_OK) { e = BMK_ERR_DB; goto done; }
    sqlite3_bind_text(st, 1, trimmed, -1, SQLITE_TRANSIENT);
    int64_t exist = 0;
    if (sqlite3_step(st) == SQLITE_ROW) exist = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st); st = NULL;

    if (exist && exist != id) {
        BMK_DB_UNLOCK(db);                     /* 先解锁再调 merge（merge 自行加锁） */
        free(trimmed);
        return bmk_tag_merge(db, id, exist);
    }

    if (sqlite3_prepare_v2(db->h, "UPDATE tags SET name=? WHERE id=?;",
                           -1, &st, NULL) != SQLITE_OK) { e = BMK_ERR_DB; goto done; }
    sqlite3_bind_text(st, 1, trimmed, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, id);
    e = (sqlite3_step(st) == SQLITE_DONE) ? BMK_OK : BMK_ERR_DB;
done:
    sqlite3_finalize(st);
    BMK_DB_UNLOCK(db);
    free(trimmed);
    return e;
}

bmk_err bmk_tag_merge(bmk_db *db, int64_t from_id, int64_t into_id)
{
    if (!db || from_id <= 0 || into_id <= 0 || from_id == into_id)
        return BMK_ERR_INVALID_ARG;
    /* begin/commit/rollback 内部 exec_sql 加锁，此处不再重复锁 */
    /* 单事务：迁移关联（避免主键冲突）后删除源标签（需求 3.4 第 3 条） */
    bmk_err e = bmk_db_begin(db);
    if (e != BMK_OK) return e;

    char *err = NULL;
    char sql[512];
    snprintf(sql, sizeof(sql),
             "INSERT OR IGNORE INTO bookmark_tags(bookmark_id, tag_id) "
             "SELECT bookmark_id, %lld FROM bookmark_tags WHERE tag_id=%lld;"
             "DELETE FROM tags WHERE id=%lld;",
             (long long)into_id, (long long)from_id, (long long)from_id);
    if (sqlite3_exec(db->h, sql, NULL, NULL, &err) != SQLITE_OK) {
        set_error(db, err);
        sqlite3_free(err);
        bmk_db_rollback(db);
        return BMK_ERR_DB;
    }
    return bmk_db_commit(db);
}

bmk_err bmk_tag_delete(bmk_db *db, int64_t id)
{
    if (!db || id <= 0) return BMK_ERR_INVALID_ARG;
    BMK_DB_GUARD(db);
    /* bookmark_tags 有 ON DELETE CASCADE，只解除关联不动 bookmarks */
    sqlite3_stmt *st = NULL;
    bmk_err e = BMK_OK;
    if (sqlite3_prepare_v2(db->h, "DELETE FROM tags WHERE id=?;",
                           -1, &st, NULL) != SQLITE_OK) { e = BMK_ERR_DB; goto done; }
    sqlite3_bind_int64(st, 1, id);
    e = (sqlite3_step(st) == SQLITE_DONE) ? BMK_OK : BMK_ERR_DB;
done:
    sqlite3_finalize(st);
    BMK_DB_UNLOCK(db);
    return e;
}

bmk_err bmk_bookmark_set_tags(bmk_db *db, int64_t bookmark_id,
                              const char *const *tags, size_t n)
{
    if (!db || bookmark_id <= 0) return BMK_ERR_INVALID_ARG;
    BMK_DB_GUARD(db);

    bmk_err e = BMK_OK;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->h, "DELETE FROM bookmark_tags WHERE bookmark_id=?;",
                           -1, &st, NULL) != SQLITE_OK) { e = BMK_ERR_DB; goto done; }
    sqlite3_bind_int64(st, 1, bookmark_id);
    sqlite3_step(st);
    sqlite3_finalize(st); st = NULL;

    for (size_t i = 0; i < n; i++) {
        if (!tags[i] || !*tags[i]) continue;
        int64_t tid = 0;
        /* bmk_tag_get_or_create 自行加锁 — 先解锁，避免嵌套死锁 */
        BMK_DB_UNLOCK(db);
        bmk_err te = bmk_tag_get_or_create(db, tags[i], &tid);
        bmk_db_lock(db);
        if (te != BMK_OK || !tid) continue;
        if (sqlite3_prepare_v2(db->h,
                "INSERT OR IGNORE INTO bookmark_tags(bookmark_id, tag_id) VALUES(?,?);",
                -1, &st, NULL) != SQLITE_OK) { e = BMK_ERR_DB; goto done; }
        sqlite3_bind_int64(st, 1, bookmark_id);
        sqlite3_bind_int64(st, 2, tid);
        sqlite3_step(st);
        sqlite3_finalize(st); st = NULL;
    }

    /* 标签变更也刷新 updated_at（需求 3.3）。独立调用（非经 update 走查）时
     * 也保证时间戳正确；经 insert/update 时重复刷新为同一秒，无副作用。 */
    if (e == BMK_OK) {
        sqlite3_stmt *ust = NULL;
        if (sqlite3_prepare_v2(db->h,
                "UPDATE bookmarks SET updated_at=? WHERE id=?;",
                -1, &ust, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(ust, 1, bmk_now());
            sqlite3_bind_int64(ust, 2, bookmark_id);
            sqlite3_step(ust);
        }
        sqlite3_finalize(ust);
    }
done:
    sqlite3_finalize(st);
    BMK_DB_UNLOCK(db);
    return e;
}

bmk_err bmk_bookmark_add_tags(bmk_db *db, const int64_t *bookmark_ids, size_t n_ids,
                              const char *const *tags, size_t n_tags)
{
    if (!db || !bookmark_ids || !tags) return BMK_ERR_INVALID_ARG;
    /* begin/commit 内部使用 exec_sql（无锁），此函数自己管理事务，
     * 调用者已持有锁的场景不应发生（本函数为独立操作）。加锁并持有至事务结束。 */
    bmk_db_lock(db);

    bmk_err e = bmk_db_begin(db);
    if (e != BMK_OK) { bmk_db_unlock(db); return e; }

    int64_t now = bmk_now();
    for (size_t t = 0; t < n_tags; t++) {
        if (!tags[t] || !*tags[t]) continue;
        int64_t tid = 0;
        /* tag_get_or_create 自行加锁——解锁避免嵌套，之后再锁回来 */
        bmk_db_unlock(db);
        bmk_err te = bmk_tag_get_or_create(db, tags[t], &tid);
        bmk_db_lock(db);
        if (te != BMK_OK || !tid) continue;
        for (size_t i = 0; i < n_ids; i++) {
            sqlite3_stmt *st = NULL;
            if (sqlite3_prepare_v2(db->h,
                    "INSERT OR IGNORE INTO bookmark_tags(bookmark_id, tag_id) VALUES(?,?);",
                    -1, &st, NULL) != SQLITE_OK) { bmk_db_rollback(db); bmk_db_unlock(db); return BMK_ERR_DB; }
            sqlite3_bind_int64(st, 1, bookmark_ids[i]);
            sqlite3_bind_int64(st, 2, tid);
            sqlite3_step(st);
            sqlite3_finalize(st);
        }
    }
    /* 标签变更刷新 updated_at（需求 3.3） */
    for (size_t i = 0; i < n_ids; i++) {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(db->h, "UPDATE bookmarks SET updated_at=? WHERE id=?;",
                               -1, &st, NULL) != SQLITE_OK) { bmk_db_rollback(db); bmk_db_unlock(db); return BMK_ERR_DB; }
        sqlite3_bind_int64(st, 1, now);
        sqlite3_bind_int64(st, 2, bookmark_ids[i]);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    e = bmk_db_commit(db);
    bmk_db_unlock(db);
    return e;
}

/* ------------------------------------------------------------------ */
/* 书签                                                                */
/* ------------------------------------------------------------------ */

static bmk_err load_tags_for(bmk_db *db, bmk_bookmark *b)
{
    sqlite3_stmt *st = NULL;
    const char *sql =
        "SELECT t.name FROM tags t JOIN bookmark_tags bt ON bt.tag_id=t.id "
        "WHERE bt.bookmark_id=? ORDER BY t.name COLLATE NOCASE;";
    if (sqlite3_prepare_v2(db->h, sql, -1, &st, NULL) != SQLITE_OK) return BMK_ERR_DB;
    sqlite3_bind_int64(st, 1, b->id);

    size_t cap = 4, n = 0;
    char **tags = (char **)calloc(cap, sizeof(char *));
    if (!tags) { sqlite3_finalize(st); return BMK_ERR_NOMEM; }
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (n == cap) {
            cap *= 2;
            char **nt = (char **)realloc(tags, cap * sizeof(char *));
            if (!nt) break;
            tags = nt;
        }
        tags[n++] = bmk_strdup((const char *)sqlite3_column_text(st, 0));
    }
    sqlite3_finalize(st);
    b->tags = tags;
    b->tag_count = n;
    return BMK_OK;
}

static bmk_bookmark *row_to_bookmark(sqlite3_stmt *st)
{
    bmk_bookmark *b = (bmk_bookmark *)calloc(1, sizeof(*b));
    if (!b) return NULL;
    b->id       = sqlite3_column_int64(st, 0);
    b->title    = bmk_strdup((const char *)sqlite3_column_text(st, 1));
    b->url      = bmk_strdup((const char *)sqlite3_column_text(st, 2));
    b->url_key  = bmk_strdup((const char *)sqlite3_column_text(st, 3));

    const void *blob = sqlite3_column_blob(st, 4);
    int blen = sqlite3_column_bytes(st, 4);
    if (blob && blen > 0) {
        b->icon = (unsigned char *)malloc((size_t)blen);
        if (b->icon) {
            memcpy(b->icon, blob, (size_t)blen);
            b->icon_len = (size_t)blen;
        }
    }
    if (sqlite3_column_type(st, 5) != SQLITE_NULL)
        b->icon_mime = bmk_strdup((const char *)sqlite3_column_text(st, 5));

    b->category_id = (sqlite3_column_type(st, 6) == SQLITE_NULL)
                     ? 0 : sqlite3_column_int64(st, 6);
    b->valid = (sqlite3_column_type(st, 7) == SQLITE_NULL)
               ? BMK_VALID_UNKNOWN : sqlite3_column_int(st, 7);
    b->status_code = (sqlite3_column_type(st, 8) == SQLITE_NULL)
                     ? -1 : sqlite3_column_int(st, 8);
    b->last_checked_at = (sqlite3_column_type(st, 9) == SQLITE_NULL)
                         ? 0 : sqlite3_column_int64(st, 9);
    if (sqlite3_column_type(st, 10) != SQLITE_NULL)
        b->check_note = bmk_strdup((const char *)sqlite3_column_text(st, 10));
    b->created_at = sqlite3_column_int64(st, 11);
    b->updated_at = sqlite3_column_int64(st, 12);
    if (sqlite3_column_count(st) > 13 && sqlite3_column_type(st, 13) != SQLITE_NULL)
        b->category_name = bmk_strdup((const char *)sqlite3_column_text(st, 13));
    return b;
}

#define BOOKMARK_COLUMNS \
    "b.id, b.title, b.url, b.url_key, b.icon, b.icon_mime, b.category_id, " \
    "b.valid, b.status_code, b.last_checked_at, b.check_note, " \
    "b.created_at, b.updated_at, c.name"

#define BOOKMARK_FROM \
    " FROM bookmarks b LEFT JOIN categories c ON c.id = b.category_id "

bmk_err bmk_bookmark_insert(bmk_db *db, bmk_bookmark *b)
{
    if (!db || !b || !b->url || !b->title) return BMK_ERR_INVALID_ARG;

    char *key = NULL;
    if (!b->url_key || !*b->url_key) {
        bmk_err e = bmk_url_normalize(b->url, &key);
        if (e != BMK_OK) return e;
        free(b->url_key);
        b->url_key = key;
    }
    int64_t now = bmk_now();
    if (b->created_at <= 0) b->created_at = now;
    b->updated_at = now;

    BMK_DB_GUARD(db);
    bmk_err e = BMK_OK;
    sqlite3_stmt *st = NULL;

    const char *sql =
        "INSERT INTO bookmarks(title,url,url_key,icon,icon_mime,category_id,"
        "valid,status_code,last_checked_at,check_note,created_at,updated_at) "
        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?);";
    if (sqlite3_prepare_v2(db->h, sql, -1, &st, NULL) != SQLITE_OK) {
        set_error(db, sqlite3_errmsg(db->h));
        e = BMK_ERR_DB; goto done;
    }
    sqlite3_bind_text(st, 1, b->title, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, b->url, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, b->url_key, -1, SQLITE_TRANSIENT);
    if (b->icon && b->icon_len) sqlite3_bind_blob(st, 4, b->icon, (int)b->icon_len, SQLITE_TRANSIENT);
    else                        sqlite3_bind_null(st, 4);
    if (b->icon_mime) sqlite3_bind_text(st, 5, b->icon_mime, -1, SQLITE_TRANSIENT);
    else              sqlite3_bind_null(st, 5);
    if (b->category_id > 0) sqlite3_bind_int64(st, 6, b->category_id);
    else                    sqlite3_bind_null(st, 6);
    if (b->valid == BMK_VALID_UNKNOWN) sqlite3_bind_null(st, 7);
    else                               sqlite3_bind_int(st, 7, b->valid);
    if (b->status_code < 0) sqlite3_bind_null(st, 8);
    else                    sqlite3_bind_int(st, 8, b->status_code);
    if (b->last_checked_at <= 0) sqlite3_bind_null(st, 9);
    else                         sqlite3_bind_int64(st, 9, b->last_checked_at);
    if (b->check_note) sqlite3_bind_text(st, 10, b->check_note, -1, SQLITE_TRANSIENT);
    else               sqlite3_bind_null(st, 10);
    sqlite3_bind_int64(st, 11, b->created_at);
    sqlite3_bind_int64(st, 12, b->updated_at);

    if (sqlite3_step(st) != SQLITE_DONE) {
        set_error(db, sqlite3_errmsg(db->h));
        e = BMK_ERR_DB; goto done;
    }
    b->id = sqlite3_last_insert_rowid(db->h);
done:
    sqlite3_finalize(st);
    BMK_DB_UNLOCK(db);
    if (e != BMK_OK) return e;

    if (b->tags && b->tag_count)
        e = bmk_bookmark_set_tags(db, b->id, (const char *const *)b->tags, b->tag_count);
    return e;
}

bmk_err bmk_bookmark_update(bmk_db *db, bmk_bookmark *b)
{
    if (!db || !b || b->id <= 0 || !b->url || !b->title) return BMK_ERR_INVALID_ARG;

    /* URL 变更必须重算 url_key（FR-05 第 3 条） */
    char *key = NULL;
    bmk_err e = bmk_url_normalize(b->url, &key);
    if (e != BMK_OK) return e;
    free(b->url_key);
    b->url_key = key;
    b->updated_at = bmk_now();

    BMK_DB_GUARD(db);
    sqlite3_stmt *st = NULL;

    const char *sql =
        "UPDATE bookmarks SET title=?,url=?,url_key=?,icon=?,icon_mime=?,"
        "category_id=?,updated_at=? WHERE id=?;";
    if (sqlite3_prepare_v2(db->h, sql, -1, &st, NULL) != SQLITE_OK) {
        set_error(db, sqlite3_errmsg(db->h));
        e = BMK_ERR_DB; goto done;
    }
    sqlite3_bind_text(st, 1, b->title, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, b->url, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, b->url_key, -1, SQLITE_TRANSIENT);
    if (b->icon && b->icon_len) sqlite3_bind_blob(st, 4, b->icon, (int)b->icon_len, SQLITE_TRANSIENT);
    else                        sqlite3_bind_null(st, 4);
    if (b->icon_mime) sqlite3_bind_text(st, 5, b->icon_mime, -1, SQLITE_TRANSIENT);
    else              sqlite3_bind_null(st, 5);
    if (b->category_id > 0) sqlite3_bind_int64(st, 6, b->category_id);
    else                    sqlite3_bind_null(st, 6);
    sqlite3_bind_int64(st, 7, b->updated_at);
    sqlite3_bind_int64(st, 8, b->id);

    e = (sqlite3_step(st) == SQLITE_DONE) ? BMK_OK : BMK_ERR_DB;
    if (e != BMK_OK) set_error(db, sqlite3_errmsg(db->h));
done:
    sqlite3_finalize(st);
    BMK_DB_UNLOCK(db);
    if (e != BMK_OK) return e;

    return bmk_bookmark_set_tags(db, b->id,
                                 (const char *const *)b->tags, b->tag_count);
}

bmk_err bmk_bookmark_get(bmk_db *db, int64_t id, bmk_bookmark **out)
{
    if (!db || !out || id <= 0) return BMK_ERR_INVALID_ARG;
    BMK_DB_GUARD(db);
    *out = NULL;

    char sql[1024];
    snprintf(sql, sizeof(sql), "SELECT %s%s WHERE b.id=?;",
             BOOKMARK_COLUMNS, BOOKMARK_FROM);
    sqlite3_stmt *st = NULL;
    bmk_err e = BMK_ERR_NOT_FOUND;
    if (sqlite3_prepare_v2(db->h, sql, -1, &st, NULL) != SQLITE_OK) { e = BMK_ERR_DB; goto done; }
    sqlite3_bind_int64(st, 1, id);

    if (sqlite3_step(st) == SQLITE_ROW) {
        bmk_bookmark *b = row_to_bookmark(st);
        if (b) {
            load_tags_for(db, b);
            *out = b;
            e = BMK_OK;
        } else {
            e = BMK_ERR_NOMEM;
        }
    }
done:
    sqlite3_finalize(st);
    BMK_DB_UNLOCK(db);
    return e;
}

bmk_err bmk_bookmark_delete(bmk_db *db, int64_t id)
{
    if (!db || id <= 0) return BMK_ERR_INVALID_ARG;
    BMK_DB_GUARD(db);
    sqlite3_stmt *st = NULL;
    bmk_err e = BMK_OK;
    /* bookmark_tags 由外键 CASCADE 清理 */
    if (sqlite3_prepare_v2(db->h, "DELETE FROM bookmarks WHERE id=?;",
                           -1, &st, NULL) != SQLITE_OK) { e = BMK_ERR_DB; goto done; }
    sqlite3_bind_int64(st, 1, id);
    e = (sqlite3_step(st) == SQLITE_DONE) ? BMK_OK : BMK_ERR_DB;
    if (e != BMK_OK) set_error(db, sqlite3_errmsg(db->h));
done:
    sqlite3_finalize(st);
    BMK_DB_UNLOCK(db);
    return e;
}

bmk_err bmk_bookmark_set_category(bmk_db *db, const int64_t *ids, size_t n, int64_t category_id)
{
    if (!db || !ids) return BMK_ERR_INVALID_ARG;
    bmk_db_lock(db);
    bmk_err e = bmk_db_begin(db);
    if (e != BMK_OK) { bmk_db_unlock(db); return e; }

    int64_t now = bmk_now();
    for (size_t i = 0; i < n; i++) {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(db->h,
                "UPDATE bookmarks SET category_id=?, updated_at=? WHERE id=?;",
                -1, &st, NULL) != SQLITE_OK) { bmk_db_rollback(db); bmk_db_unlock(db); return BMK_ERR_DB; }
        if (category_id > 0) sqlite3_bind_int64(st, 1, category_id);
        else                 sqlite3_bind_null(st, 1);
        sqlite3_bind_int64(st, 2, now);
        sqlite3_bind_int64(st, 3, ids[i]);
        if (sqlite3_step(st) != SQLITE_DONE) {
            sqlite3_finalize(st);
            bmk_db_rollback(db);
            bmk_db_unlock(db);
            return BMK_ERR_DB;
        }
        sqlite3_finalize(st);
    }
    e = bmk_db_commit(db);
    bmk_db_unlock(db);
    return e;
}

bmk_err bmk_bookmark_set_check_result(bmk_db *db, int64_t id, int valid,
                                      int status_code, const char *note, int64_t checked_at)
{
    if (!db || id <= 0) return BMK_ERR_INVALID_ARG;
    BMK_DB_GUARD(db);
    /* 校验不修改 updated_at 之外的业务字段（FR-14 第 9 条） */
    const char *sql =
        "UPDATE bookmarks SET valid=?, status_code=?, check_note=?, last_checked_at=? "
        "WHERE id=?;";
    sqlite3_stmt *st = NULL;
    bmk_err e = BMK_OK;
    if (sqlite3_prepare_v2(db->h, sql, -1, &st, NULL) != SQLITE_OK) { e = BMK_ERR_DB; goto done; }
    if (valid == BMK_VALID_UNKNOWN) sqlite3_bind_null(st, 1);
    else                            sqlite3_bind_int(st, 1, valid);
    if (status_code < 0) sqlite3_bind_null(st, 2);
    else                 sqlite3_bind_int(st, 2, status_code);
    if (note) sqlite3_bind_text(st, 3, note, -1, SQLITE_TRANSIENT);
    else      sqlite3_bind_null(st, 3);
    sqlite3_bind_int64(st, 4, checked_at > 0 ? checked_at : bmk_now());
    sqlite3_bind_int64(st, 5, id);

    e = (sqlite3_step(st) == SQLITE_DONE) ? BMK_OK : BMK_ERR_DB;
    if (e != BMK_OK) set_error(db, sqlite3_errmsg(db->h));
done:
    sqlite3_finalize(st);
    BMK_DB_UNLOCK(db);
    return e;
}

bmk_err bmk_bookmark_set_icon(bmk_db *db, int64_t id,
                              const unsigned char *icon, size_t len, const char *mime)
{
    if (!db || id <= 0) return BMK_ERR_INVALID_ARG;
    BMK_DB_GUARD(db);
    sqlite3_stmt *st = NULL;
    bmk_err e = BMK_OK;
    if (sqlite3_prepare_v2(db->h,
            "UPDATE bookmarks SET icon=?, icon_mime=?, updated_at=? WHERE id=?;",
            -1, &st, NULL) != SQLITE_OK) { e = BMK_ERR_DB; goto done; }
    if (icon && len) sqlite3_bind_blob(st, 1, icon, (int)len, SQLITE_TRANSIENT);
    else             sqlite3_bind_null(st, 1);
    if (mime) sqlite3_bind_text(st, 2, mime, -1, SQLITE_TRANSIENT);
    else      sqlite3_bind_null(st, 2);
    sqlite3_bind_int64(st, 3, bmk_now());
    sqlite3_bind_int64(st, 4, id);
    e = (sqlite3_step(st) == SQLITE_DONE) ? BMK_OK : BMK_ERR_DB;
done:
    sqlite3_finalize(st);
    BMK_DB_UNLOCK(db);
    return e;
}

int64_t bmk_bookmark_count_by_key(bmk_db *db, const char *url_key, char **out_title)
{
    if (out_title) *out_title = NULL;
    if (!db || !url_key) return 0;
    BMK_DB_GUARD_RET(db, 0);

    sqlite3_stmt *st = NULL;
    int64_t c = 0;
    if (sqlite3_prepare_v2(db->h,
            "SELECT COUNT(*), MIN(title) FROM bookmarks WHERE url_key=?;",
            -1, &st, NULL) != SQLITE_OK) goto done;
    sqlite3_bind_text(st, 1, url_key, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) {
        c = sqlite3_column_int64(st, 0);
        if (out_title && c > 0 && sqlite3_column_type(st, 1) != SQLITE_NULL)
            *out_title = bmk_strdup((const char *)sqlite3_column_text(st, 1));
    }
done:
    sqlite3_finalize(st);
    BMK_DB_UNLOCK(db);
    return c;
}

/* ------------------------------------------------------------------ */
/* 查询：筛选 + 排序（FR-09 / FR-12）                                  */
/* ------------------------------------------------------------------ */

/*
 * 排序表达式（FR-09 第 2 条）：
 * - 时间/状态码：NULL 在升序与降序中都排最后
 * - 有效性：升序 0 → NULL → 1；降序 1 → NULL → 0
 * - 分类：按分类名，NULL（未分类）排最后
 * - 恒有二级键 id ASC
 */
static void append_order_by(bmk_buf *sql, bmk_sort_key key, bool asc)
{
    const char *dir = asc ? "ASC" : "DESC";
    switch (key) {
    case BMK_SORT_TITLE:
        bmk_buf_printf(sql, " ORDER BY b.title COLLATE NOCASE %s", dir);
        break;
    case BMK_SORT_URL:
        bmk_buf_printf(sql, " ORDER BY b.url COLLATE NOCASE %s", dir);
        break;
    case BMK_SORT_CREATED_AT:
        /* NULL 不会出现（NOT NULL），但保持统一写法 */
        bmk_buf_printf(sql, " ORDER BY (b.created_at IS NULL) ASC, b.created_at %s", dir);
        break;
    case BMK_SORT_CATEGORY:
        bmk_buf_printf(sql, " ORDER BY (c.name IS NULL) ASC, c.name COLLATE NOCASE %s", dir);
        break;
    case BMK_SORT_VALID:
        /* 升序 0,NULL,1 → 权重 0,1,2；降序 1,NULL,0 → 权重取反 */
        if (asc)
            bmk_buf_append_str(sql,
                " ORDER BY CASE WHEN b.valid=0 THEN 0 WHEN b.valid IS NULL THEN 1 ELSE 2 END ASC");
        else
            bmk_buf_append_str(sql,
                " ORDER BY CASE WHEN b.valid=1 THEN 0 WHEN b.valid IS NULL THEN 1 ELSE 2 END ASC");
        break;
    case BMK_SORT_STATUS_CODE:
        bmk_buf_printf(sql,
            " ORDER BY (b.status_code IS NULL) ASC, b.status_code %s", dir);
        break;
    case BMK_SORT_LAST_CHECKED:
        bmk_buf_printf(sql,
            " ORDER BY (b.last_checked_at IS NULL) ASC, b.last_checked_at %s", dir);
        break;
    }
    bmk_buf_append_str(sql, ", b.id ASC");
}

bmk_err bmk_bookmark_query(bmk_db *db, const bmk_filter *f,
                           bmk_sort_key sort, bool ascending,
                           bmk_bookmark ***out, size_t *out_n)
{
    if (!db || !out || !out_n) return BMK_ERR_INVALID_ARG;
    BMK_DB_GUARD(db);
    *out = NULL; *out_n = 0;

    bmk_filter def;
    if (!f) { bmk_filter_init(&def); f = &def; }

    bmk_buf sql;
    bmk_buf_init(&sql);
    bmk_buf_printf(&sql, "SELECT %s%s WHERE 1=1", BOOKMARK_COLUMNS, BOOKMARK_FROM);

    /* 维度之间 AND，维度内部 OR（FR-12 第 2 条） */
    if (f->keyword && *f->keyword)
        bmk_buf_append_str(&sql, " AND (b.title LIKE ?1 ESCAPE '\\' OR b.url LIKE ?1 ESCAPE '\\')");
    if (f->url_contains && *f->url_contains)
        bmk_buf_append_str(&sql, " AND b.url LIKE ?2 ESCAPE '\\'");
    if (f->domain && *f->domain)
        bmk_buf_append_str(&sql, " AND b.url_key LIKE ?3 ESCAPE '\\'");

    if (f->category_id == BMK_CATEGORY_UNCATEGORIZED) {
        bmk_buf_append_str(&sql, " AND b.category_id IS NULL");
    } else if (f->category_id > 0) {
        /* 选中即包含其全部子孙分类（FR-12 第 1 条） */
        bmk_buf_printf(&sql,
            " AND b.category_id IN ("
            "  WITH RECURSIVE tree(id) AS ("
            "    SELECT id FROM categories WHERE id=%lld"
            "    UNION ALL"
            "    SELECT c2.id FROM categories c2 JOIN tree t ON c2.parent_id=t.id"
            "  ) SELECT id FROM tree)",
            (long long)f->category_id);
    }

    switch (f->valid_filter) {
    case BMK_VF_ALL: break;
    case BMK_VF_VALID:
        bmk_buf_append_str(&sql, " AND b.valid=1"); break;
    case BMK_VF_INVALID:
        bmk_buf_append_str(&sql, " AND b.valid=0"); break;
    case BMK_VF_UNCHECKED:
        /* 未校验 = valid 为 NULL（含 403 保持 NULL 的待复测条目） */
        bmk_buf_append_str(&sql, " AND b.valid IS NULL"); break;
    case BMK_VF_BLOCKED:
        bmk_buf_append_str(&sql, " AND b.check_note='" BMK_NOTE_BLOCKED "'"); break;
    }

    if (f->status_min >= 0)
        bmk_buf_printf(&sql, " AND b.status_code >= %d", f->status_min);
    if (f->status_max >= 0)
        bmk_buf_printf(&sql, " AND b.status_code <= %d", f->status_max);

    if (f->tag_ids && f->tag_count > 0) {
        /* 标签之间 OR */
        bmk_buf_append_str(&sql,
            " AND EXISTS (SELECT 1 FROM bookmark_tags bt WHERE bt.bookmark_id=b.id AND bt.tag_id IN (");
        for (size_t i = 0; i < f->tag_count; i++)
            bmk_buf_printf(&sql, "%s%lld", i ? "," : "", (long long)f->tag_ids[i]);
        bmk_buf_append_str(&sql, "))");
    }

    append_order_by(&sql, sort, ascending);
    bmk_buf_append_char(&sql, ';');

    bmk_err e = BMK_OK;
    sqlite3_stmt *st = NULL;
    bmk_bookmark **list = NULL;
    char *p1 = NULL, *p2 = NULL, *p3 = NULL;
    size_t n = 0;

    if (sqlite3_prepare_v2(db->h, sql.data, -1, &st, NULL) != SQLITE_OK) {
        set_error(db, sqlite3_errmsg(db->h));
        BMK_ERROR("query prepare failed: %s", sqlite3_errmsg(db->h));
        e = BMK_ERR_DB; goto done;
    }
    bmk_buf_free(&sql);

    /* LIKE 参数：转义 % 与 _ 后包裹 %...% */
    #define MAKE_LIKE(dst, src)                                    \
        do {                                                       \
            bmk_buf tmp; bmk_buf_init(&tmp);                       \
            bmk_buf_append_char(&tmp, '%');                        \
            for (const char *q = (src); *q; q++) {                 \
                if (*q == '%' || *q == '_' || *q == '\\')          \
                    bmk_buf_append_char(&tmp, '\\');               \
                bmk_buf_append_char(&tmp, *q);                     \
            }                                                      \
            bmk_buf_append_char(&tmp, '%');                        \
            (dst) = bmk_buf_steal(&tmp);                           \
        } while (0)

    if (f->keyword && *f->keyword) {
        MAKE_LIKE(p1, f->keyword);
        sqlite3_bind_text(st, 1, p1, -1, SQLITE_STATIC);
    }
    if (f->url_contains && *f->url_contains) {
        MAKE_LIKE(p2, f->url_contains);
        sqlite3_bind_text(st, 2, p2, -1, SQLITE_STATIC);
    }
    if (f->domain && *f->domain) {
        MAKE_LIKE(p3, f->domain);
        sqlite3_bind_text(st, 3, p3, -1, SQLITE_STATIC);
    }
    #undef MAKE_LIKE

    size_t cap = 64;
    list = (bmk_bookmark **)calloc(cap, sizeof(*list));
    if (!list) { e = BMK_ERR_NOMEM; goto done; }
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (n == cap) {
            cap *= 2;
            bmk_bookmark **nl = (bmk_bookmark **)realloc(list, cap * sizeof(*list));
            if (!nl) break;
            list = nl;
        }
        bmk_bookmark *b = row_to_bookmark(st);
        if (!b) break;
        load_tags_for(db, b);
        list[n++] = b;
    }
    *out = list;
    *out_n = n;
    list = NULL;   /* 转移所有权 */
done:
    sqlite3_finalize(st);
    free(p1); free(p2); free(p3);
    BMK_DB_UNLOCK(db);
    return e;
}

bmk_err bmk_db_stats(bmk_db *db, bmk_stats *out)
{
    if (!db || !out) return BMK_ERR_INVALID_ARG;
    BMK_DB_GUARD(db);
    memset(out, 0, sizeof(*out));
    const char *sql =
        "SELECT COUNT(*),"
        " SUM(CASE WHEN valid=0 THEN 1 ELSE 0 END),"
        " SUM(CASE WHEN check_note='" BMK_NOTE_BLOCKED "' THEN 1 ELSE 0 END),"
        " SUM(CASE WHEN valid IS NULL THEN 1 ELSE 0 END)"
        " FROM bookmarks;";
    sqlite3_stmt *st = NULL;
    bmk_err e = BMK_OK;
    if (sqlite3_prepare_v2(db->h, sql, -1, &st, NULL) != SQLITE_OK) { e = BMK_ERR_DB; goto done; }
    if (sqlite3_step(st) == SQLITE_ROW) {
        out->total     = sqlite3_column_int64(st, 0);
        out->invalid   = sqlite3_column_int64(st, 1);
        out->blocked   = sqlite3_column_int64(st, 2);
        out->unchecked = sqlite3_column_int64(st, 3);
    }
done:
    sqlite3_finalize(st);
    BMK_DB_UNLOCK(db);
    return e;
}

/* ------------------------------------------------------------------ */
/* 查重                                                                */
/* ------------------------------------------------------------------ */

void bmk_dup_group_list_free(bmk_dup_group **list, size_t n)
{
    if (!list) return;
    for (size_t i = 0; i < n; i++) {
        if (!list[i]) continue;
        free(list[i]->url_key);
        bmk_bookmark_list_free(list[i]->members, list[i]->count);
        free(list[i]);
    }
    free(list);
}

bmk_err bmk_dedupe_find(bmk_db *db, bmk_dup_group ***out, size_t *out_n)
{
    if (!db || !out || !out_n) return BMK_ERR_INVALID_ARG;
    BMK_DB_GUARD(db);
    *out = NULL; *out_n = 0;

    /* FR-15 第 2 条的分组语句 */
    const char *gsql =
        "SELECT url_key FROM bookmarks GROUP BY url_key HAVING COUNT(*) > 1 "
        "ORDER BY url_key;";
    sqlite3_stmt *st = NULL;
    bmk_err e = BMK_OK;
    if (sqlite3_prepare_v2(db->h, gsql, -1, &st, NULL) != SQLITE_OK) { e = BMK_ERR_DB; goto done; }

    size_t cap = 8, n = 0;
    bmk_dup_group **groups = (bmk_dup_group **)calloc(cap, sizeof(*groups));
    if (!groups) { e = BMK_ERR_NOMEM; goto done; }

    while (sqlite3_step(st) == SQLITE_ROW) {
        if (n == cap) {
            cap *= 2;
            bmk_dup_group **ng = (bmk_dup_group **)realloc(groups, cap * sizeof(*groups));
            if (!ng) break;
            groups = ng;
        }
        bmk_dup_group *g = (bmk_dup_group *)calloc(1, sizeof(*g));
        if (!g) break;
        g->url_key = bmk_strdup((const char *)sqlite3_column_text(st, 0));
        groups[n++] = g;
    }
    sqlite3_finalize(st); st = NULL;

    /* 逐组取成员（按 created_at, id 升序，便于主记录选择） */
    char sql[1024];
    snprintf(sql, sizeof(sql),
             "SELECT %s%s WHERE b.url_key=? ORDER BY b.created_at ASC, b.id ASC;",
             BOOKMARK_COLUMNS, BOOKMARK_FROM);
    for (size_t i = 0; i < n; i++) {
        sqlite3_stmt *ms = NULL;
        if (sqlite3_prepare_v2(db->h, sql, -1, &ms, NULL) != SQLITE_OK) continue;
        sqlite3_bind_text(ms, 1, groups[i]->url_key, -1, SQLITE_TRANSIENT);

        size_t mcap = 4, mn = 0;
        bmk_bookmark **members = (bmk_bookmark **)calloc(mcap, sizeof(*members));
        if (!members) { sqlite3_finalize(ms); continue; }
        while (sqlite3_step(ms) == SQLITE_ROW) {
            if (mn == mcap) {
                mcap *= 2;
                bmk_bookmark **nm = (bmk_bookmark **)realloc(members, mcap * sizeof(*members));
                if (!nm) break;
                members = nm;
            }
            bmk_bookmark *b = row_to_bookmark(ms);
            if (!b) break;
            load_tags_for(db, b);
            members[mn++] = b;
        }
        sqlite3_finalize(ms);
        groups[i]->members = members;
        groups[i]->count = mn;
    }

    *out = groups;
    *out_n = n;
    groups = NULL;
done:
    sqlite3_finalize(st);
    BMK_DB_UNLOCK(db);
    return e;
}

bmk_err bmk_deleted_log_add(bmk_db *db, const bmk_bookmark *b, const char *backup_file)
{
    if (!db || !b) return BMK_ERR_INVALID_ARG;
    BMK_DB_GUARD(db);

    /* 标签以逗号连接存档 */
    bmk_buf tagbuf;
    bmk_buf_init(&tagbuf);
    for (size_t i = 0; i < b->tag_count; i++) {
        if (i) bmk_buf_append_char(&tagbuf, ',');
        bmk_buf_append_str(&tagbuf, b->tags[i]);
    }

    const char *sql =
        "INSERT INTO deleted_log(bookmark_id,title,url,url_key,category_id,tags,"
        "deleted_at,backup_file) VALUES(?,?,?,?,?,?,?,?);";
    sqlite3_stmt *st = NULL;
    bmk_err e = BMK_OK;
    if (sqlite3_prepare_v2(db->h, sql, -1, &st, NULL) != SQLITE_OK) {
        e = BMK_ERR_DB; goto done;
    }
    sqlite3_bind_int64(st, 1, b->id);
    sqlite3_bind_text(st, 2, b->title ? b->title : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, b->url ? b->url : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, b->url_key ? b->url_key : "", -1, SQLITE_TRANSIENT);
    if (b->category_id > 0) sqlite3_bind_int64(st, 5, b->category_id);
    else                    sqlite3_bind_null(st, 5);
    sqlite3_bind_text(st, 6, tagbuf.data ? tagbuf.data : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 7, bmk_now());
    if (backup_file) sqlite3_bind_text(st, 8, backup_file, -1, SQLITE_TRANSIENT);
    else             sqlite3_bind_null(st, 8);

    e = (sqlite3_step(st) == SQLITE_DONE) ? BMK_OK : BMK_ERR_DB;
    if (e != BMK_OK) set_error(db, sqlite3_errmsg(db->h));
done:
    sqlite3_finalize(st);
    bmk_buf_free(&tagbuf);
    BMK_DB_UNLOCK(db);
    return e;
}
