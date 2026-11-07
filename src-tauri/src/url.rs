use url::Url;

/// 对字符串进行一次百分号解码（保持无效 UTF-8 容错或替换）
pub fn percent_decode_once(s: &str) -> String {
    percent_encoding::percent_decode_str(s)
        .decode_utf8_lossy()
        .to_string()
}

/// RFC 3986 5.2.4 点段归一化 (Remove Dot Segments)
pub fn remove_dot_segments(path: &str) -> String {
    if path.is_empty() {
        return "/".to_string();
    }

    let is_absolute = path.starts_with('/');
    let mut segments: Vec<&str> = Vec::new();
    let mut trailing_slash = false;

    let parts = path.split('/');
    for (idx, seg) in parts.enumerate() {
        if idx == 0 && is_absolute && seg.is_empty() {
            continue;
        }
        if seg == "." {
            trailing_slash = true;
        } else if seg == ".." {
            if !segments.is_empty() {
                segments.pop();
            }
            trailing_slash = true;
        } else if seg.is_empty() {
            // 多个斜杠或结尾斜杠
            trailing_slash = true;
        } else {
            segments.push(seg);
            trailing_slash = false;
        }
    }

    let mut result = String::new();
    if is_absolute {
        result.push('/');
    }
    result.push_str(&segments.join("/"));

    if trailing_slash && !result.ends_with('/') {
        result.push('/');
    }

    if result.is_empty() {
        "/".to_string()
    } else {
        result
    }
}

/// 按照附录 B 实现 URL 归一化（返回用于去重的 url_key）
pub fn normalize_url(raw: &str) -> Result<String, String> {
    let raw = raw.trim();
    if raw.is_empty() {
        return Err("URL cannot be empty".to_string());
    }

    // 尝试解析 URL
    let parsed = match Url::parse(raw) {
        Ok(u) => u,
        Err(_) => {
            // 如果是非标准 scheme 或其它无法直接解析的，但包含 scheme
            if let Some(pos) = raw.find(':') {
                let scheme = &raw[..pos];
                if scheme.chars().all(|c| c.is_ascii_alphanumeric() || c == '+' || c == '-' || c == '.') {
                    // 非 http(s) 原样（去 fragment）
                    let no_frag = match raw.find('#') {
                        Some(f_idx) => &raw[..f_idx],
                        None => raw,
                    };
                    return Ok(no_frag.to_string());
                }
            }
            return Err("Invalid URL format".to_string());
        }
    };

    let scheme = parsed.scheme().to_ascii_lowercase();

    // 仅 http / https 执行完整管线，其它 scheme 原样返回（去掉 fragment）
    if scheme != "http" && scheme != "https" {
        let mut cloned = parsed.clone();
        cloned.set_fragment(None);
        return Ok(cloned.to_string());
    }

    // 规则 2: scheme 归一为 https（用例 2: http://example.com/abc -> https://example.com/abc）
    let norm_scheme = "https";

    // 规则 3: host 转小写，并剥离前缀 "www."
    let host_str = parsed.host_str().ok_or_else(|| "Missing host".to_string())?.to_ascii_lowercase();
    let norm_host = if host_str.starts_with("www.") && host_str.len() > 4 {
        &host_str[4..]
    } else {
        &host_str
    };

    // 规则 4: 端口处理（删除默认端口 http=80, https=443）
    let port_str = match parsed.port() {
        Some(p) if (scheme == "http" && p == 80) || (scheme == "https" && p == 443) => String::new(),
        Some(p) => format!(":{}", p),
        None => String::new(),
    };

    // 规则 6: path 处理
    // 保留大小写；百分号解码一次；RFC 3986 点段归一；结果长度>1 时去掉末尾 `/`；空 path 视为 `/`
    let raw_path = parsed.path();
    let decoded_path = percent_decode_once(raw_path);
    let normalized_path = remove_dot_segments(&decoded_path);
    let mut final_path = if normalized_path.len() > 1 && normalized_path.ends_with('/') {
        normalized_path[..normalized_path.len() - 1].to_string()
    } else {
        normalized_path
    };
    if final_path.is_empty() {
        final_path = "/".to_string();
    }

    // 规则 7 & 8: query 拆分、百分号解码、白名单过滤
    let mut query_params: Vec<(String, String)> = Vec::new();

    if let Some(query_str) = parsed.query() {
        if !query_str.is_empty() {
            let is_bilibili = norm_host == "bilibili.com" || norm_host.ends_with(".bilibili.com");
            let is_youtube = norm_host == "youtube.com" || norm_host.ends_with(".youtube.com");

            for pair in query_str.split('&') {
                if pair.is_empty() {
                    continue;
                }
                let mut parts = pair.splitn(2, '=');
                let name = percent_decode_once(parts.next().unwrap_or(""));
                let val = percent_decode_once(parts.next().unwrap_or(""));

                if name.is_empty() {
                    continue;
                }

                // 白名单过滤
                let keep = if is_bilibili {
                    name == "p"
                } else if is_youtube {
                    name == "v"
                } else {
                    false
                };

                if keep {
                    query_params.push((name, val));
                }
            }
        }
    }

    // 规则 9: 保留的参数按 (name, value) 升序排序（UTF-8 字节序）
    query_params.sort_by(|a, b| {
        match a.0.cmp(&b.0) {
            std::cmp::Ordering::Equal => a.1.cmp(&b.1),
            other => other,
        }
    });

    let query_suffix = if !query_params.is_empty() {
        let pairs: Vec<String> = query_params
            .into_iter()
            .map(|(k, v)| format!("{}={}", k, v))
            .collect();
        format!("?{}", pairs.join("&"))
    } else {
        String::new()
    };

    // 规则 10: 拼接
    let final_url = format!("{}://{}{}{}{}", norm_scheme, norm_host, port_str, final_path, query_suffix);

    // 如果 path 是 "/" 且没有 query 并且有主机，保留 https://example.com/ 形式与用例一致
    Ok(final_url)
}

#[cfg(test)]
mod tests {
    use super::*;

    // 附录 B 中的 17 条严谨用例测试
    #[test]
    fn test_appendix_b_all_17_cases() {
        let cases = vec![
            (
                "https://example.com/abc?utm_source=x&id=1",
                "https://example.com/abc",
                "Case 1: 非白名单，参数全删",
            ),
            (
                "http://example.com/abc?utm_source=y",
                "https://example.com/abc",
                "Case 2: scheme 归一，与 #1 重复",
            ),
            (
                "https://www.example.com/abc/",
                "https://example.com/abc",
                "Case 3: 剥离 www、去尾斜杠",
            ),
            (
                "https://example.com/ABC",
                "https://example.com/ABC",
                "Case 4: 路径大小写敏感",
            ),
            (
                "https://example.com/abc#section",
                "https://example.com/abc",
                "Case 5: fragment 删除",
            ),
            (
                "https://example.com:443/abc",
                "https://example.com/abc",
                "Case 6: 默认端口删除",
            ),
            (
                "https://bilibili.com/video/BV1xx?p=1&from=search",
                "https://bilibili.com/video/BV1xx?p=1",
                "Case 7: 白名单只留 p",
            ),
            (
                "https://www.bilibili.com/video/BV1xx?p=1",
                "https://bilibili.com/video/BV1xx?p=1",
                "Case 8: 与 #7 重复",
            ),
            (
                "https://bilibili.com/video/BV1xx?p=2",
                "https://bilibili.com/video/BV1xx?p=2",
                "Case 9: p 值不同 = 不同",
            ),
            (
                "https://youtube.com/watch?v=abc&list=xyz",
                "https://youtube.com/watch?v=abc",
                "Case 10: 白名单只留 v",
            ),
            (
                "https://youtube.com/watch?v=abc",
                "https://youtube.com/watch?v=abc",
                "Case 11: 与 #10 重复",
            ),
            (
                "https://youtube.com/watch?v=abd",
                "https://youtube.com/watch?v=abd",
                "Case 12: v 值不同 = 不同",
            ),
            (
                "https://example.com/a/./b",
                "https://example.com/a/b",
                "Case 13: 点段归一",
            ),
            (
                "https://example.com/a/../b",
                "https://example.com/b",
                "Case 14: 点段归一",
            ),
            (
                "https://example.com/?",
                "https://example.com/",
                "Case 15: 空参数视为无参数",
            ),
            (
                "https://bilibili.com/video/BV1xx?p=1&p=2",
                "https://bilibili.com/video/BV1xx?p=1&p=2",
                "Case 16: 同名多值按值排序保留",
            ),
            (
                "javascript:alert(1)",
                "javascript:alert(1)",
                "Case 17: 非 http(s) 原样（去 fragment）",
            ),
        ];

        for (input, expected, desc) in cases {
            let actual = normalize_url(input).unwrap();
            assert_eq!(actual, expected, "Failed for {}", desc);
        }
    }
}
