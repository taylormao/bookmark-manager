/*
 * bmk_app.c - 应用装配层实现（FR-01）
 */
#include "bmk_app.h"
#include "bmk_log.h"
#include "bmk_settings.h"
#include "bmk_backup.h"
#include "bmk_proxy.h"
#include "bmk_fetch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <windows.h>
#  include <direct.h>
#else
#  include <unistd.h>
#endif

bool bmk_app_dir_writable(const char *dir)
{
    if (!dir || !*dir) return false;
    if (bmk_mkdirs(dir) != BMK_OK) return false;

    char *probe = bmk_path_join(dir, ".bmk_write_test");
    if (!probe) return false;

    bool ok = (bmk_write_file(probe, "1", 1) == BMK_OK);
    if (ok) remove(probe);
    free(probe);
    return ok;
}

/* 把损坏的数据库改名保留，返回新分配的新路径。 */
static char *rename_corrupt_db(const char *db_path)
{
    char ts[32];
    bmk_timestamp_str(bmk_now(), ts, sizeof ts);

    size_t n = strlen(db_path) + 64;
    char *newp = (char *)malloc(n);
    if (!newp) return NULL;
    snprintf(newp, n, "%s.corrupt_%s", db_path, ts);

    if (rename(db_path, newp) != 0) {
        free(newp);
        return NULL;
    }
    /* WAL/SHM 一并清理，避免新库继承坏日志。 */
    char side[1024];
    snprintf(side, sizeof side, "%s-wal", db_path); remove(side);
    snprintf(side, sizeof side, "%s-shm", db_path); remove(side);
    return newp;
}

static void app_free_paths(bmk_app *a)
{
    free(a->data_dir);      a->data_dir = NULL;
    free(a->db_path);       a->db_path = NULL;
    free(a->backups_dir);   a->backups_dir = NULL;
    free(a->logs_dir);      a->logs_dir = NULL;
    free(a->sync_dir);      a->sync_dir = NULL;
    free(a->recovered_from);a->recovered_from = NULL;
}

bmk_err bmk_app_startup(const char *data_dir, bmk_app *out)
{
    if (!out) return BMK_ERR_INVALID_ARG;
    memset(out, 0, sizeof *out);

    out->data_dir = data_dir && *data_dir ? bmk_strdup(data_dir)
                                          : bmk_settings_default_data_dir();
    if (!out->data_dir) return BMK_ERR_NOMEM;

    if (!bmk_app_dir_writable(out->data_dir)) {
        app_free_paths(out);
        return BMK_ERR_PERMISSION;
    }

    out->db_path     = bmk_path_join(out->data_dir, "bookmarks.db");
    out->backups_dir = bmk_path_join(out->data_dir, "backups");
    out->logs_dir    = bmk_path_join(out->data_dir, "logs");
    out->sync_dir    = bmk_path_join(out->data_dir, "sync");
    if (!out->db_path || !out->backups_dir || !out->logs_dir || !out->sync_dir) {
        app_free_paths(out);
        return BMK_ERR_NOMEM;
    }

    bmk_mkdirs(out->backups_dir);
    bmk_mkdirs(out->logs_dir);
    bmk_mkdirs(out->sync_dir);

    bmk_log_init(out->logs_dir, BMK_LOG_INFO);
    bmk_backup_set_dir(out->backups_dir);
    bmk_net_global_init();

    bmk_err e = bmk_db_open(out->db_path, &out->db);
    if (e == BMK_ERR_DB_CORRUPT) {
        /* FR-01 第 2 条：提示损坏并从最近备份恢复；此处保留坏库并新建空库，
         * 由界面/CLI 引导用户导入 backups 中的 HTML。 */
        BMK_ERROR("database corrupted, creating a fresh one");
        out->recovered_from = rename_corrupt_db(out->db_path);
        e = bmk_db_open(out->db_path, &out->db);
        out->db_recovered = (e == BMK_OK);
    }
    if (e != BMK_OK) {
        bmk_net_global_cleanup();
        bmk_log_shutdown();
        app_free_paths(out);
        return e;
    }

    /* 首次运行落盘数据目录，便于下次沿用。 */
    char *saved = bmk_settings_get_str(out->db, BMK_SET_DATA_DIR, NULL);
    if (!saved) bmk_settings_set_str(out->db, BMK_SET_DATA_DIR, out->data_dir);
    free(saved);

    /* 代理节点池与轮换策略。 */
    char *xray = bmk_settings_get_str(out->db, BMK_SET_XRAY_PATH, NULL);
    if (xray && *xray) bmk_proxy_set_xray_path(xray);
    free(xray);

    char *rot = bmk_settings_get_str(out->db, BMK_SET_PROXY_ROTATION, "round");
    if (rot) {
        if (strcmp(rot, "random") == 0)        bmk_proxy_set_rotation(BMK_ROTATE_RANDOM);
        else if (strcmp(rot, "failover") == 0) bmk_proxy_set_rotation(BMK_ROTATE_FAILOVER);
        else if (strcmp(rot, "by_host") == 0)  bmk_proxy_set_rotation(BMK_ROTATE_BY_HOST);
        else                                   bmk_proxy_set_rotation(BMK_ROTATE_ROUND);
        free(rot);
    }
    bmk_proxy_load(out->db);

    BMK_INFO("app started, data_dir=%s", out->data_dir);
    return BMK_OK;
}

void bmk_app_shutdown(bmk_app *app)
{
    if (!app) return;
    bmk_proxy_shutdown();
    if (app->db) {
        bmk_db_close(app->db);
        app->db = NULL;
    }
    bmk_net_global_cleanup();
    BMK_INFO("app stopped");
    bmk_log_shutdown();
    app_free_paths(app);
}

static bmk_err copy_file(const char *src, const char *dst)
{
    char  *data = NULL;
    size_t len  = 0;
    bmk_err e = bmk_read_file(src, &data, &len);
    if (e != BMK_OK) return e;
    e = bmk_write_file(dst, data, len);
    free(data);
    return e;
}

bmk_err bmk_app_migrate_data_dir(bmk_app *app, const char *new_dir)
{
    if (!app || !app->db || !new_dir || !*new_dir) return BMK_ERR_INVALID_ARG;
    if (strcmp(new_dir, app->data_dir) == 0) return BMK_OK;
    if (!bmk_app_dir_writable(new_dir)) return BMK_ERR_PERMISSION;

    /* 迁移前先做一次 HTML 快照，失败即中止（FR-01 第 3 条）。 */
    char *snap = NULL;
    if (bmk_backup_snapshot_html(app->db, "premigrate", &snap) != BMK_OK) {
        BMK_ERROR("migrate aborted: pre-migration backup failed");
        return BMK_ERR_IO;
    }
    free(snap);

    char *new_db      = bmk_path_join(new_dir, "bookmarks.db");
    char *new_backups = bmk_path_join(new_dir, "backups");
    char *new_logs    = bmk_path_join(new_dir, "logs");
    char *new_sync    = bmk_path_join(new_dir, "sync");
    if (!new_db || !new_backups || !new_logs || !new_sync) {
        free(new_db); free(new_backups); free(new_logs); free(new_sync);
        return BMK_ERR_NOMEM;
    }
    bmk_mkdirs(new_backups);
    bmk_mkdirs(new_logs);
    bmk_mkdirs(new_sync);

    /* 关闭连接以保证 WAL 落盘后再复制。 */
    bmk_db_close(app->db);
    app->db = NULL;

    bmk_err e = copy_file(app->db_path, new_db);
    if (e != BMK_OK) {
        BMK_ERROR("migrate failed while copying database: %s", bmk_err_str(e));
        /* 回退：重新打开原库。 */
        bmk_db_open(app->db_path, &app->db);
        free(new_db); free(new_backups); free(new_logs); free(new_sync);
        return e;
    }

    e = bmk_db_open(new_db, &app->db);
    if (e != BMK_OK) {
        remove(new_db);
        bmk_db_open(app->db_path, &app->db);
        free(new_db); free(new_backups); free(new_logs); free(new_sync);
        return e;
    }

    free(app->data_dir);    app->data_dir    = bmk_strdup(new_dir);
    free(app->db_path);     app->db_path     = new_db;
    free(app->backups_dir); app->backups_dir = new_backups;
    free(app->logs_dir);    app->logs_dir    = new_logs;
    free(app->sync_dir);    app->sync_dir    = new_sync;

    bmk_backup_set_dir(app->backups_dir);
    bmk_log_init(app->logs_dir, bmk_log_get_level());
    bmk_settings_set_str(app->db, BMK_SET_DATA_DIR, app->data_dir);

    BMK_INFO("data dir migrated to %s", app->data_dir);
    return BMK_OK;
}
