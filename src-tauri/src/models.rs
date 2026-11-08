use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Category {
    pub id: i64,
    pub name: String,
    pub parent_id: Option<i64>,
    pub sort_order: i64,
    pub updated_at: Option<i64>,
    pub is_deleted: Option<i64>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct SyncConfig {
    pub worker_url: String,
    pub token: String,
    pub auto_sync: bool,
    pub use_proxy: Option<bool>,
    pub last_synced_at: Option<i64>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct SyncResult {
    pub pushed_count: usize,
    pub pulled_count: usize,
    pub server_time: i64,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Tag {
    pub id: i64,
    pub name: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Bookmark {
    pub id: i64,
    pub title: String,
    pub url: String,
    pub url_key: String,
    pub icon_data_url: Option<String>,
    pub category_id: Option<i64>,
    pub category_name: Option<String>,
    pub valid: Option<i32>,
    pub status_code: Option<i32>,
    pub last_checked_at: Option<i64>,
    pub check_note: Option<String>,
    pub tags: Vec<String>,
    pub created_at: i64,
    pub updated_at: i64,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct BookmarkCreateInput {
    pub title: String,
    pub url: String,
    pub category_id: Option<i64>,
    pub tags: Vec<String>,
    pub icon_data_url: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct BookmarkUpdateInput {
    pub id: i64,
    pub title: String,
    pub url: String,
    pub category_id: Option<i64>,
    pub tags: Vec<String>,
    pub icon_data_url: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct BookmarkFilter {
    pub search: Option<String>,
    pub category_id: Option<i64>,
    pub tag: Option<String>,
    pub valid_state: Option<String>,
    pub check_note: Option<String>,
    pub sort_by: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ImportResult {
    pub total: usize,
    pub inserted: usize,
    pub skipped: usize,
}

