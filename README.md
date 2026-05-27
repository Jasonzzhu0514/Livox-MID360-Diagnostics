# Livox MID360 Diagnostics

用于检查 Livox MID360 是否可用：发现网卡上的雷达、修正 `MID360_config.json`，并查看实时数据。

当前目标系统是 Ubuntu/Linux。Windows 原生环境未适配；需要 Windows 时建议使用 Linux 设备或 WSL2 评估网络可用性。

## 选择版本

- Python CLI：发现雷达、更新 JSON、被动查看 UDP 包速率。适合快速配置和轻量检查。
- C++ CLI：发现雷达、更新 JSON、Livox-SDK2 实时回调统计、点云/IMU CSV 导出。适合完整诊断。
- GUI：计划作为 Qt 可视化诊断工具加入；当前还未实现。

官方依赖以 submodule 放在 `external/`，这里放的是上游源码，不是本项目自己的业务代码：

- `external/livox_ros_driver2`
- `external/Livox-SDK2`

本项目不再单独维护顶层 `third_party/`；C++ JSON 解析直接复用 `external/Livox-SDK2/3rdparty/rapidjson`。

## 目录结构

- `src/cpp/`：C++ CLI 源码。
- `src/python/`：Python CLI 源码。
- `scripts/`：构建、打包、清理和发布辅助脚本。
- `docs/`：部署、发现逻辑和安全边界文档。
- `examples/`：示例配置。
- `external/`：上游 submodule 源码。
- `dist/prebuilt/`：可发布的预编译包。

## Python CLI

一键准备：

```bash
./scripts/setup_python.sh
```

接入雷达并确认物理状态健康后：

```bash
livox-mid360-diagnostics autoconfig
livox-mid360-diagnostics udp-monitor
```

含义：

- `autoconfig`：发现雷达，搜索 MID360 配置文件，并用方向键菜单选择要更新的 JSON。
- `udp-monitor`：被动监听配置里的 UDP 端口，查看是否收到数据。

Python 版不会向雷达发送 SDK 控制命令。

## C++ CLI

如果目标设备不想拉 submodule、也不想编译 SDK，可以在同架构 Linux 设备上生成预编译包：

```bash
./scripts/package_cpp_prebuilt.sh
```

生成预编译包的机器需要 submodule；目标设备只需要拿到 `dist/prebuilt/*.tar.gz`，解压后运行：

```bash
source ./env.sh
livox_mid360_diagnostics
```

如果是远程设备，先进入交互式 SSH：

```bash
ssh user@host
cd livox-mid360-diagnostics-prebuilt
source ./env.sh
```

一键准备：

```bash
./scripts/setup_cpp.sh
```

接入雷达并确认物理状态健康后：

```bash
./build/sdk2/livox_mid360_diagnostics autoconfig
./build/sdk2/livox_mid360_diagnostics monitor
./build/sdk2/livox_mid360_diagnostics dump --duration 10 --points "$PWD/mid360_points.csv" --imu "$PWD/mid360_imu.csv"
```

含义：

- 直接运行 `livox_mid360_diagnostics`：显示 `autoconfig`、`monitor`、`dump`、`quit` 四个交互选项。
- `autoconfig`：发现雷达，搜索 MID360 配置文件，并用方向键菜单选择要更新的 JSON。
- `monitor`：通过 Livox-SDK2 显示单屏实时仪表盘，适合长时间观察。
- `dump`：通过 Livox-SDK2 把短时间点云/IMU 样本解析到 CSV。

需要请求雷达进入 normal mode 并开启数据发送时，再显式加控制参数：

```bash
./build/sdk2/livox_mid360_diagnostics monitor --set-normal-mode --enable-point-send --enable-imu
```

短时 CSV 导出也支持相同控制参数：

```bash
./build/sdk2/livox_mid360_diagnostics dump --duration 10 --points "$PWD/mid360_points.csv" --set-normal-mode --enable-point-send --enable-imu
```

## 配置文件

`setup_python.sh` 和 `setup_cpp.sh` 会自动创建：

```text
config/MID360_config.local.json
```

`autoconfig` 会自动搜索当前目录和用户目录下的 MID360 配置文件，使用上下方向键移动，空格选择/取消，回车确认，`q` 或 `Esc` 退出不修改。`monitor` 和 `dump` 会优先使用 `LIVOX_MID360_CONFIG` 指向的配置文件，你通常不需要手动传 `--config`。

如果要手动指定配置：

```bash
export LIVOX_MID360_CONFIG="/absolute/path/to/MID360_config.json"
```

实际接雷达前，请确认 `MID360_config.local.json` 里的 `MID360.host_net_info` 与本机网卡 IP 匹配。

## 实时数据

Python `udp-monitor` 在终端显示 UDP 包速率和点数估计。它只监听本机端口，不控制雷达。

C++ `monitor` 显示表格仪表盘，并在原位置刷新，不会持续刷出新行。`Link` 表示 SDK 回调是否还在更新，断网或拔网口后会变为 `LOST`；`Health` 表示雷达上报的诊断码。

C++ `dump` 输出短时采样 CSV。MID360 点云数据量较大，CSV 是文本格式，不适合长期记录；长期观察请用 `monitor`。

- `mid360_points.csv`：`timestamp_ns, x_m, y_m, z_m, reflectivity, tag` 等字段。
- `mid360_imu.csv`：`timestamp_ns, gyro_x, gyro_y, gyro_z, acc_x, acc_y, acc_z` 等字段。

需要图形化点云时，使用官方 `livox_ros_driver2` + RViz，或等待本项目后续 Qt GUI。

## 离线验证

这些命令不会连接雷达：

```bash
./scripts/check_offline.sh
PYTHONPATH=src/python python3 -m unittest discover -s tests
```

## 清理工作区

清理构建缓存并保留预编译包：

```bash
./scripts/clean_workspace.sh
```

预编译包统一保留在：

```text
dist/prebuilt/
```

## 发布版本

版本号由 `VERSION` 管理。当前版本：

```text
1.0.0
```

本地生成当前架构的 C++ 预编译包：

```bash
./scripts/package_cpp_prebuilt.sh
```

GitHub Release 由 tag 触发。发布 `Livox MID360 Diagnostics v1.0`：

```bash
git tag v1.0.0
git push origin v1.0.0
```

Release workflow 会校验 `VERSION`、tag 和 `CHANGELOG.md` 是否一致，构建 Linux x86_64 预编译包，并上传 `dist/prebuilt/*.tar.gz`。当前 v1.0.0 已准备好两个附件：

```text
livox-mid360-diagnostics-cpp-1.0.0-linux-x86_64.tar.gz
livox-mid360-diagnostics-cpp-1.0.0-linux-aarch64.tar.gz
```

## 安全边界

- 没有接入健康雷达前，不要运行带 `--set-normal-mode --enable-point-send --enable-imu` 的命令。
- Python `udp-monitor` 只监听本机 UDP 端口。
- C++ `monitor` 和 `dump` 会初始化 Livox-SDK2；只有显式加控制参数时才请求雷达切换模式或开启数据发送。
- `external/livox_ros_driver2` 只是源码 submodule，不等于已经构建好的 ROS 工作区。

## 更多文档

- `docs/DEPLOYMENT.md`：远端设备部署。
- `docs/DISCOVERY_AND_CONFIG.md`：发现和 JSON 更新逻辑。
- `docs/SAFETY.md`：命令安全边界。
