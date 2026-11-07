/*
 * bmk-cli - 命令行入口（需求 7.1：核心逻辑可脱离 GUI 运行）
 *
 * 用于冒烟验证、脚本化批处理与问题定位。所有子命令共用 libbookmark_core。
 */
#include "bmk_app.h"
#include "bmk_db.h"
#include "bmk_url.h"
#include "bmk_log.h"
#include "bmk_fetch.h"
#include "bmk_netscape.h"
#include "bmk_settings.h"
#include "bmk_validate.h"
#include "bmk_backup.h"
#include "bmk_proxy.h"
#include "bmk_sync.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(void)
{
    fputs(
"bmk-cli - 本地网址收藏夹管理工具（命令行）\n"
"\n"
"用法: bmk-cli [--data-dir <目录>] <命令> [参数...]\n"
"\n"
"命令:\n"
"  add <url> [--title T] [--category C] [--tags a,b] [--no-fetch]\n"
"                          新增网址（默认联网抓取标题与图标）\n"
"  list [--keyword K] [--domain D] [--tag T] [--category C]\n"
"       [--valid all|valid|invalid|unchecked|blocked]\n"
"       [--sort title|url|created|category|valid|status|checked] [--desc]\n"
"                          列出网址\n"
"  delete <id>...          删除网址（自动生成 HTML 备份）\n"
"  import <file.html> [--category C] [--encoding auto|utf8|gbk] [--dup skip|import]\n"
"  export <file.html>      导出全部网址为 Netscape HTML\n"
"  dedupe [--merge]        查找重复；--merge 自动合并每组\n"
"  check [--all|--id N...] 批量校验可用性\n"
"  categories              打印分类树\n"
"  tags                    打印标签及使用数\n"
"  stats                   打印统计\n"
"  normalize <url>...      打印 url_key（附录 B）\n"
"  proxy list\n"
"  proxy add <vless://...>   添加 VLESS 节点\n"
"  proxy add --type http|socks5 --host H --port P [--user U --pass P] [--name N]\n"
"                          添加普通代理节点并启用\n"
"  proxy check --type http|socks5 --host H --port P [--user U --pass P] [--url URL]\n"
"                          对未保存的配置做 TCP/握手/目标站分层校验\n"
"  proxy test <id> [--url URL]|start <id>|stop <id>\n"
"  sync github|s3          执行一次云端同步\n"
"  backup                  手动全量备份\n"
"  set <key> <value>       写入设置\n"
"  get <key>               读取设置\n"
"\n", stderr);
}

/* ---------------- 参数辅助 ---------------- */

static const char *opt_value(int argc, char **argv, int *i, const char *name)
{
    if (strcmp(argv[*i], name) != 0) return NULL;
    if (*i + 1 >= argc) {
        fprintf(stderr, "错误: %s 需要一个参数\n", name);
        exit(2);
    }
    return argv[++(*i)];
}

static bool has_flag(int argc, char **argv, const char *name)
{
    for (int i = 0; i < argc; i++)
        if (strcmp(argv[i], name) == 0) return true;
    return false;
}

/* 以逗号切分为字符串数组，调用者用 free_list 释放。 */
static char **split_commas(const char *s, size_t *out_n)
{
    *out_n = 0;
    if (!s || !*s) return NULL;

    size_t cap = 8, n = 0;
    char **arr = (char **)calloc(cap, sizeof *arr);
    if (!arr) return NULL;

    const char *p = s;
    while (*p) {
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        char *item = bmk_strndup(p, len);
        if (item) {
            bmk_str_trim(item);
            if (*item) {
                if (n == cap) {
                    cap *= 2;
                    char **grown = (char **)realloc(arr, cap * sizeof *arr);
                    if (!grown) { free(item); break; }
                    arr = grown;
                }
                arr[n++] = item;
            } else {
                free(item);
            }
        }
        if (!comma) break;
        p = comma + 1;
    }
    *out_n = n;
    return arr;
}

static void free_list(char **arr, size_t n)
{
    if (!arr) return;
    for (size_t i = 0; i < n; i++) free(arr[i]);
    free(arr);
}

/* 按名称查找分类，找不到则按 "a/b/c" 路径逐级创建。 */
static int64_t resolve_category(bmk_db *db, const char *spec, bool create)
{
    if (!spec || !*spec) return 0;

    int64_t parent = 0;
    const char *p = spec;
    while (*p) {
        const char *slash = strchr(p, '/');
        size_t len = slash ? (size_t)(slash - p) : strlen(p);
        char *name = bmk_strndup(p, len);
        if (!name) return 0;
        bmk_str_trim(name);

        int64_t id = bmk_category_find(db, name, parent);
        if (!id && create) bmk_category_create(db, name, parent, &id);
        free(name);
        if (!id) return 0;
        parent = id;
        if (!slash) break;
        p = slash + 1;
    }
    return parent;
}

static void print_category_path(bmk_db *db, int64_t id, char *out, size_t outsz)
{
    out[0] = '\0';
    if (id <= 0) { snprintf(out, outsz, "%s", "未分类"); return; }

    bmk_category **cats = NULL;
    size_t n = 0;
    if (bmk_category_list(db, &cats, &n) != BMK_OK) return;

    /* 自底向上拼路径。 */
    char parts[16][128];
    int  depth = 0;
    int64_t cur = id;
    while (cur > 0 && depth < 16) {
        bmk_category *found = NULL;
        for (size_t i = 0; i < n; i++)
            if (cats[i]->id == cur) { found = cats[i]; break; }
        if (!found) break;
        snprintf(parts[depth++], sizeof parts[0], "%s", found->name);
        cur = found->parent_id;
    }
    for (int i = depth - 1; i >= 0; i--) {
        strncat(out, parts[i], outsz - strlen(out) - 1);
        if (i) strncat(out, "/", outsz - strlen(out) - 1);
    }
    bmk_category_list_free(cats, n);
}

/* ---------------- 子命令 ---------------- */

static int cmd_normalize(int argc, char **argv)
{
    for (int i = 0; i < argc; i++) {
        char *key = NULL;
        bmk_err e = bmk_url_normalize(argv[i], &key);
        if (e == BMK_OK) {
            printf("%s\t%s\n", argv[i], key);
            free(key);
        } else {
            printf("%s\t<%s>\n", argv[i], bmk_err_str(e));
        }
    }
    return 0;
}

static int cmd_add(bmk_app *app, int argc, char **argv)
{
    if (argc < 1) { fprintf(stderr, "错误: 缺少 URL\n"); return 2; }

    const char *url = argv[0];
    const char *title = NULL, *category = NULL, *tags = NULL;
    bool fetch = true;

    for (int i = 1; i < argc; i++) {
        const char *v;
        if ((v = opt_value(argc, argv, &i, "--title")))    { title = v; continue; }
        if ((v = opt_value(argc, argv, &i, "--category"))) { category = v; continue; }
        if ((v = opt_value(argc, argv, &i, "--tags")))     { tags = v; continue; }
        if (strcmp(argv[i], "--no-fetch") == 0) { fetch = false; continue; }
    }

    const char *reason = NULL;
    if (!bmk_url_is_valid_input(url, &reason)) {
        fprintf(stderr, "错误: URL 不合法（%s）\n", reason ? reason : "invalid");
        return 1;
    }

    char *key = NULL;
    if (bmk_url_normalize(url, &key) != BMK_OK) {
        fprintf(stderr, "错误: URL 无法归一化\n");
        return 1;
    }

    char *dup_title = NULL;
    int64_t dups = bmk_bookmark_count_by_key(app->db, key, &dup_title);
    if (dups > 0) {
        printf("提示: 已存在相同网址（%s），仍将新增一条。\n",
               dup_title ? dup_title : "无标题");
    }
    free(dup_title);

    bmk_bookmark b;
    memset(&b, 0, sizeof b);
    b.url     = bmk_strdup(url);
    b.url_key = key;
    b.valid   = BMK_VALID_UNKNOWN;
    b.status_code = -1;

    bmk_fetch_result fr;
    memset(&fr, 0, sizeof fr);
    if (fetch) {
        bmk_fetch_opts fo;
        bmk_fetch_opts_init(&fo);
        fo.use_icon_service = bmk_settings_get_bool(app->db, BMK_SET_ICON_SERVICE, true);
        if (bmk_fetch_page(url, &fo, &fr) == BMK_OK) {
            if (fr.icon && fr.icon_len) {
                b.icon = (unsigned char *)malloc(fr.icon_len);
                if (b.icon) {
                    memcpy(b.icon, fr.icon, fr.icon_len);
                    b.icon_len = fr.icon_len;
                    b.icon_mime = bmk_strdup(fr.icon_mime);
                }
            }
        }
    }

    if (title && *title)          b.title = bmk_strdup(title);
    else if (fr.title && *fr.title) b.title = bmk_strdup(fr.title);
    else                           b.title = bmk_strdup(url);   /* FR-03 第 5 条 */

    if (category) b.category_id = resolve_category(app->db, category, true);

    bmk_err e = bmk_bookmark_insert(app->db, &b);
    if (e == BMK_OK && tags) {
        size_t tn = 0;
        char **tv = split_commas(tags, &tn);
        if (tn) bmk_bookmark_set_tags(app->db, b.id, (const char *const *)tv, tn);
        free_list(tv, tn);
    }

    if (e == BMK_OK) printf("已新增 #%lld  %s\n", (long long)b.id, b.title);
    else             fprintf(stderr, "错误: 新增失败（%s）\n", bmk_err_str(e));

    bmk_fetch_result_free(&fr);
    bmk_bookmark_free(&b);
    return e == BMK_OK ? 0 : 1;
}

static const char *valid_text(int v, const char *note)
{
    if (v == BMK_VALID_GOOD) return "有效";
    if (v == BMK_VALID_BAD)  return "失效";
    if (note && strcmp(note, BMK_NOTE_BLOCKED) == 0) return "待复测";
    return "未校验";
}

static int cmd_list(bmk_app *app, int argc, char **argv)
{
    bmk_filter f;
    bmk_filter_init(&f);
    bmk_sort_key sort = BMK_SORT_CREATED_AT;
    bool asc = true;
    const char *tag_name = NULL, *category = NULL;

    for (int i = 0; i < argc; i++) {
        const char *v;
        if ((v = opt_value(argc, argv, &i, "--keyword"))) { f.keyword = bmk_strdup(v); continue; }
        if ((v = opt_value(argc, argv, &i, "--url")))     { f.url_contains = bmk_strdup(v); continue; }
        if ((v = opt_value(argc, argv, &i, "--domain")))  { f.domain = bmk_strdup(v); continue; }
        if ((v = opt_value(argc, argv, &i, "--tag")))     { tag_name = v; continue; }
        if ((v = opt_value(argc, argv, &i, "--category"))){ category = v; continue; }
        if ((v = opt_value(argc, argv, &i, "--valid"))) {
            if      (strcmp(v, "valid") == 0)     f.valid_filter = BMK_VF_VALID;
            else if (strcmp(v, "invalid") == 0)   f.valid_filter = BMK_VF_INVALID;
            else if (strcmp(v, "unchecked") == 0) f.valid_filter = BMK_VF_UNCHECKED;
            else if (strcmp(v, "blocked") == 0)   f.valid_filter = BMK_VF_BLOCKED;
            else                                  f.valid_filter = BMK_VF_ALL;
            continue;
        }
        if ((v = opt_value(argc, argv, &i, "--sort"))) {
            if      (strcmp(v, "title") == 0)    sort = BMK_SORT_TITLE;
            else if (strcmp(v, "url") == 0)      sort = BMK_SORT_URL;
            else if (strcmp(v, "category") == 0) sort = BMK_SORT_CATEGORY;
            else if (strcmp(v, "valid") == 0)    sort = BMK_SORT_VALID;
            else if (strcmp(v, "status") == 0)   sort = BMK_SORT_STATUS_CODE;
            else if (strcmp(v, "checked") == 0)  sort = BMK_SORT_LAST_CHECKED;
            else                                 sort = BMK_SORT_CREATED_AT;
            continue;
        }
        if (strcmp(argv[i], "--desc") == 0) { asc = false; continue; }
    }

    if (category) {
        int64_t cid = resolve_category(app->db, category, false);
        f.category_id = cid ? cid : BMK_CATEGORY_ALL;
    }
    if (tag_name) {
        bmk_tag **tags = NULL;
        size_t tn = 0;
        if (bmk_tag_list(app->db, &tags, &tn) == BMK_OK) {
            for (size_t i = 0; i < tn; i++) {
                if (bmk_strcasecmp(tags[i]->name, tag_name) == 0) {
                    f.tag_ids = (int64_t *)malloc(sizeof(int64_t));
                    if (f.tag_ids) { f.tag_ids[0] = tags[i]->id; f.tag_count = 1; }
                    break;
                }
            }
            bmk_tag_list_free(tags, tn);
        }
    }

    bmk_bookmark **list = NULL;
    size_t n = 0;
    bmk_err e = bmk_bookmark_query(app->db, &f, sort, asc, &list, &n);
    bmk_filter_free(&f);
    if (e != BMK_OK) {
        fprintf(stderr, "错误: 查询失败（%s）\n", bmk_err_str(e));
        return 1;
    }

    printf("%-6s %-40s %-52s %-14s %-8s %s\n",
           "ID", "标题", "网址", "分类", "状态", "标签");
    for (size_t i = 0; i < n; i++) {
        bmk_bookmark *b = list[i];
        char cat[256];
        print_category_path(app->db, b->category_id, cat, sizeof cat);

        char tagbuf[256] = "";
        for (size_t t = 0; t < b->tag_count; t++) {
            if (t) strncat(tagbuf, ",", sizeof tagbuf - strlen(tagbuf) - 1);
            strncat(tagbuf, b->tags[t], sizeof tagbuf - strlen(tagbuf) - 1);
        }
        printf("%-6lld %-40.40s %-52.52s %-14.14s %-8s %s\n",
               (long long)b->id,
               b->title ? b->title : "",
               b->url ? b->url : "",
               cat,
               valid_text(b->valid, b->check_note),
               tagbuf);
    }
    printf("\n共 %zu 条\n", n);
    bmk_bookmark_list_free(list, n);
    return 0;
}

static int cmd_delete(bmk_app *app, int argc, char **argv)
{
    if (argc < 1) { fprintf(stderr, "错误: 缺少 id\n"); return 2; }

    int64_t *ids = (int64_t *)calloc((size_t)argc, sizeof *ids);
    if (!ids) return 1;
    size_t n = 0;
    for (int i = 0; i < argc; i++) {
        long long v = atoll(argv[i]);
        if (v > 0) ids[n++] = (int64_t)v;
    }

    char *backup = NULL;
    bmk_err e = bmk_backup_delete_bookmarks(app->db, ids, n, &backup);
    free(ids);
    if (e == BMK_OK) {
        printf("已删除 %zu 条，备份: %s\n", n, backup ? backup : "(无)");
        free(backup);
        return 0;
    }
    fprintf(stderr, "错误: 删除失败（%s）\n", bmk_err_str(e));
    return 1;
}

static bool import_progress(int64_t done, int64_t total, void *user)
{
    (void)user;
    if (total > 0 && done % 100 == 0)
        fprintf(stderr, "\r导入中 %lld/%lld", (long long)done, (long long)total);
    return true;
}

static int cmd_import(bmk_app *app, int argc, char **argv)
{
    if (argc < 1) { fprintf(stderr, "错误: 缺少文件路径\n"); return 2; }

    bmk_import_opts o;
    bmk_import_opts_init(&o);
    o.progress = import_progress;

    for (int i = 1; i < argc; i++) {
        const char *v;
        if ((v = opt_value(argc, argv, &i, "--category"))) {
            o.root_category_id = resolve_category(app->db, v, true);
            continue;
        }
        if ((v = opt_value(argc, argv, &i, "--encoding"))) {
            if      (strcmp(v, "utf8") == 0) o.encoding = BMK_ENC_UTF8;
            else if (strcmp(v, "gbk") == 0)  o.encoding = BMK_ENC_GBK;
            else                              o.encoding = BMK_ENC_AUTO;
            continue;
        }
        if ((v = opt_value(argc, argv, &i, "--dup"))) {
            o.dup_policy = (strcmp(v, "import") == 0) ? BMK_DUP_IMPORT : BMK_DUP_SKIP;
            continue;
        }
    }

    bmk_import_report r;
    memset(&r, 0, sizeof r);
    bmk_err e = bmk_import_html_file(app->db, argv[0], &o, &r);
    fprintf(stderr, "\r");

    if (e != BMK_OK) {
        fprintf(stderr, "错误: 导入失败（%s）\n", bmk_err_str(e));
        bmk_import_report_free(&r);
        return 1;
    }
    printf("导入完成: 新增 %lld，跳过重复 %lld，失败 %lld\n",
           (long long)r.added, (long long)r.skipped, (long long)r.failed);
    for (size_t i = 0; i < r.failed_count && i < 20; i++)
        printf("  失败: %s\n", r.failed_reasons[i]);
    bmk_import_report_free(&r);
    return 0;
}

static int cmd_export(bmk_app *app, int argc, char **argv)
{
    if (argc < 1) { fprintf(stderr, "错误: 缺少输出路径\n"); return 2; }
    bmk_err e = bmk_export_html_file(app->db, argv[0], NULL, 0);
    if (e != BMK_OK) {
        fprintf(stderr, "错误: 导出失败（%s）\n", bmk_err_str(e));
        return 1;
    }
    printf("已导出到 %s\n", argv[0]);
    return 0;
}

static int cmd_dedupe(bmk_app *app, int argc, char **argv)
{
    bool do_merge = has_flag(argc, argv, "--merge");

    bmk_dup_group **groups = NULL;
    size_t n = 0;
    if (bmk_dedupe_find(app->db, &groups, &n) != BMK_OK) {
        fprintf(stderr, "错误: 查重失败\n");
        return 1;
    }
    if (n == 0) {
        printf("未发现重复网址。\n");
        bmk_dup_group_list_free(groups, n);
        return 0;
    }

    int64_t merged = 0;
    for (size_t i = 0; i < n; i++) {
        bmk_dup_group *g = groups[i];
        printf("[%zu] %s  (%zu 条)\n", i + 1, g->url_key, g->count);
        for (size_t k = 0; k < g->count; k++)
            printf("     #%lld  %s\n", (long long)g->members[k]->id,
                   g->members[k]->title ? g->members[k]->title : "");

        if (do_merge && g->count > 1) {
            int64_t *ids = (int64_t *)calloc(g->count, sizeof *ids);
            if (!ids) continue;
            for (size_t k = 0; k < g->count; k++) ids[k] = g->members[k]->id;
            int64_t primary = 0;
            if (bmk_merge_group(app->db, ids, g->count, &primary) == BMK_OK) {
                printf("     -> 已合并为 #%lld\n", (long long)primary);
                merged++;
            }
            free(ids);
        }
    }
    printf("\n共 %zu 组重复%s\n", n,
           do_merge ? "" : "（加 --merge 可自动合并）");
    if (do_merge) printf("已合并 %lld 组\n", (long long)merged);
    bmk_dup_group_list_free(groups, n);
    return 0;
}

static bool check_progress(int64_t done, int64_t total, const bmk_bookmark *b,
                           const bmk_check_result *res, void *user)
{
    (void)user;
    printf("[%lld/%lld] %-50.50s  %s (%d)\n",
           (long long)done, (long long)total,
           b && b->url ? b->url : "",
           res ? res->note : "", res ? res->status_code : 0);
    fflush(stdout);
    return true;
}

static int cmd_check(bmk_app *app, int argc, char **argv)
{
    int64_t *ids = NULL;
    size_t n = 0;

    if (has_flag(argc, argv, "--all") || argc == 0) {
        bmk_filter f;
        bmk_filter_init(&f);
        bmk_bookmark **list = NULL;
        size_t ln = 0;
        if (bmk_bookmark_query(app->db, &f, BMK_SORT_CREATED_AT, true, &list, &ln) != BMK_OK) {
            bmk_filter_free(&f);
            return 1;
        }
        bmk_filter_free(&f);
        ids = (int64_t *)calloc(ln ? ln : 1, sizeof *ids);
        if (ids) for (size_t i = 0; i < ln; i++) ids[n++] = list[i]->id;
        bmk_bookmark_list_free(list, ln);
    } else {
        ids = (int64_t *)calloc((size_t)argc, sizeof *ids);
        if (!ids) return 1;
        for (int i = 0; i < argc; i++) {
            if (strcmp(argv[i], "--id") == 0) continue;
            long long v = atoll(argv[i]);
            if (v > 0) ids[n++] = (int64_t)v;
        }
    }
    if (!ids || n == 0) {
        free(ids);
        printf("没有需要校验的网址。\n");
        return 0;
    }

    bmk_validate_opts o;
    bmk_validate_opts_load(app->db, &o);
    bmk_err e = bmk_validate_run(app->db, ids, n, &o, check_progress, NULL);
    free(ids);

    bmk_stats st;
    if (bmk_db_stats(app->db, &st) == BMK_OK)
        printf("\n总计 %lld，失效 %lld，待复测 %lld，未校验 %lld\n",
               (long long)st.total, (long long)st.invalid,
               (long long)st.blocked, (long long)st.unchecked);
    return e == BMK_OK ? 0 : 1;
}

static void print_cat_tree(bmk_category **cats, size_t n, int64_t parent, int depth,
                           bmk_db *db)
{
    for (size_t i = 0; i < n; i++) {
        if (cats[i]->parent_id != parent) continue;
        for (int d = 0; d < depth; d++) fputs("  ", stdout);
        printf("- %s (#%lld, %lld)\n", cats[i]->name, (long long)cats[i]->id,
               (long long)bmk_category_count_recursive(db, cats[i]->id));
        print_cat_tree(cats, n, cats[i]->id, depth + 1, db);
    }
}

static int cmd_categories(bmk_app *app)
{
    bmk_category **cats = NULL;
    size_t n = 0;
    if (bmk_category_list(app->db, &cats, &n) != BMK_OK) return 1;
    print_cat_tree(cats, n, 0, 0, app->db);
    bmk_category_list_free(cats, n);
    return 0;
}

static int cmd_tags(bmk_app *app)
{
    bmk_tag **tags = NULL;
    size_t n = 0;
    if (bmk_tag_list(app->db, &tags, &n) != BMK_OK) return 1;
    for (size_t i = 0; i < n; i++)
        printf("#%lld  %-24s %lld\n", (long long)tags[i]->id, tags[i]->name,
               (long long)tags[i]->usage_count);
    bmk_tag_list_free(tags, n);
    return 0;
}

static int cmd_stats(bmk_app *app)
{
    bmk_stats st;
    if (bmk_db_stats(app->db, &st) != BMK_OK) return 1;
    printf("总计   %lld\n失效   %lld\n待复测 %lld\n未校验 %lld\n",
           (long long)st.total, (long long)st.invalid,
           (long long)st.blocked, (long long)st.unchecked);
    return 0;
}

/* 解析 --type/--host/--port/--user/--pass/--name/--url，字符串字段为堆副本。
 * 调用方负责 bmk_node_free；--url 指向 argv 或调用方的缓冲区，不接管所有权。 */
static void parse_proxy_opts(int argc, char **argv, int start,
                             bmk_node *n, const char **test_url)
{
    for (int i = start; i < argc; i++) {
        if (i + 1 >= argc) break;
        if      (strcmp(argv[i], "--type") == 0) {
            const char *v = argv[++i];
            n->type = (strcmp(v, "socks5") == 0) ? BMK_NODE_SOCKS5 : BMK_NODE_HTTP;
        } else if (strcmp(argv[i], "--host") == 0) {
            free(n->host); n->host = bmk_strdup(argv[++i]);
        } else if (strcmp(argv[i], "--port") == 0) {
            n->port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--user") == 0) {
            free(n->username); n->username = bmk_strdup(argv[++i]);
        } else if (strcmp(argv[i], "--pass") == 0) {
            free(n->password); n->password = bmk_strdup(argv[++i]);
        } else if (strcmp(argv[i], "--name") == 0) {
            free(n->name); n->name = bmk_strdup(argv[++i]);
        } else if (strcmp(argv[i], "--url") == 0) {
            *test_url = argv[++i];
        }
        /* 未知选项忽略即可，不要回退 i，否则 for 的 i++ 会原地打转 */
    }
}

static void print_proxy_check(const bmk_proxy_check *chk)
{
    static const char *kStage[] = { "TCP", "Handshake", "Target" };
    for (int i = 0; i < BMK_PCHK_STAGES; i++) {
        const char *st = (chk->state[i] == BMK_PCHK_OK)   ? "OK"
                       : (chk->state[i] == BMK_PCHK_FAIL) ? "FAIL" : "SKIP";
        printf("%-9s %-4s %4ldms  %s\n", kStage[i], st, chk->ms[i], chk->detail[i]);
    }
    printf("结论: %s (%s)\n", chk->usable ? "代理可用" : "代理不可用",
           bmk_err_str(chk->err));
    if (chk->usable && chk->state[BMK_PCHK_TARGET] == BMK_PCHK_FAIL)
        printf("注意: 代理本身可用，但目标站不通。\n");
}

static int cmd_proxy(bmk_app *app, int argc, char **argv)
{
    if (argc < 1) { fprintf(stderr, "错误: 缺少子命令\n"); return 2; }

    /* 对未保存的临时配置做分层校验：不写库、不启动进程 */
    if (strcmp(argv[0], "check") == 0) {
        bmk_node nd;
        memset(&nd, 0, sizeof nd);
        nd.type = BMK_NODE_HTTP;          /* 未指定 --type 时按 HTTP */
        const char *url = NULL;
        char *url_buf = NULL;
        parse_proxy_opts(argc, argv, 1, &nd, &url);

        if (!nd.host || !*nd.host || nd.port <= 0 || nd.port > 65535) {
            fprintf(stderr, "错误: 需要 --host 与合法的 --port (1-65535)\n");
            bmk_node_free(&nd);
            return 2;
        }
        if (!url || !*url)
            url_buf = bmk_settings_get_str(app->db, BMK_SET_PROXY_TEST_URL,
                                           "https://www.google.com/generate_204");
        const char *use_url = (url && *url) ? url : url_buf;

        bmk_proxy_check chk;
        bmk_proxy_endpoint_check(&nd, use_url, &chk);
        print_proxy_check(&chk);

        int rc = chk.usable ? 0 : 1;
        free(url_buf);
        bmk_node_free(&nd);
        return rc;
    }

    if (strcmp(argv[0], "list") == 0) {
        size_t n = bmk_proxy_node_count();
        for (size_t i = 0; i < n; i++) {
            bmk_node *nd = bmk_proxy_node_at(i);
            printf("#%lld  %-20s %-8s %s  端口=%d  %s\n",
                   (long long)nd->id, nd->name ? nd->name : "",
                   nd->type == BMK_NODE_VLESS ? "vless" :
                   nd->type == BMK_NODE_HTTP ? "http" : "socks5",
                   nd->enabled ? "已启用" : "已停用",
                   nd->local_port,
                   nd->available ? "可用" : "未测试/不可用");
        }
        if (!n) printf("尚未配置代理节点。\n");
        return 0;
    }

    if (strcmp(argv[0], "add") == 0) {
        if (argc < 2) {
            fprintf(stderr, "错误: 需要 vless:// 链接，或 --type/--host/--port 参数\n");
            return 2;
        }
        bmk_node nd;
        memset(&nd, 0, sizeof nd);
        nd.enabled = true;
        int64_t id = 0;
        bmk_err e;

        if (strncmp(argv[1], "vless://", 8) == 0) {
            bmk_vless v;
            memset(&v, 0, sizeof v);
            if (bmk_vless_parse(argv[1], &v) != BMK_OK) {
                fprintf(stderr, "错误: vless 链接解析失败\n");
                return 1;
            }
            nd.type = BMK_NODE_VLESS;
            nd.vless_uri = bmk_strdup(argv[1]);
            nd.name = bmk_strdup(v.name && *v.name ? v.name : v.host);
            e = bmk_proxy_node_add(&nd, &id);
            bmk_vless_free(&v);
        } else {
            nd.type = BMK_NODE_HTTP;      /* 未指定 --type 时按 HTTP */
            const char *ignored_url = NULL;
            parse_proxy_opts(argc, argv, 1, &nd, &ignored_url);
            if (!nd.host || !*nd.host || nd.port <= 0 || nd.port > 65535) {
                fprintf(stderr, "错误: 需要 --host 与合法的 --port (1-65535)\n");
                bmk_node_free(&nd);
                return 2;
            }
            if (!nd.name) {
                char nb[256];
                snprintf(nb, sizeof nb, "%s %s:%d",
                         nd.type == BMK_NODE_SOCKS5 ? "SOCKS5" : "HTTP",
                         nd.host, nd.port);
                nd.name = bmk_strdup(nb);
            }
            e = bmk_proxy_node_add(&nd, &id);
            /* HTTP/SOCKS5 无子进程，start 只是把 available 置 true */
            if (e == BMK_OK) bmk_proxy_node_start(id);
        }

        if (e == BMK_OK) { bmk_proxy_save(app->db); printf("已添加节点 #%lld\n", (long long)id); }
        else fprintf(stderr, "错误: 添加失败（%s）\n", bmk_err_str(e));
        bmk_node_free(&nd);
        return e == BMK_OK ? 0 : 1;
    }

    if (argc < 2) { fprintf(stderr, "错误: 缺少节点 id\n"); return 2; }
    int64_t id = (int64_t)atoll(argv[1]);

    if (strcmp(argv[0], "start") == 0) {
        bmk_err e = bmk_proxy_node_start(id);
        printf("%s\n", e == BMK_OK ? "已启动" : bmk_err_str(e));
        return e == BMK_OK ? 0 : 1;
    }
    if (strcmp(argv[0], "stop") == 0) { bmk_proxy_node_stop(id); printf("已停止\n"); return 0; }
    if (strcmp(argv[0], "test") == 0) {
        const char *override = NULL;
        for (int k = 2; k < argc; k++)
            if (strcmp(argv[k], "--url") == 0 && k + 1 < argc) {
                override = argv[++k];
                break;
            }
        char *url = override ? bmk_strdup(override)
                             : bmk_settings_get_str(app->db, BMK_SET_PROXY_TEST_URL,
                                                    "https://www.google.com/generate_204");
        long ms = 0;
        bmk_err e = bmk_proxy_node_test(id, url, &ms);
        free(url);
        if (e == BMK_OK) printf("连通，耗时 %ld ms\n", ms);
        else             fprintf(stderr, "不可用（%s）\n", bmk_err_str(e));
        return e == BMK_OK ? 0 : 1;
    }
    fprintf(stderr, "错误: 未知子命令 %s\n", argv[0]);
    return 2;
}

static void sync_log(const char *line, void *user)
{
    (void)user;
    printf("%s\n", line);
    fflush(stdout);
}

static int cmd_sync(bmk_app *app, int argc, char **argv)
{
    if (argc < 1) { fprintf(stderr, "错误: 需指定 github 或 s3\n"); return 2; }

    if (strcmp(argv[0], "github") == 0) {
        char *repo   = bmk_settings_get_str(app->db, BMK_SET_GH_REPO, NULL);
        char *branch = bmk_settings_get_str(app->db, BMK_SET_GH_BRANCH, NULL);
        char *git    = bmk_settings_get_str(app->db, BMK_SET_GH_GIT_PATH, NULL);
        char *pat    = bmk_settings_get_str(app->db, BMK_SET_GH_PAT, NULL);
        char *auth   = bmk_settings_get_str(app->db, BMK_SET_GH_AUTH, "pat");
        char *dir    = bmk_path_join(app->sync_dir, "github");

        if (!repo || !*repo) {
            fprintf(stderr, "错误: 未配置 github_repo\n");
            free(repo); free(branch); free(git); free(pat); free(auth); free(dir);
            return 1;
        }
        bmk_github_cfg cfg;
        memset(&cfg, 0, sizeof cfg);
        cfg.repo = repo;
        cfg.branch = branch;
        cfg.git_path = git;
        cfg.pat = pat;
        cfg.use_ssh = auth && strcmp(auth, "ssh") == 0;
        cfg.push_db = bmk_settings_get_bool(app->db, BMK_SET_GH_PUSH_DB, false);
        cfg.sync_dir = dir;
        cfg.db_path = app->db_path;

        bmk_err e = has_flag(argc, argv, "--restore")
                  ? bmk_sync_github_restore(app->db, &cfg, sync_log, NULL)
                  : bmk_sync_github_push(app->db, &cfg, sync_log, NULL);
        free(repo); free(branch); free(git); free(pat); free(auth); free(dir);
        if (e != BMK_OK) fprintf(stderr, "同步失败（%s）\n", bmk_err_str(e));
        return e == BMK_OK ? 0 : 1;
    }

    if (strcmp(argv[0], "s3") == 0) {
        char *ep     = bmk_settings_get_str(app->db, BMK_SET_S3_ENDPOINT, NULL);
        char *region = bmk_settings_get_str(app->db, BMK_SET_S3_REGION, "us-east-1");
        char *bucket = bmk_settings_get_str(app->db, BMK_SET_S3_BUCKET, NULL);
        char *prefix = bmk_settings_get_str(app->db, BMK_SET_S3_PREFIX, NULL);
        char *ak     = bmk_settings_get_str(app->db, BMK_SET_S3_ACCESS_KEY, NULL);
        char *sk     = bmk_settings_get_str(app->db, BMK_SET_S3_SECRET_KEY, NULL);

        if (!bucket || !*bucket || !ak || !sk) {
            fprintf(stderr, "错误: S3 配置不完整（需 bucket / access_key / secret_key）\n");
            free(ep); free(region); free(bucket); free(prefix); free(ak); free(sk);
            return 1;
        }
        bmk_s3_cfg cfg;
        memset(&cfg, 0, sizeof cfg);
        cfg.endpoint = ep;
        cfg.region = region;
        cfg.bucket = bucket;
        cfg.prefix = prefix;
        cfg.access_key = ak;
        cfg.secret_key = sk;
        cfg.path_style = bmk_settings_get_bool(app->db, BMK_SET_S3_PATH_STYLE, true);
        cfg.push_db = bmk_settings_get_bool(app->db, BMK_SET_S3_PUSH_DB, false);
        cfg.db_path = app->db_path;

        bmk_err e = has_flag(argc, argv, "--restore")
                  ? bmk_sync_s3_restore(app->db, &cfg, sync_log, NULL)
                  : bmk_sync_s3_push(app->db, &cfg, sync_log, NULL);
        free(ep); free(region); free(bucket); free(prefix); free(ak); free(sk);
        if (e != BMK_OK) fprintf(stderr, "同步失败（%s）\n", bmk_err_str(e));
        return e == BMK_OK ? 0 : 1;
    }

    fprintf(stderr, "错误: 未知同步目标 %s\n", argv[0]);
    return 2;
}

/* ---------------- main ---------------- */

int main(int argc, char **argv)
{
    const char *data_dir = NULL;
    int i = 1;

    while (i < argc && strncmp(argv[i], "--", 2) == 0) {
        if (strcmp(argv[i], "--data-dir") == 0 && i + 1 < argc) {
            data_dir = argv[++i];
            i++;
            continue;
        }
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage();
            return 0;
        }
        if (strcmp(argv[i], "--verbose") == 0) { i++; continue; }
        break;
    }
    if (i >= argc) { usage(); return 2; }

    const char *cmd = argv[i++];
    int    sub_argc = argc - i;
    char **sub_argv = argv + i;

    /* normalize 是纯函数命令，不需要打开数据库。 */
    if (strcmp(cmd, "normalize") == 0)
        return cmd_normalize(sub_argc, sub_argv);

    bmk_app app;
    bmk_err e = bmk_app_startup(data_dir, &app);
    if (e != BMK_OK) {
        fprintf(stderr, "错误: 启动失败（%s）。请检查数据目录是否可写。\n",
                bmk_err_str(e));
        return 1;
    }
    if (app.db_recovered)
        fprintf(stderr, "警告: 检测到数据库损坏，已新建空库；"
                        "原文件保留在 %s，可用 import 从 backups 恢复。\n",
                app.recovered_from ? app.recovered_from : "(未知)");

    int rc;
    if      (strcmp(cmd, "add") == 0)        rc = cmd_add(&app, sub_argc, sub_argv);
    else if (strcmp(cmd, "list") == 0)       rc = cmd_list(&app, sub_argc, sub_argv);
    else if (strcmp(cmd, "delete") == 0)     rc = cmd_delete(&app, sub_argc, sub_argv);
    else if (strcmp(cmd, "import") == 0)     rc = cmd_import(&app, sub_argc, sub_argv);
    else if (strcmp(cmd, "export") == 0)     rc = cmd_export(&app, sub_argc, sub_argv);
    else if (strcmp(cmd, "dedupe") == 0)     rc = cmd_dedupe(&app, sub_argc, sub_argv);
    else if (strcmp(cmd, "check") == 0)      rc = cmd_check(&app, sub_argc, sub_argv);
    else if (strcmp(cmd, "categories") == 0) rc = cmd_categories(&app);
    else if (strcmp(cmd, "tags") == 0)       rc = cmd_tags(&app);
    else if (strcmp(cmd, "stats") == 0)      rc = cmd_stats(&app);
    else if (strcmp(cmd, "proxy") == 0)      rc = cmd_proxy(&app, sub_argc, sub_argv);
    else if (strcmp(cmd, "sync") == 0)       rc = cmd_sync(&app, sub_argc, sub_argv);
    else if (strcmp(cmd, "backup") == 0) {
        bmk_err be = bmk_backup_full(app.db, app.db_path);
        if (be == BMK_OK) { bmk_backup_cleanup(); printf("备份完成: %s\n", app.backups_dir); rc = 0; }
        else { fprintf(stderr, "备份失败（%s）\n", bmk_err_str(be)); rc = 1; }
    }
    else if (strcmp(cmd, "set") == 0) {
        if (sub_argc < 2) { fprintf(stderr, "用法: set <key> <value>\n"); rc = 2; }
        else { rc = bmk_settings_set_str(app.db, sub_argv[0], sub_argv[1]) == BMK_OK ? 0 : 1; }
    }
    else if (strcmp(cmd, "get") == 0) {
        if (sub_argc < 1) { fprintf(stderr, "用法: get <key>\n"); rc = 2; }
        else {
            char *v = bmk_settings_get_str(app.db, sub_argv[0], NULL);
            printf("%s\n", v ? v : "(未设置)");
            free(v);
            rc = 0;
        }
    }
    else { usage(); rc = 2; }

    bmk_app_shutdown(&app);
    return rc;
}
