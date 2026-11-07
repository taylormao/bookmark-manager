# Graph Report - .  (2026-08-08)

## Corpus Check
- cluster-only mode — file stats not available

## Summary
- 663 nodes · 2228 edges · 20 communities
- Extraction: 69% EXTRACTED · 31% INFERRED · 0% AMBIGUOUS · INFERRED: 697 edges (avg confidence: 0.8)
- Token cost: 0 input · 0 output

## Community Hubs (Navigation)
- Community 0
- Community 1
- Community 2
- Community 3
- Community 4
- Community 5
- Community 6
- Community 7
- Community 8
- Community 9
- Community 10
- Community 11
- Community 12
- Community 13

## God Nodes (most connected - your core abstractions)
1. `bmk_strdup()` - 57 edges
2. `bmk_err_str()` - 31 edges
3. `bmk_bookmark_query()` - 28 edges
4. `bmk_app_startup()` - 23 edges
5. `bmk_merge_group()` - 23 edges
6. `bmk_import_html_buffer()` - 23 edges
7. `bmk_sync_github_push()` - 23 edges
8. `main()` - 22 edges
9. `bmk_url_normalize()` - 22 edges
10. `bmk_export_html_string()` - 21 edges

## Surprising Connections (you probably didn't know these)
- `main()` --calls--> `bmk_backup_set_dir()`  [INFERRED]
  tests/test_merge.c → src/core/bmk_backup.c
- `test_import_basic()` --calls--> `bmk_bookmark_list_free()`  [INFERRED]
  tests/test_netscape.c → src/core/bmk_db.c
- `total_count()` --calls--> `bmk_bookmark_list_free()`  [INFERRED]
  tests/test_netscape.c → src/core/bmk_db.c
- `test_categories()` --calls--> `bmk_category_list_free()`  [INFERRED]
  tests/test_db.c → src/core/bmk_db.c
- `test_filter()` --calls--> `bmk_tag_list_free()`  [INFERRED]
  tests/test_db.c → src/core/bmk_db.c

## Import Cycles
- None detected.

## Communities (20 total, 0 thin omitted)

### Community 0 - "Community 0"
Cohesion: 0.08
Nodes (87): bmk_dup_group, bmk_stats, sqlite3, sqlite3_stmt, cmd_check(), bmk_backup_delete_bookmarks(), bmk_merge_group(), append_order_by() (+79 more)

### Community 1 - "Community 1"
Cohesion: 0.06
Nodes (83): BmkConfirmCb, GArray, GdkPaintable, GSimpleAction, GtkCellRendererToggle, GtkGestureClick, GtkSearchEntry, GtkStringList (+75 more)

### Community 2 - "Community 2"
Cohesion: 0.09
Nodes (75): bmk_github_cfg, bmk_s3_cfg, bmk_sync_log_cb, CURL, bmk_app_dir_writable(), bmk_app_migrate_data_dir(), bmk_app_startup(), bmk_err (+67 more)

### Community 3 - "Community 3"
Cohesion: 0.08
Nodes (66): GParamSpec, GtkDropDown, ProxyCtx, SettingsCtx, bmk_proxy_save(), bmk_db, bmk_settings_get_bool(), bmk_settings_get_int() (+58 more)

### Community 4 - "Community 4"
Cohesion: 0.09
Nodes (49): bmk_node, bmk_proc, bmk_rotation, bmk_vless, node_slot, cmd_proxy(), bmk_port_is_free(), bmk_proc_alive() (+41 more)

### Community 5 - "Community 5"
Cohesion: 0.08
Nodes (47): DedupeCtx, ImportCtx, ProgressUI, bmk_dialog_check(), bmk_dialog_dedupe(), bmk_dialog_export(), bmk_dialog_import(), bmk_bookmark (+39 more)

### Community 6 - "Community 6"
Cohesion: 0.10
Nodes (42): bmk_tag, CatCtx, EditCtx, GtkCellRendererText, GtkEditable, bmk_tag_free(), bmk_tag_list(), bmk_tag_list_free() (+34 more)

### Community 7 - "Community 7"
Cohesion: 0.10
Nodes (23): bmk_validate_cb, bmk_validate_opts, CURLcode, pool_state, probe_result, app_free_paths(), bmk_app_shutdown(), bmk_app (+15 more)

### Community 8 - "Community 8"
Cohesion: 0.16
Nodes (29): bmk_app, bmk_bookmark, bmk_category, bmk_check_result, bmk_db, check_progress(), cmd_add(), cmd_categories() (+21 more)

### Community 9 - "Community 9"
Cohesion: 0.16
Nodes (28): bmk_import_opts, bmk_import_report, import_ctx, bmk_gbk_to_utf8(), bmk_import_html_buffer(), bmk_import_html_file(), bmk_import_opts_init(), bmk_import_report_free() (+20 more)

### Community 10 - "Community 10"
Cohesion: 0.12
Nodes (26): bmk_fetch_opts, bmk_fetch_result, GumboNode, html_info, http_response, bmk_default_icon(), bmk_fetch_opts_init(), bmk_fetch_page() (+18 more)

### Community 11 - "Community 11"
Cohesion: 0.14
Nodes (21): query_param(), bmk_strndup(), bmk_url_normalize(), bmk_url_percent_decode(), bmk_url_remove_dot_segments(), bmk_url_resolve(), bmk_err, extract_scheme() (+13 more)

### Community 12 - "Community 12"
Cohesion: 0.29
Nodes (13): bmk_log_level, bmk_log_get_level(), bmk_log_init(), bmk_log_redact(), bmk_log_set_level(), bmk_log_shutdown(), bmk_logf(), bmk_err (+5 more)

### Community 13 - "Community 13"
Cohesion: 0.18
Nodes (7): CliOpts, bmk_i18n_init(), gpointer, GtkApplication, main(), on_activate(), parse_args()

## Suggested Questions
_Questions this graph is uniquely positioned to answer:_

- **Why does `bmk_strdup()` connect `Community 2` to `Community 0`, `Community 1`, `Community 3`, `Community 4`, `Community 6`, `Community 7`, `Community 8`, `Community 9`, `Community 10`, `Community 11`, `Community 12`?**
  _High betweenness centrality (0.204) - this node is a cross-community bridge._
- **Why does `bmk_err_str()` connect `Community 8` to `Community 0`, `Community 1`, `Community 2`, `Community 3`, `Community 4`, `Community 5`, `Community 6`, `Community 9`, `Community 11`, `Community 13`?**
  _High betweenness centrality (0.087) - this node is a cross-community bridge._
- **Why does `bmk_settings_get_str()` connect `Community 3` to `Community 2`, `Community 4`, `Community 7`, `Community 8`, `Community 13`?**
  _High betweenness centrality (0.038) - this node is a cross-community bridge._
- **Are the 53 inferred relationships involving `bmk_strdup()` (e.g. with `cmd_add()` and `cmd_list()`) actually correct?**
  _`bmk_strdup()` has 53 INFERRED edges - model-reasoned connections that need verification._
- **Are the 29 inferred relationships involving `bmk_err_str()` (e.g. with `cmd_add()` and `cmd_delete()`) actually correct?**
  _`bmk_err_str()` has 29 INFERRED edges - model-reasoned connections that need verification._
- **Should `Community 0` be split into smaller, more focused modules?**
  _Cohesion score 0.08274894810659186 - nodes in this community are weakly interconnected._
- **Should `Community 1` be split into smaller, more focused modules?**
  _Cohesion score 0.06253585771658061 - nodes in this community are weakly interconnected._