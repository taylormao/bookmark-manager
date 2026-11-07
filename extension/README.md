# 网络收藏夹配套 Chrome 浏览器扩展插件 (Manifest V3)

本目录为配套开发的 Chrome / Edge 浏览器扩展插件，遵循最新的 **Chrome Extension Manifest V3** 规范。

---

## 🌟 核心功能

1. **一键快速收藏**：
   - 在任何网页上点击浏览器右上角的收藏夹扩展图标。
   - 自动提取当前网页的 URL、标题与 Favicon 图标。
   - 支持实时从 Cloudflare Worker 拉取最新分类目录供挑选归类，支持自定义标签。
   - 点击“保存到云端”后即时推送到 Cloudflare 边缘数据库，多端秒级互通。
2. **云端书签检索与打开**：
   - 切换到“云端检索”标签页，无需离开当前页面即可实时搜索和浏览云端收藏的书签。
   - 点击任意书签在浏览器新标签页中秒开。
3. **安全直连与多端闭环**：
   - 直连您的 Cloudflare Worker 服务（支持 Bearer Token 访问密钥鉴权）。
   - 浏览器插件保存的书签，桌面端（Tauri 2.0 客户端）只需一键同步即可直接呈现；同理，桌面端整理的书签也可在浏览器插件中随时调取。

---

## 📦 安装与使用指南 (开发者模式免打包安装)

1. 打开 Chrome、Edge 或任何 Chromium 内核浏览器（如 Brave、Vivaldi 等）。
2. 在地址栏输入并回车：
   - Chrome：`chrome://extensions/`
   - Edge：`edge://extensions/`
3. 开启页面右上角的 **开发者模式 (Developer mode)** 开关。
4. 点击左上角的 **加载已解压的扩展程序 (Load unpacked)** 按钮。
5. 在文件选择框中，选择当前项目的 `extension` 文件夹（即包含 `manifest.json` 的目录）。
6. 点击确认，扩展程序即可成功安装并显示在浏览器工具栏中！

---

## ⚙️ 首次配置与使用

1. 点击浏览器工具栏的拼图图标，将 **网络收藏夹助手** 图标固定到工具栏。
2. 首次点击图标，会弹出配置界面：
   - **Worker 线上地址**：填入部署好的 Cloudflare Worker 域名（如 `https://bookmark-online.my-subdomain.workers.dev`）。
   - **访问密钥 (ACCESS_TOKEN)**：填入 Worker 配置的访问密码。
   - 点击 **测试连接并保存**。
3. 提示连接成功后，即可在任何网页上点击一键收藏！
