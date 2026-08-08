/*
 * bmk_sync.h - 云同步（FR-18 GitHub / FR-19 S3）
 *
 * 冲突策略统一：以本地为准 + 远端旧版先备份。
 */
#ifndef BMK_SYNC_H
#define BMK_SYNC_H

#include "bmk_types.h"
#include "bmk_db.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 同步日志回调（界面展示执行日志，内容已脱敏）。 */
typedef void (*bmk_sync_log_cb)(const char *line, void *user);

/* ---- GitHub（FR-18） ---- */

typedef struct {
    const char *repo;        /* https 或 ssh 地址 */
    const char *branch;      /* 留空自动探测 master/main */
    const char *git_path;    /* 留空自动探测 PATH */
    const char *pat;         /* HTTPS+PAT 认证；不落日志 */
    bool        use_ssh;
    bool        push_db;     /* 是否同时上传数据库快照 */
    const char *sync_dir;    /* <数据目录>/sync/github/ */
    const char *db_path;
} bmk_github_cfg;

/* 立即同步（FR-18 第 4 条 a~g 的确定性序列）。 */
bmk_err bmk_sync_github_push(bmk_db *db, const bmk_github_cfg *cfg,
                             bmk_sync_log_cb log, void *user);
/* 从云端恢复：拉取后走 FR-07 导入（默认跳过重复）。 */
bmk_err bmk_sync_github_restore(bmk_db *db, const bmk_github_cfg *cfg,
                                bmk_sync_log_cb log, void *user);

/* ---- S3（FR-19 / 附录 D） ---- */

typedef struct {
    const char *endpoint;    /* 默认 https://s3.<region>.amazonaws.com */
    const char *region;      /* 默认 us-east-1 */
    const char *bucket;
    const char *prefix;      /* 可选 */
    const char *access_key;
    const char *secret_key;
    bool        path_style;  /* 默认开 */
    bool        push_db;
    const char *db_path;
} bmk_s3_cfg;

bmk_err bmk_sync_s3_push(bmk_db *db, const bmk_s3_cfg *cfg,
                         bmk_sync_log_cb log, void *user);
bmk_err bmk_sync_s3_restore(bmk_db *db, const bmk_s3_cfg *cfg,
                            bmk_sync_log_cb log, void *user);

/* ---- sync_meta ---- */

char   *bmk_sync_meta_get(bmk_db *db, const char *key);
bmk_err bmk_sync_meta_set(bmk_db *db, const char *key, const char *value);

#define BMK_SYNC_LAST_SYNC     "last_sync"
#define BMK_SYNC_GITHUB_REMOTE "github_remote"
#define BMK_SYNC_S3_TARGET     "s3_target"

#ifdef __cplusplus
}
#endif
#endif /* BMK_SYNC_H */
