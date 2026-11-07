/*
 * bmk_ui.h - GTK4 界面共享上下文与对话框声明
 *
 * 界面层只负责交互与展示，所有业务规则都在 libcore 内。
 * 耗时操作一律走 GTask 后台线程，回调里再刷新界面（需求 5.1 第 2 条）。
 */
#ifndef BMK_UI_H
#define BMK_UI_H

#include <gtk/gtk.h>

#include "bmk_app.h"
#include "bmk_db.h"
#include "bmk_i18n.h"

G_BEGIN_DECLS

typedef struct _BmkWindow BmkWindow;

/* 书签列表模型列序（FR-20 第 2 条；self-test 与主窗口共享）。 */
enum {
    COL_SELECTED,      /* gboolean */
    COL_ICON,          /* GdkPaintable* 可为 NULL */
    COL_TITLE,         /* gchararray */
    COL_URL,
    COL_CATEGORY,
    COL_TAGS,
    COL_VALID_TEXT,
    COL_VALID_DOT,
    COL_STATUS_TEXT,
    COL_CREATED_TEXT,
    COL_ID,            /* gint64 */
    N_LIST_COLS
};

/* 主窗口上下文：贯穿所有界面模块。 */
struct _BmkWindow {
    GtkApplication *app;
    GtkWindow      *window;
    bmk_app        *core;

    /* 左树 */
    GtkWidget      *tree_view;
    GtkTreeStore   *tree_store;     /* 固定节点 + 分类树 */

    /* 列表 */
    GtkWidget      *list_view;
    GtkListStore   *list_store;
    GtkWidget      *list_scroller;
    GtkWidget      *empty_page;     /* 空态引导（FR-20 第 3 条） */
    GtkWidget      *list_stack;

    /* 工具栏 */
    GtkWidget      *search_entry;

    /* 筛选面板（FR-12） */
    GtkWidget      *filter_revealer;
    GtkWidget      *filter_url_entry;
    GtkWidget      *filter_domain_entry;
    GtkWidget      *filter_valid_combo;   /* GtkDropDown */
    GtkWidget      *filter_status_min;
    GtkWidget      *filter_status_max;
    GtkWidget      *filter_tags_button;
    GtkWidget      *filter_tags_box;
    gboolean        filter_guard;         /* 程序化更新控件时抑制回写 */
    guint           search_timer;         /* 关键词 300ms 防抖（FR-12 第 3 条） */

    /* 状态栏（FR-22 第 1 条） */
    GtkWidget      *status_total;
    GtkWidget      *status_filtered;
    GtkWidget      *status_invalid;
    GtkWidget      *status_blocked;
    GtkWidget      *status_sync;
    GtkWidget      *toast_overlay;
    GtkWidget      *toast_label;
    GtkWidget      *toast_revealer;
    guint           toast_timer;

    /* 当前筛选与排序状态 */
    bmk_filter      filter;
    bmk_sort_key    sort_key;
    gboolean        sort_asc;
    gint64          selected_category;   /* 与 filter.category_id 同步，供新建分类默认父级用 */

    /* 缓存：分类 id -> 名称路径，供列表渲染 */
    GHashTable     *category_paths;

    /* 选择同步护栏：程序化操作高亮/复选框时置位，
     * 抑制 selection-changed 触发重复全量同步（FR-13）。 */
    gboolean        sel_guard;
};

/* ---- 主窗口 ---- */
BmkWindow *bmk_window_new(GtkApplication *app, bmk_app *core);
void       bmk_window_refresh_list(BmkWindow *w);
void       bmk_window_refresh_tree(BmkWindow *w);
void       bmk_window_refresh_status(BmkWindow *w);
void       bmk_window_refresh_all(BmkWindow *w);

/* 取当前列表选中的书签 id，返回数量；*out 需 g_free。 */
gsize      bmk_window_selected_ids(BmkWindow *w, gint64 **out);

/* ---- 通用反馈（FR-22 第 2 条） ---- */
void bmk_toast(BmkWindow *w, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
void bmk_error_dialog(GtkWindow *parent, const char *fmt, ...) G_GNUC_PRINTF(2, 3);

/* 确认框：确认后调用 on_confirm(user)。“取消”永远可用（FR-20 第 7 条）。 */
typedef void (*BmkConfirmCb)(gpointer user);
void bmk_confirm_dialog(GtkWindow *parent, const char *message,
                        const char *detail, const char *ok_label,
                        BmkConfirmCb on_confirm, gpointer user);

/* ---- 对话框 ---- */
void bmk_dialog_edit(BmkWindow *w, gint64 bookmark_id);   /* id<=0 表示新增 */
void bmk_dialog_import(BmkWindow *w);
void bmk_dialog_export(BmkWindow *w);
void bmk_dialog_check(BmkWindow *w, gint64 *ids, gsize n); /* ids 为 NULL 表示按范围选择 */
void bmk_dialog_dedupe(BmkWindow *w);
void bmk_dialog_settings(BmkWindow *w);
void bmk_dialog_proxy(BmkWindow *w);
void bmk_dialog_sync(BmkWindow *w);
void bmk_dialog_tags(BmkWindow *w);

/* 分类对话框：parent_id 为默认父级；category_id>0 表示重命名/移动。 */
void bmk_dialog_category_new(BmkWindow *w, gint64 parent_id);
void bmk_dialog_category_rename(BmkWindow *w, gint64 category_id);
void bmk_dialog_category_move(BmkWindow *w, gint64 category_id);

/* ---- 小工具 ---- */
char *bmk_ui_category_path(bmk_db *db, gint64 category_id);   /* 需 g_free */
void  bmk_ui_fill_category_combo(bmk_db *db, GtkStringList *list,
                                 GArray *ids, gint64 exclude_subtree);
const char *bmk_ui_valid_label(int valid, const char *note);
const char *bmk_ui_valid_icon(int valid, const char *note);   /* 圆点字符 */

G_END_DECLS
#endif /* BMK_UI_H */
