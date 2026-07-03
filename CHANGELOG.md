# 更新日志

## [2.0.2] - 2026-07-02
### 修复
- 修复 GUI Release 包启动内嵌后端时把工作目录设为编译机源码路径，导致 AppImage 内后端文件存在但仍报 `No such file or directory` 的问题。

## [2.0.1] - 2026-07-02
### 修复
- 发布流程在上传 GUI 附件前会展开 AppImage 并执行内嵌后端 `--version`，避免缺失或不可执行的后端二进制进入 Release。
- GUI 启动内嵌后端失败且文件实际存在时，会提示可能是 ELF 动态加载器、运行时依赖或架构不匹配导致，不再只显示容易误解的 `No such file or directory`。

## [2.0.0] - 2026-07-01
### 新增
- 新增兼容 Ubuntu 20.04 的 Tauri 1.x/React 桌面 GUI，提供 Diagnostics/Logs 双视图、终端风格仪表盘、持续监测、暂停/恢复/停止控制和实时日志。
- 新增 `docs/GUI.md`，说明 GUI 运行方式、二进制查找路径、Linux WebView 依赖和浏览器预览限制。
- GitHub Release 新增 GUI 安装包附件，随 tag 自动发布 Linux x86_64/aarch64 AppImage、deb 和可用时的 rpm。
- monitor summary 新增 SDK 只读详情字段，包括 MID360/MID360S 设备类型、MAC、loader/hardware/firmware、网络端点、状态码、HMS、温度、时间同步、点云/IMU 统计和配置状态。
### 优化
- GUI 复用现有 C++ 诊断二进制，Details 仅展示实际可用数据，隐藏 `N/A`、`unavailable` 和 SDK 哨兵值。
- GUI 安装包内嵌同架构 `livox_mid360_diagnostics` 后端二进制，桌面包优先使用包内资源并仍支持 `LIVOX_MID360_DIAGNOSTICS_BIN` 覆盖。
- 更新 README、忽略规则和 GUI 版本号，为 v2 GUI demo 提供独立启动说明并避免提交前端构建产物。

## [1.3.0] - 2026-06-10
### 新增
- monitor 和 C++/Python autoconfig 支持在未绑定 `192.168.1.x` 时为候选有线网卡临时添加 `192.168.1.5/24`，并提供 `--auto-bind-ip` 和 `--no-auto-bind` 控制。
- 交互式 TUI 启动时会尝试把终端窗口放大到推荐尺寸，并支持 `LIVOX_MID360_TERMINAL_SIZE` 和 `LIVOX_MID360_RESIZE_TERMINAL` 配置。
### 优化
- 抽出 C++ 网络自动绑定 helper，统一网卡枚举、候选排序、临时 IPv4 生命周期和免密 sudo 调用。
- 更新 README 和部署文档，说明默认分支 raw 下载、192.168.1.x 网段绑定和终端尺寸调整方法。
### 修复
- 修复 README 和部署文档中的 GitHub raw URL 写死 `main` 导致 Release 下载脚本 404 的问题。
- 修复 monitor TUI 在窄窗口下 `NETWORK`、`DEVICE IDENTITY` 和 `STREAM STATUS` 面板内容越过边框的问题。
- 修复已连接雷达但有线网卡没有 MID360 默认网段时，monitor 和 autoconfig 无法发现 `192.168.1.x` 雷达的问题。

## [1.2.0] - 2026-06-04
### 新增
- 新增共享 TUI 输入、刷新节流和逐行差分渲染工具，菜单、autoconfig、monitor、dump 和 Python fallback 统一复用。
- autoconfig 和 monitor 在未发现雷达或 SDK 初始化失败时支持从结果页返回主菜单。
### 优化
- 大幅降低主菜单、扫描页、monitor live dashboard 和 dump TUI 的键盘延迟，避免界面刷新被右上角时间或阻塞扫描步骤拖慢。
- 收紧 autoconfig 配置选择页布局，拆分文件名、当前 IP、路径位置和选中详情，footer 固定在终端底部。
- monitor 从 discovery 切换到 live dashboard 时重置渲染状态，避免残留错误文本和旧帧。
- dump 和 UDP monitor 使用独立统计快照与更高频 TUI 刷新，保持速率显示稳定。
- 更新 GitHub Actions checkout/upload/download artifact 运行时，消除 Node.js 20 弃用告警。
### 修复
- 修复 autoconfig 和 monitor 退出行为不一致、无雷达结果页无法回主菜单的问题。
- 修复 monitor SDK discovery 期间误报 `no MID360 lidar found` 的日志。
- 修复 autoconfig 配置选择页快捷键提示重复、位置不固定以及长路径挤压候选列表的问题。

## [1.1.0] - 2026-06-01
### 新增
- 提供单体 C++ 诊断入口，直接运行 `livox_mid360_diagnostics` 后可在菜单中选择 `autoconfig`、`monitor` 和 `dump`。
- 优化交互式 TUI：扫描进度、错误提示、无雷达状态、时间刷新和 Ctrl+C 处理都保留在界面内。
- 接入 tag 触发的 GitHub Release，CI 自动构建 Linux x86_64 和 aarch64 预编译二进制。
- Release 附件同时提供稳定文件名和带版本号文件名，目标设备可直接下载可执行文件运行。
- 移除仓库内提交的预编译产物，改为由 CI 生成和发布。

## [1.0.0] - 2026-05-27
### 新增
- 整理仓库结构，将自有源码收敛到 src/cpp 和 src/python，并保留 external 作为上游 submodule 入口。
- 增加 Python CLI，用于发现 MID360、选择并更新配置文件，以及被动查看 UDP 数据速率。
- 增加 C++ CLI，用于发现 MID360、更新配置文件、查看 SDK 实时仪表盘，以及短时导出点云和 IMU CSV。
- 准备 Linux x86_64 和 aarch64 双架构预编译包，目标设备无需拉取 submodule 或重新编译 Livox-SDK2。
- 接入 VERSION、CHANGELOG 和 GitHub Release 校验，确保发布 tag、版本文件和发布说明对应同一次提交。
