/*
 * bmk_proxy.c - 代理节点管理实现（FR-17 / 附录 C）
 *
 * 隐私：日志中不打印 UUID 与完整 vless 链接，仅输出节点名与脱敏地址
 * （需求 5.3 第 1 条）。
 */
#include "bmk_proxy.h"
#include "bmk_db.h"
#include "bmk_log.h"
#include "bmk_settings.h"
#include "bmk_url.h"
#include "bmk_process.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>
#include <cJSON.h>

#define MAX_RESTARTS 3          /* 每会话最多 3 次/节点（FR-17 第 3 条） */
#define PORT_BASE    10810

/* ------------------------------------------------------------------ */
/* vless:// 解析                                                       */
/* ------------------------------------------------------------------ */

void bmk_vless_free(bmk_vless *v)
{
    if (!v) return;
    free(v->uuid); free(v->host); free(v->encryption); free(v->flow);
    free(v->security); free(v->type); free(v->path); free(v->ws_host);
    free(v->sni); free(v->fp); free(v->alpn); free(v->pbk);
    free(v->sid); free(v->spx); free(v->name);
    memset(v, 0, sizeof(*v));
}

/* 从 query 串取参数值（已 URL 解码）；未找到返回 NULL */
static char *query_param(const char *query, const char *name)
{
    if (!query) return NULL;
    size_t nlen = strlen(name);
    const char *p = query;
    while (*p) {
        const char *amp = strchr(p, '&');
        size_t itemlen = amp ? (size_t)(amp - p) : strlen(p);
        const char *eq = memchr(p, '=', itemlen);
        if (eq) {
            size_t klen = (size_t)(eq - p);
            if (klen == nlen && strncmp(p, name, nlen) == 0) {
                char *raw = bmk_strndup(eq + 1, itemlen - klen - 1);
                if (!raw) return NULL;
                char *dec = bmk_url_percent_decode(raw);
                free(raw);
                return dec;
            }
        }
        if (!amp) break;
        p = amp + 1;
    }
    return NULL;
}

bmk_err bmk_vless_parse(const char *uri, bmk_vless *out)
{
    if (!uri || !out) return BMK_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));

    if (strncmp(uri, "vless://", 8) != 0) return BMK_ERR_PARSE;
    const char *body = uri + 8;

    /* 名称：# 之后 */
    const char *hash = strchr(body, '#');
    if (hash) out->name = bmk_url_percent_decode(hash + 1);

    size_t body_len = hash ? (size_t)(hash - body) : strlen(body);
    char *main_part = bmk_strndup(body, body_len);
    if (!main_part) return BMK_ERR_NOMEM;

    /* query：? 之后 */
    char *qmark = strchr(main_part, '?');
    char *query = NULL;
    if (qmark) { *qmark = '\0'; query = qmark + 1; }

    /* <uuid>@<host>:<port> */
    char *at = strrchr(main_part, '@');
    if (!at) { free(main_part); bmk_vless_free(out); return BMK_ERR_PARSE; }
    *at = '\0';
    out->uuid = bmk_url_percent_decode(main_part);

    char *hostport = at + 1;
    /* 支持 IPv6 字面量 [::1]:443 */
    char *colon;
    if (*hostport == '[') {
        char *rb = strchr(hostport, ']');
        if (!rb) { free(main_part); bmk_vless_free(out); return BMK_ERR_PARSE; }
        out->host = bmk_strndup(hostport + 1, (size_t)(rb - hostport - 1));
        colon = strchr(rb, ':');
    } else {
        colon = strrchr(hostport, ':');
        out->host = colon ? bmk_strndup(hostport, (size_t)(colon - hostport))
                          : bmk_strdup(hostport);
    }
    /* 端口必须显式给出（附录 C）；缺端口或端口非法均为解析失败 */
    if (!colon) {
        free(main_part);
        bmk_vless_free(out);
        return BMK_ERR_PARSE;
    }
    out->port = atoi(colon + 1);
    if (out->port <= 0 || out->port > 65535) {
        free(main_part);
        bmk_vless_free(out);
        return BMK_ERR_PARSE;
    }

    if (!out->uuid || !*out->uuid || !out->host || !*out->host) {
        free(main_part);
        bmk_vless_free(out);
        return BMK_ERR_PARSE;
    }

    /* 附录 C 参数映射 */
    out->encryption = query_param(query, "encryption");
    if (!out->encryption) out->encryption = bmk_strdup("none");
    out->flow     = query_param(query, "flow");
    out->security = query_param(query, "security");
    out->type     = query_param(query, "type");
    out->path     = query_param(query, "path");
    out->ws_host  = query_param(query, "host");
    out->sni      = query_param(query, "sni");
    out->fp       = query_param(query, "fp");
    out->alpn     = query_param(query, "alpn");
    out->pbk      = query_param(query, "pbk");
    out->sid      = query_param(query, "sid");
    out->spx      = query_param(query, "spx");

    free(main_part);
    return BMK_OK;
}

/* ------------------------------------------------------------------ */
/* xray 配置生成（附录 C 模板）                                        */
/* ------------------------------------------------------------------ */

static bool str_nonempty(const char *s) { return s && *s; }

char *bmk_vless_to_xray_config(const bmk_vless *v, int socks_port)
{
    if (!v) return NULL;

    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;

    /* log */
    cJSON *log = cJSON_CreateObject();
    cJSON_AddStringToObject(log, "loglevel", "warning");
    cJSON_AddItemToObject(root, "log", log);

    /* inbounds：仅绑定 127.0.0.1（需求 5.3 第 4 条） */
    cJSON *inbounds = cJSON_CreateArray();
    cJSON *in = cJSON_CreateObject();
    cJSON_AddStringToObject(in, "listen", "127.0.0.1");
    cJSON_AddNumberToObject(in, "port", socks_port);
    cJSON_AddStringToObject(in, "protocol", "socks");
    cJSON *inset = cJSON_CreateObject();
    cJSON_AddBoolToObject(inset, "udp", 0);
    cJSON_AddItemToObject(in, "settings", inset);
    cJSON_AddItemToArray(inbounds, in);
    cJSON_AddItemToObject(root, "inbounds", inbounds);

    /* outbounds */
    cJSON *outbounds = cJSON_CreateArray();
    cJSON *ob = cJSON_CreateObject();
    cJSON_AddStringToObject(ob, "protocol", "vless");

    cJSON *settings = cJSON_CreateObject();
    cJSON *vnext = cJSON_CreateArray();
    cJSON *srv = cJSON_CreateObject();
    cJSON_AddStringToObject(srv, "address", v->host);
    cJSON_AddNumberToObject(srv, "port", v->port);

    cJSON *users = cJSON_CreateArray();
    cJSON *u = cJSON_CreateObject();
    cJSON_AddStringToObject(u, "id", v->uuid);
    cJSON_AddStringToObject(u, "encryption",
                            str_nonempty(v->encryption) ? v->encryption : "none");
    /* flow 空则不写（生成规则） */
    if (str_nonempty(v->flow)) cJSON_AddStringToObject(u, "flow", v->flow);
    cJSON_AddItemToArray(users, u);
    cJSON_AddItemToObject(srv, "users", users);
    cJSON_AddItemToArray(vnext, srv);
    cJSON_AddItemToObject(settings, "vnext", vnext);
    cJSON_AddItemToObject(ob, "settings", settings);

    /* streamSettings */
    cJSON *ss = cJSON_CreateObject();
    const char *network = str_nonempty(v->type) ? v->type : "tcp";
    cJSON_AddStringToObject(ss, "network", network);

    /* Bug#10: security 缺失时根据端口推断，443 默认 tls */
    const char *security = str_nonempty(v->security) ? v->security
                         : (v->port == 443 ? "tls" : "none");
    cJSON_AddStringToObject(ss, "security", security);

    /* security=none 时不写 tlsSettings/realitySettings */
    if (strcmp(security, "tls") == 0) {
        cJSON *tls = cJSON_CreateObject();
        if (str_nonempty(v->sni)) cJSON_AddStringToObject(tls, "serverName", v->sni);
        if (str_nonempty(v->fp))  cJSON_AddStringToObject(tls, "fingerprint", v->fp);
        if (str_nonempty(v->alpn)) {
            cJSON *arr = cJSON_CreateArray();
            char *copy = bmk_strdup(v->alpn);
            if (copy) {
                char *tok = copy;
                while (tok) {
                    char *comma = strchr(tok, ',');
                    if (comma) *comma = '\0';
                    bmk_str_trim(tok);
                    if (*tok) cJSON_AddItemToArray(arr, cJSON_CreateString(tok));
                    tok = comma ? comma + 1 : NULL;
                }
                free(copy);
            }
            cJSON_AddItemToObject(tls, "alpn", arr);
        }
        cJSON_AddItemToObject(ss, "tlsSettings", tls);
    } else if (strcmp(security, "reality") == 0) {
        cJSON *re = cJSON_CreateObject();
        if (str_nonempty(v->sni)) cJSON_AddStringToObject(re, "serverName", v->sni);
        if (str_nonempty(v->fp))  cJSON_AddStringToObject(re, "fingerprint", v->fp);
        if (str_nonempty(v->pbk)) cJSON_AddStringToObject(re, "publicKey", v->pbk);
        if (str_nonempty(v->sid)) cJSON_AddStringToObject(re, "shortId", v->sid);
        if (str_nonempty(v->spx)) cJSON_AddStringToObject(re, "spiderX", v->spx);
        cJSON_AddItemToObject(ss, "realitySettings", re);
    }

    if (strcmp(network, "ws") == 0) {
        cJSON *ws = cJSON_CreateObject();
        cJSON_AddStringToObject(ws, "path", str_nonempty(v->path) ? v->path : "/");
        if (str_nonempty(v->ws_host)) {
            cJSON *hdr = cJSON_CreateObject();
            cJSON_AddStringToObject(hdr, "Host", v->ws_host);
            cJSON_AddItemToObject(ws, "headers", hdr);
        }
        cJSON_AddItemToObject(ss, "wsSettings", ws);
    } else if (strcmp(network, "grpc") == 0) {
        cJSON *g = cJSON_CreateObject();
        if (str_nonempty(v->path)) cJSON_AddStringToObject(g, "serviceName", v->path);
        cJSON_AddItemToObject(ss, "grpcSettings", g);
    } else if (strcmp(network, "http") == 0) {
        cJSON *hs = cJSON_CreateObject();
        if (str_nonempty(v->path)) cJSON_AddStringToObject(hs, "path", v->path);
        if (str_nonempty(v->ws_host)) {
            cJSON *arr = cJSON_CreateArray();
            cJSON_AddItemToArray(arr, cJSON_CreateString(v->ws_host));
            cJSON_AddItemToObject(hs, "host", arr);
        }
        cJSON_AddItemToObject(ss, "httpSettings", hs);
    }

    cJSON_AddItemToObject(ob, "streamSettings", ss);
    cJSON_AddItemToArray(outbounds, ob);
    cJSON_AddItemToObject(root, "outbounds", outbounds);

    char *text = cJSON_Print(root);
    cJSON_Delete(root);
    return text;
}

/* ------------------------------------------------------------------ */
/* 节点池                                                              */
/* ------------------------------------------------------------------ */

void bmk_node_free(bmk_node *n)
{
    if (!n) return;
    free(n->name);
    free(n->vless_uri);
    free(n->host);
    free(n->username);
    free(n->password);
    memset(n, 0, sizeof(*n));
}

typedef struct {
    bmk_node   node;
    bmk_proc  *proc;        /* xray 子进程；HTTP/SOCKS5 为 NULL */
    char      *config_path;
} node_slot;

/* 节点池互斥锁：g_nodes 及其所有访问点（QA 第 2 项）。
 * 校验 worker 线程会并发调用 bmk_proxy_next_endpoint，UI 线程会增删/启停节点；
 * 不加锁时 remove 的 memmove+free 与遍历存在 use-after-free 风险。
 * 约定：锁粒度要小，锁内不做网络请求/子进程拉起与终止（bmk_proc_alive 非阻塞，可在锁内）。
 * Windows 用 CRITICAL_SECTION，POSIX 用 pthread_mutex，风格与 bmk_validate.c 一致。 */
#ifdef _WIN32
#  include <windows.h>
static CRITICAL_SECTION g_node_lock;
static int g_node_lock_ready = 0;
static void proxy_lock_init(void)
{
    if (!g_node_lock_ready) { InitializeCriticalSection(&g_node_lock); g_node_lock_ready = 1; }
}
static void proxy_lock(void)   { proxy_lock_init(); EnterCriticalSection(&g_node_lock); }
static void proxy_unlock(void) { LeaveCriticalSection(&g_node_lock); }
#else
#  include <pthread.h>
static pthread_mutex_t g_node_lock = PTHREAD_MUTEX_INITIALIZER;
static void proxy_lock_init(void) { }
static void proxy_lock(void)   { pthread_mutex_lock(&g_node_lock); }
static void proxy_unlock(void) { pthread_mutex_unlock(&g_node_lock); }
#endif

static node_slot *g_nodes = NULL;
static size_t     g_node_count = 0;
static size_t     g_node_cap = 0;
static int64_t    g_next_id = 1;
static unsigned   g_cursor = 0;
static unsigned   g_failover_idx = 0;  /* Bug#2: FAILOVER 模式游标 */
static bmk_rotation g_rotation = BMK_ROTATE_ROUND;
static char      *g_xray_path = NULL;
static char      *g_config_dir = NULL;

void bmk_proxy_set_rotation(bmk_rotation r)
{
    proxy_lock();
    g_rotation = r;
    proxy_unlock();
}

void bmk_proxy_set_xray_path(const char *path)
{
    proxy_lock();
    free(g_xray_path);
    g_xray_path = bmk_strdup(path);
    proxy_unlock();
}

size_t bmk_proxy_node_count(void)
{
    proxy_lock();
    size_t n = g_node_count;
    proxy_unlock();
    return n;
}

bmk_node *bmk_proxy_node_at(size_t i)
{
    proxy_lock();
    bmk_node *n = (i < g_node_count) ? &g_nodes[i].node : NULL;
    proxy_unlock();
    /* 注意：返回的指针在解锁后仍可被并发 remove 释放；调用方应保证
     * 节点管理（增删改）只在单一线程（UI 线程）进行。 */
    return n;
}

/* 按 id 查找节点槽。调用方必须持有 g_node_lock。 */
static node_slot *find_slot(int64_t id)
{
    for (size_t i = 0; i < g_node_count; i++)
        if (g_nodes[i].node.id == id) return &g_nodes[i];
    return NULL;
}

static void copy_node(bmk_node *dst, const bmk_node *src)
{
    dst->name      = bmk_strdup(src->name);
    dst->type      = src->type;
    dst->enabled   = src->enabled;
    dst->vless_uri = bmk_strdup(src->vless_uri);
    dst->host      = bmk_strdup(src->host);
    dst->port      = src->port;
    dst->username  = bmk_strdup(src->username);
    dst->password  = bmk_strdup(src->password);
    dst->available = true;
    dst->restart_count = 0;          /* 新增/更新节点时重置每会话重启预算（FR-17 第 3 条） */
    dst->stopped_explicitly = false; /* 新增/更新节点时清除显式停止标记 */
}

bmk_err bmk_proxy_node_add(const bmk_node *n, int64_t *out_id)
{
    if (!n) return BMK_ERR_INVALID_ARG;
    if (out_id) *out_id = 0;

    proxy_lock();
    if (g_node_count == g_node_cap) {
        size_t cap = g_node_cap ? g_node_cap * 2 : 8;
        node_slot *ns = (node_slot *)realloc(g_nodes, cap * sizeof(node_slot));
        if (!ns) { proxy_unlock(); return BMK_ERR_NOMEM; }
        memset(ns + g_node_cap, 0, (cap - g_node_cap) * sizeof(node_slot));
        g_nodes = ns;
        g_node_cap = cap;
    }
    node_slot *s = &g_nodes[g_node_count];
    memset(s, 0, sizeof(*s));
    copy_node(&s->node, n);
    s->node.id = g_next_id++;
    if (out_id) *out_id = s->node.id;
    g_node_count++;
    BMK_INFO("proxy node added: %s", s->node.name ? s->node.name : "(unnamed)");
    proxy_unlock();
    return BMK_OK;
}

bmk_err bmk_proxy_node_update(int64_t id, const bmk_node *n)
{
    if (!n) return BMK_ERR_INVALID_ARG;

    /* 锁内摘除旧进程指针并替换配置，terminate 在锁外进行 */
    bmk_proc *old = NULL;
    proxy_lock();
    node_slot *s = find_slot(id);
    if (!s) { proxy_unlock(); return BMK_ERR_NOT_FOUND; }
    old = s->proc;
    s->proc = NULL;
    bmk_node_free(&s->node);
    copy_node(&s->node, n);
    s->node.id = id;
    s->node.local_port = 0;
    proxy_unlock();

    if (old) { bmk_proc_terminate(old); bmk_proc_free(old); }
    return BMK_OK;
}

bmk_err bmk_proxy_node_remove(int64_t id)
{
    bmk_proc *old = NULL;
    proxy_lock();
    node_slot *s = find_slot(id);
    if (!s) { proxy_unlock(); return BMK_ERR_NOT_FOUND; }
    size_t idx = (size_t)(s - g_nodes);
    old = g_nodes[idx].proc;
    g_nodes[idx].proc = NULL;
    bmk_node_free(&g_nodes[idx].node);
    free(g_nodes[idx].config_path);
    memmove(&g_nodes[idx], &g_nodes[idx + 1],
            (g_node_count - idx - 1) * sizeof(node_slot));
    g_node_count--;
    proxy_unlock();

    if (old) { bmk_proc_terminate(old); bmk_proc_free(old); }
    return BMK_OK;
}

/* 端口冲突自动换端口（FR-17 边界）。遍历 g_nodes，内部加锁；调用方不得持锁。 */
static int alloc_port(void)
{
    proxy_lock();
    static int next = PORT_BASE;
    int result = 0;
    for (int tries = 0; tries < 200; tries++) {
        int p = next++;
        if (next > 65000) next = PORT_BASE;
        bool used = false;
        for (size_t i = 0; i < g_node_count; i++)
            if (g_nodes[i].node.local_port == p) { used = true; break; }
        if (!used && bmk_port_is_free(p)) { result = p; break; }
    }
    proxy_unlock();
    return result;
}

/* 启动 VLESS 节点的 xray 子进程（FR-17）。
 * 两段式加锁：锁内快照配置，锁外完成解析/写配置/拉起子进程（避免锁内做
 * 进程/文件操作），最后锁内提交 proc 指针与状态。重启路径（QA 第 4 项）在
 * 提交时先摘除旧 proc 指针、锁外 terminate+free，避免句柄泄漏。 */
bmk_err bmk_proxy_node_start(int64_t id)
{
    char *vless_uri = NULL;
    char *xray_path = NULL;

    proxy_lock();
    node_slot *s = find_slot(id);
    if (!s) { proxy_unlock(); return BMK_ERR_NOT_FOUND; }

    /* HTTP/SOCKS5 无需子进程 */
    if (s->node.type != BMK_NODE_VLESS) {
        s->node.available = true;
        s->node.stopped_explicitly = false;
        proxy_unlock();
        return BMK_OK;
    }
    if (s->proc && bmk_proc_alive(s->proc)) { proxy_unlock(); return BMK_OK; }
    if (!s->node.vless_uri) { proxy_unlock(); return BMK_ERR_INVALID_ARG; }

    /* 锁内拷贝需要的数据 */
    vless_uri = bmk_strdup(s->node.vless_uri);
    xray_path = bmk_strdup(g_xray_path ? g_xray_path : "xray");
    proxy_unlock();

    if (!vless_uri || !xray_path) {
        free(vless_uri); free(xray_path);
        return BMK_ERR_NOMEM;
    }

    bmk_vless v;
    memset(&v, 0, sizeof(v));
    bmk_err e = bmk_vless_parse(vless_uri, &v);
    free(vless_uri);
    if (e != BMK_OK) {
        proxy_lock();
        node_slot *s2 = find_slot(id);
        if (s2) s2->node.available = false;
        proxy_unlock();
        BMK_WARN("vless parse failed for node id=%lld", (long long)id);
        free(xray_path);
        return e;
    }

    int port = alloc_port();
    if (!port) { bmk_vless_free(&v); free(xray_path); return BMK_ERR_IO; }

    char *config = bmk_vless_to_xray_config(&v, port);
    bmk_vless_free(&v);
    if (!config) { free(xray_path); return BMK_ERR_NOMEM; }

    /* 写入配置文件（锁外） */
    char fname[64];
    snprintf(fname, sizeof(fname), "xray_%lld.json", (long long)id);
    char *config_path = NULL;
    proxy_lock();
    if (g_config_dir) config_path = bmk_path_join(g_config_dir, fname);
    proxy_unlock();
    if (!config_path) { free(config); free(xray_path); return BMK_ERR_IO; }
    e = bmk_write_file(config_path, config, strlen(config));
    free(config);
    if (e != BMK_OK) { free(config_path); free(xray_path); return e; }

    /* 拉起子进程（锁外） */
    bmk_proc *proc = NULL;
    const char *argv[] = { xray_path, "run", "-c", config_path, NULL };
    e = bmk_proc_spawn(argv, &proc);
    if (e != BMK_OK) {
        proxy_lock();
        node_slot *s2 = find_slot(id);
        if (s2) { s2->node.available = false; s2->node.local_port = 0; }
        proxy_unlock();
        BMK_WARN("xray-core failed to start for node id=%lld (path=%s)",
                 (long long)id, xray_path);
        free(config_path); free(xray_path);
        return e;
    }

    /* 锁内提交；若节点已被删除，则在锁外终止刚拉起的进程 */
    bmk_proc *old = NULL;
    proxy_lock();
    s = find_slot(id);
    if (!s) { proxy_unlock(); bmk_proc_terminate(proc); bmk_proc_free(proc); free(config_path); free(xray_path); return BMK_ERR_NOT_FOUND; }
    old = s->proc;
    s->proc = proc;
    s->node.local_port = port;
    s->node.available = true;
    s->node.stopped_explicitly = false;   /* 用户重新 start 清除显式停止标记 */
    free(s->config_path);
    s->config_path = config_path;
    proxy_unlock();

    if (old) { bmk_proc_terminate(old); bmk_proc_free(old); }
    BMK_INFO("proxy node id=%lld started on 127.0.0.1:%d", (long long)id, port);
    free(xray_path);
    return BMK_OK;
}

void bmk_proxy_node_stop(int64_t id)
{
    /* 显式停止：摘除 proc 指针并标记 stopped_explicitly（QA 第 5 项），
     * 禁止 ensure_node_ready 自动重启；terminate 在锁外执行。 */
    bmk_proc *p = NULL;
    proxy_lock();
    node_slot *s = find_slot(id);
    if (!s) { proxy_unlock(); return; }
    p = s->proc;
    s->proc = NULL;
    s->node.local_port = 0;
    s->node.available = false;
    s->node.stopped_explicitly = true;
    proxy_unlock();

    if (p) { bmk_proc_terminate(p); bmk_proc_free(p); }
}

void bmk_proxy_shutdown(void)
{
    /* 应用退出时强制结束所有子进程（需求 2.4）。锁内摘除，锁外 terminate。 */
    bmk_proc *procs[64];
    size_t n = 0;
    proxy_lock();
    for (size_t i = 0; i < g_node_count; i++) {
        if (g_nodes[i].proc && n < 64) procs[n++] = g_nodes[i].proc;
        bmk_node_free(&g_nodes[i].node);
        free(g_nodes[i].config_path);
    }
    g_node_count = g_node_cap = 0;
    free(g_nodes);
    g_nodes = NULL;
    free(g_xray_path);
    g_xray_path = NULL;
    free(g_config_dir);
    g_config_dir = NULL;
    proxy_unlock();

    for (size_t i = 0; i < n; i++) {
        bmk_proc_terminate(procs[i]);
        bmk_proc_free(procs[i]);
    }
}

/* 构造某节点的 curl 代理串 */
static char *slot_endpoint(const node_slot *s)
{
    char buf[512];
    if (s->node.type == BMK_NODE_VLESS) {
        if (!s->node.local_port) return NULL;
        snprintf(buf, sizeof(buf), "socks5h://127.0.0.1:%d", s->node.local_port);
        return bmk_strdup(buf);
    }
    if (!s->node.host || s->node.port <= 0) return NULL;
    const char *scheme = (s->node.type == BMK_NODE_SOCKS5) ? "socks5h" : "http";
    if (s->node.username && *s->node.username) {
        snprintf(buf, sizeof(buf), "%s://%s:%s@%s:%d", scheme,
                 s->node.username, s->node.password ? s->node.password : "",
                 s->node.host, s->node.port);
    } else {
        snprintf(buf, sizeof(buf), "%s://%s:%d", scheme, s->node.host, s->node.port);
    }
    return bmk_strdup(buf);
}

/* 确保 VLESS 节点的 xray 子进程存活：进程异常退出时自动重启（FR-17 第 3 条），
 * 每会话最多 MAX_RESTARTS 次/节点，超过则标记节点不可用并从轮换池剔除。
 * 用户显式停止的节点（stopped_explicitly）不自动重启（QA 第 5 项）。
 * HTTP/SOCKS5 节点无子进程，直接返回当前可用性。
 * 内部自行加锁；调用方不得持有锁。 */
static bool ensure_node_ready(int64_t id)
{
    proxy_lock();
    node_slot *s = find_slot(id);
    if (!s) { proxy_unlock(); return false; }
    if (s->node.type != BMK_NODE_VLESS) {
        bool a = s->node.available;
        proxy_unlock();
        return a;
    }
    if (s->node.stopped_explicitly) { proxy_unlock(); return false; }
    if (s->node.local_port && s->proc && bmk_proc_alive(s->proc)) {
        bool a = s->node.available;
        proxy_unlock();
        return a;
    }
    if (s->node.restart_count >= MAX_RESTARTS) {
        s->node.available = false;
        BMK_WARN("proxy node id=%lld exceeded restart limit; marking unavailable",
                 (long long)id);
        proxy_unlock();
        return false;
    }
    s->node.restart_count++;
    proxy_unlock();

    /* 锁外重启（node_start 内部自行加锁） */
    return bmk_proxy_node_start(id) == BMK_OK;
}

char *bmk_proxy_next_endpoint(const char *target_url)
{
    /* 第一遍：锁内快照出需要重启的 VLESS 节点 id */
    int64_t need_restart[64];
    size_t nrestart = 0;
    proxy_lock();
    for (size_t i = 0; i < g_node_count && nrestart < 64; i++) {
        node_slot *s = &g_nodes[i];
        if (!s->node.enabled) continue;
        if (s->node.type != BMK_NODE_VLESS) continue;
        if (s->node.stopped_explicitly) continue;
        if (!(s->node.local_port && s->proc && bmk_proc_alive(s->proc)))
            need_restart[nrestart++] = s->node.id;
    }
    proxy_unlock();

    /* 锁外自动重启（进程拉起/写配置不应持锁） */
    for (size_t k = 0; k < nrestart; k++) ensure_node_ready(need_restart[k]);

    /* 第二遍：锁内选取可用节点并生成代理串（slot_endpoint 仅构建字符串，可在锁内） */
    size_t avail[64];
    size_t navail = 0;
    proxy_lock();
    for (size_t i = 0; i < g_node_count && navail < 64; i++) {
        node_slot *s = &g_nodes[i];
        if (!s->node.enabled || !s->node.available) continue;
        if (s->node.type == BMK_NODE_VLESS &&
            !(s->node.local_port && s->proc && bmk_proc_alive(s->proc)))
            continue;
        avail[navail++] = i;
    }
    if (navail == 0) { proxy_unlock(); return NULL; }

    size_t pick;
    switch (g_rotation) {
    case BMK_ROTATE_RANDOM:
        pick = avail[(size_t)rand() % navail];
        break;
    case BMK_ROTATE_BY_HOST: {
        /* 同一域名整会话内固定同一节点 */
        char *host = target_url ? bmk_url_host(target_url) : NULL;
        unsigned long hash = 5381;
        for (const char *p = host ? host : ""; *p; p++)
            hash = hash * 33 + (unsigned char)*p;
        free(host);
        pick = avail[hash % navail];
        break;
    }
    case BMK_ROTATE_FAILOVER:
        /* Bug#2：使用独立游标轮转，失败重试时自动换下一个 */
        pick = avail[g_failover_idx % navail];
        break;
    case BMK_ROTATE_ROUND:
    default:
        pick = avail[g_cursor++ % navail];
        break;
    }
    char *endpoint = slot_endpoint(&g_nodes[pick]);
    proxy_unlock();
    return endpoint;
}

/* ------------------------------------------------------------------ */
/* 连通性测试                                                          */
/* ------------------------------------------------------------------ */

static size_t sink_cb(char *p, size_t s, size_t n, void *u)
{
    (void)p; (void)u;
    return s * n;
}

bmk_err bmk_proxy_node_test(int64_t id, const char *test_url, long *out_ms)
{
    /* 锁内取端点串，curl 网络请求在锁外执行 */
    char *endpoint = NULL;
    proxy_lock();
    node_slot *s = find_slot(id);
    if (!s) { proxy_unlock(); return BMK_ERR_NOT_FOUND; }
    endpoint = slot_endpoint(s);
    proxy_unlock();

    if (!endpoint) return BMK_ERR_UNSUPPORTED;

    const char *url = (test_url && *test_url) ? test_url
                                              : "https://www.google.com/generate_204";
    CURL *c = curl_easy_init();
    if (!c) { free(endpoint); return BMK_ERR_NOMEM; }

    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_PROXY, endpoint);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 8L);   /* 超时 8s（FR-17 第 4 条） */
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, sink_cb);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode rc = curl_easy_perform(c);
    long status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    if (out_ms) {
        curl_off_t t = 0;
        curl_easy_getinfo(c, CURLINFO_TOTAL_TIME_T, &t);
        *out_ms = (long)(t / 1000);
    }
    curl_easy_cleanup(c);
    free(endpoint);

    bool ok = (rc == CURLE_OK && status > 0 && status < 500);
    /* 失败节点自动标记不可用并从轮换池剔除 */
    proxy_lock();
    s = find_slot(id);
    if (s) s->node.available = ok;
    proxy_unlock();
    BMK_INFO("proxy test id=%lld: %s (status=%ld)",
             (long long)id, ok ? "ok" : "failed", status);
    return ok ? BMK_OK : BMK_ERR_NETWORK;
}

/* ------------------------------------------------------------------ */
/* 持久化                                                              */
/* ------------------------------------------------------------------ */

#define PROXY_NODES_KEY "proxy_nodes"

bmk_err bmk_proxy_load(bmk_db *db)
{
    if (!db) return BMK_ERR_INVALID_ARG;
    proxy_lock_init();

    /* 配置目录：<数据目录>/proxy */
    char *data_dir = bmk_settings_get_str(db, BMK_SET_DATA_DIR, NULL);
    char *new_cfg_dir = data_dir ? bmk_path_join(data_dir, "proxy") : NULL;
    free(data_dir);
    if (new_cfg_dir) bmk_mkdirs(new_cfg_dir);

    char *xp = bmk_settings_get_str(db, BMK_SET_XRAY_PATH, NULL);

    char *rot = bmk_settings_get_str(db, BMK_SET_PROXY_ROTATION, "round");
    bmk_rotation rotation = BMK_ROTATE_ROUND;
    if (rot) {
        if      (strcmp(rot, "random") == 0)   rotation = BMK_ROTATE_RANDOM;
        else if (strcmp(rot, "failover") == 0) rotation = BMK_ROTATE_FAILOVER;
        else if (strcmp(rot, "by_host") == 0)  rotation = BMK_ROTATE_BY_HOST;
        free(rot);
    }

    /* 清空旧节点（重复调用不叠加），进程终止在锁外进行 */
    bmk_proc *old_procs[64];
    size_t nold = 0;
    proxy_lock();
    for (size_t i = 0; i < g_node_count; i++) {
        if (g_nodes[i].proc && nold < 64) old_procs[nold++] = g_nodes[i].proc;
        bmk_node_free(&g_nodes[i].node);
        free(g_nodes[i].config_path);
    }
    g_node_count = g_node_cap = 0;
    free(g_nodes);
    g_nodes = NULL;
    free(g_config_dir);
    g_config_dir = new_cfg_dir;
    new_cfg_dir = NULL;
    free(g_xray_path);
    g_xray_path = xp;
    xp = NULL;
    g_rotation = rotation;
    proxy_unlock();
    for (size_t i = 0; i < nold; i++) { bmk_proc_terminate(old_procs[i]); bmk_proc_free(old_procs[i]); }
    free(new_cfg_dir);
    free(xp);

    char *json = bmk_settings_get_str(db, PROXY_NODES_KEY, NULL);
    if (!json) return BMK_OK;

    cJSON *arr = cJSON_Parse(json);
    free(json);
    if (!arr || !cJSON_IsArray(arr)) { cJSON_Delete(arr); return BMK_OK; }

    /* 解析与添加均在锁外进行（node_add 内部自行加锁） */
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, arr) {
        bmk_node n;
        memset(&n, 0, sizeof(n));
        cJSON *j;
        j = cJSON_GetObjectItem(item, "name");
        n.name = (j && cJSON_IsString(j)) ? j->valuestring : NULL;
        j = cJSON_GetObjectItem(item, "type");
        n.type = (j && cJSON_IsNumber(j)) ? (bmk_node_type)j->valueint : BMK_NODE_VLESS;
        j = cJSON_GetObjectItem(item, "enabled");
        n.enabled = j ? cJSON_IsTrue(j) : false;
        j = cJSON_GetObjectItem(item, "vless_uri");
        n.vless_uri = (j && cJSON_IsString(j)) ? j->valuestring : NULL;
        j = cJSON_GetObjectItem(item, "host");
        n.host = (j && cJSON_IsString(j)) ? j->valuestring : NULL;
        j = cJSON_GetObjectItem(item, "port");
        n.port = (j && cJSON_IsNumber(j)) ? j->valueint : 0;
        j = cJSON_GetObjectItem(item, "username");
        n.username = (j && cJSON_IsString(j)) ? j->valuestring : NULL;
        j = cJSON_GetObjectItem(item, "password");
        n.password = (j && cJSON_IsString(j)) ? j->valuestring : NULL;
        bmk_proxy_node_add(&n, NULL);   /* 内部 strdup，无需释放上面的引用 */
    }
    cJSON_Delete(arr);

    /* 启用的节点立即拉起（FR-17 第 3 条）。node_start 内部自行加锁。 */
    proxy_lock();
    size_t ncount = g_node_count;
    int64_t *enabled_ids = NULL;
    if (ncount > 0) {
        enabled_ids = (int64_t *)malloc(ncount * sizeof(int64_t));
        if (enabled_ids) {
            size_t e = 0;
            for (size_t i = 0; i < ncount; i++)
                if (g_nodes[i].node.enabled) enabled_ids[e++] = g_nodes[i].node.id;
            ncount = e;
        }
    }
    proxy_unlock();
    if (enabled_ids) {
        for (size_t i = 0; i < ncount; i++) bmk_proxy_node_start(enabled_ids[i]);
        free(enabled_ids);
    }

    return BMK_OK;
}

bmk_err bmk_proxy_save(bmk_db *db)
{
    if (!db) return BMK_ERR_INVALID_ARG;

    proxy_lock();
    cJSON *arr = cJSON_CreateArray();
    if (!arr) { proxy_unlock(); return BMK_ERR_NOMEM; }
    for (size_t i = 0; i < g_node_count; i++) {
        const bmk_node *n = &g_nodes[i].node;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", n->name ? n->name : "");
        cJSON_AddNumberToObject(o, "type", (int)n->type);
        cJSON_AddBoolToObject(o, "enabled", n->enabled ? 1 : 0);
        if (n->vless_uri) cJSON_AddStringToObject(o, "vless_uri", n->vless_uri);
        if (n->host)      cJSON_AddStringToObject(o, "host", n->host);
        cJSON_AddNumberToObject(o, "port", n->port);
        if (n->username)  cJSON_AddStringToObject(o, "username", n->username);
        if (n->password)  cJSON_AddStringToObject(o, "password", n->password);
        cJSON_AddItemToArray(arr, o);
    }
    char *text = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    proxy_unlock();
    if (!text) return BMK_ERR_NOMEM;

    bmk_err e = bmk_settings_set_str(db, PROXY_NODES_KEY, text);
    free(text);
    return e;
}
