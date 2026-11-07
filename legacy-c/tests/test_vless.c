/*
 * test_vless.c - vless:// 解析与 xray 配置生成（附录 C）
 */
#include "test_util.h"
#include "bmk_proxy.h"

#include <stdio.h>

static const char *FULL_URI =
"vless://11111111-2222-3333-4444-555555555555@example.org:443"
"?encryption=none&security=tls&type=ws&path=%2Fray%2Fpath&host=cdn.example.com"
"&sni=sni.example.com&fp=chrome&alpn=http%2F1.1&flow=xtls-rprx-vision"
"#%E9%A6%99%E6%B8%AF%E8%8A%82%E7%82%B9";

static void test_parse_full(void)
{
    bmk_vless v;
    memset(&v, 0, sizeof v);
    CHECK_EQ(bmk_vless_parse(FULL_URI, &v), BMK_OK, "完整链接应解析成功");

    CHECK_STR(v.uuid, "11111111-2222-3333-4444-555555555555", "uuid");
    CHECK_STR(v.host, "example.org", "host");
    CHECK_EQ(v.port, 443, "port");
    CHECK_STR(v.encryption, "none", "encryption");
    CHECK_STR(v.security, "tls", "security");
    CHECK_STR(v.type, "ws", "type");
    CHECK_STR(v.path, "/ray/path", "path 应 URL 解码");
    CHECK_STR(v.ws_host, "cdn.example.com", "host 伪装头");
    CHECK_STR(v.sni, "sni.example.com", "sni");
    CHECK_STR(v.fp, "chrome", "fingerprint");
    CHECK_STR(v.alpn, "http/1.1", "alpn 应 URL 解码");
    CHECK_STR(v.flow, "xtls-rprx-vision", "flow");
    CHECK_STR(v.name, "香港节点", "名称应 URL 解码为 UTF-8");

    bmk_vless_free(&v);
}

static void test_parse_reality(void)
{
    const char *uri =
      "vless://abc-uuid@1.2.3.4:8443?encryption=none&security=reality&type=tcp"
      "&sni=www.microsoft.com&fp=chrome&pbk=PUBKEY123&sid=ab12&spx=%2F#reality";

    bmk_vless v;
    memset(&v, 0, sizeof v);
    CHECK_EQ(bmk_vless_parse(uri, &v), BMK_OK, "reality 链接应解析成功");
    CHECK_STR(v.security, "reality", "security=reality");
    CHECK_STR(v.pbk, "PUBKEY123", "reality publicKey");
    CHECK_STR(v.sid, "ab12", "reality shortId");
    CHECK_STR(v.spx, "/", "reality spiderX 解码");
    CHECK_STR(v.type, "tcp", "network=tcp");
    bmk_vless_free(&v);
}

static void test_parse_minimal(void)
{
    bmk_vless v;
    memset(&v, 0, sizeof v);
    CHECK_EQ(bmk_vless_parse("vless://uuid-only@host.tld:1080", &v), BMK_OK,
             "最简链接应解析成功");
    CHECK_STR(v.uuid, "uuid-only", "uuid");
    CHECK_STR(v.host, "host.tld", "host");
    CHECK_EQ(v.port, 1080, "port");
    CHECK(v.flow == NULL || v.flow[0] == '\0', "缺失的 flow 应为空");
    bmk_vless_free(&v);
}

static void test_parse_invalid(void)
{
    bmk_vless v;
    memset(&v, 0, sizeof v);
    CHECK(bmk_vless_parse("https://example.com", &v) != BMK_OK, "非 vless 前缀应失败");
    CHECK(bmk_vless_parse("vless://nohost", &v) != BMK_OK, "缺少 @host:port 应失败");
    CHECK(bmk_vless_parse("vless://uuid@host", &v) != BMK_OK, "缺少端口应失败");
    CHECK(bmk_vless_parse("vless://uuid@host:0", &v) != BMK_OK, "端口 0 应失败");
    CHECK(bmk_vless_parse("vless://uuid@host:70000", &v) != BMK_OK, "端口越界应失败");
    CHECK(bmk_vless_parse("vless://uuid@host:abc", &v) != BMK_OK, "非数字端口应失败");
    CHECK(bmk_vless_parse("vless://@host:443", &v) != BMK_OK, "缺少 uuid 应失败");
    CHECK(bmk_vless_parse("", &v) != BMK_OK, "空串应失败");
    CHECK(bmk_vless_parse(NULL, &v) != BMK_OK, "NULL 应失败");
}

static void test_xray_config_ws_tls(void)
{
    bmk_vless v;
    memset(&v, 0, sizeof v);
    bmk_vless_parse(FULL_URI, &v);

    char *cfg = bmk_vless_to_xray_config(&v, 10808);
    CHECK(cfg != NULL, "生成配置应成功");
    if (cfg) {
        /* 入站：127.0.0.1 上的 socks，端口由应用分配 */
        CHECK(strstr(cfg, "\"listen\":\t\"127.0.0.1\"") ||
              strstr(cfg, "\"listen\":") , "含 listen 字段");
        CHECK(strstr(cfg, "10808") != NULL, "入站端口写入");
        CHECK(strstr(cfg, "\"socks\"") != NULL, "入站协议为 socks");

        /* 出站映射（附录 C 表） */
        CHECK(strstr(cfg, "\"vless\"") != NULL, "出站协议为 vless");
        CHECK(strstr(cfg, "11111111-2222-3333-4444-555555555555") != NULL, "uuid 写入");
        CHECK(strstr(cfg, "example.org") != NULL, "address 写入");
        CHECK(strstr(cfg, "xtls-rprx-vision") != NULL, "flow 写入");
        CHECK(strstr(cfg, "\"network\"") && strstr(cfg, "\"ws\""), "network=ws");
        CHECK(strstr(cfg, "tlsSettings") != NULL, "security=tls 应写 tlsSettings");
        CHECK(strstr(cfg, "sni.example.com") != NULL, "serverName 来自 sni");
        CHECK(strstr(cfg, "chrome") != NULL, "fingerprint 写入");
        CHECK(strstr(cfg, "http/1.1") != NULL, "alpn 写入");
        CHECK(strstr(cfg, "wsSettings") != NULL, "含 wsSettings");
        CHECK(strstr(cfg, "/ray/path") != NULL, "ws path 写入");
        CHECK(strstr(cfg, "cdn.example.com") != NULL, "ws Host 头写入");
        CHECK(strstr(cfg, "realitySettings") == NULL, "非 reality 不应写 realitySettings");
        free(cfg);
    }
    bmk_vless_free(&v);
}

static void test_xray_config_reality(void)
{
    const char *uri =
      "vless://abc-uuid@1.2.3.4:8443?encryption=none&security=reality&type=tcp"
      "&sni=www.microsoft.com&fp=chrome&pbk=PUBKEY123&sid=ab12#r";

    bmk_vless v;
    memset(&v, 0, sizeof v);
    bmk_vless_parse(uri, &v);

    char *cfg = bmk_vless_to_xray_config(&v, 20808);
    CHECK(cfg != NULL, "reality 配置生成");
    if (cfg) {
        CHECK(strstr(cfg, "realitySettings") != NULL, "含 realitySettings");
        CHECK(strstr(cfg, "PUBKEY123") != NULL, "publicKey 写入");
        CHECK(strstr(cfg, "ab12") != NULL, "shortId 写入");
        CHECK(strstr(cfg, "www.microsoft.com") != NULL, "serverName 写入");
        CHECK(strstr(cfg, "tlsSettings") == NULL, "reality 不应写 tlsSettings");
        CHECK(strstr(cfg, "wsSettings") == NULL, "tcp 不应写 wsSettings");
        free(cfg);
    }
    bmk_vless_free(&v);
}

static void test_xray_config_none_security(void)
{
    const char *uri = "vless://u@h.tld:80?encryption=none&security=none&type=tcp#plain";

    bmk_vless v;
    memset(&v, 0, sizeof v);
    bmk_vless_parse(uri, &v);

    char *cfg = bmk_vless_to_xray_config(&v, 30808);
    CHECK(cfg != NULL, "security=none 配置生成");
    if (cfg) {
        /* 附录 C：security=none 时不写 tlsSettings/realitySettings */
        CHECK(strstr(cfg, "tlsSettings") == NULL, "不应写 tlsSettings");
        CHECK(strstr(cfg, "realitySettings") == NULL, "不应写 realitySettings");
        free(cfg);
    }
    bmk_vless_free(&v);
}

static void test_xray_config_grpc(void)
{
    const char *uri =
      "vless://u@h.tld:443?encryption=none&security=tls&type=grpc&path=mysvc&sni=h.tld#g";

    bmk_vless v;
    memset(&v, 0, sizeof v);
    bmk_vless_parse(uri, &v);

    char *cfg = bmk_vless_to_xray_config(&v, 40808);
    CHECK(cfg != NULL, "grpc 配置生成");
    if (cfg) {
        CHECK(strstr(cfg, "grpcSettings") != NULL, "含 grpcSettings");
        CHECK(strstr(cfg, "serviceName") != NULL, "含 serviceName");
        CHECK(strstr(cfg, "mysvc") != NULL, "path 映射为 serviceName");
        free(cfg);
    }
    bmk_vless_free(&v);
}

/* QA 第 5 项：用户显式停止的 VLESS 节点不得被自动重启。
 * 观察点：stop 后 next_endpoint 不选中该节点，且 restart_count 保持 0
 * （若误触发自动重启，restart_count 会递增、node_start 因无配置目录而失败）。 */
static void test_node_stop_no_restart(void)
{
    bmk_node n;
    memset(&n, 0, sizeof n);
    n.name = bmk_strdup("stop-test");
    n.type = BMK_NODE_VLESS;
    n.enabled = true;
    n.vless_uri = bmk_strdup("vless://uuid@example.org:443");

    int64_t id = 0;
    CHECK_EQ(bmk_proxy_node_add(&n, &id), BMK_OK, "添加 VLESS 节点");
    bmk_node_free(&n);
    CHECK(id > 0, "节点 id 有效");
    if (id <= 0) return;

    bmk_proxy_node_stop(id);

    char *ep = bmk_proxy_next_endpoint(NULL);
    CHECK(ep == NULL, "显式停止后不应被轮换选中/自动重启");
    free(ep);

    bmk_node *nd = bmk_proxy_node_at(0);
    CHECK(nd != NULL && nd->restart_count == 0,
          "显式停止后不应触发自动重启（restart_count 保持 0）");
    CHECK(nd != NULL && nd->stopped_explicitly,
          "stop 应置 stopped_explicitly 标记");

    CHECK_EQ(bmk_proxy_node_remove(id), BMK_OK, "清理节点");
}

int main(void)
{
    test_parse_full();
    test_parse_reality();
    test_parse_minimal();
    test_parse_invalid();
    test_xray_config_ws_tls();
    test_xray_config_reality();
    test_xray_config_none_security();
    test_xray_config_grpc();
    test_node_stop_no_restart();
    return test_report("vless 解析与 xray 配置（附录 C）");
}
