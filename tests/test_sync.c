/*
 * test_sync.c - 云同步本地 mock（FR-18）
 *
 * 用本地 bare git 仓库模拟远端：验证 push（导出→提交→推送→写 meta）
 * 与 restore（拉取→FR-07 导入）的完整确定性序列，不依赖 GitHub 网络。
 * git 不在 PATH 时跳过。
 */
#include "test_util.h"
#include "bmk_sync.h"
#include "bmk_db.h"
#include "bmk_process.h"
#include "bmk_netscape.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <windows.h>
#  include <process.h>
#  define SEP '\\'
#else
#  include <unistd.h>
#  include <dirent.h>
#  include <sys/stat.h>
#  define SEP '/'
#endif

#define P_BUF 1024

static int g_seq = 0;

static void make_temp_dir(char *buf, size_t sz, const char *tag)
{
    const char *base = getenv("TEMP");
    if (!base || !*base) base = "/tmp";
#ifdef _WIN32
    snprintf(buf, sz, "%s%cbmk_%s_%d_%d", base, SEP, tag, (int)_getpid(), g_seq++);
#else
    snprintf(buf, sz, "%s%cbmk_%s_%d_%d", base, SEP, tag, (int)getpid(), g_seq++);
#endif
    bmk_mkdirs(buf);
}

static void remove_tree(const char *path)
{
    if (!path || !*path) return;
#ifdef _WIN32
    char pat[P_BUF];
    snprintf(pat, sizeof pat, "%s\\*", path);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
                continue;
            char sub[P_BUF];
            snprintf(sub, sizeof sub, "%s\\%s", path, fd.cFileName);
            /* git 松散对象/包文件会置只读位，先清属性再删，否则 DeleteFile 失败 */
            SetFileAttributesA(sub, FILE_ATTRIBUTE_NORMAL);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                remove_tree(sub);
            else
                DeleteFileA(sub);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL);
    RemoveDirectoryA(path);
#else
    DIR *d = opendir(path);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
            char sub[P_BUF];
            snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
            struct stat st;
            if (stat(sub, &st) == 0 && S_ISDIR(st.st_mode)) remove_tree(sub);
            else remove(sub);
        }
        closedir(d);
    }
    rmdir(path);
#endif
}

static void set_git_env(void)
{
#ifdef _WIN32
    _putenv("GIT_AUTHOR_NAME=bmk-test");
    _putenv("GIT_AUTHOR_EMAIL=bmk-test@example.com");
    _putenv("GIT_COMMITTER_NAME=bmk-test");
    _putenv("GIT_COMMITTER_EMAIL=bmk-test@example.com");
#else
    setenv("GIT_AUTHOR_NAME", "bmk-test", 1);
    setenv("GIT_AUTHOR_EMAIL", "bmk-test@example.com", 1);
    setenv("GIT_COMMITTER_NAME", "bmk-test", 1);
    setenv("GIT_COMMITTER_EMAIL", "bmk-test@example.com", 1);
#endif
}

static void test_meta_roundtrip(void)
{
    char dir[P_BUF];
    make_temp_dir(dir, sizeof dir, "meta");
    char dbp[P_BUF];
    snprintf(dbp, sizeof dbp, "%s%cmeta.db", dir, SEP);

    bmk_db *db = NULL;
    CHECK_EQ(bmk_db_open(dbp, &db), BMK_OK, "打开临时库");
    if (!db) { remove_tree(dir); return; }

    CHECK_EQ(bmk_sync_meta_set(db, BMK_SYNC_LAST_SYNC, "2026-08-08T00:00:00Z"),
             BMK_OK, "写入 meta");
    char *v = bmk_sync_meta_get(db, BMK_SYNC_LAST_SYNC);
    CHECK_STR(v, "2026-08-08T00:00:00Z", "读回 meta");
    free(v);

    CHECK_EQ(bmk_sync_meta_set(db, BMK_SYNC_LAST_SYNC, "x"), BMK_OK, "覆盖 meta");
    v = bmk_sync_meta_get(db, BMK_SYNC_LAST_SYNC);
    CHECK_STR(v, "x", "覆盖后读回");
    free(v);

    bmk_db_close(db);
    remove_tree(dir);
}

static int64_t add_bookmark(bmk_db *db, const char *title, const char *url)
{
    bmk_bookmark b;
    memset(&b, 0, sizeof b);
    b.title = bmk_strdup(title);
    b.url   = bmk_strdup(url);
    b.valid = BMK_VALID_UNKNOWN;
    b.status_code = -1;
    bmk_err e = bmk_bookmark_insert(db, &b);
    int64_t id = (e == BMK_OK) ? b.id : 0;
    bmk_bookmark_free(&b);
    CHECK(id > 0, "插入 %s", url);
    return id;
}

static void test_github_push_restore(void)
{
    char *git = bmk_which("git");
    if (!git) { printf("SKIP test_sync: git not found in PATH\n"); return; }

    char root[P_BUF];
    make_temp_dir(root, sizeof root, "sync");
    char remote[P_BUF], data_dir[P_BUF], sync_dir[P_BUF], dbp[P_BUF];
    snprintf(remote,   sizeof remote,   "%s%cremote.git", root, SEP);
    snprintf(data_dir, sizeof data_dir, "%s%cdata", root, SEP);
    snprintf(sync_dir, sizeof sync_dir, "%s%csync", root, SEP);
    snprintf(dbp,      sizeof dbp,      "%s%cbookmarks.db", data_dir, SEP);
    bmk_mkdirs(data_dir);
    bmk_mkdirs(sync_dir);

    const char *init[] = { git, "init", "--bare", "-b", "main", remote, NULL };
    CHECK_EQ(bmk_proc_run(init, NULL, NULL, NULL), BMK_OK, "创建 bare 仓库");

    bmk_db *db = NULL;
    CHECK_EQ(bmk_db_open(dbp, &db), BMK_OK, "打开数据库");
    if (!db) { free(git); remove_tree(root); return; }
    add_bookmark(db, "测试一", "https://example.com/aaa");
    add_bookmark(db, "测试二", "https://example.org/bbb");

    /* git 使用正斜杠路径 */
    char repo_fwd[P_BUF];
    size_t rl = strlen(remote);
    for (size_t i = 0; i < rl; i++)
        repo_fwd[i] = (remote[i] == '\\') ? '/' : remote[i];
    repo_fwd[rl] = '\0';

    bmk_github_cfg cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.repo     = repo_fwd;
    cfg.branch   = "main";
    cfg.sync_dir = sync_dir;
    cfg.db_path  = dbp;
    cfg.push_db  = true;

    bmk_err e = bmk_sync_github_push(db, &cfg, NULL, NULL);
    CHECK_EQ(e, BMK_OK, "GitHub push（本地仓库）");
    if (e != BMK_OK) { bmk_db_close(db); free(git); remove_tree(root); return; }

    char *last = bmk_sync_meta_get(db, BMK_SYNC_LAST_SYNC);
    CHECK(last && *last, "push 后写入 last_sync");
    free(last);
    char *rm = bmk_sync_meta_get(db, BMK_SYNC_GITHUB_REMOTE);
    CHECK_STR(rm, repo_fwd, "记录远端地址");
    free(rm);

    /* 远端应包含导出的 bookmarks.html */
    char *out = NULL;
    int code = -1;
    const char *cat[] = { git, "--git-dir", remote, "cat-file", "-p",
                          "HEAD:bookmarks.html", NULL };
    CHECK_EQ(bmk_proc_run(cat, NULL, &out, &code), BMK_OK, "读取远端文件");
    CHECK(code == 0 && out && strstr(out, "https://example.com/aaa") != NULL,
          "远端包含导出内容");
    free(out);

    /* restore 到新库 */
    bmk_db_close(db);
    char walp[P_BUF], shmp[P_BUF];
    snprintf(walp, sizeof walp, "%s-wal", dbp);
    snprintf(shmp, sizeof shmp, "%s-shm", dbp);
    remove(dbp);
    remove(walp);
    remove(shmp);

    bmk_db *db2 = NULL;
    CHECK_EQ(bmk_db_open(dbp, &db2), BMK_OK, "重建库");
    if (!db2) { free(git); remove_tree(root); return; }
    e = bmk_sync_github_restore(db2, &cfg, NULL, NULL);
    CHECK_EQ(e, BMK_OK, "GitHub restore（本地仓库）");
    if (e == BMK_OK) {
        bmk_bookmark **list = NULL;
        size_t n = 0;
        CHECK_EQ(bmk_bookmark_query(db2, NULL, BMK_SORT_CREATED_AT, true,
                                    &list, &n), BMK_OK, "查询恢复结果");
        CHECK_EQ(n, 2, "恢复 2 条");
        bool found_a = false, found_b = false;
        for (size_t i = 0; i < n; i++) {
            if (list[i]->url && strstr(list[i]->url, "example.com/aaa")) found_a = true;
            if (list[i]->url && strstr(list[i]->url, "example.org/bbb")) found_b = true;
        }
        CHECK(found_a && found_b, "两条 URL 都恢复");
        bmk_bookmark_list_free(list, n);
    }
    bmk_db_close(db2);

    free(git);
    remove_tree(root);
}

/* QA 第 1 项：git commit 因缺身份配置失败时，push 必须中止（返回错误），
 * 不得静默成功导致远端未更新。 */
static void clear_git_identity(void)
{
#ifdef _WIN32
    _putenv("GIT_AUTHOR_NAME=");
    _putenv("GIT_AUTHOR_EMAIL=");
    _putenv("GIT_COMMITTER_NAME=");
    _putenv("GIT_COMMITTER_EMAIL=");
    _putenv("GIT_CONFIG_GLOBAL=C:\\bmk_no_such_config");
    _putenv("GIT_CONFIG_SYSTEM=C:\\bmk_no_such_system");
#else
    unsetenv("GIT_AUTHOR_NAME");
    unsetenv("GIT_AUTHOR_EMAIL");
    unsetenv("GIT_COMMITTER_NAME");
    unsetenv("GIT_COMMITTER_EMAIL");
    setenv("GIT_CONFIG_GLOBAL", "/nonexistent-bmk-config", 1);
    setenv("GIT_CONFIG_SYSTEM", "/nonexistent-bmk-system", 1);
#endif
}

static void restore_git_identity(void)
{
#ifdef _WIN32
    _putenv("GIT_CONFIG_GLOBAL=");
    _putenv("GIT_CONFIG_SYSTEM=");
#else
    unsetenv("GIT_CONFIG_GLOBAL");
    unsetenv("GIT_CONFIG_SYSTEM");
#endif
    set_git_env();
}

static void to_forward_slashes(char *dst, size_t cap, const char *src)
{
    size_t l = strlen(src);
    if (l >= cap) l = cap - 1;
    for (size_t i = 0; i < l; i++)
        dst[i] = (src[i] == '\\') ? '/' : src[i];
    dst[l] = '\0';
}

static void test_github_commit_failure(void)
{
    char *git = bmk_which("git");
    if (!git) { printf("SKIP test_sync: git not found in PATH\n"); return; }

    char root[P_BUF];
    make_temp_dir(root, sizeof root, "cmt");
    char remote[P_BUF], data_dir[P_BUF], sync_dir[P_BUF], dbp[P_BUF];
    snprintf(remote,   sizeof remote,   "%s%cremote.git", root, SEP);
    snprintf(data_dir, sizeof data_dir, "%s%cdata", root, SEP);
    snprintf(sync_dir, sizeof sync_dir, "%s%csync", root, SEP);
    snprintf(dbp,      sizeof dbp,      "%s%cbookmarks.db", data_dir, SEP);
    bmk_mkdirs(data_dir);
    bmk_mkdirs(sync_dir);

    const char *init[] = { git, "init", "--bare", "-b", "main", remote, NULL };
    CHECK_EQ(bmk_proc_run(init, NULL, NULL, NULL), BMK_OK, "创建 bare 仓库");

    bmk_db *db = NULL;
    CHECK_EQ(bmk_db_open(dbp, &db), BMK_OK, "打开数据库");
    if (!db) { free(git); remove_tree(root); return; }
    add_bookmark(db, "commit-fail", "https://example.com/cf");

    char repo_fwd[P_BUF];
    to_forward_slashes(repo_fwd, sizeof repo_fwd, remote);

    bmk_github_cfg cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.repo     = repo_fwd;
    cfg.branch   = "main";
    cfg.sync_dir = sync_dir;

    /* 第一次 push（有 git 身份）→ 远端与本地各有一个提交，后续 push 若 commit
     * 失败，远端已无新内容可推，会报 "Everything up-to-date" 并返回成功。 */
    CHECK_EQ(bmk_sync_github_push(db, &cfg, NULL, NULL), BMK_OK, "首次 push 成功");
    add_bookmark(db, "commit-fail-2", "https://example.com/cf2");

    /* 清掉 git 身份：第二次 push 时 commit 失败（缺 user.name/email）。
     * 旧实现会忽略该失败、push 静默成功（远端未更新）；修复后必须返回错误。 */
    clear_git_identity();
    bmk_err e = bmk_sync_github_push(db, &cfg, NULL, NULL);
    restore_git_identity();
    CHECK(e != BMK_OK, "缺 git 身份时 push 应返回失败而非静默成功");

    bmk_db_close(db);
    free(git);
    remove_tree(root);
}

/* QA 第 3 项：首次恢复（sync_dir 无 .git）应自动 clone 后再导入，而非直接失败 */
static void test_github_restore_first_time(void)
{
    char *git = bmk_which("git");
    if (!git) { printf("SKIP test_sync: git not found in PATH\n"); return; }

    char root[P_BUF];
    make_temp_dir(root, sizeof root, "rst");
    char remote[P_BUF], data_dir[P_BUF], sync_dir[P_BUF], dbp[P_BUF];
    snprintf(remote,   sizeof remote,   "%s%cremote.git", root, SEP);
    snprintf(data_dir, sizeof data_dir, "%s%cdata", root, SEP);
    snprintf(sync_dir, sizeof sync_dir, "%s%csync", root, SEP);
    snprintf(dbp,      sizeof dbp,      "%s%cbookmarks.db", data_dir, SEP);
    bmk_mkdirs(data_dir);
    bmk_mkdirs(sync_dir);

    const char *init[] = { git, "init", "--bare", "-b", "main", remote, NULL };
    CHECK_EQ(bmk_proc_run(init, NULL, NULL, NULL), BMK_OK, "创建 bare 仓库");

    bmk_db *db = NULL;
    CHECK_EQ(bmk_db_open(dbp, &db), BMK_OK, "打开数据库");
    if (!db) { free(git); remove_tree(root); return; }
    add_bookmark(db, "远端一", "https://example.com/r1");
    add_bookmark(db, "远端二", "https://example.org/r2");

    char repo_fwd[P_BUF];
    to_forward_slashes(repo_fwd, sizeof repo_fwd, remote);

    bmk_github_cfg cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.repo     = repo_fwd;
    cfg.branch   = "main";
    cfg.sync_dir = sync_dir;

    /* 先 push 一次，让远端有内容 */
    CHECK_EQ(bmk_sync_github_push(db, &cfg, NULL, NULL), BMK_OK, "首次 push 填充远端");
    bmk_db_close(db);

    /* 删除本地 sync 目录，模拟“新机器”首次恢复 */
    remove_tree(sync_dir);
    bmk_mkdirs(sync_dir);

    char walp[P_BUF], shmp[P_BUF];
    snprintf(walp, sizeof walp, "%s-wal", dbp);
    snprintf(shmp, sizeof shmp, "%s-shm", dbp);
    remove(dbp);
    remove(walp);
    remove(shmp);

    bmk_db *db2 = NULL;
    CHECK_EQ(bmk_db_open(dbp, &db2), BMK_OK, "重建空库");
    if (!db2) { free(git); remove_tree(root); return; }
    bmk_err e = bmk_sync_github_restore(db2, &cfg, NULL, NULL);
    CHECK_EQ(e, BMK_OK, "首次恢复应自动 clone 后导入");
    if (e == BMK_OK) {
        bmk_bookmark **list = NULL;
        size_t n = 0;
        CHECK_EQ(bmk_bookmark_query(db2, NULL, BMK_SORT_CREATED_AT, true,
                                    &list, &n), BMK_OK, "查询恢复结果");
        CHECK_EQ(n, 2, "恢复 2 条");
        bmk_bookmark_list_free(list, n);
    }
    bmk_db_close(db2);
    free(git);
    remove_tree(root);
}

/* QA 遗留项：push 后 .git/config 的 remote URL 不得包含 PAT（凭据只经
 * http.extraheader 单次注入，不落盘）。 */
static void test_pat_not_persisted(void)
{
    char *git = bmk_which("git");
    if (!git) { printf("SKIP test_sync: git not found in PATH\n"); return; }

    char root[P_BUF];
    make_temp_dir(root, sizeof root, "pat");
    char remote[P_BUF], data_dir[P_BUF], sync_dir[P_BUF], dbp[P_BUF];
    snprintf(remote,   sizeof remote,   "%s%cremote.git", root, SEP);
    snprintf(data_dir, sizeof data_dir, "%s%cdata", root, SEP);
    snprintf(sync_dir, sizeof sync_dir, "%s%csync", root, SEP);
    snprintf(dbp,      sizeof dbp,      "%s%cbookmarks.db", data_dir, SEP);
    bmk_mkdirs(data_dir);
    bmk_mkdirs(sync_dir);

    const char *init[] = { git, "init", "--bare", "-b", "main", remote, NULL };
    CHECK_EQ(bmk_proc_run(init, NULL, NULL, NULL), BMK_OK, "创建 bare 仓库");

    bmk_db *db = NULL;
    CHECK_EQ(bmk_db_open(dbp, &db), BMK_OK, "打开数据库");
    if (!db) { free(git); remove_tree(root); return; }
    add_bookmark(db, "pat-test", "https://example.com/pat");

    char repo_fwd[P_BUF];
    to_forward_slashes(repo_fwd, sizeof repo_fwd, remote);

    const char *dummy_pat = "ghp_dummy_pat_12345678";
    bmk_github_cfg cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.repo     = repo_fwd;
    cfg.branch   = "main";
    cfg.sync_dir = sync_dir;
    cfg.pat      = dummy_pat;

    CHECK_EQ(bmk_sync_github_push(db, &cfg, NULL, NULL), BMK_OK,
             "带 PAT 的 push（本地仓库）");

    /* 读取 .git/config，remote URL 必须是干净地址，不得含 PAT */
    char cfgpath[P_BUF];
    snprintf(cfgpath, sizeof cfgpath, "%s%c.git%cconfig", sync_dir, SEP, SEP);
    FILE *f = fopen(cfgpath, "rb");
    CHECK(f != NULL, "读取 .git/config");
    if (f) {
        char buf[8192];
        size_t rd = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[rd] = '\0';
        CHECK(strstr(buf, dummy_pat) == NULL,
              ".git/config 不得包含 PAT");
        CHECK(strstr(buf, repo_fwd) != NULL,
              ".git/config remote url 为干净地址");
    }

    bmk_db_close(db);
    free(git);
    remove_tree(root);
}

static void test_invalid_args(void)
{
    CHECK_EQ(bmk_sync_github_push(NULL, NULL, NULL, NULL),
             BMK_ERR_INVALID_ARG, "NULL 参数拒绝");
    CHECK_EQ(bmk_sync_github_restore(NULL, NULL, NULL, NULL),
             BMK_ERR_INVALID_ARG, "restore NULL 拒绝");

    char dir[P_BUF];
    make_temp_dir(dir, sizeof dir, "arg");
    char dbp[P_BUF];
    snprintf(dbp, sizeof dbp, "%s%carg.db", dir, SEP);
    bmk_db *db = NULL;
    if (bmk_db_open(dbp, &db) == BMK_OK && db) {
        bmk_github_cfg cfg;
        memset(&cfg, 0, sizeof cfg);
        cfg.sync_dir = dir;
        CHECK_EQ(bmk_sync_github_push(db, &cfg, NULL, NULL),
                 BMK_ERR_INVALID_ARG, "缺 repo 拒绝");
        cfg.repo = "https://example.invalid/repo.git";
        cfg.sync_dir = NULL;
        CHECK_EQ(bmk_sync_github_push(db, &cfg, NULL, NULL),
                 BMK_ERR_INVALID_ARG, "缺 sync_dir 拒绝");
        bmk_db_close(db);
    }
    remove_tree(dir);
}

int main(void)
{
    set_git_env();
    test_meta_roundtrip();
    test_github_push_restore();
    test_github_commit_failure();
    test_github_restore_first_time();
    test_pat_not_persisted();
    test_invalid_args();
    return test_report("同步（本地 git mock）");
}
