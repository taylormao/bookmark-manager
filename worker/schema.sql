-- Cloudflare D1 Database Schema for Online Bookmark Manager

CREATE TABLE IF NOT EXISTS categories (
  id          INTEGER PRIMARY KEY AUTOINCREMENT,
  uuid        TEXT UNIQUE,
  name        TEXT NOT NULL,
  parent_id   INTEGER REFERENCES categories(id) ON DELETE CASCADE,
  sort_order  INTEGER DEFAULT 0,
  updated_at  INTEGER NOT NULL,
  is_deleted  INTEGER DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_categories_updated ON categories(updated_at);

CREATE TABLE IF NOT EXISTS tags (
  id   INTEGER PRIMARY KEY AUTOINCREMENT,
  name TEXT NOT NULL UNIQUE COLLATE NOCASE
);

CREATE TABLE IF NOT EXISTS bookmarks (
  id              INTEGER PRIMARY KEY AUTOINCREMENT,
  uuid            TEXT UNIQUE,
  title           TEXT NOT NULL,
  url             TEXT NOT NULL,
  url_key         TEXT NOT NULL,
  icon            TEXT,
  category_id     INTEGER REFERENCES categories(id) ON DELETE SET NULL,
  valid           INTEGER CHECK (valid IN (0,1) OR valid IS NULL),
  status_code     INTEGER,
  last_checked_at INTEGER,
  check_note      TEXT,
  created_at      INTEGER NOT NULL,
  updated_at      INTEGER NOT NULL,
  is_deleted      INTEGER DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_bookmarks_url_key  ON bookmarks(url_key);
CREATE INDEX IF NOT EXISTS idx_bookmarks_category ON bookmarks(category_id);
CREATE INDEX IF NOT EXISTS idx_bookmarks_updated  ON bookmarks(updated_at);
CREATE INDEX IF NOT EXISTS idx_bookmarks_deleted  ON bookmarks(is_deleted);

CREATE TABLE IF NOT EXISTS bookmark_tags (
  bookmark_id INTEGER NOT NULL REFERENCES bookmarks(id) ON DELETE CASCADE,
  tag_id      INTEGER NOT NULL REFERENCES tags(id) ON DELETE CASCADE,
  PRIMARY KEY (bookmark_id, tag_id)
);
CREATE INDEX IF NOT EXISTS idx_bookmark_tags_tag ON bookmark_tags(tag_id);

CREATE TABLE IF NOT EXISTS sync_meta (
  key   TEXT PRIMARY KEY,
  value TEXT NOT NULL
);
