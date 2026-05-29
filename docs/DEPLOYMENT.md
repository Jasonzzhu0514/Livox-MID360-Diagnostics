# 远端设备部署

下面假设远端设备是 Ubuntu/Linux，并且你已经能通过自己的方式登录远端设备。本文只说明部署步骤，不要求在本机直接 SSH 到远端。

## 1. 选择部署方式

推荐两种方式：

- 远端能访问 GitHub：用 `git clone --recursive` 或 `git submodule update --init --recursive`。
- 远端不能访问 GitHub：在本机打包仓库和 submodule 内容，再传到远端。

## 2. 远端能访问 GitHub

先把本仓库提交并推到你自己的 Git 仓库，然后在远端执行：

```bash
git clone --recursive "$YOUR_REPO_URL" livox-mid360-diagnostics
cd livox-mid360-diagnostics
git submodule update --init --recursive
```

如果已经 clone 过但 submodule 为空：

```bash
./scripts/update_submodules.sh
```

## 3. 远端不能访问 GitHub

在本机仓库目录生成部署包：

```bash
git submodule update --init --recursive
tar --exclude='./build' --exclude='./external/*/build' --exclude='./.git' --exclude='./external/*/.git' -czf livox-mid360-diagnostics.tar.gz .
```

把 `livox-mid360-diagnostics.tar.gz` 传到远端后解压：

```bash
mkdir -p livox-mid360-diagnostics
tar -xzf livox-mid360-diagnostics.tar.gz -C livox-mid360-diagnostics
cd livox-mid360-diagnostics
```

也可以用 `rsync` 传输当前工作树：

```bash
rsync -av --delete --exclude build --exclude 'external/*/build' ./ "$REMOTE_HOST:$REMOTE_DIR/livox-mid360-diagnostics/"
```

## 4. 安装基础依赖

远端设备至少需要：

```bash
sudo apt update
sudo apt install -y build-essential cmake git python3 python3-pip tcpdump iproute2 iputils-ping
```

如果要用 ROS/RViz，还需要安装与你系统匹配的 ROS/ROS2，并构建 `livox_ros_driver2` 工作区。

## 5. 设置路径变量

进入远端仓库目录后：

```bash
source scripts/prepare_config.sh
```

脚本会创建 `config/MID360_config.local.json`，并设置 `LIVOX_MID360_DIAGNOSTICS_ROOT`、`LIVOX_MID360_CONFIG`、`LIVOX_SDK2_ROOT`。然后根据远端网卡 IP 修改 `MID360_config.local.json` 里的 `MID360.host_net_info`。

## 6. 离线构建检查

如果只跑 Python 版，不需要 CMake，也不需要编译 Livox-SDK2：

```bash
./scripts/setup_python.sh
```

如果只跑 C++ 版，执行本地构建检查和 SDK 工具构建。这一步不会连接雷达：

```bash
./scripts/setup_cpp.sh
```

这一步会编译 `external/Livox-SDK2` 和本仓库的 `livox_mid360_diagnostics`、`mid360_sdk_monitor`、`mid360_sdk_dump`，但不会运行 SDK 工具。

如果目标设备不想拉 submodule、也不想编译 SDK，可以在同架构 Linux 设备上生成 C++ 预编译包：

```bash
./scripts/package_cpp_prebuilt.sh
```

生成预编译包的机器需要 submodule；目标设备只需要拿到 `dist/prebuilt/*.tar.gz`，解压后执行：

```bash
source ./env.sh
```

远端交互式使用时，先登录再加载环境：

```bash
ssh user@host
cd livox-mid360-diagnostics-prebuilt
source ./env.sh
```

## 7. 接入雷达后的配置流程

确认雷达供电和物理状态健康、远端设备网卡连接正确后，再运行发现命令。

Python 版：

```bash
livox-mid360-diagnostics autoconfig
```

命令会发现雷达，搜索用户目录和当前目录下的 MID360 配置文件，然后显示 Neon Protocol 自适应表格。配置会按“推荐更新 / 已匹配 / 其它候选”分组，示例、依赖和构建产物里的低优先级配置默认折叠。上下方向键移动，空格选择/取消，`a` 显示或隐藏低优先级候选，回车确认，`q` 或 `Esc` 退出不修改；如果只有低优先级候选，会直接显示。需要默认展示所有候选时可加 `--show-all`；需要纯文本输出时可加 `--no-color` 或设置 `NO_COLOR=1`。

C++ 版：

```bash
./build/sdk2/livox_mid360_diagnostics
./build/sdk2/livox_mid360_diagnostics autoconfig
```

直接运行 `livox_mid360_diagnostics` 会显示 Neon Protocol 自适应入口，选择 `autoconfig`、`monitor`、`dump` 或 `quit`。C++ `autoconfig`、`monitor`、`dump` 在交互终端都会使用 Neon Protocol TUI 并随窗口大小调整；非交互 SSH 或日志管道会降级为纯文本摘要。

如果没有安装 Python package，可以用：

```bash
PYTHONPATH=src/python python3 -m livox_mid360_diagnostics autoconfig
```

脚本化部署时仍然可以显式指定配置并跳过交互：

```bash
./build/sdk2/livox_mid360_diagnostics autoconfig --config "$LIVOX_MID360_CONFIG" --apply --yes
```

## 8. 实时查看数据

Python 版只能被动监听本机 UDP 端口；完整的 SDK 回调统计和 CSV 解析用 C++ 版。

### Python 版被动查看

轻量级终端查看 UDP 包速率，交互终端会显示 Neon Protocol 面板并原地刷新：

```bash
livox-mid360-diagnostics udp-monitor
```

Python 版只监听本机 UDP 端口，不会向雷达发送运行期控制请求。

### C++ 版

SDK 回调统计：

```bash
./build/sdk2/livox_mid360_diagnostics monitor
```

默认先通过 Livox-SDK2 discovery 发现雷达，然后显示 Neon Protocol 实时 TUI，并在原位置刷新，不会持续刷出新行。`monitor` 不再默认使用 `LIVOX_MID360_CONFIG` 里的雷达 IP。多网卡场景可以显式指定：

```bash
./build/sdk2/livox_mid360_diagnostics monitor --iface eth0
```

交互终端中 `monitor` 会显示设备侧栏、点云/IMU 速率条、点云密度预览和模块诊断，画面约 20 FPS 原地刷新；`--interval` 控制速率统计窗口。通过非交互 SSH 或日志管道运行时，会改为每个 interval 输出一行摘要，避免把整屏仪表盘反复追加到终端。`DEVICE/status` 表示 SDK 回调是否还在更新，断网或拔网口后会变为 `LOST`；`DEVICE/health` 表示雷达上报的诊断码。

不通过 ROS 解析并导出短时点云/IMU CSV：

```bash
./build/sdk2/livox_mid360_diagnostics dump --duration 10 --points "$PWD/mid360_points.csv" --imu "$PWD/mid360_imu.csv"
```

`livox_mid360_diagnostics dump` 通过 Livox-SDK2 回调接收数据，不启动 ROS launch。交互终端中会显示 Neon Protocol 采样进度面板并原地刷新；非交互运行时保留每个 interval 一行摘要。点云会统一转换成米制 `x_m,y_m,z_m`，IMU 输出到单独 CSV。CSV 只建议短时采样；长期观察请用 `monitor`。

如果需要请求雷达进入 normal mode 并开启点云/IMU，只在确认雷达连接正确且物理状态健康后使用：

```bash
./build/sdk2/livox_mid360_diagnostics monitor --set-normal-mode --enable-point-send --enable-imu
```

短时 CSV 导出工具也支持相同控制参数：

```bash
./build/sdk2/livox_mid360_diagnostics dump --duration 10 --points "$PWD/mid360_points.csv" --set-normal-mode --enable-point-send --enable-imu
```

## 9. ROS/RViz 查看

`external/livox_ros_driver2` 是源码 submodule，不等于已经构建好的 ROS 工作区。你需要先在远端或目标 ROS 环境中构建工作区，然后设置：

```bash
export LIVOX_ROS_DRIVER2_WS="你的 livox_ros_driver2 工作区根目录"
```

启动 RViz launch：

```bash
./scripts/run_ros2_mid360_rviz.sh "$LIVOX_ROS_DRIVER2_WS"
```

如果远端设备没有图形界面，通常在远端运行驱动，在本地带图形界面的机器上运行 RViz，并确保 ROS 网络配置正确。

## 10. 清理构建缓存

需要收紧工作区时执行：

```bash
./scripts/clean_workspace.sh
```

脚本会清理本仓库和 submodule 里的构建缓存，同时保留 `dist/prebuilt/` 下的预编译包。
