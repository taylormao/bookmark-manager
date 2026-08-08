/*
 * bmk_db.h - SQLite 数据层与领域模型（第 3 章、FR-05/06/10/11/12/09）
 *
 * 线程约定（需求 2.4）：一个 bmk_db 句柄对应一个连接，内部加锁保证
 * 串行访问；网络线程的写操作通过 UI/服务层队列汇聚到单一写句柄。
 */
#ifndef BMK_DB_H
#define BMK_DB_H

#include "bmk_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct bmk_db bmk_db;

/* ------------------------------------------------------------------ */
/* 领域模型                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    int64_t id;
    char   *name;
    int64_t parent_id;   /* 0 = 根（对应 NULL） */
} bmk_category;

typedef struct {
    int64_t id;
    char   *name;
    int64_t usage_count; /* 仅在列表查询时填充 */
} bmk_tag;

typedef struct {
    int64_t  id;
    char    *title;
    char    *url;
    char    *url_key;
    unsigned char *icon;      /* 可为 NULL */
    size_t   icon_len;
    char    *icon_mime;       /* 可为 NULL */
    int64_t  category_id;     /* 0 = 未分类（NULL） */
    char    *category_name;   /* 查询时填充，可为 NULL */
    int      valid;           /* BMK_VALID_UNKNOWN / 0 / 1 */
    int      status_code;     /* -1 表示 NULL */
    int64_t  last_checked_at; /* 0 表示 NULL */
    char    *check_note;      /* 可为 NULL */
    int64_t  created_at;
    int64_t  updated_at;
    char   **tags;            /* 查询时填充 */
    size_t   tag_count;
} bmk_bookmark;

/* 仅释放内部字段；结构体本体所有权归调用方（可栈可堆）。
 * 与之配套，bmk_bookmark_list_free 会额外释放每个元素本体。 */
void bmk_bookmark_free(bmk_bookmark *b);
void bmk_bookmark_list_free(bmk_bookmark **list, size_t n);
void bmk_category_free(bmk_category *c);
void bmk_tag_free(bmk_tag *t);

/* ------------------------------------------------------------------ */
/* 排序与筛选（FR-09 / FR-12）                                          */
/* ------------------------------------------------------------------ */

typedef enum {
    BMK_SORT_TITLE = 0,
    BMK_SORT_URL,
    BMK_SORT_CREATED_AT,
    BMK_SORT_CATEGORY,
    BMK_SORT_VALID,
    BMK_SORT_STATUS_CODE,
    BMK_SORT_LAST_CHECKED,
} bmk_sort_key;

typedef enum {
    BMK_VF_ALL = 0,
    BMK_VF_VALID,
    BMK_VF_INVALID,
    BMK_VF_UNCHECKED,
    BMK_VF_BLOCKED,    /* 待复测：check_note = 'blocked' */
} bmk_valid_filter;

typedef struct {
    char   *keyword;        /* 对 title 与 url 子串匹配（OR） */
    char   *url_contains;   /* 对 url 子串匹配 */
    char   *domain;         /* 对归一化 host 子串匹配 */
    int64_t *tag_ids;       /* 标签之间 OR */
    size_t  tag_count;
    int64_t category_id;    /* >0 含子孙；0=全部；-1=未分类 */
    bmk_valid_filter valid_filter;
    int     status_min;     /* -1 = 不限 */
    int     status_max;     /* -1 = 不限 */
} bmk_filter;

#define BMK_CATEGORY_ALL         0
#define BMK_CATEGORY_UNCATEGORIZED (-1)

void bmk_filter_init(bmk_filter *f);
void bmk_filter_free(bmk_filter *f);

/* ------------------------------------------------------------------ */
/* 生命周期                                                            */
/* ------------------------------------------------------------------ */

/*
 * 打开（必要时创建）数据库，执行 PRAGMA 与 schema 迁移。
 * 库损坏时返回 BMK_ERR_DB_CORRUPT（FR-01 第 2 条），调用方应引导恢复。
 */
bmk_err bmk_db_open(const char *db_path, bmk_db **out);
void    bmk_db_close(bmk_db *db);
const char *bmk_db_last_error(bmk_db *db);
/* 完整性检查，用于启动时探测损坏。 */
bool bmk_db_integrity_ok(bmk_db *db);

bmk_err bmk_db_begin(bmk_db *db);
bmk_err bmk_db_commit(bmk_db *db);
bmk_err bmk_db_rollback(bmk_db *db);

/* 供 settings 模块使用的内部 handle 访问器（同一静态库内）。 */
struct sqlite3 *bmk_db_handle(bmk_db *db);
void bmk_db_lock(bmk_db *db);
void bmk_db_unlock(bmk_db *db);

/* ------------------------------------------------------------------ */
/* 分类（FR-10）                                                       */
/* ------------------------------------------------------------------ */

bmk_err bmk_category_create(bmk_db *db, const char *name, int64_t parent_id, int64_t *out_id);
bmk_err bmk_category_rename(bmk_db *db, int64_t id, const char *new_name);
bmk_err bmk_category_move(bmk_db *db, int64_t id, int64_t new_parent_id);
bmk_err bmk_category_delete(bmk_db *db, int64_t id);
/* 列出全部分类（按 parent_id, name 排序）。 */
bmk_err bmk_category_list(bmk_db *db, bmk_category ***out, size_t *out_n);
void    bmk_category_list_free(bmk_category **list, size_t n);
/* 查找同父同名分类；未找到返回 0。 */
int64_t bmk_category_find(bmk_db *db, const char *name, int64_t parent_id);
/* 含子孙的书签计数（FR-10 第 5 条）。 */
int64_t bmk_category_count_recursive(bmk_db *db, int64_t id);
/* id 是否为 ancestor 的子孙（用于禁止移动到自身/子孙）。 */
bool    bmk_category_is_descendant(bmk_db *db, int64_t id, int64_t ancestor);
/* 该分类及其子孙下的书签数（用于删除确认提示）。 */
int64_t bmk_category_affected_bookmarks(bmk_db *db, int64_t id);
/* 未分类（category_id IS NULL）书签数，供左树固定节点显示计数。 */
int64_t bmk_category_count_uncategorized(bmk_db *db);

/* ------------------------------------------------------------------ */
/* 标签（FR-11）                                                       */
/* ------------------------------------------------------------------ */

/* 不区分大小写查找或创建；保留先创建者的大小写形式。 */
bmk_err bmk_tag_get_or_create(bmk_db *db, const char *name, int64_t *out_id);
bmk_err bmk_tag_list(bmk_db *db, bmk_tag ***out, size_t *out_n);
void    bmk_tag_list_free(bmk_tag **list, size_t n);
bmk_err bmk_tag_rename(bmk_db *db, int64_t id, const char *new_name);
/* 将 from 并入 into：关联迁移后删除 from。 */
bmk_err bmk_tag_merge(bmk_db *db, int64_t from_id, int64_t into_id);
/* 删除标签（仅解除关联，不删除网址）。 */
bmk_err bmk_tag_delete(bmk_db *db, int64_t id);

bmk_err bmk_bookmark_set_tags(bmk_db *db, int64_t bookmark_id,
                              const char *const *tags, size_t n);
bmk_err bmk_bookmark_add_tags(bmk_db *db, const int64_t *bookmark_ids, size_t n_ids,
                              const char *const *tags, size_t n_tags);

/* ------------------------------------------------------------------ */
/* 书签（FR-02 / FR-05 / FR-06 / FR-12）                                */
/* ------------------------------------------------------------------ */

/* 插入；b->url_key 为空时自动计算。成功后回填 b->id。 */
bmk_err bmk_bookmark_insert(bmk_db *db, bmk_bookmark *b);
/* 全量更新（含 url_key 重算与 updated_at 刷新）。 */
bmk_err bmk_bookmark_update(bmk_db *db, bmk_bookmark *b);
bmk_err bmk_bookmark_get(bmk_db *db, int64_t id, bmk_bookmark **out);
/* 硬删除（调用方负责 deleted_log 与备份，见 bmk_backup_delete_bookmarks）。 */
bmk_err bmk_bookmark_delete(bmk_db *db, int64_t id);
bmk_err bmk_bookmark_set_category(bmk_db *db, const int64_t *ids, size_t n, int64_t category_id);
/* 写入校验结果（FR-14）。status_code<0 表示 NULL；valid 用 BMK_VALID_*。 */
bmk_err bmk_bookmark_set_check_result(bmk_db *db, int64_t id, int valid,
                                      int status_code, const char *note, int64_t checked_at);
bmk_err bmk_bookmark_set_icon(bmk_db *db, int64_t id,
                              const unsigned char *icon, size_t len, const char *mime);

/* 查询（应用筛选与排序）。 */
bmk_err bmk_bookmark_query(bmk_db *db, const bmk_filter *f,
                           bmk_sort_key sort, bool ascending,
                           bmk_bookmark ***out, size_t *out_n);

/* 按 url_key 查找已存在记录，返回条数；out_title 为首条标题（可为 NULL）。 */
int64_t bmk_bookmark_count_by_key(bmk_db *db, const char *url_key, char **out_title);

/* 统计（状态栏，FR-20 第 2 条）。 */
typedef struct {
    int64_t total;
    int64_t invalid;
    int64_t blocked;
    int64_t unchecked;
} bmk_stats;
bmk_err bmk_db_stats(bmk_db *db, bmk_stats *out);

/* ------------------------------------------------------------------ */
/* 查重与合并（FR-15 / FR-16）                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    char          *url_key;
    bmk_bookmark **members;
    size_t         count;
} bmk_dup_group;

bmk_err bmk_dedupe_find(bmk_db *db, bmk_dup_group ***out, size_t *out_n);
void    bmk_dup_group_list_free(bmk_dup_group **list, size_t n);

/* deleted_log 写入（FR-06 第 3 条）。 */
bmk_err bmk_deleted_log_add(bmk_db *db, const bmk_bookmark *b, const char *backup_file);

#ifdef __cplusplus
}
#endif
#endif /* BMK_DB_H */
