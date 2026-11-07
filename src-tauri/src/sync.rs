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

    let client = reqwest::Client::builder()
        .timeout(std::time::Duration::from_secs(25))
        .build()
        .map_err(|e| format!("创建 HTTP 客户端失败: {}", e))?;

    // 1. 获取本地待推送变更
    let (local_cats, local_bms) = db
        .get_sync_payload_since(last_synced_at)
        .map_err(|e| format!("获取本地待同步数据失败: {}", e))?;

    let pushed_count = local_cats.len() + local_bms.len();

    // 2. 推送本地变更到 Cloudflare
    if pushed_count > 0 {
        let push_url = format!("{}/api/sync", worker_url);
        let mut req = client.post(&push_url).json(&CloudSyncPushPayload {
            categories: local_cats,
            bookmarks: local_bms,
        });
        if !token.is_empty() {
            req = req.header("Authorization", format!("Bearer {}", token));
        }

        let res = req
            .send()
            .await
            .map_err(|e| format!("推送变更至 Cloudflare 失败: {}", e))?;

        if !res.status().is_success() {
            let status = res.status();
            let text = res.text().await.unwrap_or_default();
            return Err(format!("云端合并返回错误 ({}): {}", status, text));
        }
    }

    // 3. 从 Cloudflare 拉取最新增量数据
    let pull_url = format!("{}/api/sync?since={}", worker_url, last_synced_at);
    let mut req = client.get(&pull_url);
    if !token.is_empty() {
        req = req.header("Authorization", format!("Bearer {}", token));
    }

    let res = req
        .send()
        .await
        .map_err(|e| format!("从 Cloudflare 拉取数据失败: {}", e))?;

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
