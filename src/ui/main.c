/*
 * main.c - GTK4 应用入口
 *
 * 启动流程：解析参数 → 装配 libcore（数据目录、日志、数据库、代理）→
 * 按 settings.language 初始化 i18n → 打开主窗口。
 */
#include "bmk_ui.h"

#include "bmk_settings.h"
#include "bmk_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <windows.h>
#endif

typedef struct {
    char *data_dir;
} CliOpts;

static void parse_args(int argc, char **argv, CliOpts *o, int *out_argc)
{
    memset(o, 0, sizeof *o);
    /* 压缩 argv：移除 --data-dir 及其值，避免 GApplication 报未知选项 */
    int w = 1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--data-dir") == 0 && i + 1 < argc) {
            o->data_dir = argv[++i];
            continue;
        }
        argv[w++] = argv[i];
    }
    argv[w] = NULL;
    *out_argc = w;
}

static void on_activate(GtkApplication *app, gpointer user)
{
    (void)app;
    CliOpts *o = user;

    bmk_app *core = g_new0(bmk_app, 1);
    bmk_err e = bmk_app_startup(o->data_dir, core);
    if (e != BMK_OK) {
        GtkWidget *d = gtk_message_dialog_new(
            NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
            "%s (%s)", "启动失败", bmk_err_str(e));
        gtk_window_set_title(GTK_WINDOW(d), "错误");
        g_signal_connect(d, "response", G_CALLBACK(gtk_window_destroy), NULL);
        gtk_widget_set_visible(d, TRUE);
        g_free(core);
        return;
    }

    /* 按设置的语言初始化界面字符串（需求 5.6）。 */
    char *lang = bmk_settings_get_str(core->db, BMK_SET_LANGUAGE, "zh");
    bmk_i18n_init(lang);
    free(lang);

    bmk_window_new(app, core);

    /* 应用退出时释放核心资源（代理子进程、数据库、日志）。 */
    g_object_set_data_full(G_OBJECT(app), "bmk-core", core,
                           (GDestroyNotify)bmk_app_shutdown);
}

int main(int argc, char **argv)
{
#ifdef _WIN32
    /* 便携版不含 dbus 二进制：让 GIO 跳过会话总线自动探测，
     * 消除 "win32 session dbus binary not found" 的无害警告。
     * 应用本身不使用 D-Bus，此设置无功能影响。 */
    SetEnvironmentVariableA("DBUS_SESSION_BUS_ADDRESS", "disabled:");
#endif

    CliOpts o;
    int g_argc = argc;
    parse_args(argc, argv, &o, &g_argc);

    GtkApplication *app = gtk_application_new("local.bmk.BookmarkManager",
                                               G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), &o);

    int status = g_application_run(G_APPLICATION(app), g_argc, argv);

    g_object_unref(app);
    return status;
}
