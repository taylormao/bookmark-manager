use crate::db::Database;
use crate::models::SyncResult;
use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct CloudCategory {
    pub id: Option<i64>,
    pub name: String,
    pub parent_id: Option<i64>,
    pub sort_order: Option<i64>,
    pub updated_at: i64,
    pub is_deleted: Option<i64>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct CloudBookmark {
    pub id: Option<i64>,
    pub title: String,
    pub url: String,
    pub url_key: Option<String>,
    pub icon: Option<String>,
    pub category_id: Option<i64>,
    pub category_name: Option<String>,
    pub tags: Option<Vec<String>>,
    pub valid: Option<i64>,
    pub created_at: Option<i64>,
    pub updated_at: i64,
    pub is_deleted: Option<i64>,
}

#[derive(Debug, Serialize, Deserialize)]
pub struct CloudSyncPullResponse {
    pub server_time: i64,
    pub categories: Vec<CloudCategory>,
    pub bookmarks: Vec<CloudBookmark>,
}

#[derive(Debug, Serialize, Deserialize)]
pub struct CloudSyncPushPayload {
    pub categories: Vec<CloudCategory>,
    pub bookmarks: Vec<CloudBookmark>,
}

#[derive(Debug, Serialize, Deserialize)]
pub struct CloudSyncPushResponse {
    pub success: bool,
    pub server_time: i64,
    pub message: Option<String>,
}

pub async fn perform_two_way_sync(db: &Database) -> Result<SyncResult, String> {
    let worker_url = db
        .get_setting("cf_worker_url")
        .map_err(|e| e.to_string())?
        .unwrap_or_default()
        .trim()
        .trim_end_matches('/')
        .to_string();

    if worker_url.is_empty() {
        return Err("请先在【云同步设置】中配置 Cloudflare Worker 网址".to_string());
    }

    let token = db
        .get_setting("cf_sync_token")
        .map_err(|e| e.to_string())?
        .unwrap_or_default()
        .trim()
        .to_string();

    let last_synced_at: i64 = db
        .get_setting("cf_last_synced_at")
        .map_err(|e| e.to_string())?
        .and_then(|s| s.parse().ok())
        .unwrap_or(0);

    let proxy_mode = db
        .get_setting("proxy_mode")
        .unwrap_or_default()
        .unwrap_or_else(|| "system".to_string());
    let proxy_url = db.get_setting("proxy_url").unwrap_or_default();
    let proxy_cfg = crate::fetch::ProxyConfig {
        mode: proxy_mode.clone(),
        url: proxy_url,
    };
    let client = crate::fetch::build_client_with_proxy(30, Some(&proxy_cfg));

    // 若当前为系统代理模式，准备一个直连客户端作为故障回退（应对代理工具如 v2ray/clash DNS缓存未刷新等情况）
    let direct_fallback_client = if proxy_mode == "system" {
        Some(crate::fetch::build_client_with_proxy(
            20,
            Some(&crate::fetch::ProxyConfig {
                mode: "direct".to_string(),
                url: None,
            }),
        ))
    } else {
        None
    };

    let format_net_err = |action: &str, e: reqwest::Error| -> String {
        let err_str = e.to_string();
        if err_str.contains("SEC_E") || err_str.contains("certificate") || err_str.contains("wrong principal") {
            format!("{}: SSL/TLS 证书校验失败 ({})。排查提示：若绑定了自定义域名，请确保在 Cloudflare Workers 后台绑定的是【自定义域 (Custom Domains)】而非【路由 (Routes)】，并确认 Cloudflare 已签发有效边缘证书", action, e)
        } else if err_str.contains("dns") || err_str.contains("resolve") {
            format!("{}: 域名 DNS 解析失败 ({})。请检查域名拼写或 DNS 记录是否生效", action, e)
        } else if e.is_timeout() {
            format!("{}: 请求超时 ({})。请检查网络连通性或当前网络代理设置", action, e)
        } else {
            format!("{}: {}", action, e)
        }
    };

    // 1. 获取本地待推送变更
    let (local_cats, local_bms) = db
        .get_sync_payload_since(last_synced_at)
        .map_err(|e| format!("获取本地待同步数据失败: {}", e))?;

    let pushed_count = local_cats.len() + local_bms.len();

    // 2. 推送本地变更到 Cloudflare (分批推送，每批 50 条，彻底避免超过 Worker 资源与请求体限制)
    if pushed_count > 0 {
        let push_url = format!("{}/api/sync", worker_url);
        const CHUNK_SIZE: usize = 50;

        let mut cats_to_send = Some(local_cats);
        let bm_chunks: Vec<Vec<CloudBookmark>> = if local_bms.is_empty() {
            vec![Vec::new()]
        } else {
            local_bms.chunks(CHUNK_SIZE).map(|c| c.to_vec()).collect()
        };

        for chunk in bm_chunks {
            let payload = CloudSyncPushPayload {
                categories: cats_to_send.take().unwrap_or_default(),
                bookmarks: chunk,
            };

            let make_push_req = |c: &reqwest::Client| {
                let mut req = c.post(&push_url).json(&payload);
                if !token.is_empty() {
                    req = req.header("Authorization", format!("Bearer {}", token));
                }
                req
            };

            let mut push_res = make_push_req(&client).send().await;

            // 若主客户端请求失败或返回 404/5xx，且存在直连回退客户端，则尝试直连
            let need_fallback = match &push_res {
                Ok(r) => !r.status().is_success(),
                Err(_) => true,
            };

            if need_fallback {
                if let Some(ref dc) = direct_fallback_client {
                    if let Ok(dr) = make_push_req(dc).send().await {
                        if dr.status().is_success() {
                            push_res = Ok(dr);
                        }
                    }
                }
            }

            let res = push_res.map_err(|e| format_net_err("推送变更至 Cloudflare 失败", e))?;

            if !res.status().is_success() {
                let status = res.status();
                let text = res.text().await.unwrap_or_default();
                return Err(format!("云端合并返回错误 ({}): {}", status, text));
            }
        }
    }

    // 3. 从 Cloudflare 拉取最新增量数据
    let pull_url = format!("{}/api/sync?since={}", worker_url, last_synced_at);
    let make_pull_req = |c: &reqwest::Client| {
        let mut req = c.get(&pull_url);
        if !token.is_empty() {
            req = req.header("Authorization", format!("Bearer {}", token));
        }
        req
    };

    let mut pull_res = make_pull_req(&client).send().await;

    let need_pull_fallback = match &pull_res {
        Ok(r) => !r.status().is_success(),
        Err(_) => true,
    };

    if need_pull_fallback {
        if let Some(ref dc) = direct_fallback_client {
            if let Ok(dr) = make_pull_req(dc).send().await {
                if dr.status().is_success() {
                    pull_res = Ok(dr);
                }
            }
        }
    }

    let res = pull_res.map_err(|e| format_net_err("从 Cloudflare 拉取数据失败", e))?;

    if !res.status().is_success() {
        let status = res.status();
        let text = res.text().await.unwrap_or_default();
        return Err(format!("云端拉取返回错误 ({}): {}", status, text));
    }

    let pull_data: CloudSyncPullResponse = res
        .json()
        .await
        .map_err(|e| format!("解析云端数据格式失败: {}", e))?;

    let pulled_count = pull_data.categories.len() + pull_data.bookmarks.len();

    // 4. 应用云端变更至本地 SQLite
    db.apply_cloud_sync_changes(&pull_data.categories, &pull_data.bookmarks)
        .map_err(|e| format!("合并云端数据至本地数据库失败: {}", e))?;

    // 5. 更新本地 last_synced_at
    let _ = db.set_setting("cf_last_synced_at", &pull_data.server_time.to_string());

    Ok(SyncResult {
        pushed_count,
        pulled_count,
        server_time: pull_data.server_time,
    })
}


