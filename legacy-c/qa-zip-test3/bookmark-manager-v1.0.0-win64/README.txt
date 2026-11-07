网址收藏夹管理工具 (Bookmark Manager) v1.0.0
Windows x64 便携版（离线优先，数据保存在本地 SQLite）

【运行】
  界面版：双击 bmk-gui.exe
  命令行版：bmk-cli.exe（支持 add / list / delete / normalize / dedupe / sync 等，运行 bmk-cli.exe --help 查看）
  自定义数据目录：bmk-gui.exe --data-dir <目录> / bmk-cli.exe --data-dir <目录>

【功能】导入/导出 Netscape HTML 书签、添加/编辑/删除、多级分类、标签、排序筛选、
  批量校验（HTTP 状态码/待复测）、URL 归一化查重与合并、GitHub/S3 云同步、VLESS 代理轮换。

【VLESS 代理】
  已内置 xray-core 26.6.1（xray.exe，应用会优先从本目录加载，无需额外安装）。
  如需覆盖，可在“设置 → 代理”中指定其他 xray 可执行文件路径。
  说明：geoip.dat / geosite.dat 等地理数据文件未内置；应用生成的配置不依赖它们，
  若自行扩展路由规则需要，可把对应 dat 文件放到本目录。

【云同步】
  需要 git（>=2.30）与 GitHub token 或 S3/MinIO/OSS/R2 凭据，在“设置 → 同步”中配置。
  git 不在 PATH 时可在同步设置中指定 git 路径。

【依赖说明】本目录已内置 GTK4 / SQLite / libcurl / cJSON 等运行时，无需安装 MSYS2。
  仅支持 Windows 10/11 x64。杀毒软件可能对未签名 exe 提示，请选择“仍要运行”。

【文档】docs/bookmark-manager-requirements.md（完整需求说明）、docs/README.md（工程说明）

【构建】如需自行构建：CMake >= 3.20，-DBMK_BUILD_TESTS=ON 可跑测试（当前 8/8 通过）。
