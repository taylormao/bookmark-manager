/*
 * bmk_i18n.c - 界面字符串表（需求 5.6）
 */
#include "bmk_i18n.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

typedef struct { const char *key, *zh, *en; } entry;

/* 保持按 key 分组，便于查漏。 */
static const entry TABLE[] = {
    /* 应用与窗口 */
    { "app.title",            "网址收藏夹",                 "Bookmark Manager" },
    { "app.untitled",         "无标题",                     "Untitled" },

    /* 工具栏（FR-20 第 2 条） */
    { "tb.add",               "添加",                       "Add" },
    { "tb.import",            "导入",                       "Import" },
    { "tb.export",            "导出",                       "Export" },
    { "tb.check",             "校验",                       "Check" },
    { "tb.dedupe",            "查重",                       "Duplicates" },
    { "tb.sync",              "同步",                       "Sync" },
    { "tb.proxy",             "代理设置",                   "Proxy" },
    { "tb.settings",          "设置",                       "Settings" },
    { "tb.search_hint",       "搜索标题或网址",             "Search title or URL" },
    { "tb.filter",            "筛选",                       "Filter" },

    /* 左侧树（FR-10 第 1 条） */
    { "tree.all",             "全部",                       "All" },
    { "tree.uncategorized",   "未分类",                     "Uncategorized" },
    { "tree.blocked",         "待复测",                     "Needs recheck" },
    { "tree.invalid",         "无效",                       "Invalid" },
    { "tree.categories",      "分类",                       "Categories" },
    { "tree.new_category",    "新建分类",                   "New category" },
    { "tree.rename",          "重命名",                     "Rename" },
    { "tree.move",            "移动",                       "Move" },
    { "tree.delete",          "删除",                       "Delete" },

    /* 列表列头（FR-20 第 2 条） */
    { "col.select",           "选择",                       "Select" },
    { "col.icon",             "图标",                       "Icon" },
    { "col.title",            "标题",                       "Title" },
    { "col.url",              "网址",                       "URL" },
    { "col.category",         "分类",                       "Category" },
    { "col.tags",             "标签",                       "Tags" },
    { "col.valid",            "有效性",                     "Status" },
    { "col.status_code",      "状态码",                     "Code" },
    { "col.created",          "添加时间",                   "Added" },
    { "col.last_checked",     "最近校验",                   "Last checked" },

    /* 有效性显示（FR-20 第 6 条） */
    { "valid.good",           "有效",                       "Valid" },
    { "valid.bad",            "无效",                       "Invalid" },
    { "valid.unknown",        "未校验",                     "Unchecked" },
    { "valid.blocked",        "待复测",                     "Needs recheck" },

    /* 空态（FR-20 第 3 条） */
    { "empty.title",          "还没有收藏任何网址",         "No bookmarks yet" },
    { "empty.body",           "点击“添加”手动录入，或“导入”浏览器书签文件。",
                              "Click Add to create one, or Import a browser bookmark file." },

    /* 右键菜单（FR-13） */
    { "ctx.open",             "打开",                       "Open" },
    { "ctx.copy_url",         "复制网址",                   "Copy URL" },
    { "ctx.edit",             "编辑",                       "Edit" },
    { "ctx.delete",           "删除",                       "Delete" },
    { "ctx.move_to",          "移动到分类",                 "Move to category" },
    { "ctx.add_tag",          "添加标签",                   "Add tag" },
    { "ctx.remove_tag",       "移除标签",                   "Remove tag" },
    { "ctx.check_now",        "立即校验",                   "Check now" },
    { "ctx.refetch",          "重新抓取标题与图标",         "Refetch title and icon" },
    { "ctx.merge",            "合并网址",                   "Merge bookmarks" },
    { "ctx.batch_tag",        "批量添加标签",               "Add tags" },
    { "ctx.batch_category",   "批量设置分类",               "Set category" },

    /* 筛选面板（FR-12） */
    { "filter.url",           "网址包含",                   "URL contains" },
    { "filter.domain",        "域名",                       "Domain" },
    { "filter.valid",         "有效性",                     "Status" },
    { "filter.status_min",    "状态码 ≥",                   "Code ≥" },
    { "filter.status_max",    "状态码 ≤",                   "Code ≤" },
    { "filter.tags",          "标签",                       "Tags" },
    { "filter.clear",         "清除筛选",                   "Clear filters" },
    { "filter.vf_all",        "全部",                       "All" },
    { "filter.vf_valid",      "有效",                       "Valid" },
    { "filter.vf_invalid",    "无效",                       "Invalid" },
    { "filter.vf_unchecked",  "未校验",                     "Unchecked" },
    { "filter.vf_blocked",    "待复测",                     "Needs recheck" },

    /* 批量操作（FR-13） */
    { "batch.tags_title",     "批量添加标签",               "Add tags" },
    { "batch.tags_prompt",    "标签（逗号分隔）",           "Tags (comma separated)" },
    { "batch.tags_done",      "已为 %d 条记录添加标签",     "Added tags to %d bookmarks" },
    { "batch.cat_title",      "批量设置分类",               "Set category" },
    { "batch.cat_done",       "已更新 %d 条记录的分类",     "Updated category for %d bookmarks" },
    { "merge.confirm",        "将合并 %d 条网址：保留最早添加的一条，其余删除且标签取并集。是否继续？",
                              "Merge %d bookmarks? The earliest is kept; others are deleted and tags merged." },
    { "merge.done",           "已合并 %d 条记录",           "Merged %d bookmarks" },
    { "merge.not_same",       "所选记录的归一化网址不一致，无法合并",
                              "Selected bookmarks have different normalized URLs" },

    /* 添加/编辑对话框（FR-02 / FR-05） */
    { "edit.add_title",       "添加网址",                   "Add bookmark" },
    { "edit.edit_title",      "编辑网址",                   "Edit bookmark" },
    { "edit.url",             "网址",                       "URL" },
    { "edit.title",           "标题",                       "Title" },
    { "edit.category",        "分类",                       "Category" },
    { "edit.tags",            "标签（逗号分隔）",           "Tags (comma separated)" },
    { "edit.fetch",           "抓取标题与图标",             "Fetch title and icon" },
    { "edit.fetching",        "抓取中…",                    "Fetching..." },
    { "edit.fetch_failed",    "抓取失败，可手动填写标题",   "Fetch failed; you can enter a title manually" },
    { "edit.dup_warn",        "已存在相同网址：",           "A bookmark with the same URL exists: " },
    { "edit.invalid_url",     "网址不合法",                 "Invalid URL" },
    { "edit.url_required",    "请填写网址",                 "URL is required" },

    /* 导入（FR-07） */
    { "import.title",         "导入书签",                   "Import bookmarks" },
    { "import.file",          "文件",                       "File" },
    { "import.encoding",      "编码",                       "Encoding" },
    { "import.enc_auto",      "自动检测",                   "Auto detect" },
    { "import.target",        "导入到分类",                 "Import into" },
    { "import.keep_tree",     "保留原分类层级",             "Keep original folder tree" },
    { "import.dup",           "重复处理",                   "Duplicates" },
    { "import.dup_skip",      "跳过重复",                   "Skip duplicates" },
    { "import.dup_import",    "全部导入并标记",             "Import all and mark" },
    { "import.running",       "正在导入…",                  "Importing..." },
    { "import.done",          "导入完成",                   "Import finished" },
    { "import.summary",       "新增 %lld 条，跳过 %lld 条，失败 %lld 条",
                              "%lld added, %lld skipped, %lld failed" },

    /* 导出（FR-08） */
    { "export.title",         "导出书签",                   "Export bookmarks" },
    { "export.scope_all",     "导出全部",                   "Export all" },
    { "export.scope_filtered","仅导出当前筛选结果",         "Export current filter only" },
    { "export.done",          "导出完成",                   "Export finished" },

    /* 校验（FR-14） */
    { "check.title",          "批量校验",                   "Check links" },
    { "check.scope",          "范围",                       "Scope" },
    { "check.scope_selected", "选中项",                     "Selected" },
    { "check.scope_filtered", "当前筛选结果",               "Current filter" },
    { "check.scope_all",      "全部",                       "All" },
    { "check.running",        "正在校验…",                  "Checking..." },
    { "check.done",           "校验完成",                   "Check finished" },
    { "check.summary",        "共 %lld 条：有效 %lld，无效 %lld，待复测 %lld",
                              "%lld checked: %lld valid, %lld invalid, %lld need recheck" },

    /* 查重与合并（FR-15 / FR-16） */
    { "dedupe.title",         "重复网址",                   "Duplicate bookmarks" },
    { "dedupe.none",          "未发现重复网址",             "No duplicates found" },
    { "dedupe.group",         "第 %d 组（%d 条）",          "Group %d (%d items)" },
    { "dedupe.merge",         "合并本组",                   "Merge group" },
    { "dedupe.merge_all",     "合并全部",                   "Merge all" },
    { "dedupe.merged",        "已合并 %d 组",               "Merged %d groups" },
    { "dedupe.confirm",       "合并后将保留最早添加的一条，其余记录会被删除，标签取并集。是否继续？",
                              "The earliest bookmark is kept; others are deleted and tags are merged. Continue?" },

    /* 删除（FR-06） */
    { "delete.confirm_one",   "确定删除选中的 1 条网址吗？",
                              "Delete the selected bookmark?" },
    { "delete.confirm_many",  "确定删除选中的 %d 条网址吗？",
                              "Delete %d selected bookmarks?" },
    { "delete.backup_note",   "删除前会自动生成 HTML 备份。",
                              "An HTML backup is created before deletion." },
    { "delete.backup_failed", "备份失败，已取消删除。",     "Backup failed; deletion canceled." },
    { "delete.done",          "已删除 %d 条",               "%d bookmarks deleted" },

    /* 分类对话框（FR-10） */
    { "cat.new_title",        "新建分类",                   "New category" },
    { "cat.rename_title",     "重命名分类",                 "Rename category" },
    { "cat.move_title",       "移动分类",                   "Move category" },
    { "cat.name",             "名称",                       "Name" },
    { "cat.parent",           "父分类",                     "Parent" },
    { "cat.root",             "（根级）",                   "(Top level)" },
    { "cat.conflict",         "同级下已存在同名分类",       "A sibling with this name already exists" },
    { "cat.cycle",            "不能移动到自身或其子分类下", "Cannot move into itself or its descendants" },
    { "cat.delete_confirm",   "删除分类将递归删除子分类，其下 %lld 条网址将归入“未分类”。是否继续？",
                              "Deleting removes subcategories; %lld bookmarks become uncategorized. Continue?" },

    /* 标签（FR-11） */
    { "tag.title",            "标签管理",                   "Tags" },
    { "tag.name",             "标签",                       "Tag" },
    { "tag.usage",            "使用数",                     "Used" },
    { "tag.rename",           "重命名",                     "Rename" },
    { "tag.merge",            "合并到…",                    "Merge into..." },
    { "tag.delete",           "删除",                       "Delete" },
    { "tag.delete_confirm",   "删除标签只会解除与网址的关联，不会删除网址。是否继续？",
                              "Deleting a tag only removes its associations. Continue?" },

    /* 设置（FR-01 / FR-03 / FR-14 / FR-21） */
    { "set.title",            "设置",                       "Settings" },
    { "set.tab_general",      "常规",                       "General" },
    { "set.tab_network",      "网络",                       "Network" },
    { "set.tab_sync",         "同步",                       "Sync" },
    { "set.tab_backup",       "备份",                       "Backup" },
    { "set.data_dir",         "数据目录",                   "Data directory" },
    { "set.change_dir",       "更改…",                      "Change..." },
    { "set.language",         "语言",                       "Language" },
    { "set.lang_zh",          "中文",                       "Chinese" },
    { "set.lang_en",          "English",                    "English" },
    { "set.restart_hint",     "语言更改将在下次启动后生效。",
                              "Language takes effect after restart." },
    { "set.icon_service",     "允许使用外部图标服务",       "Allow external icon services" },
    { "set.icon_service_hint","关闭后不会向第三方发起任何请求。",
                              "When off, no third-party requests are made." },
    { "set.timeout",          "超时（秒）",                 "Timeout (seconds)" },
    { "set.concurrency",      "并发数",                     "Concurrency" },
    { "set.retry",            "重试次数",                   "Retries" },
    { "set.interval",         "同域最小间隔（毫秒）",       "Min interval per host (ms)" },
    { "set.use_proxy",        "校验时使用代理",             "Use proxy when checking" },
    { "set.xray_path",        "xray 可执行文件",            "xray executable" },
    { "set.git_path",         "git 可执行文件",             "git executable" },
    { "set.backup_now",       "立即备份",                   "Back up now" },
    { "set.backup_cleanup",   "清理旧备份",                 "Clean old backups" },
    { "set.open_backup_dir",  "打开备份目录",               "Open backup folder" },
    { "set.restore",          "从备份恢复…",                "Restore from backup..." },

    /* 代理（FR-17） */
    { "proxy.title",          "代理节点",                   "Proxy nodes" },
    { "proxy.add",            "添加节点",                   "Add node" },
    { "proxy.import_uri",     "粘贴 vless:// 链接",         "Paste vless:// link" },
    { "proxy.name",           "名称",                       "Name" },
    { "proxy.type",           "类型",                       "Type" },
    { "proxy.enabled",        "启用",                       "Enabled" },
    { "proxy.port",           "本地端口",                   "Local port" },
    { "proxy.latency",        "延迟",                       "Latency" },
    { "proxy.test",           "测试",                       "Test" },
    { "proxy.test_all",       "测试全部",                   "Test all" },
    { "proxy.start",          "启动",                       "Start" },
    { "proxy.stop",           "停止",                       "Stop" },
    { "proxy.rotation",       "轮换策略",                   "Rotation" },
    { "proxy.rot_round",      "轮询",                       "Round robin" },
    { "proxy.rot_random",     "随机",                       "Random" },
    { "proxy.rot_failover",   "故障转移",                   "Failover" },
    { "proxy.rot_by_host",    "按域名固定",                 "Sticky by host" },
    { "proxy.parse_failed",   "链接解析失败",               "Failed to parse link" },
    { "proxy.xray_missing",   "未找到 xray 可执行文件，请在设置中指定路径。",
                              "xray executable not found; set its path in Settings." },
    { "proxy.available",      "可用",                       "Available" },
    { "proxy.unavailable",    "不可用",                     "Unavailable" },
    { "proxy.untested",       "未测试",                     "Untested" },

    /* 同步（FR-18 / FR-19） */
    { "sync.title",           "云端同步",                   "Cloud sync" },
    { "sync.provider",        "同步方式",                   "Provider" },
    { "sync.github",          "GitHub 仓库",                "GitHub repository" },
    { "sync.s3",              "S3 兼容存储",                "S3-compatible storage" },
    { "sync.repo",            "仓库（owner/repo）",         "Repository (owner/repo)" },
    { "sync.branch",          "分支",                       "Branch" },
    { "sync.auth",            "认证方式",                   "Authentication" },
    { "sync.pat",             "访问令牌",                   "Access token" },
    { "sync.ssh",             "SSH 密钥",                   "SSH key" },
    { "sync.push_db",         "同时上传数据库文件",         "Upload database file too" },
    { "sync.endpoint",        "Endpoint",                   "Endpoint" },
    { "sync.region",          "Region",                     "Region" },
    { "sync.bucket",          "Bucket",                     "Bucket" },
    { "sync.prefix",          "前缀",                       "Prefix" },
    { "sync.access_key",      "Access Key",                 "Access Key" },
    { "sync.secret_key",      "Secret Key",                 "Secret Key" },
    { "sync.path_style",      "使用 path-style 寻址",       "Use path-style addressing" },
    { "sync.push",            "上传",                       "Push" },
    { "sync.restore",         "从云端恢复",                 "Restore" },
    { "sync.running",         "正在同步…",                  "Syncing..." },
    { "sync.done",            "同步完成",                   "Sync finished" },
    { "sync.failed",          "同步失败",                   "Sync failed" },
    { "sync.never",           "从未同步",                   "Never synced" },
    { "sync.last",            "最近同步：%s",               "Last sync: %s" },
    { "sync.restore_confirm", "从云端恢复会覆盖本地数据，覆盖前会自动备份。是否继续？",
                              "Restoring overwrites local data (a backup is made first). Continue?" },
    { "sync.secret_masked",   "（已保存，留空则不修改）",   "(saved; leave blank to keep)" },

    /* 状态栏（FR-22） */
    { "status.total",         "总数 %lld",                  "%lld total" },
    { "status.filtered",      "当前 %lld",                  "%lld shown" },
    { "status.invalid",       "无效 %lld",                  "%lld invalid" },
    { "status.blocked",       "待复测 %lld",                "%lld need recheck" },
    { "status.selected",      "已选 %d",                    "%d selected" },

    /* 通用按钮与提示 */
    { "btn.ok",               "确定",                       "OK" },
    { "btn.cancel",           "取消",                       "Cancel" },
    { "btn.close",            "关闭",                       "Close" },
    { "btn.save",             "保存",                       "Save" },
    { "btn.apply",            "应用",                       "Apply" },
    { "btn.browse",           "浏览…",                      "Browse..." },
    { "btn.retry",            "重试",                       "Retry" },
    { "btn.continue",         "继续",                       "Continue" },
    { "btn.stop",             "停止",                       "Stop" },
    { "btn.select_all",       "全选",                       "Select all" },
    { "btn.deselect_all",     "取消全选",                   "Deselect all" },

    { "msg.error",            "出错了",                     "Error" },
    { "msg.warning",          "警告",                       "Warning" },
    { "msg.confirm",          "请确认",                     "Confirm" },
    { "msg.saved",            "已保存",                     "Saved" },
    { "msg.copied",           "已复制到剪贴板",             "Copied to clipboard" },
    { "msg.canceled",         "已取消",                     "Canceled" },
    { "msg.no_selection",     "请先选择网址",               "Select bookmarks first" },
    { "msg.db_corrupt",       "检测到数据库损坏，已新建空库。可从备份目录导入 HTML 恢复数据。",
                              "Database was corrupted; a new one was created. Restore from an HTML backup." },
    { "msg.dir_not_writable", "所选目录不可写，请重新选择。",
                              "The selected folder is not writable. Choose another one." },
};

static const char *g_lang = "zh";

void bmk_i18n_init(const char *lang)
{
    g_lang = (lang && strcmp(lang, "en") == 0) ? "en" : "zh";
}

const char *bmk_i18n_lang(void) { return g_lang; }

const char *bmk_i18n_get(const char *key)
{
    if (!key) return "";
    bool en = (g_lang[0] == 'e');
    for (size_t i = 0; i < sizeof TABLE / sizeof TABLE[0]; i++) {
        if (strcmp(TABLE[i].key, key) == 0)
            return en ? TABLE[i].en : TABLE[i].zh;
    }
    return key;   /* 未登记：原样返回，便于定位遗漏 */
}
