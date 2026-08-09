/*
 * bmk_window.c - 主窗口（FR-20 / FR-13 / FR-22）
 *
 * 布局：顶部工具栏 + 左侧分类树 + 中部书签列表 + 底部状态栏。
 */
#include "bmk_ui.h"

#include "bmk_url.h"
#include "bmk_log.h"
#include "bmk_backup.h"
#include "bmk_settings.h"
#include "bmk_sync.h"
#include "bmk_fetch.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* 列表模型列序 */
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

/* 左树模型列序 */
enum {
    TCOL_LABEL,        /* 显示文本（含计数） */
    TCOL_KIND,         /* 0=固定节点 1=分类 */
    TCOL_VALUE,        /* 固定节点：BMK_CATEGORY_* 或特殊码；分类：category id */
    N_TREE_COLS
};

/* 固定节点的 value 编码（避开合法 category id） */
#define NODE_ALL        (-100)
#define NODE_UNCAT      (-101)
#define NODE_BLOCKED    (-102)
#define NODE_INVALID    (-103)

static void rebuild_filter_tags(BmkWindow *w);

/* ------------------------------------------------------------------ */
/* 通用反馈                                                            */
/* ------------------------------------------------------------------ */

static gboolean toast_hide(gpointer user)
{
    BmkWindow *w = user;
    gtk_revealer_set_reveal_child(GTK_REVEALER(w->toast_revealer), FALSE);
    w->toast_timer = 0;
    return G_SOURCE_REMOVE;
}

void bmk_toast(BmkWindow *w, const char *fmt, ...)
{
    if (!w || !w->toast_label) return;

    va_list ap;
    va_start(ap, fmt);
    char *msg = g_strdup_vprintf(fmt, ap);
    va_end(ap);

    gtk_label_set_text(GTK_LABEL(w->toast_label), msg);
    g_free(msg);

    gtk_revealer_set_reveal_child(GTK_REVEALER(w->toast_revealer), TRUE);
    if (w->toast_timer) g_source_remove(w->toast_timer);
    w->toast_timer = g_timeout_add_seconds(4, toast_hide, w);   /* FR-22 第 2 条 */
}

void bmk_error_dialog(GtkWindow *parent, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *msg = g_strdup_vprintf(fmt, ap);
    va_end(ap);

    GtkWidget *d = gtk_message_dialog_new(parent,
                                          GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                          GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
                                          "%s", msg);
    gtk_window_set_title(GTK_WINDOW(d), T("msg.error"));
    g_signal_connect(d, "response", G_CALLBACK(gtk_window_destroy), NULL);
    gtk_widget_set_visible(d, TRUE);
    g_free(msg);
}

typedef struct {
    BmkConfirmCb cb;
    gpointer     user;
} ConfirmCtx;

static void on_confirm_response(GtkDialog *d, int response, gpointer user)
{
    ConfirmCtx *c = user;
    if (response == GTK_RESPONSE_ACCEPT && c->cb) c->cb(c->user);
    g_free(c);
    gtk_window_destroy(GTK_WINDOW(d));
}

void bmk_confirm_dialog(GtkWindow *parent, const char *message,
                        const char *detail, const char *ok_label,
                        BmkConfirmCb on_confirm, gpointer user)
{
    GtkWidget *d = gtk_message_dialog_new(parent,
                                          GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                          GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
                                          "%s", message);
    if (detail && *detail)
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "%s", detail);

    gtk_window_set_title(GTK_WINDOW(d), T("msg.confirm"));
    /* “取消”永远可用且为默认（FR-20 第 7 条） */
    gtk_dialog_add_button(GTK_DIALOG(d), T("btn.cancel"), GTK_RESPONSE_CANCEL);
    GtkWidget *ok = gtk_dialog_add_button(GTK_DIALOG(d),
                                          ok_label ? ok_label : T("btn.ok"),
                                          GTK_RESPONSE_ACCEPT);
    gtk_widget_add_css_class(ok, "destructive-action");
    gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_CANCEL);

    ConfirmCtx *c = g_new0(ConfirmCtx, 1);
    c->cb = on_confirm;
    c->user = user;
    g_signal_connect(d, "response", G_CALLBACK(on_confirm_response), c);
    gtk_widget_set_visible(d, TRUE);
}

/* ------------------------------------------------------------------ */
/* 小工具                                                              */
/* ------------------------------------------------------------------ */

const char *bmk_ui_valid_label(int valid, const char *note)
{
    if (note && strcmp(note, BMK_NOTE_BLOCKED) == 0) return T("valid.blocked");
    if (valid == BMK_VALID_GOOD) return T("valid.good");
    if (valid == BMK_VALID_BAD)  return T("valid.bad");
    return T("valid.unknown");
}

/* FR-20 第 6 条：绿/红/灰/黄圆点 */
const char *bmk_ui_valid_icon(int valid, const char *note)
{
    if (note && strcmp(note, BMK_NOTE_BLOCKED) == 0) return "\xF0\x9F\x9F\xA1"; /* 🟡 */
    if (valid == BMK_VALID_GOOD) return "\xF0\x9F\x9F\xA2";                     /* 🟢 */
    if (valid == BMK_VALID_BAD)  return "\xF0\x9F\x94\xB4";                     /* 🔴 */
    return "\xE2\x9A\xAA";                                                       /* ⚪ */
}

char *bmk_ui_category_path(bmk_db *db, gint64 category_id)
{
    if (category_id <= 0) return g_strdup("");

    bmk_category **cats = NULL;
    size_t n = 0;
    if (bmk_category_list(db, &cats, &n) != BMK_OK) return g_strdup("");

    GString *s = g_string_new(NULL);
    const char *parts[40];
    int depth = 0;
    int64_t cur = category_id;
    while (cur > 0 && depth < 40) {
        bmk_category *f = NULL;
        for (size_t i = 0; i < n; i++)
            if (cats[i]->id == cur) { f = cats[i]; break; }
        if (!f) break;
        parts[depth++] = f->name;
        cur = f->parent_id;
    }
    for (int i = depth - 1; i >= 0; i--) {
        g_string_append(s, parts[i]);
        if (i) g_string_append_c(s, '/');
    }
    char *out = g_strdup(s->str);
    g_string_free(s, TRUE);
    bmk_category_list_free(cats, n);
    return out;
}

/* 填充分类下拉：可排除某个子树（移动分类时避免形成环）。 */
void bmk_ui_fill_category_combo(bmk_db *db, GtkStringList *list, GArray *ids,
                                gint64 exclude_subtree)
{
    gtk_string_list_append(list, T("cat.root"));
    gint64 zero = 0;
    g_array_append_val(ids, zero);

    bmk_category **cats = NULL;
    size_t n = 0;
    if (bmk_category_list(db, &cats, &n) != BMK_OK) return;

    for (size_t i = 0; i < n; i++) {
        gint64 id = cats[i]->id;
        if (exclude_subtree > 0 &&
            (id == exclude_subtree || bmk_category_is_descendant(db, id, exclude_subtree)))
            continue;
        char *path = bmk_ui_category_path(db, id);
        gtk_string_list_append(list, path);
        g_free(path);
        g_array_append_val(ids, id);
    }
    bmk_category_list_free(cats, n);
}

/* ------------------------------------------------------------------ */
/* 列表刷新                                                            */
/* ------------------------------------------------------------------ */

static GdkPixbuf *icon_pixbuf(const unsigned char *data, size_t len)
{
    if (!data || !len) return NULL;

    GBytes *bytes = g_bytes_new(data, len);
    GdkTexture *tex = gdk_texture_new_from_bytes(bytes, NULL);
    g_bytes_unref(bytes);
    if (!tex) return NULL;
    /* GtkCellRendererPixbuf 只认 pixbuf 属性，需从纹理转 GdkPixbuf */
    GdkPixbuf *pb = gdk_pixbuf_get_from_texture(tex);
    g_object_unref(tex);
    return pb;
}

void bmk_window_refresh_list(BmkWindow *w)
{
    bmk_bookmark **list = NULL;
    size_t n = 0;

    bmk_err e = bmk_bookmark_query(w->core->db, &w->filter, w->sort_key,
                                   w->sort_asc, &list, &n);
    if (e != BMK_OK) {
        bmk_error_dialog(w->window, "%s (%s)", T("msg.error"), bmk_err_str(e));
        return;
    }

    gtk_list_store_clear(w->list_store);
    for (size_t i = 0; i < n; i++) {
        bmk_bookmark *b = list[i];

        char *cat = bmk_ui_category_path(w->core->db, b->category_id);
        if (!*cat) { g_free(cat); cat = g_strdup(T("tree.uncategorized")); }

        GString *tags = g_string_new(NULL);
        for (size_t t = 0; t < b->tag_count; t++) {
            if (t) g_string_append(tags, ", ");
            g_string_append(tags, b->tags[t]);
        }

        char status[16];
        if (b->status_code > 0) g_snprintf(status, sizeof status, "%d", b->status_code);
        else                    g_strlcpy(status, "-", sizeof status);

        char created[32];
        if (b->created_at > 0) {
            GDateTime *dt = g_date_time_new_from_unix_local(b->created_at);
            char *s = g_date_time_format(dt, "%Y-%m-%d %H:%M");
            g_strlcpy(created, s, sizeof created);
            g_free(s);
            g_date_time_unref(dt);
        } else {
            g_strlcpy(created, "-", sizeof created);
        }

        /* FR-04：无图标时使用内置默认图标 */
        const unsigned char *icon_data = b->icon;
        size_t              icon_sz   = b->icon_len;
        if (!icon_data || !icon_sz) {
            icon_data = bmk_default_icon(&icon_sz);
        }
        GdkPixbuf *pic = icon_pixbuf(icon_data, icon_sz);

        GtkTreeIter it;
        gtk_list_store_append(w->list_store, &it);
        gtk_list_store_set(w->list_store, &it,
                           COL_SELECTED,     FALSE,
                           COL_ICON,         pic,
                           COL_TITLE,        b->title ? b->title : T("app.untitled"),
                           COL_URL,          b->url ? b->url : "",
                           COL_CATEGORY,     cat,
                           COL_TAGS,         tags->str,
                           COL_VALID_TEXT,   bmk_ui_valid_label(b->valid, b->check_note),
                           COL_VALID_DOT,    bmk_ui_valid_icon(b->valid, b->check_note),
                           COL_STATUS_TEXT,  status,
                           COL_CREATED_TEXT, created,
                           COL_ID,           (gint64)b->id,
                           -1);
        if (pic) g_object_unref(pic);
        g_string_free(tags, TRUE);
        g_free(cat);
    }
    bmk_bookmark_list_free(list, n);

    /* 空态引导（FR-20 第 3 条） */
    gtk_stack_set_visible_child_name(GTK_STACK(w->list_stack),
                                     n ? "list" : "empty");
    bmk_window_refresh_status(w);
}

/* ------------------------------------------------------------------ */
/* 左树刷新                                                            */
/* ------------------------------------------------------------------ */

static void tree_add_children(BmkWindow *w, GtkTreeIter *parent_iter,
                              bmk_category **cats, size_t n, gint64 parent_id)
{
    for (size_t i = 0; i < n; i++) {
        if (cats[i]->parent_id != parent_id) continue;

        int64_t cnt = bmk_category_count_recursive(w->core->db, cats[i]->id);
        char *label = g_strdup_printf("%s (%lld)", cats[i]->name, (long long)cnt);

        GtkTreeIter it;
        gtk_tree_store_append(w->tree_store, &it, parent_iter);
        gtk_tree_store_set(w->tree_store, &it,
                           TCOL_LABEL, label,
                           TCOL_KIND,  1,
                           TCOL_VALUE, (gint64)cats[i]->id,
                           -1);
        g_free(label);
        tree_add_children(w, &it, cats, n, cats[i]->id);
    }
}

void bmk_window_refresh_tree(BmkWindow *w)
{
    bmk_stats st;
    memset(&st, 0, sizeof st);
    bmk_db_stats(w->core->db, &st);

    int64_t uncat = bmk_category_count_uncategorized(w->core->db);

    gtk_tree_store_clear(w->tree_store);

    struct { const char *key; gint64 value; long long count; } fixed[] = {
        { "tree.all",           NODE_ALL,     (long long)st.total },
        { "tree.uncategorized", NODE_UNCAT,   (long long)uncat },
        { "tree.blocked",       NODE_BLOCKED, (long long)st.blocked },
        { "tree.invalid",       NODE_INVALID, (long long)st.invalid },
    };
    for (size_t i = 0; i < G_N_ELEMENTS(fixed); i++) {
        char *label = g_strdup_printf("%s (%lld)", T(fixed[i].key), fixed[i].count);
        GtkTreeIter it;
        gtk_tree_store_append(w->tree_store, &it, NULL);
        gtk_tree_store_set(w->tree_store, &it,
                           TCOL_LABEL, label, TCOL_KIND, 0,
                           TCOL_VALUE, fixed[i].value, -1);
        g_free(label);
    }

    bmk_category **cats = NULL;
    size_t n = 0;
    if (bmk_category_list(w->core->db, &cats, &n) == BMK_OK) {
        tree_add_children(w, NULL, cats, n, 0);
        bmk_category_list_free(cats, n);
    }
    gtk_tree_view_expand_all(GTK_TREE_VIEW(w->tree_view));
}

/* ------------------------------------------------------------------ */
/* 状态栏（FR-22 第 1 条）                                             */
/* ------------------------------------------------------------------ */

void bmk_window_refresh_status(BmkWindow *w)
{
    bmk_stats st;
    memset(&st, 0, sizeof st);
    bmk_db_stats(w->core->db, &st);

    gint shown = gtk_tree_model_iter_n_children(GTK_TREE_MODEL(w->list_store), NULL);

    char *s;
    s = g_strdup_printf(T("status.total"),    (long long)st.total);
    gtk_label_set_text(GTK_LABEL(w->status_total), s);    g_free(s);
    s = g_strdup_printf(T("status.filtered"), (long long)shown);
    gtk_label_set_text(GTK_LABEL(w->status_filtered), s); g_free(s);
    s = g_strdup_printf(T("status.invalid"),  (long long)st.invalid);
    gtk_label_set_text(GTK_LABEL(w->status_invalid), s);  g_free(s);
    s = g_strdup_printf(T("status.blocked"),  (long long)st.blocked);
    gtk_label_set_text(GTK_LABEL(w->status_blocked), s);  g_free(s);

    char *last = bmk_sync_meta_get(w->core->db, BMK_SYNC_LAST_SYNC);
    if (last && *last) {
        s = g_strdup_printf(T("sync.last"), last);
        gtk_label_set_text(GTK_LABEL(w->status_sync), s);
        g_free(s);
    } else {
        gtk_label_set_text(GTK_LABEL(w->status_sync), T("sync.never"));
    }
    free(last);
}

void bmk_window_refresh_all(BmkWindow *w)
{
    bmk_window_refresh_tree(w);
    bmk_window_refresh_list(w);
    rebuild_filter_tags(w);   /* 标签增删后保持多选面板最新 */
}

/* ------------------------------------------------------------------ */
/* 选中项                                                              */
/* ------------------------------------------------------------------ */

gsize bmk_window_selected_ids(BmkWindow *w, gint64 **out)
{
    GArray *ids = g_array_new(FALSE, FALSE, sizeof(gint64));
    GtkTreeModel *m = GTK_TREE_MODEL(w->list_store);
    GtkTreeIter it;

    /* 优先取复选框勾选项；没有勾选时退回到高亮行。 */
    if (gtk_tree_model_get_iter_first(m, &it)) {
        do {
            gboolean sel = FALSE;
            gint64 id = 0;
            gtk_tree_model_get(m, &it, COL_SELECTED, &sel, COL_ID, &id, -1);
            if (sel) g_array_append_val(ids, id);
        } while (gtk_tree_model_iter_next(m, &it));
    }

    if (ids->len == 0) {
        GtkTreeSelection *sel =
            gtk_tree_view_get_selection(GTK_TREE_VIEW(w->list_view));
        GList *rows = gtk_tree_selection_get_selected_rows(sel, NULL);
        for (GList *l = rows; l; l = l->next) {
            if (gtk_tree_model_get_iter(m, &it, (GtkTreePath *)l->data)) {
                gint64 id = 0;
                gtk_tree_model_get(m, &it, COL_ID, &id, -1);
                g_array_append_val(ids, id);
            }
        }
        g_list_free_full(rows, (GDestroyNotify)gtk_tree_path_free);
    }

    gsize n = ids->len;
    *out = (gint64 *)g_array_free(ids, n == 0);
    if (n == 0) *out = NULL;
    return n;
}

/* ------------------------------------------------------------------ */
/* 交互回调                                                            */
/* ------------------------------------------------------------------ */

static gboolean search_debounce_cb(gpointer user)
{
    BmkWindow *w = user;
    w->search_timer = 0;
    const char *text = gtk_editable_get_text(GTK_EDITABLE(w->search_entry));

    free(w->filter.keyword);
    w->filter.keyword = (text && *text) ? bmk_strdup(text) : NULL;
    bmk_window_refresh_list(w);
    return G_SOURCE_REMOVE;
}

static void on_search_changed(GtkSearchEntry *entry, gpointer user)
{
    BmkWindow *w = user;
    /* 关键词 300ms 防抖（FR-12 第 3 条） */
    if (w->search_timer) g_source_remove(w->search_timer);
    w->search_timer = g_timeout_add(300, search_debounce_cb, w);
}

static void on_tree_selection_changed(GtkTreeSelection *sel, gpointer user)
{
    BmkWindow *w = user;
    GtkTreeModel *m = NULL;
    GtkTreeIter it;
    if (!gtk_tree_selection_get_selected(sel, &m, &it)) return;

    int kind = 0;
    gint64 value = 0;
    gtk_tree_model_get(m, &it, TCOL_KIND, &kind, TCOL_VALUE, &value, -1);

    /* 固定节点会覆盖有效性维度；分类节点只改分类维度。 */
    w->filter.category_id = BMK_CATEGORY_ALL;
    w->filter.valid_filter = BMK_VF_ALL;
    w->selected_category = 0;

    if (kind == 0) {
        switch (value) {
        case NODE_UNCAT:   w->filter.category_id = BMK_CATEGORY_UNCATEGORIZED; break;
        case NODE_BLOCKED: w->filter.valid_filter = BMK_VF_BLOCKED; break;
        case NODE_INVALID: w->filter.valid_filter = BMK_VF_INVALID; break;
        default: break;   /* NODE_ALL */
        }
    } else {
        w->filter.category_id = value;
        w->selected_category = value;
    }
    bmk_window_refresh_list(w);
}

static void on_column_clicked(GtkTreeViewColumn *col, gpointer user)
{
    BmkWindow *w = user;
    bmk_sort_key key = (bmk_sort_key)GPOINTER_TO_INT(
        g_object_get_data(G_OBJECT(col), "sort-key"));

    if (w->sort_key == key) w->sort_asc = !w->sort_asc;      /* FR-09 第 3 条 */
    else { w->sort_key = key; w->sort_asc = TRUE; }

    /* 更新箭头 */
    GtkTreeView *tv = GTK_TREE_VIEW(w->list_view);
    for (guint i = 0; i < gtk_tree_view_get_n_columns(tv); i++) {
        GtkTreeViewColumn *c = gtk_tree_view_get_column(tv, i);
        gtk_tree_view_column_set_sort_indicator(c, c == col);
        if (c == col)
            gtk_tree_view_column_set_sort_order(
                c, w->sort_asc ? GTK_SORT_ASCENDING : GTK_SORT_DESCENDING);
    }
    bmk_window_refresh_list(w);
}

static void on_select_toggled(GtkCellRendererToggle *cr, char *path_str, gpointer user)
{
    (void)cr;
    BmkWindow *w = user;
    GtkTreeIter it;
    GtkTreePath *path = gtk_tree_path_new_from_string(path_str);
    if (gtk_tree_model_get_iter(GTK_TREE_MODEL(w->list_store), &it, path)) {
        gboolean sel = FALSE;
        gtk_tree_model_get(GTK_TREE_MODEL(w->list_store), &it, COL_SELECTED, &sel, -1);
        gtk_list_store_set(w->list_store, &it, COL_SELECTED, !sel, -1);
    }
    gtk_tree_path_free(path);
}

static gint64 row_id_at(BmkWindow *w, GtkTreePath *path)
{
    GtkTreeIter it;
    gint64 id = 0;
    if (gtk_tree_model_get_iter(GTK_TREE_MODEL(w->list_store), &it, path))
        gtk_tree_model_get(GTK_TREE_MODEL(w->list_store), &it, COL_ID, &id, -1);
    return id;
}

static void on_row_activated(GtkTreeView *tv, GtkTreePath *path,
                             GtkTreeViewColumn *col, gpointer user)
{
    (void)tv; (void)col;
    BmkWindow *w = user;
    gint64 id = row_id_at(w, path);
    if (id > 0) bmk_dialog_edit(w, id);   /* FR-20 第 5 条：双击 = 编辑 */
}

/* ---- 框选（FR-13 第 1 条：GtkGestureDrag） ---- */

static void clear_all_checked(BmkWindow *w)
{
    GtkTreeModel *m = GTK_TREE_MODEL(w->list_store);
    GtkTreeIter it;
    if (gtk_tree_model_get_iter_first(m, &it)) {
        do {
            gtk_list_store_set(w->list_store, &it, COL_SELECTED, FALSE, -1);
        } while (gtk_tree_model_iter_next(m, &it));
    }
}

static void marquee_select_range(BmkWindow *w, int start_idx, int end_idx)
{
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(w->list_view));
    gtk_tree_selection_unselect_all(sel);

    int lo = MIN(start_idx, end_idx);
    int hi = MAX(start_idx, end_idx);
    for (int i = lo; i <= hi; i++) {
        GtkTreePath *p = gtk_tree_path_new_from_indices(i, -1);
        gtk_tree_selection_select_path(sel, p);
        GtkTreeIter it;
        if (gtk_tree_model_get_iter(GTK_TREE_MODEL(w->list_store), &it, p))
            gtk_list_store_set(w->list_store, &it, COL_SELECTED, TRUE, -1);
        gtk_tree_path_free(p);
    }
}

static void on_drag_begin(GtkGestureDrag *g, double x, double y, gpointer user)
{
    BmkWindow *w = user;
    /* 认领本次事件序列：避免 TreeView 自带的点击/切换处理在松手时覆盖框选 */
    gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
    /* 清除历史勾选，防止多次框选后残留（乱多选根源） */
    clear_all_checked(w);

    int start = 0;   /* 空白处开始视为从第一行开始；无数据时 -1 表示不启用 */
    GtkTreePath *path = NULL;
    if (gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(w->list_view), (int)x, (int)y,
                                      &path, NULL, NULL, NULL)) {
        start = gtk_tree_path_get_indices(path)[0];
        gtk_tree_path_free(path);
    } else if (gtk_tree_model_iter_n_children(GTK_TREE_MODEL(w->list_store), NULL) == 0) {
        start = -1;
    }
    g_object_set_data(G_OBJECT(w->list_view), "marquee-start", GINT_TO_POINTER(start));
}

static void on_drag_update(GtkGestureDrag *g, double x, double y, gpointer user)
{
    (void)g;
    BmkWindow *w = user;
    int start = GPOINTER_TO_INT(
        g_object_get_data(G_OBJECT(w->list_view), "marquee-start"));
    if (start < 0) return;

    GtkTreePath *end = NULL;
    if (gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(w->list_view), (int)x, (int)y,
                                      &end, NULL, NULL, NULL)) {
        int ei = gtk_tree_path_get_indices(end)[0];
        marquee_select_range(w, start, ei);
        gtk_tree_path_free(end);
    }
}

static void on_drag_end(GtkGestureDrag *g, double x, double y, gpointer user)
{
    (void)g; (void)x; (void)y;
    BmkWindow *w = user;
    g_object_set_data_full(G_OBJECT(w->list_view), "marquee-start", NULL, NULL);
}

/* 所选记录的归一化 url_key 是否全部相同（FR-13：合并网址启用条件） */
static bool selection_same_key(BmkWindow *w, gint64 *ids, gsize n)
{
    char *key = NULL;
    bool same = true;
    for (gsize i = 0; i < n; i++) {
        bmk_bookmark *b = NULL;
        if (bmk_bookmark_get(w->core->db, ids[i], &b) != BMK_OK || !b) { same = false; break; }
        if (!b->url_key || !*b->url_key) {
            same = false;
        } else if (!key) {
            key = bmk_strdup(b->url_key);
        } else if (strcmp(key, b->url_key) != 0) {
            same = false;
        }
        bmk_bookmark_free(b);
        free(b);
        if (!same) break;
    }
    free(key);
    return same;
}

/* ---- 动作（工具栏 / 快捷键 / 右键共用） ---- */

static void act_add(GSimpleAction *a, GVariant *p, gpointer user)
{
    (void)a; (void)p;
    bmk_dialog_edit(user, 0);
}

static void act_edit(GSimpleAction *a, GVariant *p, gpointer user)
{
    (void)a; (void)p;
    BmkWindow *w = user;
    gint64 *ids = NULL;
    gsize n = bmk_window_selected_ids(w, &ids);
    if (n == 0) { bmk_toast(w, "%s", T("msg.no_selection")); return; }
    bmk_dialog_edit(w, ids[0]);
    g_free(ids);
}

typedef struct { BmkWindow *w; gint64 *ids; gsize n; } DeleteCtx;

static void do_delete(gpointer user)
{
    DeleteCtx *c = user;
    char *backup = NULL;

    /* FR-06：先备份，备份失败则中止删除。 */
    bmk_err e = bmk_backup_delete_bookmarks(c->w->core->db,
                                            (int64_t *)c->ids, c->n, &backup);
    if (e != BMK_OK) {
        bmk_error_dialog(c->w->window, "%s (%s)",
                         T("delete.backup_failed"), bmk_err_str(e));
    } else {
        bmk_toast(c->w, T("delete.done"), (int)c->n);
        bmk_window_refresh_all(c->w);
    }
    free(backup);
    g_free(c->ids);
    g_free(c);
}

static void act_delete(GSimpleAction *a, GVariant *p, gpointer user)
{
    (void)a; (void)p;
    BmkWindow *w = user;
    gint64 *ids = NULL;
    gsize n = bmk_window_selected_ids(w, &ids);
    if (n == 0) { bmk_toast(w, "%s", T("msg.no_selection")); return; }

    char *msg = (n == 1) ? g_strdup(T("delete.confirm_one"))
                         : g_strdup_printf(T("delete.confirm_many"), (int)n);

    DeleteCtx *c = g_new0(DeleteCtx, 1);
    c->w = w; c->ids = ids; c->n = n;
    bmk_confirm_dialog(w->window, msg, T("delete.backup_note"),
                       T("ctx.delete"), do_delete, c);
    g_free(msg);
}

static void act_focus_search(GSimpleAction *a, GVariant *p, gpointer user)
{
    (void)a; (void)p;
    BmkWindow *w = user;
    gtk_widget_grab_focus(w->search_entry);
}

static void act_check(GSimpleAction *a, GVariant *p, gpointer user)
{
    (void)a; (void)p;
    BmkWindow *w = user;
    gint64 *ids = NULL;
    gsize n = bmk_window_selected_ids(w, &ids);
    bmk_dialog_check(w, ids, n);
    g_free(ids);
}

static void act_dedupe(GSimpleAction *a, GVariant *p, gpointer user)
{ (void)a; (void)p; bmk_dialog_dedupe(user); }
static void act_sync(GSimpleAction *a, GVariant *p, gpointer user)
{ (void)a; (void)p; bmk_dialog_sync(user); }
static void act_import(GSimpleAction *a, GVariant *p, gpointer user)
{ (void)a; (void)p; bmk_dialog_import(user); }
static void act_export(GSimpleAction *a, GVariant *p, gpointer user)
{ (void)a; (void)p; bmk_dialog_export(user); }
static void act_settings(GSimpleAction *a, GVariant *p, gpointer user)
{ (void)a; (void)p; bmk_dialog_settings(user); }
static void act_proxy(GSimpleAction *a, GVariant *p, gpointer user)
{ (void)a; (void)p; bmk_dialog_proxy(user); }
static void act_tags(GSimpleAction *a, GVariant *p, gpointer user)
{ (void)a; (void)p; bmk_dialog_tags(user); }

static void act_open(GSimpleAction *a, GVariant *p, gpointer user)
{
    (void)a; (void)p;
    BmkWindow *w = user;
    gint64 *ids = NULL;
    gsize n = bmk_window_selected_ids(w, &ids);
    if (n == 0) { bmk_toast(w, "%s", T("msg.no_selection")); return; }

    for (gsize i = 0; i < n && i < 10; i++) {
        bmk_bookmark *b = NULL;
        if (bmk_bookmark_get(w->core->db, ids[i], &b) == BMK_OK && b) {
            GtkUriLauncher *l = gtk_uri_launcher_new(b->url);
            gtk_uri_launcher_launch(l, w->window, NULL, NULL, NULL);
            g_object_unref(l);
            bmk_bookmark_free(b);
            free(b);
        }
    }
    g_free(ids);
}

static void act_copy_url(GSimpleAction *a, GVariant *p, gpointer user)
{
    (void)a; (void)p;
    BmkWindow *w = user;
    gint64 *ids = NULL;
    gsize n = bmk_window_selected_ids(w, &ids);
    if (n == 0) { bmk_toast(w, "%s", T("msg.no_selection")); return; }

    GString *s = g_string_new(NULL);
    for (gsize i = 0; i < n; i++) {
        bmk_bookmark *b = NULL;
        if (bmk_bookmark_get(w->core->db, ids[i], &b) == BMK_OK && b) {
            if (s->len) g_string_append_c(s, '\n');
            g_string_append(s, b->url);
            bmk_bookmark_free(b);
            free(b);
        }
    }
    gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(w->window)), s->str);
    bmk_toast(w, "%s", T("msg.copied"));
    g_string_free(s, TRUE);
    g_free(ids);
}

static void act_new_category(GSimpleAction *a, GVariant *p, gpointer user)
{
    (void)a; (void)p;
    BmkWindow *w = user;
    bmk_dialog_category_new(w, w->selected_category);   /* 默认挂在选中节点下 */
}

static void act_rename_category(GSimpleAction *a, GVariant *p, gpointer user)
{
    (void)a; (void)p;
    BmkWindow *w = user;
    if (w->selected_category > 0) bmk_dialog_category_rename(w, w->selected_category);
}

static void act_move_category(GSimpleAction *a, GVariant *p, gpointer user)
{
    (void)a; (void)p;
    BmkWindow *w = user;
    if (w->selected_category > 0) bmk_dialog_category_move(w, w->selected_category);
}

typedef struct { BmkWindow *w; gint64 cat; } DelCatCtx;

static void do_delete_category(gpointer user)
{
    DelCatCtx *c = user;
    bmk_err e = bmk_category_delete(c->w->core->db, c->cat);
    if (e != BMK_OK)
        bmk_error_dialog(c->w->window, "%s (%s)", T("msg.error"), bmk_err_str(e));
    else {
        c->w->filter.category_id = BMK_CATEGORY_ALL;
        c->w->selected_category = 0;
        bmk_window_refresh_all(c->w);
    }
    g_free(c);
}

static void act_delete_category(GSimpleAction *a, GVariant *p, gpointer user)
{
    (void)a; (void)p;
    BmkWindow *w = user;
    if (w->selected_category <= 0) return;

    int64_t affected = bmk_category_affected_bookmarks(w->core->db, w->selected_category);
    char *msg = g_strdup_printf(T("cat.delete_confirm"), (long long)affected);

    DelCatCtx *c = g_new0(DelCatCtx, 1);
    c->w = w; c->cat = w->selected_category;
    bmk_confirm_dialog(w->window, msg, NULL, T("tree.delete"), do_delete_category, c);
    g_free(msg);
}

static void act_select_all(GSimpleAction *a, GVariant *p, gpointer user)
{
    (void)a; (void)p;
    BmkWindow *w = user;
    GtkTreeIter it;
    if (!gtk_tree_model_get_iter_first(GTK_TREE_MODEL(w->list_store), &it)) return;
    do {
        gtk_list_store_set(w->list_store, &it, COL_SELECTED, TRUE, -1);
    } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(w->list_store), &it));
}

/* ---- 批量操作（FR-13） ---- */

typedef struct { BmkWindow *w; gint64 *ids; gsize n; } BatchCtx;

static void do_merge(gpointer user)
{
    BatchCtx *c = user;
    int64_t primary = 0;
    bmk_err e = bmk_merge_group(c->w->core->db, (const int64_t *)c->ids, c->n, &primary);
    if (e != BMK_OK)
        bmk_error_dialog(c->w->window, "%s (%s)", T("msg.error"), bmk_err_str(e));
    else {
        bmk_toast(c->w, T("merge.done"), (int)c->n);
        bmk_window_refresh_all(c->w);
    }
    g_free(c->ids);
    g_free(c);
}

static void act_merge_selection(GSimpleAction *a, GVariant *p, gpointer user)
{
    (void)a; (void)p;
    BmkWindow *w = user;
    gint64 *ids = NULL;
    gsize n = bmk_window_selected_ids(w, &ids);
    if (n < 2) { bmk_toast(w, "%s", T("msg.no_selection")); g_free(ids); return; }
    if (!selection_same_key(w, ids, n)) {
        bmk_toast(w, "%s", T("merge.not_same"));
        g_free(ids);
        return;
    }

    BatchCtx *c = g_new0(BatchCtx, 1);
    c->w = w; c->ids = ids; c->n = n;
    char *msg = g_strdup_printf(T("merge.confirm"), (int)n);
    bmk_confirm_dialog(w->window, msg, NULL, T("ctx.merge"), do_merge, c);   /* FR-16 第 2 条 */
    g_free(msg);
}

typedef struct {
    BmkWindow *w;
    gint64    *ids;
    gsize      n;
    GtkWidget *entry;
} TagsCtx;

static void batch_tags_response(GtkDialog *d, int response, gpointer user)
{
    TagsCtx *c = user;
    if (response == GTK_RESPONSE_ACCEPT) {
        const char *text = gtk_editable_get_text(GTK_EDITABLE(c->entry));
        GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
        if (text && *text) {
            char **parts = g_strsplit(text, ",", -1);
            for (int i = 0; parts[i]; i++) {
                char *t = g_strstrip(g_strdup(parts[i]));
                if (*t) g_ptr_array_add(names, t);
                else    g_free(t);
            }
            g_strfreev(parts);
        }
        bmk_err e = (names->len > 0)
            ? bmk_bookmark_add_tags(c->w->core->db, (const int64_t *)c->ids, c->n,
                                    (const char *const *)names->pdata, names->len)
            : BMK_ERR_INVALID_ARG;
        g_ptr_array_free(names, TRUE);
        if (e != BMK_OK)
            bmk_error_dialog(c->w->window, "%s (%s)", T("msg.error"), bmk_err_str(e));
        else {
            bmk_toast(c->w, T("batch.tags_done"), (int)c->n);
            bmk_window_refresh_all(c->w);
        }
    }
    gtk_window_destroy(GTK_WINDOW(d));
    g_free(c->ids);
    g_free(c);
}

static void act_add_tags(GSimpleAction *a, GVariant *p, gpointer user)
{
    (void)a; (void)p;
    BmkWindow *w = user;
    gint64 *ids = NULL;
    gsize n = bmk_window_selected_ids(w, &ids);
    if (n < 2) { bmk_toast(w, "%s", T("msg.no_selection")); g_free(ids); return; }

    TagsCtx *c = g_new0(TagsCtx, 1);
    c->w = w; c->ids = ids; c->n = n;

    GtkWidget *d = gtk_dialog_new_with_buttons(
        T("batch.tags_title"), GTK_WINDOW(w->window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        T("btn.cancel"), GTK_RESPONSE_CANCEL,
        T("btn.ok"), GTK_RESPONSE_ACCEPT, NULL);
    c->entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(c->entry), T("batch.tags_prompt"));
    gtk_widget_set_margin_start(c->entry, 12);
    gtk_widget_set_margin_end(c->entry, 12);
    gtk_widget_set_margin_top(c->entry, 12);
    gtk_widget_set_margin_bottom(c->entry, 12);
    gtk_box_append(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(d))), c->entry);
    g_signal_connect(d, "response", G_CALLBACK(batch_tags_response), c);
    gtk_widget_set_visible(c->entry, TRUE);
    gtk_widget_set_visible(d, TRUE);
}

typedef struct {
    BmkWindow *w;
    gint64    *ids;
    gsize      n;
    GtkWidget *combo;
    GArray    *cat_ids;
} CatCtx;

static void batch_cat_response(GtkDialog *d, int response, gpointer user)
{
    CatCtx *c = user;
    if (response == GTK_RESPONSE_ACCEPT) {
        guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(c->combo));
        int64_t cat = (sel != GTK_INVALID_LIST_POSITION && sel < c->cat_ids->len)
                      ? g_array_index(c->cat_ids, gint64, sel) : 0;
        bmk_err e = bmk_bookmark_set_category(c->w->core->db,
                                              (const int64_t *)c->ids, c->n, cat);
        if (e != BMK_OK)
            bmk_error_dialog(c->w->window, "%s (%s)", T("msg.error"), bmk_err_str(e));
        else {
            bmk_toast(c->w, T("batch.cat_done"), (int)c->n);
            bmk_window_refresh_all(c->w);
        }
    }
    gtk_window_destroy(GTK_WINDOW(d));
    g_array_free(c->cat_ids, TRUE);
    g_free(c->ids);
    g_free(c);
}

static void act_set_category(GSimpleAction *a, GVariant *p, gpointer user)
{
    (void)a; (void)p;
    BmkWindow *w = user;
    gint64 *ids = NULL;
    gsize n = bmk_window_selected_ids(w, &ids);
    if (n < 2) { bmk_toast(w, "%s", T("msg.no_selection")); g_free(ids); return; }

    CatCtx *c = g_new0(CatCtx, 1);
    c->w = w; c->ids = ids; c->n = n;
    c->cat_ids = g_array_new(FALSE, FALSE, sizeof(gint64));

    GtkStringList *sl = gtk_string_list_new(NULL);
    bmk_ui_fill_category_combo(w->core->db, sl, c->cat_ids, 0);
    c->combo = gtk_drop_down_new(G_LIST_MODEL(sl), NULL);

    GtkWidget *d = gtk_dialog_new_with_buttons(
        T("batch.cat_title"), GTK_WINDOW(w->window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        T("btn.cancel"), GTK_RESPONSE_CANCEL,
        T("btn.ok"), GTK_RESPONSE_ACCEPT, NULL);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);
    gtk_widget_set_margin_top(box, 12);
    gtk_widget_set_margin_bottom(box, 12);
    gtk_box_append(GTK_BOX(box), c->combo);
    gtk_box_append(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(d))), box);
    g_signal_connect(d, "response", G_CALLBACK(batch_cat_response), c);
    gtk_widget_set_visible(c->combo, TRUE);
    gtk_widget_set_visible(box, TRUE);
    gtk_widget_set_visible(d, TRUE);
}

static void act_toggle_filter(GSimpleAction *a, GVariant *p, gpointer user)
{
    (void)a; (void)p;
    BmkWindow *w = user;
    gboolean reveal = !gtk_revealer_get_reveal_child(GTK_REVEALER(w->filter_revealer));
    gtk_revealer_set_reveal_child(GTK_REVEALER(w->filter_revealer), reveal);
}

static const GActionEntry ACTIONS[] = {
    { "add",             act_add,             NULL, NULL, NULL, {0} },
    { "edit",            act_edit,            NULL, NULL, NULL, {0} },
    { "delete",          act_delete,          NULL, NULL, NULL, {0} },
    { "focus-search",    act_focus_search,    NULL, NULL, NULL, {0} },
    { "check",           act_check,           NULL, NULL, NULL, {0} },
    { "dedupe",          act_dedupe,          NULL, NULL, NULL, {0} },
    { "sync",            act_sync,            NULL, NULL, NULL, {0} },
    { "import",          act_import,          NULL, NULL, NULL, {0} },
    { "export",          act_export,          NULL, NULL, NULL, {0} },
    { "settings",        act_settings,        NULL, NULL, NULL, {0} },
    { "proxy",           act_proxy,           NULL, NULL, NULL, {0} },
    { "tags",            act_tags,            NULL, NULL, NULL, {0} },
    { "open",            act_open,            NULL, NULL, NULL, {0} },
    { "copy-url",        act_copy_url,        NULL, NULL, NULL, {0} },
    { "new-category",    act_new_category,    NULL, NULL, NULL, {0} },
    { "rename-category", act_rename_category, NULL, NULL, NULL, {0} },
    { "move-category",   act_move_category,   NULL, NULL, NULL, {0} },
    { "delete-category", act_delete_category, NULL, NULL, NULL, {0} },
    { "select-all",      act_select_all,      NULL, NULL, NULL, {0} },
    { "merge-selection", act_merge_selection, NULL, NULL, NULL, {0} },
    { "add-tags",        act_add_tags,        NULL, NULL, NULL, {0} },
    { "set-category",    act_set_category,    NULL, NULL, NULL, {0} },
    { "toggle-filter",   act_toggle_filter,   NULL, NULL, NULL, {0} },
};

/* 右键菜单（FR-13） */
static void on_list_right_click(GtkGestureClick *g, int n_press, double x, double y,
                                gpointer user)
{
    (void)n_press;
    BmkWindow *w = user;
    GtkTreeView *tv = GTK_TREE_VIEW(w->list_view);

    GtkTreePath *path = NULL;
    if (gtk_tree_view_get_path_at_pos(tv, (int)x, (int)y, &path, NULL, NULL, NULL)) {
        GtkTreeSelection *sel = gtk_tree_view_get_selection(tv);
        if (!gtk_tree_selection_path_is_selected(sel, path)) {
            gtk_tree_selection_unselect_all(sel);
            gtk_tree_selection_select_path(sel, path);
        }
        gtk_tree_path_free(path);
    }

    /* 按启用条件更新动作状态（FR-13 第 2 条） */
    gint64 *ids = NULL;
    gsize n = bmk_window_selected_ids(w, &ids);
    bool same_key = (n >= 2) && selection_same_key(w, ids, n);

    GAction *ga;
    ga = g_action_map_lookup_action(G_ACTION_MAP(w->window), "open");
    if (ga) g_simple_action_set_enabled(G_SIMPLE_ACTION(ga), n == 1);
    ga = g_action_map_lookup_action(G_ACTION_MAP(w->window), "copy-url");
    if (ga) g_simple_action_set_enabled(G_SIMPLE_ACTION(ga), n >= 1);
    ga = g_action_map_lookup_action(G_ACTION_MAP(w->window), "edit");
    if (ga) g_simple_action_set_enabled(G_SIMPLE_ACTION(ga), n == 1);
    ga = g_action_map_lookup_action(G_ACTION_MAP(w->window), "check");
    if (ga) g_simple_action_set_enabled(G_SIMPLE_ACTION(ga), n >= 1);
    ga = g_action_map_lookup_action(G_ACTION_MAP(w->window), "delete");
    if (ga) g_simple_action_set_enabled(G_SIMPLE_ACTION(ga), n >= 1);
    ga = g_action_map_lookup_action(G_ACTION_MAP(w->window), "merge-selection");
    if (ga) g_simple_action_set_enabled(G_SIMPLE_ACTION(ga), n >= 2 && same_key);
    ga = g_action_map_lookup_action(G_ACTION_MAP(w->window), "add-tags");
    if (ga) g_simple_action_set_enabled(G_SIMPLE_ACTION(ga), n >= 2);
    ga = g_action_map_lookup_action(G_ACTION_MAP(w->window), "set-category");
    if (ga) g_simple_action_set_enabled(G_SIMPLE_ACTION(ga), n >= 2);
    g_free(ids);

    GMenu *menu = g_menu_new();
    g_menu_append(menu, T("ctx.open"),           "win.open");
    g_menu_append(menu, T("ctx.copy_url"),       "win.copy-url");
    g_menu_append(menu, T("ctx.edit"),           "win.edit");
    g_menu_append(menu, T("ctx.check_now"),      "win.check");
    g_menu_append(menu, T("ctx.merge"),          "win.merge-selection");
    g_menu_append(menu, T("ctx.batch_tag"),      "win.add-tags");
    g_menu_append(menu, T("ctx.batch_category"), "win.set-category");
    g_menu_append(menu, T("ctx.delete"),         "win.delete");

    GtkWidget *pop = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
    gtk_widget_set_parent(pop, GTK_WIDGET(tv));
    gtk_popover_set_has_arrow(GTK_POPOVER(pop), FALSE);
    gtk_popover_set_pointing_to(GTK_POPOVER(pop),
                                &(GdkRectangle){ (int)x, (int)y, 1, 1 });
    gtk_popover_popup(GTK_POPOVER(pop));
    g_object_unref(menu);
    (void)g;
}

static void on_tree_right_click(GtkGestureClick *g, int n_press, double x, double y,
                                gpointer user)
{
    (void)n_press; (void)g;
    BmkWindow *w = user;

    GMenu *menu = g_menu_new();
    g_menu_append(menu, T("tree.new_category"), "win.new-category");
    if (w->selected_category > 0) {
        g_menu_append(menu, T("tree.rename"), "win.rename-category");
        g_menu_append(menu, T("tree.move"),   "win.move-category");
        g_menu_append(menu, T("tree.delete"), "win.delete-category");
    }

    GtkWidget *pop = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
    gtk_widget_set_parent(pop, w->tree_view);
    gtk_popover_set_has_arrow(GTK_POPOVER(pop), FALSE);
    gtk_popover_set_pointing_to(GTK_POPOVER(pop),
                                &(GdkRectangle){ (int)x, (int)y, 1, 1 });
    gtk_popover_popup(GTK_POPOVER(pop));
    g_object_unref(menu);
}

/* ------------------------------------------------------------------ */
/* 筛选面板（FR-12）                                                   */
/* ------------------------------------------------------------------ */

static void clear_box_children(GtkWidget *box)
{
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(box)) != NULL)
        gtk_widget_unparent(child);
}

static void filter_apply_from_widgets(BmkWindow *w)
{
    const char *s;

    s = gtk_editable_get_text(GTK_EDITABLE(w->filter_url_entry));
    free(w->filter.url_contains);
    w->filter.url_contains = (s && *s) ? bmk_strdup(s) : NULL;

    s = gtk_editable_get_text(GTK_EDITABLE(w->filter_domain_entry));
    free(w->filter.domain);
    w->filter.domain = (s && *s) ? bmk_strdup(s) : NULL;

    /* 有效性：全部/有效/无效/未校验/待复测 */
    static const bmk_valid_filter valid_map[] = {
        BMK_VF_ALL, BMK_VF_VALID, BMK_VF_INVALID,
        BMK_VF_UNCHECKED, BMK_VF_BLOCKED,
    };
    guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(w->filter_valid_combo));
    w->filter.valid_filter = (sel < G_N_ELEMENTS(valid_map)) ? valid_map[sel] : BMK_VF_ALL;

    /* 状态码区间：留空 = 不限 */
    const char *t1 = gtk_editable_get_text(GTK_EDITABLE(w->filter_status_min));
    const char *t2 = gtk_editable_get_text(GTK_EDITABLE(w->filter_status_max));
    w->filter.status_min = (t1 && *t1) ? atoi(t1) : -1;
    w->filter.status_max = (t2 && *t2) ? atoi(t2) : -1;

    bmk_window_refresh_list(w);
}

static void filter_entry_changed(GtkEditable *e, gpointer user)
{
    (void)e;
    BmkWindow *w = user;
    if (w->filter_guard) return;
    filter_apply_from_widgets(w);
}

static void filter_combo_changed(GObject *o, GParamSpec *ps, gpointer user)
{
    (void)o; (void)ps;
    BmkWindow *w = user;
    if (w->filter_guard) return;
    filter_apply_from_widgets(w);
}

static void filter_tag_toggled(GtkCheckButton *btn, gpointer user)
{
    (void)btn;
    BmkWindow *w = user;
    if (w->filter_guard) return;

    free(w->filter.tag_ids);
    w->filter.tag_ids = NULL;
    w->filter.tag_count = 0;

    GArray *ids = g_array_new(FALSE, FALSE, sizeof(int64_t));
    for (GtkWidget *c = gtk_widget_get_first_child(w->filter_tags_box);
         c; c = gtk_widget_get_next_sibling(c)) {
        if (!GTK_IS_CHECK_BUTTON(c)) continue;
        if (!gtk_check_button_get_active(GTK_CHECK_BUTTON(c))) continue;
        gint64 *pid = g_object_get_data(G_OBJECT(c), "tag-id");
        if (pid) g_array_append_val(ids, *pid);
    }
    if (ids->len) {
        w->filter.tag_ids = (int64_t *)malloc(ids->len * sizeof(int64_t));
        if (w->filter.tag_ids) {
            memcpy(w->filter.tag_ids, ids->data, ids->len * sizeof(int64_t));
            w->filter.tag_count = ids->len;
        }
    }
    g_array_free(ids, TRUE);
    bmk_window_refresh_list(w);
}

static void rebuild_filter_tags(BmkWindow *w)
{
    w->filter_guard = TRUE;
    clear_box_children(w->filter_tags_box);

    bmk_tag **tags = NULL;
    size_t n = 0;
    if (bmk_tag_list(w->core->db, &tags, &n) == BMK_OK) {
        for (size_t i = 0; i < n; i++) {
            GtkWidget *cb = gtk_check_button_new_with_label(tags[i]->name);
            gint64 *pid = g_new(int64_t, 1);
            *pid = tags[i]->id;
            g_object_set_data_full(G_OBJECT(cb), "tag-id", pid, g_free);
            g_signal_connect(cb, "toggled", G_CALLBACK(filter_tag_toggled), w);
            gtk_box_append(GTK_BOX(w->filter_tags_box), cb);

            gboolean active = FALSE;
            for (size_t k = 0; k < w->filter.tag_count; k++)
                if (w->filter.tag_ids[k] == tags[i]->id) { active = TRUE; break; }
            gtk_check_button_set_active(GTK_CHECK_BUTTON(cb), active);
        }
        bmk_tag_list_free(tags, n);
    }
    w->filter_guard = FALSE;
}

static void on_clear_filters(GtkButton *btn, gpointer user)
{
    (void)btn;
    BmkWindow *w = user;

    if (w->search_timer) { g_source_remove(w->search_timer); w->search_timer = 0; }
    w->filter_guard = TRUE;
    gtk_editable_set_text(GTK_EDITABLE(w->search_entry), "");
    gtk_editable_set_text(GTK_EDITABLE(w->filter_url_entry), "");
    gtk_editable_set_text(GTK_EDITABLE(w->filter_domain_entry), "");
    gtk_drop_down_set_selected(GTK_DROP_DOWN(w->filter_valid_combo), 0);
    gtk_editable_set_text(GTK_EDITABLE(w->filter_status_min), "");
    gtk_editable_set_text(GTK_EDITABLE(w->filter_status_max), "");
    for (GtkWidget *c = gtk_widget_get_first_child(w->filter_tags_box);
         c; c = gtk_widget_get_next_sibling(c))
        if (GTK_IS_CHECK_BUTTON(c))
            gtk_check_button_set_active(GTK_CHECK_BUTTON(c), FALSE);
    w->filter_guard = FALSE;

    free(w->filter.keyword);    w->filter.keyword = NULL;
    free(w->filter.url_contains); w->filter.url_contains = NULL;
    free(w->filter.domain);     w->filter.domain = NULL;
    free(w->filter.tag_ids);    w->filter.tag_ids = NULL; w->filter.tag_count = 0;
    w->filter.valid_filter = BMK_VF_ALL;
    w->filter.status_min = w->filter.status_max = -1;
    bmk_window_refresh_list(w);
}

static GtkWidget *build_filter_panel(BmkWindow *w)
{
    w->filter_revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(w->filter_revealer),
                                     GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    gtk_revealer_set_reveal_child(GTK_REVEALER(w->filter_revealer), FALSE);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(row, 10);
    gtk_widget_set_margin_end(row, 10);
    gtk_widget_set_margin_top(row, 6);
    gtk_widget_set_margin_bottom(row, 6);

    /* 网址包含 */
    GtkWidget *l1 = gtk_label_new(T("filter.url"));
    w->filter_url_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(w->filter_url_entry), T("filter.url"));
    gtk_widget_set_size_request(w->filter_url_entry, 160, -1);
    g_signal_connect(w->filter_url_entry, "changed",
                     G_CALLBACK(filter_entry_changed), w);
    gtk_box_append(GTK_BOX(row), l1);
    gtk_box_append(GTK_BOX(row), w->filter_url_entry);

    /* 域名 */
    GtkWidget *l2 = gtk_label_new(T("filter.domain"));
    w->filter_domain_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(w->filter_domain_entry), T("filter.domain"));
    gtk_widget_set_size_request(w->filter_domain_entry, 140, -1);
    g_signal_connect(w->filter_domain_entry, "changed",
                     G_CALLBACK(filter_entry_changed), w);
    gtk_box_append(GTK_BOX(row), l2);
    gtk_box_append(GTK_BOX(row), w->filter_domain_entry);

    /* 有效性 */
    GtkWidget *l3 = gtk_label_new(T("filter.valid"));
    GtkStringList *vl = gtk_string_list_new(NULL);
    gtk_string_list_append(vl, T("filter.vf_all"));
    gtk_string_list_append(vl, T("filter.vf_valid"));
    gtk_string_list_append(vl, T("filter.vf_invalid"));
    gtk_string_list_append(vl, T("filter.vf_unchecked"));
    gtk_string_list_append(vl, T("filter.vf_blocked"));
    w->filter_valid_combo = gtk_drop_down_new(G_LIST_MODEL(vl), NULL);
    g_signal_connect(w->filter_valid_combo, "notify::selected",
                     G_CALLBACK(filter_combo_changed), w);
    gtk_box_append(GTK_BOX(row), l3);
    gtk_box_append(GTK_BOX(row), w->filter_valid_combo);

    /* 状态码区间 */
    GtkWidget *l4 = gtk_label_new(T("filter.status_min"));
    w->filter_status_min = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(w->filter_status_min), "400");
    gtk_widget_set_size_request(w->filter_status_min, 64, -1);
    g_signal_connect(w->filter_status_min, "changed",
                     G_CALLBACK(filter_entry_changed), w);
    GtkWidget *l5 = gtk_label_new(T("filter.status_max"));
    w->filter_status_max = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(w->filter_status_max), "499");
    gtk_widget_set_size_request(w->filter_status_max, 64, -1);
    g_signal_connect(w->filter_status_max, "changed",
                     G_CALLBACK(filter_entry_changed), w);
    gtk_box_append(GTK_BOX(row), l4);
    gtk_box_append(GTK_BOX(row), w->filter_status_min);
    gtk_box_append(GTK_BOX(row), l5);
    gtk_box_append(GTK_BOX(row), w->filter_status_max);

    /* 标签多选（popover） */
    w->filter_tags_button = gtk_menu_button_new();
    gtk_menu_button_set_child(GTK_MENU_BUTTON(w->filter_tags_button),
                              gtk_label_new(T("filter.tags")));
    w->filter_tags_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *tag_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(tag_scroll), w->filter_tags_box);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(tag_scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(tag_scroll, 180, 220);
    /* GtkMenuButton 不会自建 popover：显式创建并挂载 */
    GtkWidget *tag_pop = gtk_popover_new();
    gtk_popover_set_child(GTK_POPOVER(tag_pop), tag_scroll);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(w->filter_tags_button), tag_pop);
    gtk_box_append(GTK_BOX(row), w->filter_tags_button);

    /* 清除 */
    GtkWidget *clear = gtk_button_new_with_label(T("filter.clear"));
    g_signal_connect(clear, "clicked", G_CALLBACK(on_clear_filters), w);
    gtk_box_append(GTK_BOX(row), clear);

    gtk_revealer_set_child(GTK_REVEALER(w->filter_revealer), row);
    return w->filter_revealer;
}

/* ------------------------------------------------------------------ */
/* 构建                                                                */
/* ------------------------------------------------------------------ */

static GtkWidget *toolbar_button(const char *icon, const char *label,
                                 const char *action)
{
    GtkWidget *b = gtk_button_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(box), gtk_image_new_from_icon_name(icon));
    gtk_box_append(GTK_BOX(box), gtk_label_new(label));
    gtk_button_set_child(GTK_BUTTON(b), box);
    gtk_actionable_set_action_name(GTK_ACTIONABLE(b), action);
    gtk_widget_set_tooltip_text(b, label);
    return b;
}

static void add_text_column(BmkWindow *w, const char *title, int col,
                            bmk_sort_key key, int width, gboolean expand)
{
    GtkCellRenderer *cr = gtk_cell_renderer_text_new();
    g_object_set(cr, "ellipsize", PANGO_ELLIPSIZE_END, NULL);

    GtkTreeViewColumn *c =
        gtk_tree_view_column_new_with_attributes(title, cr, "text", col, NULL);
    gtk_tree_view_column_set_resizable(c, TRUE);           /* FR-20 第 3 条 */
    gtk_tree_view_column_set_clickable(c, TRUE);
    gtk_tree_view_column_set_sizing(c, GTK_TREE_VIEW_COLUMN_FIXED);
    gtk_tree_view_column_set_fixed_width(c, width);
    gtk_tree_view_column_set_expand(c, expand);
    g_object_set_data(G_OBJECT(c), "sort-key", GINT_TO_POINTER(key));
    g_signal_connect(c, "clicked", G_CALLBACK(on_column_clicked), w);
    gtk_tree_view_append_column(GTK_TREE_VIEW(w->list_view), c);
}

static GtkWidget *build_empty_page(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(box, GTK_ALIGN_CENTER);

    GtkWidget *icon = gtk_image_new_from_icon_name("user-bookmarks-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 64);
    gtk_widget_add_css_class(icon, "dim-label");

    GtkWidget *title = gtk_label_new(T("empty.title"));
    gtk_widget_add_css_class(title, "title-2");

    GtkWidget *body = gtk_label_new(T("empty.body"));
    gtk_widget_add_css_class(body, "dim-label");
    gtk_label_set_wrap(GTK_LABEL(body), TRUE);
    gtk_label_set_justify(GTK_LABEL(body), GTK_JUSTIFY_CENTER);

    GtkWidget *btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(btns, GTK_ALIGN_CENTER);
    GtkWidget *add = gtk_button_new_with_label(T("tb.add"));
    gtk_actionable_set_action_name(GTK_ACTIONABLE(add), "win.add");
    gtk_widget_add_css_class(add, "suggested-action");
    GtkWidget *imp = gtk_button_new_with_label(T("tb.import"));
    gtk_actionable_set_action_name(GTK_ACTIONABLE(imp), "win.import");
    gtk_box_append(GTK_BOX(btns), add);
    gtk_box_append(GTK_BOX(btns), imp);

    gtk_box_append(GTK_BOX(box), icon);
    gtk_box_append(GTK_BOX(box), title);
    gtk_box_append(GTK_BOX(box), body);
    gtk_box_append(GTK_BOX(box), btns);
    return box;
}

static GtkWidget *status_label(GtkWidget *bar, gboolean separator_before)
{
    if (separator_before)
        gtk_box_append(GTK_BOX(bar),
                       gtk_separator_new(GTK_ORIENTATION_VERTICAL));
    GtkWidget *l = gtk_label_new("");
    gtk_widget_add_css_class(l, "dim-label");
    gtk_box_append(GTK_BOX(bar), l);
    return l;
}

static void on_window_destroy(GtkWidget *widget, gpointer user)
{
    (void)widget;
    BmkWindow *w = user;
    if (w->search_timer) g_source_remove(w->search_timer);
    if (w->toast_timer) g_source_remove(w->toast_timer);
    bmk_filter_free(&w->filter);
    if (w->category_paths) g_hash_table_destroy(w->category_paths);
    g_free(w);
}

BmkWindow *bmk_window_new(GtkApplication *app, bmk_app *core)
{
    BmkWindow *w = g_new0(BmkWindow, 1);
    w->app  = app;
    w->core = core;
    bmk_filter_init(&w->filter);
    w->sort_key = BMK_SORT_CREATED_AT;
    w->sort_asc = FALSE;               /* FR-09 第 3 条：默认添加时间降序 */

    GtkWidget *win = gtk_application_window_new(app);
    w->window = GTK_WINDOW(win);
    gtk_window_set_title(w->window, T("app.title"));
    gtk_window_set_default_size(w->window, 1200, 760);    /* FR-20 第 1 条 */

    g_action_map_add_action_entries(G_ACTION_MAP(win), ACTIONS,
                                    G_N_ELEMENTS(ACTIONS), w);

    /* 快捷键（FR-20 第 4 条） */
    struct { const char *accel, *action; } keys[] = {
        { "<Control>n",      "win.add" },
        { "<Control>e",      "win.edit" },
        { "Delete",          "win.delete" },
        { "<Control>f",      "win.focus-search" },
        { "F5",              "win.check" },
        { "<Control>d",      "win.dedupe" },
        { "<Control>s",      "win.sync" },
        { "<Control>i",      "win.import" },
        { "<Control>a",      "win.select-all" },
        { "<Control>comma",  "win.settings" },
    };
    for (size_t i = 0; i < G_N_ELEMENTS(keys); i++) {
        const char *accels[] = { keys[i].accel, NULL };
        gtk_application_set_accels_for_action(app, keys[i].action, accels);
    }

    /* ---- 顶部工具栏 ---- */
    GtkWidget *header = gtk_header_bar_new();
    gtk_window_set_titlebar(w->window, header);

    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_append(GTK_BOX(left), toolbar_button("system-search-symbolic",
                                                 T("tb.filter"), "win.toggle-filter"));
    gtk_box_append(GTK_BOX(left), toolbar_button("list-add-symbolic",
                                                 T("tb.add"), "win.add"));
    gtk_box_append(GTK_BOX(left), toolbar_button("document-open-symbolic",
                                                 T("tb.import"), "win.import"));
    gtk_box_append(GTK_BOX(left), toolbar_button("document-save-symbolic",
                                                 T("tb.export"), "win.export"));
    gtk_box_append(GTK_BOX(left), gtk_separator_new(GTK_ORIENTATION_VERTICAL));
    gtk_box_append(GTK_BOX(left), toolbar_button("emblem-ok-symbolic",
                                                 T("tb.check"), "win.check"));
    gtk_box_append(GTK_BOX(left), toolbar_button("edit-copy-symbolic",
                                                 T("tb.dedupe"), "win.dedupe"));
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), left);

    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_append(GTK_BOX(right), toolbar_button("preferences-system-symbolic",
                                                  T("tb.settings"), "win.settings"));
    gtk_box_append(GTK_BOX(right), toolbar_button("network-workgroup-symbolic",
                                                  T("tb.proxy"), "win.proxy"));
    gtk_box_append(GTK_BOX(right), toolbar_button("folder-remote-symbolic",
                                                  T("tb.sync"), "win.sync"));
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), right);

    w->search_entry = gtk_search_entry_new();
    gtk_widget_set_size_request(w->search_entry, 280, -1);
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(w->search_entry),
                                          T("tb.search_hint"));
    g_signal_connect(w->search_entry, "search-changed",
                     G_CALLBACK(on_search_changed), w);
    gtk_header_bar_set_title_widget(GTK_HEADER_BAR(header), w->search_entry);

    /* ---- 左侧分类树 ---- */
    w->tree_store = gtk_tree_store_new(N_TREE_COLS, G_TYPE_STRING,
                                       G_TYPE_INT, G_TYPE_INT64);
    w->tree_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(w->tree_store));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(w->tree_view), FALSE);
    gtk_tree_view_append_column(
        GTK_TREE_VIEW(w->tree_view),
        gtk_tree_view_column_new_with_attributes(
            T("tree.categories"), gtk_cell_renderer_text_new(),
            "text", TCOL_LABEL, NULL));

    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(w->tree_view)),
                     "changed", G_CALLBACK(on_tree_selection_changed), w);

    GtkGesture *tg = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(tg), GDK_BUTTON_SECONDARY);
    g_signal_connect(tg, "pressed", G_CALLBACK(on_tree_right_click), w);
    gtk_widget_add_controller(w->tree_view, GTK_EVENT_CONTROLLER(tg));

    GtkWidget *tree_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(tree_scroll), w->tree_view);
    gtk_widget_set_vexpand(tree_scroll, TRUE);

    GtkWidget *tree_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(tree_box), tree_scroll);
    GtkWidget *newcat = gtk_button_new_with_label(T("tree.new_category"));
    gtk_actionable_set_action_name(GTK_ACTIONABLE(newcat), "win.new-category");
    gtk_widget_set_margin_start(newcat, 6);
    gtk_widget_set_margin_end(newcat, 6);
    gtk_widget_set_margin_bottom(newcat, 6);
    gtk_box_append(GTK_BOX(tree_box), newcat);

    /* ---- 中部列表 ---- */
    w->list_store = gtk_list_store_new(N_LIST_COLS,
                                       G_TYPE_BOOLEAN,   /* selected */
                                       GDK_TYPE_PIXBUF,
                                       G_TYPE_STRING, G_TYPE_STRING,
                                       G_TYPE_STRING, G_TYPE_STRING,
                                       G_TYPE_STRING, G_TYPE_STRING,
                                       G_TYPE_STRING, G_TYPE_STRING,
                                       G_TYPE_INT64);
    w->list_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(w->list_store));
    gtk_tree_view_set_enable_search(GTK_TREE_VIEW(w->list_view), FALSE);
    gtk_tree_selection_set_mode(
        gtk_tree_view_get_selection(GTK_TREE_VIEW(w->list_view)),
        GTK_SELECTION_MULTIPLE);

    /* 选择框列 */
    GtkCellRenderer *toggle = gtk_cell_renderer_toggle_new();
    g_signal_connect(toggle, "toggled", G_CALLBACK(on_select_toggled), w);
    gtk_tree_view_append_column(
        GTK_TREE_VIEW(w->list_view),
        gtk_tree_view_column_new_with_attributes(
            "", toggle, "active", COL_SELECTED, NULL));

    /* 图标列 */
    GtkTreeViewColumn *icol = gtk_tree_view_column_new_with_attributes(
        "", gtk_cell_renderer_pixbuf_new(), "pixbuf", COL_ICON, NULL);
    gtk_tree_view_column_set_fixed_width(icol, 32);
    gtk_tree_view_column_set_sizing(icol, GTK_TREE_VIEW_COLUMN_FIXED);
    gtk_tree_view_append_column(GTK_TREE_VIEW(w->list_view), icol);

    /* 有效性圆点（FR-20 第 6 条） */
    GtkTreeViewColumn *dot = gtk_tree_view_column_new_with_attributes(
        "", gtk_cell_renderer_text_new(), "text", COL_VALID_DOT, NULL);
    gtk_tree_view_column_set_fixed_width(dot, 28);
    gtk_tree_view_column_set_sizing(dot, GTK_TREE_VIEW_COLUMN_FIXED);
    gtk_tree_view_append_column(GTK_TREE_VIEW(w->list_view), dot);

    add_text_column(w, T("col.title"),       COL_TITLE,        BMK_SORT_TITLE,        280, TRUE);
    add_text_column(w, T("col.url"),         COL_URL,          BMK_SORT_URL,          320, TRUE);
    add_text_column(w, T("col.category"),    COL_CATEGORY,     BMK_SORT_CATEGORY,     140, FALSE);
    add_text_column(w, T("col.tags"),        COL_TAGS,         BMK_SORT_TITLE,        140, FALSE);
    add_text_column(w, T("col.valid"),       COL_VALID_TEXT,   BMK_SORT_VALID,         90, FALSE);
    add_text_column(w, T("col.status_code"), COL_STATUS_TEXT,  BMK_SORT_STATUS_CODE,   70, FALSE);
    add_text_column(w, T("col.created"),     COL_CREATED_TEXT, BMK_SORT_CREATED_AT,   140, FALSE);

    g_signal_connect(w->list_view, "row-activated", G_CALLBACK(on_row_activated), w);

    /* 框选（FR-13 第 1 条） */
    GtkGesture *dg = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(dg), GDK_BUTTON_PRIMARY);
    g_signal_connect(dg, "drag-begin",  G_CALLBACK(on_drag_begin), w);
    g_signal_connect(dg, "drag-update", G_CALLBACK(on_drag_update), w);
    g_signal_connect(dg, "drag-end",    G_CALLBACK(on_drag_end), w);
    gtk_widget_add_controller(w->list_view, GTK_EVENT_CONTROLLER(dg));

    GtkGesture *lg = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(lg), GDK_BUTTON_SECONDARY);
    g_signal_connect(lg, "pressed", G_CALLBACK(on_list_right_click), w);
    gtk_widget_add_controller(w->list_view, GTK_EVENT_CONTROLLER(lg));

    w->list_scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(w->list_scroller), w->list_view);
    gtk_widget_set_vexpand(w->list_scroller, TRUE);
    gtk_widget_set_hexpand(w->list_scroller, TRUE);

    w->empty_page = build_empty_page();
    w->list_stack = gtk_stack_new();
    gtk_stack_add_named(GTK_STACK(w->list_stack), w->list_scroller, "list");
    gtk_stack_add_named(GTK_STACK(w->list_stack), w->empty_page, "empty");

    /* ---- 分栏（左树可拖拽调宽，FR-20 第 1 条） ---- */
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_start_child(GTK_PANED(paned), tree_box);
    gtk_paned_set_end_child(GTK_PANED(paned), w->list_stack);
    gtk_paned_set_position(GTK_PANED(paned), 220);
    gtk_paned_set_resize_start_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_shrink_start_child(GTK_PANED(paned), FALSE);

    /* ---- 底部状态栏 ---- */
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_margin_start(bar, 10);
    gtk_widget_set_margin_end(bar, 10);
    gtk_widget_set_margin_top(bar, 4);
    gtk_widget_set_margin_bottom(bar, 4);
    w->status_total    = status_label(bar, FALSE);
    w->status_filtered = status_label(bar, TRUE);
    w->status_invalid  = status_label(bar, TRUE);
    w->status_blocked  = status_label(bar, TRUE);
    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(bar), spacer);
    w->status_sync = status_label(bar, FALSE);

    /* ---- toast（FR-22 第 2 条） ---- */
    w->toast_label = gtk_label_new("");
    gtk_widget_set_margin_start(w->toast_label, 16);
    gtk_widget_set_margin_end(w->toast_label, 16);
    gtk_widget_set_margin_top(w->toast_label, 10);
    gtk_widget_set_margin_bottom(w->toast_label, 10);

    GtkWidget *toast_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_append(GTK_BOX(toast_box), w->toast_label);
    gtk_widget_add_css_class(toast_box, "osd");
    gtk_widget_add_css_class(toast_box, "toast");

    w->toast_revealer = gtk_revealer_new();
    gtk_revealer_set_child(GTK_REVEALER(w->toast_revealer), toast_box);
    gtk_revealer_set_transition_type(GTK_REVEALER(w->toast_revealer),
                                     GTK_REVEALER_TRANSITION_TYPE_SLIDE_UP);
    gtk_widget_set_halign(w->toast_revealer, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(w->toast_revealer, GTK_ALIGN_END);
    gtk_widget_set_margin_bottom(w->toast_revealer, 48);
    gtk_widget_set_can_target(w->toast_revealer, FALSE);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(root), build_filter_panel(w));
    gtk_box_append(GTK_BOX(root), paned);
    gtk_box_append(GTK_BOX(root), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    gtk_box_append(GTK_BOX(root), bar);

    w->toast_overlay = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(w->toast_overlay), root);
    gtk_overlay_add_overlay(GTK_OVERLAY(w->toast_overlay), w->toast_revealer);
    gtk_window_set_child(w->window, w->toast_overlay);

    g_signal_connect(win, "destroy", G_CALLBACK(on_window_destroy), w);

    /* 先呈现窗口再刷新：树视图在 realize 前填充时，GTK 会对每个固定节点
     * 触发一次 selection-changed（最后一个是“无效”节点），导致 valid_filter
     * 被错误置为仅显示无效链接，新增书签（valid=UNKNOWN）会从列表消失。
     * 先 present 使树视图完成 realize，刷新时不再产生这些伪事件。 */
    gtk_window_present(w->window);
    bmk_window_refresh_all(w);

    /* FR-01 第 2 条：损坏恢复提示 */
    if (core->db_recovered)
        bmk_error_dialog(w->window, "%s", T("msg.db_corrupt"));

    return w;
}
