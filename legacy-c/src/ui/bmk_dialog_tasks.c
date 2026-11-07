/*
 * bmk_dialog_tasks.c - 长任务对话框：导入、导出、校验、查重合并
 * （FR-07 / FR-08 / FR-14 / FR-15 / FR-16）
 *
 * 所有耗时操作在后台线程执行，进度通过 g_idle_add 回主线程刷新；
 * 每个任务都提供取消按钮（FR-20 第 7 条）。
 */
#include "bmk_ui.h"

#include "bmk_netscape.h"
#include "bmk_validate.h"
#include "bmk_backup.h"
#include "bmk_settings.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* 通用进度对话框 */
/* ------------------------------------------------------------------ */

typedef struct {
    GtkWidget *dialog;
    GtkWidget *bar;
    GtkWidget *label;
    GtkWidget *detail;
    gboolean   canceled;      /* 由主线程写，后台线程读 */
} ProgressUI;

static void on_progress_response(GtkDialog *d, int response, gpointer user)
{
    (void)d; (void)response;
    ProgressUI *p = user;
    p->canceled = TRUE;       /* 只置位；实际收尾在后台线程返回后 */
}

static ProgressUI *progress_open(GtkWindow *parent, const char *title,
                                 const char *message)
{
    ProgressUI *p = g_new0(ProgressUI, 1);

    p->dialog = gtk_dialog_new_with_buttons(
        title, parent, GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        T("btn.cancel"), GTK_RESPONSE_CANCEL, NULL);
    gtk_window_set_default_size(GTK_WINDOW(p->dialog), 460, -1);
    gtk_window_set_deletable(GTK_WINDOW(p->dialog), FALSE);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top(box, 16);
    gtk_widget_set_margin_bottom(box, 16);
    gtk_widget_set_margin_start(box, 16);
    gtk_widget_set_margin_end(box, 16);

    p->label = gtk_label_new(message);
    gtk_widget_set_halign(p->label, GTK_ALIGN_START);

    p->bar = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(p->bar), TRUE);

    p->detail = gtk_label_new("");
    gtk_widget_set_halign(p->detail, GTK_ALIGN_START);
    gtk_widget_add_css_class(p->detail, "dim-label");
    gtk_label_set_ellipsize(GTK_LABEL(p->detail), PANGO_ELLIPSIZE_MIDDLE);

    gtk_box_append(GTK_BOX(box), p->label);
    gtk_box_append(GTK_BOX(box), p->bar);
    gtk_box_append(GTK_BOX(box), p->detail);
    gtk_box_append(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(p->dialog))), box);

    g_signal_connect(p->dialog, "response", G_CALLBACK(on_progress_response), p);
    gtk_widget_set_visible(p->dialog, TRUE);
    return p;
}

static void progress_close(ProgressUI *p)
{
    if (!p) return;
    gtk_window_destroy(GTK_WINDOW(p->dialog));
    g_free(p);
}

/* 后台线程 -> 主线程的进度更新 */
typedef struct {
    ProgressUI *ui;
    gint64      done, total;
    char       *detail;
} ProgressMsg;

static gboolean progress_apply(gpointer user)
{
    ProgressMsg *m = user;
    double frac = (m->total > 0) ? (double)m->done / (double)m->total : 0.0;
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(m->ui->bar), frac);

    char *txt = g_strdup_printf("%lld / %lld", (long long)m->done, (long long)m->total);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(m->ui->bar), txt);
    g_free(txt);

    if (m->detail) gtk_label_set_text(GTK_LABEL(m->ui->detail), m->detail);

    g_free(m->detail);
    g_free(m);
    return G_SOURCE_REMOVE;
}

static void progress_post(ProgressUI *ui, gint64 done, gint64 total,
                          const char *detail)
{
    ProgressMsg *m = g_new0(ProgressMsg, 1);
    m->ui = ui;
    m->done = done;
    m->total = total;
    m->detail = detail ? g_strdup(detail) : NULL;
    g_idle_add(progress_apply, m);
}

/* ================================================================== */
/* 导入（FR-07） */
/* ================================================================== */

typedef struct {
    BmkWindow        *w;
    ProgressUI       *ui;
    char             *path;
    bmk_import_opts   opts;
    bmk_import_report report;
    bmk_err           err;
} ImportJob;

static bool import_progress_cb(int64_t done, int64_t total, void *user)
{
    ImportJob *j = user;
    progress_post(j->ui, done, total, NULL);
    return !j->ui->canceled;   /* 返回 false 触发整体回滚 */
}

static void import_thread(GTask *task, gpointer src, gpointer data, GCancellable *c)
{
    (void)src; (void)c;
    ImportJob *j = data;
    j->err = bmk_import_html_file(j->w->core->db, j->path, &j->opts, &j->report);
    g_task_return_boolean(task, TRUE);
}

static void import_done(GObject *src, GAsyncResult *r, gpointer user)
{
    (void)src; (void)user;
    ImportJob *j = g_task_get_task_data(G_TASK(r));
    BmkWindow *w = j->w;

    progress_close(j->ui);

    if (j->err == BMK_ERR_CANCELED) {
        bmk_toast(w, "%s", T("msg.canceled"));
    } else if (j->err != BMK_OK) {
        bmk_error_dialog(w->window, "%s (%s)", T("msg.error"), bmk_err_str(j->err));
    } else {
        char *msg = g_strdup_printf(T("import.summary"),
                                    (long long)j->report.added,
                                    (long long)j->report.skipped,
                                    (long long)j->report.failed);
        bmk_toast(w, "%s: %s", T("import.done"), msg);
        g_free(msg);
    }
    bmk_window_refresh_all(w);
}

static void import_job_free(gpointer p)
{
    ImportJob *j = p;
    bmk_import_report_free(&j->report);
    g_free(j->path);
    g_free(j);
}

typedef struct {
    BmkWindow *w;
    GtkWidget *file_label;
    GtkWidget *enc_combo;
    GtkWidget *cat_combo;
    GtkWidget *dup_combo;
    GArray    *cat_ids;
    char      *path;
} ImportCtx;

static void import_ctx_free(ImportCtx *c)
{
    if (!c) return;
    if (c->cat_ids) g_array_free(c->cat_ids, TRUE);
    g_free(c->path);
    g_free(c);
}

static void on_import_file_chosen(GObject *src, GAsyncResult *r, gpointer user)
{
    ImportCtx *c = user;
    GFile *f = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), r, NULL);
    if (!f) return;

    g_free(c->path);
    c->path = g_file_get_path(f);
    gtk_label_set_text(GTK_LABEL(c->file_label), c->path ? c->path : "");
    g_object_unref(f);
}

static void on_import_browse(GtkButton *b, gpointer user)
{
    ImportCtx *c = user;
    GtkFileDialog *fd = gtk_file_dialog_new();
    gtk_file_dialog_set_title(fd, T("import.file"));

    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "HTML");
    gtk_file_filter_add_pattern(filter, "*.html");
    gtk_file_filter_add_pattern(filter, "*.htm");
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(filters, filter);
    gtk_file_dialog_set_filters(fd, G_LIST_MODEL(filters));
    g_object_unref(filters);
    g_object_unref(filter);

    gtk_file_dialog_open(fd, GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(b))),
                         NULL, on_import_file_chosen, c);
    g_object_unref(fd);
}

static void on_import_response(GtkDialog *d, int response, gpointer user)
{
    ImportCtx *c = user;
    if (response != GTK_RESPONSE_ACCEPT || !c->path) {
        gtk_window_destroy(GTK_WINDOW(d));
        import_ctx_free(c);
        return;
    }

    ImportJob *j = g_new0(ImportJob, 1);
    j->w = c->w;
    j->path = g_strdup(c->path);
    bmk_import_opts_init(&j->opts);

    switch (gtk_drop_down_get_selected(GTK_DROP_DOWN(c->enc_combo))) {
    case 1:  j->opts.encoding = BMK_ENC_UTF8; break;
    case 2:  j->opts.encoding = BMK_ENC_GBK;  break;
    default: j->opts.encoding = BMK_ENC_AUTO; break;
    }
    j->opts.dup_policy =
        gtk_drop_down_get_selected(GTK_DROP_DOWN(c->dup_combo)) == 1
        ? BMK_DUP_IMPORT : BMK_DUP_SKIP;

    guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(c->cat_combo));
    j->opts.root_category_id =
        (sel != GTK_INVALID_LIST_POSITION && sel < c->cat_ids->len)
        ? g_array_index(c->cat_ids, gint64, sel) : 0;

    j->opts.progress = import_progress_cb;
    j->opts.progress_user = j;

    gtk_window_destroy(GTK_WINDOW(d));
    BmkWindow *w = c->w;
    import_ctx_free(c);

    j->ui = progress_open(w->window, T("import.title"), T("import.running"));

    GTask *task = g_task_new(NULL, NULL, import_done, NULL);
    g_task_set_task_data(task, j, import_job_free);
    g_task_run_in_thread(task, import_thread);
    g_object_unref(task);
}

void bmk_dialog_import(BmkWindow *w)
{
    ImportCtx *c = g_new0(ImportCtx, 1);
    c->w = w;
    c->cat_ids = g_array_new(FALSE, FALSE, sizeof(gint64));

    GtkWidget *d = gtk_dialog_new_with_buttons(
        T("import.title"), w->window, GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        T("btn.cancel"), GTK_RESPONSE_CANCEL,
        T("tb.import"),  GTK_RESPONSE_ACCEPT, NULL);
    gtk_window_set_default_size(GTK_WINDOW(d), 560, -1);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_widget_set_margin_top(grid, 16);
    gtk_widget_set_margin_bottom(grid, 16);
    gtk_widget_set_margin_start(grid, 16);
    gtk_widget_set_margin_end(grid, 16);

    /* 文件选择 */
    GtkWidget *fbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    c->file_label = gtk_label_new("");
    gtk_widget_set_hexpand(c->file_label, TRUE);
    gtk_widget_set_halign(c->file_label, GTK_ALIGN_START);
    gtk_label_set_ellipsize(GTK_LABEL(c->file_label), PANGO_ELLIPSIZE_MIDDLE);
    GtkWidget *browse = gtk_button_new_with_label(T("btn.browse"));
    g_signal_connect(browse, "clicked", G_CALLBACK(on_import_browse), c);
    gtk_box_append(GTK_BOX(fbox), c->file_label);
    gtk_box_append(GTK_BOX(fbox), browse);

    gtk_grid_attach(GTK_GRID(grid), gtk_label_new(T("import.file")), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), fbox, 1, 0, 1, 1);

    /* 编码 */
    const char *encs[] = { NULL, "UTF-8", "GBK", NULL };
    encs[0] = T("import.enc_auto");
    c->enc_combo = gtk_drop_down_new_from_strings(encs);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new(T("import.encoding")), 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), c->enc_combo, 1, 1, 1, 1);

    /* 目标分类 */
    GtkStringList *cats = gtk_string_list_new(NULL);
    bmk_ui_fill_category_combo(w->core->db, cats, c->cat_ids, 0);
    c->cat_combo = gtk_drop_down_new(G_LIST_MODEL(cats), NULL);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new(T("import.target")), 0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), c->cat_combo, 1, 2, 1, 1);

    /* 重复策略 */
    const char *dups[] = { NULL, NULL, NULL };
    dups[0] = T("import.dup_skip");
    dups[1] = T("import.dup_import");
    c->dup_combo = gtk_drop_down_new_from_strings(dups);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new(T("import.dup")), 0, 3, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), c->dup_combo, 1, 3, 1, 1);

    gtk_box_append(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(d))), grid);
    g_signal_connect(d, "response", G_CALLBACK(on_import_response), c);
    gtk_widget_set_visible(d, TRUE);
}

/* ================================================================== */
/* 导出（FR-08） */
/* ================================================================== */

typedef struct {
    BmkWindow *w;
    gboolean   filtered_only;
} ExportCtx;

static void on_export_file_chosen(GObject *src, GAsyncResult *r, gpointer user)
{
    ExportCtx *c = user;
    GFile *f = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), r, NULL);
    if (!f) { g_free(c); return; }

    char *path = g_file_get_path(f);
    g_object_unref(f);

    bmk_err e;
    if (c->filtered_only) {
        /* 仅导出当前筛选结果：先查询 id 列表 */
        bmk_bookmark **list = NULL;
        size_t n = 0;
        e = bmk_bookmark_query(c->w->core->db, &c->w->filter, c->w->sort_key,
                               c->w->sort_asc, &list, &n);
        if (e == BMK_OK) {
            e = bmk_export_html_file(c->w->core->db, path, list, n);
            bmk_bookmark_list_free(list, n);
        }
    } else {
        e = bmk_export_html_file(c->w->core->db, path, NULL, 0);
    }

    if (e != BMK_OK)
        bmk_error_dialog(c->w->window, "%s (%s)", T("msg.error"), bmk_err_str(e));
    else
        bmk_toast(c->w, "%s: %s", T("export.done"), path);

    g_free(path);
    g_free(c);
}

static void on_export_response(GtkDialog *d, int response, gpointer user)
{
    ExportCtx *ctx = user;
    if (response != GTK_RESPONSE_ACCEPT) {
        gtk_window_destroy(GTK_WINDOW(d));
        g_free(ctx);
        return;
    }
    GtkWidget *radio = g_object_get_data(G_OBJECT(d), "radio-filtered");
    ctx->filtered_only = gtk_check_button_get_active(GTK_CHECK_BUTTON(radio));
    gtk_window_destroy(GTK_WINDOW(d));

    GtkFileDialog *fd = gtk_file_dialog_new();
    gtk_file_dialog_set_title(fd, T("export.title"));
    /* FR-08 第 4 条：默认文件名带时间戳 */
    GDateTime *now = g_date_time_new_now_local();
    char *defname = g_date_time_format(now, "bookmarks_%Y%m%d_%H%M%S.html");
    g_date_time_unref(now);
    gtk_file_dialog_set_initial_name(fd, defname);
    g_free(defname);
    gtk_file_dialog_save(fd, ctx->w->window, NULL, on_export_file_chosen, ctx);
    g_object_unref(fd);
}

void bmk_dialog_export(BmkWindow *w)
{
    ExportCtx *c = g_new0(ExportCtx, 1);
    c->w = w;

    GtkWidget *d = gtk_dialog_new_with_buttons(
        T("export.title"), w->window, GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        T("btn.cancel"), GTK_RESPONSE_CANCEL,
        T("tb.export"),  GTK_RESPONSE_ACCEPT, NULL);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(box, 16);
    gtk_widget_set_margin_bottom(box, 16);
    gtk_widget_set_margin_start(box, 16);
    gtk_widget_set_margin_end(box, 16);

    GtkWidget *all = gtk_check_button_new_with_label(T("export.scope_all"));
    GtkWidget *filtered = gtk_check_button_new_with_label(T("export.scope_filtered"));
    gtk_check_button_set_group(GTK_CHECK_BUTTON(filtered), GTK_CHECK_BUTTON(all));
    gtk_check_button_set_active(GTK_CHECK_BUTTON(all), TRUE);

    gtk_box_append(GTK_BOX(box), all);
    gtk_box_append(GTK_BOX(box), filtered);
    gtk_box_append(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(d))), box);

    g_object_set_data(G_OBJECT(d), "radio-filtered", filtered);
    g_signal_connect(d, "response", G_CALLBACK(on_export_response), c);
    gtk_widget_set_visible(d, TRUE);
}

/* ================================================================== */
/* 批量校验（FR-14） */
/* ================================================================== */

typedef struct {
    BmkWindow        *w;
    ProgressUI       *ui;
    int64_t          *ids;
    size_t            n;
    bmk_validate_opts opts;
    bmk_err           err;
    gint64            good, bad, blocked;
} CheckJob;

static bool check_progress_cb(int64_t done, int64_t total, const bmk_bookmark *b,
                              const bmk_check_result *res, void *user)
{
    CheckJob *j = user;
    if (res) {
        if (res->note[0] && strcmp(res->note, BMK_NOTE_BLOCKED) == 0) j->blocked++;
        else if (res->valid == BMK_VALID_GOOD) j->good++;
        else if (res->valid == BMK_VALID_BAD)  j->bad++;
    }
    progress_post(j->ui, done, total, b ? b->url : NULL);
    return !j->ui->canceled;
}

static void check_thread(GTask *task, gpointer src, gpointer data, GCancellable *c)
{
    (void)src; (void)c;
    CheckJob *j = data;
    j->err = bmk_validate_run(j->w->core->db, j->ids, j->n, &j->opts,
                              check_progress_cb, j);
    g_task_return_boolean(task, TRUE);
}

static void check_done(GObject *src, GAsyncResult *r, gpointer user)
{
    (void)src; (void)user;
    CheckJob *j = g_task_get_task_data(G_TASK(r));
    BmkWindow *w = j->w;

    progress_close(j->ui);

    if (j->err == BMK_ERR_CANCELED) {
        /* 已完成的结果照常入库（FR-14 第 6 条） */
        bmk_toast(w, "%s", T("msg.canceled"));
    } else if (j->err != BMK_OK) {
        bmk_error_dialog(w->window, "%s (%s)", T("msg.error"), bmk_err_str(j->err));
    } else {
        char *msg = g_strdup_printf(T("check.summary"), (long long)j->n,
                                    (long long)j->good, (long long)j->bad,
                                    (long long)j->blocked);
        bmk_toast(w, "%s: %s", T("check.done"), msg);
        g_free(msg);
    }
    bmk_window_refresh_all(w);
}

static void check_job_free(gpointer p)
{
    CheckJob *j = p;
    g_free(j->ids);
    g_free(j);
}

static void start_check(BmkWindow *w, int64_t *ids, size_t n)
{
    if (n == 0) { bmk_toast(w, "%s", T("msg.no_selection")); g_free(ids); return; }

    CheckJob *j = g_new0(CheckJob, 1);
    j->w = w;
    j->ids = ids;
    j->n = n;
    bmk_validate_opts_load(w->core->db, &j->opts);

    j->ui = progress_open(w->window, T("check.title"), T("check.running"));

    GTask *task = g_task_new(NULL, NULL, check_done, NULL);
    g_task_set_task_data(task, j, check_job_free);
    g_task_run_in_thread(task, check_thread);
    g_object_unref(task);
}

/* 取当前筛选结果的 id 列表 */
static size_t ids_from_filter(BmkWindow *w, int64_t **out)
{
    bmk_bookmark **list = NULL;
    size_t n = 0;
    if (bmk_bookmark_query(w->core->db, &w->filter, w->sort_key, w->sort_asc,
                           &list, &n) != BMK_OK) {
        *out = NULL;
        return 0;
    }
    int64_t *ids = (int64_t *)g_malloc0(sizeof(int64_t) * (n ? n : 1));
    for (size_t i = 0; i < n; i++) ids[i] = list[i]->id;
    bmk_bookmark_list_free(list, n);
    *out = ids;
    return n;
}

static size_t ids_all(BmkWindow *w, int64_t **out)
{
    bmk_filter f;
    bmk_filter_init(&f);
    bmk_bookmark **list = NULL;
    size_t n = 0;
    bmk_err e = bmk_bookmark_query(w->core->db, &f, BMK_SORT_CREATED_AT, true,
                                   &list, &n);
    bmk_filter_free(&f);
    if (e != BMK_OK) { *out = NULL; return 0; }

    int64_t *ids = (int64_t *)g_malloc0(sizeof(int64_t) * (n ? n : 1));
    for (size_t i = 0; i < n; i++) ids[i] = list[i]->id;
    bmk_bookmark_list_free(list, n);
    *out = ids;
    return n;
}

typedef struct {
    BmkWindow *w;
    GtkWidget *scope_selected;
    GtkWidget *scope_filtered;
    gint64    *sel_ids;
    gsize      sel_n;
} CheckCtx;

static void on_check_scope_response(GtkDialog *d, int response, gpointer user)
{
    CheckCtx *c = user;
    if (response != GTK_RESPONSE_ACCEPT) {
        gtk_window_destroy(GTK_WINDOW(d));
        g_free(c->sel_ids);
        g_free(c);
        return;
    }

    int64_t *ids = NULL;
    size_t   n = 0;

    if (gtk_check_button_get_active(GTK_CHECK_BUTTON(c->scope_selected))) {
        n = c->sel_n;
        ids = (int64_t *)g_malloc0(sizeof(int64_t) * (n ? n : 1));
        for (gsize i = 0; i < n; i++) ids[i] = c->sel_ids[i];
    } else if (gtk_check_button_get_active(GTK_CHECK_BUTTON(c->scope_filtered))) {
        n = ids_from_filter(c->w, &ids);
    } else {
        n = ids_all(c->w, &ids);
    }

    BmkWindow *w = c->w;
    gtk_window_destroy(GTK_WINDOW(d));
    g_free(c->sel_ids);
    g_free(c);

    start_check(w, ids, n);
}

void bmk_dialog_check(BmkWindow *w, gint64 *ids, gsize n)
{
    CheckCtx *c = g_new0(CheckCtx, 1);
    c->w = w;
    c->sel_n = n;
    if (n) {
        c->sel_ids = g_malloc0(sizeof(gint64) * n);
        memcpy(c->sel_ids, ids, sizeof(gint64) * n);
    }

    GtkWidget *d = gtk_dialog_new_with_buttons(
        T("check.title"), w->window, GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        T("btn.cancel"), GTK_RESPONSE_CANCEL,
        T("tb.check"),   GTK_RESPONSE_ACCEPT, NULL);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(box, 16);
    gtk_widget_set_margin_bottom(box, 16);
    gtk_widget_set_margin_start(box, 16);
    gtk_widget_set_margin_end(box, 16);

    gtk_box_append(GTK_BOX(box), gtk_label_new(T("check.scope")));

    c->scope_selected = gtk_check_button_new_with_label(T("check.scope_selected"));
    c->scope_filtered = gtk_check_button_new_with_label(T("check.scope_filtered"));
    GtkWidget *scope_all = gtk_check_button_new_with_label(T("check.scope_all"));
    gtk_check_button_set_group(GTK_CHECK_BUTTON(c->scope_filtered),
                               GTK_CHECK_BUTTON(c->scope_selected));
    gtk_check_button_set_group(GTK_CHECK_BUTTON(scope_all),
                               GTK_CHECK_BUTTON(c->scope_selected));

    if (n) gtk_check_button_set_active(GTK_CHECK_BUTTON(c->scope_selected), TRUE);
    else {
        gtk_widget_set_sensitive(c->scope_selected, FALSE);
        gtk_check_button_set_active(GTK_CHECK_BUTTON(c->scope_filtered), TRUE);
    }

    gtk_box_append(GTK_BOX(box), c->scope_selected);
    gtk_box_append(GTK_BOX(box), c->scope_filtered);
    gtk_box_append(GTK_BOX(box), scope_all);
    gtk_box_append(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(d))), box);

    g_signal_connect(d, "response", G_CALLBACK(on_check_scope_response), c);
    gtk_widget_set_visible(d, TRUE);
}

/* ================================================================== */
/* 查重与合并（FR-15 / FR-16） */
/* ================================================================== */

typedef struct {
    BmkWindow      *w;
    GtkWidget      *dialog;
    GtkWidget      *box;         /* 组列表容器 */
    bmk_dup_group **groups;
    size_t          n;
} DedupeCtx;

static void dedupe_reload(DedupeCtx *c);

typedef struct { DedupeCtx *c; size_t index; gboolean all; } MergeCtx;

static void do_merge(gpointer user)
{
    MergeCtx *m = user;
    DedupeCtx *c = m->c;
    int merged = 0;

    size_t from = m->all ? 0 : m->index;
    size_t to   = m->all ? c->n : m->index + 1;

    for (size_t i = from; i < to && i < c->n; i++) {
        bmk_dup_group *g = c->groups[i];
        if (g->count < 2) continue;

        int64_t *ids = (int64_t *)g_malloc0(sizeof(int64_t) * g->count);
        for (size_t k = 0; k < g->count; k++) ids[k] = g->members[k]->id;

        int64_t primary = 0;
        if (bmk_merge_group(c->w->core->db, ids, g->count, &primary) == BMK_OK)
            merged++;
        g_free(ids);
    }

    bmk_toast(c->w, T("dedupe.merged"), merged);
    bmk_window_refresh_all(c->w);
    dedupe_reload(c);
    g_free(m);
}

static void on_merge_clicked(GtkButton *b, gpointer user)
{
    MergeCtx *tpl = user;
    MergeCtx *m = g_new0(MergeCtx, 1);
    *m = *tpl;
    bmk_confirm_dialog(GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(b))),
                       T("dedupe.confirm"), NULL, T("dedupe.merge"), do_merge, m);
}

static void dedupe_reload(DedupeCtx *c)
{
    /* 清空旧内容 */
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(c->box)))
        gtk_box_remove(GTK_BOX(c->box), child);

    if (c->groups) {
        bmk_dup_group_list_free(c->groups, c->n);
        c->groups = NULL;
        c->n = 0;
    }

    if (bmk_dedupe_find(c->w->core->db, &c->groups, &c->n) != BMK_OK) {
        bmk_error_dialog(c->w->window, "%s", T("msg.error"));
        return;
    }

    if (c->n == 0) {
        GtkWidget *l = gtk_label_new(T("dedupe.none"));
        gtk_widget_add_css_class(l, "dim-label");
        gtk_widget_set_margin_top(l, 40);
        gtk_box_append(GTK_BOX(c->box), l);
        return;
    }

    for (size_t i = 0; i < c->n; i++) {
        bmk_dup_group *g = c->groups[i];

        GtkWidget *frame = gtk_frame_new(NULL);
        GtkWidget *vb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_widget_set_margin_top(vb, 8);
        gtk_widget_set_margin_bottom(vb, 8);
        gtk_widget_set_margin_start(vb, 8);
        gtk_widget_set_margin_end(vb, 8);

        GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        char *title = g_strdup_printf(T("dedupe.group"), (int)(i + 1), (int)g->count);
        GtkWidget *tl = gtk_label_new(title);
        gtk_widget_add_css_class(tl, "heading");
        gtk_widget_set_hexpand(tl, TRUE);
        gtk_widget_set_halign(tl, GTK_ALIGN_START);
        g_free(title);

        GtkWidget *mb = gtk_button_new_with_label(T("dedupe.merge"));
        MergeCtx *tpl = g_new0(MergeCtx, 1);
        tpl->c = c; tpl->index = i; tpl->all = FALSE;
        g_signal_connect_data(mb, "clicked", G_CALLBACK(on_merge_clicked),
                              tpl, (GClosureNotify)g_free, 0);

        gtk_box_append(GTK_BOX(head), tl);
        gtk_box_append(GTK_BOX(head), mb);
        gtk_box_append(GTK_BOX(vb), head);

        GtkWidget *key = gtk_label_new(g->url_key);
        gtk_widget_add_css_class(key, "dim-label");
        gtk_widget_set_halign(key, GTK_ALIGN_START);
        gtk_label_set_ellipsize(GTK_LABEL(key), PANGO_ELLIPSIZE_MIDDLE);
        gtk_box_append(GTK_BOX(vb), key);

        for (size_t k = 0; k < g->count; k++) {
            bmk_bookmark *b = g->members[k];
            char *line = g_strdup_printf("#%lld  %s  -> %s",
                                         (long long)b->id,
                                         b->title ? b->title : T("app.untitled"),
                                         b->url ? b->url : "");
            GtkWidget *l = gtk_label_new(line);
            gtk_widget_set_halign(l, GTK_ALIGN_START);
            gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_MIDDLE);
            gtk_box_append(GTK_BOX(vb), l);
            g_free(line);
        }

        gtk_frame_set_child(GTK_FRAME(frame), vb);
        gtk_box_append(GTK_BOX(c->box), frame);
    }
}

static void on_dedupe_response(GtkDialog *d, int response, gpointer user)
{
    DedupeCtx *c = user;
    if (response == GTK_RESPONSE_APPLY) {
        MergeCtx *m = g_new0(MergeCtx, 1);
        m->c = c; m->all = TRUE;
        bmk_confirm_dialog(GTK_WINDOW(d), T("dedupe.confirm"), NULL,
                           T("dedupe.merge_all"), do_merge, m);
        return;
    }
    if (c->groups) bmk_dup_group_list_free(c->groups, c->n);
    g_free(c);
    gtk_window_destroy(GTK_WINDOW(d));
}

void bmk_dialog_dedupe(BmkWindow *w)
{
    DedupeCtx *c = g_new0(DedupeCtx, 1);
    c->w = w;

    GtkWidget *d = gtk_dialog_new_with_buttons(
        T("dedupe.title"), w->window, GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        T("dedupe.merge_all"), GTK_RESPONSE_APPLY,
        T("btn.close"),        GTK_RESPONSE_CLOSE, NULL);
    gtk_window_set_default_size(GTK_WINDOW(d), 720, 560);
    c->dialog = d;

    c->box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top(c->box, 12);
    gtk_widget_set_margin_bottom(c->box, 12);
    gtk_widget_set_margin_start(c->box, 12);
    gtk_widget_set_margin_end(c->box, 12);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), c->box);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(d))), scroll);

    dedupe_reload(c);

    g_signal_connect(d, "response", G_CALLBACK(on_dedupe_response), c);
    gtk_widget_set_visible(d, TRUE);
}
