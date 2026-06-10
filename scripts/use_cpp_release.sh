#!/usr/bin/env bash

set -euo pipefail

# Downloads the latest single-file C++ launcher to the current directory.

_livox_mid360_release_fail() {
  echo "ERROR: $*" >&2
  exit 1
}

_livox_mid360_repo="${LIVOX_MID360_RELEASE_REPO:-Jasonzzhu0514/Livox-MID360-Diagnostics}"
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

_livox_mid360_download() {
  local url="$1"
  local output="$2"

  curl -fL --progress-bar "$url" -o "$output"
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
  _livox_mid360_tmp_dir="$(mktemp -d)"
  _livox_mid360_pkg="$_livox_mid360_tmp_dir/${_livox_mid360_tar_url##*/}"
  echo "Downloading ${_livox_mid360_tar_url##*/}"
  _livox_mid360_download "$_livox_mid360_tar_url" "$_livox_mid360_pkg" || {
    rm -rf "$_livox_mid360_tmp_dir"
    _livox_mid360_release_fail "download failed: $_livox_mid360_tar_url"
  }
  tar -xzf "$_livox_mid360_pkg" -C "$_livox_mid360_tmp_dir" || {
    rm -rf "$_livox_mid360_tmp_dir"
    _livox_mid360_release_fail "failed to extract $_livox_mid360_pkg"
  }
  _livox_mid360_extracted_binary="$(find "$_livox_mid360_tmp_dir" -type f -name 'livox_mid360_diagnostics-linux-*' -print -quit)"
  if [[ -n "$_livox_mid360_extracted_binary" ]]; then
    cp "$_livox_mid360_extracted_binary" "$_livox_mid360_launcher_path"
    chmod +x "$_livox_mid360_launcher_path"
    rm -rf "$_livox_mid360_tmp_dir"
  else
    _livox_mid360_release_dir="$(find "$_livox_mid360_tmp_dir" -mindepth 1 -maxdepth 1 -type d -print -quit)"
    if [[ -z "$_livox_mid360_release_dir" || ! -x "$_livox_mid360_release_dir/env.sh" ]]; then
      rm -rf "$_livox_mid360_tmp_dir"
      _livox_mid360_release_fail "legacy release layout not recognized"
    fi
    _livox_mid360_install_dir="${LIVOX_MID360_RELEASE_DIR:-$HOME/.local/share/livox-mid360-diagnostics}/${_livox_mid360_release_dir##*/}"
    mkdir -p "${_livox_mid360_install_dir%/*}"
    rm -rf "$_livox_mid360_install_dir"
    mv "$_livox_mid360_release_dir" "$_livox_mid360_install_dir"
    rm -rf "$_livox_mid360_tmp_dir"
    cat > "$_livox_mid360_launcher_path" <<EOF
#!/usr/bin/env bash
set -euo pipefail
release_root="$_livox_mid360_install_dir"
export LIVOX_MID360_DIAGNOSTICS_ROOT="\$release_root"
export LIVOX_MID360_CONFIG="\$release_root/config/MID360_config.local.json"
export PATH="\$release_root/bin:\$PATH"
exec "\$release_root/bin/livox_mid360_diagnostics" "\$@"
EOF
    chmod +x "$_livox_mid360_launcher_path"
  fi
fi

if [[ ! -x "$_livox_mid360_launcher_path" && -n "$_livox_mid360_url" ]]; then
  echo "Downloading ${_livox_mid360_url##*/} -> $_livox_mid360_launcher_path"
  _livox_mid360_download "$_livox_mid360_url" "$_livox_mid360_launcher_path" || {
    _livox_mid360_release_fail "download failed: $_livox_mid360_url"
  }
  chmod +x "$_livox_mid360_launcher_path"
fi

echo "Ready: $_livox_mid360_launcher_path"
echo "Run:"
echo "  ./$(basename "$_livox_mid360_launcher_path")"
