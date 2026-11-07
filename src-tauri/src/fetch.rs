use base64::engine::general_purpose::STANDARD as BASE64;
use base64::Engine;
use encoding_rs::{Encoding, GBK, UTF_8};
use reqwest::header::{HeaderMap, HeaderValue, ACCEPT, ACCEPT_LANGUAGE, CONTENT_TYPE, USER_AGENT};
use reqwest::{Client, Proxy};
use scraper::{Html, Selector};
use serde::{Deserialize, Serialize};
use std::time::{Duration, Instant};

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ProxyConfig {
    pub mode: String,          // "direct", "system", "custom"
    pub url: Option<String>,   // 例如 "http://127.0.0.1:7890" 或 "socks5://127.0.0.1:10808"
}

impl Default for ProxyConfig {
    fn default() -> Self {
        Self {
            mode: "system".to_string(),
            url: None,
        }
    }
}

#[derive(Debug, Clone, Serialize)]
pub struct FetchMetadataResult {
    pub title: Option<String>,
    pub favicon_data_url: Option<String>,
}

#[derive(Debug, Clone, Serialize)]
pub struct CheckResult {
    pub id: i64,
    pub valid: Option<i32>,
    pub status_code: i32,
    pub check_note: Option<String>,
}

#[derive(Debug, Clone, Serialize)]
pub struct ProxyTestResult {
    pub success: bool,
    pub latency_ms: Option<u128>,
    pub error: Option<String>,
}

/// 确保 URL 带有协议头（默认补 https://）
pub fn ensure_scheme(url_str: &str) -> String {
    let trimmed = url_str.trim();
    if !trimmed.contains("://") {
        format!("https://{}", trimmed)
    } else {
        trimmed.to_string()
    }
}

/// 创建带代理支持与真实浏览器特征的 HTTP 客户端
pub fn build_client_with_proxy(timeout_secs: u64, proxy_cfg: Option<&ProxyConfig>) -> Client {
    let mut headers = HeaderMap::new();
    headers.insert(
        USER_AGENT,
        HeaderValue::from_static("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36"),
    );
    headers.insert(
        ACCEPT,
        HeaderValue::from_static("text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,*/*;q=0.8"),
    );
    headers.insert(
        ACCEPT_LANGUAGE,
        HeaderValue::from_static("zh-CN,zh;q=0.9,en;q=0.8"),
    );

    let mut builder = Client::builder()
        .timeout(Duration::from_secs(timeout_secs))
        .default_headers(headers)
        .redirect(reqwest::redirect::Policy::limited(8))
        .danger_accept_invalid_certs(true);

    if let Some(cfg) = proxy_cfg {
        match cfg.mode.as_str() {
            "custom" => {
                if let Some(ref proxy_url) = cfg.url {
                    let trimmed = proxy_url.trim();
                    if !trimmed.is_empty() {
                        if let Ok(proxy) = Proxy::all(trimmed) {
                            builder = builder.proxy(proxy);
                        }
                    }
                }
            }
            "direct" => {
                builder = builder.no_proxy();
            }
            _ => {
                // "system": 默认使用系统环境代理
            }
        }
    }

    builder.build().unwrap_or_else(|_| Client::new())
}

/// 测试代理服务器连通性
pub async fn test_proxy_connection(proxy_url: &str) -> ProxyTestResult {
    let trimmed = proxy_url.trim();
    if trimmed.is_empty() {
        return ProxyTestResult {
            success: false,
            latency_ms: None,
            error: Some("代理地址不能为空".to_string()),
        };
    }

    let proxy = match Proxy::all(trimmed) {
        Ok(p) => p,
        Err(e) => {
            return ProxyTestResult {
                success: false,
                latency_ms: None,
                error: Some(format!("代理地址格式错误: {}", e)),
            };
        }
    };

    let client = match Client::builder()
        .timeout(Duration::from_secs(6))
        .proxy(proxy)
        .build()
    {
        Ok(c) => c,
        Err(e) => {
            return ProxyTestResult {
                success: false,
                latency_ms: None,
                error: Some(format!("创建测试连接失败: {}", e)),
            };
        }
    };

    let start = Instant::now();
    // 使用全球高可用连通性测试端点
    let test_target = "https://www.google.com/generate_204";
    match client.get(test_target).send().await {
        Ok(resp) => {
            let elapsed = start.elapsed().as_millis();
            let status = resp.status().as_u16();
            if (200..=399).contains(&status) || status == 204 {
                ProxyTestResult {
                    success: true,
                    latency_ms: Some(elapsed),
                    error: None,
                }
            } else {
                ProxyTestResult {
                    success: false,
                    latency_ms: Some(elapsed),
                    error: Some(format!("代理响应异常状态码: HTTP {}", status)),
                }
            }
        }
        Err(e) => {
            // 如果连接 google 超时，尝试 cloudflare 作为备用
            let fallback_target = "https://1.1.1.1";
            match client.get(fallback_target).send().await {
                Ok(_) => ProxyTestResult {
                    success: true,
                    latency_ms: Some(start.elapsed().as_millis()),
                    error: None,
                },
                Err(_) => ProxyTestResult {
                    success: false,
                    latency_ms: None,
                    error: Some(format!("连接超时或失败: {}", e)),
                },
            }
        }
    }
}

/// 智能解码响应字节流（自动识别 UTF-8 / GBK / GB2312）
fn decode_html_bytes(bytes: &[u8], content_type_header: Option<&str>) -> String {
    let mut encoding: Option<&'static Encoding> = None;
    if let Some(ct) = content_type_header {
        let ct_lower = ct.to_ascii_lowercase();
        if ct_lower.contains("gbk") || ct_lower.contains("gb2312") || ct_lower.contains("gb18030") {
            encoding = Some(GBK);
        } else if ct_lower.contains("utf-8") {
            encoding = Some(UTF_8);
        }
    }

    if encoding.is_none() {
        let probe_len = bytes.len().min(2048);
        let probe_str = String::from_utf8_lossy(&bytes[..probe_len]).to_ascii_lowercase();
        if probe_str.contains("charset=gbk")
            || probe_str.contains("charset=gb2312")
            || probe_str.contains("charset=\"gbk\"")
            || probe_str.contains("charset=\"gb2312\"")
        {
            encoding = Some(GBK);
        }
    }

    let chosen_encoding = encoding.unwrap_or(UTF_8);
    let (cow, _, _) = chosen_encoding.decode(bytes);
    cow.into_owned()
}

/// 抓取指定图片的二进制并转为 Base64 Data URL
pub async fn fetch_image_as_data_url(client: &Client, icon_url: &str) -> Option<String> {
    let resp = client.get(icon_url).send().await.ok()?;
    if !resp.status().is_success() {
        return None;
    }

    let mime = resp
        .headers()
        .get(CONTENT_TYPE)
        .and_then(|v| v.to_str().ok())
        .map(|s| s.to_string())
        .unwrap_or_else(|| {
            if icon_url.ends_with(".png") {
                "image/png".to_string()
            } else if icon_url.ends_with(".svg") {
                "image/svg+xml".to_string()
            } else {
                "image/x-icon".to_string()
            }
        });

    let bytes = resp.bytes().await.ok()?;
    if bytes.is_empty() || bytes.len() > 512 * 1024 {
        return None;
    }

    let b64 = BASE64.encode(&bytes);
    Some(format!("data:{};base64,{}", mime, b64))
}

/// 抓取网页元数据（Title 与 Favicon Data URL，带代理支持）
pub async fn fetch_url_metadata(raw_url: &str, proxy_cfg: Option<&ProxyConfig>) -> Result<FetchMetadataResult, String> {
    let full_url = ensure_scheme(raw_url);
    let client = build_client_with_proxy(8, proxy_cfg);

    let resp = client.get(&full_url).send().await.map_err(|e| e.to_string())?;

    let final_url = resp.url().clone();
    let content_type = resp
        .headers()
        .get(CONTENT_TYPE)
        .and_then(|v| v.to_str().ok())
        .map(|s| s.to_string());

    let bytes = resp.bytes().await.map_err(|e| e.to_string())?;
    let body = decode_html_bytes(&bytes, content_type.as_deref());

    let (title, icon_url) = {
        let document = Html::parse_document(&body);

        let mut title: Option<String> = None;
        if let Ok(og_selector) = Selector::parse(r#"meta[property="og:title"]"#) {
            if let Some(og) = document.select(&og_selector).next() {
                if let Some(content) = og.value().attr("content") {
                    let t = content.trim();
                    if !t.is_empty() {
                        title = Some(t.to_string());
                    }
                }
            }
        }

        if title.is_none() {
            if let Ok(title_selector) = Selector::parse("title") {
                if let Some(t_elem) = document.select(&title_selector).next() {
                    let t = t_elem.text().collect::<Vec<_>>().join("").trim().to_string();
                    if !t.is_empty() {
                        title = Some(t);
                    }
                }
            }
        }

        let mut icon_url: Option<String> = None;
        if let Ok(icon_selector) = Selector::parse(r#"link[rel*="icon"]"#) {
            if let Some(icon_elem) = document.select(&icon_selector).next() {
                if let Some(href) = icon_elem.value().attr("href") {
                    if let Ok(joined) = final_url.join(href) {
                        icon_url = Some(joined.to_string());
                    }
                }
            }
        }

        if icon_url.is_none() {
            if let Ok(default_ico) = final_url.join("/favicon.ico") {
                icon_url = Some(default_ico.to_string());
            }
        }

        (title, icon_url)
    };

    let mut favicon_data_url: Option<String> = None;
    if let Some(ref u) = icon_url {
        favicon_data_url = fetch_image_as_data_url(&client, u).await;
    }

    Ok(FetchMetadataResult {
        title,
        favicon_data_url,
    })
}

/// 检查单条 URL 的有效性（带代理支持）
pub async fn check_url_validity(
    id: i64,
    url_str: &str,
    current_valid: Option<i32>,
    proxy_cfg: Option<&ProxyConfig>,
) -> CheckResult {
    let full_url = ensure_scheme(url_str);
    let client = build_client_with_proxy(8, proxy_cfg);

    match client.get(&full_url).send().await {
        Ok(resp) => {
            let status = resp.status().as_u16() as i32;

            if (200..=399).contains(&status) {
                CheckResult {
                    id,
                    valid: Some(1),
                    status_code: status,
                    check_note: None,
                }
            } else if status == 403 || status == 429 {
                CheckResult {
                    id,
                    valid: current_valid,
                    status_code: status,
                    check_note: Some("blocked".to_string()),
                }
            } else {
                CheckResult {
                    id,
                    valid: Some(0),
                    status_code: status,
                    check_note: Some(format!("HTTP {}", status)),
                }
            }
        }
        Err(err) => {
            let err_msg = if err.is_timeout() {
                "timeout".to_string()
            } else {
                "network_error".to_string()
            };

            CheckResult {
                id,
                valid: Some(0),
                status_code: 0,
                check_note: Some(err_msg),
            }
        }
    }
}
