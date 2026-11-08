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

fn get_sync_clients(db: &Database) -> (reqwest::Client, Option<reqwest::Client>) {
    let use_proxy = db
        .get_setting("cf_sync_use_proxy")
        .unwrap_or_default()
        .map(|v| v == "true" || v == "1")
        .unwrap_or(false);

    if use_proxy {
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
        let direct_fallback = Some(crate::fetch::build_client_with_proxy(
            20,
            Some(&crate::fetch::ProxyConfig {
                mode: "direct".to_string(),
                url: None,
            }),
        ));
        (client, direct_fallback)
    } else {
        let direct_cfg = crate::fetch::ProxyConfig {
            mode: "direct".to_string(),
            url: None,
        };
        let client = crate::fetch::build_client_with_proxy(30, Some(&direct_cfg));
        (client, None)
    }
}

fn format_sync_net_err(action: &str, e: reqwest::Error) -> String {
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

    let (client, direct_fallback_client) = get_sync_clients(db);

    // 1. 获取本地待推送增量变更
    let (mut local_cats, local_bms) = db
        .get_sync_payload_since(last_synced_at)
        .map_err(|e| format!("获取本地待同步数据失败: {}", e))?;

    let pushed_count = local_cats.len() + local_bms.len();

    // 2. 推送本地变更到 Cloudflare (分批推送，每批 50 条)
    if pushed_count > 0 {
        let push_url = format!("{}/api/sync", worker_url);
        const CHUNK_SIZE: usize = 50;

        let mut cats_to_send = Some(std::mem::take(&mut local_cats));
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

            let res = push_res.map_err(|e| format_sync_net_err("推送变更至 Cloudflare 失败", e))?;

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

    let res = pull_res.map_err(|e| format_sync_net_err("从 Cloudflare 拉取数据失败", e))?;

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

/// 全新覆盖上传：先清空云端数据库，再将本地全部数据（since=0）分批完整推送至云端
pub async fn perform_wipe_and_push_all(db: &Database) -> Result<SyncResult, String> {
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

    let (client, direct_fallback_client) = get_sync_clients(db);

    // 1. 调用云端清空接口 POST /api/wipe
    let wipe_url = format!("{}/api/wipe", worker_url);
    let make_wipe_req = |c: &reqwest::Client| {
        let mut req = c.post(&wipe_url);
        if !token.is_empty() {
            req = req.header("Authorization", format!("Bearer {}", token));
        }
        req
    };

    let mut wipe_res = make_wipe_req(&client).send().await;
    if wipe_res.as_ref().map(|r| !r.status().is_success()).unwrap_or(true) {
        if let Some(ref dc) = direct_fallback_client {
            if let Ok(dr) = make_wipe_req(dc).send().await {
                if dr.status().is_success() {
                    wipe_res = Ok(dr);
                }
            }
        }
    }

    let wipe_r = wipe_res.map_err(|e| format_sync_net_err("调用云端清空接口失败", e))?;
    if !wipe_r.status().is_success() {
        let status = wipe_r.status();
        let text = wipe_r.text().await.unwrap_or_default();
        return Err(format!("云端清空返回错误 ({}): {}", status, text));
    }

    // 2. 获取本地全部有效分类与书签 (since=0)
    let (mut local_cats, local_bms) = db
        .get_sync_payload_since(0)
        .map_err(|e| format!("获取本地待上传全量数据失败: {}", e))?;

    let pushed_count = local_cats.len() + local_bms.len();
    let push_url = format!("{}/api/sync", worker_url);
    const CHUNK_SIZE: usize = 50;

    let mut cats_to_send = Some(std::mem::take(&mut local_cats));
    let bm_chunks: Vec<Vec<CloudBookmark>> = if local_bms.is_empty() {
        vec![Vec::new()]
    } else {
        local_bms.chunks(CHUNK_SIZE).map(|c| c.to_vec()).collect()
    };

    let mut latest_server_time = chrono::Utc::now().timestamp();

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

        let res = push_res.map_err(|e| format_sync_net_err("推送全量数据至 Cloudflare 失败", e))?;

        if !res.status().is_success() {
            let status = res.status();
            let text = res.text().await.unwrap_or_default();
            return Err(format!("云端合并返回错误 ({}): {}", status, text));
        }

        if let Ok(push_resp) = res.json::<CloudSyncPushResponse>().await {
            latest_server_time = push_resp.server_time;
        }
    }

    // 3. 更新本地 last_synced_at
    let _ = db.set_setting("cf_last_synced_at", &latest_server_time.to_string());

    Ok(SyncResult {
        pushed_count,
        pulled_count: 0,
        server_time: latest_server_time,
    })
}

/// 全新覆盖下载：从云端拉取全量数据，并彻底清空覆盖本地 SQLite 数据
pub async fn perform_full_pull_and_overwrite(db: &Database) -> Result<SyncResult, String> {
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

    let (client, direct_fallback_client) = get_sync_clients(db);

    // 1. 拉取云端全量数据 since=0
    let pull_url = format!("{}/api/sync?since=0", worker_url);
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

    let res = pull_res.map_err(|e| format_sync_net_err("从 Cloudflare 拉取全量数据失败", e))?;

    if !res.status().is_success() {
        let status = res.status();
        let text = res.text().await.unwrap_or_default();
        return Err(format!("云端拉取返回错误 ({}): {}", status, text));
    }

    let pull_data: CloudSyncPullResponse = res
        .json()
        .await
        .map_err(|e| format!("解析云端全量数据格式失败: {}", e))?;

    let pulled_count = pull_data.categories.len() + pull_data.bookmarks.len();

    // 2. 清空并覆盖写入本地 SQLite
    db.overwrite_all_from_cloud(&pull_data.categories, &pull_data.bookmarks)
        .map_err(|e| format!("写入本地数据库失败: {}", e))?;

    // 3. 更新本地 last_synced_at
    let _ = db.set_setting("cf_last_synced_at", &pull_data.server_time.to_string());

    Ok(SyncResult {
        pushed_count: 0,
        pulled_count,
        server_time: pull_data.server_time,
    })
}


