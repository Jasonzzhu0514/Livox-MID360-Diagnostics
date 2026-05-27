#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

prebuilt_dir="$repo_root/dist/prebuilt"

mkdir -p "$prebuilt_dir"

find "$repo_root/dist" -maxdepth 1 -type f -name '*.tar.gz' -exec mv -n -t "$prebuilt_dir" {} +
find "$repo_root/dist" -mindepth 1 -maxdepth 1 -type d ! -name prebuilt -exec rm -rf {} +

rm -rf "$repo_root/build"
rm -rf "$repo_root/.pytest_cache" "$repo_root/.mypy_cache" "$repo_root/.venv"

find "$repo_root" -type d -name '__pycache__' -prune -exec rm -rf {} +
find "$repo_root" -type f \( -name '*.pyc' -o -name '*.pyo' \) -delete
find "$repo_root" -maxdepth 2 -type d -name '*.egg-info' -prune -exec rm -rf {} +
find "$repo_root/external" -mindepth 2 -maxdepth 2 \( -name build -o -name install \) -type d -prune -exec rm -rf {} +

echo "工作区已清理。预编译包保留在：$prebuilt_dir"
find "$prebuilt_dir" -maxdepth 1 -type f -name '*.tar.gz' -printf '  %f\n' | sort
