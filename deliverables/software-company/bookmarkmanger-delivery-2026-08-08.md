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

*文档结束。修复后测试基线：编译 0 错误、ctest 7/7、test_sync 38 断言、test_vless 71 断言。*
