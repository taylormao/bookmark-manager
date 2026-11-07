/*
 * bmk_dialog_settings.c - 设置、代理节点、云端同步对话框
 * （FR-01 / FR-03 / FR-14 / FR-17 / FR-18 / FR-19 / FR-21）
 *
 * 密钥类输入一律 visibility=false，且已保存的值不回填明文（需求 5.3 第 1 条）。
 */
#include "bmk_ui.h"

#include "bmk_settings.h"
#include "bmk_backup.h"
#include "bmk_proxy.h"
#include "bmk_sync.h"
#include "bmk_process.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* 表单辅助                                                            */
/* ------------------------------------------------------------------ */

static GtkWidget *grid_new(void)
{
    GtkWidget *g = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(g), 10);
    gtk_grid_set_column_spacing(GTK_GRID(g), 10);
    gtk_widget_set_margin_top(g, 16);
    gtk_widget_set_margin_bottom(g, 16);
    gtk_widget_set_margin_start(g, 16);
    gtk_widget_set_margin_end(g, 16);
    return g;
}

static GtkWidget *row_widget(GtkWidget *grid, int row, const char *label, GtkWidget *ctl)
{
    GtkWidget *l = gtk_label_new(label);
    gtk_widget_set_halign(l, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), l, 0, row, 1, 1);
    gtk_widget_set_hexpand(ctl, TRUE);
    gtk_grid_attach(GTK_GRID(grid), ctl, 1, row, 1, 1);
    return ctl;
}

static GtkWidget *row_entry(GtkWidget *grid, int row, const char *label,
                            const char *value)
{
    GtkWidget *e = gtk_entry_new();
    if (value) gtk_editable_set_text(GTK_EDITABLE(e), value);
    return row_widget(grid, row, label, e);
}

/* 密钥输入：不回填明文，仅提示已保存 */
static GtkWidget *row_secret(GtkWidget *grid, int row, const char *label,
                             gboolean has_value)
{
    GtkWidget *e = gtk_entry_new();
    gtk_entry_set_visibility(GTK_ENTRY(e), FALSE);
    gtk_entry_set_input_purpose(GTK_ENTRY(e), GTK_INPUT_PURPOSE_PASSWORD);
    if (has_value)
        gtk_entry_set_placeholder_text(GTK_ENTRY(e), T("sync.secret_masked"));
    return row_widget(grid, row, label, e);
}

static GtkWidget *row_spin(GtkWidget *grid, int row, const char *label,
                           double lo, double hi, double value)
{
    GtkWidget *s = gtk_spin_button_new_with_range(lo, hi, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(s), value);
    return row_widget(grid, row, label, s);
}

/* 只有输入了新值才覆盖旧密钥 */
static void save_secret_if_set(bmk_db *db, const char *key, GtkWidget *entry)
{
    const char *v = gtk_editable_get_text(GTK_EDITABLE(entry));
    if (v && *v) bmk_settings_set_str(db, key, v);
}

static gboolean has_setting(bmk_db *db, const char *key)
{
    char *v = bmk_settings_get_str(db, key, NULL);
    gboolean has = (v && *v);
    free(v);
    return has;
}

/* ================================================================== */
/* 设置对话框                                                          */
/* ================================================================== */

typedef struct {
    BmkWindow *w;

    /* 常规 */
    GtkWidget *data_dir_label;
    GtkWidget *lang_combo;
    GtkWidget *theme_combo;
    char      *new_data_dir;

    /* 网络 */
    GtkWidget *icon_service;
    GtkWidget *timeout;
    GtkWidget *concurrency;
    GtkWidget *retry;
    GtkWidget *interval;
    GtkWidget *proxy_mode;     /* direct / always / auto，真正作用于批量校验 */

    /* 直接填写的代理配置（FR-17 补充：协议 / 地址 / 端口 / 凭据 + 即时校验） */
    GtkWidget *proxy_type;
    GtkWidget *proxy_host;
    GtkWidget *proxy_port;
    GtkWidget *proxy_user;
    GtkWidget *proxy_pass;
    GtkWidget *proxy_test_url;
    GtkWidget *proxy_result;

    GtkWidget *xray_path;
    GtkWidget *git_path;
} SettingsCtx;

static void settings_ctx_free(SettingsCtx *c)
{
    if (!c) return;
    g_free(c->new_data_dir);
    g_free(c);
}

/* ================================================================== */
/* 直接填写代理配置 + 即时分层校验（FR-17 补充）                       */
/* ================================================================== */

typedef struct {
    BmkWindow *w;
    GtkWidget *result;   /* 额外持有一个 ref，防止对话框先于后台任务销毁 */
    bmk_node   cfg;
    char      *test_url;
    bmk_proxy_check chk;
    bmk_err   err;
} ProxyCfgJob;

static void collect_proxy_cfg(SettingsCtx *c, bmk_node *n, char **test_url)
{
    memset(n, 0, sizeof(*n));
    n->type = (gtk_drop_down_get_selected(GTK_DROP_DOWN(c->proxy_type)) == 1)
            ? BMK_NODE_SOCKS5 : BMK_NODE_HTTP;
    n->enabled = true;
    n->host = g_strdup(gtk_editable_get_text(GTK_EDITABLE(c->proxy_host)));
    n->port = (int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(c->proxy_port));

    const char *u = gtk_editable_get_text(GTK_EDITABLE(c->proxy_user));
    const char *p = gtk_editable_get_text(GTK_EDITABLE(c->proxy_pass));
    if (u && *u) n->username = g_strdup(u);
    if (p && *p) n->password = g_strdup(p);

    const char *t = gtk_editable_get_text(GTK_EDITABLE(c->proxy_test_url));
    *test_url = (t && *t) ? g_strdup(t) : NULL;
}

static void proxy_cfg_node_free(bmk_node *n)
{
    g_free(n->name);
    g_free(n->host);
    g_free(n->username);
    g_free(n->password);
    memset(n, 0, sizeof(*n));
}

static void proxy_cfg_job_free(gpointer data)
{
    ProxyCfgJob *j = data;
    if (!j) return;
    proxy_cfg_node_free(&j->cfg);
    g_free(j->test_url);
    if (j->result) g_object_unref(j->result);
    g_free(j);
}

static void proxy_cfg_test_done(GObject *src, GAsyncResult *r, gpointer user)
{
    (void)src; (void)user;
    ProxyCfgJob *j = g_task_get_task_data(G_TASK(r));
    static const char *kStage[] = { "TCP", "Handshake", "Target" };

    GString *s = g_string_new(NULL);
    for (int i = 0; i < BMK_PCHK_STAGES; i++) {
        const char *st = (j->chk.state[i] == BMK_PCHK_OK)   ? "OK"
                       : (j->chk.state[i] == BMK_PCHK_FAIL) ? "FAIL" : "SKIP";
        g_string_append_printf(s, "%-9s %-4s %4ldms  %s\n",
                               kStage[i], st, j->chk.ms[i], j->chk.detail[i]);
    }
    g_string_append_printf(s, "%s: %s", T("set.proxy_result"),
                           j->chk.usable ? T("set.proxy_usable")
                                         : T("set.proxy_unusable"));
    if (j->chk.usable && j->chk.state[BMK_PCHK_TARGET] == BMK_PCHK_FAIL)
        g_string_append_printf(s, " (%s)", T("set.proxy_target_only"));

    if (j->result && GTK_IS_LABEL(j->result))
        gtk_label_set_text(GTK_LABEL(j->result), s->str);
    g_string_free(s, TRUE);
    if (j->result) { g_object_unref(j->result); j->result = NULL; }
}

static void proxy_cfg_test_thread(GTask *task, gpointer src, gpointer data,
                                  GCancellable *cc)
{
    (void)src; (void)cc;
    ProxyCfgJob *j = data;
    j->err = bmk_proxy_endpoint_check(&j->cfg, j->test_url, &j->chk);
    g_task_return_boolean(task, TRUE);
}

static void on_proxy_cfg_test(GtkButton *b, gpointer user)
{
    (void)b;
    SettingsCtx *c = user;
    ProxyCfgJob *j = g_new0(ProxyCfgJob, 1);
    j->w = c->w;
    j->result = c->proxy_result;
    if (j->result) g_object_ref(j->result);
    collect_proxy_cfg(c, &j->cfg, &j->test_url);

    if (j->result)
        gtk_label_set_text(GTK_LABEL(j->result), T("set.proxy_testing"));

    GTask *task = g_task_new(NULL, NULL, proxy_cfg_test_done, NULL);
    g_task_set_task_data(task, j, proxy_cfg_job_free);
    g_task_run_in_thread(task, proxy_cfg_test_thread);
    g_object_unref(task);
}

static void on_proxy_cfg_save(GtkButton *b, gpointer user)
{
    (void)b;
    SettingsCtx *c = user;
    bmk_node n;
    char *test_url = NULL;
    collect_proxy_cfg(c, &n, &test_url);
    g_free(test_url);

    if (!n.host || !*n.host || n.port <= 0 || n.port > 65535) {
        bmk_error_dialog(c->w->window, "%s", T("set.proxy_invalid"));
        proxy_cfg_node_free(&n);
        return;
    }
    n.name = g_strdup_printf("%s %s:%d",
        n.type == BMK_NODE_SOCKS5 ? "SOCKS5" : "HTTP", n.host, n.port);

    int64_t id = 0;
    bmk_err e = bmk_proxy_node_add(&n, &id);
    if (e == BMK_OK) {
        bmk_proxy_node_start(id);  /* HTTP/SOCKS5 无子进程，直接置为可用 */
        bmk_proxy_save(c->w->core->db);
        bmk_settings_set_str(c->w->core->db, BMK_SET_PROXY_MODE, "always");
        bmk_toast(c->w, "%s", T("set.proxy_saved"));
    } else {
        bmk_error_dialog(c->w->window, "%s (%s)",
                         T("set.proxy_save_failed"), bmk_err_str(e));
    }
    proxy_cfg_node_free(&n);
}

static void on_data_dir_chosen(GObject *src, GAsyncResult *r, gpointer user)
{
    SettingsCtx *c = user;
    GFile *f = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(src), r, NULL);
    if (!f) return;

    char *path = g_file_get_path(f);
    g_object_unref(f);
    if (!path) return;

    if (!bmk_app_dir_writable(path)) {
        bmk_error_dialog(c->w->window, "%s", T("msg.dir_not_writable"));
        g_free(path);
        return;
    }
    g_free(c->new_data_dir);
    c->new_data_dir = path;
    gtk_label_set_text(GTK_LABEL(c->data_dir_label), path);
}

static void on_change_data_dir(GtkButton *b, gpointer user)
{
    SettingsCtx *c = user;
    GtkFileDialog *fd = gtk_file_dialog_new();
    gtk_file_dialog_set_title(fd, T("set.data_dir"));
    gtk_file_dialog_select_folder(fd, GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(b))),
                                  NULL, on_data_dir_chosen, c);
    g_object_unref(fd);
}

static void on_backup_now(GtkButton *b, gpointer user)
{
    (void)b;
    SettingsCtx *c = user;
    bmk_err e = bmk_backup_full(c->w->core->db, c->w->core->db_path);
    if (e != BMK_OK)
        bmk_error_dialog(c->w->window, "%s (%s)", T("msg.error"), bmk_err_str(e));
    else
        bmk_toast(c->w, "%s: %s", T("set.backup_now"), c->w->core->backups_dir);
}

static void on_backup_cleanup(GtkButton *b, gpointer user)
{
    (void)b;
    SettingsCtx *c = user;
    bmk_backup_cleanup();
    bmk_toast(c->w, "%s", T("set.backup_cleanup"));
}

static void on_open_backup_dir(GtkButton *b, gpointer user)
{
    (void)b;
    SettingsCtx *c = user;
    GFile *f = g_file_new_for_path(c->w->core->backups_dir);
    GtkFileLauncher *l = gtk_file_launcher_new(f);
    gtk_file_launcher_launch(l, c->w->window, NULL, NULL, NULL);
    g_object_unref(l);
    g_object_unref(f);
}

static void on_settings_response(GtkDialog *d, int response, gpointer user)
{
    SettingsCtx *c = user;
    if (response != GTK_RESPONSE_ACCEPT) {
        gtk_window_destroy(GTK_WINDOW(d));
        settings_ctx_free(c);
        return;
    }

    bmk_db *db = c->w->core->db;

    /* 常规 */
    bmk_settings_set_str(db, BMK_SET_LANGUAGE,
        gtk_drop_down_get_selected(GTK_DROP_DOWN(c->lang_combo)) == 1 ? "en" : "zh");
    bmk_settings_set_str(db, BMK_SET_THEME,
        gtk_drop_down_get_selected(GTK_DROP_DOWN(c->theme_combo)) == 1 ? "dark" : "light");

    /* 网络 */
    bmk_settings_set_bool(db, BMK_SET_ICON_SERVICE,
        gtk_check_button_get_active(GTK_CHECK_BUTTON(c->icon_service)));
    bmk_settings_set_int(db, BMK_SET_CHK_TIMEOUT,
        (int64_t)gtk_spin_button_get_value(GTK_SPIN_BUTTON(c->timeout)));
    bmk_settings_set_int(db, BMK_SET_CHK_CONCURRENCY,
        (int64_t)gtk_spin_button_get_value(GTK_SPIN_BUTTON(c->concurrency)));
    bmk_settings_set_int(db, BMK_SET_CHK_RETRY,
        (int64_t)gtk_spin_button_get_value(GTK_SPIN_BUTTON(c->retry)));
    bmk_settings_set_int(db, BMK_SET_CHK_INTERVAL_MS,
        (int64_t)gtk_spin_button_get_value(GTK_SPIN_BUTTON(c->interval)));
    /* 写入批量校验真正读取的 proxy_mode，而不是旧的 check_use_proxy 布尔键 */
    static const char *kProxyModes[] = { "direct", "always", "auto" };
    guint mi = gtk_drop_down_get_selected(GTK_DROP_DOWN(c->proxy_mode));
    if (mi > 2) mi = 0;
    bmk_settings_set_str(db, BMK_SET_PROXY_MODE, kProxyModes[mi]);

    const char *tu = gtk_editable_get_text(GTK_EDITABLE(c->proxy_test_url));
    if (tu && *tu) bmk_settings_set_str(db, BMK_SET_PROXY_TEST_URL, tu);

    const char *xp = gtk_editable_get_text(GTK_EDITABLE(c->xray_path));
    bmk_settings_set_str(db, BMK_SET_XRAY_PATH, xp ? xp : "");
    if (xp && *xp) bmk_proxy_set_xray_path(xp);

    const char *gp = gtk_editable_get_text(GTK_EDITABLE(c->git_path));
    bmk_settings_set_str(db, BMK_SET_GH_GIT_PATH, gp ? gp : "");

    /* 数据目录迁移放最后，失败也不影响其它设置（FR-01 第 3 条） */
    if (c->new_data_dir && strcmp(c->new_data_dir, c->w->core->data_dir) != 0) {
        bmk_err e = bmk_app_migrate_data_dir(c->w->core, c->new_data_dir);
        if (e != BMK_OK) {
            bmk_error_dialog(c->w->window, "%s (%s)", T("msg.error"), bmk_err_str(e));
        } else {
            bmk_window_refresh_all(c->w);
        }
    }

    bmk_toast(c->w, "%s", T("msg.saved"));
    gtk_window_destroy(GTK_WINDOW(d));
    settings_ctx_free(c);
}

void bmk_dialog_settings(BmkWindow *w)
{
    SettingsCtx *c = g_new0(SettingsCtx, 1);
    c->w = w;
    bmk_db *db = w->core->db;

    GtkWidget *d = gtk_dialog_new_with_buttons(
        T("set.title"), w->window, GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        T("btn.cancel"), GTK_RESPONSE_CANCEL,
        T("btn.save"),   GTK_RESPONSE_ACCEPT, NULL);
    gtk_window_set_default_size(GTK_WINDOW(d), 620, 520);

    GtkWidget *nb = gtk_notebook_new();

    /* ---- 常规 ---- */
    GtkWidget *g1 = grid_new();
    int r = 0;

    GtkWidget *dirbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    c->data_dir_label = gtk_label_new(w->core->data_dir);
    gtk_widget_set_hexpand(c->data_dir_label, TRUE);
    gtk_widget_set_halign(c->data_dir_label, GTK_ALIGN_START);
    gtk_label_set_ellipsize(GTK_LABEL(c->data_dir_label), PANGO_ELLIPSIZE_MIDDLE);
    GtkWidget *chg = gtk_button_new_with_label(T("set.change_dir"));
    g_signal_connect(chg, "clicked", G_CALLBACK(on_change_data_dir), c);
    gtk_box_append(GTK_BOX(dirbox), c->data_dir_label);
    gtk_box_append(GTK_BOX(dirbox), chg);
    row_widget(g1, r++, T("set.data_dir"), dirbox);

    const char *langs[] = { NULL, NULL, NULL };
    langs[0] = T("set.lang_zh");
    langs[1] = T("set.lang_en");
    c->lang_combo = row_widget(g1, r++, T("set.language"),
                               gtk_drop_down_new_from_strings(langs));
    {
        char *cur = bmk_settings_get_str(db, BMK_SET_LANGUAGE, "zh");
        gtk_drop_down_set_selected(GTK_DROP_DOWN(c->lang_combo),
                                   (cur && strcmp(cur, "en") == 0) ? 1 : 0);
        free(cur);
    }
    /* 主题（FR-01）：浅色/深色，默认浅色 */
    const char *themes[] = { NULL, NULL, NULL };
    themes[0] = T("set.theme_light");
    themes[1] = T("set.theme_dark");
    c->theme_combo = row_widget(g1, r++, T("set.theme"),
                                gtk_drop_down_new_from_strings(themes));
    {
        char *th = bmk_settings_get_str(db, BMK_SET_THEME, "light");
        gtk_drop_down_set_selected(GTK_DROP_DOWN(c->theme_combo),
                                   (th && strcmp(th, "dark") == 0) ? 1 : 0);
        free(th);
    }

    GtkWidget *hint = gtk_label_new(T("set.restart_hint"));
    gtk_widget_add_css_class(hint, "dim-label");
    gtk_widget_set_halign(hint, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(g1), hint, 1, r++, 1, 1);

    gtk_notebook_append_page(GTK_NOTEBOOK(nb), g1, gtk_label_new(T("set.tab_general")));

    /* ---- 网络 ---- */
    GtkWidget *g2 = grid_new();
    r = 0;

    c->icon_service = gtk_check_button_new_with_label(T("set.icon_service"));
    gtk_check_button_set_active(GTK_CHECK_BUTTON(c->icon_service),
                                bmk_settings_get_bool(db, BMK_SET_ICON_SERVICE, true));
    gtk_grid_attach(GTK_GRID(g2), c->icon_service, 1, r++, 1, 1);

    GtkWidget *ihint = gtk_label_new(T("set.icon_service_hint"));
    gtk_widget_add_css_class(ihint, "dim-label");
    gtk_widget_set_halign(ihint, GTK_ALIGN_START);
    gtk_label_set_wrap(GTK_LABEL(ihint), TRUE);
    gtk_grid_attach(GTK_GRID(g2), ihint, 1, r++, 1, 1);

    c->timeout     = row_spin(g2, r++, T("set.timeout"), 1, 120,
                              (double)bmk_settings_get_int(db, BMK_SET_CHK_TIMEOUT, 10));
    c->concurrency = row_spin(g2, r++, T("set.concurrency"), 1, 32,
                              (double)bmk_settings_get_int(db, BMK_SET_CHK_CONCURRENCY, 8));
    c->retry       = row_spin(g2, r++, T("set.retry"), 0, 5,
                              (double)bmk_settings_get_int(db, BMK_SET_CHK_RETRY, 1));
    c->interval    = row_spin(g2, r++, T("set.interval"), 0, 10000,
                              (double)bmk_settings_get_int(db, BMK_SET_CHK_INTERVAL_MS, 500));

    const char *modes[] = { NULL, NULL, NULL, NULL };
    modes[0] = T("set.proxy_direct");
    modes[1] = T("set.proxy_always");
    modes[2] = T("set.proxy_auto");
    c->proxy_mode = row_widget(g2, r++, T("set.use_proxy"),
                               gtk_drop_down_new_from_strings(modes));
    {
        char *pm = bmk_settings_get_str(db, BMK_SET_PROXY_MODE, NULL);
        guint sel = 0;
        if (pm && strcmp(pm, "always") == 0) sel = 1;
        else if (pm && strcmp(pm, "auto") == 0) sel = 2;
        else if (!pm && bmk_settings_get_bool(db, BMK_SET_CHK_USE_PROXY, false)) sel = 1;
        gtk_drop_down_set_selected(GTK_DROP_DOWN(c->proxy_mode), sel);
        free(pm);
    }

    /* ---- 直接填协议 / IP / 端口并当场校验（FR-17 补充） ---- */
    GtkWidget *psep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_margin_top(psep, 8);
    gtk_widget_set_margin_bottom(psep, 8);
    gtk_grid_attach(GTK_GRID(g2), psep, 0, r++, 2, 1);

    GtkWidget *ptitle = gtk_label_new(T("set.proxy_section"));
    gtk_widget_set_halign(ptitle, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(g2), ptitle, 0, r++, 2, 1);

    const char *ptypes[] = { "HTTP", "SOCKS5", NULL };
    c->proxy_type = row_widget(g2, r++, T("set.proxy_type"),
                               gtk_drop_down_new_from_strings(ptypes));
    c->proxy_host = row_entry(g2, r++, T("set.proxy_host"), "");
    gtk_entry_set_placeholder_text(GTK_ENTRY(c->proxy_host), "127.0.0.1");
    c->proxy_port = row_spin(g2, r++, T("set.proxy_port"), 1, 65535, 8080);
    c->proxy_user = row_entry(g2, r++, T("set.proxy_user"), "");
    c->proxy_pass = row_secret(g2, r++, T("set.proxy_pass"), FALSE);
    {
        char *tu = bmk_settings_get_str(db, BMK_SET_PROXY_TEST_URL,
                                        "https://www.google.com/generate_204");
        c->proxy_test_url = row_entry(g2, r++, T("set.proxy_test_url"), tu);
        free(tu);
    }

    GtkWidget *pbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *tbtn = gtk_button_new_with_label(T("set.proxy_test"));
    g_signal_connect(tbtn, "clicked", G_CALLBACK(on_proxy_cfg_test), c);
    GtkWidget *sbtn = gtk_button_new_with_label(T("set.proxy_save"));
    g_signal_connect(sbtn, "clicked", G_CALLBACK(on_proxy_cfg_save), c);
    gtk_box_append(GTK_BOX(pbox), tbtn);
    gtk_box_append(GTK_BOX(pbox), sbtn);
    gtk_grid_attach(GTK_GRID(g2), pbox, 1, r++, 1, 1);

    c->proxy_result = gtk_label_new(T("set.proxy_hint"));
    gtk_label_set_wrap(GTK_LABEL(c->proxy_result), TRUE);
    gtk_label_set_selectable(GTK_LABEL(c->proxy_result), TRUE);
    gtk_widget_set_halign(c->proxy_result, GTK_ALIGN_START);
    gtk_widget_add_css_class(c->proxy_result, "dim-label");
    gtk_grid_attach(GTK_GRID(g2), c->proxy_result, 1, r++, 1, 1);

    {
        char *xp = bmk_settings_get_str(db, BMK_SET_XRAY_PATH, "");
        c->xray_path = row_entry(g2, r++, T("set.xray_path"), xp);
        free(xp);
        char *gp = bmk_settings_get_str(db, BMK_SET_GH_GIT_PATH, "");
        c->git_path = row_entry(g2, r++, T("set.git_path"), gp);
        free(gp);
    }

    gtk_notebook_append_page(GTK_NOTEBOOK(nb), g2, gtk_label_new(T("set.tab_network")));

    /* ---- 备份 ---- */
    GtkWidget *g3 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top(g3, 16);
    gtk_widget_set_margin_bottom(g3, 16);
    gtk_widget_set_margin_start(g3, 16);
    gtk_widget_set_margin_end(g3, 16);

    GtkWidget *bpath = gtk_label_new(w->core->backups_dir);
    gtk_widget_set_halign(bpath, GTK_ALIGN_START);
    gtk_widget_add_css_class(bpath, "dim-label");
    gtk_label_set_ellipsize(GTK_LABEL(bpath), PANGO_ELLIPSIZE_MIDDLE);
    gtk_box_append(GTK_BOX(g3), bpath);

    GtkWidget *bnow = gtk_button_new_with_label(T("set.backup_now"));
    g_signal_connect(bnow, "clicked", G_CALLBACK(on_backup_now), c);
    GtkWidget *bcln = gtk_button_new_with_label(T("set.backup_cleanup"));
    g_signal_connect(bcln, "clicked", G_CALLBACK(on_backup_cleanup), c);
    GtkWidget *bopen = gtk_button_new_with_label(T("set.open_backup_dir"));
    g_signal_connect(bopen, "clicked", G_CALLBACK(on_open_backup_dir), c);
    gtk_box_append(GTK_BOX(g3), bnow);
    gtk_box_append(GTK_BOX(g3), bcln);
    gtk_box_append(GTK_BOX(g3), bopen);

    gtk_notebook_append_page(GTK_NOTEBOOK(nb), g3, gtk_label_new(T("set.tab_backup")));

    gtk_box_append(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(d))), nb);
    gtk_widget_set_vexpand(nb, TRUE);

    g_signal_connect(d, "response", G_CALLBACK(on_settings_response), c);
    gtk_widget_set_visible(d, TRUE);
}

/* ================================================================== */
/* 代理节点（FR-17）                                                    */
/* ================================================================== */

enum { PCOL_NAME, PCOL_TYPE, PCOL_STATE, PCOL_PORT, PCOL_LATENCY, PCOL_ID, N_PCOLS };

typedef struct {
    BmkWindow    *w;
    GtkWidget    *view;
    GtkListStore *store;
    GtkWidget    *rot_combo;
} ProxyCtx;

static void proxy_reload(ProxyCtx *c)
{
    gtk_list_store_clear(c->store);

    size_t n = bmk_proxy_node_count();
    for (size_t i = 0; i < n; i++) {
        bmk_node *nd = bmk_proxy_node_at(i);
        if (!nd) continue;

        const char *state = nd->available ? T("proxy.available") : T("proxy.untested");
        char lat[32];
        g_strlcpy(lat, "-", sizeof lat);

        GtkTreeIter it;
        gtk_list_store_append(c->store, &it);
        gtk_list_store_set(c->store, &it,
                           PCOL_NAME,    nd->name ? nd->name : "",
                           PCOL_TYPE,    nd->type == BMK_NODE_VLESS ? "vless"
                                       : nd->type == BMK_NODE_HTTP  ? "http" : "socks5",
                           PCOL_STATE,   state,
                           PCOL_PORT,    nd->local_port,
                           PCOL_LATENCY, lat,
                           PCOL_ID,      (gint64)nd->id, -1);
    }
}

static gint64 proxy_selected_id(ProxyCtx *c)
{
    GtkTreeModel *m = NULL;
    GtkTreeIter it;
    if (!gtk_tree_selection_get_selected(
            gtk_tree_view_get_selection(GTK_TREE_VIEW(c->view)), &m, &it))
        return 0;
    gint64 id = 0;
    gtk_tree_model_get(m, &it, PCOL_ID, &id, -1);
    return id;
}

/* ---- 添加节点 ---- */

typedef struct { ProxyCtx *c; GtkWidget *entry; GtkWidget *hint; } AddNodeCtx;

static void on_add_node_response(GtkDialog *d, int response, gpointer user)
{
    AddNodeCtx *a = user;
    if (response != GTK_RESPONSE_ACCEPT) {
        gtk_window_destroy(GTK_WINDOW(d));
        g_free(a);
        return;
    }

    const char *uri = gtk_editable_get_text(GTK_EDITABLE(a->entry));
    bmk_vless v;
    memset(&v, 0, sizeof v);
    if (bmk_vless_parse(uri, &v) != BMK_OK) {
        gtk_label_set_text(GTK_LABEL(a->hint), T("proxy.parse_failed"));
        return;
    }

    bmk_node nd;
    memset(&nd, 0, sizeof nd);
    nd.type = BMK_NODE_VLESS;
    nd.enabled = true;
    nd.vless_uri = bmk_strdup(uri);
    nd.name = bmk_strdup(v.name && *v.name ? v.name : v.host);

    int64_t id = 0;
    bmk_err e = bmk_proxy_node_add(&nd, &id);
    bmk_node_free(&nd);
    bmk_vless_free(&v);

    if (e != BMK_OK) {
        bmk_error_dialog(a->c->w->window, "%s (%s)", T("msg.error"), bmk_err_str(e));
    } else {
        bmk_proxy_save(a->c->w->core->db);
        proxy_reload(a->c);
    }
    gtk_window_destroy(GTK_WINDOW(d));
    g_free(a);
}

static void on_proxy_add(GtkButton *b, gpointer user)
{
    ProxyCtx *c = user;
    AddNodeCtx *a = g_new0(AddNodeCtx, 1);
    a->c = c;

    GtkWidget *d = gtk_dialog_new_with_buttons(
        T("proxy.add"), GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(b))),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        T("btn.cancel"), GTK_RESPONSE_CANCEL,
        T("btn.ok"),     GTK_RESPONSE_ACCEPT, NULL);
    gtk_window_set_default_size(GTK_WINDOW(d), 620, -1);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(box, 16);
    gtk_widget_set_margin_bottom(box, 16);
    gtk_widget_set_margin_start(box, 16);
    gtk_widget_set_margin_end(box, 16);

    gtk_box_append(GTK_BOX(box), gtk_label_new(T("proxy.import_uri")));
    a->entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(a->entry), "vless://...");
    gtk_box_append(GTK_BOX(box), a->entry);

    a->hint = gtk_label_new("");
    gtk_widget_add_css_class(a->hint, "error");
    gtk_widget_set_halign(a->hint, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(box), a->hint);

    gtk_box_append(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(d))), box);
    g_signal_connect(d, "response", G_CALLBACK(on_add_node_response), a);
    gtk_widget_set_visible(d, TRUE);
}

/* ---- 测试 / 启停 / 删除 ---- */

typedef struct {
    ProxyCtx *c;
    gint64    id;      /* 0 = 全部 */
    long      ms;
    bmk_err   err;
} TestJob;

static void test_thread(GTask *task, gpointer src, gpointer data, GCancellable *cc)
{
    (void)src; (void)cc;
    TestJob *j = data;

    char *url = bmk_settings_get_str(j->c->w->core->db, BMK_SET_PROXY_TEST_URL,
                                     "https://www.google.com/generate_204");
    if (j->id > 0) {
        j->err = bmk_proxy_node_test(j->id, url, &j->ms);
    } else {
        size_t n = bmk_proxy_node_count();
        for (size_t i = 0; i < n; i++) {
            bmk_node *nd = bmk_proxy_node_at(i);
            if (nd) { long ms = 0; bmk_proxy_node_test(nd->id, url, &ms); }
        }
        j->err = BMK_OK;
    }
    free(url);
    g_task_return_boolean(task, TRUE);
}

static void test_done(GObject *src, GAsyncResult *r, gpointer user)
{
    (void)src; (void)user;
    TestJob *j = g_task_get_task_data(G_TASK(r));
    proxy_reload(j->c);
    if (j->id > 0) {
        if (j->err == BMK_OK) bmk_toast(j->c->w, "%s: %ld ms", T("proxy.available"), j->ms);
        else                  bmk_toast(j->c->w, "%s (%s)", T("proxy.unavailable"),
                                        bmk_err_str(j->err));
    } else {
        bmk_toast(j->c->w, "%s", T("proxy.test_all"));
    }
}

static void run_test(ProxyCtx *c, gint64 id)
{
    TestJob *j = g_new0(TestJob, 1);
    j->c = c;
    j->id = id;

    GTask *task = g_task_new(NULL, NULL, test_done, NULL);
    g_task_set_task_data(task, j, g_free);
    g_task_run_in_thread(task, test_thread);
    g_object_unref(task);
}

static void on_proxy_test(GtkButton *b, gpointer user)
{
    (void)b;
    ProxyCtx *c = user;
    gint64 id = proxy_selected_id(c);
    if (id > 0) run_test(c, id);
}

static void on_proxy_test_all(GtkButton *b, gpointer user)
{ (void)b; run_test(user, 0); }

static void on_proxy_start(GtkButton *b, gpointer user)
{
    (void)b;
    ProxyCtx *c = user;
    gint64 id = proxy_selected_id(c);
    if (id <= 0) return;

    bmk_err e = bmk_proxy_node_start(id);
    if (e == BMK_ERR_NOT_FOUND)
        bmk_error_dialog(c->w->window, "%s", T("proxy.xray_missing"));
    else if (e != BMK_OK)
        bmk_error_dialog(c->w->window, "%s (%s)", T("msg.error"), bmk_err_str(e));
    proxy_reload(c);
}

static void on_proxy_stop(GtkButton *b, gpointer user)
{
    (void)b;
    ProxyCtx *c = user;
    gint64 id = proxy_selected_id(c);
    if (id > 0) { bmk_proxy_node_stop(id); proxy_reload(c); }
}

static void on_proxy_remove(GtkButton *b, gpointer user)
{
    (void)b;
    ProxyCtx *c = user;
    gint64 id = proxy_selected_id(c);
    if (id <= 0) return;
    bmk_proxy_node_remove(id);
    bmk_proxy_save(c->w->core->db);
    proxy_reload(c);
}

static void on_rotation_changed(GtkDropDown *dd, GParamSpec *ps, gpointer user)
{
    (void)ps;
    ProxyCtx *c = user;
    static const char *names[] = { "round", "random", "failover", "by_host" };
    guint i = gtk_drop_down_get_selected(dd);
    if (i >= G_N_ELEMENTS(names)) return;

    bmk_proxy_set_rotation((bmk_rotation)i);
    bmk_settings_set_str(c->w->core->db, BMK_SET_PROXY_ROTATION, names[i]);
}

static void on_proxy_response(GtkDialog *d, int response, gpointer user)
{
    (void)response;
    ProxyCtx *c = user;
    bmk_proxy_save(c->w->core->db);
    g_free(c);
    gtk_window_destroy(GTK_WINDOW(d));
}

static void add_pcol(ProxyCtx *c, const char *title, int col, gboolean is_int)
{
    gtk_tree_view_append_column(
        GTK_TREE_VIEW(c->view),
        gtk_tree_view_column_new_with_attributes(
            title, gtk_cell_renderer_text_new(),
            is_int ? "text" : "text", col, NULL));
}

void bmk_dialog_proxy(BmkWindow *w)
{
    ProxyCtx *c = g_new0(ProxyCtx, 1);
    c->w = w;

    GtkWidget *d = gtk_dialog_new_with_buttons(
        T("proxy.title"), w->window, GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        T("btn.close"), GTK_RESPONSE_CLOSE, NULL);
    gtk_window_set_default_size(GTK_WINDOW(d), 760, 520);

    c->store = gtk_list_store_new(N_PCOLS, G_TYPE_STRING, G_TYPE_STRING,
                                  G_TYPE_STRING, G_TYPE_INT, G_TYPE_STRING,
                                  G_TYPE_INT64);
    c->view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(c->store));
    add_pcol(c, T("proxy.name"),    PCOL_NAME,    FALSE);
    add_pcol(c, T("proxy.type"),    PCOL_TYPE,    FALSE);
    add_pcol(c, T("proxy.enabled"), PCOL_STATE,   FALSE);
    add_pcol(c, T("proxy.port"),    PCOL_PORT,    TRUE);
    add_pcol(c, T("proxy.latency"), PCOL_LATENCY, FALSE);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), c->view);
    gtk_widget_set_vexpand(scroll, TRUE);

    GtkWidget *btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    struct { const char *label; GCallback cb; } acts[] = {
        { T("proxy.add"),      G_CALLBACK(on_proxy_add) },
        { T("proxy.test"),     G_CALLBACK(on_proxy_test) },
        { T("proxy.test_all"), G_CALLBACK(on_proxy_test_all) },
        { T("proxy.start"),    G_CALLBACK(on_proxy_start) },
        { T("proxy.stop"),     G_CALLBACK(on_proxy_stop) },
        { T("tag.delete"),     G_CALLBACK(on_proxy_remove) },
    };
    for (size_t i = 0; i < G_N_ELEMENTS(acts); i++) {
        GtkWidget *b = gtk_button_new_with_label(acts[i].label);
        g_signal_connect(b, "clicked", acts[i].cb, c);
        gtk_box_append(GTK_BOX(btns), b);
    }

    /* 轮换策略 */
    GtkWidget *rotbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(rotbox), gtk_label_new(T("proxy.rotation")));
    const char *rots[] = { NULL, NULL, NULL, NULL, NULL };
    rots[0] = T("proxy.rot_round");
    rots[1] = T("proxy.rot_random");
    rots[2] = T("proxy.rot_failover");
    rots[3] = T("proxy.rot_by_host");
    c->rot_combo = gtk_drop_down_new_from_strings(rots);
    {
        char *cur = bmk_settings_get_str(w->core->db, BMK_SET_PROXY_ROTATION, "round");
        guint idx = 0;
        if (cur) {
            if      (strcmp(cur, "random") == 0)   idx = 1;
            else if (strcmp(cur, "failover") == 0) idx = 2;
            else if (strcmp(cur, "by_host") == 0)  idx = 3;
            free(cur);
        }
        gtk_drop_down_set_selected(GTK_DROP_DOWN(c->rot_combo), idx);
    }
    g_signal_connect(c->rot_combo, "notify::selected",
                     G_CALLBACK(on_rotation_changed), c);
    gtk_box_append(GTK_BOX(rotbox), c->rot_combo);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top(box, 12);
    gtk_widget_set_margin_bottom(box, 12);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);
    gtk_box_append(GTK_BOX(box), scroll);
    gtk_box_append(GTK_BOX(box), btns);
    gtk_box_append(GTK_BOX(box), rotbox);

    gtk_box_append(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(d))), box);
    proxy_reload(c);

    g_signal_connect(d, "response", G_CALLBACK(on_proxy_response), c);
    gtk_widget_set_visible(d, TRUE);
}

/* ================================================================== */
/* 云端同步（FR-18 / FR-19）                                            */
/* ================================================================== */

typedef struct {
    BmkWindow *w;
    GtkWidget *log_view;
    GtkTextBuffer *log_buf;
    gboolean   restore;
    gboolean   use_s3;
    bmk_err    err;

    /* 配置快照，避免后台线程访问控件 */
    char *repo, *branch, *git_path, *pat;
    gboolean use_ssh, push_db;
    char *sync_dir, *db_path;

    char *endpoint, *region, *bucket, *prefix, *ak, *sk;
    gboolean path_style;
} SyncJob;

static void sync_job_free(gpointer p)
{
    SyncJob *j = p;
    g_free(j->repo); g_free(j->branch); g_free(j->git_path); g_free(j->pat);
    g_free(j->sync_dir); g_free(j->db_path);
    g_free(j->endpoint); g_free(j->region); g_free(j->bucket);
    g_free(j->prefix); g_free(j->ak); g_free(j->sk);
    g_free(j);
}

typedef struct { SyncJob *j; char *line; } LogMsg;

static gboolean sync_log_apply(gpointer user)
{
    LogMsg *m = user;
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(m->j->log_buf, &end);
    gtk_text_buffer_insert(m->j->log_buf, &end, m->line, -1);
    gtk_text_buffer_get_end_iter(m->j->log_buf, &end);
    gtk_text_buffer_insert(m->j->log_buf, &end, "\n", 1);
    g_free(m->line);
    g_free(m);
    return G_SOURCE_REMOVE;
}

static void sync_log_cb(const char *line, void *user)
{
    SyncJob *j = user;
    LogMsg *m = g_new0(LogMsg, 1);
    m->j = j;
    m->line = g_strdup(line ? line : "");
    g_idle_add(sync_log_apply, m);
}

static void sync_thread(GTask *task, gpointer src, gpointer data, GCancellable *cc)
{
    (void)src; (void)cc;
    SyncJob *j = data;
    bmk_db *db = j->w->core->db;

    if (j->use_s3) {
        bmk_s3_cfg cfg;
        memset(&cfg, 0, sizeof cfg);
        cfg.endpoint = j->endpoint;
        cfg.region = j->region;
        cfg.bucket = j->bucket;
        cfg.prefix = j->prefix;
        cfg.access_key = j->ak;
        cfg.secret_key = j->sk;
        cfg.path_style = j->path_style;
        cfg.push_db = j->push_db;
        cfg.db_path = j->db_path;
        j->err = j->restore ? bmk_sync_s3_restore(db, &cfg, sync_log_cb, j)
                            : bmk_sync_s3_push(db, &cfg, sync_log_cb, j);
    } else {
        bmk_github_cfg cfg;
        memset(&cfg, 0, sizeof cfg);
        cfg.repo = j->repo;
        cfg.branch = j->branch;
        cfg.git_path = j->git_path;
        cfg.pat = j->pat;
        cfg.use_ssh = j->use_ssh;
        cfg.push_db = j->push_db;
        cfg.sync_dir = j->sync_dir;
        cfg.db_path = j->db_path;
        j->err = j->restore ? bmk_sync_github_restore(db, &cfg, sync_log_cb, j)
                            : bmk_sync_github_push(db, &cfg, sync_log_cb, j);
    }
    g_task_return_boolean(task, TRUE);
}

static void sync_done(GObject *src, GAsyncResult *r, gpointer user)
{
    (void)src; (void)user;
    SyncJob *j = g_task_get_task_data(G_TASK(r));

    if (j->err == BMK_OK) bmk_toast(j->w, "%s", T("sync.done"));
    else bmk_error_dialog(j->w->window, "%s (%s)", T("sync.failed"), bmk_err_str(j->err));

    bmk_window_refresh_status(j->w);
}

typedef struct {
    BmkWindow *w;
    GtkWidget *nb;
    GtkTextBuffer *log_buf;

    /* GitHub */
    GtkWidget *gh_repo, *gh_branch, *gh_pat, *gh_ssh, *gh_push_db;
    /* S3 */
    GtkWidget *s3_ep, *s3_region, *s3_bucket, *s3_prefix;
    GtkWidget *s3_ak, *s3_sk, *s3_path_style, *s3_push_db;
} SyncCtx;

/* 把界面配置写回 settings 并生成后台任务快照 */
static SyncJob *sync_collect(SyncCtx *c, gboolean restore)
{
    bmk_db *db = c->w->core->db;
    gboolean use_s3 = gtk_notebook_get_current_page(GTK_NOTEBOOK(c->nb)) == 1;

    SyncJob *j = g_new0(SyncJob, 1);
    j->w = c->w;
    j->restore = restore;
    j->use_s3 = use_s3;
    j->log_buf = c->log_buf;
    j->db_path = g_strdup(c->w->core->db_path);

    if (use_s3) {
        const char *ep     = gtk_editable_get_text(GTK_EDITABLE(c->s3_ep));
        const char *region = gtk_editable_get_text(GTK_EDITABLE(c->s3_region));
        const char *bucket = gtk_editable_get_text(GTK_EDITABLE(c->s3_bucket));
        const char *prefix = gtk_editable_get_text(GTK_EDITABLE(c->s3_prefix));
        gboolean ps = gtk_check_button_get_active(GTK_CHECK_BUTTON(c->s3_path_style));
        gboolean pd = gtk_check_button_get_active(GTK_CHECK_BUTTON(c->s3_push_db));

        bmk_settings_set_str(db, BMK_SET_S3_ENDPOINT, ep);
        bmk_settings_set_str(db, BMK_SET_S3_REGION, region);
        bmk_settings_set_str(db, BMK_SET_S3_BUCKET, bucket);
        bmk_settings_set_str(db, BMK_SET_S3_PREFIX, prefix);
        bmk_settings_set_bool(db, BMK_SET_S3_PATH_STYLE, ps);
        bmk_settings_set_bool(db, BMK_SET_S3_PUSH_DB, pd);
        save_secret_if_set(db, BMK_SET_S3_ACCESS_KEY, c->s3_ak);
        save_secret_if_set(db, BMK_SET_S3_SECRET_KEY, c->s3_sk);

        j->endpoint = g_strdup(ep);
        j->region   = g_strdup(region && *region ? region : "us-east-1");
        j->bucket   = g_strdup(bucket);
        j->prefix   = g_strdup(prefix);
        j->path_style = ps;
        j->push_db  = pd;

        char *v;
        v = bmk_settings_get_str(db, BMK_SET_S3_ACCESS_KEY, "");
        j->ak = g_strdup(v ? v : ""); free(v);
        v = bmk_settings_get_str(db, BMK_SET_S3_SECRET_KEY, "");
        j->sk = g_strdup(v ? v : ""); free(v);

        if (!j->bucket || !*j->bucket || !*j->ak || !*j->sk) {
            sync_job_free(j);
            return NULL;
        }
    } else {
        const char *repo   = gtk_editable_get_text(GTK_EDITABLE(c->gh_repo));
        const char *branch = gtk_editable_get_text(GTK_EDITABLE(c->gh_branch));
        gboolean ssh = gtk_check_button_get_active(GTK_CHECK_BUTTON(c->gh_ssh));
        gboolean pd  = gtk_check_button_get_active(GTK_CHECK_BUTTON(c->gh_push_db));

        bmk_settings_set_str(db, BMK_SET_GH_REPO, repo);
        bmk_settings_set_str(db, BMK_SET_GH_BRANCH, branch);
        bmk_settings_set_str(db, BMK_SET_GH_AUTH, ssh ? "ssh" : "pat");
        bmk_settings_set_bool(db, BMK_SET_GH_PUSH_DB, pd);
        save_secret_if_set(db, BMK_SET_GH_PAT, c->gh_pat);

        j->repo    = g_strdup(repo);
        j->branch  = g_strdup(branch && *branch ? branch : "main");
        j->use_ssh = ssh;
        j->push_db = pd;
        j->sync_dir = g_build_filename(c->w->core->sync_dir, "github", NULL);

        char *v;
        v = bmk_settings_get_str(db, BMK_SET_GH_GIT_PATH, "");
        j->git_path = g_strdup(v ? v : ""); free(v);
        v = bmk_settings_get_str(db, BMK_SET_GH_PAT, "");
        j->pat = g_strdup(v ? v : ""); free(v);

        if (!j->repo || !*j->repo) {
            sync_job_free(j);
            return NULL;
        }
    }
    return j;
}

static void launch_sync(SyncCtx *c, gboolean restore)
{
    SyncJob *j = sync_collect(c, restore);
    if (!j) {
        bmk_error_dialog(c->w->window, "%s", T("msg.error"));
        return;
    }
    gtk_text_buffer_set_text(c->log_buf, T("sync.running"), -1);

    GTask *task = g_task_new(NULL, NULL, sync_done, NULL);
    g_task_set_task_data(task, j, sync_job_free);
    g_task_run_in_thread(task, sync_thread);
    g_object_unref(task);
}

static void do_restore(gpointer user) { launch_sync(user, TRUE); }

static void on_sync_response(GtkDialog *d, int response, gpointer user)
{
    SyncCtx *c = user;
    if (response == GTK_RESPONSE_APPLY) {          /* 上传 */
        launch_sync(c, FALSE);
        return;
    }
    if (response == GTK_RESPONSE_YES) {            /* 从云端恢复 */
        bmk_confirm_dialog(GTK_WINDOW(d), T("sync.restore_confirm"), NULL,
                           T("sync.restore"), do_restore, c);
        return;
    }
    g_free(c);
    gtk_window_destroy(GTK_WINDOW(d));
}

void bmk_dialog_sync(BmkWindow *w)
{
    SyncCtx *c = g_new0(SyncCtx, 1);
    c->w = w;
    bmk_db *db = w->core->db;

    GtkWidget *d = gtk_dialog_new_with_buttons(
        T("sync.title"), w->window, GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        T("sync.restore"), GTK_RESPONSE_YES,
        T("sync.push"),    GTK_RESPONSE_APPLY,
        T("btn.close"),    GTK_RESPONSE_CLOSE, NULL);
    gtk_window_set_default_size(GTK_WINDOW(d), 660, 620);

    c->nb = gtk_notebook_new();

    /* ---- GitHub ---- */
    GtkWidget *g1 = grid_new();
    int r = 0;
    {
        char *v;
        v = bmk_settings_get_str(db, BMK_SET_GH_REPO, "");
        c->gh_repo = row_entry(g1, r++, T("sync.repo"), v); free(v);
        v = bmk_settings_get_str(db, BMK_SET_GH_BRANCH, "main");
        c->gh_branch = row_entry(g1, r++, T("sync.branch"), v); free(v);
    }
    c->gh_pat = row_secret(g1, r++, T("sync.pat"), has_setting(db, BMK_SET_GH_PAT));

    c->gh_ssh = gtk_check_button_new_with_label(T("sync.ssh"));
    {
        char *auth = bmk_settings_get_str(db, BMK_SET_GH_AUTH, "pat");
        gtk_check_button_set_active(GTK_CHECK_BUTTON(c->gh_ssh),
                                    auth && strcmp(auth, "ssh") == 0);
        free(auth);
    }
    gtk_grid_attach(GTK_GRID(g1), c->gh_ssh, 1, r++, 1, 1);

    c->gh_push_db = gtk_check_button_new_with_label(T("sync.push_db"));
    gtk_check_button_set_active(GTK_CHECK_BUTTON(c->gh_push_db),
                                bmk_settings_get_bool(db, BMK_SET_GH_PUSH_DB, false));
    gtk_grid_attach(GTK_GRID(g1), c->gh_push_db, 1, r++, 1, 1);

    gtk_notebook_append_page(GTK_NOTEBOOK(c->nb), g1, gtk_label_new(T("sync.github")));

    /* ---- S3 ---- */
    GtkWidget *g2 = grid_new();
    r = 0;
    {
        char *v;
        v = bmk_settings_get_str(db, BMK_SET_S3_ENDPOINT, "");
        c->s3_ep = row_entry(g2, r++, T("sync.endpoint"), v); free(v);
        v = bmk_settings_get_str(db, BMK_SET_S3_REGION, "us-east-1");
        c->s3_region = row_entry(g2, r++, T("sync.region"), v); free(v);
        v = bmk_settings_get_str(db, BMK_SET_S3_BUCKET, "");
        c->s3_bucket = row_entry(g2, r++, T("sync.bucket"), v); free(v);
        v = bmk_settings_get_str(db, BMK_SET_S3_PREFIX, "");
        c->s3_prefix = row_entry(g2, r++, T("sync.prefix"), v); free(v);
    }
    c->s3_ak = row_secret(g2, r++, T("sync.access_key"),
                          has_setting(db, BMK_SET_S3_ACCESS_KEY));
    c->s3_sk = row_secret(g2, r++, T("sync.secret_key"),
                          has_setting(db, BMK_SET_S3_SECRET_KEY));

    c->s3_path_style = gtk_check_button_new_with_label(T("sync.path_style"));
    gtk_check_button_set_active(GTK_CHECK_BUTTON(c->s3_path_style),
                                bmk_settings_get_bool(db, BMK_SET_S3_PATH_STYLE, true));
    gtk_grid_attach(GTK_GRID(g2), c->s3_path_style, 1, r++, 1, 1);

    c->s3_push_db = gtk_check_button_new_with_label(T("sync.push_db"));
    gtk_check_button_set_active(GTK_CHECK_BUTTON(c->s3_push_db),
                                bmk_settings_get_bool(db, BMK_SET_S3_PUSH_DB, false));
    gtk_grid_attach(GTK_GRID(g2), c->s3_push_db, 1, r++, 1, 1);

    gtk_notebook_append_page(GTK_NOTEBOOK(c->nb), g2, gtk_label_new(T("sync.s3")));

    /* ---- 日志 ---- */
    GtkWidget *log = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(log), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(log), TRUE);
    c->log_buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(log));

    GtkWidget *log_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(log_scroll), log);
    gtk_widget_set_vexpand(log_scroll, TRUE);
    gtk_widget_set_size_request(log_scroll, -1, 200);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_box_append(GTK_BOX(box), c->nb);
    gtk_box_append(GTK_BOX(box), log_scroll);
    gtk_box_append(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(d))), box);

    g_signal_connect(d, "response", G_CALLBACK(on_sync_response), c);
    gtk_widget_set_visible(d, TRUE);
}
