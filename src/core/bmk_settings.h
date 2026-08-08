/*
 * bmk_settings.h - 设置读写（FR-01）
 *
 * 所有配置以 JSON 编码存入 settings 表的 value 列（需求 3.3）。
 * 敏感项（PAT / secret_key / vless uuid）仅存本地，不落日志。
 */
#ifndef BMK_SETTINGS_H
#define BMK_SETTINGS_H

#include "bmk_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct bmk_db bmk_db;

/* ---- 设置键（稳定字符串） ---- */
#define BMK_SET_SCHEMA_VERSION   "schema_version"
#define BMK_SET_LANGUAGE         "language"          /* "zh" | "en"，默认 zh */
#define BMK_SET_THEME            "theme"             /* "light" | "dark"，默认 light */
#define BMK_SET_DATA_DIR         "data_dir"
#define BMK_SET_ICON_SERVICE     "icon_service"      /* bool，默认 true（FR-03 3c） */
#define BMK_SET_IMPORT_DUP       "import_duplicate"  /* "skip" | "import"，默认 skip */

/* 校验参数（FR-14 第 3 条） */
#define BMK_SET_CHK_TIMEOUT      "check_timeout"        /* 秒，默认 10 */
#define BMK_SET_CHK_CONN_TIMEOUT "check_connect_timeout"/* 秒，默认 5 */
#define BMK_SET_CHK_MAX_REDIR    "check_max_redirects"  /* 默认 5 */
#define BMK_SET_CHK_CONCURRENCY  "check_concurrency"    /* 1-10，默认 4 */
#define BMK_SET_CHK_PER_HOST     "check_per_host"       /* 默认 2 */
#define BMK_SET_CHK_INTERVAL_MS  "check_interval_ms"    /* 默认 500 */
#define BMK_SET_CHK_JITTER_MS    "check_jitter_ms"      /* 默认 200 */
#define BMK_SET_CHK_RETRY        "check_retry"          /* 默认 1 */
#define BMK_SET_CHK_USE_PROXY    "check_use_proxy"      /* 校验时是否使用代理 */
#define BMK_SET_PROXY_MODE       "proxy_mode"           /* "direct"|"always"|"auto" */
#define BMK_SET_PROXY_ROTATION   "proxy_rotation"       /* "round"|"random"|"failover"|"by_host" */
#define BMK_SET_PROXY_TEST_URL   "proxy_test_url"
#define BMK_SET_XRAY_PATH        "xray_path"

/* 同步（FR-18 / FR-19） */
#define BMK_SET_GH_REPO          "github_repo"
#define BMK_SET_GH_BRANCH        "github_branch"
#define BMK_SET_GH_GIT_PATH      "github_git_path"
#define BMK_SET_GH_AUTH          "github_auth"       /* "pat" | "ssh" */
#define BMK_SET_GH_PAT           "github_pat"        /* 敏感 */
#define BMK_SET_GH_PUSH_DB       "github_push_db"
#define BMK_SET_S3_ENDPOINT      "s3_endpoint"
#define BMK_SET_S3_REGION        "s3_region"
#define BMK_SET_S3_BUCKET        "s3_bucket"
#define BMK_SET_S3_PREFIX        "s3_prefix"
#define BMK_SET_S3_ACCESS_KEY    "s3_access_key"     /* 敏感 */
#define BMK_SET_S3_SECRET_KEY    "s3_secret_key"     /* 敏感 */
#define BMK_SET_S3_PATH_STYLE    "s3_path_style"
#define BMK_SET_S3_PUSH_DB       "s3_push_db"

/*
 * 读写接口。get 系列在键不存在时返回 def。
 * get_str 返回新分配字符串（可能为 def 的副本），调用者 free。
 */
char   *bmk_settings_get_str(bmk_db *db, const char *key, const char *def);
int64_t bmk_settings_get_int(bmk_db *db, const char *key, int64_t def);
bool    bmk_settings_get_bool(bmk_db *db, const char *key, bool def);

bmk_err bmk_settings_set_str(bmk_db *db, const char *key, const char *value);
bmk_err bmk_settings_set_int(bmk_db *db, const char *key, int64_t value);
bmk_err bmk_settings_set_bool(bmk_db *db, const char *key, bool value);

/* 返回默认数据目录：Windows=%APPDATA%\BookmarkManager，Linux=~/.local/share/BookmarkManager */
char *bmk_settings_default_data_dir(void);

#ifdef __cplusplus
}
#endif
#endif /* BMK_SETTINGS_H */
