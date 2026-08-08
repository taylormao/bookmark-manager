/*
 * bmk_netscape.h - Netscape HTML 书签导入导出（FR-07 / FR-08 / 附录 A）
 *
 * 解析与生成均为纯函数式流程，可脱离 GUI 单元测试（需求 7.1 第 2 条）。
 */
#ifndef BMK_NETSCAPE_H
#define BMK_NETSCAPE_H

#include "bmk_types.h"
#include "bmk_db.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 导入 ---- */

typedef enum {
    BMK_ENC_AUTO = 0,
    BMK_ENC_UTF8,
    BMK_ENC_GBK,
} bmk_encoding;

typedef enum {
    BMK_DUP_SKIP = 0,     /* 默认：不导入并计数 */
    BMK_DUP_IMPORT,       /* 导入并标记，不合并 */
} bmk_dup_policy;

typedef struct {
    int64_t added;
    int64_t skipped;
    int64_t failed;
    char  **skipped_titles;  /* 明细（标题 - URL） */
    size_t  skipped_count;
    char  **failed_reasons;  /* 明细（URL - 原因） */
    size_t  failed_count;
} bmk_import_report;

void bmk_import_report_free(bmk_import_report *r);

/* 进度回调：返回 false 表示用户请求取消（FR-07 第 4 条：取消=回滚）。 */
typedef bool (*bmk_progress_cb)(int64_t done, int64_t total, void *user);

typedef struct {
    bmk_encoding    encoding;
    bmk_dup_policy  dup_policy;
    int64_t         root_category_id;  /* 导入到指定分类下；0=根 */
    bmk_progress_cb progress;
    void           *progress_user;
} bmk_import_opts;

void bmk_import_opts_init(bmk_import_opts *o);

/*
 * 导入 HTML 书签文件。整个过程为单事务，取消或失败则回滚。
 */
bmk_err bmk_import_html_file(bmk_db *db, const char *path,
                             const bmk_import_opts *opts,
                             bmk_import_report *report);

/* 从内存缓冲导入（供测试与云端恢复复用）。buf 须为 UTF-8。 */
bmk_err bmk_import_html_buffer(bmk_db *db, const char *buf, size_t len,
                               const bmk_import_opts *opts,
                               bmk_import_report *report);

/* ---- 导出 ---- */

/*
 * 导出为标准 Netscape HTML。
 * list 为 NULL 时导出全部（按分类树），否则按给定顺序导出（当前筛选结果）。
 */
bmk_err bmk_export_html_file(bmk_db *db, const char *path,
                             bmk_bookmark **list, size_t n);

/* 导出到字符串（供同步模块直接上传）。调用者 free。 */
bmk_err bmk_export_html_string(bmk_db *db, bmk_bookmark **list, size_t n,
                               char **out);

/* ---- 编码 ---- */

/* GBK → UTF-8。失败返回 NULL。 */
char *bmk_gbk_to_utf8(const char *gbk, size_t len);
/* 判断缓冲是否为合法 UTF-8。 */
bool  bmk_is_valid_utf8(const char *buf, size_t len);
/* HTML 实体转义/反转义。返回新分配字符串。 */
char *bmk_html_escape(const char *s);
char *bmk_html_unescape(const char *s);

#ifdef __cplusplus
}
#endif
#endif /* BMK_NETSCAPE_H */
