#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

version="${1:-$(tr -d '[:space:]' < VERSION)}"
arch="$(uname -m)"
package_name="livox-mid360-diagnostics-cpp-${version}-linux-${arch}"
prebuilt_dir="$repo_root/dist/prebuilt"
package_path="$prebuilt_dir/$package_name.tar.gz"
launcher_path="$prebuilt_dir/livox_mid360_diagnostics-linux-${arch}"

./scripts/build_cpp_with_sdk2.sh

mkdir -p "$prebuilt_dir"
cp build/sdk2/livox_mid360_diagnostics "$launcher_path"
chmod +x "$launcher_path"

tar -C "$prebuilt_dir" -czf "$package_path" "$(basename "$launcher_path")"
echo "created: $launcher_path"
echo "created: $package_path"
