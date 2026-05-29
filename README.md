# Livox MID360 Diagnostics

用于检查 Livox MID360 是否可用：发现网卡上的雷达、修正 `MID360_config.json`，并查看实时 SDK 数据状态。

当前目标系统是 Ubuntu/Linux。Windows 原生环境未适配；需要 Windows 时建议使用 Linux 设备或 WSL2 评估网络可用性。

## C++ CLI

C++ CLI 是推荐主路径。它直接走 Livox-SDK2 discovery，不要求提前准备 `config/MID360_config.local.json`。

### 准备

普通使用优先下载 GitHub Release 里的预编译二进制：

- 最新版本：<https://github.com/Jasonzzhu0514/Livox-MID360-Diagnostics/releases/latest>

目标设备上可以自动下载当前架构的单体程序并运行：

```bash
bash <(curl -fsSL https://raw.githubusercontent.com/Jasonzzhu0514/Livox-MID360-Diagnostics/main/scripts/use_cpp_release.sh)
./livox_mid360_diagnostics
```

如果手动下载 Release 附件，选择当前架构的 `livox_mid360_diagnostics-linux-*`，然后执行：

```bash
mv ./livox_mid360_diagnostics-linux-* ./livox_mid360_diagnostics
chmod +x ./livox_mid360_diagnostics
./livox_mid360_diagnostics
```

这个文件就是完整 C++ 诊断入口。后续继续执行 `./livox_mid360_diagnostics` 即可。

如果已经 clone 了本仓库，也可以运行：

```bash
bash scripts/use_cpp_release.sh
./livox_mid360_diagnostics
```

需要从源码构建时再运行：

```bash
./scripts/setup_cpp.sh
```

如果要自己生成同架构预编译包：

```bash
./scripts/package_cpp_prebuilt.sh
```

### 常规诊断

```bash
./livox_mid360_diagnostics
```

启动后在菜单里选择：

- `autoconfig`：发现雷达，搜索 MID360 配置文件，并选择要更新的 JSON。
- `monitor`：通过 Livox-SDK2 发现雷达，显示实时点云/IMU 回调状态，适合常规验证和长期观察。
- `dump`：短时抓取点云/IMU 到 CSV，适合留样或离线分析。

如果是源码构建，使用 `./build/sdk2/livox_mid360_diagnostics`。

需要脚本化运行或指定参数时，才直接加子命令。例如多网卡环境下指定扫描网卡：

```bash
./livox_mid360_diagnostics monitor --iface eth0
```

需要请求雷达进入 normal mode 并开启点云/IMU 发送时，确认设备连接正确且物理状态健康后再运行：

```bash
./livox_mid360_diagnostics monitor --set-normal-mode --enable-point-send --enable-imu
```

### 可选功能

`dump` 可从菜单进入，也可以用命令行直接指定 CSV 输出。它需要 `MID360_config.json`；CSV 不适合长期记录，常规验证请用 `monitor`。

```bash
./livox_mid360_diagnostics dump --duration 10 --points "$PWD/mid360_points.csv" --imu "$PWD/mid360_imu.csv"
```

输出字段：

- `mid360_points.csv`：`timestamp_ns, offset_s, x_m, y_m, z_m, reflectivity, tag` 等字段。
- `mid360_imu.csv`：`timestamp_ns, offset_s, gyro_x, gyro_y, gyro_z, acc_x, acc_y, acc_z` 等字段。

需要图形化查看点云时，使用 `livox_ros_driver2` + RViz。`external/livox_ros_driver2` 只是源码 submodule，不等于已经构建好的 ROS 工作区。

## Python CLI

Python CLI 适合轻量配置和低层 UDP 端口检查。它不走 Livox-SDK2 callback，不向雷达发送 SDK 控制命令。

### 准备

```bash
./scripts/setup_python.sh
```

### 可用命令

```bash
livox-mid360-diagnostics autoconfig
livox-mid360-diagnostics udp-monitor
```

- Python `autoconfig`：发现雷达并更新配置文件。非交互终端不会自动写文件，脚本化写入需要显式使用 `--config PATH --apply --yes`。
- Python `udp-monitor`：只监听本机 UDP 端口是否收到包，不初始化 SDK，也不控制雷达。它需要从配置文件读取 `host_net_info` 端口。

## 配置文件

主诊断流程不需要提前创建 `config/MID360_config.local.json`：

- `autoconfig` 会先发现已连接雷达，再搜索和更新配置文件。
- `monitor` 会根据本机网卡临时生成 SDK discovery 配置。

只有这些场景需要配置文件：

- 需要保存给 `livox_ros_driver2` 使用的 `MID360_config.json`。
- 需要运行 C++ `dump`。
- 需要运行 Python `udp-monitor`。
- 需要显式指定 SDK 初始化配置。

手动指定配置：

```bash
export LIVOX_MID360_CONFIG="/absolute/path/to/MID360_config.json"
```

生成本地配置模板：

```bash
source scripts/prepare_config.sh
```

如果配置文件要给 ROS driver、`dump` 或 `udp-monitor` 使用，请确认 `MID360.host_net_info` 与本机网卡 IP 匹配。

## 远程部署

远程设备建议进入交互式 SSH 后加载最新 C++ Release：

```bash
ssh user@host
bash <(curl -fsSL https://raw.githubusercontent.com/Jasonzzhu0514/Livox-MID360-Diagnostics/main/scripts/use_cpp_release.sh)
./livox_mid360_diagnostics
```

## 安全边界

- 没有接入健康雷达前，不要运行带 `--set-normal-mode --enable-point-send --enable-imu` 的命令。
- Python `udp-monitor` 只监听本机 UDP 端口，不初始化 SDK，不控制雷达。
- C++ `monitor` 和 `dump` 会初始化 Livox-SDK2；只有显式加控制参数时才请求雷达切换模式或开启数据发送。
- `external/livox_ros_driver2` 只是源码 submodule，不等于已经构建好的 ROS 工作区。

## 更多文档

- `docs/DEPLOYMENT.md`：远端设备部署。
- `docs/DISCOVERY_AND_CONFIG.md`：发现和 JSON 更新逻辑。
- `docs/SAFETY.md`：命令安全边界。
