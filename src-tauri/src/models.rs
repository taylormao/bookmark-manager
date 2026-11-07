use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Category {
    pub id: i64,
    pub name: String,
    pub parent_id: Option<i64>,
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
    pub valid_state: Option<String>, // "all", "valid", "invalid", "unknown", "blocked"
    pub check_note: Option<String>,  // 具体失效原因过滤，如 "timeout", "HTTP 404"
    pub sort_by: Option<String>,     // "created_desc", "created_asc", "title_asc", "url_asc"
}
