#!/usr/bin/env bash

set -euo pipefail

# Downloads the latest single-file C++ launcher to the current directory.

_livox_mid360_release_fail() {
  echo "ERROR: $*" >&2
  exit 1
}

_livox_mid360_repo="${LIVOX_MID360_RELEASE_REPO:-Jasonzzhu0514/Livox-MID360-Diagnostics}"
_livox_mid360_base_dir="${LIVOX_MID360_RELEASE_DIR:-$HOME/.local/share/livox-mid360-diagnostics}"
_livox_mid360_launcher_path="${LIVOX_MID360_LAUNCHER_PATH:-$PWD/livox_mid360_diagnostics}"
_livox_mid360_arch="$(uname -m)"

case "$_livox_mid360_arch" in
  x86_64)
    _livox_mid360_suffix="livox_mid360_diagnostics-linux-x86_64"
    _livox_mid360_tar_suffix="linux-x86_64.tar.gz"
    ;;
  aarch64|arm64)
    _livox_mid360_suffix="livox_mid360_diagnostics-linux-aarch64"
    _livox_mid360_tar_suffix="linux-aarch64.tar.gz"
    ;;
  *)
    _livox_mid360_release_fail "unsupported arch: $_livox_mid360_arch"
    ;;
esac

command -v curl >/dev/null 2>&1 || {
  _livox_mid360_release_fail "curl is required"
}
command -v awk >/dev/null 2>&1 || {
  _livox_mid360_release_fail "awk is required"
}
command -v tar >/dev/null 2>&1 || {
  _livox_mid360_release_fail "tar is required"
}

mkdir -p "$_livox_mid360_base_dir" || {
  _livox_mid360_release_fail "failed to create $_livox_mid360_base_dir"
}

_livox_mid360_url="$(
  curl -fsSL "https://api.github.com/repos/$_livox_mid360_repo/releases/latest" |
    awk -F\" -v suffix="$_livox_mid360_suffix" '$2 == "browser_download_url" && $4 ~ suffix "$" { print $4; exit }'
)" || {
  _livox_mid360_release_fail "failed to query latest release"
}

if [[ -z "$_livox_mid360_url" ]]; then
  _livox_mid360_tar_url="$(
    curl -fsSL "https://api.github.com/repos/$_livox_mid360_repo/releases/latest" |
      awk -F\" -v suffix="$_livox_mid360_tar_suffix" '$2 == "browser_download_url" && $4 ~ suffix "$" { print $4; exit }'
  )" || {
    _livox_mid360_release_fail "failed to query latest release"
  }
  if [[ -z "$_livox_mid360_tar_url" ]]; then
    _livox_mid360_release_fail "release asset not found for $_livox_mid360_suffix"
  fi
  _livox_mid360_pkg="${_livox_mid360_tar_url##*/}"
  _livox_mid360_release_dir="$_livox_mid360_base_dir/${_livox_mid360_pkg%.tar.gz}"
  _livox_mid360_pkg_path="$_livox_mid360_base_dir/$_livox_mid360_pkg"
  if [[ ! -x "$_livox_mid360_release_dir/env.sh" ]]; then
    echo "Downloading $_livox_mid360_pkg"
    curl -fsSL "$_livox_mid360_tar_url" -o "$_livox_mid360_pkg_path" || {
      _livox_mid360_release_fail "download failed: $_livox_mid360_tar_url"
    }
    tar -xzf "$_livox_mid360_pkg_path" -C "$_livox_mid360_base_dir" || {
      _livox_mid360_release_fail "failed to extract $_livox_mid360_pkg_path"
    }
  fi
  if [[ -x "$_livox_mid360_release_dir/livox_mid360_diagnostics" ]]; then
    ln -sf "$_livox_mid360_release_dir/livox_mid360_diagnostics" "$_livox_mid360_launcher_path"
    echo "Ready: $_livox_mid360_launcher_path"
    echo "Run:"
    echo "  ./$(basename "$_livox_mid360_launcher_path")"
    exit 0
  fi
  echo "Ready: $_livox_mid360_release_dir"
  echo "This release uses the legacy layout. Run:"
  echo "  cd \"$_livox_mid360_release_dir\""
  echo "  source ./env.sh"
  echo "  livox_mid360_diagnostics"
  exit 0
fi

if [[ ! -x "$_livox_mid360_launcher_path" ]]; then
  echo "Downloading ${_livox_mid360_url##*/} -> $_livox_mid360_launcher_path"
  curl -fsSL "$_livox_mid360_url" -o "$_livox_mid360_launcher_path" || {
    _livox_mid360_release_fail "download failed: $_livox_mid360_url"
  }
  chmod +x "$_livox_mid360_launcher_path"
fi

echo "Ready: $_livox_mid360_launcher_path"
echo "Run:"
echo "  ./$(basename "$_livox_mid360_launcher_path")"
