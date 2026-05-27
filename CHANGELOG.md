# 更新日志

## [1.0.0] - 2026-05-27

### 新增

- 整理仓库结构，将自有源码收敛到 src/cpp 和 src/python，并保留 external 作为上游 submodule 入口。
- 增加 Python CLI，用于发现 MID360、选择并更新配置文件，以及被动查看 UDP 数据速率。
- 增加 C++ CLI，用于发现 MID360、更新配置文件、查看 SDK 实时仪表盘，以及短时导出点云和 IMU CSV。
- 准备 Linux x86_64 和 aarch64 双架构预编译包，目标设备无需拉取 submodule 或重新编译 Livox-SDK2。
- 接入 VERSION、CHANGELOG 和 GitHub Release 校验，确保发布 tag、版本文件和发布说明对应同一次提交。
