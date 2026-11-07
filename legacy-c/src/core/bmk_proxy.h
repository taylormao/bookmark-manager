/*
 * bmk_proxy.h - 代理节点管理（FR-17 / 附录 C）
 *
 * VLESS 不自研协议：为每个启用节点生成 xray-core 配置并以子进程启动，
 * 入站为 127.0.0.1:<动态端口> 的 SOCKS5，libcurl 通过 socks5h:// 使用。
 */
#ifndef BMK_PROXY_H
#define BMK_PROXY_H

#include "bmk_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BMK_NODE_VLESS = 0,
    BMK_NODE_HTTP,
    BMK_NODE_SOCKS5,
} bmk_node_type;

/* vless:// 解析结果（附录 C 字段映射表） */
typedef struct {
    char *uuid;
    char *host;
    int   port;
    char *encryption;   /* 固定 none */
    char *flow;         /* 可空 */
    char *security;     /* none / tls / reality */
    char *type;         /* tcp / ws / grpc / http */
    char *path;
    char *ws_host;      /* ws/HTTP 伪装 Host */
    char *sni;
    char *fp;
    char *alpn;         /* 逗号分隔原文 */
    char *pbk;          /* reality publicKey */
    char *sid;          /* reality shortId */
    char *spx;          /* reality spiderX */
    char *name;         /* # 之后的名称 */
} bmk_vless;

void bmk_vless_free(bmk_vless *v);

/*
 * 解析 vless:// 链接（附录 C）。
 * 参数值必须原样 URL 解码后写入。失败返回 BMK_ERR_PARSE。
 */
bmk_err bmk_vless_parse(const char *uri, bmk_vless *out);

/*
 * 依据解析结果生成 xray-core 配置 JSON（附录 C 模板）。
 * 未知/缺失参数不写对应键；security=none 时不写 tlsSettings/realitySettings。
 * 返回新分配字符串，调用者 free。
 */
char *bmk_vless_to_xray_config(const bmk_vless *v, int socks_port);

/* ---- 节点 ---- */

typedef struct {
    int64_t        id;
    char          *name;
    bmk_node_type  type;
    bool           enabled;
    /* VLESS */
    char          *vless_uri;
    /* HTTP / SOCKS5 */
    char          *host;
    int            port;
    char          *username;
    char          *password;
    /* 运行时 */
    int            local_port;    /* xray 入站端口，0 表示未启动 */
    bool           available;
    int            restart_count;
    bool           stopped_explicitly; /* 用户显式停止：禁止自动重启，直至重新 start */
} bmk_node;

void bmk_node_free(bmk_node *n);

/*
 * 节点池（进程级）。节点配置持久化在 settings 的 "proxy_nodes" 键（JSON 数组）。
 */
typedef struct bmk_db bmk_db;

bmk_err bmk_proxy_load(bmk_db *db);
bmk_err bmk_proxy_save(bmk_db *db);
void    bmk_proxy_shutdown(void);   /* 终止全部子进程（应用退出时调用） */

size_t     bmk_proxy_node_count(void);
bmk_node  *bmk_proxy_node_at(size_t i);
bmk_err    bmk_proxy_node_add(const bmk_node *n, int64_t *out_id);
bmk_err    bmk_proxy_node_update(int64_t id, const bmk_node *n);
bmk_err    bmk_proxy_node_remove(int64_t id);

/* 启用节点：VLESS 拉起 xray 子进程；HTTP/SOCKS5 直接可用。 */
bmk_err bmk_proxy_node_start(int64_t id);
void    bmk_proxy_node_stop(int64_t id);

/* ---- 代理配置分层校验（FR-17 补充） ---- */

typedef enum {
    BMK_PCHK_SKIP = 0,   /* 该阶段未执行 */
    BMK_PCHK_OK,
    BMK_PCHK_FAIL,
} bmk_proxy_stage_state;

typedef enum {
    BMK_PCHK_TCP = 0,    /* 1. TCP 连到 host:port */
    BMK_PCHK_HANDSHAKE,  /* 2. HTTP CONNECT / SOCKS5 greeting */
    BMK_PCHK_TARGET,     /* 3. 经代理请求测试 URL（可选） */
    BMK_PCHK_STAGES,
} bmk_proxy_stage;

typedef struct {
    bmk_proxy_stage_state state[BMK_PCHK_STAGES];
    long                  ms[BMK_PCHK_STAGES];
    char                  detail[BMK_PCHK_STAGES][128];
    bool                  usable;  /* 只由 TCP + 握手决定：IP/端口/协议本身可用 */
    bmk_err               err;
} bmk_proxy_check;

/*
 * 对一份「尚未保存」的节点配置做分层校验：不读写节点池、不落库、不启动进程。
 * 阶段 1 TCP 连通、阶段 2 按 type 做协议握手（能识别协议填错与认证失败）、
 * 阶段 3 在 test_url 非空时经代理访问目标站。
 *
 * 阶段 3 的结果只记在 state[BMK_PCHK_TARGET]，不改变 usable：
 * 目标站被墙/超时不等于代理服务器本身不可用，这样才不会把「代理是对的、
 * 只是 google 访问不了」误报成配置错误。
 *
 * VLESS 依赖本地 xray 入站，需 cfg->local_port > 0（即已保存并启动），
 * 否则返回 BMK_ERR_UNSUPPORTED。
 */
bmk_err bmk_proxy_endpoint_check(const bmk_node *cfg, const char *test_url,
                                 bmk_proxy_check *out);

/* 连通性测试（FR-17 第 4 条）。out_ms 为耗时毫秒。 */
bmk_err bmk_proxy_node_test(int64_t id, const char *test_url, long *out_ms);

/*
 * 按轮换策略取下一个可用节点的 curl 代理串（如 socks5h://127.0.0.1:10808）。
 * 无可用节点返回 NULL。target_url 用于“按域名固定”策略。
 * 返回新分配字符串，调用者 free。
 */
char *bmk_proxy_next_endpoint(const char *target_url);

typedef enum {
    BMK_ROTATE_ROUND = 0,  /* 轮询（默认） */
    BMK_ROTATE_RANDOM,
    BMK_ROTATE_FAILOVER,
    BMK_ROTATE_BY_HOST,
} bmk_rotation;

void bmk_proxy_set_rotation(bmk_rotation r);
void bmk_proxy_set_xray_path(const char *path);

#ifdef __cplusplus
}
#endif
#endif /* BMK_PROXY_H */
