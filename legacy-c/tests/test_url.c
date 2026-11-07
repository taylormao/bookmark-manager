/*
 * test_url.c - URL 归一化单元测试（附录 B 用例表逐条断言）
 */
#include "test_util.h"
#include "bmk_url.h"

static void expect_key(const char *input, const char *expect)
{
    char *key = NULL;
    bmk_err e = bmk_url_normalize(input, &key);
    if (e != BMK_OK) {
        g_tests++;
        g_fails++;
        fprintf(stderr, "FAIL 归一化失败 %s -> %s（期望 %s）\n",
                input, bmk_err_str(e), expect);
        return;
    }
    CHECK_STR(key, expect, "归一化 %s", input);
    free(key);
}

/* 附录 B 用例表 1-17 */
static void test_appendix_b(void)
{
    expect_key("https://example.com/abc?utm_source=x&id=1", "https://example.com/abc");
    expect_key("http://example.com/abc?utm_source=y",       "https://example.com/abc");
    expect_key("https://www.example.com/abc/",              "https://example.com/abc");
    expect_key("https://example.com/ABC",                   "https://example.com/ABC");
    expect_key("https://example.com/abc#section",           "https://example.com/abc");
    expect_key("https://example.com:443/abc",               "https://example.com/abc");
    expect_key("https://bilibili.com/video/BV1xx?p=1&from=search",
               "https://bilibili.com/video/BV1xx?p=1");
    expect_key("https://www.bilibili.com/video/BV1xx?p=1",
               "https://bilibili.com/video/BV1xx?p=1");
    expect_key("https://bilibili.com/video/BV1xx?p=2",
               "https://bilibili.com/video/BV1xx?p=2");
    expect_key("https://youtube.com/watch?v=abc&list=xyz",
               "https://youtube.com/watch?v=abc");
    expect_key("https://youtube.com/watch?v=abc", "https://youtube.com/watch?v=abc");
    expect_key("https://youtube.com/watch?v=abd", "https://youtube.com/watch?v=abd");
    expect_key("https://example.com/a/./b",  "https://example.com/a/b");
    expect_key("https://example.com/a/../b", "https://example.com/b");
    expect_key("https://example.com/?",      "https://example.com/");
    expect_key("https://bilibili.com/video/BV1xx?p=1&p=2",
               "https://bilibili.com/video/BV1xx?p=1&p=2");
    expect_key("javascript:alert(1)", "javascript:alert(1)");
}

/* 附录 B 管线的补充边界 */
static void test_edge_cases(void)
{
    /* 空 path 视为 "/" */
    expect_key("https://example.com", "https://example.com/");
    /* 显式 http 默认端口 */
    expect_key("http://example.com:80/x", "https://example.com/x");
    /* https 的 80 端口不是默认端口，应保留（删除默认端口仅限 http=80 / https=443） */
    expect_key("https://example.com:80/x", "https://example.com:80/x");
    /* 非默认端口保留（http 的 443 端口也非默认） */
    expect_key("https://example.com:8080/x", "https://example.com:8080/x");
    expect_key("http://example.com:8080/x", "https://example.com:8080/x");
    /* scheme/host 大小写 */
    expect_key("HTTPS://Www.Example.COM/abc", "https://example.com/abc");
    /* fragment + query 同时存在 */
    expect_key("https://youtube.com/watch?v=abc#t=10", "https://youtube.com/watch?v=abc");
    /* .. 越界保留原样不报错 */
    expect_key("https://example.com/../a", "https://example.com/a");
    /* 子域名命中白名单后缀 */
    expect_key("https://m.bilibili.com/video/BV1?p=3&x=1",
               "https://m.bilibili.com/video/BV1?p=3");
    /* 非 http(s)：去 fragment 后原样 */
    expect_key("mailto:a@b.com", "mailto:a@b.com");
    /* 路径百分号解码一次 */
    expect_key("https://example.com/a%2Fb", "https://example.com/a/b");
    /* 末尾斜杠仅在长度 > 1 时去除 */
    expect_key("https://example.com/", "https://example.com/");
}

static void test_valid_input(void)
{
    const char *reason = NULL;
    CHECK(bmk_url_is_valid_input("https://example.com/a", &reason), "https 应合法");
    CHECK(bmk_url_is_valid_input("http://example.com", &reason), "http 应合法");
    CHECK(!bmk_url_is_valid_input("javascript:alert(1)", &reason),
          "输入校验应拒绝非 http(s)");
    CHECK(!bmk_url_is_valid_input("https://exa mple.com", &reason),
          "输入校验应拒绝含空格");
    CHECK(!bmk_url_is_valid_input("https://", &reason), "输入校验应拒绝空 host");
    CHECK(!bmk_url_is_valid_input("", &reason), "输入校验应拒绝空串");
    CHECK(!bmk_url_is_valid_input(NULL, &reason), "输入校验应拒绝 NULL");
}

static void test_host(void)
{
    char *h = bmk_url_host("https://Www.Example.COM/abc");
    CHECK_STR(h, "example.com", "host 归一化");
    free(h);

    h = bmk_url_host("https://sub.example.com:8443/x");
    CHECK_STR(h, "sub.example.com", "保留非 www 子域");
    free(h);
}

static void test_dot_segments(void)
{
    char *p;
    p = bmk_url_remove_dot_segments("/a/./b");   CHECK_STR(p, "/a/b", "点段 ./"); free(p);
    p = bmk_url_remove_dot_segments("/a/../b");  CHECK_STR(p, "/b", "点段 ../"); free(p);
    p = bmk_url_remove_dot_segments("/a/b/../../c"); CHECK_STR(p, "/c", "多级 ../"); free(p);
    p = bmk_url_remove_dot_segments("/../a");    CHECK_STR(p, "/a", "越界 ../"); free(p);
    p = bmk_url_remove_dot_segments("/a/b/");    CHECK_STR(p, "/a/b/", "保留尾斜杠"); free(p);
}

static void test_resolve(void)
{
    char *u = bmk_url_resolve("https://example.com/a/b", "/c/d");
    CHECK_STR(u, "https://example.com/c/d", "绝对路径解析");
    free(u);

    u = bmk_url_resolve("https://example.com/a/b", "c.ico");
    CHECK_STR(u, "https://example.com/a/c.ico", "相对路径解析");
    free(u);

    u = bmk_url_resolve("https://example.com/a/b", "//cdn.example.com/x.png");
    CHECK_STR(u, "https://cdn.example.com/x.png", "协议相对解析");
    free(u);
}

int main(void)
{
    test_appendix_b();
    test_edge_cases();
    test_valid_input();
    test_host();
    test_dot_segments();
    test_resolve();
    return test_report("url 归一化（附录 B）");
}
