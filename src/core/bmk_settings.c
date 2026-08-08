#include "bmk_settings.h"
#include "bmk_db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sqlite3.h>
#include <cJSON.h>

#ifdef _WIN32
#  include <windows.h>
#  include <shlobj.h>
#endif

/*
 * 所有设置以 JSON 编码存入 settings.value（需求 3.3）。
 * 例如字符串 "zh" 存为 "\"zh\""，布尔存为 true/false，整数存为 12。
 */

static char *raw_get(bmk_db *db, const char *key)
{
    if (!db || !key) return NULL;
    sqlite3 *h = bmk_db_handle(db);
    if (!h) return NULL;

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(h, "SELECT value FROM settings WHERE key=?;",
                           -1, &st, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_text(st, 1, key, -1, SQLITE_TRANSIENT);
    char *val = NULL;
    if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) != SQLITE_NULL)
        val = bmk_strdup((const char *)sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
    return val;
}

static bmk_err raw_set(bmk_db *db, const char *key, const char *json_value)
{
    if (!db || !key) return BMK_ERR_INVALID_ARG;
    sqlite3 *h = bmk_db_handle(db);
    if (!h) return BMK_ERR_DB;

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(h,
            "INSERT INTO settings(key,value) VALUES(?,?) "
            "ON CONFLICT(key) DO UPDATE SET value=excluded.value;",
            -1, &st, NULL) != SQLITE_OK) return BMK_ERR_DB;
    sqlite3_bind_text(st, 1, key, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, json_value, -1, SQLITE_TRANSIENT);
    bmk_err e = (sqlite3_step(st) == SQLITE_DONE) ? BMK_OK : BMK_ERR_DB;
    sqlite3_finalize(st);
    return e;
}

char *bmk_settings_get_str(bmk_db *db, const char *key, const char *def)
{
    char *raw = raw_get(db, key);
    if (!raw) return bmk_strdup(def);

    cJSON *j = cJSON_Parse(raw);
    free(raw);
    if (!j) return bmk_strdup(def);

    char *result = NULL;
    if (cJSON_IsString(j) && j->valuestring)
        result = bmk_strdup(j->valuestring);
    cJSON_Delete(j);
    return result ? result : bmk_strdup(def);
}

int64_t bmk_settings_get_int(bmk_db *db, const char *key, int64_t def)
{
    char *raw = raw_get(db, key);
    if (!raw) return def;
    cJSON *j = cJSON_Parse(raw);
    free(raw);
    if (!j) return def;
    int64_t v = def;
    if (cJSON_IsNumber(j)) v = (int64_t)j->valuedouble;
    cJSON_Delete(j);
    return v;
}

bool bmk_settings_get_bool(bmk_db *db, const char *key, bool def)
{
    char *raw = raw_get(db, key);
    if (!raw) return def;
    cJSON *j = cJSON_Parse(raw);
    free(raw);
    if (!j) return def;
    bool v = def;
    if (cJSON_IsBool(j))        v = cJSON_IsTrue(j) ? true : false;
    else if (cJSON_IsNumber(j)) v = (j->valuedouble != 0);
    cJSON_Delete(j);
    return v;
}

bmk_err bmk_settings_set_str(bmk_db *db, const char *key, const char *value)
{
    cJSON *j = cJSON_CreateString(value ? value : "");
    if (!j) return BMK_ERR_NOMEM;
    char *txt = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!txt) return BMK_ERR_NOMEM;
    bmk_err e = raw_set(db, key, txt);
    free(txt);
    return e;
}

bmk_err bmk_settings_set_int(bmk_db *db, const char *key, int64_t value)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", (long long)value);
    return raw_set(db, key, buf);
}

bmk_err bmk_settings_set_bool(bmk_db *db, const char *key, bool value)
{
    return raw_set(db, key, value ? "true" : "false");
}

char *bmk_settings_default_data_dir(void)
{
#ifdef _WIN32
    /* Windows：%APPDATA%\BookmarkManager（需求 3.1） */
    char path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, path)))
        return bmk_path_join(path, "BookmarkManager");
    const char *appdata = getenv("APPDATA");
    if (appdata) return bmk_path_join(appdata, "BookmarkManager");
    return bmk_strdup(".\\BookmarkManager");
#else
    const char *xdg = getenv("XDG_DATA_HOME");
    if (xdg && *xdg) return bmk_path_join(xdg, "BookmarkManager");
    const char *home = getenv("HOME");
    if (home && *home) {
        char *share = bmk_path_join(home, ".local/share");
        if (!share) return NULL;
        char *p = bmk_path_join(share, "BookmarkManager");
        free(share);
        return p;
    }
    return bmk_strdup("./BookmarkManager");
#endif
}
