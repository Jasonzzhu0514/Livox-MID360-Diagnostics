# 安全说明

在操作真实 MID360 之前，不要运行会接触雷达的命令，除非已经确认：

- 雷达供电正确，物理状态健康。
- 主机以太网适配器连接在预期网络上。
- `MID360_config.json` 中的 host IP 字段与主机网卡 IP 一致。
- 已明确当前命令是只读检查、本地配置写入，还是运行期 SDK 控制命令。

命令分类：

- `livox-mid360-diagnostics autoconfig` 和 `mid360_config_tool` 运行时会执行网络发现；交互选择配置文件或显式使用 `--apply` 时才会更新文件。
- `livox-mid360-diagnostics udp-monitor` 只绑定本机 UDP 端口并观察收到的数据包，不会向雷达发送命令。
- `mid360_sdk_monitor` 会初始化 Livox-SDK2 并打印回调速率；如果使用相应参数，还可以请求 normal mode、点云发送或 IMU 发送。
- `mid360_sdk_dump` 会初始化 Livox-SDK2 并把回调数据解析成 CSV；CSV 只建议短时采样，默认不主动发送控制请求，只有带相应参数时才会请求 normal mode、点云发送或 IMU 发送。
- `scripts/run_ros2_mid360_rviz.sh` 会启动官方 ROS2 驱动和 RViz，并通过 `livox_ros_driver2` 连接雷达；它需要传入已经构建好的 ROS2 工作区，`external/livox_ros_driver2` 只是源码 submodule。

创建本仓库期间，只做了离线语法检查和构建检查；没有连接雷达，没有 SSH，也没有对物理设备做测试。
