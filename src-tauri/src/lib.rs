pub mod ai;
pub mod db;
pub mod fetch;
pub mod models;
pub mod netscape;
pub mod sync;
pub mod url;

use db::Database;
use fetch::ProxyConfig;
use models::{
    Bookmark, BookmarkCreateInput, BookmarkFilter, BookmarkUpdateInput, Category, SyncConfig,
    SyncResult, Tag,
};
use std::sync::Arc;
use tauri::{Manager, State};

pub struct AppState {
    pub db: Arc<Database>,
}

// 辅助函数：获取当前持久化的代理配置
fn load_current_proxy(db: &Database) -> ProxyConfig {
    let mode = db.get_setting("proxy_mode").unwrap_or_default().unwrap_or_else(|| "system".to_string());
    let url = db.get_setting("proxy_url").unwrap_or_default();
    ProxyConfig { mode, url }
}

#[tauri::command]
fn get_categories(state: State<'_, AppState>) -> Result<Vec<Category>, String> {
    state.db.get_categories().map_err(|e| e.to_string())
}

#[tauri::command]
fn create_category(name: String, parent_id: Option<i64>, state: State<'_, AppState>) -> Result<i64, String> {
    state.db.create_category(&name, parent_id).map_err(|e| e.to_string())
}

#[tauri::command]
fn delete_category(id: i64, state: State<'_, AppState>) -> Result<(), String> {
    state.db.delete_category(id).map_err(|e| e.to_string())
}

#[tauri::command]
fn rename_category(id: i64, name: String, state: State<'_, AppState>) -> Result<(), String> {
    state.db.rename_category(id, &name)
}

#[tauri::command]
fn update_category_parent(id: i64, parent_id: Option<i64>, state: State<'_, AppState>) -> Result<(), String> {
    state.db.update_category_parent(id, parent_id)
}

#[tauri::command]
fn reorder_categories(ordered_ids: Vec<i64>, state: State<'_, AppState>) -> Result<(), String> {
    state.db.reorder_categories(&ordered_ids).map_err(|e| e.to_string())
}

#[tauri::command]
fn get_tags(state: State<'_, AppState>) -> Result<Vec<Tag>, String> {
    state.db.get_tags().map_err(|e| e.to_string())
}

#[tauri::command]
fn get_failure_reasons(state: State<'_, AppState>) -> Result<Vec<String>, String> {
    state.db.get_failure_reasons().map_err(|e| e.to_string())
}

#[tauri::command]
fn list_bookmarks(filter: BookmarkFilter, state: State<'_, AppState>) -> Result<Vec<Bookmark>, String> {
    state.db.list_bookmarks(&filter).map_err(|e| e.to_string())
}

#[tauri::command]
async fn create_bookmark(mut input: BookmarkCreateInput, state: State<'_, AppState>) -> Result<i64, String> {
    input.url = fetch::ensure_scheme(&input.url);
    let proxy_cfg = load_current_proxy(&state.db);

    if input.title.trim().is_empty() || input.icon_data_url.is_none() {
        if let Ok(meta) = fetch::fetch_url_metadata(&input.url, Some(&proxy_cfg)).await {
            if input.title.trim().is_empty() {
                if let Some(t) = meta.title {
                    input.title = t;
                }
            }
            if input.icon_data_url.is_none() {
                input.icon_data_url = meta.favicon_data_url;
            }
        }
    }
    if input.title.trim().is_empty() {
        input.title = input.url.clone();
    }

    let is_category_empty = input.category_id.is_none();
    let is_tags_empty = input.tags.is_empty();

    if is_category_empty || is_tags_empty {
        let existing_cats: Vec<String> = state
            .db
            .get_categories()
            .unwrap_or_default()
            .into_iter()
            .map(|c| c.name)
            .collect();

        let suggestion = ai::suggest_category_and_tags(&input.url, &input.title, &existing_cats);

        if is_category_empty {
            let cat_id = match state
                .db
                .get_categories()
                .unwrap_or_default()
                .into_iter()
                .find(|c| c.name.eq_ignore_ascii_case(&suggestion.category_name))
            {
                Some(existing) => existing.id,
                None => state
                    .db
                    .create_category(&suggestion.category_name, None)
                    .unwrap_or(0),
            };
            if cat_id > 0 {
                input.category_id = Some(cat_id);
            }

            if !input.tags.iter().any(|t| t == "ai辅助") {
                input.tags.push("ai辅助".to_string());
            }
        }

        if is_tags_empty {
            for tag in suggestion.tags {
                if !input.tags.contains(&tag) {
                    input.tags.push(tag);
                }
            }
        }
    }

    state.db.create_bookmark(&input).map_err(|e| e.to_string())
}

#[tauri::command]
fn update_bookmark(mut input: BookmarkUpdateInput, state: State<'_, AppState>) -> Result<(), String> {
    input.url = fetch::ensure_scheme(&input.url);
    state.db.update_bookmark(&input).map_err(|e| e.to_string())
}

#[tauri::command]
fn set_bookmark_category(id: i64, category_id: Option<i64>, state: State<'_, AppState>) -> Result<(), String> {
    state.db.update_bookmark_category(id, category_id).map_err(|e| e.to_string())
}

#[tauri::command]
fn batch_set_bookmarks_category(ids: Vec<i64>, category_id: Option<i64>, state: State<'_, AppState>) -> Result<(), String> {
    state.db.batch_update_bookmarks_category(&ids, category_id).map_err(|e| e.to_string())
}

#[tauri::command]
fn delete_bookmark(id: i64, state: State<'_, AppState>) -> Result<(), String> {
    state.db.delete_bookmark(id).map_err(|e| e.to_string())
}

#[tauri::command]
fn import_bookmarks_html(content: String, state: State<'_, AppState>) -> Result<usize, String> {
    let items = netscape::parse_netscape_html(&content);
    state.db.import_netscape_bookmarks(&items).map_err(|e| e.to_string())
}

#[tauri::command]
fn export_bookmarks_html(state: State<'_, AppState>) -> Result<String, String> {
    let bookmarks = state
        .db
        .list_bookmarks(&BookmarkFilter {
            search: None,
            category_id: None,
            tag: None,
            valid_state: None,
            check_note: None,
            sort_by: Some("created_asc".to_string()),
        })
        .map_err(|e| e.to_string())?;

    let categories = state.db.get_categories().map_err(|e| e.to_string())?;
    Ok(netscape::export_netscape_html(&bookmarks, &categories))
}

#[tauri::command]
fn find_duplicates(state: State<'_, AppState>) -> Result<Vec<Vec<Bookmark>>, String> {
    state.db.find_duplicates().map_err(|e| e.to_string())
}

#[tauri::command]
fn merge_duplicates(state: State<'_, AppState>) -> Result<usize, String> {
    state.db.merge_duplicates().map_err(|e| e.to_string())
}

// 抓取 Title 与 Favicon（自动注入代理）
#[tauri::command]
async fn fetch_metadata(url: String, state: State<'_, AppState>) -> Result<fetch::FetchMetadataResult, String> {
    let proxy_cfg = load_current_proxy(&state.db);
    fetch::fetch_url_metadata(&url, Some(&proxy_cfg)).await
}

#[tauri::command]
fn suggest_ai_metadata(
    url: String,
    title: String,
    state: State<'_, AppState>,
) -> Result<ai::AiSuggestion, String> {
    let existing_cats: Vec<String> = state
        .db
        .get_categories()
        .unwrap_or_default()
        .into_iter()
        .map(|c| c.name)
        .collect();
    Ok(ai::suggest_category_and_tags(&url, &title, &existing_cats))
}

// 代理管理命令
#[tauri::command]
fn get_proxy_settings(state: State<'_, AppState>) -> Result<ProxyConfig, String> {
    Ok(load_current_proxy(&state.db))
}

#[tauri::command]
fn save_proxy_settings(config: ProxyConfig, state: State<'_, AppState>) -> Result<(), String> {
    state.db.set_setting("proxy_mode", &config.mode).map_err(|e| e.to_string())?;
    if let Some(ref u) = config.url {
        state.db.set_setting("proxy_url", u).map_err(|e| e.to_string())?;
    } else {
        state.db.set_setting("proxy_url", "").map_err(|e| e.to_string())?;
    }
    Ok(())
}

#[tauri::command]
async fn test_proxy(proxy_url: String) -> Result<fetch::ProxyTestResult, String> {
    Ok(fetch::test_proxy_connection(&proxy_url).await)
}

// 云同步管理命令
#[tauri::command]
fn get_sync_config(state: State<'_, AppState>) -> Result<SyncConfig, String> {
    let worker_url = state.db.get_setting("cf_worker_url").unwrap_or_default().unwrap_or_default();
    let token = state.db.get_setting("cf_sync_token").unwrap_or_default().unwrap_or_default();
    let auto_sync = state.db.get_setting("cf_auto_sync").unwrap_or_default().unwrap_or_else(|| "false".to_string()) == "true";
    let last_synced_at = state.db.get_setting("cf_last_synced_at").unwrap_or_default().and_then(|s| s.parse().ok());
    Ok(SyncConfig {
        worker_url,
        token,
        auto_sync,
        last_synced_at,
    })
}

#[tauri::command]
fn save_sync_config(config: SyncConfig, state: State<'_, AppState>) -> Result<(), String> {
    state.db.set_setting("cf_worker_url", &config.worker_url.trim()).map_err(|e| e.to_string())?;
    state.db.set_setting("cf_sync_token", &config.token.trim()).map_err(|e| e.to_string())?;
    state.db.set_setting("cf_auto_sync", if config.auto_sync { "true" } else { "false" }).map_err(|e| e.to_string())?;
    Ok(())
}

#[tauri::command]
async fn sync_with_cloudflare(state: State<'_, AppState>) -> Result<SyncResult, String> {
    sync::perform_two_way_sync(&state.db).await
}

#[derive(serde::Deserialize)]
pub struct CheckTarget {
    pub id: i64,
    pub url: String,
    pub current_valid: Option<i32>,
}

#[tauri::command]
async fn check_bookmarks_batch(
    targets: Vec<CheckTarget>,
    state: State<'_, AppState>,
) -> Result<Vec<fetch::CheckResult>, String> {
    use tokio::sync::Semaphore;

    let semaphore = Arc::new(Semaphore::new(10));
    let mut tasks = Vec::new();
    let proxy_cfg = Arc::new(load_current_proxy(&state.db));

    for t in targets {
        let sem = semaphore.clone();
        let p_cfg = proxy_cfg.clone();
        tasks.push(tokio::spawn(async move {
            let _permit = sem.acquire().await.unwrap();
            fetch::check_url_validity(t.id, &t.url, t.current_valid, Some(&p_cfg)).await
        }));
    }

    let mut results = Vec::new();
    for task in tasks {
        if let Ok(res) = task.await {
            let _ = state.db.update_validation_status(res.id, res.valid, res.status_code, res.check_note.clone());
            results.push(res);
        }
    }

    Ok(results)
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_opener::init())
        .setup(|app| {
            let app_data = app.path().app_data_dir().unwrap_or_else(|_| std::path::PathBuf::from("."));
            std::fs::create_dir_all(&app_data).ok();
            let db_path = app_data.join("bookmarks.db");
            let db = Arc::new(Database::new(&db_path).expect("Failed to initialize database"));
            app.manage(AppState { db });
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![
            get_categories,
            create_category,
            delete_category,
            rename_category,
            update_category_parent,
            reorder_categories,
            get_tags,
            get_failure_reasons,
            list_bookmarks,
            create_bookmark,
            update_bookmark,
            set_bookmark_category,
            batch_set_bookmarks_category,
            delete_bookmark,
            import_bookmarks_html,
            export_bookmarks_html,
            find_duplicates,
            merge_duplicates,
            fetch_metadata,
            suggest_ai_metadata,
            get_proxy_settings,
            save_proxy_settings,
            test_proxy,
            get_sync_config,
            save_sync_config,
            sync_with_cloudflare,
            check_bookmarks_batch
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
