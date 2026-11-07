# Cloudflare Worker 在线网络收藏夹与云同步服务端

本模块基于 **Cloudflare Workers + Cloudflare D1（Serverless SQLite）** 构建，具备两大核心能力：
1. **独立在线网络收藏夹 (Standalone Web App)**：直接在浏览器访问 Worker 域名，即可获得一个美观、现代、极速的在线书签管理系统（支持搜索、分类筛选、网页标题/Favicon 自动抓取与密码登录）。
2. **多端双向增量同步服务端 (Sync Engine)**：为 Tauri 桌面客户端和 Chrome 浏览器插件提供增量同步与一键收藏 API。

---

## 🚀 方式一：GitHub 自动化一键部署 (推荐)

仓库已内置 GitHub Actions 工作流（`.github/workflows/deploy-worker.yml`），只需在 GitHub 仓库设置中配置两个 Secrets，推送到 GitHub 后将自动完成部署与数据库迁移：

### 1. 获取 Cloudflare 凭证
1. 登录 [Cloudflare Dashboard](https://dash.cloudflare.com/)。
2. 点击右上角头像 -> **My Profile (个人资料)** -> **API Tokens**。
3. 点击 **Create Token**，选择 **Edit Cloudflare Workers** 模板（包含 Workers 脚本和 D1 权限），生成并复制 Token。
4. 在 Cloudflare 概览页右侧获取您的 **Account ID (账户 ID)**。

### 2. 在 GitHub 仓库中添加 Secrets
进入您的 GitHub 仓库：
- **Settings** -> **Secrets and variables** -> **Actions** -> **New repository secret**：
  - `CLOUDFLARE_API_TOKEN`: 填入刚才创建的 API Token。
  - `CLOUDFLARE_ACCOUNT_ID`: 填入您的 Cloudflare Account ID。

### 3. 创建 Cloudflare D1 数据库
在 Cloudflare Dashboard 左侧菜单 -> **Workers & Pages** -> **D1**：
- 点击 **Create database**，名称命名为 `bookmarks-db`。
- 创建完成后复制生成的 **Database ID**。
- 将 `worker/wrangler.jsonc` 中的 `"database_id"` 替换为您真实的 Database ID。

### 4. 设置访问密钥 (可选但强烈建议)
在 Cloudflare Dashboard 中打开该 Worker -> **Settings** -> **Variables and Secrets**：
- 添加 Secret：变量名为 `ACCESS_TOKEN`，值设为您自定义的强密码（如 `my-super-secret-sync-token`）。
- 该 Token 将同时用于网页端登录、桌面客户端同步和 Chrome 插件授权。

### 5. 自动部署
推送代码到 GitHub 仓库主分支（或在 GitHub 页面 **Actions** 中手动点击 **Run workflow**），即自动完成构建、D1 建表迁移并上线！

---

## 💻 方式二：本地命令行部署 (Wrangler CLI)

```bash
cd worker
npm install

# 登录 Cloudflare
npx wrangler login

# 创建远程 D1 数据库
npx wrangler d1 create bookmarks-db
# 将终端输出的 database_id 填入 wrangler.jsonc

# 初始化远程数据表
npm run db:init:remote

# 配置私有访问密钥 (Token)
npx wrangler secret put ACCESS_TOKEN

# 部署上线
npm run deploy
```

部署完成后，Cloudflare 会分配一个形如 `https://bookmark-online.<your-subdomain>.workers.dev` 的线上访问地址。

---

## 🔌 API 接口概览

| 接口 | 方法 | 描述 |
| :--- | :--- | :--- |
| `/` | `GET` | 独立 Web 端网络收藏夹界面 |
| `/api/auth/login` | `POST` | 网页端 Access Token 登录 |
| `/api/status` | `GET` | 服务端健康检查与书签/分类计数 |
| `/api/categories` | `GET / POST` | 获取 / 新建分类 |
| `/api/bookmarks` | `GET / POST` | 检索书签 / 添加书签（支持自动抓取标题与图标） |
| `/api/bookmarks/:id` | `DELETE` | 软删除书签 |
| `/api/sync` | `GET` | 增量拉取（`?since=<timestamp>`） |
| `/api/sync` | `POST` | 桌面客户端双向增量合并与推送 |

