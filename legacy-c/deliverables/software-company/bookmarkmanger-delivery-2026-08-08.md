# Bookmark Manager 接手修复交付报告

> 日期：2026-08-08
> 团队：software-bookmarkmanger（主理人齐活林 + 工程师寇豆码 + QA 严过关）
> 结论：**可交付（YES）**，QA 两轮独立回归验证通过

---

## 一、接手诊断

| 检查项 | 修复前状态 |
|---|---|
| 编译（Debug + Release） | ✅ 通过 |
| 单元测试（手动加 PATH） | ✅ 7/7 通过 |
| **ctest 直接运行** | ❌ **6/7 崩溃（0xc0000135，缺 DLL）** |
| CLI 冒烟（10+ 命令） | ✅ 正常 |
| 发布包 dist/（1635 文件） | ✅ 完整 |
| 代码健康度 | ✅ 无 stub/TODO，i18n 齐全 |

**核心 bug 根因**：测试可执行文件链接 msys2 动态库（`libsqlite3-0.dll`/`libcurl-4.dll` 及传递依赖），但 `ctest` 运行时 PATH 不含 `mingw64/bin`。原团队只把 DLL 拷贝逻辑做到了 dist/ 发布包，未固化到测试构建配置。

---

## 二、修复清单（11 项）

### 阻塞级（1 项）
1. **测试运行时 DLL 崩溃**：新增 `cmake/BMKWindowsRuntime.cmake`（多级 fallback 定位 mingw64/bin + 幂等拷贝 22 个运行时 DLL），`tests/CMakeLists.txt` 与 `src/cli/CMakeLists.txt` 接入 → 干净 PATH 下 ctest 开箱 7/7 通过

### 严重（3 项）
2. **PAT 密钥泄漏**（bmk_sync.c）：git 命令输出可能回显带 PAT 的远端 URL 并写入日志/界面 → 新增 redact_set/redact_in_place/redact_clear，所有 git 输出出口脱敏（<8 字符不替换防误伤）
3. **同步静默假成功**（bmk_sync.c）：commit 失败（缺 user.name/email）被忽略，push 报 "Everything up-to-date" 但远端未更新 → commit_is_noop() 区分"无变更"（正常）与真实失败（返回 BMK_ERR_SUBPROCESS 中止）
4. **代理节点池数据竞争**（bmk_proxy.c）：worker 线程遍历 g_nodes 时 UI 线程 remove 会 memmove+free → use-after-free → 全访问点加互斥锁，进程操作移出锁外

### 中等（4 项）
5. **首次云端恢复失败**（bmk_sync.c）：restore 假定 sync_dir 已 clone → 无 .git 时自动 clone，pull 失败不再忽略
6. **favicon 固定 https**（bmk_fetch.c）：纯 http 站点取不到图标 → 按页面实际 scheme
7. **301/308 提示不触发**（bmk_fetch.c）：permanent_redirect 误用最终状态码 → header_cb 捕获首个响应码
8. **VLESS 自动重启未实现**（bmk_proxy.c）：MAX_RESTARTS 定义了未用 → ensure_node_ready() 实现，超 3 次标记不可用

### 轻微（3 项 + 收尾 1 项）
9. **取消时内存泄漏**（bmk_validate.c）：队列残留 url → join 后统一释放
10. **set_tags 不刷新 updated_at**（bmk_db.c）：补 UPDATE（需求 3.3）
11. **stop 后节点被悄悄重启**（bmk_proxy.c）：新增 stopped_explicitly 标记
12. **重启泄漏进程句柄**（bmk_proxy.c）：spawn 前先释放旧句柄
13. **commit_is_noop locale 敏感**（bmk_sync.c）：git_run 固定 LC_ALL=C/LANG=C，中文 git 环境输出恒为英文

### 连带修复
- tests/test_sync.c 与 test_validate.c：remove_tree 在 Windows 删除只读文件失败 → 先 SetFileAttributesA(FILE_ATTRIBUTE_NORMAL)，消除临时目录泄漏
- bmk_proxy_load 顺带修复重复追加问题

---

## 三、验证结果（QA 两轮独立回归）

| 验证项 | 第 1 轮 | 第 2 轮 |
|---|---|---|
| 干净 PATH ctest | ✅ 7/7 | ✅ 7/7 |
| test_sync（带 git） | 23 断言 | **38 断言全过**（+commit 失败 +首次 restore 用例） |
| test_vless | 63 断言 | **71 断言全过**（+stop 不重启用例） |
| 新增测试真实性 | 2 真 1 假（PAT 无测试） | **3/3 真实**（去修复必失败） |
| 修复点审查 | 5 项通过，2 新问题 | 5 项全部闭环 |
| 中文 locale 回归 | — | ✅ 38 断言全过 |

---

## 四、遗留事项（排期项）

| 级别 | 事项 | 建议 |
|---|---|---|
| 交付 | dist/ 发布包为旧代码编译 | 重建发布包以包含本次修复 |
| 中 | PAT 持久化进 .git/config | 改用 credential helper 或推送后重置 remote URL |
| 中 | 备份保留策略按文件数计 20 份 | 按时间戳归组计数 |
| 低 | popen 命令行引号未转义 | 参数受控，低危；如传含引号参数需处理 |
| 低 | slot_endpoint 用户密码未 URL 编码 | 含特殊字符时代理串非法 |
| 低 | bmk_validate.c BOM+GBK 乱码注释 | 转存 UTF-8 |
| 低 | FR-04 默认图标为内嵌 1×1 PNG | 需求要求 data/default_icon.png 64×64 |
| 工程 | 项目无 git 仓库 | git init 追踪变更 |

---

## 五、2026-11-07 更新（构建恢复 + 需求补齐 + 代理功能）

**构建环境**：MSYS2 MinGW 开发包链丢失的问题已通过代码级方案绕开——SQLite3/libcurl/GTK4
在无 `.pc` 文件时走 `find_path`/`find_library` 回退（支持 `-D<DEP>_ROOT`），cJSON v1.7.19
vendored 进 `third_party/cJSON/`，运行时 DLL 自动拷贝已兼容 scoop 命名（`libcurl-x64.dll`）。
**完整 `cmake --build` + `ctest` 已恢复并可复现**（命令见 README 构建章节），全程不改动 scoop 环境。

**测试规模**：8 → 11 个 CTest 目标，当前 10/11 通过；唯一失败的 `gui-selftest` 是
GTK 4.18 运行时 gstreamer 依赖链的环境待办（退出码 0xc0000139），非代码问题。

**遗留事项闭环状态**：

| 遗留项 | 状态 |
|---|---|
| 发布包为旧代码编译 | ✅ 本次重建（见下） |
| popen 命令行转义 | ✅ 已修（先校验后引用，拒绝命令元字符） |
| 代理串凭据未 URL 编码 | ✅ 已修（`curl_easy_escape` + `CURLOPT_PROXYUSERPWD`） |
| FR-04 默认图标 1×1 | ✅ 已修（64×64 `data/default_icon.png` + 内嵌数组同步） |
| 备份保留策略按时间戳归组 | ✅ 此前已修 |
| PAT 持久化进 .git/config | ⬜ 未动（推送后重置 remote 的方案已实现，彻底改 credential helper 待排期） |

**新增功能（FR-17 补充条款，需求文档已同步）**：

- 设置页可直接填协议（HTTP/SOCKS5）/地址/端口/凭据并当场分层校验
  （TCP → 协议握手 → 可选目标站，407/403/429 不再误判为可用）；
- 「保存并启用」写入节点并把批量校验代理模式置为「全部走代理」，
  修复了旧设置键 `check_use_proxy` 对批量校验完全无效的断链；
- CLI 新增 `proxy add --type`、`proxy check`、`proxy test --url`。

**GUI 强制项补齐**：主题切换（FR-01）、图标服务开关在 GUI 抓取路径生效（FR-03）、
301/308「是否使用最终网址」（FR-03）、输入 URL 自动抓取 + 标题 host 兜底（FR-02）、
导出默认名带时间戳（FR-08）。**UI 代码已过完整编译**（`bmk-gui.exe` 构建成功）。

**§7.2 集成测试补齐**：5xx 最终码、两跳重定向链、真实 TLS 失败、git 分叉备份 +
force-with-lease（`tests/test_validate.c` / `tests/test_sync.c`）；
§5.1 性能基准自动化（`tests/test_perf.c`，10k 条排序筛选 < 200ms、导入 < 10s 均达标）。

**仍待外部资源**：MinIO 实例与 GitHub 测试仓库的 P3 真实推拉记录；
gui-selftest 运行环境；64 条 UI 手工清单执行（清单文档已标注自动化基线与覆盖重叠项）。

---

*文档结束。当前测试基线：ctest 10/11（gui-selftest 环境待办），新增 test_proxy_http（60 断言）、
test_perf（10k 基准）、test_sync 分叉用例（60+ 断言）、test_validate 校验矩阵（12 场景 + TLS）。*
