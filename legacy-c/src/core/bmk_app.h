/*
 * bmk_app.h - 应用装配层（FR-01）
 *
 * 负责数据目录探测/创建、日志初始化、数据库打开与损坏恢复引导、
 * 备份目录设置、代理节点池载入。CLI 与 GTK4 界面共用此层。
 */
#ifndef BMK_APP_H
#define BMK_APP_H

#include "bmk_types.h"
#include "bmk_db.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char   *data_dir;      /* 数据根目录 */
    char   *db_path;       /* <data_dir>/bookmarks.db */
    char   *backups_dir;   /* <data_dir>/backups */
    char   *logs_dir;      /* <data_dir>/logs */
    char   *sync_dir;      /* <data_dir>/sync */
    bmk_db *db;
    bool    db_recovered;  /* 曾检测到损坏并重建（FR-01 第 2 条） */
    char   *recovered_from;/* 损坏库被改名后的路径，可为 NULL */
} bmk_app;

/*
 * 启动应用。
 * data_dir 为 NULL 时使用默认目录（%APPDATA%\BookmarkManager / ~/.local/share/...）。
 * 目录不可写时返回 BMK_ERR_PERMISSION，调用方应提示用户改选目录。
 */
bmk_err bmk_app_startup(const char *data_dir, bmk_app *out);

/* 关闭应用：停止代理子进程、关闭数据库与日志。 */
void bmk_app_shutdown(bmk_app *app);

/*
 * 迁移数据目录（FR-01 第 3 条）：复制数据库与备份到新目录，
 * 成功后写入 settings.data_dir，失败保持原目录不变。
 */
bmk_err bmk_app_migrate_data_dir(bmk_app *app, const char *new_dir);

/* 目录是否可写（探测式创建临时文件）。 */
bool bmk_app_dir_writable(const char *dir);

#ifdef __cplusplus
}
#endif
#endif /* BMK_APP_H */
