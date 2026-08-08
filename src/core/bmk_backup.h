/*
 * bmk_backup.h - 备份、删除与合并（FR-06 / FR-16 / FR-21）
 */
#ifndef BMK_BACKUP_H
#define BMK_BACKUP_H

#include "bmk_types.h"
#include "bmk_db.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 设置备份根目录 <数据目录>/backups/ */
bmk_err bmk_backup_set_dir(const char *backups_dir);
const char *bmk_backup_get_dir(void);

/*
 * 删除书签（FR-06 第 3 条）：单事务内
 *   1. 生成备份 HTML（deleted_<时间戳>.html），失败则中止删除；
 *   2. 写 deleted_log；
 *   3. 删除记录。
 * out_backup_path 可为 NULL；否则返回新分配路径。
 */
bmk_err bmk_backup_delete_bookmarks(bmk_db *db, const int64_t *ids, size_t n,
                                    char **out_backup_path);

/*
 * 合并一组重复记录（FR-16 第 3 条）。
 * ids 为组内全部成员；合并后仅保留主记录。
 */
bmk_err bmk_merge_group(bmk_db *db, const int64_t *ids, size_t n,
                        int64_t *out_primary_id);

/* 手动全量备份：生成 bookmarks.html + bookmarks.db 副本（FR-21 第 3 条）。 */
bmk_err bmk_backup_full(bmk_db *db, const char *db_path);

/* 同步覆盖前 / 迁移前的 HTML 备份。prefix 如 "remote"、"premigrate"。 */
bmk_err bmk_backup_snapshot_html(bmk_db *db, const char *prefix, char **out_path);
/* 将任意内容存为备份文件（用于保存远端旧版）。 */
bmk_err bmk_backup_write_raw(const char *prefix, const char *ext,
                             const void *data, size_t len, char **out_path);

/*
 * 备份保留策略（FR-21 第 5 条）：保留最近 20 份全量备份与 7 天内的自动备份。
 */
bmk_err bmk_backup_cleanup(void);

#ifdef __cplusplus
}
#endif
#endif /* BMK_BACKUP_H */
