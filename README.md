# Bookmark Manager（本地网址收藏夹管理工具）

一个基于 **C11 + SQLite3** 的本地优先桌面书签管理工具，提供 GTK4 图形界面与命令行工具两套入口。
数据默认全部存于本机，不依赖任何账户体系。

> 详细需求见 [`bookmark-manager-requirements.md`](bookmark-manager-requirements.md)。

## 功能特性

- **本地数据**：SQLite（WAL + 外键），多级分类树、标签、图标、校验结果一应俱全。
- **URL 归一化**：`http→https`、去 `www`、去跟踪参数、点段规整等（附录 B 17 条用例全通过）。
- **导入/导出**：标准 Netscape HTML，与 Chrome / Firefox / Edge 互通，支持 UTF-8 与 GBK。
- **查重与合并**：按归一化 `url_key` 自动去重，可一键合并（最早记录保留、标签取并集）。
- **批量校验**：并发抓取判定有效性，403/429 视为反爬待复测（附录 F），不翻转原状态。
- **代理**：解析 `vless://` 链接，拉起 xray-core 子进程（仅绑定 127.0.0.1），多种轮换策略。
- **云端同步**：GitHub（git CLI）与 S3（libcurl SigV4）双通道，密钥永不落日志、不导出。
- **备份**：删除/合并/同步前自动生成 HTML 快照，保留最近 20 份全量备份。

## 技术栈

| 组件 | 说明 |
|---|---|
| 语言 | C11 |
| 构建 | CMake ≥ 3.20（生成器按平台自选：Ninja / MinGW Makefiles） |
| 数据库 | SQLite3 ≥ 3.35（WAL） |
| 网络 | libcurl ≥ 7.85 |
| HTML 解析 | Gumbo-parser（标题/图标抓取） |
| JSON | cJSON |
| 界面 | GTK4（可选） |
| 代理 | xray-core 子进程（可选） |

## 目录结构

```
bookmark-manager/
├── CMakeLists.txt
├── third_party/          # cJSON / gumbo（本地优先，回退 pkg-config）
├── src/
│   ├── core/             # libbookmark_core（纯 C，不依赖 GTK）
│   │   ├── bmk_url.c     # URL 归一化（附录 B）
│   │   ├── bmk_db.c      # SQLite 数据层
│   │   ├── bmk_netscape.c# HTML 导入导出（附录 A）
│   │   ├── bmk_fetch.c   # 标题/图标抓取
│   │   ├── bmk_validate.c# 批量校验
│   │   ├── bmk_proxy.c   # vless 解析 + xray 配置（附录 C）
│   │   ├── bmk_sync.c    # GitHub / S3 同步（附录 D）
│   │   └── ...           # 日志、设置、备份、子进程、装配层
│   ├── cli/              # bmk-cli 命令行工具
│   └── ui/               # GTK4 图形界面
├── tests/                # CTest 单元测试
└── data/                 # 默认图标、示例书签（mock server 内嵌于 tests/test_validate.c）
```

## 构建

前置依赖（只需头文件与链接库；pkg-config 的 `.pc` 文件是可选加速路径）：

- SQLite3 ≥ 3.35
- libcurl ≥ 7.85
- cJSON：**已内置**于 `third_party/cJSON/`，无需系统包或 `.pc`
- Gumbo-parser（可选，缺失时关闭标题/图标抓取）
- GTK4（可选，缺失时跳过 `src/ui`）
- Windows 另需 `git` CLI 与运行时 DLL（见 `cmake/BMKWindowsRuntime.cmake`）

常规构建（生成器按平台自选，不必固定 `-G Ninja`）：

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Windows / scoop 环境下已验证通过的一套完整命令（**不装任何新包、不依赖
scoop 的失效 `current` junction**）：

```bash
cmake -S . -B build -G "MinGW Makefiles" \
  -DCMAKE_C_COMPILER=<mingw>/bin/gcc.exe \
  -DCMAKE_MAKE_PROGRAM=<mingw>/bin/mingw32-make.exe \
  -DSQLITE3_ROOT=<mingw>/opt \
  -DCURL_ROOT=<scoop-curl> \
  -DBMK_MINGW_BIN=<scoop-curl>/bin \
  -DBMK_BUILD_UI=OFF
cmake --build build -j 4
ctest --test-dir build --output-on-failure
```

要点：

- SQLite3 / libcurl 优先走 pkg-config；找不到时按 `-D<DEP>_ROOT` 或
  `CMAKE_PREFIX_PATH` 用 `find_path` / `find_library` 回退，也可用
  `-D<DEP>_INCLUDE_DIR` / `-D<DEP>_LIBRARY` 直接指定。
- `BMK_MINGW_BIN` 指向含运行时 DLL 的目录（`libcurl-4.dll` 或 scoop 的
  `libcurl-x64.dll`），构建时自动拷到产物旁，`ctest` 无需手工设置 PATH。
- 若 `git` 的 scoop shim 失效，运行 `ctest` 前把真实 `git.exe` 所在目录加入 PATH，
  否则 `test_sync` 会失败（不是代码问题）。
- 升级工具链后旧 `build/` 缓存里的绝对路径会失效，请清空构建目录重新 configure。

> 界面（`src/ui`）仅在检测到 GTK4 时构建；即使没有 GTK4，`libbookmark_core` 与 CLI 也能正常编译与测试。

## 命令行示例

```bash
# 归一化测试（附录 B）
bmk-cli normalize "https://www.example.com/abc?utm_source=x&id=1"

# 导入浏览器导出的书签
bmk-cli import ~/Downloads/bookmarks.html --encoding auto

# 导出全部
bmk-cli export ~/backup.html

# 查重并合并
bmk-cli dedupe
bmk-cli dedupe --merge

# 批量校验
bmk-cli check --all

# 添加代理节点
bmk-cli proxy add 'vless://uuid@host:443?security=tls&type=ws&path=%2F#节点名'

# 云端同步
bmk-cli sync github
bmk-cli sync s3
```

## 数据目录

默认数据目录：

- Windows：`%APPDATA%\BookmarkManager`
- Linux：`~/.local/share/BookmarkManager`

```
<data_dir>/
├── bookmarks.db      # 主数据库（WAL）
├── backups/          # 各类备份
├── logs/             # 滚动日志（5MB × 3）
└── sync/             # 同步工作区
```

## 测试

`tests/` 为 CTest + 轻量断言宏（不引入大型框架），共 8 个测试，GTK4 可用时另注册 1 个 GUI 自测：

| 测试 | 覆盖 |
|---|---|
| `test_url` | 附录 B URL 归一化 17 条用例 + 边界 |
| `test_db` | 分类递归删除/计数、标签大小写合并、排序（FR-09）、筛选（FR-12） |
| `test_netscape` | 层级还原、实体转义、往返一致性、取消回滚（FR-07） |
| `test_merge` | 合并规则（FR-16） |
| `test_validate` | 批量校验判定（FR-14），内嵌本地 HTTP mock server |
| `test_vless` | vless 解析与 xray 配置生成（附录 C） |
| `test_sync` | GitHub 同步（需要 `git` CLI） |
| `test_backup` | 备份保留策略（FR-06/16/21） |
| `gui-selftest` | GTK4 界面 15 步自动化自测（仅 Windows + GTK4 可用时） |

当环境不满足（缺 `git`、端口被占用等）导致用例被跳过时，测试返回 `77`，CTest 会将其标记为 **Skipped** 而不是计入通过，避免零断言假绿。

## 许可

参考 `bookmark-manager-requirements.md` 中的约束与验收标准实现。
