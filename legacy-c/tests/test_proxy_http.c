/*
 * test_proxy_http.c - 代理配置分层校验（FR-17 补充）
 *
 * 用本地 mock 代理验证：参数校验、TCP 不可达、SOCKS5/HTTP 握手成功、
 * 协议不匹配、认证失败、407；以及端点串的凭据百分号编码。
 * 全程不依赖外网、不写数据库、不启动 xray。
 */
#include "test_util.h"
#include "bmk_proxy.h"
#include "bmk_process.h"

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
   static unsigned __stdcall mock_thread(void *a);
#  define THREAD_START(t, fn, a) \
       ((*(t) = (HANDLE)_beginthreadex(NULL, 0, fn, a, 0, NULL)) != NULL)
#  define THREAD_JOIN(t) WaitForSingleObject((t), INFINITE)
#else
#  include <pthread.h>
#  include <sys/socket.h>
#  include <sys/select.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
   typedef int net_sock;
#  define INVALID_S (-1)
#  define CLOSE_SOCK(s) close(s)
   typedef pthread_t thr_t;
   static void *mock_thread(void *a);
#  define THREAD_START(t, fn, a) (pthread_create((t), NULL, fn, a) == 0)
#  define THREAD_JOIN(t) pthread_join((t), NULL)
#endif

typedef enum {
    MOCK_SOCKS5_OK,
    MOCK_SOCKS5_NEED_AUTH,
    MOCK_SOCKS5_AUTH_REJECT,
    MOCK_HTTP_200,
    MOCK_HTTP_407,
    MOCK_GARBAGE,
} mock_mode;

typedef struct {
    net_sock  lsock;
    int       port;
    thr_t     thr;
    mock_mode mode;
} mock_srv;

static void mock_loop(mock_srv *s)
{
    /* 只服务一个连接；3s 内无连接则退出，保证 join 不会挂住 */
    fd_set rf;
    FD_ZERO(&rf);
    FD_SET(s->lsock, &rf);
    struct timeval tv;
    tv.tv_sec = 3;
    tv.tv_usec = 0;
    if (select((int)s->lsock + 1, &rf, NULL, NULL, &tv) <= 0) return;

    net_sock c = accept(s->lsock, NULL, NULL);
    if (c == INVALID_S) return;

    char buf[2048];
    int n = recv(c, buf, (int)sizeof(buf) - 1, 0);
    (void)n;                       /* 请求内容不参与判定，只需读走 */

    switch (s->mode) {
    case MOCK_SOCKS5_OK: {
        unsigned char rep[2] = { 0x05, 0x00 };
        send(c, (const char *)rep, 2, 0);
        break;
    }
    case MOCK_SOCKS5_NEED_AUTH: {
        unsigned char rep[2] = { 0x05, 0x02 };
        send(c, (const char *)rep, 2, 0);
        break;
    }
    case MOCK_SOCKS5_AUTH_REJECT: {
        unsigned char rep[2] = { 0x05, 0x02 };
        send(c, (const char *)rep, 2, 0);
        char sub[512];
        int m = recv(c, sub, (int)sizeof(sub), 0);
        (void)m;
        unsigned char bad[2] = { 0x01, 0x01 };
        send(c, (const char *)bad, 2, 0);
        break;
    }
    case MOCK_HTTP_200: {
        const char *r = "HTTP/1.1 200 Connection established\r\n\r\n";
        send(c, r, (int)strlen(r), 0);
        break;
    }
    case MOCK_HTTP_407: {
        const char *r = "HTTP/1.1 407 Proxy Authentication Required\r\n\r\n";
        send(c, r, (int)strlen(r), 0);
        break;
    }
    case MOCK_GARBAGE:
    default: {
        const char *r = "NOT-A-PROXY\r\n";
        send(c, r, (int)strlen(r), 0);
        break;
    }
    }
    CLOSE_SOCK(c);
}

#ifdef _WIN32
static unsigned __stdcall mock_thread(void *a)
{
    mock_loop((mock_srv *)a);
    return 0;
}
#else
static void *mock_thread(void *a)
{
    mock_loop((mock_srv *)a);
    return NULL;
}
#endif

static int mock_start(mock_srv *s, mock_mode mode)
{
    memset(s, 0, sizeof *s);
    s->mode = mode;
    s->lsock = socket(AF_INET, SOCK_STREAM, 0);
    if (s->lsock == INVALID_S) return -1;

    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(s->lsock, (struct sockaddr *)&a, sizeof a) != 0) return -1;
    if (listen(s->lsock, 4) != 0) return -1;

#ifdef _WIN32
    int alen = (int)sizeof a;
#else
    socklen_t alen = sizeof a;
#endif
    if (getsockname(s->lsock, (struct sockaddr *)&a, &alen) != 0) return -1;
    s->port = ntohs(a.sin_port);
    return THREAD_START(&s->thr, mock_thread, s) ? 0 : -1;
}

static void mock_join(mock_srv *s)
{
    THREAD_JOIN(s->thr);
    CLOSE_SOCK(s->lsock);
}

/* ------------------------------------------------------------------ */

static void test_invalid_args(void)
{
    bmk_proxy_check chk;
    CHECK(bmk_proxy_endpoint_check(NULL, NULL, &chk) == BMK_ERR_INVALID_ARG,
          "NULL cfg 应返回 INVALID_ARG");

    bmk_node n;
    memset(&n, 0, sizeof n);
    n.type = BMK_NODE_HTTP;
    CHECK(bmk_proxy_endpoint_check(&n, NULL, NULL) == BMK_ERR_INVALID_ARG,
          "NULL out 应返回 INVALID_ARG");

    char host[] = "127.0.0.1";
    n.host = host;
    n.port = 0;
    CHECK(bmk_proxy_endpoint_check(&n, NULL, &chk) == BMK_ERR_INVALID_ARG,
          "端口 0 应被拒绝");
    CHECK(chk.state[BMK_PCHK_TCP] == BMK_PCHK_FAIL, "TCP 阶段应标记失败");
    CHECK(!chk.usable, "参数非法时 usable=false");

    n.port = 70000;
    CHECK(bmk_proxy_endpoint_check(&n, NULL, &chk) == BMK_ERR_INVALID_ARG,
          "端口越界应被拒绝");

    n.port = 8080;
    n.type = BMK_NODE_VLESS;
    n.local_port = 0;
    CHECK(bmk_proxy_endpoint_check(&n, NULL, &chk) == BMK_ERR_UNSUPPORTED,
          "未启动的 VLESS 应返回 UNSUPPORTED");
    CHECK(chk.state[BMK_PCHK_TCP] == BMK_PCHK_SKIP, "VLESS 未启动不应做 TCP 探测");
}

static void test_tcp_unreachable(void)
{
    int port = 0;
    for (int p = 45000; p < 45100; p++) {
        if (bmk_port_is_free(p)) { port = p; break; }
    }
    if (!port) {
        TEST_SKIP("test_proxy_http: no free port for TCP probe");
        return;
    }

    bmk_node n;
    memset(&n, 0, sizeof n);
    char host[] = "127.0.0.1";
    n.type = BMK_NODE_HTTP;
    n.host = host;
    n.port = port;

    bmk_proxy_check chk;
    bmk_err e = bmk_proxy_endpoint_check(&n, NULL, &chk);
    CHECK(e == BMK_ERR_NETWORK, "TCP 不通应返回 NETWORK");
    CHECK(chk.state[BMK_PCHK_TCP] == BMK_PCHK_FAIL, "TCP 阶段 FAIL");
    CHECK(chk.state[BMK_PCHK_HANDSHAKE] == BMK_PCHK_SKIP, "握手阶段应 SKIP");
    CHECK(chk.state[BMK_PCHK_TARGET] == BMK_PCHK_SKIP, "test_url 为空时目标站 SKIP");
    CHECK(!chk.usable, "TCP 不通时 usable=false");
}

static void test_endpoint_encoding(void)
{
    bmk_node n;
    memset(&n, 0, sizeof n);
    char host[] = "127.0.0.1";
    n.type = BMK_NODE_HTTP;
    n.enabled = true;
    n.host = host;
    n.port = 8080;
    n.username = "u@ser";
    n.password = "p:/#x";

    int64_t id = 0;
    CHECK(bmk_proxy_node_add(&n, &id) == BMK_OK, "添加 HTTP 节点");
    bmk_proxy_node_start(id);

    char *ep = bmk_proxy_next_endpoint("https://example.com/");
    CHECK(ep != NULL, "应能取得端点串");
    if (ep) {
        CHECK(strncmp(ep, "http://", 7) == 0, "HTTP 协议前缀");
        CHECK(strstr(ep, "%40") != NULL, "用户名 @ 应被百分号编码");
        CHECK(strstr(ep, "%23") != NULL, "密码 # 应被百分号编码");
        CHECK(strchr(ep, '#') == NULL, "端点串中不应残留裸 #");
        CHECK(strstr(ep, ":8080") != NULL, "端口应出现在端点串");
        free(ep);
    }
}

typedef struct {
    mock_mode     mode;
    bmk_node_type type;
    bool          has_cred;
    bool          expect_usable;
    bmk_err       expect_err;
    const char   *label;
} hs_case;

static void run_handshake_case(const hs_case *tc)
{
    mock_srv srv;
    if (mock_start(&srv, tc->mode) != 0) {
        TEST_SKIP("test_proxy_http: mock server unavailable (%s)", tc->label);
        return;
    }

    bmk_node n;
    memset(&n, 0, sizeof n);
    char host[] = "127.0.0.1";
    n.type = tc->type;
    n.host = host;
    n.port = srv.port;
    if (tc->has_cred) { n.username = "user"; n.password = "pass"; }

    bmk_proxy_check chk;
    bmk_proxy_endpoint_check(&n, NULL, &chk);
    mock_join(&srv);

    CHECK(chk.state[BMK_PCHK_TCP] == BMK_PCHK_OK, "%s: TCP 应连通", tc->label);
    CHECK(chk.usable == tc->expect_usable, "%s: usable 判定", tc->label);
    CHECK(chk.state[BMK_PCHK_TARGET] == BMK_PCHK_SKIP,
          "%s: 未传 test_url 时目标站 SKIP", tc->label);
    if (!tc->expect_usable) {
        CHECK(chk.err == tc->expect_err, "%s: 错误码应正确分层", tc->label);
        CHECK(chk.state[BMK_PCHK_HANDSHAKE] == BMK_PCHK_FAIL,
              "%s: 握手阶段应 FAIL", tc->label);
    } else {
        CHECK(chk.state[BMK_PCHK_HANDSHAKE] == BMK_PCHK_OK,
              "%s: 握手阶段应 OK", tc->label);
        CHECK(chk.err == BMK_OK, "%s: 成功时 err=OK", tc->label);
    }
}

static void test_handshake_cases(void)
{
    static const hs_case cases[] = {
        { MOCK_SOCKS5_OK,          BMK_NODE_SOCKS5, false, true,  BMK_OK,            "socks5 ok" },
        { MOCK_HTTP_200,           BMK_NODE_HTTP,   false, true,  BMK_OK,            "http CONNECT ok" },
        { MOCK_SOCKS5_NEED_AUTH,   BMK_NODE_SOCKS5, false, false, BMK_ERR_AUTH,      "socks5 needs auth" },
        { MOCK_SOCKS5_AUTH_REJECT, BMK_NODE_SOCKS5, true,  false, BMK_ERR_AUTH,      "socks5 auth reject" },
        { MOCK_HTTP_407,           BMK_NODE_HTTP,   true,  false, BMK_ERR_AUTH,      "http 407" },
        { MOCK_GARBAGE,            BMK_NODE_HTTP,   false, false, BMK_ERR_NETWORK,   "http garbage reply" },
        { MOCK_SOCKS5_OK,          BMK_NODE_HTTP,   false, false, BMK_ERR_NETWORK,   "http vs socks5 mismatch" },
        { MOCK_HTTP_200,           BMK_NODE_SOCKS5, false, false, BMK_ERR_NETWORK,   "socks5 vs http mismatch" },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++)
        run_handshake_case(&cases[i]);
}

int main(void)
{
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        TEST_SKIP("test_proxy_http: WSAStartup failed");
        return test_report("proxy check");
    }
#endif
    test_invalid_args();
    test_tcp_unreachable();
    test_endpoint_encoding();
    test_handshake_cases();
#ifdef _WIN32
    WSACleanup();
#endif
    return test_report("proxy check");
}
