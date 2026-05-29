#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

version="${1:-$(tr -d '[:space:]' < VERSION)}"
arch="$(uname -m)"
package_name="livox-mid360-diagnostics-cpp-${version}-linux-${arch}"
stage="$repo_root/dist/$package_name"
prebuilt_dir="$repo_root/dist/prebuilt"
package_path="$prebuilt_dir/$package_name.tar.gz"
launcher_path="$prebuilt_dir/livox_mid360_diagnostics-linux-${arch}"

./scripts/build_cpp_with_sdk2.sh

rm -rf "$stage"
mkdir -p "$stage/bin" "$stage/config" "$stage/docs" "$prebuilt_dir"

cp build/sdk2/livox_mid360_diagnostics "$stage/bin/"
cp build/sdk2/mid360_config_tool "$stage/bin/"
cp build/sdk2/mid360_sdk_monitor "$stage/bin/"
cp build/sdk2/mid360_sdk_dump "$stage/bin/"
cp external/Livox-SDK2/samples/livox_lidar_quick_start/mid360_config.json "$stage/config/MID360_config.local.json"
cp docs/SAFETY.md "$stage/docs/"
cp docs/DISCOVERY_AND_CONFIG.md "$stage/docs/"

cat > "$stage/env.sh" <<'EOF'
#!/usr/bin/env bash

release_root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export LIVOX_MID360_DIAGNOSTICS_ROOT="$release_root"
export LIVOX_MID360_CONFIG="$release_root/config/MID360_config.local.json"
export PATH="$release_root/bin:$PATH"

echo "LIVOX_MID360_CONFIG=$LIVOX_MID360_CONFIG"
echo "Run: ./livox_mid360_diagnostics"
EOF
chmod +x "$stage/env.sh"

cat > "$stage/livox_mid360_diagnostics" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

release_root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export LIVOX_MID360_DIAGNOSTICS_ROOT="$release_root"
export LIVOX_MID360_CONFIG="$release_root/config/MID360_config.local.json"
export PATH="$release_root/bin:$PATH"

exec "$release_root/bin/livox_mid360_diagnostics" "$@"
EOF
chmod +x "$stage/livox_mid360_diagnostics"

cat > "$stage/README.md" <<'EOF'
# Livox MID360 Diagnostics C++ Prebuilt

这是 C++ CLI 预编译包，适用于同架构 Linux 目标设备。运行这个包不需要 submodule，也不需要在目标设备上编译 Livox-SDK2。

接入雷达并确认物理状态健康后，直接启动菜单入口：

```bash
./livox_mid360_diagnostics
```

直接运行 `./livox_mid360_diagnostics` 会显示 `autoconfig`、`monitor`、`dump`、`quit` 四个交互选项。

`autoconfig` 会搜索用户目录和当前目录下的 MID360 配置文件；上下方向键移动，空格选择/取消，回车确认，q 或 Esc 退出不修改。

`monitor` 显示表格仪表盘并在原位置刷新。`Link` 表示 SDK 回调是否还在更新，断网或拔网口后会变为 `LOST`。

CSV 只建议短时采样；长期观察请用 `monitor`。

需要脚本化运行或指定参数时，再显式加子命令。例如请求雷达进入 normal mode 并开启数据发送：

```bash
./livox_mid360_diagnostics monitor --set-normal-mode --enable-point-send --enable-imu
```
EOF

tar -C "$repo_root/dist" -czf "$package_path" "$package_name"
cat > "$launcher_path" <<EOF
#!/usr/bin/env bash
set -euo pipefail

install_root="\${LIVOX_MID360_RELEASE_DIR:-\$HOME/.local/share/livox-mid360-diagnostics}"
package_name="$package_name"
target="\$install_root/\$package_name"

mkdir -p "\$install_root"
if [[ ! -x "\$target/livox_mid360_diagnostics" ]]; then
  payload_line="\$(awk '/^__LIVOX_MID360_PAYLOAD_BELOW__\$/ { print NR + 1; exit 0; }' "\$0")"
  if [[ -z "\$payload_line" ]]; then
    echo "ERROR: embedded payload not found" >&2
    exit 1
  fi
  tail -n +"\$payload_line" "\$0" | tar -xzf - -C "\$install_root"
fi

exec "\$target/livox_mid360_diagnostics" "\$@"
exit 0
__LIVOX_MID360_PAYLOAD_BELOW__
EOF
cat "$package_path" >> "$launcher_path"
chmod +x "$launcher_path"
rm -rf "$stage"
echo "created: $package_path"
echo "created: $launcher_path"
