/*
 * test_backup.c - 备份清理策略（FR-21 第 5 条）
 *
 * 全量快照 bookmarks_<ts>.html + bookmarks_<ts>.db 成对出现，
 * 同一时间戳的两份文件算一个快照：保留最近 20 个快照，其余整组清理；
 * 自动备份（deleted_/merged_/remote_/premigrate_）仅保留 7 天内。
 */
#include "test_util.h"
#include "bmk_backup.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifdef _WIN32
#  include <windows.h>
#  include <direct.h>
#  include <process.h>
#  include <sys/utime.h>
#  define SEP '\\'
#  define U_TIME _utime
#  define U_TIMBUF _utimbuf
#else
#  include <dirent.h>
#  include <unistd.h>
#  include <utime.h>
#  define SEP '/'
#  define U_TIME utime
#  define U_TIMBUF utimbuf
#endif

#define P_BUF 1024

static int g_seq = 0;

static void make_temp_dir(char *buf, size_t sz)
{
    const char *base = getenv("TEMP");
    if (!base || !*base) base = "/tmp";
#ifdef _WIN32
    snprintf(buf, sz, "%s%cbmk_backup_%d_%d", base, SEP, (int)_getpid(), g_seq++);
#else
    snprintf(buf, sz, "%s%cbmk_backup_%d_%d", base, SEP, (int)getpid(), g_seq++);
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

static void write_dummy(const char *path, const char *content)
{
    FILE *f = fopen(path, "wb");
    if (f) { fputs(content, f); fclose(f); }
}

static void set_mtime(const char *path, time_t t)
{
    struct U_TIMBUF ut;
    ut.actime = t;
    ut.modtime = t;
    U_TIME(path, &ut);
}

/* 统计目录内以 prefix 开头（且后缀为 suffix，suffix 为 NULL 时不限制）的文件数 */
static size_t count_files(const char *dir, const char *prefix, const char *suffix)
{
    size_t c = 0;
#ifdef _WIN32
    char pat[P_BUF];
    snprintf(pat, sizeof pat, "%s\\%s*", dir, prefix);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (suffix && !strstr(fd.cFileName, suffix)) continue;
        c++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, prefix, strlen(prefix)) != 0) continue;
        if (suffix && !strstr(e->d_name, suffix)) continue;
        c++;
    }
    closedir(d);
#endif
    return c;
}

static void test_cleanup_keeps_20_snapshots(void)
{
    char dir[P_BUF];
    make_temp_dir(dir, sizeof dir);
    CHECK_EQ(bmk_backup_set_dir(dir), BMK_OK, "设置备份目录");

    time_t now = time(NULL);

    /* 25 个全量快照：每个快照 html + db 成对，mtime 依次递增 */
    for (int i = 0; i < 25; i++) {
        char h[P_BUF], d[P_BUF];
        snprintf(h, sizeof h, "%s%cbookmarks_snap%02d.html", dir, SEP, i);
        snprintf(d, sizeof d, "%s%cbookmarks_snap%02d.db", dir, SEP, i);
        write_dummy(h, "<html/>");
        write_dummy(d, "sqlite");
        /* 越晚创建 mtime 越大：i=24 最新，i=0 最旧 */
        set_mtime(h, now - (25 - i) * 60);
        set_mtime(d, now - (25 - i) * 60 + 1);
    }
    /* 旧自动备份（30 天前）应删除；新自动备份（1 小时前）应保留 */
    char olda[P_BUF], oldb[P_BUF], fresh[P_BUF];
    snprintf(olda, sizeof olda, "%s%cdeleted_old1.html", dir, SEP);
    snprintf(oldb, sizeof oldb, "%s%cmerged_old1.html", dir, SEP);
    snprintf(fresh, sizeof fresh, "%s%cdeleted_fresh1.html", dir, SEP);
    write_dummy(olda, "a");
    write_dummy(oldb, "b");
    write_dummy(fresh, "f");
    set_mtime(olda, now - 30 * 86400);
    set_mtime(oldb, now - 30 * 86400);
    set_mtime(fresh, now - 3600);

    CHECK_EQ(bmk_backup_cleanup(), BMK_OK, "清理执行成功");

    /* 保留 20 个快照 = 40 个文件（html 20 + db 20） */
    CHECK_EQ((long long)count_files(dir, "bookmarks_", ".html"), 20LL,
             "保留 20 份 html");
    CHECK_EQ((long long)count_files(dir, "bookmarks_", ".db"), 20LL,
             "保留 20 份 db");
    CHECK_EQ((long long)count_files(dir, "bookmarks_", NULL), 40LL,
             "快照文件总数 40");
    /* 旧自动备份被清，新的保留 */
    CHECK_EQ((long long)count_files(dir, "deleted_", NULL), 1LL,
             "旧 deleted 清理、新 deleted 保留");
    CHECK_EQ((long long)count_files(dir, "merged_", NULL), 0LL,
             "旧 merged 清理");

    /* 保留的是最新 20 个快照：snap05..snap24 应存在，snap00..snap04 应删除 */
    char probe[P_BUF];
    int kept_new = 0, kept_old = 0;
    for (int i = 0; i < 25; i++) {
        snprintf(probe, sizeof probe, "%s%cbookmarks_snap%02d.html", dir, SEP, i);
        struct stat st;
        if (stat(probe, &st) == 0) {
            if (i >= 5) kept_new++;
            else        kept_old++;
        }
    }
    CHECK_EQ(kept_new, 20, "最新 20 个快照保留");
    CHECK_EQ(kept_old, 0, "最旧 5 个快照删除");

    remove_tree(dir);
}

int main(void)
{
    test_cleanup_keeps_20_snapshots();
    return test_report("备份清理（按快照计数）");
}
