# 本地网址收藏夹管理工具（Bookmark Manager）开发需求说明文档

> 版本：1.0
> 日期：2026-08-07
> 状态：已确认（决策点均已确认，可直接进入实现）
> 目标读者：AI 开发助手 / 软件工程师

---

## 0. 文档说明与阅读约定

### 0.1 文档目的

本文档是“本地网址收藏夹管理工具”（下称**本应用**）的唯一需求依据，用于直接指导开发。文档中的每一条规则都经过消歧，开发时应**严格按文字实现**；出现本文档未覆盖的情况时，以“确定性优先、不引入新概念”为原则自行补全，并在代码注释中说明。

### 0.2 阅读约定

- 功能需求使用编号 `FR-xx`，每条包含：**描述 / 触发与交互 / 详细规则 / 边界与异常 / 验收标准**。
- 优先级：`P0`（工程骨架，必须先有）、`P1`（核心管理，MVP 必须）、`P2`（校验与查重）、`P3`（云同步与代理）。
- “必须”“不得”“默认”为强制语义；“可”“建议”为可选语义。
- 时间统一使用 Unix 秒（UTC）；界面显示按系统时区。
- 数据库字段大小写遵循第 3 章 DDL 原文；URL、标签等的比较规则见对应章节。

---

## 1. 项目概述

### 1.1 背景与目标

开发一款**本地优先**的桌面网址收藏夹管理工具，用 C 语言编写，数据存储在本地 SQLite 数据库中。核心能力包括：标准书签文件导入导出、单条添加（自动抓取标题与图标）、编辑删除、多级分类、标签、排序与筛选、批量校验可用性（写入状态码）、基于 URL 归一化的查重与手动合并、批量操作、GitHub 与 S3 云同步，以及基于多个 VLESS 代理节点轮换的反爬应对能力。

应用必须**离线可用**：除“抓取标题/图标”“批量校验”“云同步”“代理测试”外，其余功能在断网环境下完整可用。

### 1.2 功能总览

| 编号 | 功能 | 对应需求章节 |
|---|---|---|
| 1 | 标准网址收藏夹导入 | FR-07 |
| 2 | 单条添加 + 自动抓取图标/标题 + 默认图标兜底 | FR-02、FR-03、FR-04 |
| 3 | 单条编辑（删、改） | FR-05、FR-06 |
| 4 | 多种排序 | FR-09 |
| 5 | 标签 | FR-11 |
| 6 | 筛选（标签/网址/分类）并可排序 | FR-12 |
| 7 | 单个网址属性（必填 3 项 + 可选属性） | 第 3 章 |
| 8 | 多级分类 | FR-10 |
| 9 | 查重（URL 归一化） | FR-15、附录 B |
| 10 | 手动确认合并 | FR-16 |
| 11 | 批量校验 + 状态码写入 | FR-14 |
| 12 | 多选/单选/框选 + 右键批量菜单 | FR-13 |
| 13 | 双击编辑对话框 | FR-05 |
| 14 | 导出标准 HTML 书签 | FR-08 |
| 15 | 云端同步（GitHub、S3） | FR-18、FR-19 |
| 16 | 反爬应对 + 多 VLESS 代理节点轮换 | FR-17、FR-14 |

### 1.3 术语表

| 术语 | 定义 |
|---|---|
| 网址 / 书签 | 一条收藏记录，必含标题、URL、图标三属性 |
| 分类 | 多级树形组织单元；一条网址最多属于一个分类；多归属用标签表达 |
| 标签 | 扁平的多对多标记，无层级，比较不区分大小写 |
| 去重键（url_key） | 按附录 B 归一化后得到的字符串，用于判断两条网址是否重复 |
| 有效性 valid | 三态：`NULL`（未校验或待复测）、`1`（有效）、`0`（无效） |
| 状态码 status_code | HTTP 状态码；网络层失败时为 `0` |
| 待复测 | valid 保持原值、check_note 为 `blocked` 的条目（疑似反爬拦截，需设置代理后重测） |
| 未分类 | category_id 为 NULL 的逻辑分组 |
| 标准 HTML 书签 | Netscape Bookmark File 格式（见附录 A），Chrome/Firefox/Edge 导出即此格式 |
| 备份 | 应用自动生成的 HTML 或数据库副本，位于 `<数据目录>/backups/` |
| 节点 | 代理配置单元（VLESS / HTTP / SOCKS5） |
| 同步目录 | GitHub 同步用的本地 git 工作副本目录 |

### 1.4 非目标（明确不做）

以下能力**不在**本需求范围内，实现时不得为它们预留复杂架构：

- 浏览器插件、移动端应用、Web 服务端；
- 网页内容存档（read-later 快照）、全文内容检索；
- 直接读取浏览器内部数据库（如 Chrome `Bookmarks` 文件）；只支持导入导出 HTML 文件；
- 多用户 / 权限 / 协作；
- 标签层级、标签颜色；
- 定时自动同步（仅手动触发；自动同步列为后续增强）；
- 自有 VLESS 协议实现（协议由 xray-core 子进程承担，见 FR-17）。

---

## 2. 技术选型与总体架构

### 2.1 技术选型（已确认，不得更改）

| 项 | 选型 | 约束/说明 |
|---|---|---|
| 语言 | C11 | GUI、核心库、CLI 均为 C；不得引入 C++ |
| 构建 | CMake ≥ 3.20 + Ninja | 支持 Windows（MSVC 或 MinGW-w64）与 Linux |
| GUI | GTK4（纯 C API） | Windows 用 gvsbuild/MSYS2 构建；不使用 webview/IUP/Qt |
| 数据库 | SQLite3 ≥ 3.35 | WAL 模式；启用外键约束 |
| HTTP 客户端 | libcurl ≥ 7.85 | 使用其 URL API、重定向/超时、HTTP/SOCKS5 代理、AWS SigV4 |
| HTML 解析 | Gumbo-parser | 提取 `<title>`、og:title、`<link rel=icon>`；导入文件解析可复用 |
| JSON | cJSON | 设置、同步元数据、xray 配置生成 |
| 图标渲染 | gdk-pixbuf（GTK 自带） | 支持 PNG/ICO/JPEG 解码与缩放 |
| GitHub 同步 | git CLI（外部命令） | 最低 2.30；不内嵌 libgit2 |
| S3 同步 | libcurl `CURLOPT_AWS_SIGV4` | 兼容 AWS S3 / MinIO / COS / OSS / R2 |
| VLESS 代理 | xray-core（≥1.8）子进程 | 应用不自研协议；发布包内置 xray-core 可执行文件 |
| 测试 | CTest + 轻量断言宏 | 不引入大型测试框架；见第 7 章 |

### 2.2 架构分层

```text
┌───────────────────────────────────────────────┐
│ GTK4 界面层（src/ui）                          │
│ 主窗口 / 列表 / 分类树 / 各对话框 / 进度与消息  │
├───────────────────────────────────────────────┤
│ 应用服务层（src/core，纯 C 静态库 libcore）     │
│ bookmark 服务 / 导入导出 / 排序筛选 /          │
│ 查重合并 / 校验 / 同步 / 代理管理 / 设置       │
├───────────────────────────────────────────────┤
│ 基础设施：SQLite3 · libcurl · Gumbo · cJSON   │
│            git CLI · xray-core 子进程          │
└───────────────────────────────────────────────┘
```

约束：

1. `libcore` 不依赖 GTK；所有业务规则（归一化、导入导出、合并、vless 解析）放在 `libcore`，必须可被 CLI 与单元测试直接调用。
2. 界面层只负责事件与展示，不直接写 SQL。
3. 网络请求全部在 worker 线程池执行，主线程只做 UI 更新；跨线程一律通过 GLib 主循环调度（`g_idle_add` / `GTask`）。

### 2.3 项目目录结构（目标结构，实现时保持）

```text
bookmark-manager/
├── CMakeLists.txt
├── src/
│   ├── core/          # libbookmark_core：db / url / fetch / validate / dedupe / sync / proxy / settings / backup
│   ├── ui/            # GTK4 应用入口与窗口、对话框
│   └── cli/           # bmk-cli：命令行冒烟与调试命令
├── tests/             # 单元与集成测试（ctest 注册）
├── data/              # 内置默认图标、示例 bookmarks.html、测试用本地 HTTP 服务脚本
├── third_party/       # vendored 依赖（按需）：gumbo、cJSON；xray-core 二进制单独发布
├── docs/
└── README.md
```

### 2.4 线程与并发模型

- 主线程：GTK 事件循环。
- 网络线程池：全局固定 4 个工作线程（可配置），承担抓取、校验、同步中的网络操作。
- SQLite：单写连接（WAL），网络线程的写操作通过队列串行化到同一写线程；读操作可另开只读连接。
- 子进程：git CLI 与 xray-core 均以子进程方式启动；启动与销毁由 `process` 模块统一管理；应用退出时强制结束所有子进程。
- 取消语义：所有长任务（导入/校验/同步）必须支持用户取消；取消后已完成的部分结果保留，未完成部分丢弃，并给出明确提示。

### 2.5 错误处理与日志

- 日志文件：`<数据目录>/logs/app.log`，级别 DEBUG/INFO/WARN/ERROR，默认 INFO，滚动上限 5MB×3。
- 所有网络/数据库/子进程错误必须被捕获并转为用户可读提示；日志中不得输出 PAT、密钥、完整 vless 链接（可输出脱敏后的节点名与地址）。
- 数据库操作使用事务；失败必须回滚并提示，不得留下半写状态。

---

## 3. 数据模型（SQLite）

### 3.1 通用约定

- 数据库文件：`<数据目录>/bookmarks.db`；`<数据目录>` 默认 = 系统应用数据目录（Windows 为 `%APPDATA%\BookmarkManager`），可在设置中修改。
- 建库即执行 `PRAGMA journal_mode=WAL; PRAGMA foreign_keys=ON;`。
- 所有时间字段为 INTEGER（Unix 秒）；布尔用 INTEGER 0/1；可空字段用 NULL。
- 文本统一 UTF-8。

### 3.2 表结构 DDL（实现必须与本 DDL 一致）

```sql
CREATE TABLE categories (
  id        INTEGER PRIMARY KEY AUTOINCREMENT,
  name      TEXT NOT NULL,
  parent_id INTEGER REFERENCES categories(id) ON DELETE CASCADE,
  UNIQUE (parent_id, name COLLATE NOCASE)
);

CREATE TABLE tags (
  id   INTEGER PRIMARY KEY AUTOINCREMENT,
  name TEXT NOT NULL UNIQUE COLLATE NOCASE
);

CREATE TABLE bookmarks (
  id              INTEGER PRIMARY KEY AUTOINCREMENT,
  title           TEXT NOT NULL,
  url             TEXT NOT NULL,
  url_key         TEXT NOT NULL,
  icon            BLOB,
  icon_mime       TEXT,
  category_id     INTEGER REFERENCES categories(id) ON DELETE SET NULL,
  valid           INTEGER CHECK (valid IN (0,1) OR valid IS NULL),
  status_code     INTEGER,
  last_checked_at INTEGER,
  check_note      TEXT,
  created_at      INTEGER NOT NULL,
  updated_at      INTEGER NOT NULL
);
CREATE INDEX idx_bookmarks_url_key  ON bookmarks(url_key);
CREATE INDEX idx_bookmarks_category ON bookmarks(category_id);
CREATE INDEX idx_bookmarks_valid    ON bookmarks(valid);

CREATE TABLE bookmark_tags (
  bookmark_id INTEGER NOT NULL REFERENCES bookmarks(id) ON DELETE CASCADE,
  tag_id      INTEGER NOT NULL REFERENCES tags(id) ON DELETE CASCADE,
  PRIMARY KEY (bookmark_id, tag_id)
);

CREATE TABLE deleted_log (
  id          INTEGER PRIMARY KEY AUTOINCREMENT,
  bookmark_id INTEGER NOT NULL,
  title       TEXT,
  url         TEXT,
  url_key     TEXT,
  category_id INTEGER,
  tags        TEXT,
  deleted_at  INTEGER NOT NULL,
  backup_file TEXT
);

CREATE TABLE sync_meta (
  key   TEXT PRIMARY KEY,
  value TEXT
);

CREATE TABLE settings (
  key   TEXT PRIMARY KEY,
  value TEXT
);
```

### 3.3 字段语义

| 字段 | 语义 |
|---|---|
| bookmarks.title | 必填；抓取失败时用 host 兜底，用户可改 |
| bookmarks.url | 必填；http/https（导入可含其他 scheme，见 FR-07） |
| bookmarks.url_key | 必填；按附录 B 生成；不设唯一约束（合并前允许重复），但必须建索引 |
| bookmarks.icon | 图标原始字节；可为 NULL（界面显示默认图标） |
| bookmarks.icon_mime | 如 `image/png`、`image/x-icon`；NULL 时界面按默认图标处理 |
| bookmarks.category_id | NULL = 未分类；删除分类时自动置 NULL |
| bookmarks.valid | 三态：NULL / 1 / 0 |
| bookmarks.status_code | HTTP 状态码；网络层失败为 0；未校验为 NULL |
| bookmarks.last_checked_at | 最近一次校验时间；未校验为 NULL |
| bookmarks.check_note | 校验备注：`ok` / 网络错误类型（`dns_failed`、`timeout`、`connect_failed`、`tls_failed`、`network_error`）/ `blocked`（待复测）/ 用户可见原因文本 |
| bookmarks.created_at / updated_at | 创建/最后修改时间；URL、标题、分类、标签、图标的变更都会刷新 updated_at |
| deleted_log | 删除与合并的审计记录；backup_file 指向自动备份文件 |
| settings | 所有配置 JSON 编码存入 value |
| sync_meta | 最近同步时间、仓库/桶标识、冲突备份计数等 |

### 3.4 数据完整性规则

1. 删除分类：外键 `ON DELETE CASCADE` 递归删除子分类；其下网址 `category_id` 自动置 NULL（界面须先弹二次确认并说明数量）。
2. 删除/合并网址：硬删除 + 写入 deleted_log + 自动生成备份文件（FR-06、FR-16）。
3. 标签重命名/合并：单事务内更新 `tags.name` 与 `bookmark_tags` 关联。
4. 应用升级采用 schema_version（settings 中 `schema_version` 键）做增量迁移；迁移前自动备份数据库副本。

---

## 4. 功能需求

### FR-01 应用启动与设置（P0）

**描述**：应用可正常启动、初始化数据库与数据目录、加载设置。

**触发与交互**：双击可执行文件启动；设置界面可修改数据目录、界面语言（中文/英文，默认中文）、主题（浅色/深色，默认浅色）。

**详细规则**：
1. 启动顺序：加载设置 → 打开/创建数据库 → 执行迁移 → 启动主窗口。
2. 数据库损坏（`SQLITE_CORRUPT` 等）时不得崩溃：提示用户，引导从最近备份恢复（FR-21），并可选择“重新初始化空库”。
3. 数据目录不存在时自动创建，并在其中创建 `backups/`、`logs/` 子目录。
4. 设置界面包含：数据目录（只读展示 + “打开”按钮）、图标服务开关（FR-03 第 3 步）、校验参数默认值（FR-14）、同步配置（FR-18/19）、代理配置入口（FR-17）、导入重复行为（FR-07）、备份按钮（FR-21）。

**边界与异常**：数据目录被占用/无权限时给出具体错误提示并退出。

**验收标准**：首次启动自动创建目录与空库；重复启动不报错；损坏库场景能提示并可恢复。

### FR-02 添加网址（P1）

**描述**：手动添加单条网址，自动抓取标题与图标，可同时指定分类与标签。

**触发与交互**：工具栏“添加”按钮或快捷键 `Ctrl+N`，弹出添加对话框。

**详细规则**：
1. 对话框字段：URL（必填）、标题（自动填充，可编辑）、分类（下拉树，可选）、标签（多选/输入，可选）、图标预览（抓取完成后显示，可“重新抓取”）。
2. URL 校验：仅接受 http/https；解析失败、host 为空、包含非法字符（如空格、控制字符）时禁止保存并提示原因。
3. 输入 URL 后自动触发抓取（FR-03）；抓取在后台进行，期间不阻塞对话框。
4. 保存时计算 `url_key`（附录 B）并写入；若与现有记录重复，提示“该网址与现有收藏重复（<现有标题>），是否仍要保存？”，确认后允许保存。
5. 标题兜底：抓取失败或为空时填入 host（如 `example.com`），用户可改。
6. 创建时间取保存时刻；valid/status_code/last_checked_at 初始为 NULL。

**边界与异常**：URL 含中文/IDN 时正常保存（归一化时转为 punycode 比较，见附录 B）；抓取超时不阻断保存。

**验收标准**：添加一条合法网址成功入库并立即显示；非法 URL 被拒绝；重复 URL 出现提示。

### FR-03 标题与图标自动抓取（P1）

**描述**：为指定 URL 抓取网页标题与网站图标。

**触发与交互**：添加时自动触发；编辑对话框与右键菜单提供“重新抓取”；抓取结果写入 title/icon/icon_mime。

**详细规则**：
1. 请求：GET，跟随重定向最多 5 次，超时 10s，最大接收 2 MiB；使用 UA 池（FR-14 第 4 条）。
2. 标题提取优先级：`<meta property="og:title">` → `<meta name="twitter:title">` → `<title>`；文本压缩空白（连续空白/换行合并为单个空格）后取首尾去空白；长度超过 300 字符截断。
3. 图标提取顺序（逐级尝试，任一成功即结束）：
   a. 从已抓取 HTML 中解析 `<link rel="icon"|"shortcut icon"|"apple-touch-icon">` 的 `href`（相对路径按页面 URL 绝对化）；
   b. 请求 `<scheme>://<host>/favicon.ico`；
   c. 外部图标服务（默认开启，可在设置关闭）：DuckDuckGo `https://icons.duckduckgo.com/ip3/<host>.ico`，失败再 Google `https://www.google.com/s2/favicons?domain=<host>&sz=64`；
   d. 全部失败 → 使用默认图标（FR-04）。
4. 图标合法性：响应 Content-Type 以 `image/` 开头且字节数 ≤ 512 KiB；否则视为失败并继续下一级。
5. 图标原始字节与 mime 入库；界面渲染时用 gdk-pixbuf 缩放到 16/32px。
6. 抓取失败（超时/网络错误/解析失败）不报错弹窗，仅保留当前值并在日志记 WARN。
7. 若跟随重定向后最终 URL 与输入不同且原响应为 301/308，提示“是否使用最终网址 <url>？”（默认否）。

**边界与异常**：favicon 返回 HTML 页面（Content-Type 不符）视为失败；`data:` URI 图标直接解码入库。

**验收标准**：对可访问网站能抓取标题与图标；无图标网站回退默认图标；关闭图标服务后不发起外部请求（可用抓包/日志验证）。

### FR-04 默认图标（P1）

**描述**：无法获取图标时使用内置默认图标。

**详细规则**：
1. 默认图标为项目内置资源（`data/default_icon.png`，64×64 PNG），应用不得依赖网络获取。
2. 显示逻辑：`icon` 为 NULL 或解码失败时显示默认图标；不修改数据库。
3. 用户可在编辑对话框中手动指定图标文件（PNG/JPEG/ICO，≤512 KiB）或“恢复默认”（将 icon 置 NULL）。

**验收标准**：无图标网址在列表中均显示默认图标。

### FR-05 编辑网址（P1）

**描述**：双击列表行打开编辑对话框，修改属性并保存。

**详细规则**：
1. 可编辑字段：标题、URL、分类、标签、图标（重新抓取/选择文件/恢复默认）。
2. 只读字段：创建时间、有效性、状态码、最近校验时间。
3. URL 修改后：若与修改前不同，提示“是否重新抓取标题与图标？”（默认是）；无论是否重抓，必须重算 `url_key`，并执行 FR-02 第 4 条的重复提示。
4. 保存规则：校验必填项；分类选择支持“未分类”（置 NULL）；标签编辑立即生效。
5. 保存成功刷新列表与分类计数；`updated_at` 更新。

**边界与异常**：URL 非法时禁止保存并提示；重复保存提示不阻止保存。

**验收标准**：双击可打开；所有可编辑字段修改后正确落库并刷新界面。

### FR-06 删除网址（P1）

**描述**：删除单条或批量网址，带二次确认与自动备份。

**详细规则**：
1. 触发：右键菜单“删除”、列表焦点 `Delete` 键。
2. 二次确认对话框：显示将删除的数量（≤5 条时列出标题）；按钮“取消 / 确认删除”。
3. 确认后执行事务：删除 `bookmarks` 与 `bookmark_tags` 关联；每条记录写入 `deleted_log`；**同时**将被删记录按导出格式（FR-08）追加写入 `<数据目录>/backups/deleted_<yyyyMMdd_HHmmss>.html`，`backup_file` 记录该路径。
4. 删除后刷新列表与统计。

**边界与异常**：备份文件写入失败时中止删除并提示（不允许无备份删除）；已删除内容可通过该 HTML 文件导入恢复。

**验收标准**：删除前必须出现确认框；删除后备份文件存在且含被删记录。

### FR-07 导入（P1）

**描述**：导入标准 Netscape HTML 书签文件（Chrome/Firefox/Edge 导出）。

**详细规则**：
1. 文件选择对话框导入；编码自动识别：UTF-8 BOM → UTF-8；无 BOM 先按 UTF-8 严格解码，失败按 GBK；对话框提供手动覆盖（自动/UTF-8/GBK）。
2. 解析规则（格式见附录 A）：
   - 按 `<DL>` 层级递归；`<H3>` 文本创建分类（同名同级去重：已存在同父同名分类则复用）；`<A>` 创建网址；
   - `<A>` 属性：`HREF` → url；文本 → title；`ADD_DATE` → created_at（缺失用导入时间）；`ICON` 为 `data:image/...;base64,...` 时解码入库（≤512 KiB），为 URL 时**不下载**、icon 置 NULL（可由“重新抓取”补齐）；`TAGS`（Firefox）按逗号拆分建标签；
   - 非 http/https 的 `<A>`（如 `javascript:`、`mailto:`）照常导入（图标默认），`url_key` 按附录 B 第 9 条处理；
   - `PERSONAL_TOOLBAR_FOLDER` 等扩展属性忽略，文件夹作为普通分类。
3. 重复处理（设置默认“跳过”）：按 `url_key` 判重；`跳过`=不导入并计数；`导入并标记`=照常导入不合并。
4. 导入是单事务，可中途取消（取消=回滚）；完成后弹出结果报告：新增 N、跳过 N（附明细列表，默认收起）、失败 N（附原因）。
5. 导入期间界面显示进度（解析条数），不阻塞其他窗口操作。

**边界与异常**：文件损坏/无 `<DL>` 结构时提示“不是有效的书签文件”并中止；单条 URL 非法时计入失败明细，不中断整体导入。

**验收标准**：用 Chrome 与 Firefox 各导出一份文件，导入后分类树、标题、URL、标签、图标（data URI）与源一致；重复条目默认被跳过并计数。

### FR-08 导出（P1）

**描述**：将全部或当前筛选结果导出为标准 HTML 书签文件。

**详细规则**：
1. 范围选项：全部 / 当前筛选结果（含当前排序顺序）。
2. 输出格式严格遵循附录 A；UTF-8、LF 换行、`ADD_DATE`=created_at、`LAST_MODIFIED`=updated_at、`TAGS` 逗号分隔、`ICON` 为 data URI（icon 存在时）。
3. 分类树映射为嵌套 `<DL><H3>`；未分类网址放根 `<DL>`。
4. 默认文件名 `bookmarks_<yyyyMMdd_HHmmss>.html`，保存位置由用户选择。
5. 导出为纯文件操作，不修改数据库。

**验收标准**：导出文件可被 Chrome/Firefox 重新导入且层级、标题、URL、标签、图标不丢失；导出后应用内导出文件的导入（round-trip）测试通过（第 7 章）。

### FR-09 排序（P1）

**描述**：列表按多种键排序，支持升/降序。

**详细规则**：
1. 可排序列：标题、URL、添加时间、分类、有效性、状态码、最近校验时间。
2. 排序规则（确定性）：
   - 标题/URL/分类名：不区分大小写（NOCASE），按 UTF-8 码点顺序；
   - 时间/状态码：数字序；NULL 在升序中排最后、降序中也排最后；
   - 有效性：升序 `0 → NULL → 1`，降序 `1 → NULL → 0`；
   - 分类：按分类名，NULL（未分类）排最后；
   - 恒有二级键 `id ASC`，保证顺序稳定。
3. 点击列头切换升/降序并显示箭头；默认按“添加时间 降序”。
4. 排序作用于当前筛选结果。

**验收标准**：对含 NULL 与中文的数据，各列升/降序结果与规则一致（有单元测试覆盖）。

### FR-10 分类管理（P1）

**描述**：多级分类树的新建、重命名、移动、删除。

**详细规则**：
1. 树结构：根节点为固定“全部”（显示所有）；“未分类”为固定过滤节点（category_id IS NULL）；其后为分类树。
2. 操作：右键或工具栏“新建分类”（默认挂在当前选中节点下）、重命名、移动（选择目标父级，禁止移动到自身或子孙）、删除。
3. 约束：同一父级下分类名不区分大小写唯一，冲突提示；支持无限层级（实现上至少支持 32 层）。
4. 删除：递归删除子分类；其下网址自动置“未分类”；删除前弹确认框并显示受影响网址数量。
5. 每个分类节点显示计数（含子孙的网址数）。

**验收标准**：树 CRUD 正确；删除父分类后子分类消失、网址归入未分类且计数正确。

### FR-11 标签管理（P1）

**描述**：标签的添加、重命名、合并、删除；标签不区分大小写。

**详细规则**：
1. 标签录入：输入或从已有标签选择；自动去首尾空格；同名不同大小写自动合并（`Dev` 与 `dev` 视为同一标签，保存先创建者的大小写形式）。
2. 管理对话框：列出全部标签及使用次数；支持重命名（单事务全局替换）、合并（A 并入 B，A 删除，关联迁移到 B）、删除（仅解除关联，不删除网址）。
3. 网址的标签在编辑对话框与“批量添加标签”（FR-13）中维护。

**验收标准**：大小写变体合并；重命名/合并/删除后关联关系正确。

### FR-12 筛选（P1）

**描述**：按关键词、标签、分类、有效性、状态码、域名筛选；筛选结果可排序、可导出、可批量操作。

**详细规则**：
1. 筛选面板（工具栏下方，可折叠）包含：
   - 关键词：对 title 与 url 做不区分大小写的子串匹配（OR 语义）；
   - 网址包含：对 url 子串匹配（独立维度，与关键词 AND）；
   - 域名：对归一化 host 子串匹配（独立维度）；
   - 标签：多选，选中标签之间 OR；
   - 分类：单选下拉树，选中即包含其全部子孙分类；选项含“全部”“未分类”；
   - 有效性：全部 / 有效 / 无效 / 未校验 / 待复测；
   - 状态码区间：起止可空（如 400–499）。
2. 组合规则：**维度之间 AND，维度内部 OR**。示例见附录 E。
3. 关键词输入 300ms 防抖实时筛选；其余控件即时生效。
4. 筛选结果可应用 FR-09 排序，并可导出（FR-08）、批量校验（FR-14）、批量操作（FR-13）。
5. 空结果时显示“无匹配记录”占位。

**验收标准**：组合条件结果与附录 E 用例一致；筛选后排序、导出、批量操作均作用于筛选结果。

### FR-13 多选 / 框选与右键菜单（P1）

**描述**：单选、多选、框选；右键菜单提供删除、重新校验、合并、批量分类、批量标签。

**详细规则**：
1. 每行首列复选框；点击切换单行；`Ctrl` 点击切换、`Shift` 点击范围选择；支持在列表空白处按住左键拖拽框选（GTK4 用 `GtkGestureDrag` 自绘选区，选中矩形内所有行）。
2. 右键菜单项及启用条件：

| 菜单项 | 启用条件 | 行为 |
|---|---|---|
| 打开 | 恰 1 条 | 系统默认浏览器打开 |
| 复制链接 | ≥1 条 | 复制 URL（多条按行分隔） |
| 编辑 | 恰 1 条 | 打开 FR-05 编辑对话框 |
| 删除 | ≥1 条 | 走 FR-06 |
| 重新校验 | ≥1 条 | 走 FR-14（仅选中项） |
| 合并网址 | ≥2 条且所选 url_key 全部相同 | 走 FR-16；否则置灰 |
| 批量添加标签 | ≥2 条 | 对话框输入标签，单事务关联到全部选中项 |
| 批量设置分类 | ≥2 条 | 对话框选择分类/未分类，单事务更新 |

3. 批量操作后刷新列表与计数，并显示结果提示（如“已为 5 条记录添加标签”）。

**验收标准**：三种选择方式可用；菜单启用/置灰符合上表；批量操作事务正确。

### FR-14 批量校验（P2）

**描述**：批量判断网址是否可访问，写入状态码与有效性；疑似反爬的条目标记为“待复测”。

**详细规则**：
1. 触发：工具栏“校验”或右键“重新校验”，范围=当前筛选结果或选中项。
2. 请求方法：默认 HEAD；响应为 405/501 或服务器不返回头信息时自动降级 GET（响应体最多接收 1 MiB 后中断，只取状态码）。
3. 参数（设置可改，括号内为默认）：超时 10s、连接超时 5s、最大重定向 5、并发 4（1–10）、同域名最大并发 2、请求间隔 500ms ± 200ms 随机抖动、失败重试 1 次（仅网络错误与 5xx）。
4. UA 池：内置 5 个常见浏览器 UA，按请求轮换。
5. 结果判定：
   - 最终状态码 2xx → valid=1，check_note=`ok`；
   - 3xx（跟随重定向后）→ 按最终码判定；
   - 4xx/5xx → valid=0，status_code=实际码；
   - 网络层失败（DNS/超时/连接拒绝/TLS/其他）→ valid=0，status_code=0，check_note=`dns_failed`/`timeout`/`connect_failed`/`tls_failed`/`network_error`；
   - **403/429 → valid 保持原值（不改为 0），status_code 写入实际码，check_note=`blocked`**（即“待复测”，见术语表）。
6. 每次判定写入 last_checked_at。
7. 代理行为：设置“直连/全部走代理/自动”（默认直连）；`自动`=直连失败或 blocked 时，按 FR-17 轮换策略换节点重试 1 次。
8. 进度对话框：总体进度条、成功/失败/待复测计数、日志区（脱敏）、“取消”按钮；取消后已写入结果保留。
9. 校验不修改 URL/标题/图标，不触发 FR-06 备份。

**边界与异常**：证书校验失败按网络错误处理；校验期间 URL 被删除则跳过并计数。

**验收标准**：对 mock 服务器（200/404/405 降级 GET/403/429/超时/重定向）的判定全部符合第 5 条；并发下 UI 不卡顿；取消后可继续使用。

### FR-15 查重（P2）

**描述**：按 url_key 查找重复分组。

**详细规则**：
1. 触发：工具栏“查重”。
2. 分组规则：`SELECT url_key, COUNT(*) FROM bookmarks GROUP BY url_key HAVING COUNT(*) > 1`；每组列出成员（标题、URL、有效性、创建时间）。
3. 结果对话框：分组列表，每组可勾选；提供“全选/全不选”；显示总重复条数。
4. 归一化算法见附录 B，结果必须与附录 B 用例表完全一致。
5. 查重本身不修改数据。

**验收标准**：附录 B 用例全部通过单元测试；重复分组与预期一致。

### FR-16 合并（P2）

**描述**：对重复分组手动确认后合并。

**详细规则**：
1. 触发：查重对话框内“合并选中组”，或右键“合并网址”（FR-13）。
2. **必须手动确认**：确认对话框列出每组摘要（成员条数与标题），按钮“取消 / 合并全部”；逐组确认模式可选（“逐组确认”开关，默认关）。
3. 组内主记录 = `created_at` 最早者（并列取 id 最小）；合并规则：
   - title/url 取主记录；
   - icon/icon_mime：主记录为 NULL 时取组内第一条非 NULL；
   - category_id：主记录优先，NULL 则取组内最早的非 NULL；
   - 标签：组内并集；
   - valid/status_code/last_checked_at/check_note：取组内 `last_checked_at` 最新者（均为 NULL 则保持 NULL）；
   - created_at 保持主记录，updated_at=合并时刻。
4. 合并前：按 FR-06 第 3 条生成备份文件（含将被合并删除的全部记录）并写 deleted_log；合并过程为单事务。
5. 合并期间组内记录已被外部修改/删除 → 跳过该组并提示。

**验收标准**：确认前不执行任何写入；合并后主记录属性符合第 3 条；备份文件与 deleted_log 可追溯。

### FR-17 代理节点管理（P3）

**描述**：管理多个代理节点（VLESS / HTTP / SOCKS5），用于校验与抓取时的轮换。

**详细规则**：
1. 节点字段：名称、类型（VLESS/HTTP/SOCKS5）、启用开关；VLESS 支持粘贴 `vless://` 链接自动解析（字段见附录 C）或手动填写；HTTP/SOCKS5 支持地址、端口、可选用户名/密码。
2. VLESS 实现：**不得自研协议**。应用为每个启用的 VLESS 节点生成 xray-core `config.json`（附录 C 模板），以子进程启动；入站为 `127.0.0.1:<动态端口>` 的 SOCKS5；libcurl 通过 `CURLOPT_PROXY=socks5h://127.0.0.1:<port>` 使用。
3. 子进程管理：节点启用即拉起；应用退出统一终止；进程异常退出自动重启（每会话最多 3 次/节点），超过则标记节点不可用。
4. 节点测试：连通性测试请求默认 `https://www.google.com/generate_204`（设置可改），超时 8s；失败节点自动标记不可用并可从轮换池剔除。
5. 轮换策略（设置可选）：`轮询`（按请求顺序逐个使用，默认）/ `随机` / `失败切换`（仅失败重试时换下一个）/ `按域名固定`（同一域名整会话内固定同一节点）。
6. 应用范围：校验（FR-14）与抓取（FR-03）可选择“直连 / 全部走代理 / 自动”。
7. 隐私：节点列表只存本地；日志输出脱敏（不打印 UUID、完整链接）。

**边界与异常**：xray-core 缺失或启动失败 → 对应节点不可用并提示，不影响直连功能；端口冲突自动换端口。

**验收标准**：可添加/编辑/删除/测试节点；`vless://` 解析字段与附录 C 映射一致；启用后校验流量走代理（日志可见）；轮询策略下连续请求轮流使用不同节点。

> **补充条款（2026-11-07，随实现一并落地）**：
>
> 8. **设置页直接配置与即时校验**：设置页提供代理配置区，可直接选择协议
>    （HTTP/SOCKS5）、填写服务器地址、端口（1-65535）、可选用户名/密码，并当场校验，
>    无需先保存为节点。「测试当前输入」不读写节点池、不落库、不启动子进程；
>    「保存并启用」将配置存为节点并把代理模式置为「全部走代理」。
> 9. **分层校验语义**（`bmk_proxy_endpoint_check`，吃完整配置而非节点 id）：
>    - 阶段 1 TCP：连接 `<host:port>`，超时 3s；
>    - 阶段 2 协议握手：HTTP 代理发 CONNECT 并校验状态行（407 明确判为认证失败），
>      SOCKS5 做 greeting 与可选 RFC1929 用户名/密码子协商，响应非约定格式即判「协议不匹配」；
>    - 阶段 3 目标站（可选）：经代理请求测试 URL（默认 `https://www.google.com/generate_204`，
>      设置可改），2xx/3xx 为通过，407/403/429 明确判失败；目标站不可达**不改变**
>      「代理本身可用」的结论（避免把「代理正确、仅目标站被墙」误报为配置错误）。
>    - 三阶段的状态/耗时/原因在界面与 CLI 分别展示。
> 10. **CLI**：`proxy add --type http|socks5 --host H --port P [--user U --pass P] [--name N]`、
>     `proxy check --type ... --host ... --port ... [--url URL]`、`proxy test <id> [--url URL]`。
> 11. **代理模式落地**：设置页的「直连 / 全部走代理 / 自动」写入 `proxy_mode`，批量校验
>     （FR-14 第 7 条）读取该键；兼容早期仅写 `check_use_proxy` 布尔键的旧配置（缺失时回退）。
> 12. HTTP/SOCKS5 节点不启动子进程；凭据在代理串中做百分号编码，并经
>     `CURLOPT_PROXYUSERPWD` 传递（密码含 `@ : / #` 等字符时不会破坏代理串结构）。
> 13. **节点测试修正**：一次连通性测试判为 407/403/429 的响应不再计入「可用」，
>     测试失败节点从轮换池剔除（与第 4 条一致），结果按三阶段归因，便于区分
>     「端口不通 / 协议填错 / 认证失败 / 目标站不通」。
> 14. **现代桌面端统一代理上网设置（Tauri / Rust 落地，2026-11-07）**：
>     - **代理模式**：支持 `direct`（完全直连）、`system`（遵循操作系统系统代理）、`custom`（自定义 HTTP / SOCKS5 代理串，例如 `http://127.0.0.1:7890` 或 `socks5h://127.0.0.1:10808`）。
>     - **网络统一接管**：该代理配置全局接管网页元信息抓取（Title、Favicon）与批量死链/反爬校验流程，解决墙外或受限站点的抓取报错。
>     - **即时连通性检测**：界面提供一键“测试代理”功能，向基准测试站（如 Google 204 或 Cloudflare）发起握手探测，实时汇报连通状态与往返延迟（ms）。
>     - **持久化**：配置写入 SQLite `settings` 表（`proxy_mode`、`proxy_url`），跨会话持久保留。

### FR-18 GitHub 云同步（P3）

**描述**：将书签推送到 GitHub 私有仓库，并支持从云端恢复。

**详细规则**：
1. 配置项：仓库地址（https 或 ssh）、分支（留空自动探测 `master`/`main`）、git 可执行路径（自动探测 PATH）、认证方式（HTTPS+PAT 或 SSH）、是否同时上传数据库快照（默认关）。
2. 同步内容：`bookmarks.html`（每次同步由 FR-08 逻辑生成，范围=全部）必传；`bookmarks-<时间戳>.db` 快照可选。
3. 同步目录：`<数据目录>/sync/github/`；首次执行 `git clone`，后续复用。
4. “立即同步”执行序列（确定性）：
   a. `git fetch origin`；
   b. 若本地落后于远端（可快进）→ `git merge --ff-only origin/<branch>`；
   c. 若本地与远端分叉 → 先用 `git show origin/<branch>:bookmarks.html` 保存为 `backups/remote_<时间戳>.html`（如该对象存在）；
   d. 用本地导出的 `bookmarks.html`（与可选 db 快照）覆盖工作区文件；
   e. `git add` + `git commit -m "sync <ISO 时间>"`；
   f. 推送：分叉时 `git push --force-with-lease origin <branch>`，否则普通 `git push`；
   g. 成功后写入 `sync_meta.last_sync` 与 `sync_meta.github_remote`。
5. 冲突策略：**以本地为准**；远端旧版已备份（第 4c 步），用户可从备份文件手动恢复。
6. 同步界面：显示最近同步时间、远端地址、执行日志（可展开）、错误信息；“从云端恢复”= 拉取后将远端 `bookmarks.html` 走 FR-07 导入流程（默认跳过重复）。
7. 凭据安全：PAT 只存本地设置，日志与界面均不显示明文。

**边界与异常**：网络失败/认证失败给出明确错误；git 不在 PATH 且未配置时提示安装；首次同步前必须确认仓库已存在。

**验收标准**：配置后可完成首次同步；本地删除一条书签再同步，远端文件同步删除；人为制造分叉后远端旧版被备份且推送成功。

### FR-19 S3 云同步（P3）

**描述**：将书签推送到 S3 兼容存储，并支持从云端恢复。

**详细规则**：
1. 配置项：endpoint（默认 `https://s3.<region>.amazonaws.com`）、region（默认 `us-east-1`）、bucket、prefix（可选）、access_key、secret_key、path_style（默认开，MinIO/COS/OSS 用）。
2. 实现：libcurl `CURLOPT_HTTPAUTH=CURLAUTH_AWS_SIGV4`、`CURLOPT_AWS_SIGV4="aws:amz:<region>:s3"`、`CURLOPT_USERPWD="<access_key>:<secret_key>"`；上传 `PUT`，下载 `GET`，对象名 `<prefix>/bookmarks.html`（必传）与 `<prefix>/bookmarks-<时间戳>.db`（可选快照）。
3. “推送”执行序列：
   a. 凭据校验：`HEAD <prefix>/bookmarks.html`（404 视为合法）；
   b. 若远端对象存在 → 先下载保存为 `backups/remote_<时间戳>.html`；
   c. 上传本地导出文件；
   d. 成功后写入 `sync_meta`。
4. “从云端恢复”= 下载远端 `bookmarks.html` 走 FR-07 导入（默认跳过重复）。
5. 冲突策略：同 FR-18 第 5 条（以本地为准 + 远端备份）。
6. secret_key 输入遮蔽，可切换显示；不落日志。

**边界与异常**：endpoint 不可达/凭据无效给出明确错误；bucket 不存在时提示先创建。

**验收标准**：对 MinIO（本地起一个实例）可完成推/拉；对象内容与本地导出一致；远端旧版在推送前被备份。

### FR-20 主界面布局与交互（P1）

**描述**：主窗口布局与通用交互规范。

**详细规则**：
1. 窗口默认 1200×760，可缩放；左侧分类树宽 220（可拖拽调宽）。
2. 布局：
   - 顶部工具栏：添加、导入、导出、校验、查重、同步、代理设置、设置、搜索框（等价 FR-12 关键词）；
   - 左侧：固定节点（全部、未分类、待复测、无效）+ 分类树（计数）；
   - 中部：书签列表（列：选择框、图标、标题、URL、分类、标签、有效性、状态码、添加时间）；
   - 底部状态栏：总数 / 当前筛选数 / 无效数 / 待复测数 / 最近同步时间。
3. 列表列头点击排序（FR-09）；列可调宽；支持滚动；空态显示引导文案（“添加”或“导入”按钮）。
4. 快捷键：`Ctrl+N` 添加、`Ctrl+E` 编辑选中、`Delete` 删除选中、`Ctrl+F` 聚焦搜索、`F5` 校验选中、`Ctrl+D` 查重、`Ctrl+S` 同步。
5. 双击行 = 编辑（FR-05）；右键菜单 = FR-13。
6. 有效性列显示：有效（绿点）/无效（红点）/未校验（灰点）/待复测（黄点 + “待复测”字样）。
7. 所有确认框按钮明确（“取消”永远可用）；所有长任务有进度与取消。

**验收标准**：主窗口布局与快捷键全部可用；多选/双击/右键交互符合本文档。

### FR-21 备份与恢复（P2）

**描述**：自动/手动备份与恢复。

**详细规则**：
1. 备份目录 `<数据目录>/backups/`；文件名含类型与时间戳。
2. 自动备份触发点：删除（FR-06）、合并（FR-16）、同步覆盖前（FR-18/19）、schema 迁移前（FR-01）。
3. 手动备份：设置中“立即备份”→ 生成全量 `bookmarks.html` + `bookmarks.db` 副本。
4. 恢复：从备份 HTML 走 FR-07 导入；数据库副本恢复 = 替换数据文件（先备份当前库）；数据库损坏时引导恢复（FR-01）。
5. 备份保留策略：保留最近 20 份全量备份与 7 天内的自动备份，可手动清理。

**验收标准**：各触发点均产生备份文件；用备份可完整恢复数据。

### FR-22 状态栏与消息反馈（P1）

**描述**：全局状态与操作反馈。

**详细规则**：
1. 状态栏常驻显示第 FR-20 条第 2 节计数与同步时间。
2. 短消息（成功/失败）用 toast 显示 4s；破坏性操作错误用对话框。
3. 所有异步任务完成或失败必须有明确反馈，不得静默失败。

---

## 2.4 新阶段扩展需求（现代桌面端与生态联动，2026-11-07）

### FR-23 分类管理与层级/顺序拖拽（P1）

**描述**：支持对书签分类进行全生命周期管理，包括重命名、递归/级联删除、同级拖动调整顺序、以及拖动改变父子层级关系。

**详细规则**：
1. **分类重命名**：支持点击分类旁编辑图标或双击名称即时修改分类名，并在 SQLite 中更新。
2. **分类删除**：删除分类时支持将分类下书签安全移至“未分类”（`SET NULL`），子分类级联删除或提升为顶层。
3. **同级拖动排序**：分类表增加 `sort_order` 字段，支持在分类树中拖拽上/下移动位置，持久化顺序。
4. **层级拖拽变更**：支持将某一分类拖拽放到另一分类上，将其变为子分类（原子更新 `parent_id`）；防止成环（不能将父节点拖入其自身的子孙节点中）。

### FR-24 网址卡片拖拽归类（P1）

**描述**：用户可直接通过鼠标拖拉右侧书签列表/卡片中的任意网址，拖入左侧分类树的对应节点中并松开，即刻完成书签的分类变更。

**详细规则**：
1. 采用 HTML5 Drag & Drop 规范，拖拽时卡片显示半透明抓取状态。
2. 左侧分类节点在悬停（dragover）时高亮显示接收状态。
3. 释放（drop）后，前端通过 Tauri 命令原子更新目标书签的 `category_id`，并即时更新视图。

### FR-25 基于 Cloudflare Workers 的独立在线网络收藏夹与远端云同步（P2）

**描述**：将远端模块设计为基于 Cloudflare Workers + D1 的独立可运行在线网络收藏夹系统，支持通过 GitHub Actions 自动化 CI/CD 部署。既能作为独立 Web 端在线收藏夹直接访问和管理，又能为桌面客户端提供高效的双向增量同步服务。

**详细规则**：
1. **GitHub 自动化部署 (CI/CD)**：
   - 建立 `.github/workflows/deploy-worker.yml`，在推送到仓库主分支或手动触发时，自动通过 `cloudflare/wrangler-action` 部署 Worker 与执行 D1 数据库迁移。
   - 用户只需在 GitHub 仓库中配置 `CLOUDFLARE_API_TOKEN` 和 `CLOUDFLARE_ACCOUNT_ID` 即可实现全自动交付。
2. **独立在线 Web 收藏夹功能**：
   - 访问 Worker 域名时提供轻量、现代、自适应移动端的独立 Web 应用界面。
   - 支持网页端实时检索、按分类/标签筛选、打开链接、新增/编辑/删除书签。
   - 支持安全密码鉴权（Token / Session Cookie），保障在线私有收藏数据的安全。
   - 支持云端边缘自动抓取标题与 Favicon 图标。
3. **服务端架构与存储**：
   - 基于 Cloudflare Workers + D1（Serverless SQLite），数据结构与桌面端 SQLite 高度同构（`bookmarks`, `categories`, `tags`, `bookmark_tags`）。
   - 引入软删除机制（`is_deleted` 与 `updated_at`），精准支持多端无缝双向增量同步。
4. **客户端与插件同步 API**：
   - `GET /api/sync?since=<timestamp>`：增量拉取自指定时间以来的所有新增、修改和删除记录。
   - `POST /api/sync`：提交本地增量变更，服务端原子事务合并后返回最新时间戳。
   - `POST /api/bookmarks`：专供 Chrome 浏览器插件与快捷工具调用的单条收藏端点。
5. **桌面客户端操作**：
   - 桌面端配置 Cloudflare Worker 地址与同步 Token。
   - 提供“一键同步”、“从云端拉取”、“覆盖云端”等选项，支持自动/定时后台同步与操作日志展示。

### FR-26 配套 Chrome 浏览器插件（Manifest V3）（P2）

**描述**：配套开发一款轻量 Chrome 浏览器扩展插件，实现浏览器端与 Cloudflare Workers 的无缝联动。

**详细规则**：
1. **标准规范**：严格遵循 Chrome Extension Manifest V3 规范。
2. **快速收藏**：点击插件图标弹出精美 Popup，一键自动获取当前活动标签页的 URL、Title、Favicon，并支持快捷选择分类与标签保存。
3. **云端互通**：插件直连 Cloudflare Workers 同步端点，保存的书签直接推送到云端；同时支持从云端拉取书签列表在插件中快捷搜索与打开。
4. **全生态闭环**：浏览器插件保存 -> Cloudflare 边缘存储 -> 桌面客户端一键拉取呈现。

**验收标准**：各操作均有可见反馈。

---

## 5. 非功能需求

### 5.1 性能

1. 10,000 条书签、200 个标签、300 个分类下：启动 < 2s；列表滚动流畅（无卡顿感）；排序与筛选响应 < 200ms。
2. 校验并发 10 时 UI 线程不阻塞，界面响应 < 100ms。
3. 导入 10,000 条标准文件 < 10s（不含抓取）。

### 5.2 可靠性

1. 所有写操作为事务；异常中断不产生半写状态（WAL + 回滚）。
2. 数据库损坏可检测并引导恢复；删除/合并/同步均可追溯（deleted_log + 备份）。
3. 应用崩溃后重启不丢已提交数据。

### 5.3 安全与隐私

1. PAT、S3 密钥、代理 UUID 不得出现在日志、界面默认展示（遮蔽）、导出文件或同步仓库中。
2. 外部图标服务默认开启但可全局关闭；关闭后不得发起任何外部请求（FR-03）。
3. 设置文件权限收紧（Windows：当前用户可读写）。
4. xray-core 以子进程运行，仅绑定 127.0.0.1 动态端口。

### 5.4 兼容性

1. 主目标平台：Windows 10/11 x64；保持 Linux（GTK4）可编译。
2. 数据库文件跨平台兼容；导入导出文件在 Chrome/Firefox/Edge 间互通。
3. 支持 UTF-8 与 GBK 编码的书签文件（FR-07）。

### 5.5 可维护性与测试

1. 核心规则（归一化、导入导出、合并、vless 解析）为纯函数，无全局状态，可直接单元测试。
2. 全部功能需求有对应验收标准与测试（第 6、7 章）。
3. 代码注释使用中文或英文均可，但模块职责与边界必须清晰；公共函数必须有文档注释。

### 5.6 国际化

1. 界面字符串集中管理（至少中/英两套，默认中文）；界面文本不得硬编码散落。
2. 日志与错误提示支持中英（错误码稳定，文案可翻译）。

---

## 6. 验收标准汇总

按 P0→P3 里程碑分别验收：

| 里程碑 | 验收要点 |
|---|---|
| P0 | 空库启动、目录创建、日志、设置读写、损坏库提示 |
| P1 | FR-02~FR-13、FR-20、FR-22 全部验收条目通过；10k 条数据下性能达标 |
| P2 | FR-14~FR-16、FR-21 验收条目通过；附录 B 归一化用例 100% 通过；删除/合并可追溯 |
| P3 | FR-17~FR-19 验收条目通过；MinIO 实例与 GitHub 测试仓库各完成一次推/拉验证 |

任一功能验收不通过视为该里程碑未完成。

---

## 7. 测试要求

### 7.1 单元测试（必须）

1. URL 归一化：附录 B 用例表逐条断言（含 NULL/空值/大小写/编码/IDN 边界）。
2. 导入导出 round-trip：样例文件（`data/example_bookmarks.html`）导入→导出→再导入，结构与属性一致。
3. 合并规则：主记录选择、标签并集、字段补齐、并列 id 规则。
4. vless 解析：附录 C 字段映射用例。
5. 排序/筛选：FR-09、FR-12 规则用例（含 NULL 排序位置）。

### 7.2 集成测试（必须）

1. 用本地 HTTP 测试服务器（`tests/mock_http_server.py` 或测试内嵌 C 服务器）覆盖：200、404、403、429、405（触发 GET 降级）、5xx、重定向、超时、TLS 失败。
2. git 同步：本地建临时 git 仓库（`git init`）模拟远端，验证快进、分叉备份、force-with-lease 推送。
3. S3：本地 MinIO 容器/进程验证推/拉与凭据校验。
4. xray-core：用测试用 vless 节点（或仅验证配置生成与进程生命周期）验证启用/退出清理。

> **实现说明（2026-11-07）**：第 1 条以内嵌 C mock server（`tests/test_validate.c`）落地，
> 覆盖 200 / 404 / 403 / 429 / 405 降级 / 302 单跳与两跳链 / 超时 / 5xx 最终码 /
> 真实 TLS 失败（对 HTTP 端口发起 https 请求）；第 2 条以 `tests/test_sync.c` 的本地
> bare 仓库用例落地（快进、分叉备份含第三方改动 + force-with-lease 以本地为准）。
> 第 3、4 条依赖 MinIO 实例与 xray 二进制，按 P3 计划实跑验证。
> 另 `tests/test_perf.c` 落地 §5.1 第 1/3 条的 10,000 条自动化基准
> （筛选+排序 < 200ms、导入 < 10s，均已达标）；启动时间与界面流畅度仍需人工观察。

### 7.3 UI 手工测试清单（交付前必须逐项执行）

- 添加（含抓取与默认图标）；编辑（双击）；删除（确认/备份）；
- 导入导出（Chrome、Firefox 各一份）；排序；筛选组合；多选/框选/右键菜单；
- 查重与合并；批量校验进度与取消；代理节点增删与测试；
- GitHub/S3 推拉；深色主题；空态与错误提示。

---

## 8. 里程碑与开发顺序

依赖顺序固定，前一阶段完成（验收通过）后才进入下一阶段：

| 阶段 | 范围 | 依赖 | 说明 |
|---|---|---|---|
| P0 骨架 | FR-01、工程结构、CI 冒烟测试 | 无 | 建立 libcore + GTK4 壳 + CLI |
| P1 核心管理 | FR-02~FR-13、FR-20、FR-22 | P0 | 交付可完整日常使用的收藏夹 |
| P2 校验与查重 | FR-14~FR-16、FR-21 | P1 | 覆盖需求 9/10/11；含归一化与备份体系 |
| P3 云同步与代理 | FR-17~FR-19 | P2 | 风险最高的 VLESS/同步放最后；含打包与文档 |

实现顺序建议（同一阶段内）：
1. libcore 数据层（DDL、CRUD、设置）→ 2. 归一化 + 导入导出（可先用 CLI 验证）→ 3. 抓取（标题/图标）→ 4. UI 主窗口与列表 → 5. 编辑/删除/分类/标签 → 6. 排序筛选 → 7. 批量操作 → 8. 校验 → 9. 查重合并 → 10. 备份 → 11. 代理 → 12. 同步 → 13. 打包。

---

## 9. 附录

### 附录 A：Netscape HTML 书签格式规范与示例

格式要点：

- 首行 `<!DOCTYPE NETSCAPE-Bookmark-file-1>`；
- `<META HTTP-EQUIV="Content-Type" CONTENT="text/html; charset=UTF-8">`；
- 结构：`<H1>标题</H1>` → 根 `<DL><p>`；`<DT><H3 ...>文件夹名</H3>` 后跟 `<DL><p>...</DL><p>` 表示文件夹内容；`<DT><A ...>名称</A>` 表示网址；
- 时间属性为 Unix 秒：`ADD_DATE`、`LAST_MODIFIED`；
- `ICON` 属性为 data URI（Chrome/Firefox 均导出，如 `ICON="data:image/png;base64,..."`）；
- `TAGS` 属性为逗号分隔标签（Firefox 导出）；
- `PERSONAL_TOOLBAR_FOLDER="true"` 表示书签栏文件夹（导入时视为普通分类）。

示例文件（导入测试基准，存于 `data/example_bookmarks.html`）：

```html
<!DOCTYPE NETSCAPE-Bookmark-file-1>
<META HTTP-EQUIV="Content-Type" CONTENT="text/html; charset=UTF-8">
<TITLE>Bookmarks</TITLE>
<H1>Bookmarks</H1>
<DL><p>
    <DT><H3 ADD_DATE="1700000000" LAST_MODIFIED="1700000100" PERSONAL_TOOLBAR_FOLDER="true">书签栏</H3>
    <DL><p>
        <DT><A HREF="https://example.com/" ADD_DATE="1700000001" ICON="data:image/png;base64,iVBORw0KGgo=">Example</A>
        <DT><A HREF="https://github.com/" ADD_DATE="1700000002" TAGS="dev,code">GitHub</A>
    </DL><p>
    <DT><H3 ADD_DATE="1700001000">技术</H3>
    <DL><p>
        <DT><A HREF="https://news.ycombinator.com/" ADD_DATE="1700001001">Hacker News</A>
    </DL><p>
</DL><p>
```

### 附录 B：URL 归一化规则详表与用例

**适用范围**：仅 `http`/`https` 执行完整管线；其他 scheme 的 url_key = 去掉 fragment 后的完整 URL 原文。

**管线（按序执行）**：
1. 用 libcurl URL API 解析 URL（失败则该条 URL 不合法）；
2. scheme 转小写；
3. host 转小写，并剥离前缀 `www.`（如 `Www.Example.COM` → `example.com`）；
4. 删除默认端口（http=80、https=443）；显式写默认端口与不写等价；
5. 删除 fragment；
6. path：保留大小写；百分号解码一次；RFC 3986 点段归一（`/a/./b`→`/a/b`，`/a/../b`→`/b`）；结果长度>1 时去掉末尾 `/`；空 path 视为 `/`；`..` 越界时保留原样不报错；
7. query：按 `&` 分割，每项按首个 `=` 拆为 name/value，name 与 value 各百分号解码一次；
8. 白名单过滤：
   - host == `bilibili.com` 或以 `.bilibili.com` 结尾 → 仅保留 name==`p` 的参数；
   - host == `youtube.com` 或以 `.youtube.com` 结尾 → 仅保留 name==`v` 的参数；
   - 其余域名 → 删除全部参数；
9. 保留的参数按 (name, value) 升序排序（UTF-8 字节序）；
10. 拼接 `url_key = scheme + "://" + host + path + (保留参数非空 ? "?" + "name=value&..." : "")`。

**用例表（单元测试必须逐条断言）**：

| # | 输入 URL | 期望 url_key | 说明 |
|---|---|---|---|
| 1 | `https://example.com/abc?utm_source=x&id=1` | `https://example.com/abc` | 非白名单，参数全删 |
| 2 | `http://example.com/abc?utm_source=y` | `https://example.com/abc` | scheme 归一，与 #1 重复 |
| 3 | `https://www.example.com/abc/` | `https://example.com/abc` | 剥离 www、去尾斜杠 |
| 4 | `https://example.com/ABC` | `https://example.com/ABC` | 路径大小写敏感 |
| 5 | `https://example.com/abc#section` | `https://example.com/abc` | fragment 删除 |
| 6 | `https://example.com:443/abc` | `https://example.com/abc` | 默认端口删除 |
| 7 | `https://bilibili.com/video/BV1xx?p=1&from=search` | `https://bilibili.com/video/BV1xx?p=1` | 白名单只留 p |
| 8 | `https://www.bilibili.com/video/BV1xx?p=1` | `https://bilibili.com/video/BV1xx?p=1` | 与 #7 重复 |
| 9 | `https://bilibili.com/video/BV1xx?p=2` | `https://bilibili.com/video/BV1xx?p=2` | p 值不同 = 不同 |
| 10 | `https://youtube.com/watch?v=abc&list=xyz` | `https://youtube.com/watch?v=abc` | 白名单只留 v |
| 11 | `https://youtube.com/watch?v=abc` | `https://youtube.com/watch?v=abc` | 与 #10 重复 |
| 12 | `https://youtube.com/watch?v=abd` | `https://youtube.com/watch?v=abd` | v 值不同 = 不同 |
| 13 | `https://example.com/a/./b` | `https://example.com/a/b` | 点段归一 |
| 14 | `https://example.com/a/../b` | `https://example.com/b` | 点段归一 |
| 15 | `https://example.com/?` | `https://example.com/` | 空参数视为无参数 |
| 16 | `https://bilibili.com/video/BV1xx?p=1&p=2` | `https://bilibili.com/video/BV1xx?p=1&p=2` | 同名多值按值排序保留 |
| 17 | `javascript:alert(1)` | `javascript:alert(1)` | 非 http(s) 原样（去 fragment） |

### 附录 C：vless:// 链接字段与 xray-core 配置生成

**vless:// 格式**：

```text
vless://<uuid>@<host>:<port>?encryption=none&security=tls&type=ws&path=%2F&host=example.com&sni=example.com&fp=chrome&alpn=http%2F1.1&flow=xtls-rprx-vision&pbk=<publicKey>&sid=<shortId>#<名称>
```

**字段映射（解析表）**：

| URI 参数 | 含义 | xray 配置位置 |
|---|---|---|
| （uuid） | 用户 ID | `outbounds[0].settings.vnext[0].users[0].id` |
| host / port | 服务器 | `settings.vnext[0].address / port` |
| encryption | 加密，固定 `none` | `users[0].encryption` |
| flow | 流控（可空） | `users[0].flow`（空则不写） |
| security | `none`/`tls`/`reality` | `streamSettings.security` |
| type | `tcp`/`ws`/`grpc`/`http` | `streamSettings.network` |
| path | ws/grpc 路径 | `wsSettings.path` / `grpcSettings.serviceName` |
| host | ws/HTTP 伪装的 Host | `wsSettings.headers.Host` / `httpSettings.host` |
| sni | 证书校验名 | `tlsSettings.serverName` / `realitySettings.serverName` |
| fp | TLS 指纹 | `tlsSettings.fingerprint` / `realitySettings.fingerprint` |
| alpn | ALPN | `tlsSettings.alpn`（数组） |
| pbk / sid / spx | Reality 公钥/短 ID/spider | `realitySettings.publicKey / shortId / spiderX` |

**生成模板**（每个节点一份，端口由应用分配）：

```json
{
  "log": { "loglevel": "warning" },
  "inbounds": [{
    "listen": "127.0.0.1",
    "port": 10808,
    "protocol": "socks",
    "settings": { "udp": false }
  }],
  "outbounds": [{
    "protocol": "vless",
    "settings": {
      "vnext": [{
        "address": "HOST",
        "port": 443,
        "users": [{ "id": "UUID", "encryption": "none", "flow": "xtls-rprx-vision" }]
      }]
    },
    "streamSettings": {
      "network": "ws",
      "security": "tls",
      "tlsSettings": { "serverName": "SNI", "fingerprint": "chrome", "alpn": ["http/1.1"] },
      "wsSettings": { "path": "/", "headers": { "Host": "HOST" } }
    }
  }]
}
```

生成规则：未知/缺失参数不写对应 JSON 键；`security=none` 时不写 tlsSettings/realitySettings；参数值必须原样 URL 解码后写入。

### 附录 D：S3 同步实现要点（libcurl AWS SigV4）

```c
curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_AWS_SIGV4);
curl_easy_setopt(curl, CURLOPT_AWS_SIGV4, "aws:amz:<region>:s3");
curl_easy_setopt(curl, CURLOPT_USERPWD, "<access_key>:<secret_key>");
/* path-style: https://<endpoint>/<bucket>/<key> */
```

- endpoint 可自定义（MinIO/COS/OSS/R2）；region 与 path_style 按设置传入；
- 上传用 `PUT`（Content-Length 前置），下载用 `GET`；
- 对象存在性检查用 `HEAD`（404 视为不存在）；
- 所有密钥仅存本地设置，日志脱敏。

### 附录 E：筛选与排序组合示例

| 场景 | 条件 | 期望结果 |
|---|---|---|
| 标签 OR | 标签 {工作, 开发} | 含“工作”或“开发”任一的记录 |
| 分类含子孙 | 分类“技术”（含子类“前端”“后端”） | 技术/前端/后端下的全部记录 |
| 跨维度 AND | 标签{工作} 且 分类“技术” 且 有效性=有效 | 同时满足三者 |
| 网址包含 | 输入 `github.com/foo` | url 含该子串的记录 |
| 域名 | 输入 `bilibili` | host 含 `bilibili` 的记录 |
| 筛后排序 | 上述结果按“状态码 降序” | 状态码从高到低，NULL 最后 |

### 附录 F：常见反爬特征与处理

MVP 阶段仅按 HTTP 状态码判定：`403`、`429` → check_note=`blocked`（待复测），valid 不变。

后续增强（本期不实现，架构需预留 check_note 扩展空间）：响应体前 64KiB 含 `captcha`/`verify`/`security check` 等关键词且状态码为 200/3xx 时也判为 blocked；对单域名连续 N 次 blocked 自动暂停该域名并提示。

“待复测”条目的处理路径：筛选面板“待复测” → 设置代理（FR-17）→ 重新校验（FR-14）→ 判定正常后 valid 恢复。

---

*本文档结束。实现中如遇冲突，以本文档为准；确需变更时先更新本文档再改代码。*
