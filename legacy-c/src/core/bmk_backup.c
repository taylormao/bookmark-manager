/*
 * bmk_backup.c - 备份、删除与合并实现
 */
#include "bmk_backup.h"
#include "bmk_db.h"
#include "bmk_log.h"
#include "bmk_netscape.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <dirent.h>
#  include <unistd.h>
#endif

static char *g_backup_dir = NULL;

bmk_err bmk_backup_set_dir(const char *backups_dir)
{
    if (!backups_dir) return BMK_ERR_INVALID_ARG;
    free(g_backup_dir);
    g_backup_dir = bmk_strdup(backups_dir);
    if (!g_backup_dir) return BMK_ERR_NOMEM;
    return bmk_mkdirs(g_backup_dir);
}

const char *bmk_backup_get_dir(void) { return g_backup_dir; }

/* 构造 <backups>/<prefix>_<时间戳>.<ext> */
static char *make_backup_path(const char *prefix, const char *ext)
{
    if (!g_backup_dir) return NULL;
    char ts[32];
    bmk_timestamp_str(bmk_now(), ts, sizeof(ts));
    char name[256];
    snprintf(name, sizeof(name), "%s_%s.%s", prefix, ts, ext);
    return bmk_path_join(g_backup_dir, name);
}

bmk_err bmk_backup_write_raw(const char *prefix, const char *ext,
                             const void *data, size_t len, char **out_path)
{
    if (out_path) *out_path = NULL;
    char *path = make_backup_path(prefix, ext ? ext : "html");
    if (!path) return BMK_ERR_IO;

    bmk_err e = bmk_write_file(path, data, len);
    if (e != BMK_OK) { free(path); return e; }

    BMK_INFO("backup written: %s", path);
    if (out_path) *out_path = path;
    else          free(path);
    return BMK_OK;
}

bmk_err bmk_backup_snapshot_html(bmk_db *db, const char *prefix, char **out_path)
{
    if (!db) return BMK_ERR_INVALID_ARG;
    char *html = NULL;
    bmk_err e = bmk_export_html_string(db, NULL, 0, &html);
    if (e != BMK_OK) return e;
    e = bmk_backup_write_raw(prefix ? prefix : "backup", "html",
                             html, strlen(html), out_path);
    free(html);
    return e;
}

/* ------------------------------------------------------------------ */
/* 删除（FR-06）                                                       */
/* ------------------------------------------------------------------ */

bmk_err bmk_backup_delete_bookmarks(bmk_db *db, const int64_t *ids, size_t n,
                                    char **out_backup_path)
{
    if (!db || !ids || n == 0) return BMK_ERR_INVALID_ARG;
    if (out_backup_path) *out_backup_path = NULL;

    /* 载入待删记录 */
    bmk_bookmark **items = (bmk_bookmark **)calloc(n, sizeof(*items));
    if (!items) return BMK_ERR_NOMEM;
    size_t cnt = 0;
    for (size_t i = 0; i < n; i++) {
        bmk_bookmark *b = NULL;
        if (bmk_bookmark_get(db, ids[i], &b) == BMK_OK && b) items[cnt++] = b;
    }
    if (cnt == 0) { free(items); return BMK_ERR_NOT_FOUND; }

    /*
     * 先写备份文件：写入失败时中止删除（不允许无备份删除，FR-06 边界）。
     */
    char *html = NULL;
    bmk_err e = bmk_export_html_string(db, items, cnt, &html);
    if (e != BMK_OK) {
        bmk_bookmark_list_free(items, cnt);
        return e;
    }
    char *backup_path = NULL;
    e = bmk_backup_write_raw("deleted", "html", html, strlen(html), &backup_path);
    free(html);
    if (e != BMK_OK) {
        BMK_ERROR("delete aborted: backup file could not be written");
        bmk_bookmark_list_free(items, cnt);
        return e;
    }

    /* 事务：写 deleted_log + 删除记录。
     * bmk_db_begin/commit 不持有锁，由调用者控制；
     * deleted_log_add / bookmark_delete 自行加锁 → 调用前需解锁避免死锁。 */
    bmk_db_lock(db);
    e = bmk_db_begin(db);
    if (e != BMK_OK) {
        bmk_db_unlock(db);
        free(backup_path);
        bmk_bookmark_list_free(items, cnt);
        return e;
    }
    for (size_t i = 0; i < cnt; i++) {
        bmk_db_unlock(db);
        e = bmk_deleted_log_add(db, items[i], backup_path);
        if (e != BMK_OK) { bmk_db_lock(db); break; }
        e = bmk_bookmark_delete(db, items[i]->id);
        bmk_db_lock(db);
        if (e != BMK_OK) break;
    }
    if (e != BMK_OK) {
        bmk_db_rollback(db);
        bmk_db_unlock(db);
        free(backup_path);
        bmk_bookmark_list_free(items, cnt);
        return e;
    }
    e = bmk_db_commit(db);
    bmk_db_unlock(db);
    bmk_bookmark_list_free(items, cnt);

    if (e != BMK_OK) { free(backup_path); return e; }

    BMK_INFO("deleted %zu bookmark(s), backup=%s", cnt, backup_path);
    if (out_backup_path) *out_backup_path = backup_path;
    else                 free(backup_path);
    return BMK_OK;
}

/* ------------------------------------------------------------------ */
/* 合并（FR-16 第 3 条）                                               */
/* ------------------------------------------------------------------ */

bmk_err bmk_merge_group(bmk_db *db, const int64_t *ids, size_t n,
                        int64_t *out_primary_id)
{
    if (!db || !ids || n < 2) return BMK_ERR_INVALID_ARG;

    bmk_bookmark **items = (bmk_bookmark **)calloc(n, sizeof(*items));
    if (!items) return BMK_ERR_NOMEM;
    size_t cnt = 0;
    for (size_t i = 0; i < n; i++) {
        bmk_bookmark *b = NULL;
        if (bmk_bookmark_get(db, ids[i], &b) == BMK_OK && b) items[cnt++] = b;
    }
    /* 组内记录已被外部修改/删除 → 跳过该组（FR-16 第 5 条） */
    if (cnt < 2) {
        bmk_bookmark_list_free(items, cnt);
        return BMK_ERR_CONFLICT;
    }

    /* 主记录 = created_at 最早者，并列取 id 最小 */
    size_t pidx = 0;
    for (size_t i = 1; i < cnt; i++) {
        if (items[i]->created_at < items[pidx]->created_at ||
            (items[i]->created_at == items[pidx]->created_at &&
             items[i]->id < items[pidx]->id))
            pidx = i;
    }
    bmk_bookmark *primary = items[pidx];

    /* icon：主记录为 NULL 时取组内第一条非 NULL */
    const bmk_bookmark *icon_src = NULL;
    if (!primary->icon) {
        for (size_t i = 0; i < cnt; i++) {
            if (items[i]->icon) { icon_src = items[i]; break; }
        }
    }

    /* category_id：主记录优先，NULL 则取组内最早的非 NULL */
    int64_t category = primary->category_id;
    if (category == 0) {
        int64_t best_created = 0;
        for (size_t i = 0; i < cnt; i++) {
            if (items[i]->category_id == 0) continue;
            if (!category || items[i]->created_at < best_created) {
                category = items[i]->category_id;
                best_created = items[i]->created_at;
            }
        }
    }

    /* 标签：组内并集 */
    char **tags = NULL;
    size_t ntags = 0;
    for (size_t i = 0; i < cnt; i++) {
        for (size_t t = 0; t < items[i]->tag_count; t++) {
            bool dup = false;
            for (size_t k = 0; k < ntags; k++) {
                if (bmk_strcasecmp(tags[k], items[i]->tags[t]) == 0) { dup = true; break; }
            }
            if (dup) continue;
            char **nt = (char **)realloc(tags, (ntags + 1) * sizeof(char *));
            if (!nt) break;
            tags = nt;
            tags[ntags++] = bmk_strdup(items[i]->tags[t]);
        }
    }

    /* 校验字段：取组内 last_checked_at 最新者 */
    const bmk_bookmark *check_src = NULL;
    for (size_t i = 0; i < cnt; i++) {
        if (items[i]->last_checked_at <= 0) continue;
        if (!check_src || items[i]->last_checked_at > check_src->last_checked_at)
            check_src = items[i];
    }

    /* 合并前生成备份并写 deleted_log（FR-16 第 4 条） */
    bmk_bookmark **victims = (bmk_bookmark **)calloc(cnt, sizeof(*victims));
    if (!victims) {
        for (size_t i = 0; i < ntags; i++) free(tags[i]);
        free(tags);
        bmk_bookmark_list_free(items, cnt);
        return BMK_ERR_NOMEM;
    }
    size_t nvic = 0;
    for (size_t i = 0; i < cnt; i++)
        if (i != pidx) victims[nvic++] = items[i];

    char *html = NULL;
    bmk_err e = bmk_export_html_string(db, victims, nvic, &html);
    char *backup_path = NULL;
    if (e == BMK_OK) {
        e = bmk_backup_write_raw("merged", "html", html, strlen(html), &backup_path);
        free(html);
    }
    if (e != BMK_OK) {
        free(victims);
        for (size_t i = 0; i < ntags; i++) free(tags[i]);
        free(tags);
        bmk_bookmark_list_free(items, cnt);
        return e;
    }

    /* 单事务执行合并。
     * 事务内的 CRUD 函数自行加锁，故每次调用前解锁、调用后重锁。 */
    bmk_db_lock(db);
    e = bmk_db_begin(db);
    if (e == BMK_OK) {
        /* 更新主记录 */
        free(primary->category_name);
        primary->category_name = NULL;
        primary->category_id = category;
        if (icon_src) {
            free(primary->icon);
            primary->icon = (unsigned char *)malloc(icon_src->icon_len);
            if (primary->icon) {
                memcpy(primary->icon, icon_src->icon, icon_src->icon_len);
                primary->icon_len = icon_src->icon_len;
            } else {
                primary->icon_len = 0;  /* Bug#4：malloc 失败时清零 icon_len */
            }
            free(primary->icon_mime);
            primary->icon_mime = bmk_strdup(icon_src->icon_mime);
        }
        /* 用并集替换主记录标签 */
        for (size_t i = 0; i < primary->tag_count; i++) free(primary->tags[i]);
        free(primary->tags);
        primary->tags = tags;
        primary->tag_count = ntags;
        tags = NULL;
        ntags = 0;

        bmk_db_unlock(db);
        e = bmk_bookmark_update(db, primary);   /* 内部刷新 updated_at */
        bmk_db_lock(db);

        /* 校验字段来自最新者 */
        if (e == BMK_OK && check_src && check_src != primary) {
            bmk_db_unlock(db);
            e = bmk_bookmark_set_check_result(db, primary->id, check_src->valid,
                                              check_src->status_code,
                                              check_src->check_note,
                                              check_src->last_checked_at);
            bmk_db_lock(db);
        }

        /* 删除其余成员并留痕 */
        for (size_t i = 0; i < nvic && e == BMK_OK; i++) {
            bmk_db_unlock(db);
            e = bmk_deleted_log_add(db, victims[i], backup_path);
            if (e == BMK_OK) e = bmk_bookmark_delete(db, victims[i]->id);
            bmk_db_lock(db);
        }

        if (e == BMK_OK) e = bmk_db_commit(db);
        else             bmk_db_rollback(db);
    }
    bmk_db_unlock(db);

    if (e == BMK_OK) {
        if (out_primary_id) *out_primary_id = primary->id;
        BMK_INFO("merged %zu records into #%lld", cnt, (long long)primary->id);
    }

    free(backup_path);
    free(victims);
    for (size_t i = 0; i < ntags; i++) free(tags[i]);
    free(tags);
    bmk_bookmark_list_free(items, cnt);
    return e;
}

/* ------------------------------------------------------------------ */
/* 全量备份与清理                                                      */
/* ------------------------------------------------------------------ */

bmk_err bmk_backup_full(bmk_db *db, const char *db_path)
{
    if (!db) return BMK_ERR_INVALID_ARG;

    bmk_err e = bmk_backup_snapshot_html(db, "bookmarks", NULL);
    if (e != BMK_OK) return e;

    /* 数据库副本 */
    if (db_path && bmk_file_exists(db_path)) {
        char *data = NULL;
        size_t len = 0;
        if (bmk_read_file(db_path, &data, &len) == BMK_OK) {
            e = bmk_backup_write_raw("bookmarks", "db", data, len, NULL);
            free(data);
        }
    }
    return e;
}

/* 全量备份文件名形如 bookmarks_<时间戳>.html / bookmarks_<时间戳>.db，
 * 提取时间戳作为快照标识；非全量备份返回 NULL。 */
static char *full_snapshot_key(const char *name)
{
    if (!name || strncmp(name, "bookmarks_", 10) != 0) return NULL;
    char *key = bmk_strdup(name + 10);
    if (!key) return NULL;
    char *dot = strrchr(key, '.');
    if (dot) *dot = '\0';
    return key;
}

bmk_err bmk_backup_cleanup(void)
{
    if (!g_backup_dir) return BMK_ERR_INVALID_ARG;

    /*
     * 保留最近 20 个全量快照与 7 天内的自动备份（FR-21 第 5 条）。
     * 全量快照 = bookmarks_*.html + bookmarks_*.db 成对出现，
     * 同一时间戳的两份文件算一个快照（按时间戳前缀归组计数）。
     * 自动备份 = deleted_* / merged_* / remote_* / premigrate_*
     */
    const int64_t week_ago = bmk_now() - 7 * 24 * 3600;

    typedef struct { char *path; char *key; int64_t mtime; bool is_full; } entry;
    entry *entries = NULL;
    size_t n = 0, cap = 0;

#ifdef _WIN32
    char pattern[1024];
    snprintf(pattern, sizeof(pattern), "%s\\*", g_backup_dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return BMK_OK;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const char *name = fd.cFileName;
#else
    DIR *d = opendir(g_backup_dir);
    if (!d) return BMK_OK;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const char *name = de->d_name;
        if (name[0] == '.') continue;
#endif
        char *full = bmk_path_join(g_backup_dir, name);
        if (!full) continue;
        struct stat stbuf;
        if (stat(full, &stbuf) != 0) { free(full); continue; }

        if (n == cap) {
            cap = cap ? cap * 2 : 32;
            entry *ne = (entry *)realloc(entries, cap * sizeof(entry));
            if (!ne) { free(full); break; }
            entries = ne;
        }
        entries[n].path = full;
        entries[n].key = full_snapshot_key(name);
        entries[n].mtime = (int64_t)stbuf.st_mtime;
        entries[n].is_full = (entries[n].key != NULL);
        n++;
#ifdef _WIN32
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    }
    closedir(d);
#endif

    /* 全量快照按时间降序，保留最近 20 个快照 */
    for (size_t i = 0; i < n; i++) {
        for (size_t j = i + 1; j < n; j++) {
            if (entries[j].mtime > entries[i].mtime) {
                entry t = entries[i]; entries[i] = entries[j]; entries[j] = t;
            }
        }
    }

    char *kept[20];   /* 已保留快照的时间戳 key（最多 20 个） */
    size_t kept_n = 0;
    int removed = 0;
    for (size_t i = 0; i < n; i++) {
        bool remove_it = false;
        if (entries[i].is_full) {
            bool seen = false;
            for (size_t k = 0; k < kept_n; k++) {
                if (strcmp(kept[k], entries[i].key) == 0) { seen = true; break; }
            }
            if (seen) {
                /* 同一快照的另一份文件（html/db），随快照保留 */
            } else if (kept_n < 20) {
                kept[kept_n++] = entries[i].key;   /* 新增保留快照 */
            } else {
                remove_it = true;   /* 超出 20 个快照的整组清理 */
            }
        } else if (entries[i].mtime < week_ago) {
            remove_it = true;
        }
        if (remove_it && remove(entries[i].path) == 0) removed++;
        free(entries[i].path);
        free(entries[i].key);
    }
    free(entries);

    if (removed) BMK_INFO("backup cleanup removed %d file(s)", removed);
    return BMK_OK;
}
