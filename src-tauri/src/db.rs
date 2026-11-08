use rusqlite::{params, Connection, Result};
use std::path::Path;
use std::sync::Mutex;
use crate::models::{Bookmark, BookmarkCreateInput, BookmarkFilter, BookmarkUpdateInput, Category, Tag};
use crate::url::normalize_url;

pub struct Database {
    pub conn: Mutex<Connection>,
}

impl Database {
    pub fn new<P: AsRef<Path>>(path: P) -> Result<Self> {
        let conn = Connection::open(path)?;

        conn.execute_batch(
            "PRAGMA journal_mode=WAL;
             PRAGMA foreign_keys=ON;"
        )?;

        let db = Database {
            conn: Mutex::new(conn),
        };

        db.init_tables()?;
        Ok(db)
    }

    pub fn new_in_memory() -> Result<Self> {
        let conn = Connection::open_in_memory()?;
        conn.execute_batch("PRAGMA foreign_keys=ON;")?;
        let db = Database {
            conn: Mutex::new(conn),
        };
        db.init_tables()?;
        Ok(db)
    }

    fn init_tables(&self) -> Result<()> {
        let conn = self.conn.lock().unwrap();

        conn.execute_batch(
            "CREATE TABLE IF NOT EXISTS categories (
              id        INTEGER PRIMARY KEY AUTOINCREMENT,
              name      TEXT NOT NULL,
              parent_id INTEGER REFERENCES categories(id) ON DELETE CASCADE,
              sort_order INTEGER DEFAULT 0,
              UNIQUE (parent_id, name COLLATE NOCASE)
            );

            CREATE TABLE IF NOT EXISTS tags (
              id   INTEGER PRIMARY KEY AUTOINCREMENT,
              name TEXT NOT NULL UNIQUE COLLATE NOCASE
            );

            CREATE TABLE IF NOT EXISTS bookmarks (
              id              INTEGER PRIMARY KEY AUTOINCREMENT,
              title           TEXT NOT NULL,
              url             TEXT NOT NULL,
              url_key         TEXT NOT NULL,
              icon            TEXT,
              icon_mime       TEXT,
              category_id     INTEGER REFERENCES categories(id) ON DELETE SET NULL,
              valid           INTEGER CHECK (valid IN (0,1) OR valid IS NULL),
              status_code     INTEGER,
              last_checked_at INTEGER,
              check_note      TEXT,
              created_at      INTEGER NOT NULL,
              updated_at      INTEGER NOT NULL
            );
            CREATE INDEX IF NOT EXISTS idx_bookmarks_url_key  ON bookmarks(url_key);
            CREATE INDEX IF NOT EXISTS idx_bookmarks_category ON bookmarks(category_id);
            CREATE INDEX IF NOT EXISTS idx_bookmarks_valid    ON bookmarks(valid);

            CREATE TABLE IF NOT EXISTS bookmark_tags (
              bookmark_id INTEGER NOT NULL REFERENCES bookmarks(id) ON DELETE CASCADE,
              tag_id      INTEGER NOT NULL REFERENCES tags(id) ON DELETE CASCADE,
              PRIMARY KEY (bookmark_id, tag_id)
            );
            CREATE INDEX IF NOT EXISTS idx_bookmark_tags_tag ON bookmark_tags(tag_id);

            CREATE TABLE IF NOT EXISTS settings (
              key   TEXT PRIMARY KEY,
              value TEXT NOT NULL
            );"
        )?;

        // 向后兼容迁移：补充可能缺失的字段
        let _ = conn.execute("ALTER TABLE categories ADD COLUMN sort_order INTEGER DEFAULT 0;", []);
        let _ = conn.execute("ALTER TABLE categories ADD COLUMN updated_at INTEGER DEFAULT 0;", []);
        let _ = conn.execute("ALTER TABLE categories ADD COLUMN is_deleted INTEGER DEFAULT 0;", []);
        let _ = conn.execute("ALTER TABLE bookmarks ADD COLUMN is_deleted INTEGER DEFAULT 0;", []);

        Ok(())
    }

    // --- 分类管理 ---
    pub fn get_categories(&self) -> Result<Vec<Category>> {
        let conn = self.conn.lock().unwrap();
        let mut stmt = conn.prepare("SELECT id, name, parent_id, coalesce(sort_order, 0), coalesce(updated_at, 0), coalesce(is_deleted, 0) FROM categories WHERE coalesce(is_deleted, 0) = 0 ORDER BY coalesce(sort_order, 0) ASC, name COLLATE NOCASE ASC")?;
        let rows = stmt.query_map([], |row| {
            Ok(Category {
                id: row.get(0)?,
                name: row.get(1)?,
                parent_id: row.get(2)?,
                sort_order: row.get(3)?,
                updated_at: Some(row.get(4)?),
                is_deleted: Some(row.get(5)?),
            })
        })?;

        let mut list = Vec::new();
        for r in rows {
            list.push(r?);
        }
        Ok(list)
    }

    pub fn create_category(&self, name: &str, parent_id: Option<i64>) -> Result<i64> {
        let conn = self.conn.lock().unwrap();
        let now = chrono::Utc::now().timestamp();
        // 计算下一个 sort_order
        let next_order: i64 = conn
            .query_row(
                "SELECT coalesce(MAX(sort_order), -1) + 1 FROM categories WHERE parent_id IS ?1",
                params![parent_id],
                |r| r.get(0),
            )
            .unwrap_or(0);

        conn.execute(
            "INSERT INTO categories (name, parent_id, sort_order, updated_at, is_deleted) VALUES (?1, ?2, ?3, ?4, 0)",
            params![name.trim(), parent_id, next_order, now],
        )?;
        Ok(conn.last_insert_rowid())
    }

    pub fn rename_category(&self, id: i64, new_name: &str) -> std::result::Result<(), String> {
        let trimmed = new_name.trim();
        if trimmed.is_empty() {
            return Err("分类名称不能为空".to_string());
        }
        let now = chrono::Utc::now().timestamp();
        let conn = self.conn.lock().map_err(|e| e.to_string())?;
        conn.execute(
            "UPDATE categories SET name = ?1, updated_at = ?2 WHERE id = ?3",
            params![trimmed, now, id],
        ).map_err(|e| format!("重命名分类失败: {}", e))?;
        Ok(())
    }

    pub fn update_category_parent(&self, id: i64, new_parent_id: Option<i64>) -> std::result::Result<(), String> {
        let conn = self.conn.lock().map_err(|e| e.to_string())?;
        if let Some(target_parent) = new_parent_id {
            if target_parent == id {
                return Err("不能将分类移动到自身下方".to_string());
            }
            // 环路检测：防止将父节点拖入其自身的子孙节点
            let mut curr = Some(target_parent);
            while let Some(p) = curr {
                if p == id {
                    return Err("不能将分类移动到其子分类下方（避免循环层级）".to_string());
                }
                curr = conn
                    .query_row(
                        "SELECT parent_id FROM categories WHERE id = ?1",
                        params![p],
                        |r| r.get(0),
                    )
                    .unwrap_or(None);
            }
        }

        let now = chrono::Utc::now().timestamp();
        conn.execute(
            "UPDATE categories SET parent_id = ?1, updated_at = ?2 WHERE id = ?3",
            params![new_parent_id, now, id],
        ).map_err(|e| format!("更新分类层级失败: {}", e))?;

        Ok(())
    }

    pub fn reorder_categories(&self, ordered_ids: &[i64]) -> Result<()> {
        let mut conn = self.conn.lock().unwrap();
        let now = chrono::Utc::now().timestamp();
        let tx = conn.transaction()?;
        {
            let mut stmt = tx.prepare("UPDATE categories SET sort_order = ?1, updated_at = ?2 WHERE id = ?3")?;
            for (idx, &id) in ordered_ids.iter().enumerate() {
                stmt.execute(params![idx as i64, now, id])?;
            }
        }
        tx.commit()?;
        Ok(())
    }

    pub fn delete_category(&self, id: i64) -> Result<()> {
        let now = chrono::Utc::now().timestamp();
        let conn = self.conn.lock().unwrap();
        conn.execute("UPDATE categories SET is_deleted = 1, updated_at = ?1 WHERE id = ?2 OR parent_id = ?2", params![now, id])?;
        conn.execute("UPDATE bookmarks SET category_id = NULL, updated_at = ?1 WHERE category_id = ?2", params![now, id])?;
        Ok(())
    }

    // --- 标签管理 ---
    pub fn get_tags(&self) -> Result<Vec<Tag>> {
        let conn = self.conn.lock().unwrap();
        let mut stmt = conn.prepare("SELECT id, name FROM tags ORDER BY name COLLATE NOCASE ASC")?;
        let rows = stmt.query_map([], |row| {
            Ok(Tag {
                id: row.get(0)?,
                name: row.get(1)?,
            })
        })?;

        let mut list = Vec::new();
        for r in rows {
            list.push(r?);
        }
        Ok(list)
    }

    // --- 获取所有失效原因列表（供前端筛选） ---
    pub fn get_failure_reasons(&self) -> Result<Vec<String>> {
        let conn = self.conn.lock().unwrap();
        let mut stmt = conn.prepare(
            "SELECT DISTINCT check_note FROM bookmarks
             WHERE valid = 0 AND check_note IS NOT NULL AND check_note != ''
             ORDER BY check_note ASC"
        )?;
        let rows = stmt.query_map([], |row| row.get::<_, String>(0))?;
        let mut list = Vec::new();
        for r in rows {
            list.push(r?);
        }
        Ok(list)
    }

    // --- 书签管理 ---
    pub fn create_bookmark(&self, input: &BookmarkCreateInput) -> Result<i64> {
        let now = chrono::Utc::now().timestamp();
        let url_key = normalize_url(&input.url).unwrap_or_else(|_| input.url.clone());

        let mut conn = self.conn.lock().unwrap();
        let tx = conn.transaction()?;

        tx.execute(
            "INSERT INTO bookmarks (title, url, url_key, icon, category_id, created_at, updated_at)
             VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)",
            params![input.title, input.url, url_key, input.icon_data_url, input.category_id, now, now],
        )?;
        let bookmark_id = tx.last_insert_rowid();

        for tag_name in &input.tags {
            let tag_name = tag_name.trim();
            if tag_name.is_empty() {
                continue;
            }
            tx.execute(
                "INSERT OR IGNORE INTO tags (name) VALUES (?1)",
                params![tag_name],
            )?;
            let tag_id: i64 = tx.query_row(
                "SELECT id FROM tags WHERE name = ?1 COLLATE NOCASE",
                params![tag_name],
                |r| r.get(0),
            )?;
            tx.execute(
                "INSERT OR IGNORE INTO bookmark_tags (bookmark_id, tag_id) VALUES (?1, ?2)",
                params![bookmark_id, tag_id],
            )?;
        }

        tx.commit()?;
        Ok(bookmark_id)
    }

    pub fn update_bookmark(&self, input: &BookmarkUpdateInput) -> Result<()> {
        let now = chrono::Utc::now().timestamp();
        let url_key = normalize_url(&input.url).unwrap_or_else(|_| input.url.clone());

        let mut conn = self.conn.lock().unwrap();
        let tx = conn.transaction()?;

        tx.execute(
            "UPDATE bookmarks
             SET title = ?1, url = ?2, url_key = ?3, icon = coalesce(?4, icon), category_id = ?5, updated_at = ?6
             WHERE id = ?7",
            params![input.title, input.url, url_key, input.icon_data_url, input.category_id, now, input.id],
        )?;

        tx.execute("DELETE FROM bookmark_tags WHERE bookmark_id = ?1", params![input.id])?;
        for tag_name in &input.tags {
            let tag_name = tag_name.trim();
            if tag_name.is_empty() {
                continue;
            }
            tx.execute(
                "INSERT OR IGNORE INTO tags (name) VALUES (?1)",
                params![tag_name],
            )?;
            let tag_id: i64 = tx.query_row(
                "SELECT id FROM tags WHERE name = ?1 COLLATE NOCASE",
                params![tag_name],
                |r| r.get(0),
            )?;
            tx.execute(
                "INSERT OR IGNORE INTO bookmark_tags (bookmark_id, tag_id) VALUES (?1, ?2)",
                params![input.id, tag_id],
            )?;
        }

        tx.commit()?;
        Ok(())
    }

    pub fn update_bookmark_category(&self, id: i64, category_id: Option<i64>) -> Result<()> {
        let now = chrono::Utc::now().timestamp();
        let conn = self.conn.lock().unwrap();
        conn.execute(
            "UPDATE bookmarks SET category_id = ?1, updated_at = ?2 WHERE id = ?3",
            params![category_id, now, id],
        )?;
        Ok(())
    }

    pub fn batch_update_bookmarks_category(&self, ids: &[i64], category_id: Option<i64>) -> Result<()> {
        let now = chrono::Utc::now().timestamp();
        let mut conn = self.conn.lock().unwrap();
        let tx = conn.transaction()?;
        {
            let mut stmt = tx.prepare("UPDATE bookmarks SET category_id = ?1, updated_at = ?2 WHERE id = ?3")?;
            for &id in ids {
                stmt.execute(params![category_id, now, id])?;
            }
        }
        tx.commit()?;
        Ok(())
    }

    pub fn delete_bookmark(&self, id: i64) -> Result<()> {
        let now = chrono::Utc::now().timestamp();
        let conn = self.conn.lock().unwrap();
        conn.execute("UPDATE bookmarks SET is_deleted = 1, updated_at = ?1 WHERE id = ?2", params![now, id])?;
        Ok(())
    }

    pub fn list_bookmarks(&self, filter: &BookmarkFilter) -> Result<Vec<Bookmark>> {
        let conn = self.conn.lock().unwrap();

        let mut sql = String::from(
            "SELECT b.id, b.title, b.url, b.url_key, b.icon, b.category_id, c.name as category_name,
                    b.valid, b.status_code, b.last_checked_at, b.check_note, b.created_at, b.updated_at
             FROM bookmarks b
             LEFT JOIN categories c ON b.category_id = c.id
             WHERE coalesce(b.is_deleted, 0) = 0"
        );

        let mut conditions = Vec::new();

        // 关键词搜索：包含标题、URL、或者失效说明
        if let Some(ref search) = filter.search {
            let s = search.trim();
            if !s.is_empty() {
                let escaped = s.replace('\'', "''");
                conditions.push(format!(
                    " AND (b.title LIKE '%{0}%' OR b.url LIKE '%{0}%' OR (b.check_note IS NOT NULL AND b.check_note LIKE '%{0}%'))",
                    escaped
                ));
            }
        }

        // 分类过滤
        if let Some(cat_id) = filter.category_id {
            if cat_id > 0 {
                conditions.push(format!(" AND b.category_id = {}", cat_id));
            } else if cat_id == -1 {
                conditions.push(" AND b.category_id IS NULL".to_string());
            }
        }

        // 标签过滤
        if let Some(ref tag_name) = filter.tag {
            let t = tag_name.trim();
            if !t.is_empty() {
                conditions.push(format!(
                    " AND b.id IN (
                        SELECT bt.bookmark_id FROM bookmark_tags bt
                        JOIN tags t ON bt.tag_id = t.id
                        WHERE t.name = '{}' COLLATE NOCASE
                    )",
                    t.replace('\'', "''")
                ));
            }
        }

        // 有效性状态过滤
        if let Some(ref v_state) = filter.valid_state {
            match v_state.as_str() {
                "valid" => conditions.push(" AND b.valid = 1".to_string()),
                "invalid" => conditions.push(" AND b.valid = 0".to_string()),
                "unknown" => conditions.push(" AND b.valid IS NULL".to_string()),
                "blocked" => conditions.push(" AND b.check_note = 'blocked'".to_string()),
                _ => {}
            }
        }

        // 具体失效原因过滤
        if let Some(ref note) = filter.check_note {
            let n = note.trim();
            if !n.is_empty() {
                conditions.push(format!(" AND b.check_note = '{}'", n.replace('\'', "''")));
            }
        }

        for c in conditions {
            sql.push_str(&c);
        }

        match filter.sort_by.as_deref().unwrap_or("created_desc") {
            "created_asc" => sql.push_str(" ORDER BY b.created_at ASC"),
            "title_asc" => sql.push_str(" ORDER BY b.title COLLATE NOCASE ASC"),
            "url_asc" => sql.push_str(" ORDER BY b.url_key ASC"),
            _ => sql.push_str(" ORDER BY b.created_at DESC"),
        }

        let mut stmt = conn.prepare(&sql)?;
        let rows = stmt.query_map([], |row| {
            Ok(Bookmark {
                id: row.get(0)?,
                title: row.get(1)?,
                url: row.get(2)?,
                url_key: row.get(3)?,
                icon_data_url: row.get(4)?,
                category_id: row.get(5)?,
                category_name: row.get(6)?,
                valid: row.get(7)?,
                status_code: row.get(8)?,
                last_checked_at: row.get(9)?,
                check_note: row.get(10)?,
                tags: Vec::new(),
                created_at: row.get(11)?,
                updated_at: row.get(12)?,
            })
        })?;

        let mut bookmarks = Vec::new();
        for r in rows {
            let mut bm = r?;
            let mut tag_stmt = conn.prepare(
                "SELECT t.name FROM tags t
                 JOIN bookmark_tags bt ON t.id = bt.tag_id
                 WHERE bt.bookmark_id = ?1
                 ORDER BY t.name COLLATE NOCASE ASC"
            )?;
            let tag_rows = tag_stmt.query_map(params![bm.id], |tr| tr.get::<_, String>(0))?;
            for t in tag_rows {
                bm.tags.push(t?);
            }
            bookmarks.push(bm);
        }

        Ok(bookmarks)
    }

    pub fn update_validation_status(
        &self,
        id: i64,
        valid: Option<i32>,
        status_code: i32,
        check_note: Option<String>,
    ) -> Result<()> {
        let now = chrono::Utc::now().timestamp();
        let conn = self.conn.lock().unwrap();
        conn.execute(
            "UPDATE bookmarks
             SET valid = ?1, status_code = ?2, last_checked_at = ?3, check_note = ?4
             WHERE id = ?5",
            params![valid, status_code, now, check_note, id],
        )?;
        Ok(())
    }

    pub fn find_duplicates(&self) -> Result<Vec<Vec<Bookmark>>> {
        let conn = self.conn.lock().unwrap();
        let mut stmt = conn.prepare(
            "SELECT url_key FROM bookmarks
             GROUP BY url_key
             HAVING count(*) > 1"
        )?;
        let url_keys = stmt
            .query_map([], |row| row.get::<_, String>(0))?
            .collect::<Result<Vec<_>, _>>()?;

        let mut groups = Vec::new();
        for key in url_keys {
            let mut bm_stmt = conn.prepare(
                "SELECT b.id, b.title, b.url, b.url_key, b.icon, b.category_id, c.name as category_name,
                        b.valid, b.status_code, b.last_checked_at, b.check_note, b.created_at, b.updated_at
                 FROM bookmarks b
                 LEFT JOIN categories c ON b.category_id = c.id
                 WHERE b.url_key = ?1
                 ORDER BY b.created_at ASC"
            )?;
            let bms = bm_stmt
                .query_map(params![key], |row| {
                    Ok(Bookmark {
                        id: row.get(0)?,
                        title: row.get(1)?,
                        url: row.get(2)?,
                        url_key: row.get(3)?,
                        icon_data_url: row.get(4)?,
                        category_id: row.get(5)?,
                        category_name: row.get(6)?,
                        valid: row.get(7)?,
                        status_code: row.get(8)?,
                        last_checked_at: row.get(9)?,
                        check_note: row.get(10)?,
                        tags: Vec::new(),
                        created_at: row.get(11)?,
                        updated_at: row.get(12)?,
                    })
                })?
                .collect::<Result<Vec<_>, _>>()?;

            groups.push(bms);
        }

        Ok(groups)
    }

    pub fn merge_duplicates(&self) -> Result<usize> {
        let groups = self.find_duplicates()?;
        let mut merged_count = 0;

        let mut conn = self.conn.lock().unwrap();
        let tx = conn.transaction()?;

        for group in groups {
            if group.len() <= 1 {
                continue;
            }

            let primary = &group[0];
            let mut all_tags = std::collections::HashSet::new();

            for bm in &group {
                let mut tag_stmt = tx.prepare(
                    "SELECT t.name FROM tags t
                     JOIN bookmark_tags bt ON t.id = bt.tag_id
                     WHERE bt.bookmark_id = ?1"
                )?;
                let rows = tag_stmt.query_map(params![bm.id], |r| r.get::<_, String>(0))?;
                for t in rows {
                    all_tags.insert(t?);
                }
            }

            tx.execute("DELETE FROM bookmark_tags WHERE bookmark_id = ?1", params![primary.id])?;
            for tag_name in all_tags {
                tx.execute("INSERT OR IGNORE INTO tags (name) VALUES (?1)", params![tag_name])?;
                let tag_id: i64 = tx.query_row(
                    "SELECT id FROM tags WHERE name = ?1 COLLATE NOCASE",
                    params![tag_name],
                    |r| r.get(0),
                )?;
                tx.execute(
                    "INSERT OR IGNORE INTO bookmark_tags (bookmark_id, tag_id) VALUES (?1, ?2)",
                    params![primary.id, tag_id],
                )?;
            }

            for dup in &group[1..] {
                tx.execute("DELETE FROM bookmarks WHERE id = ?1", params![dup.id])?;
                merged_count += 1;
            }
        }

        tx.commit()?;
        Ok(merged_count)
    }

    pub fn import_netscape_bookmarks(
        &self,
        items: &[crate::netscape::ImportedBookmark],
    ) -> Result<usize> {
        let mut conn = self.conn.lock().unwrap();
        let tx = conn.transaction()?;
        let now = chrono::Utc::now().timestamp();
        let mut count = 0;

        for item in items {
            let mut parent_id: Option<i64> = None;
            for folder in &item.folder_path {
                let folder = folder.trim();
                if folder.is_empty() {
                    continue;
                }
                let existing_id: Result<i64, _> = match parent_id {
                    Some(pid) => tx.query_row(
                        "SELECT id FROM categories WHERE parent_id = ?1 AND name = ?2 COLLATE NOCASE",
                        params![pid, folder],
                        |r| r.get(0),
                    ),
                    None => tx.query_row(
                        "SELECT id FROM categories WHERE parent_id IS NULL AND name = ?1 COLLATE NOCASE",
                        params![folder],
                        |r| r.get(0),
                    ),
                };

                parent_id = match existing_id {
                    Ok(id) => Some(id),
                    Err(_) => {
                        tx.execute(
                            "INSERT INTO categories (name, parent_id) VALUES (?1, ?2)",
                            params![folder, parent_id],
                        )?;
                        Some(tx.last_insert_rowid())
                    }
                };
            }

            let url_key = normalize_url(&item.url).unwrap_or_else(|_| item.url.clone());
            let created_at = item.add_date.unwrap_or(now);

            tx.execute(
                "INSERT INTO bookmarks (title, url, url_key, icon, category_id, created_at, updated_at)
                 VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)",
                params![item.title, item.url, url_key, item.icon, parent_id, created_at, now],
            )?;
            let bm_id = tx.last_insert_rowid();

            for tag in &item.tags {
                let tag = tag.trim();
                if tag.is_empty() {
                    continue;
                }
                tx.execute("INSERT OR IGNORE INTO tags (name) VALUES (?1)", params![tag])?;
                let tag_id: i64 = tx.query_row(
                    "SELECT id FROM tags WHERE name = ?1 COLLATE NOCASE",
                    params![tag],
                    |r| r.get(0),
                )?;
                tx.execute(
                    "INSERT OR IGNORE INTO bookmark_tags (bookmark_id, tag_id) VALUES (?1, ?2)",
                    params![bm_id, tag_id],
                )?;
            }

            count += 1;
        }

        tx.commit()?;
        Ok(count)
    }

    // --- 系统设置项（代理设置等） ---
    pub fn get_setting(&self, key: &str) -> Result<Option<String>> {
        let conn = self.conn.lock().unwrap();
        let mut stmt = conn.prepare("SELECT value FROM settings WHERE key = ?1")?;
        let mut rows = stmt.query(params![key])?;
        if let Some(row) = rows.next()? {
            Ok(Some(row.get(0)?))
        } else {
            Ok(None)
        }
    }

    pub fn set_setting(&self, key: &str, value: &str) -> Result<()> {
        let conn = self.conn.lock().unwrap();
        conn.execute(
            "INSERT INTO settings (key, value) VALUES (?1, ?2)
             ON CONFLICT(key) DO UPDATE SET value = excluded.value",
            params![key, value],
        )?;
        Ok(())
    }

    // --- 云同步增量数据拉取与合并 ---
    pub fn get_sync_payload_since(
        &self,
        since: i64,
    ) -> Result<(Vec<crate::sync::CloudCategory>, Vec<crate::sync::CloudBookmark>)> {
        let conn = self.conn.lock().unwrap();

        // 1. 获取变更分类 (since 为 0 时同步全部历史分类)
        let mut cat_stmt = conn.prepare(
            "SELECT id, name, parent_id, sort_order, coalesce(updated_at, 0), coalesce(is_deleted, 0)
             FROM categories
             WHERE (?1 = 0) OR (coalesce(updated_at, 0) > ?1)",
        )?;
        let cat_rows = cat_stmt.query_map(params![since], |row| {
            Ok(crate::sync::CloudCategory {
                id: Some(row.get(0)?),
                name: row.get(1)?,
                parent_id: row.get(2)?,
                sort_order: Some(row.get(3)?),
                updated_at: row.get(4)?,
                is_deleted: Some(row.get(5)?),
            })
        })?;
        let mut cats = Vec::new();
        for c in cat_rows {
            cats.push(c?);
        }

        // 2. 获取变更书签
        let mut bm_stmt = conn.prepare(
            "SELECT b.id, b.title, b.url, b.url_key, b.icon, b.category_id,
                    c.name as category_name, b.valid, b.created_at, b.updated_at, coalesce(b.is_deleted, 0)
             FROM bookmarks b
             LEFT JOIN categories c ON b.category_id = c.id
             WHERE b.updated_at > ?1",
        )?;
        let bm_rows = bm_stmt.query_map(params![since], |row| {
            Ok((
                row.get::<_, i64>(0)?,
                crate::sync::CloudBookmark {
                    id: Some(row.get(0)?),
                    title: row.get(1)?,
                    url: row.get(2)?,
                    url_key: Some(row.get(3)?),
                    icon: row.get(4)?,
                    category_id: row.get(5)?,
                    category_name: row.get(6)?,
                    tags: None,
                    valid: row.get(7)?,
                    created_at: Some(row.get(8)?),
                    updated_at: row.get(9)?,
                    is_deleted: Some(row.get(10)?),
                },
            ))
        })?;

        let mut bms = Vec::new();
        for r in bm_rows {
            let (bm_id, mut cloud_bm) = r?;
            let mut tag_stmt = conn.prepare(
                "SELECT t.name FROM tags t
                 JOIN bookmark_tags bt ON t.id = bt.tag_id
                 WHERE bt.bookmark_id = ?1",
            )?;
            let tag_rows = tag_stmt.query_map(params![bm_id], |tr| tr.get::<_, String>(0))?;
            let mut tags = Vec::new();
            for t in tag_rows {
                tags.push(t?);
            }
            cloud_bm.tags = Some(tags);
            bms.push(cloud_bm);
        }

        Ok((cats, bms))
    }

    pub fn apply_cloud_sync_changes(
        &self,
        categories: &[crate::sync::CloudCategory],
        bookmarks: &[crate::sync::CloudBookmark],
    ) -> Result<()> {
        let mut conn = self.conn.lock().unwrap();
        let tx = conn.transaction()?;

        // 1. 合并分类
        for cat in categories {
            let is_del = cat.is_deleted.unwrap_or(0) == 1;
            let existing_id: Option<(i64, i64)> = tx
                .query_row(
                    "SELECT id, coalesce(updated_at, 0) FROM categories WHERE name = ?1 COLLATE NOCASE",
                    params![cat.name],
                    |r| Ok((r.get(0)?, r.get(1)?)),
                )
                .ok();

            if is_del {
                if let Some((id, _)) = existing_id {
                    tx.execute(
                        "UPDATE categories SET is_deleted = 1, updated_at = ?1 WHERE id = ?2",
                        params![cat.updated_at, id],
                    )?;
                }
            } else if let Some((id, local_updated)) = existing_id {
                if cat.updated_at >= local_updated {
                    tx.execute(
                        "UPDATE categories SET sort_order = ?1, updated_at = ?2, is_deleted = 0 WHERE id = ?3",
                        params![cat.sort_order.unwrap_or(0), cat.updated_at, id],
                    )?;
                }
            } else {
                tx.execute(
                    "INSERT INTO categories (name, parent_id, sort_order, updated_at, is_deleted)
                     VALUES (?1, ?2, ?3, ?4, 0)",
                    params![
                        cat.name,
                        cat.parent_id,
                        cat.sort_order.unwrap_or(0),
                        cat.updated_at
                    ],
                )?;
            }
        }

        // 2. 合并书签
        for bm in bookmarks {
            let is_del = bm.is_deleted.unwrap_or(0) == 1;
            let url_key = bm.url_key.clone().unwrap_or_else(|| {
                crate::url::normalize_url(&bm.url).unwrap_or_else(|_| bm.url.clone())
            });

            let existing: Option<(i64, i64)> = tx
                .query_row(
                    "SELECT id, updated_at FROM bookmarks WHERE url_key = ?1",
                    params![url_key],
                    |r| Ok((r.get(0)?, r.get(1)?)),
                )
                .ok();

            if is_del {
                if let Some((id, _)) = existing {
                    tx.execute(
                        "UPDATE bookmarks SET is_deleted = 1, updated_at = ?1 WHERE id = ?2",
                        params![bm.updated_at, id],
                    )?;
                }
            } else {
                let target_category_id = if let Some(ref cat_name) = bm.category_name {
                    tx.query_row(
                        "SELECT id FROM categories WHERE name = ?1 COLLATE NOCASE",
                        params![cat_name],
                        |r| r.get::<_, i64>(0),
                    )
                    .ok()
                } else {
                    None
                };

                let bookmark_id = match existing {
                    Some((id, local_updated)) => {
                        if bm.updated_at >= local_updated {
                            tx.execute(
                                "UPDATE bookmarks
                                 SET title = ?1, url = ?2, icon = coalesce(?3, icon), category_id = ?4, valid = ?5, updated_at = ?6, is_deleted = 0
                                 WHERE id = ?7",
                                params![
                                    bm.title,
                                    bm.url,
                                    bm.icon,
                                    target_category_id,
                                    bm.valid.unwrap_or(1),
                                    bm.updated_at,
                                    id
                                ],
                            )?;
                        }
                        id
                    }
                    None => {
                        tx.execute(
                            "INSERT INTO bookmarks (title, url, url_key, icon, category_id, valid, created_at, updated_at, is_deleted)
                             VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, 0)",
                            params![
                                bm.title,
                                bm.url,
                                url_key,
                                bm.icon,
                                target_category_id,
                                bm.valid.unwrap_or(1),
                                bm.created_at.unwrap_or(bm.updated_at),
                                bm.updated_at
                            ],
                        )?;
                        tx.last_insert_rowid()
                    }
                };

                // 合并标签
                if let Some(ref tags) = bm.tags {
                    for tag_name in tags {
                        let trimmed = tag_name.trim();
                        if trimmed.is_empty() {
                            continue;
                        }
                        tx.execute(
                            "INSERT OR IGNORE INTO tags (name) VALUES (?1)",
                            params![trimmed],
                        )?;
                        let tag_id: i64 = tx.query_row(
                            "SELECT id FROM tags WHERE name = ?1 COLLATE NOCASE",
                            params![trimmed],
                            |r| r.get(0),
                        )?;
                        tx.execute(
                            "INSERT OR IGNORE INTO bookmark_tags (bookmark_id, tag_id) VALUES (?1, ?2)",
                            params![bookmark_id, tag_id],
                        )?;
                    }
                }
            }
        }

        tx.commit()?;
        Ok(())
    }

    /// 全新覆盖下载：清空本地数据，并将从云端拉取的全量分类与书签完整写入
    pub fn overwrite_all_from_cloud(
        &self,
        categories: &[crate::sync::CloudCategory],
        bookmarks: &[crate::sync::CloudBookmark],
    ) -> Result<()> {
        let mut conn = self.conn.lock().unwrap();
        let tx = conn.transaction()?;

        // 清理本地旧数据
        tx.execute("DELETE FROM bookmark_tags", [])?;
        tx.execute("DELETE FROM bookmarks", [])?;
        tx.execute("DELETE FROM categories", [])?;
        tx.execute("DELETE FROM tags", [])?;

        // 1. 写入所有非删除分类
        for cat in categories {
            if cat.is_deleted.unwrap_or(0) == 1 {
                continue;
            }
            tx.execute(
                "INSERT INTO categories (name, parent_id, sort_order, updated_at, is_deleted)
                 VALUES (?1, ?2, ?3, ?4, 0)",
                params![
                    cat.name,
                    cat.parent_id,
                    cat.sort_order.unwrap_or(0),
                    cat.updated_at
                ],
            )?;
        }

        // 2. 写入所有非删除书签
        for bm in bookmarks {
            if bm.is_deleted.unwrap_or(0) == 1 {
                continue;
            }
            let url_key = bm.url_key.clone().unwrap_or_else(|| {
                crate::url::normalize_url(&bm.url).unwrap_or_else(|_| bm.url.clone())
            });

            let target_category_id = if let Some(ref cat_name) = bm.category_name {
                tx.query_row(
                    "SELECT id FROM categories WHERE name = ?1 COLLATE NOCASE",
                    params![cat_name],
                    |r| r.get::<_, i64>(0),
                )
                .ok()
            } else {
                None
            };

            tx.execute(
                "INSERT INTO bookmarks (title, url, url_key, icon, category_id, valid, created_at, updated_at, is_deleted)
                 VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, 0)",
                params![
                    bm.title,
                    bm.url,
                    url_key,
                    bm.icon,
                    target_category_id,
                    bm.valid.unwrap_or(1),
                    bm.created_at.unwrap_or(bm.updated_at),
                    bm.updated_at
                ],
            )?;
            let bookmark_id = tx.last_insert_rowid();

            // 写入标签
            if let Some(ref tags) = bm.tags {
                for tag_name in tags {
                    let trimmed = tag_name.trim();
                    if trimmed.is_empty() {
                        continue;
                    }
                    tx.execute(
                        "INSERT OR IGNORE INTO tags (name) VALUES (?1)",
                        params![trimmed],
                    )?;
                    let tag_id: i64 = tx.query_row(
                        "SELECT id FROM tags WHERE name = ?1 COLLATE NOCASE",
                        params![trimmed],
                        |r| r.get(0),
                    )?;
                    tx.execute(
                        "INSERT OR IGNORE INTO bookmark_tags (bookmark_id, tag_id) VALUES (?1, ?2)",
                        params![bookmark_id, tag_id],
                    )?;
                }
            }
        }

        tx.commit()?;
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_db_crud_and_tag_merge() {
        let db = Database::new_in_memory().unwrap();

        let cat_id = db.create_category("技术开发", None).unwrap();
        assert!(cat_id > 0);

        let b1 = BookmarkCreateInput {
            title: "Rust 官方网站".to_string(),
            url: "https://www.rust-lang.org/?utm_source=test".to_string(),
            category_id: Some(cat_id),
            tags: vec!["编程".to_string(), "rust".to_string()],
            icon_data_url: None,
        };
        let b1_id = db.create_bookmark(&b1).unwrap();
        assert!(b1_id > 0);

        let list = db.list_bookmarks(&BookmarkFilter {
            search: None,
            category_id: None,
            tag: None,
            valid_state: None,
            check_note: None,
            sort_by: None,
        }).unwrap();

        assert_eq!(list.len(), 1);
        assert_eq!(list[0].title, "Rust 官方网站");
        assert_eq!(list[0].url_key, "https://rust-lang.org/");
        assert_eq!(list[0].tags.len(), 2);
    }

    #[test]
    fn test_dedupe_and_merge() {
        let db = Database::new_in_memory().unwrap();

        db.create_bookmark(&BookmarkCreateInput {
            title: "示例 1".to_string(),
            url: "https://example.com/page?utm_source=a".to_string(),
            category_id: None,
            tags: vec!["tag1".to_string()],
            icon_data_url: None,
        }).unwrap();

        db.create_bookmark(&BookmarkCreateInput {
            title: "示例 2 (带不同参数)".to_string(),
            url: "https://www.example.com/page?utm_source=b".to_string(),
            category_id: None,
            tags: vec!["tag2".to_string()],
            icon_data_url: None,
        }).unwrap();

        let dups = db.find_duplicates().unwrap();
        assert_eq!(dups.len(), 1);
        assert_eq!(dups[0].len(), 2);

        let merged = db.merge_duplicates().unwrap();
        assert_eq!(merged, 1);

        let remaining = db.list_bookmarks(&BookmarkFilter {
            search: None,
            category_id: None,
            tag: None,
            valid_state: None,
            check_note: None,
            sort_by: None,
        }).unwrap();

        assert_eq!(remaining.len(), 1);
        assert_eq!(remaining[0].title, "示例 1");
        assert_eq!(remaining[0].tags.len(), 2);
    }

    #[test]
    fn test_category_hierarchy_and_reorder() {
        let db = Database::new_in_memory().unwrap();

        let c1 = db.create_category("技术", None).unwrap();
        let c2 = db.create_category("生活", None).unwrap();
        let c3 = db.create_category("Rust", Some(c1)).unwrap();
        let c4 = db.create_category("Async", Some(c3)).unwrap();

        // 验证重命名
        db.rename_category(c2, "日常生活").unwrap();
        let cats = db.get_categories().unwrap();
        let c2_renamed = cats.iter().find(|c| c.id == c2).unwrap();
        assert_eq!(c2_renamed.name, "日常生活");

        // 验证层级变更：将 c2 变为 c1 的子分类
        db.update_category_parent(c2, Some(c1)).unwrap();
        let cats = db.get_categories().unwrap();
        assert_eq!(cats.iter().find(|c| c.id == c2).unwrap().parent_id, Some(c1));

        // 验证循环依赖防御：不能将 c1 设为自身
        assert!(db.update_category_parent(c1, Some(c1)).is_err());

        // 验证循环依赖防御：不能将 c1 设为其子孙 c4 的子节点
        assert!(db.update_category_parent(c1, Some(c4)).is_err());

        // 验证将 c2 移回根级
        db.update_category_parent(c2, None).unwrap();
        let cats = db.get_categories().unwrap();
        assert_eq!(cats.iter().find(|c| c.id == c2).unwrap().parent_id, None);

        // 验证重新排序
        db.reorder_categories(&[c2, c1]).unwrap();
        let cats = db.get_categories().unwrap();
        let roots: Vec<i64> = cats.into_iter().filter(|c| c.parent_id.is_none()).map(|c| c.id).collect();
        assert_eq!(roots, vec![c2, c1]);
    }

    #[test]
    fn test_update_bookmark_category() {
        let db = Database::new_in_memory().unwrap();
        let c1 = db.create_category("分类A", None).unwrap();
        let c2 = db.create_category("分类B", None).unwrap();

        let bm_id = db.create_bookmark(&BookmarkCreateInput {
            title: "测试网址".to_string(),
            url: "https://test.org".to_string(),
            category_id: None,
            tags: vec![],
            icon_data_url: None,
        }).unwrap();

        // 移入分类A
        db.update_bookmark_category(bm_id, Some(c1)).unwrap();
        let bms = db.list_bookmarks(&BookmarkFilter {
            search: None,
            category_id: Some(c1),
            tag: None,
            valid_state: None,
            check_note: None,
            sort_by: None,
        }).unwrap();
        assert_eq!(bms.len(), 1);
        assert_eq!(bms[0].category_id, Some(c1));

        // 批量移入分类B
        db.batch_update_bookmarks_category(&[bm_id], Some(c2)).unwrap();
        let bms = db.list_bookmarks(&BookmarkFilter {
            search: None,
            category_id: Some(c2),
            tag: None,
            valid_state: None,
            check_note: None,
            sort_by: None,
        }).unwrap();
        assert_eq!(bms.len(), 1);
        assert_eq!(bms[0].category_id, Some(c2));

        // 移出分类（变为未分类）
        db.update_bookmark_category(bm_id, None).unwrap();
        let bms = db.list_bookmarks(&BookmarkFilter {
            search: None,
            category_id: Some(-1),
            tag: None,
            valid_state: None,
            check_note: None,
            sort_by: None,
        }).unwrap();
        assert_eq!(bms.len(), 1);
        assert_eq!(bms[0].category_id, None);
    }

    #[test]
    fn test_sync_payload_and_cloud_merge() {
        let db = Database::new_in_memory().unwrap();

        // 1. 本地创建数据
        let c_id = db.create_category("技术分类", None).unwrap();
        let _bm_id = db.create_bookmark(&BookmarkCreateInput {
            title: "本地收藏".to_string(),
            url: "https://local-test.com".to_string(),
            category_id: Some(c_id),
            tags: vec!["本地".to_string()],
            icon_data_url: None,
        }).unwrap();

        let (cats, bms) = db.get_sync_payload_since(0).unwrap();
        assert_eq!(cats.len(), 1);
        assert_eq!(bms.len(), 1);
        assert_eq!(bms[0].title, "本地收藏");

        // 2. 模拟从云端同步新分类和新书签
        let cloud_cats = vec![crate::sync::CloudCategory {
            id: None,
            name: "云端分类".to_string(),
            parent_id: None,
            sort_order: Some(0),
            updated_at: 1000,
            is_deleted: Some(0),
        }];
        let cloud_bms = vec![crate::sync::CloudBookmark {
            id: None,
            title: "云端收藏".to_string(),
            url: "https://cloud-test.com".to_string(),
            url_key: Some("https://cloud-test.com".to_string()),
            icon: None,
            category_id: None,
            category_name: Some("云端分类".to_string()),
            tags: Some(vec!["云端".to_string()]),
            valid: Some(1),
            created_at: Some(1000),
            updated_at: 1000,
            is_deleted: Some(0),
        }];

        db.apply_cloud_sync_changes(&cloud_cats, &cloud_bms).unwrap();

        let all_cats = db.get_categories().unwrap();
        assert_eq!(all_cats.len(), 2);

        let all_bms = db.list_bookmarks(&BookmarkFilter {
            search: None,
            category_id: None,
            tag: None,
            valid_state: None,
            check_note: None,
            sort_by: None,
        }).unwrap();
        assert_eq!(all_bms.len(), 2);
    }
}
