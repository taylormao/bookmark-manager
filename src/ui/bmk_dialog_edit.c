/*
 * bmk_dialog_edit.c - 添加/编辑网址（FR-02 / FR-05）与分类、标签对话框（FR-10 / FR-11）
 */
#include "bmk_ui.h"

#include "bmk_url.h"
#include "bmk_fetch.h"
#include "bmk_settings.h"

#include <stdlib.h>
#include <string.h>

/* ================================================================== */
/* 添加 / 编辑网址                                                      */
/* ================================================================== */

typedef struct {
    BmkWindow *w;
    gint64     id;              /* 0 = 新增 */
    GtkWindow *dlg;
    GtkWidget *url_entry;
    GtkWidget *title_entry;
    GtkWidget *tags_entry;
    GtkWidget *cat_combo;
    GtkWidget *hint;
    GtkWidget *fetch_btn;
    GtkWidget *spinner;
    GArray    *cat_ids;

    /* 抓取结果暂存（后台线程写，主线程读） */
    unsigned char *icon;
    size_t         icon_len;
    char          *icon_mime;

    /* FR-02 第 3 条：自动抓取的防抖 source id（0 = 未安排） */
    guint fetch_debounce_id;
} EditCtx;

static void edit_ctx_free(EditCtx *c)
{
    if (!c) return;
    if (c->fetch_debounce_id) {
        g_source_remove(c->fetch_debounce_id);
        c->fetch_debounce_id = 0;
    }
    if (c->cat_ids) g_array_free(c->cat_ids, TRUE);
    free(c->icon);
    free(c->icon_mime);
    g_free(c);
}

/* ---- 后台抓取（FR-03，不阻塞 UI） ---- */

typedef struct {
    char             *url;
    bmk_fetch_result  res;
    bmk_err           err;
    bool              use_icon_service;  /* FR-03 第 3c 条（主线程读取，线程内使用） */
} FetchJob;

static void fetch_job_free(gpointer p)
{
    FetchJob *j = p;
    g_free(j->url);
    bmk_fetch_result_free(&j->res);
    g_free(j);
}

static void fetch_thread(GTask *task, gpointer src, gpointer data, GCancellable *c)
{
    (void)src; (void)c;
    FetchJob *j = data;

    bmk_fetch_opts o;
    bmk_fetch_opts_init(&o);
    o.use_icon_service = j->use_icon_service;
    j->err = bmk_fetch_page(j->url, &o, &j->res);
    g_task_return_boolean(task, TRUE);
}

/* FR-03 第 7 条：301/308 且最终 URL 不同 → 询问是否改用最终网址 */
static void on_final_url_response(GtkDialog *q, int response, gpointer user)
{
    (void)user;
    EditCtx    *c   = g_object_get_data(G_OBJECT(q), "bmk-edit-ctx");
    const char *furl = g_object_get_data(G_OBJECT(q), "bmk-final-url");
    if (response == GTK_RESPONSE_YES && c && furl && *furl) {
        /* set_text 会触发 changed，查重与自动抓取随之更新 */
        gtk_editable_set_text(GTK_EDITABLE(c->url_entry), furl);
    }
    gtk_window_destroy(GTK_WINDOW(q));
}

static void fetch_done(GObject *src, GAsyncResult *r, gpointer user)
{
    (void)src; (void)r;
    EditCtx  *c = user;
    FetchJob *j = g_task_get_task_data(G_TASK(r));

    gtk_widget_set_visible(c->spinner, FALSE);
    gtk_widget_set_sensitive(c->fetch_btn, TRUE);

    if (j->err != BMK_OK) {
        /* FR-03 第 5 条：抓取失败不阻断保存 */
        gtk_label_set_text(GTK_LABEL(c->hint), T("edit.fetch_failed"));
        return;
    }

    if (j->res.permanent_redirect && j->res.final_url && *j->res.final_url) {
        GtkWidget *q = gtk_message_dialog_new(
            c->dlg, GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO,
            "%s\n%s", T("edit.final_url_ask"), j->res.final_url);
        gtk_window_set_title(GTK_WINDOW(q), T("edit.final_url_title"));
        g_object_set_data_full(G_OBJECT(q), "bmk-final-url",
                               g_strdup(j->res.final_url), g_free);
        g_object_set_data(G_OBJECT(q), "bmk-edit-ctx", c);
        g_signal_connect(q, "response", G_CALLBACK(on_final_url_response), NULL);
        gtk_widget_set_visible(q, TRUE);
    }

    const char *cur = gtk_editable_get_text(GTK_EDITABLE(c->title_entry));
    if ((!cur || !*cur) && j->res.title && *j->res.title)
        gtk_editable_set_text(GTK_EDITABLE(c->title_entry), j->res.title);

    free(c->icon);      c->icon = NULL; c->icon_len = 0;
    free(c->icon_mime); c->icon_mime = NULL;
    if (j->res.icon && j->res.icon_len) {
        c->icon = (unsigned char *)malloc(j->res.icon_len);
        if (c->icon) {
            memcpy(c->icon, j->res.icon, j->res.icon_len);
            c->icon_len  = j->res.icon_len;
            c->icon_mime = bmk_strdup(j->res.icon_mime);
        }
    }
    gtk_label_set_text(GTK_LABEL(c->hint), "");
}

static void start_fetch(EditCtx *c)
{
    const char *url = gtk_editable_get_text(GTK_EDITABLE(c->url_entry));
    const char *reason = NULL;
    if (!bmk_url_is_valid_input(url, &reason)) {
        gtk_label_set_text(GTK_LABEL(c->hint), T("edit.invalid_url"));
        return;
    }

    gtk_label_set_text(GTK_LABEL(c->hint), T("edit.fetching"));
    gtk_widget_set_visible(c->spinner, TRUE);
    gtk_widget_set_sensitive(c->fetch_btn, FALSE);

    FetchJob *j = g_new0(FetchJob, 1);
    j->url = g_strdup(url);
    /* FR-03 第 3c 条：读取设置里的外部图标服务开关（旧 GUI 路径未读取） */
    j->use_icon_service =
        bmk_settings_get_bool(c->w->core->db, BMK_SET_ICON_SERVICE, true);

    GTask *task = g_task_new(NULL, NULL, fetch_done, c);
    g_task_set_task_data(task, j, fetch_job_free);
    g_task_run_in_thread(task, fetch_thread);
    g_object_unref(task);
}

static void on_fetch_clicked(GtkButton *b, gpointer user)
{
    (void)b;
    start_fetch(user);
}

/* FR-02 第 3 条：输入 URL 后自动抓取（防抖 500ms，仅新增模式） */
static gboolean auto_fetch_cb(gpointer user)
{
    EditCtx *c = user;
    c->fetch_debounce_id = 0;
    start_fetch(c);
    return G_SOURCE_REMOVE;
}

/* URL 变化时查重提示（FR-02 第 4 条）并安排自动抓取 */
static void on_url_changed(GtkEditable *e, gpointer user)
{
    EditCtx *c = user;
    if (c->fetch_debounce_id) {
        g_source_remove(c->fetch_debounce_id);
        c->fetch_debounce_id = 0;
    }
    const char *url = gtk_editable_get_text(e);
    if (!url || !*url) { gtk_label_set_text(GTK_LABEL(c->hint), ""); return; }

    char *key = NULL;
    if (bmk_url_normalize(url, &key) != BMK_OK) {
        gtk_label_set_text(GTK_LABEL(c->hint), T("edit.invalid_url"));
        return;
    }

    char *dup_title = NULL;
    int64_t n = bmk_bookmark_count_by_key(c->w->core->db, key, &dup_title);
    free(key);

    if (n > 0 && c->id <= 0) {
        char *msg = g_strdup_printf("%s%s", T("edit.dup_warn"),
                                    dup_title ? dup_title : T("app.untitled"));
        gtk_label_set_text(GTK_LABEL(c->hint), msg);
        g_free(msg);
    } else {
        gtk_label_set_text(GTK_LABEL(c->hint), "");
    }
    free(dup_title);

    /* FR-02 第 3 条：新增模式下 URL 合法，防抖后自动抓取标题与图标 */
    if (c->id <= 0 && bmk_url_is_valid_input(url, NULL))
        c->fetch_debounce_id = g_timeout_add(500, auto_fetch_cb, c);
}

static void set_tags_from_entry(bmk_db *db, gint64 id, const char *text)
{
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
    bmk_bookmark_set_tags(db, id, (const char *const *)names->pdata, names->len);
    g_ptr_array_free(names, TRUE);
}

static void on_edit_response(GtkDialog *d, int response, gpointer user)
{
    EditCtx *c = user;

    if (response != GTK_RESPONSE_ACCEPT) {
        gtk_window_destroy(GTK_WINDOW(d));
        edit_ctx_free(c);
        return;
    }

    const char *url   = gtk_editable_get_text(GTK_EDITABLE(c->url_entry));
    const char *title = gtk_editable_get_text(GTK_EDITABLE(c->title_entry));
    const char *tags  = gtk_editable_get_text(GTK_EDITABLE(c->tags_entry));

    if (!url || !*url) {
        gtk_label_set_text(GTK_LABEL(c->hint), T("edit.url_required"));
        return;   /* 保持对话框打开 */
    }
    const char *reason = NULL;
    if (!bmk_url_is_valid_input(url, &reason)) {
        gtk_label_set_text(GTK_LABEL(c->hint), T("edit.invalid_url"));
        return;
    }

    guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(c->cat_combo));
    gint64 cat = (sel != GTK_INVALID_LIST_POSITION && sel < c->cat_ids->len)
               ? g_array_index(c->cat_ids, gint64, sel) : 0;

    bmk_err e;
    gint64  id = c->id;

    if (id > 0) {
        bmk_bookmark *b = NULL;
        if (bmk_bookmark_get(c->w->core->db, id, &b) != BMK_OK || !b) {
            bmk_error_dialog(c->w->window, "%s", T("msg.error"));
            gtk_window_destroy(GTK_WINDOW(d));
            edit_ctx_free(c);
            return;
        }
        free(b->url);   b->url   = bmk_strdup(url);
        /* FR-02 第 5 条：标题兜底用 host 而非完整 URL */
        char *ehost = bmk_url_host(url);
        free(b->title); b->title = bmk_strdup(title && *title ? title
                                                : (ehost && *ehost ? ehost : url));
        free(ehost);
        b->category_id = cat;
        /* URL 改动需重算 url_key（FR-05 第 3 条） */
        free(b->url_key); b->url_key = NULL;
        bmk_url_normalize(url, &b->url_key);
        if (c->icon && c->icon_len) {
            free(b->icon);      b->icon = NULL;
            free(b->icon_mime); b->icon_mime = NULL;
            b->icon = (unsigned char *)malloc(c->icon_len);
            if (b->icon) {
                memcpy(b->icon, c->icon, c->icon_len);
                b->icon_len  = c->icon_len;
                b->icon_mime = bmk_strdup(c->icon_mime);
            }
        }
        e = bmk_bookmark_update(c->w->core->db, b);
        bmk_bookmark_free(b);
        free(b);
    } else {
        bmk_bookmark b;
        memset(&b, 0, sizeof b);
        b.url   = bmk_strdup(url);
        /* FR-02 第 5 条：标题兜底用 host 而非完整 URL */
        char *nhost = bmk_url_host(url);
        b.title = bmk_strdup(title && *title ? title
                                             : (nhost && *nhost ? nhost : url));
        free(nhost);
        b.category_id = cat;
        b.valid = BMK_VALID_UNKNOWN;
        b.status_code = -1;
        bmk_url_normalize(url, &b.url_key);
        if (c->icon && c->icon_len) {
            b.icon = (unsigned char *)malloc(c->icon_len);
            if (b.icon) {
                memcpy(b.icon, c->icon, c->icon_len);
                b.icon_len  = c->icon_len;
                b.icon_mime = bmk_strdup(c->icon_mime);
            }
        }
        e = bmk_bookmark_insert(c->w->core->db, &b);
        id = b.id;
        bmk_bookmark_free(&b);
    }

    if (e != BMK_OK) {
        bmk_error_dialog(c->w->window, "%s (%s)", T("msg.error"), bmk_err_str(e));
    } else {
        set_tags_from_entry(c->w->core->db, id, tags);
        bmk_toast(c->w, "%s", T("msg.saved"));
        bmk_window_refresh_all(c->w);
    }
    gtk_window_destroy(GTK_WINDOW(d));
    edit_ctx_free(c);
}

static GtkWidget *form_row(GtkWidget *grid, int row, const char *label, GtkWidget *ctl)
{
    GtkWidget *l = gtk_label_new(label);
    gtk_widget_set_halign(l, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), l, 0, row, 1, 1);
    gtk_widget_set_hexpand(ctl, TRUE);
    gtk_grid_attach(GTK_GRID(grid), ctl, 1, row, 1, 1);
    return ctl;
}

void bmk_dialog_edit(BmkWindow *w, gint64 bookmark_id)
{
    EditCtx *c = g_new0(EditCtx, 1);
    c->w  = w;
    c->id = bookmark_id;
    c->cat_ids = g_array_new(FALSE, FALSE, sizeof(gint64));

    GtkWidget *d = gtk_dialog_new_with_buttons(
        bookmark_id > 0 ? T("edit.edit_title") : T("edit.add_title"),
        w->window, GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        T("btn.cancel"), GTK_RESPONSE_CANCEL,
        T("btn.save"),   GTK_RESPONSE_ACCEPT,
        NULL);
    c->dlg = GTK_WINDOW(d);
    gtk_window_set_default_size(GTK_WINDOW(d), 560, -1);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_widget_set_margin_top(grid, 16);
    gtk_widget_set_margin_bottom(grid, 16);
    gtk_widget_set_margin_start(grid, 16);
    gtk_widget_set_margin_end(grid, 16);

    c->url_entry   = form_row(grid, 0, T("edit.url"),      gtk_entry_new());
    c->title_entry = form_row(grid, 1, T("edit.title"),    gtk_entry_new());

    GtkStringList *cats = gtk_string_list_new(NULL);
    bmk_ui_fill_category_combo(w->core->db, cats, c->cat_ids, 0);
    c->cat_combo = form_row(grid, 2, T("edit.category"),
                            gtk_drop_down_new(G_LIST_MODEL(cats), NULL));

    c->tags_entry  = form_row(grid, 3, T("edit.tags"),     gtk_entry_new());

    /* 抓取按钮 + 转轮 */
    GtkWidget *fetch_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    c->fetch_btn = gtk_button_new_with_label(T("edit.fetch"));
    c->spinner   = gtk_spinner_new();
    gtk_spinner_start(GTK_SPINNER(c->spinner));
    gtk_widget_set_visible(c->spinner, FALSE);
    gtk_box_append(GTK_BOX(fetch_box), c->fetch_btn);
    gtk_box_append(GTK_BOX(fetch_box), c->spinner);
    gtk_grid_attach(GTK_GRID(grid), fetch_box, 1, 4, 1, 1);
    g_signal_connect(c->fetch_btn, "clicked", G_CALLBACK(on_fetch_clicked), c);

    c->hint = gtk_label_new("");
    gtk_widget_set_halign(c->hint, GTK_ALIGN_START);
    gtk_widget_add_css_class(c->hint, "dim-label");
    gtk_label_set_wrap(GTK_LABEL(c->hint), TRUE);
    gtk_grid_attach(GTK_GRID(grid), c->hint, 0, 5, 2, 1);

    g_signal_connect(c->url_entry, "changed", G_CALLBACK(on_url_changed), c);

    /* 载入既有数据 */
    if (bookmark_id > 0) {
        bmk_bookmark *b = NULL;
        if (bmk_bookmark_get(w->core->db, bookmark_id, &b) == BMK_OK && b) {
            gtk_editable_set_text(GTK_EDITABLE(c->url_entry), b->url ? b->url : "");
            gtk_editable_set_text(GTK_EDITABLE(c->title_entry), b->title ? b->title : "");

            GString *tg = g_string_new(NULL);
            for (size_t i = 0; i < b->tag_count; i++) {
                if (i) g_string_append(tg, ", ");
                g_string_append(tg, b->tags[i]);
            }
            gtk_editable_set_text(GTK_EDITABLE(c->tags_entry), tg->str);
            g_string_free(tg, TRUE);

            for (guint i = 0; i < c->cat_ids->len; i++)
                if (g_array_index(c->cat_ids, gint64, i) == b->category_id) {
                    gtk_drop_down_set_selected(GTK_DROP_DOWN(c->cat_combo), i);
                    break;
                }
            bmk_bookmark_free(b);
            free(b);
        }
    } else if (w->selected_category > 0) {
        for (guint i = 0; i < c->cat_ids->len; i++)
            if (g_array_index(c->cat_ids, gint64, i) == w->selected_category) {
                gtk_drop_down_set_selected(GTK_DROP_DOWN(c->cat_combo), i);
                break;
            }
    }

    gtk_box_append(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(d))), grid);
    g_signal_connect(d, "response", G_CALLBACK(on_edit_response), c);
    gtk_widget_set_visible(d, TRUE);
}

/* ================================================================== */
/* 分类对话框（FR-10）                                                  */
/* ================================================================== */

typedef struct {
    BmkWindow *w;
    gint64     cat_id;       /* 0 = 新建 */
    int        mode;         /* 0=新建 1=重命名 2=移动 */
    GtkWidget *name_entry;
    GtkWidget *parent_combo;
    GtkWidget *hint;
    GArray    *ids;
} CatCtx;

static void cat_ctx_free(CatCtx *c)
{
    if (!c) return;
    if (c->ids) g_array_free(c->ids, TRUE);
    g_free(c);
}

static void on_cat_response(GtkDialog *d, int response, gpointer user)
{
    CatCtx *c = user;
    if (response != GTK_RESPONSE_ACCEPT) {
        gtk_window_destroy(GTK_WINDOW(d));
        cat_ctx_free(c);
        return;
    }

    bmk_err e = BMK_OK;

    if (c->mode == 2) {                       /* 移动 */
        guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(c->parent_combo));
        gint64 parent = (sel != GTK_INVALID_LIST_POSITION && sel < c->ids->len)
                      ? g_array_index(c->ids, gint64, sel) : 0;
        e = bmk_category_move(c->w->core->db, c->cat_id, parent);
    } else {
        const char *name = gtk_editable_get_text(GTK_EDITABLE(c->name_entry));
        if (!name || !*name) {
            gtk_label_set_text(GTK_LABEL(c->hint), T("cat.name"));
            return;
        }
        if (c->mode == 1) {
            e = bmk_category_rename(c->w->core->db, c->cat_id, name);
        } else {
            guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(c->parent_combo));
            gint64 parent = (sel != GTK_INVALID_LIST_POSITION && sel < c->ids->len)
                          ? g_array_index(c->ids, gint64, sel) : 0;
            gint64 id = 0;
            e = bmk_category_create(c->w->core->db, name, parent, &id);
        }
    }

    if (e == BMK_ERR_CONFLICT) {
        gtk_label_set_text(GTK_LABEL(c->hint), T("cat.conflict"));
        return;                               /* 冲突时保持对话框，方便改名 */
    }
    if (e == BMK_ERR_INVALID_ARG && c->mode == 2) {
        gtk_label_set_text(GTK_LABEL(c->hint), T("cat.cycle"));
        return;
    }
    if (e != BMK_OK) {
        bmk_error_dialog(c->w->window, "%s (%s)", T("msg.error"), bmk_err_str(e));
    } else {
        bmk_window_refresh_all(c->w);
    }
    gtk_window_destroy(GTK_WINDOW(d));
    cat_ctx_free(c);
}

static void category_dialog(BmkWindow *w, int mode, gint64 cat_id, gint64 default_parent)
{
    CatCtx *c = g_new0(CatCtx, 1);
    c->w = w;
    c->mode = mode;
    c->cat_id = cat_id;
    c->ids = g_array_new(FALSE, FALSE, sizeof(gint64));

    const char *title = (mode == 0) ? T("cat.new_title")
                      : (mode == 1) ? T("cat.rename_title")
                                    : T("cat.move_title");

    GtkWidget *d = gtk_dialog_new_with_buttons(
        title, w->window, GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        T("btn.cancel"), GTK_RESPONSE_CANCEL,
        T("btn.ok"),     GTK_RESPONSE_ACCEPT,
        NULL);
    gtk_window_set_default_size(GTK_WINDOW(d), 420, -1);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_widget_set_margin_top(grid, 16);
    gtk_widget_set_margin_bottom(grid, 16);
    gtk_widget_set_margin_start(grid, 16);
    gtk_widget_set_margin_end(grid, 16);

    int row = 0;
    if (mode != 2) {
        c->name_entry = form_row(grid, row++, T("cat.name"), gtk_entry_new());
        if (mode == 1) {
            bmk_category **cats = NULL;
            size_t n = 0;
            if (bmk_category_list(w->core->db, &cats, &n) == BMK_OK) {
                for (size_t i = 0; i < n; i++)
                    if (cats[i]->id == cat_id) {
                        gtk_editable_set_text(GTK_EDITABLE(c->name_entry), cats[i]->name);
                        break;
                    }
                bmk_category_list_free(cats, n);
            }
        }
    }

    if (mode != 1) {
        GtkStringList *list = gtk_string_list_new(NULL);
        /* 移动时排除自身子树，从根上杜绝成环（FR-10 第 2 条） */
        bmk_ui_fill_category_combo(w->core->db, list, c->ids,
                                   mode == 2 ? cat_id : 0);
        c->parent_combo = form_row(grid, row++, T("cat.parent"),
                                   gtk_drop_down_new(G_LIST_MODEL(list), NULL));

        gint64 want = (mode == 2) ? 0 : default_parent;
        for (guint i = 0; i < c->ids->len; i++)
            if (g_array_index(c->ids, gint64, i) == want) {
                gtk_drop_down_set_selected(GTK_DROP_DOWN(c->parent_combo), i);
                break;
            }
    }

    c->hint = gtk_label_new("");
    gtk_widget_set_halign(c->hint, GTK_ALIGN_START);
    gtk_widget_add_css_class(c->hint, "error");
    gtk_grid_attach(GTK_GRID(grid), c->hint, 0, row, 2, 1);

    gtk_box_append(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(d))), grid);
    g_signal_connect(d, "response", G_CALLBACK(on_cat_response), c);
    gtk_widget_set_visible(d, TRUE);
}

void bmk_dialog_category_new(BmkWindow *w, gint64 parent_id)
{ category_dialog(w, 0, 0, parent_id); }
void bmk_dialog_category_rename(BmkWindow *w, gint64 category_id)
{ category_dialog(w, 1, category_id, 0); }
void bmk_dialog_category_move(BmkWindow *w, gint64 category_id)
{ category_dialog(w, 2, category_id, 0); }

/* ================================================================== */
/* 标签管理（FR-11）                                                    */
/* ================================================================== */

enum { TAGCOL_NAME, TAGCOL_USAGE, TAGCOL_ID, N_TAGCOLS };

typedef struct {
    BmkWindow    *w;
    GtkWidget    *view;
    GtkListStore *store;
} TagCtx;

static void tag_reload(TagCtx *c)
{
    gtk_list_store_clear(c->store);

    bmk_tag **tags = NULL;
    size_t n = 0;
    if (bmk_tag_list(c->w->core->db, &tags, &n) != BMK_OK) return;

    for (size_t i = 0; i < n; i++) {
        GtkTreeIter it;
        gtk_list_store_append(c->store, &it);
        gtk_list_store_set(c->store, &it,
                           TAGCOL_NAME,  tags[i]->name,
                           TAGCOL_USAGE, (gint)tags[i]->usage_count,
                           TAGCOL_ID,    (gint64)tags[i]->id, -1);
    }
    bmk_tag_list_free(tags, n);
}

static gint64 tag_selected_id(TagCtx *c)
{
    GtkTreeModel *m = NULL;
    GtkTreeIter it;
    GtkTreeSelection *s = gtk_tree_view_get_selection(GTK_TREE_VIEW(c->view));
    if (!gtk_tree_selection_get_selected(s, &m, &it)) return 0;
    gint64 id = 0;
    gtk_tree_model_get(m, &it, TAGCOL_ID, &id, -1);
    return id;
}

static void on_tag_cell_edited(GtkCellRendererText *cr, char *path, char *text,
                               gpointer user)
{
    (void)cr;
    TagCtx *c = user;
    GtkTreeIter it;
    if (!gtk_tree_model_get_iter_from_string(GTK_TREE_MODEL(c->store), &it, path))
        return;
    gint64 id = 0;
    gtk_tree_model_get(GTK_TREE_MODEL(c->store), &it, TAGCOL_ID, &id, -1);

    bmk_err e = bmk_tag_rename(c->w->core->db, id, text);
    if (e == BMK_ERR_CONFLICT) {
        /* FR-11 第 4 条：重名即合并 */
        bmk_tag **tags = NULL;
        size_t n = 0;
        if (bmk_tag_list(c->w->core->db, &tags, &n) == BMK_OK) {
            for (size_t i = 0; i < n; i++)
                if (bmk_strcasecmp(tags[i]->name, text) == 0 && tags[i]->id != id) {
                    bmk_tag_merge(c->w->core->db, id, tags[i]->id);
                    break;
                }
            bmk_tag_list_free(tags, n);
        }
    } else if (e != BMK_OK) {
        bmk_error_dialog(c->w->window, "%s (%s)", T("msg.error"), bmk_err_str(e));
    }
    tag_reload(c);
    bmk_window_refresh_list(c->w);
}

typedef struct { TagCtx *c; gint64 id; } TagDelCtx;

static void do_tag_delete(gpointer user)
{
    TagDelCtx *t = user;
    bmk_tag_delete(t->c->w->core->db, t->id);
    tag_reload(t->c);
    bmk_window_refresh_list(t->c->w);
    g_free(t);
}

static void on_tag_delete(GtkButton *b, gpointer user)
{
    (void)b;
    TagCtx *c = user;
    gint64 id = tag_selected_id(c);
    if (id <= 0) return;

    TagDelCtx *t = g_new0(TagDelCtx, 1);
    t->c = c; t->id = id;
    bmk_confirm_dialog(c->w->window, T("tag.delete_confirm"), NULL,
                       T("tag.delete"), do_tag_delete, t);
}

static void on_tags_dialog_response(GtkDialog *d, int response, gpointer user)
{
    (void)response;
    g_free(user);
    gtk_window_destroy(GTK_WINDOW(d));
}

void bmk_dialog_tags(BmkWindow *w)
{
    TagCtx *c = g_new0(TagCtx, 1);
    c->w = w;

    GtkWidget *d = gtk_dialog_new_with_buttons(
        T("tag.title"), w->window, GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        T("btn.close"), GTK_RESPONSE_CLOSE, NULL);
    gtk_window_set_default_size(GTK_WINDOW(d), 520, 480);

    c->store = gtk_list_store_new(N_TAGCOLS, G_TYPE_STRING, G_TYPE_INT, G_TYPE_INT64);
    c->view  = gtk_tree_view_new_with_model(GTK_TREE_MODEL(c->store));

    GtkCellRenderer *name_cr = gtk_cell_renderer_text_new();
    g_object_set(name_cr, "editable", TRUE, NULL);
    g_signal_connect(name_cr, "edited", G_CALLBACK(on_tag_cell_edited), c);
    gtk_tree_view_append_column(
        GTK_TREE_VIEW(c->view),
        gtk_tree_view_column_new_with_attributes(T("tag.name"), name_cr,
                                                 "text", TAGCOL_NAME, NULL));
    gtk_tree_view_append_column(
        GTK_TREE_VIEW(c->view),
        gtk_tree_view_column_new_with_attributes(T("tag.usage"),
                                                 gtk_cell_renderer_text_new(),
                                                 "text", TAGCOL_USAGE, NULL));

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), c->view);
    gtk_widget_set_vexpand(scroll, TRUE);

    GtkWidget *btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *del = gtk_button_new_with_label(T("tag.delete"));
    g_signal_connect(del, "clicked", G_CALLBACK(on_tag_delete), c);
    gtk_box_append(GTK_BOX(btns), del);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top(box, 12);
    gtk_widget_set_margin_bottom(box, 12);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);
    gtk_box_append(GTK_BOX(box), scroll);
    gtk_box_append(GTK_BOX(box), btns);

    gtk_box_append(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(d))), box);
    tag_reload(c);

    g_signal_connect(d, "response", G_CALLBACK(on_tags_dialog_response), c);
    gtk_widget_set_visible(d, TRUE);
}
