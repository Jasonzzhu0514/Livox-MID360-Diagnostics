# 更新日志

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
