/*
 * test_validate.c - 批量校验本地 mock（FR-14）
 *
 * 用本地 HTTP 服务器模拟 200/404/403/429/405降级GET/302重定向/超时，
 * 以及“HEAD 返回错误但 GET 正常”的误杀场景，
 * 端到端跑 bmk_validate_run 并断言写库结果。
 */
#include "test_util.h"
#include "bmk_db.h"
#include "bmk_validate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <winsock2.h>
#  include <windows.h>
#  include <process.h>
   typedef SOCKET net_sock;
#  define INVALID_S INVALID_SOCKET
#  define CLOSE_SOCK(s) closesocket(s)
   typedef HANDLE thr_t;
   static unsigned __stdcall server_thread(void *a);
#  define THREAD_START(t, fn, a) \
      ((*(t) = (HANDLE)_beginthreadex(NULL, 0, fn, a, 0, NULL)) != NULL)
#  define THREAD_JOIN(t) WaitForSingleObject((t), INFINITE)
#  define SLEEP_MS(ms) Sleep(ms)
#else
#  include <pthread.h>
#  include <unistd.h>
#  include <dirent.h>
#  include <sys/stat.h>
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
   typedef int net_sock;
#  define INVALID_S (-1)
#  define CLOSE_SOCK(s) close(s)
   typedef pthread_t thr_t;
   static void *server_thread(void *a);
#  define THREAD_START(t, fn, a) (pthread_create((t), NULL, fn, a) == 0)
#  define THREAD_JOIN(t) pthread_join((t), NULL)
#  define SLEEP_MS(ms) usleep((useconds_t)(ms) * 1000)
#endif

#define P_BUF 1024

typedef struct {
    net_sock      lsock;
    int           port;
    volatile int  stop;
    thr_t         thr;
} http_srv;

static void http_server_loop(http_srv *s)
{
    for (;;) {
        net_sock c = accept(s->lsock, NULL, NULL);
        if (c == INVALID_S || s->stop) { if (c != INVALID_S) CLOSE_SOCK(c); break; }

        char buf[8192];
        int n = recv(c, buf, (int)sizeof(buf) - 1, 0);
        if (n > 0) {
            buf[n] = '\0';
            char method[8] = {0}, path[512] = {0};
            sscanf(buf, "%7s %511s", method, path);

            const char *status = "404 Not Found";
            const char *loc = NULL;
            if (strcmp(path, "/ok") == 0) status = "200 OK";
            else if (strcmp(path, "/missing") == 0) status = "404 Not Found";
            else if (strcmp(path, "/blocked") == 0) status = "403 Forbidden";
            else if (strcmp(path, "/ratelimit") == 0) status = "429 Too Many Requests";
            else if (strcmp(path, "/redir") == 0) { status = "302 Found"; loc = "/ok"; }
            else if (strcmp(path, "/needget") == 0) {
                status = (strcmp(method, "HEAD") == 0)
                         ? "405 Method Not Allowed" : "200 OK";
            } else if (strcmp(path, "/head404") == 0) {
                status = (strcmp(method, "HEAD") == 0) ? "404 Not Found" : "200 OK";
            } else if (strcmp(path, "/head403") == 0) {
                status = (strcmp(method, "HEAD") == 0) ? "403 Forbidden" : "200 OK";
            } else if (strcmp(path, "/head500") == 0) {
                status = (strcmp(method, "HEAD") == 0) ? "500 Internal Server Error" : "200 OK";
            } else if (strcmp(path, "/slow") == 0) {
                SLEEP_MS(3000);
                status = "200 OK";
            }

            char resp[2048];
            int len;
            if (loc)
                len = snprintf(resp, sizeof resp,
                    "HTTP/1.1 %s\r\nContent-Length: 0\r\nLocation: %s\r\n\r\n",
                    status, loc);
            else
                len = snprintf(resp, sizeof resp,
                    "HTTP/1.1 %s\r\nContent-Length: 0\r\n\r\n", status);
            send(c, resp, len, 0);
        }
        CLOSE_SOCK(c);
    }
}

#ifdef _WIN32
static unsigned __stdcall server_thread(void *a)
{
    http_server_loop((http_srv *)a);
    return 0;
}
#else
static void *server_thread(void *a)
{
    http_server_loop((http_srv *)a);
    return NULL;
}
#endif

static int http_srv_start(http_srv *s)
{
    memset(s, 0, sizeof *s);
    s->lsock = socket(AF_INET, SOCK_STREAM, 0);
    if (s->lsock == INVALID_S) return -1;

    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(s->lsock, (struct sockaddr *)&a, sizeof a) != 0) return -1;
    if (listen(s->lsock, 8) != 0) return -1;
    int alen = (int)sizeof a;
    if (getsockname(s->lsock, (struct sockaddr *)&a, &alen) != 0) return -1;
    s->port = ntohs(a.sin_port);
    if (!THREAD_START(&s->thr, server_thread, s)) return -1;
    return 0;
}

static void http_srv_stop(http_srv *s)
{
    s->stop = 1;
    CLOSE_SOCK(s->lsock);
    THREAD_JOIN(s->thr);
}

static int g_seq = 0;

static void make_temp_dir(char *buf, size_t sz, const char *tag)
{
    const char *base = getenv("TEMP");
    if (!base || !*base) base = "/tmp";
#ifdef _WIN32
    snprintf(buf, sz, "%s\\bmk_%s_%d_%d", base, tag, (int)_getpid(), g_seq++);
#else
    snprintf(buf, sz, "%s/bmk_%s_%d_%d", base, tag, (int)getpid(), g_seq++);
#endif
    bmk_mkdirs(buf);
}

static void remove_tree(const char *path)
{
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

static int64_t add_url(bmk_db *db, int port, const char *path)
{
    char url[512];
    snprintf(url, sizeof url, "http://127.0.0.1:%d%s", port, path);

    bmk_bookmark b;
    memset(&b, 0, sizeof b);
    b.title = bmk_strdup(url);
    b.url   = bmk_strdup(url);
    b.valid = BMK_VALID_UNKNOWN;
    b.status_code = -1;
    bmk_err e = bmk_bookmark_insert(db, &b);
    int64_t id = (e == BMK_OK) ? b.id : 0;
    bmk_bookmark_free(&b);
    CHECK(id > 0, "插入 %s", path);
    return id;
}

static void check_result(bmk_db *db, int64_t id, int valid, int status,
                         const char *note, const char *tag)
{
    bmk_bookmark *b = NULL;
    CHECK_EQ(bmk_bookmark_get(db, id, &b), BMK_OK, "%s: 读取记录", tag);
    if (!b) return;
    CHECK_EQ(b->valid, valid, "%s: valid", tag);
    CHECK_EQ(b->status_code, status, "%s: status", tag);
    CHECK_STR(b->check_note ? b->check_note : "", note ? note : "", "%s: note", tag);
    bmk_bookmark_free(b);
    free(b);
}

static void test_validate_pipeline(void)
{
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("SKIP test_validate: WSAStartup failed\n");
        return;
    }
#endif

    /* 故意设置不可达的环境代理：验证“直连”不会被环境变量带偏（误杀修复）。
     * 修复前请求会走死代理而全部失败，修复后仍应直连本地 mock。 */
#ifdef _WIN32
    _putenv("HTTP_PROXY=http://127.0.0.1:1");
    _putenv("http_proxy=http://127.0.0.1:1");
    _putenv("HTTPS_PROXY=http://127.0.0.1:1");
    _putenv("https_proxy=http://127.0.0.1:1");
    _putenv("ALL_PROXY=http://127.0.0.1:1");
    _putenv("all_proxy=http://127.0.0.1:1");
#else
    setenv("HTTP_PROXY", "http://127.0.0.1:1", 1);
    setenv("http_proxy", "http://127.0.0.1:1", 1);
    setenv("HTTPS_PROXY", "http://127.0.0.1:1", 1);
    setenv("https_proxy", "http://127.0.0.1:1", 1);
    setenv("ALL_PROXY", "http://127.0.0.1:1", 1);
    setenv("all_proxy", "http://127.0.0.1:1", 1);
#endif

    http_srv srv;
    if (http_srv_start(&srv) != 0) {
        printf("SKIP test_validate: cannot start mock server\n");
        return;
    }

    char dir[P_BUF];
    make_temp_dir(dir, sizeof dir, "val");
    char dbp[P_BUF];
#ifdef _WIN32
    snprintf(dbp, sizeof dbp, "%s\\val.db", dir);
#else
    snprintf(dbp, sizeof dbp, "%s/val.db", dir);
#endif
    bmk_db *db = NULL;
    if (bmk_db_open(dbp, &db) != BMK_OK || !db) {
        http_srv_stop(&srv);
        remove_tree(dir);
        printf("SKIP test_validate: cannot open db\n");
        return;
    }

    int64_t ids[10];
    ids[0] = add_url(db, srv.port, "/ok");
    ids[1] = add_url(db, srv.port, "/missing");
    ids[2] = add_url(db, srv.port, "/blocked");
    ids[3] = add_url(db, srv.port, "/ratelimit");
    ids[4] = add_url(db, srv.port, "/redir");
    ids[5] = add_url(db, srv.port, "/needget");
    /* /slow 会阻塞 mock 服务器 3 秒，必须放最后，避免拖垮后续请求 */
    ids[6] = add_url(db, srv.port, "/head404");
    ids[7] = add_url(db, srv.port, "/head403");
    ids[8] = add_url(db, srv.port, "/head500");
    ids[9] = add_url(db, srv.port, "/slow");

    bmk_validate_opts o;
    bmk_validate_opts_init(&o);
    o.concurrency = 1;
    o.per_host_limit = 1;
    o.timeout_s = 1;
    o.connect_timeout_s = 1;
    o.retry = 0;
    o.interval_ms = 0;
    o.jitter_ms = 0;

    bmk_err e = bmk_validate_run(db, ids, 10, &o, NULL, NULL);
    CHECK_EQ(e, BMK_OK, "校验运行完成（无用户取消）");

    check_result(db, ids[0], BMK_VALID_GOOD,  200, BMK_NOTE_OK,            "/ok");
    check_result(db, ids[1], BMK_VALID_BAD,   404, "http_404",             "/missing");
    check_result(db, ids[2], BMK_VALID_UNKNOWN, 403, BMK_NOTE_BLOCKED,     "/blocked");
    check_result(db, ids[3], BMK_VALID_UNKNOWN, 429, BMK_NOTE_BLOCKED,     "/ratelimit");
    check_result(db, ids[4], BMK_VALID_GOOD,  200, BMK_NOTE_OK,            "/redir");
    check_result(db, ids[5], BMK_VALID_GOOD,  200, BMK_NOTE_OK,            "/needget");
    check_result(db, ids[6], BMK_VALID_GOOD,  200, BMK_NOTE_OK,            "/head404");
    check_result(db, ids[7], BMK_VALID_GOOD,  200, BMK_NOTE_OK,            "/head403");
    check_result(db, ids[8], BMK_VALID_GOOD,  200, BMK_NOTE_OK,            "/head500");
    check_result(db, ids[9], BMK_VALID_BAD,   0,   BMK_NOTE_TIMEOUT,       "/slow");

    bmk_db_close(db);
    remove_tree(dir);
    http_srv_stop(&srv);
#ifdef _WIN32
    WSACleanup();
#endif
}

int main(void)
{
    test_validate_pipeline();
    return test_report("批量校验（本地 HTTP mock）");
}
