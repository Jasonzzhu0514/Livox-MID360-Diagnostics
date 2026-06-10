# 更新日志

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
