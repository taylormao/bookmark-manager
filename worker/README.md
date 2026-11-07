# 🌐 Cloudflare Workers 独立在线网络收藏夹与云同步服务

本模块是一个基于 **Cloudflare Workers + Cloudflare D1（全球边缘 Serverless SQLite）** 构建的轻量级、高可用云端系统。它具备两大核心定位：
1. **独立在线网络收藏夹 (Standalone Web App)**：
   - 无需运行任何桌面软件，直接在任何电脑或手机浏览器中打开 Worker 域名，即可享有现代、自适应、极速的个人在线书签网站。
   - 支持按分类树和常用标签筛选、实时标题与网址模糊检索、在线一键外链打开。
   - 网页端支持直接添加书签（Worker 边缘自动并发抓取网页标题和 Favicon 图标）。
   - 内置安全密码锁屏与认证机制（支持自定义 `ACCESS_TOKEN` / Cookie 记忆登录）。
2. **多端双向增量同步枢纽 (Multi-client Sync Hub)**：
   - 为 **Tauri 桌面客户端** 提供毫秒级增量双向同步，冲突按时间戳最新胜出原则自动合并。
   - 为 **Chrome 浏览器扩展插件** 提供一键收集并推送到云端的 REST API。

---

## 📑 目录
- [准备工作：获取 Cloudflare 鉴权信息](#-准备工作获取-cloudflare-鉴权信息)
- [方式一：通过 GitHub Actions 全自动持续部署 (推荐)](#-方式一通过-github-actions-全自动持续部署-推荐)
- [方式二：通过本地命令行一键部署 (Wrangler CLI)](#-方式二通过本地命令行一键部署-wrangler-cli)
- [三端联动配置教程](#-三端联动配置教程)
- [REST API 接口规范](#-rest-api-接口规范)
- [常见问题与排查指南 (FAQ)](#-常见问题与排查指南-faq)

---

## 🔑 准备工作：获取 Cloudflare 鉴权信息

在部署前，您需要从 Cloudflare 控制台准备以下三个基础信息：

### 1. 获取 Account ID (账户 ID)
1. 登录 [Cloudflare 控制台](https://dash.cloudflare.com/)。
2. 在任意域名的概览页右下角，或在左侧导航栏点击 **Workers & Pages** -> **Overview**。
3. 在页面右侧侧边栏中找到 **Account ID (账户 ID)**，点击复制备用。

### 2. 创建 API Token (具备 Workers 与 D1 权限)
1. 点击 Cloudflare 控制台右上角头像 -> **My Profile (个人资料)** -> **API Tokens**。
2. 点击 **Create Token (创建令牌)**。
3. 找到 **Edit Cloudflare Workers** 模板，点击 **Use template**。
4. 在 Permissions (权限) 列表中，确认包含以下权限（若未包含可点击 Add more 添加）：
   - `Account` - `Workers Scripts` - `Edit`
   - `Account` - `Workers D1` - `Edit`
   - `Account` - `Workers KV Storage` - `Edit` (可选)
5. 在 Account Resources (账户资源) 中选择 **All accounts**（或您的具体账户）。
6. 点击 **Continue to summary** -> **Create Token**。
7. **复制生成的 API Token 并妥善保存**（该 Token 只会展示一次）。

### 3. 创建 D1 数据库
1. 在 Cloudflare 控制台左侧菜单点击 **Workers & Pages** -> **D1**。
2. 点击 **Create database (创建数据库)**，数据库名称填写：`bookmarks-db`。
3. 点击 **Create** 保存。
4. 在数据库详情页中，复制显示的 **Database ID**（形如 `xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx`）。
5. 打开本项目中的 [`worker/wrangler.jsonc`](wrangler.jsonc) 文件，将 `"database_id"` 对应的值替换为该真实的 Database ID。

---

## 🚀 方式一：通过 GitHub Actions 全自动持续部署 (推荐)

仓库已内置 [`.github/workflows/deploy-worker.yml`](../.github/workflows/deploy-worker.yml) 自动化流水线。只需在 GitHub 仓库中配置好 Secrets，代码推送到 GitHub 时即可自动完成构建、建表迁移并部署上线！

### 步骤 1：在 GitHub 仓库添加 Secrets
1. 打开您的 GitHub 仓库页面（例如 `https://github.com/your-username/bookmark-manager`）。
2. 点击仓库顶部的 **Settings (设置)** -> 左侧菜单选择 **Secrets and variables** -> **Actions**。
3. 点击 **New repository secret** 按钮添加以下两个密钥：
   - 密钥名：`CLOUDFLARE_API_TOKEN`，内容：填入上一步获取的 **API Token**。
   - 密钥名：`CLOUDFLARE_ACCOUNT_ID`，内容：填入上一步获取的 **Account ID**。

### 步骤 2：自动触发部署
- **自动触发**：只要对仓库主分支（`main`）提交推送包含 `worker/**` 目录的代码，GitHub Actions 会自动触发部署。
- **手动触发**：进入 GitHub 仓库页面 -> 点击 **Actions** 标签页 -> 选择 **Deploy Cloudflare Worker Online Bookmark Manager** -> 点击右侧 **Run workflow** 即可随时手动触发。

部署成功后，GitHub Actions 日志会输出您的线上 Worker 网址（形如：`https://bookmark-online.<your-subdomain>.workers.dev`）。

### 步骤 3：配置在线访问安全密码 (ACCESS_TOKEN)
为了防止他人未授权访问您的私人收藏夹，建议配置访问密钥：
1. 在 Cloudflare 控制台左侧进入 **Workers & Pages**，点击刚刚部署好的 `bookmark-online` Worker。
2. 点击 **Settings (设置)** -> **Variables and Secrets (变量与机密)**。
3. 在 **Secrets** 区域点击 **Add**：
   - 变量名：`ACCESS_TOKEN`
   - 变量值：设置一个您自己专属的高强度密码（如 `MyBookmarkPass@2026`）。
4. 点击 **Deploy / Save** 保存。

---

## 💻 方式二：通过本地命令行一键部署 (Wrangler CLI)

如果您习惯使用本地终端，也可以使用 Wrangler CLI 进行本地预览和手动部署：

```bash
# 1. 进入 worker 目录并安装依赖
cd worker
npm install

# 2. 登录 Cloudflare 账号
npx wrangler login

# 3. 创建远程 D1 数据库（如尚未在控制台创建）
npx wrangler d1 create bookmarks-db
# 执行后终端会打印 database_id，将其填入 worker/wrangler.jsonc 中

# 4. 初始化远程数据库数据表结构
npm run db:init:remote

# 5. 设置访问密码 Secret
npx wrangler secret put ACCESS_TOKEN
# 提示输入密码时，输入您自定义的访问 Token 并回车

# 6. 本地开发预览（使用本地沙箱测试）
npm run dev

# 7. 正式一键部署到 Cloudflare 全球边缘节点
npm run deploy
```

---

## 📱 三端联动配置教程

当您的 Cloudflare Worker 部署完成后，便可轻松实现 Web 端、桌面端与 Chrome 插件三位一体：

### 1. 独立 Web 端使用
- 直接在浏览器访问您的 Worker 网址：`https://bookmark-online.<subdomain>.workers.dev`。
- 若设置了 `ACCESS_TOKEN`，系统会展示优雅的锁屏登录卡片，输入您的 Token 即可进入主界面。
- 支持实时检索、分类筛选、卡片浏览以及在线添加新书签（自动从边缘抓取网页标题和图标）。

### 2. 桌面客户端（Tauri 2.0）一键同步
1. 启动桌面端书签管理器。
2. 点击右上角 **云同步** 按钮打开配置弹窗。
3. 填入：
   - **Worker 线上地址**：`https://bookmark-online.<subdomain>.workers.dev`
   - **访问密钥 (ACCESS_TOKEN)**：填入您在 Worker 中配置的 Token。
   - 勾选 **启动软件时自动静默双向同步**（可选）。
4. 点击 **立即双向同步**，客户端将立即完成与云端的原子级增量数据合并！

### 3. Chrome 浏览器扩展插件使用
1. 打开 Chrome 扩展页面 `chrome://extensions/`，开启右上角“开发者模式”。
2. 点击“加载已解压的扩展程序”，选择项目根目录下的 `extension` 文件夹。
3. 点击扩展图标 -> 设置按钮 ⚙️：填入 Worker 线上地址与 ACCESS_TOKEN，点击“测试连接并保存”。
4. 在任意网页上点击插件图标，即可一键将该网页收藏并归类至云端！

---

## 🔌 REST API 接口规范

| 接口端点 | 请求方式 | 认证要求 | 说明 |
| :--- | :--- | :--- | :--- |
| `/` | `GET` | Cookie / Token / 公开 | 独立 Web 端在线网络收藏夹界面 |
| `/api/auth/login` | `POST` | 无 | 网页端登录接口，校验 Token 并写入安全 Cookie |
| `/api/status` | `GET` | 无 | 服务端健康检查与书签、分类实时总数统计 |
| `/api/categories` | `GET` | Bearer Token | 获取全部未删除分类树 |
| `/api/categories` | `POST` | Bearer Token | 创建新分类（接收 `{ name, parent_id, sort_order }`） |
| `/api/bookmarks` | `GET` | Bearer Token | 检索与过滤书签列表（支持 `search`, `category_id` 等参数） |
| `/api/bookmarks` | `POST` | Bearer Token | 新增书签（支持自动抓取标题与 Favicon） |
| `/api/bookmarks/:id` | `DELETE` | Bearer Token | 软删除指定书签（`is_deleted = 1`） |
| `/api/sync` | `GET` | Bearer Token | 增量拉取接口：`?since=<timestamp>`，拉取该时间戳后的所有变更 |
| `/api/sync` | `POST` | Bearer Token | 增量推送与合并接口：原子事务合并客户端数据并返回最新服务器时间戳 |

---

## ❓ 常见问题与排查指南 (FAQ)

### Q1: 部署后打开页面显示 401 Unauthorized 或密码无效？
- 请检查在 Cloudflare Worker 的 Variables and Secrets 中配置的 `ACCESS_TOKEN` 是否与输入的完全一致（注意区分大小写，避免多余空格）。
- 若希望公开只读访问（无需密码即可浏览，仅编辑需密码），可在 `wrangler.jsonc` 中将 `"ALLOW_PUBLIC_READ"` 设置为 `"true"`。

### Q2: GitHub Actions 部署报错 `Error: Unknown or missing D1 database`？
- 请确保在 Cloudflare 控制台创建的 D1 数据库名称为 `bookmarks-db`。
- 请检查 [`worker/wrangler.jsonc`](wrangler.jsonc) 中的 `database_id` 是否已替换为您在控制台创建的具体 ID。

### Q3: 域名被阻断或访问慢，如何绑定自定义域名？
- Cloudflare Workers 免费支持绑定自己的个性化域名：
  1. 在 Cloudflare 控制台进入您的域名（如 `example.com`）。
  2. 在 Worker 详情页点击 **Settings** -> **Domains & Routes** -> **Add Custom Domain**。
  3. 输入您想使用的子域名（如 `bookmarks.example.com`），Cloudflare 会自动完成全球 Anycast CDN 解析与免费 SSL 证书下发。

---
*文档更新于 2026 年，适配 Cloudflare Workers v4 及 D1 SQLite 引擎。*
