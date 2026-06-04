#!/usr/bin/env python3
"""Discover a Livox MID360-like lidar on an Ethernet port and update config.

Default mode is read-only: it actively scans for the lidar IP first, then tries
cheap optional sources for broadcast code/SN.
Interactive terminals show a config picker. Use --apply with --config for scripted writes.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import ipaddress
import json
import os
import re
import shutil
import select
import subprocess
import sys
import termios
import tempfile
import time
import tty
from dataclasses import dataclass
from pathlib import Path

try:
    from . import neon_tui as neon
except ImportError:  # pragma: no cover - supports direct script execution.
    import neon_tui as neon


DISCOVERY_PORT = 56000
LIVOX_MAC_PREFIXES = ("8c:58:23",)
MIN_ACTIVE_SCAN_LIDAR_SCORE = 100
CONFIG_ENV_VARS = ("LIVOX_MID360_CONFIG", "MID360_CONFIG")
CONFIG_SEARCH_ROOT_ENV = "LIVOX_MID360_SEARCH_ROOTS"
SDK_SEARCH_ROOT_ENV = "LIVOX_SDK_SEARCH_ROOTS"
SDK_QUERY_CACHE_ENV = "LIVOX_MID360_SDK_QUERY_CACHE"
MID360_CONFIG_KEYS = ("MID360", "Mid360s")

SCREEN_ACTIVE = False
SCAN_LINES: list[str] = []
ORIGINAL_TERMIOS = None
SCAN_RENDERER = neon.LineDiffRenderer()


SDK_QUERY_MAIN_CPP = r'''
#include "livox_lidar_api.h"
#include "livox_lidar_def.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace {
std::mutex g_mutex;
std::condition_variable g_cv;
std::atomic<bool> g_found(false);
std::string g_sn;
std::string g_ip;
uint8_t g_dev_type = 0;

void LidarInfoChangeCallback(const uint32_t handle, const LivoxLidarInfo* info, void*) {
  (void)handle;
  if (info == nullptr) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_sn = info->sn;
    g_ip = info->lidar_ip;
    g_dev_type = info->dev_type;
    g_found = true;
  }
  g_cv.notify_one();
}
}  // namespace

int main(int argc, const char* argv[]) {
  if (argc < 2 || argc > 3) {
    std::fprintf(stderr, "usage: %s <MID360_config.json> [timeout_sec]\n", argv[0]);
    return 2;
  }
  const char* config_path = argv[1];
  const double timeout_sec = argc == 3 ? std::atof(argv[2]) : 4.0;

  DisableLivoxSdkConsoleLogger();
  if (!LivoxLidarSdkInit(config_path)) {
    std::fprintf(stderr, "SDK_ERROR init_failed config=%s\n", config_path);
    LivoxLidarSdkUninit();
    return 2;
  }
  SetLivoxLidarInfoChangeCallback(LidarInfoChangeCallback, nullptr);

  std::unique_lock<std::mutex> lock(g_mutex);
  g_cv.wait_for(lock, std::chrono::milliseconds(static_cast<int>(timeout_sec * 1000.0)), [] {
    return g_found.load();
  });

  if (!g_found.load()) {
    std::printf("SDK_SN N/A\n");
    LivoxLidarSdkUninit();
    return 1;
  }

  std::printf("SDK_SN %s\n", g_sn.c_str());
  std::printf("SDK_IP %s\n", g_ip.c_str());
  std::printf("SDK_DEV_TYPE %u\n", static_cast<unsigned>(g_dev_type));
  LivoxLidarSdkUninit();
  return 0;
}
'''


def _expand_path(raw_path: str | os.PathLike[str]) -> Path:
    return Path(os.path.expandvars(str(raw_path))).expanduser()


def _split_env_paths(value: str) -> list[Path]:
    return [_expand_path(item) for item in value.split(os.pathsep) if item.strip()]


def _env_paths(*names: str) -> list[Path]:
    paths: list[Path] = []
    for name in names:
        value = os.environ.get(name)
        if value:
            paths.extend(_split_env_paths(value))
    return paths


def _dedupe_paths(paths: list[Path]) -> list[Path]:
    deduped: list[Path] = []
    seen: set[str] = set()
    for path in paths:
        key = str(path)
        if key not in seen:
            seen.add(key)
            deduped.append(path)
    return deduped


def repo_root() -> Path:
    return Path(__file__).resolve().parents[3]


def config_search_roots() -> list[Path]:
    env_roots = _env_paths(CONFIG_SEARCH_ROOT_ENV)
    roots = env_roots if env_roots else [
        Path.home(),
        Path.cwd(),
        repo_root(),
    ]
    return [path for path in _dedupe_paths(roots) if path.is_dir()]


def sdk_search_roots() -> list[Path]:
    env_roots = _env_paths(SDK_SEARCH_ROOT_ENV)
    roots = env_roots if env_roots else [
        repo_root() / "external/Livox-SDK2",
        repo_root(),
        Path.cwd(),
        Path.home(),
    ]
    return [path for path in _dedupe_paths(roots) if path.is_dir()]


def sdk_query_cache_dir() -> Path:
    override = os.environ.get(SDK_QUERY_CACHE_ENV)
    if override:
        return _expand_path(override)
    cache_home = _expand_path(os.environ.get("XDG_CACHE_HOME", str(Path.home() / ".cache")))
    return cache_home / "livox-mid360-diagnostics/livox_sdk_sn_query"


def default_config_path() -> Path:
    env_paths = _env_paths(*CONFIG_ENV_VARS)
    if env_paths:
        return env_paths[0]
    local_config = repo_root() / "config" / "MID360_config.local.json"
    return local_config if local_config.is_file() else Path("MID360_config.json")


def _config_priority(path: Path) -> tuple[int, float, int, str]:
    lowered = str(path).lower()
    if "livox-mid360-diagnostics-prebuilt/config/" in lowered:
        priority = 0
    elif "/config/" in lowered:
        priority = 1
    elif "/external/" in lowered or "/samples/" in lowered:
        priority = 3
    elif "/dist/" in lowered or "/build/" in lowered:
        priority = 4
    else:
        priority = 2
    try:
        newest_first = -path.stat().st_mtime
    except OSError:
        newest_first = 0.0
    return priority, newest_first, 0, str(path)


def is_mid360_config_file(path: Path) -> bool:
    if not path.is_file():
        return False
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        return False
    if isinstance(data.get("lidar_configs"), list):
        return True
    return any(isinstance(data.get(key), dict) and "host_net_info" in data[key] for key in MID360_CONFIG_KEYS)


def discover_mid360_config_paths(verbose: bool = False) -> list[Path]:
    found: list[Path] = []
    seen: set[Path] = set()
    for root in config_search_roots():
        if SCREEN_ACTIVE:
            progress(f"searching config files under {root}")
        output = run_text(
            [
                "find",
                str(root),
                "-maxdepth",
                "8",
                "-type",
                "f",
                "(",
                "-iname",
                "*mid360*config*.json",
                "-o",
                "-iname",
                "mid360_config.json",
                ")",
                "-not",
                "-path",
                "*/.git/*",
                "-not",
                "-path",
                "*/build/*",
            ],
            timeout=12,
        )
        for line in output.splitlines():
            path = Path(line.strip()).expanduser()
            key = path.resolve() if path.exists() else path
            if is_mid360_config_file(path) and key not in seen:
                seen.add(key)
                found.append(path)
    found.sort(key=_config_priority)
    verbose_print(
        verbose,
        "discovered MID360 configs: "
        + (
            str(
                [
                    {
                        "path": str(path),
                        "mtime": int(path.stat().st_mtime),
                    }
                    for path in found
                ]
            )
            if found
            else "N/A"
        ),
    )
    return found


def resolve_config_paths(raw_paths: list[str] | None, verbose: bool = False) -> list[Path]:
    if raw_paths:
        return [_expand_path(raw_path) for raw_path in raw_paths]
    discovered = discover_mid360_config_paths(verbose=verbose)
    if discovered:
        return discovered
    env_paths = _env_paths(*CONFIG_ENV_VARS)
    existing_env_paths = [path for path in env_paths if path.is_file()]
    if existing_env_paths:
        return existing_env_paths
    if env_paths:
        return env_paths
    local_config = repo_root() / "config" / "MID360_config.local.json"
    if local_config.is_file():
        return [local_config]
    return [default_config_path()]


def should_enable_color(no_color: bool = False) -> bool:
    return (
        not no_color
        and sys.stdout.isatty()
        and not os.environ.get("NO_COLOR")
        and os.environ.get("TERM", "") != "dumb"
    )


def init_theme(no_color: bool = False) -> None:
    global THEME
    if not should_enable_color(no_color):
        THEME = Theme()
        return
    THEME = Theme(
        enabled=True,
        reset="\033[0m",
        bold="\033[1m",
        dim="\033[2m",
        red="\033[31m",
        green="\033[32m",
        yellow="\033[33m",
        blue="\033[34m",
        cyan="\033[36m",
    )


def colorize(text: str, color: str) -> str:
    if not THEME.enabled or not color:
        return text
    return f"{color}{text}{THEME.reset}"


def bold(text: str) -> str:
    return colorize(text, THEME.bold)


def dim(text: str) -> str:
    return colorize(text, THEME.dim)


def green(text: str) -> str:
    return colorize(text, THEME.green)


def yellow(text: str) -> str:
    return colorize(text, THEME.yellow)


def red(text: str) -> str:
    return colorize(text, THEME.red)


def blue(text: str) -> str:
    return colorize(text, THEME.blue)


def cyan(text: str) -> str:
    return colorize(text, THEME.cyan)


def status_label(status: str) -> str:
    if status == "mismatch":
        return yellow(status)
    if status == "match":
        return green(status)
    if status == "unavailable":
        return dim(status)
    return status


def path_display(path: Path) -> str:
    try:
        absolute = path.resolve()
    except OSError:
        absolute = path
    try:
        cwd = Path.cwd().resolve()
        return str(absolute.relative_to(cwd))
    except (OSError, ValueError):
        pass
    try:
        home = Path.home().resolve()
        return "~/" + str(absolute.relative_to(home))
    except (OSError, ValueError):
        return str(path)


def compact_path(path: Path, max_width: int = 76) -> str:
    display = path_display(path)
    if len(display) <= max_width:
        return display
    if max_width <= 10:
        return display[:max_width]
    return ".../" + display[-(max_width - 4) :]


def is_low_priority_config_path(path: Path) -> bool:
    lowered = path_display(path).lower()
    return (
        "/external/" in lowered
        or lowered.startswith("external/")
        or "/samples/" in lowered
        or lowered.startswith("samples/")
        or "/thirdparty/" in lowered
        or lowered.startswith("thirdparty/")
        or "/3rdparty/" in lowered
        or lowered.startswith("3rdparty/")
        or "/.runtime/" in lowered
        or lowered.startswith(".runtime/")
        or "/examples/" in lowered
        or lowered.startswith("examples/")
        or "/build/" in lowered
        or lowered.startswith("build/")
        or "/dist/" in lowered
        or lowered.startswith("dist/")
    )


@dataclass
class Discovery:
    lidar_ip: str | None = None
    broadcast_code: str | None = None
    requested_host_ip: str | None = None
    iface_ip: str | None = None
    raw_packets: int = 0
    method: str = ""


@dataclass
class ConfigState:
    path: Path
    configured_ip: str | None
    status: str
    low_priority: bool = False
    original_index: int = 0


@dataclass
class ConfigView:
    recommended: list[ConfigState]
    matched: list[ConfigState]
    other: list[ConfigState]
    hidden_count: int = 0


@dataclass
class Theme:
    enabled: bool = False
    reset: str = ""
    bold: str = ""
    dim: str = ""
    red: str = ""
    green: str = ""
    yellow: str = ""
    blue: str = ""
    cyan: str = ""


THEME = Theme()


def append_method(method: str, suffix: str) -> str:
    return f"{method}+{suffix}" if method else suffix


def scan_wait_tick() -> None:
    if not SCREEN_ACTIVE:
        return
    key = neon.read_key(0.0, 0.02)
    if key in {neon.KEY_QUIT, neon.KEY_ESCAPE}:
        raise KeyboardInterrupt
    render_scan_screen()


def run_text(command: list[str], timeout: float | None = None) -> str:
    if not SCREEN_ACTIVE:
        try:
            proc = subprocess.run(
                command,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=timeout,
                check=False,
            )
        except subprocess.TimeoutExpired as exc:
            stdout = exc.stdout or ""
            stderr = exc.stderr or ""
            if isinstance(stdout, bytes):
                stdout = stdout.decode(errors="replace")
            if isinstance(stderr, bytes):
                stderr = stderr.decode(errors="replace")
            return stdout + stderr
        return proc.stdout + proc.stderr

    deadline = time.monotonic() + timeout if timeout is not None else None
    try:
        proc = subprocess.Popen(
            command,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            start_new_session=True,
        )
    except OSError:
        return ""

    chunks: list[str] = []
    try:
        while True:
            if SCREEN_ACTIVE:
                scan_wait_tick()
            if proc.poll() is not None:
                break
            if deadline is not None and time.monotonic() >= deadline:
                proc.terminate()
                try:
                    proc.wait(timeout=0.5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                break
            time.sleep(0.05 if SCREEN_ACTIVE else 0.02)
    except KeyboardInterrupt:
        proc.terminate()
        try:
            proc.wait(timeout=0.5)
        except subprocess.TimeoutExpired:
            proc.kill()
        raise

    stdout, stderr = proc.communicate()
    chunks.append(stdout or "")
    chunks.append(stderr or "")
    return "".join(chunks)


def iface_ipv4(iface: str) -> str | None:
    output = run_text(["ip", "-4", "-o", "addr", "show", "dev", iface])
    match = re.search(r"\binet\s+(\d+\.\d+\.\d+\.\d+)/", output)
    return match.group(1) if match else None


def iface_ipv4_info(iface: str) -> tuple[str | None, int | None]:
    output = run_text(["ip", "-4", "-o", "addr", "show", "dev", iface])
    match = re.search(r"\binet\s+(\d+\.\d+\.\d+\.\d+)/(\d+)", output)
    if not match:
        return None, None
    return match.group(1), int(match.group(2))


def list_ipv4_interfaces() -> list[tuple[str, str, int]]:
    output = run_text(["ip", "-4", "-o", "addr", "show"])
    interfaces: list[tuple[str, str, int]] = []
    for line in output.splitlines():
        match = re.search(r"^\d+:\s+([^:\s]+)\s+.*\binet\s+(\d+\.\d+\.\d+\.\d+)/(\d+)", line)
        if not match:
            continue
        iface, ip, prefix = match.groups()
        if iface == "lo" or iface.startswith("docker"):
            continue
        interfaces.append((iface, ip, int(prefix)))
    return interfaces


def candidate_ifaces(requested_iface: str, config_paths: list[Path]) -> list[str]:
    if requested_iface.strip().lower() != "auto":
        return [requested_iface]

    configured_ips = [read_config_lidar_ip(path) for path in config_paths]
    configured_ips = [ip for ip in configured_ips if ip]
    interfaces = list_ipv4_interfaces()
    candidates: list[str] = []

    for configured_ip in configured_ips:
        try:
            lidar_addr = ipaddress.ip_address(configured_ip)
        except ValueError:
            continue
        for iface, host_ip, prefix in interfaces:
            network = ipaddress.ip_network(f"{host_ip}/{prefix}", strict=False)
            if lidar_addr in network and iface not in candidates:
                candidates.append(iface)

    ethernet_like = [
        iface
        for iface, _, _ in interfaces
        if iface.startswith(("eth", "en", "eno", "ens", "enp"))
    ]
    for iface in ethernet_like:
        if iface not in candidates:
            candidates.append(iface)
    for iface, _, _ in interfaces:
        if iface not in candidates:
            candidates.append(iface)
    return candidates or ["eth0"]


def verbose_print(enabled: bool, message: str) -> None:
    if enabled:
        print(f"[debug] {message}", file=sys.stderr)


def render_scan_screen(message: str | None = None) -> None:
    if message:
        SCAN_LINES.append(f"{neon.text('扫描', neon.ACCENT, True)} {message}")
        del SCAN_LINES[:-128]
    rows, cols = neon.terminal_size()
    width = max(40, cols)
    max_lines = max(3, rows - 8)
    visible = SCAN_LINES[-max_lines:] or [f"{neon.text('扫描', neon.ACCENT, True)} preparing autoconfig"]
    out = [neon.header("LIVOX MID-360 AUTOCONFIG", "SCAN", width)]
    used_rows = 3
    panel = neon.box("DISCOVERY TRACE", visible, width)
    used_rows += neon.append_lines(out, panel, width, rows - used_rows - 1)
    neon.append_footer_at_bottom(out, "[CTRL+C] STOP", rows, width, used_rows)
    SCAN_RENDERER.render("".join(out), rows, width)


def progress(message: str) -> None:
    if SCREEN_ACTIVE:
        render_scan_screen(message)
    else:
        line = f"{cyan('扫描')} {message}"
        print(line, flush=True)


def parse_ascii_from_hex_dump(lines: list[str], verbose: bool = False) -> str | None:
    blob = bytearray()
    for line in lines:
        match = re.search(r"0x[0-9a-fA-F]+:\s+(.*)$", line)
        if not match:
            continue
        for token in match.group(1).split():
            if not re.fullmatch(r"[0-9a-fA-F]{4}", token):
                break
            blob.extend(bytes.fromhex(token))
    if not blob:
        verbose_print(verbose, "hex dump: no payload bytes found")
        return None
    text = "".join(chr(b) if 32 <= b < 127 else " " for b in blob)
    candidates = re.findall(r"[A-Z0-9]{10,16}", text)
    verbose_print(verbose, f"hex dump: decoded {len(blob)} bytes")
    verbose_print(verbose, f"hex dump ascii: {text.strip() or 'N/A'}")
    verbose_print(verbose, f"broadcast candidates: {candidates or 'N/A'}")
    if not candidates:
        return None
    candidates.sort(key=lambda item: (("LIVOX" in item) or ("ARM" in item), len(item)), reverse=True)
    verbose_print(verbose, f"selected broadcast_code candidate: {candidates[0]}")
    return candidates[0]


def _looks_like_livox_sn(value: str) -> bool:
    return bool(re.fullmatch(r"[A-Z0-9]{10,16}", value)) and any(ch.isalpha() for ch in value)


def parse_sn_from_text(text: str) -> str | None:
    patterns = [
        r"(?i)\bSN\s*[:=]\s*([A-Z0-9]{10,16})\b",
        r"(?i)\bsn\s*[:=]\s*([A-Z0-9]{10,16})\b",
        r"(?i)\bbroadcast[_ -]?code\s*[:=]\s*([A-Z0-9]{10,16})\b",
    ]
    for pattern in patterns:
        for match in re.finditer(pattern, text):
            candidate = match.group(1).upper()
            if _looks_like_livox_sn(candidate):
                return candidate
    return None


def recent_log_sn(lidar_ip: str | None, max_age_days: int = 14, verbose: bool = False) -> str | None:
    log_root = _expand_path(os.environ.get("ROS_LOG_DIR", str(Path.home() / ".ros/log")))
    if not log_root.is_dir():
        verbose_print(verbose, f"recent log SN skipped: ROS log directory not found: {log_root}")
        return None
    output = run_text(
        [
            "find",
            str(log_root),
            "-type",
            "f",
            "-mtime",
            f"-{max_age_days}",
        ],
        timeout=5,
    )
    files = [Path(line.strip()) for line in output.splitlines() if line.strip()]
    files = [path for path in files if path.is_file()]
    files.sort(key=lambda path: path.stat().st_mtime, reverse=True)
    verbose_print(verbose, f"recent log SN scan files={len(files)}")

    fallback_sn: str | None = None
    for path in files[:80]:
        try:
            with path.open("rb") as fh:
                fh.seek(0, os.SEEK_END)
                size = fh.tell()
                fh.seek(max(0, size - 256 * 1024), os.SEEK_SET)
                text = fh.read().decode(errors="replace")
        except OSError:
            continue
        sn = parse_sn_from_text(text)
        if not sn:
            continue
        verbose_print(verbose, f"recent log SN candidate: sn={sn}, file={path}")
        if lidar_ip and lidar_ip in text:
            return sn
        if fallback_sn is None:
            fallback_sn = sn
    return fallback_sn


def parse_tcpdump(output: str, iface: str, verbose: bool = False) -> Discovery:
    result = Discovery(iface_ip=iface_ipv4(iface))
    hex_lines: list[str] = []
    lines = output.splitlines()
    verbose_print(verbose, f"parse iface={iface}, iface_ip={result.iface_ip or 'N/A'}")
    verbose_print(verbose, f"tcpdump output lines={len(lines)}")
    for line in output.splitlines():
        if f".{DISCOVERY_PORT} >" in line and "IP " in line:
            match = re.search(r"\bIP\s+(\d+\.\d+\.\d+\.\d+)\.%d\s+>" % DISCOVERY_PORT, line)
            if match:
                packet_source_ip = match.group(1)
                if packet_source_ip == result.iface_ip:
                    verbose_print(verbose, f"ignore host discovery packet: source_ip={packet_source_ip}, line={line}")
                    continue
                result.lidar_ip = packet_source_ip
                result.method = "livox_discovery"
                result.raw_packets += 1
                verbose_print(verbose, f"UDP discovery packet: lidar_ip={result.lidar_ip}, line={line}")
        if "ARP," in line and "who-has" in line and " tell " in line:
            match = re.search(
                r"who-has\s+(\d+\.\d+\.\d+\.\d+)\s+tell\s+(\d+\.\d+\.\d+\.\d+)",
                line,
            )
            if match:
                host_ip, lidar_ip = match.groups()
                verbose_print(verbose, f"ARP request: lidar_ip={lidar_ip}, requested_host_ip={host_ip}")
                if result.lidar_ip is None or result.lidar_ip == lidar_ip:
                    result.lidar_ip = lidar_ip
                    result.requested_host_ip = host_ip
                    result.method = "arp_observed"
        if re.search(r"0x[0-9a-fA-F]+:", line):
            hex_lines.append(line)
    verbose_print(verbose, f"hex dump lines={len(hex_lines)}")
    result.broadcast_code = parse_ascii_from_hex_dump(hex_lines, verbose=verbose)
    verbose_print(
        verbose,
        "parse result: "
        f"lidar_ip={result.lidar_ip or 'N/A'}, "
        f"broadcast_code={result.broadcast_code or 'N/A'}, "
        f"requested_host_ip={result.requested_host_ip or 'N/A'}, "
        f"raw_packets={result.raw_packets}",
    )
    return result


def sniff(iface: str, timeout_sec: float, sudo: bool, verbose: bool = False) -> Discovery:
    tcpdump = shutil.which("tcpdump")
    if not tcpdump:
        raise RuntimeError("tcpdump not found. Install it first, e.g. sudo apt install tcpdump")
    command = [
        tcpdump,
        "-ni",
        iface,
        f"(udp and port {DISCOVERY_PORT}) or arp",
        "-X",
    ]
    if sudo and hasattr(os, "geteuid") and os.geteuid() != 0:
        command.insert(0, "sudo")
    verbose_print(verbose, f"sniff command: {' '.join(command)}")
    verbose_print(verbose, f"sniff timeout: {timeout_sec}s")
    progress(f"listening on {iface} for Livox discovery packets ({timeout_sec:.1f}s)")
    output = run_text(["timeout", "-k", "2", str(timeout_sec), *command], timeout=timeout_sec + 5)
    if verbose:
        preview = "\n".join(output.splitlines()[:40])
        verbose_print(verbose, "tcpdump preview begin")
        if preview:
            print(preview, file=sys.stderr)
        else:
            print("(empty tcpdump output)", file=sys.stderr)
        verbose_print(verbose, "tcpdump preview end")
    return parse_tcpdump(output, iface, verbose=verbose)


def _scan_network_for_iface(iface: str) -> ipaddress.IPv4Network | None:
    host_ip, prefix = iface_ipv4_info(iface)
    if not host_ip or prefix is None:
        return None
    network = ipaddress.ip_network(f"{host_ip}/{prefix}", strict=False)
    if network.num_addresses > 256:
        network = ipaddress.ip_network(f"{host_ip}/24", strict=False)
    return network


def _ping_once(iface: str, ip: str) -> None:
    run_text(["timeout", "0.8", "ping", "-I", iface, "-c", "1", "-W", "1", ip], timeout=1.2)


def _neighbor_entries(iface: str) -> list[dict[str, str]]:
    output = run_text(["ip", "neigh", "show", "dev", iface])
    entries: list[dict[str, str]] = []
    for line in output.splitlines():
        parts = line.split()
        if not parts:
            continue
        ip = parts[0]
        if "lladdr" not in parts:
            continue
        state = parts[-1]
        if state in {"FAILED", "INCOMPLETE"}:
            continue
        mac = parts[parts.index("lladdr") + 1]
        entries.append({"ip": ip, "mac": mac.lower(), "state": state})
    return entries


def _ping_ttl(iface: str, ip: str) -> int | None:
    output = run_text(["timeout", "1.5", "ping", "-I", iface, "-c", "1", "-W", "1", ip], timeout=2.0)
    match = re.search(r"\bttl=(\d+)", output, flags=re.IGNORECASE)
    return int(match.group(1)) if match else None


def _gateway_ips(iface: str) -> set[str]:
    output = run_text(["ip", "route", "show", "dev", iface])
    gateways: set[str] = set()
    for line in output.splitlines():
        match = re.search(r"\bvia\s+(\d+\.\d+\.\d+\.\d+)", line)
        if match:
            gateways.add(match.group(1))
    return gateways


def active_scan(iface: str, verbose: bool = False) -> Discovery:
    host_ip = iface_ipv4(iface)
    result = Discovery(iface_ip=host_ip, method="active_scan")
    network = _scan_network_for_iface(iface)
    if network is None:
        verbose_print(verbose, f"active scan skipped: no IPv4 network for iface={iface}")
        return result

    hosts = [str(ip) for ip in network.hosts() if str(ip) != host_ip]
    progress(f"active scan on {iface} network={network} hosts={len(hosts)}")
    verbose_print(verbose, f"active scan iface={iface}, network={network}, hosts={len(hosts)}")
    with concurrent.futures.ThreadPoolExecutor(max_workers=64) as executor:
        futures = [executor.submit(_ping_once, iface, ip) for ip in hosts]
        completed = 0
        report_step = max(16, len(futures) // 8) if futures else 1
        for future in concurrent.futures.as_completed(futures):
            future.result()
            completed += 1
            if completed == len(futures) or completed % report_step == 0:
                print(f"\r[scan] active scan progress {completed}/{len(futures)}", end="", flush=True)
        if futures:
            print()

    gateways = _gateway_ips(iface)
    candidates = []
    for entry in _neighbor_entries(iface):
        ip = entry["ip"]
        if ip == host_ip or ip in gateways:
            continue
        ttl = _ping_ttl(iface, ip)
        mac = entry["mac"]
        score = 0
        if mac.startswith(LIVOX_MAC_PREFIXES):
            score += 100
        if ttl is not None and ttl >= 200:
            score += 40
        if ip.startswith("192.168.1."):
            score += 5
        candidates.append({**entry, "ttl": ttl, "score": score})

    candidates.sort(key=lambda item: (item["score"], item["state"] == "REACHABLE"), reverse=True)
    verbose_print(verbose, f"active scan candidates={candidates}")
    if candidates:
        preview = ", ".join(
            f"{item['ip']} ttl={item['ttl'] or 'N/A'} mac={item['mac']} score={item['score']}"
            for item in candidates[:5]
        )
        progress(f"active scan candidates: {preview}")
    else:
        progress(f"active scan found no candidates on {iface}")
    if candidates and candidates[0]["score"] >= MIN_ACTIVE_SCAN_LIDAR_SCORE:
        result.lidar_ip = candidates[0]["ip"]
    elif candidates:
        progress(
            "active scan found online devices, but none match known MID360/Livox signatures; "
            "not treating them as lidar"
        )
    return result


def _sdk2_candidates() -> list[Path]:
    def is_sdk_root(path: Path) -> bool:
        return (
            (path / "include/livox_lidar_api.h").is_file()
            and (path / "include/livox_lidar_def.h").is_file()
        )

    def add_candidate(path: Path) -> None:
        resolved = Path(os.path.expandvars(str(path))).expanduser()
        if is_sdk_root(resolved) and resolved not in candidates:
            candidates.append(resolved)

    candidates: list[Path] = []
    for env_name in ("LIVOX_SDK2_ROOT", "LIVOX_SDK_ROOT", "LIVOX_SDK_PATH"):
        env_value = os.environ.get(env_name)
        if env_value:
            add_candidate(Path(env_value))

    if candidates:
        return candidates

    seen_search_roots: set[Path] = set()
    for root in sdk_search_roots():
        if not root.is_dir() or root in seen_search_roots:
            continue
        seen_search_roots.add(root)
        output = run_text(
            [
                "find",
                str(root),
                "-maxdepth",
                "7",
                "-type",
                "f",
                "-path",
                "*/include/livox_lidar_api.h",
            ],
            timeout=8,
        )
        for line in output.splitlines():
            api_header = Path(line.strip())
            if api_header.name != "livox_lidar_api.h":
                continue
            add_candidate(api_header.parent.parent)
        if candidates:
            break
    return candidates


def _sdk_source_files(sdk_root: Path) -> list[str]:
    sdk_core = sdk_root / "sdk_core"
    platform = "unix"
    rel_paths = [
        "device_manager.cpp",
        "livox_lidar_sdk.cpp",
        "params_check.cpp",
        "parse_cfg_file.cpp",
        "base/io_loop.cpp",
        "base/thread_base.cpp",
        "base/io_thread.cpp",
        "base/logging.cpp",
        f"base/network/{platform}/network_util.cpp",
        "base/multiple_io/multiple_io_base.cpp",
        "base/multiple_io/multiple_io_epoll.cpp",
        "base/multiple_io/multiple_io_poll.cpp",
        "base/multiple_io/multiple_io_select.cpp",
        "base/multiple_io/multiple_io_kqueue.cpp",
        f"base/wake_up/{platform}/wake_up_pipe.cpp",
        "comm/comm_port.cpp",
        "comm/sdk_protocol.cpp",
        "comm/generate_seq.cpp",
        "upgrade_manager.cpp",
        "upgrade/firmware.cpp",
        "upgrade/livox_lidar_upgrader.cpp",
        "logger_handler/logger_manager.cpp",
        "logger_handler/logger_handler.cpp",
        "logger_handler/file_manager.cpp",
        "data_handler/data_handler.cpp",
        "command_handler/command_impl.cpp",
        "command_handler/general_command_handler.cpp",
        "command_handler/hap_command_handler.cpp",
        "command_handler/mid360_command_handler.cpp",
        "command_handler/build_request.cpp",
        "command_handler/parse_lidar_state_info.cpp",
        "debug_point_cloud_handler/debug_point_cloud_manager.cpp",
        "debug_point_cloud_handler/debug_point_cloud_handler.cpp",
    ]
    files = [str(sdk_root / "3rdparty/FastCRC/FastCRCsw.cpp")]
    files.extend(str(sdk_core / rel_path) for rel_path in rel_paths)
    return files


def _build_sdk_query_tool(sdk_root: Path, verbose: bool = False) -> Path | None:
    build_dir = sdk_query_cache_dir() / re.sub(r"[^A-Za-z0-9_.-]+", "_", str(sdk_root))
    binary = build_dir / "livox_sdk_sn_query"
    if binary.is_file():
        return binary
    verbose_print(verbose, f"SDK SN query skipped: no cached helper at {binary}")
    return None


def _default_sdk_query_config(lidar_ip: str, iface_ip: str | None) -> dict:
    host_ip = iface_ip or "0.0.0.0"
    return {
        "lidar_summary_info": {
            "lidar_type": 8,
        },
        "MID360": {
            "lidar_net_info": {
                "cmd_data_port": 56100,
                "push_msg_port": 56200,
                "point_data_port": 56300,
                "imu_data_port": 56400,
                "log_data_port": 56500,
            },
            "host_net_info": {
                "cmd_data_ip": host_ip,
                "cmd_data_port": 56101,
                "push_msg_ip": host_ip,
                "push_msg_port": 56201,
                "point_data_ip": host_ip,
                "point_data_port": 56301,
                "imu_data_ip": host_ip,
                "imu_data_port": 56401,
                "log_data_ip": "",
                "log_data_port": 56501,
            },
        },
        "lidar_configs": [
            {
                "ip": lidar_ip,
                "pcl_data_type": 1,
                "pattern_mode": 0,
                "extrinsic_parameter": {
                    "roll": 0.0,
                    "pitch": 0.0,
                    "yaw": 0.0,
                    "x": 0,
                    "y": 0,
                    "z": 0,
                },
            }
        ],
    }


def _make_sdk_query_config(base_config: Path | None, lidar_ip: str, iface_ip: str | None) -> Path | None:
    if base_config and base_config.is_file():
        data = json.loads(base_config.read_text(encoding="utf-8"))
    else:
        data = _default_sdk_query_config(lidar_ip, iface_ip)
    lidar_configs = data.setdefault("lidar_configs", [{}])
    if not lidar_configs:
        lidar_configs.append({})
    lidar_configs[0]["ip"] = lidar_ip
    if iface_ip:
        mid360 = data.get("MID360")
        if isinstance(mid360, dict):
            host_net_info = mid360.get("host_net_info")
            if isinstance(host_net_info, dict):
                for key in (
                    "cmd_data_ip",
                    "push_msg_ip",
                    "point_data_ip",
                    "imu_data_ip",
                    "log_data_ip",
                ):
                    if key in host_net_info:
                        host_net_info[key] = iface_ip
    tmp = tempfile.NamedTemporaryFile(
        "w",
        prefix="livox_mid360_sdk_query_",
        suffix=".json",
        encoding="utf-8",
        delete=False,
    )
    with tmp:
        json.dump(data, tmp, indent=2, ensure_ascii=False)
        tmp.write("\n")
    return Path(tmp.name)


def query_sn_by_sdk(config_paths: list[Path], lidar_ip: str, iface_ip: str | None, timeout_sec: float, verbose: bool = False) -> str | None:
    sdk_roots = _sdk2_candidates()
    if not sdk_roots:
        verbose_print(verbose, "SDK SN query skipped: Livox-SDK2 not found")
        return None
    base_config = next((path for path in config_paths if path.is_file()), None)
    if base_config is None:
        verbose_print(verbose, "SDK SN query: MID360_config.json not found; using temporary minimal config")
    for sdk_root in sdk_roots:
        binary = _build_sdk_query_tool(sdk_root, verbose=verbose)
        if binary is None:
            continue
        query_config = _make_sdk_query_config(base_config, lidar_ip, iface_ip)
        if query_config is None:
            continue
        try:
            progress(f"querying SN with Livox SDK2 ({sdk_root})")
            output = run_text([str(binary), str(query_config), str(max(1.0, timeout_sec))], timeout=timeout_sec + 8)
            verbose_print(verbose, f"SDK SN query output from {sdk_root}: {output.strip() or 'N/A'}")
            match = re.search(r"^SDK_SN\s+([A-Z0-9]{10,16})\s*$", output, flags=re.MULTILINE)
            if match:
                return match.group(1)
        finally:
            try:
                query_config.unlink()
            except OSError:
                pass
    return None


def discover(ifaces: list[str], timeout_sec: float, sudo: bool, verbose: bool = False) -> tuple[str, Discovery]:
    if not ifaces:
        ifaces = ["eth0"]
    per_iface_timeout = timeout_sec if len(ifaces) == 1 else max(1.5, min(timeout_sec, 2.0))
    last_result = Discovery()
    progress(f"candidate interfaces: {', '.join(ifaces)}")
    for iface in ifaces:
        progress(f"checking interface {iface} by active scan")
        verbose_print(verbose, f"active scan iface={iface}")
        result = active_scan(iface, verbose=verbose)
        if result.lidar_ip:
            progress(f"found lidar by active scan on {iface}: {result.lidar_ip}")
            return iface, result
        last_result = result
    for iface in ifaces:
        progress(f"checking interface {iface} by passive discovery fallback")
        verbose_print(verbose, f"try iface={iface}")
        result = sniff(iface, per_iface_timeout, sudo=sudo, verbose=verbose)
        if result.lidar_ip:
            progress(f"found lidar by passive discovery on {iface}: {result.lidar_ip}")
            return iface, result
        last_result = result
    return ifaces[-1], last_result


def update_config(path: Path, lidar_ip: str, host_ip: str | None = None) -> None:
    data = json.loads(path.read_text(encoding="utf-8"))
    sdk2_host_infos = [
        data[key].get("host_net_info")
        for key in MID360_CONFIG_KEYS
        if isinstance(data.get(key), dict)
    ]
    if "lidar_configs" not in data and not any(isinstance(info, list) for info in sdk2_host_infos):
        data["lidar_configs"] = [{}]
    lidar_configs = data.get("lidar_configs")
    if isinstance(lidar_configs, list):
        if not lidar_configs:
            lidar_configs.append({})
        if not isinstance(lidar_configs[0], dict):
            lidar_configs[0] = {}
        lidar_configs[0]["ip"] = lidar_ip
    for key in MID360_CONFIG_KEYS:
        lidar_object = data.get(key)
        if not isinstance(lidar_object, dict):
            continue
        sdk2_host_info = lidar_object.get("host_net_info")
        if isinstance(sdk2_host_info, list):
            if not sdk2_host_info:
                sdk2_host_info.append({})
            if not isinstance(sdk2_host_info[0], dict):
                sdk2_host_info[0] = {}
            lidar_ips = sdk2_host_info[0].setdefault("lidar_ip", [])
            if not isinstance(lidar_ips, list):
                lidar_ips = []
                sdk2_host_info[0]["lidar_ip"] = lidar_ips
            if lidar_ips:
                lidar_ips[0] = lidar_ip
            else:
                lidar_ips.append(lidar_ip)
            if host_ip:
                sdk2_host_info[0]["host_ip"] = host_ip
            sdk2_host_info[0].pop("multicast_ip", None)
        elif isinstance(sdk2_host_info, dict) and host_ip:
            for ip_key in ("cmd_data_ip", "push_msg_ip", "point_data_ip", "imu_data_ip"):
                if ip_key in sdk2_host_info:
                    sdk2_host_info[ip_key] = host_ip

    path.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"updated: {path}")


def confirm_update(path: Path, configured_ip: str | None, lidar_ip: str, assume_yes: bool) -> bool:
    if assume_yes:
        return True
    if not sys.stdin.isatty():
        return False
    current = configured_ip or "N/A"
    answer = input(f"Update {path} lidar IP from {current} to {lidar_ip}? [y/N]: ").strip().lower()
    return answer in {"y", "yes"}


def read_config_lidar_ip(path: Path) -> str | None:
    if not path.is_file():
        return None
    data = json.loads(path.read_text(encoding="utf-8"))
    lidar_configs = data.get("lidar_configs")
    if isinstance(lidar_configs, list) and lidar_configs and isinstance(lidar_configs[0], dict):
        ip = lidar_configs[0].get("ip")
        if ip:
            return str(ip)
    for key in MID360_CONFIG_KEYS:
        lidar_object = data.get(key)
        host_net_info = lidar_object.get("host_net_info") if isinstance(lidar_object, dict) else None
        if isinstance(host_net_info, list) and host_net_info and isinstance(host_net_info[0], dict):
            lidar_ips = host_net_info[0].get("lidar_ip")
            if isinstance(lidar_ips, list) and lidar_ips:
                ip = lidar_ips[0]
                return str(ip) if ip else None
    return None


def parse_selection(answer: str, count: int) -> list[int]:
    answer = answer.replace(",", " ").replace(";", " ").strip()
    if not answer:
        return []
    if answer.lower() == "all" or answer == "*":
        return list(range(count))
    selected: set[int] = set()
    for token in answer.split():
        if "-" in token:
            start_raw, end_raw = token.split("-", 1)
            start = int(start_raw)
            end = int(end_raw)
            if start > end:
                raise ValueError(f"invalid selection range: {token}")
            values = range(start, end + 1)
        else:
            values = (int(token),)
        for value in values:
            if value < 1 or value > count:
                raise ValueError(f"selection out of range: {value}")
            selected.add(value - 1)
    return sorted(selected)


def make_config_view(config_states: list[ConfigState], show_all: bool) -> ConfigView:
    view = ConfigView(recommended=[], matched=[], other=[])
    has_normal_priority = any(not state.low_priority for state in config_states)
    for state in config_states:
        if state.low_priority and not show_all and has_normal_priority:
            view.hidden_count += 1
            continue
        if state.status == "mismatch":
            view.recommended.append(state)
        elif state.status == "match":
            view.matched.append(state)
        else:
            view.other.append(state)
    return view


def flatten_config_view(view: ConfigView) -> list[ConfigState]:
    return [*view.recommended, *view.matched, *view.other]


def print_config_entry(index: int, state: ConfigState, cursor: bool = False, checked: bool = False) -> None:
    pointer = cyan("> ") if cursor else "  "
    marker = green("x") if checked else " "
    print(f"{pointer}[{marker}] [{index}] {compact_path(state.path)}")
    low_priority = f"  {dim('sample/low-priority')}" if state.low_priority else ""
    print(f"      current={state.configured_ip or dim('N/A')}  status={status_label(state.status)}{low_priority}")


def print_config_section(
    title: str,
    states: list[ConfigState],
    start_index: int,
    cursor: int | None = None,
    selected: list[bool] | None = None,
) -> int:
    if not states:
        return start_index
    print(f"\n{bold(title)}")
    index = start_index
    for state in states:
        checked = bool(selected and state.original_index < len(selected) and selected[state.original_index])
        print_config_entry(index + 1, state, cursor=index == cursor, checked=checked)
        index += 1
    return index


def print_hidden_hint(hidden_count: int) -> None:
    if hidden_count:
        print(f"\n{dim(f'其它低优先级候选已折叠: {hidden_count} 个，按 a 显示全部。')}")


def print_config_table(config_states: list[ConfigState], show_all: bool = False) -> None:
    print(f"\n{bold('可更新的 MID360 配置文件')}")
    if not config_states:
        print("  未发现带有雷达 IP 的配置文件。")
        return
    view = make_config_view(config_states, show_all)
    index = 0
    index = print_config_section("推荐更新", view.recommended, index)
    index = print_config_section("已匹配", view.matched, index)
    index = print_config_section("其它候选", view.other, index)
    if index == 0:
        print("  当前可见列表为空。")
    print_hidden_hint(view.hidden_count)


def print_detection_summary(iface: str, result: Discovery) -> None:
    print(bold("检测结果"))
    print(f"  iface:           {cyan(iface)}")
    print(f"  iface_ip:        {result.iface_ip or 'N/A'}")
    print(f"  lidar_ip:        {green(result.lidar_ip) if result.lidar_ip else red('N/A')}")
    print(f"  broadcast_code:  {result.broadcast_code or 'N/A'}")
    print(f"  arp_host_ip:     {result.requested_host_ip or 'N/A'}")
    print(f"  discovery_pkts:  {result.raw_packets}")
    print(f"  detect_method:   {result.method or 'N/A'}")


def detection_rows(iface: str, result: Discovery) -> list[str]:
    return [
        "IFACE        " + neon.text(iface or "N/A", neon.ACCENT, True),
        "IFACE_IP     " + (result.iface_ip or "N/A"),
        "LIDAR_IP     " + neon.text(result.lidar_ip or "N/A", neon.SUCCESS if result.lidar_ip else neon.DANGER, True),
        "BROADCAST    " + (result.broadcast_code or "N/A"),
        "ARP_HOST     " + (result.requested_host_ip or "N/A"),
        "PACKETS      " + str(result.raw_packets),
        "METHOD       " + (result.method or "N/A"),
        "",
        neon.badge("HEALTH", "FOUND" if result.lidar_ip else "WAIT", neon.SUCCESS if result.lidar_ip else neon.WARNING),
        neon.badge("MODE", "AUTOCONFIG", neon.ACCENT),
    ]


def status_plain(status: str) -> str:
    if status == "mismatch":
        return "UPDATE"
    if status == "match":
        return "MATCH"
    if status == "unavailable":
        return "NO-IP"
    return status or "N/A"


def status_color(status: str) -> str:
    if status == "mismatch":
        return neon.WARNING
    if status == "match":
        return neon.SUCCESS
    if status == "unavailable":
        return neon.MUTED
    return neon.TEXT


def visible_group_labels(view: ConfigView) -> list[str]:
    return ["推荐更新"] * len(view.recommended) + ["已匹配"] * len(view.matched) + ["其它候选"] * len(view.other)


def render_candidate_rows(
    visible_states: list[ConfigState],
    labels: list[str],
    selected: list[bool],
    cursor: int,
    first: int,
    max_rows: int,
    width: int,
) -> list[str]:
    rows = ["上下方向键移动，空格选择/取消，回车确认，a 显示/隐藏低优先级候选，q 或 Esc 退出。", ""]
    path_width = max(10, width - 39)
    for index in range(first, len(visible_states)):
        if len(rows) >= max_rows + 2:
            break
        state = visible_states[index]
        checked = state.original_index < len(selected) and selected[state.original_index]
        marker = neon.text("▶", neon.ACCENT, True) if index == cursor else " "
        check = neon.text("[x]", neon.SUCCESS, True) if checked else "[ ]"
        group = neon.text(neon.pad_right(labels[index] if index < len(labels) else "候选", 8), neon.MUTED, True)
        status = neon.text(neon.pad_right(status_plain(state.status), 7), status_color(state.status), True)
        rows.append(
            f"{marker} {check} {neon.pad_left(str(index + 1), 2)} {group} {status} "
            f"{neon.fit(compact_path(state.path, path_width), path_width)}"
        )
    if not visible_states:
        rows.append(neon.text("当前可见列表为空。", neon.WARNING, True))
    return rows


def render_config_picker(
    config_states: list[ConfigState],
    iface: str,
    result: Discovery,
    show_all: bool,
    cursor: int,
    selected: list[bool],
) -> str:
    view = make_config_view(config_states, show_all)
    visible_states = flatten_config_view(view)
    labels = visible_group_labels(view)
    rows, cols = neon.terminal_size()
    width = max(48, cols)
    out = [neon.header("LIVOX MID-360 AUTOCONFIG", "CONFIG", width)]
    used_rows = 3
    if rows < 16 or cols < 58:
        used_rows += neon.append_line(out, neon.text("Terminal too small for Autoconfig TUI", neon.WARNING, True), width)
        used_rows += neon.append_line(out, "Resize to at least 58x16.", width)
        neon.append_footer_at_bottom(out, "[ENTER] APPLY   [Q/ESC] QUIT", rows, width, used_rows)
        return "".join(out)

    selected_count = sum(1 for item in selected if item)
    selection_line = f"VISIBLE {len(visible_states)} / TOTAL {len(config_states)} / SELECTED {selected_count}"
    hidden_line = "LOW PRIORITY shown" if view.hidden_count == 0 else f"LOW PRIORITY folded: {view.hidden_count} (press a)"

    if cols >= 104:
        left_w = min(44, max(33, cols // 3))
        right_w = max(58, cols - left_w - 1)
        max_candidate_rows = max(1, rows - used_rows - 6)
        first = max(0, cursor - max_candidate_rows + 1) if visible_states and cursor >= max_candidate_rows else 0
        left_rows = [*detection_rows(iface, result), "", selection_line, hidden_line]
        right_rows = render_candidate_rows(visible_states, labels, selected, cursor, first, max_candidate_rows, right_w)
        used_rows += neon.append_lines(
            out,
            neon.hstack(neon.box("DEVICE IDENTITY", left_rows, left_w), neon.box("CONFIG CANDIDATES", right_rows, right_w), left_w, right_w),
            width,
            rows - used_rows - 2,
        )
    else:
        top_rows = detection_rows(iface, result)[: min(7, max(3, rows // 4))]
        top_rows.extend([selection_line, hidden_line])
        identity_lines = neon.box("DEVICE IDENTITY", top_rows, width)
        if rows - used_rows - 2 >= len(identity_lines) + 5:
            used_rows += neon.append_lines(out, identity_lines, width, rows - used_rows - 2)
        max_candidate_rows = max(1, rows - used_rows - 6)
        first = max(0, cursor - max_candidate_rows + 1) if visible_states and cursor >= max_candidate_rows else 0
        right_rows = render_candidate_rows(visible_states, labels, selected, cursor, first, max_candidate_rows, width)
        used_rows += neon.append_lines(out, neon.box("CONFIG CANDIDATES", right_rows, width), width, rows - used_rows - 2)

    if visible_states and used_rows < rows - 1:
        used_rows += neon.append_line(out, neon.text("PATH ", neon.MUTED, True) + neon.fit(str(visible_states[cursor].path), max(8, width - 5)), width)
    neon.append_footer_at_bottom(out, "[↑/↓] MOVE   [SPACE] SELECT   [A] LOW-PRIORITY   [ENTER] APPLY   [Q/ESC] QUIT", rows, width, used_rows)
    return "".join(out)


def enter_screen() -> None:
    global SCREEN_ACTIVE, ORIGINAL_TERMIOS
    if not sys.stdout.isatty():
        return
    if sys.stdin.isatty() and ORIGINAL_TERMIOS is None:
        ORIGINAL_TERMIOS = termios.tcgetattr(sys.stdin.fileno())
        tty.setcbreak(sys.stdin.fileno())
    print(neon.enter_alt_screen(), end="")
    sys.stdout.flush()
    SCREEN_ACTIVE = True
    SCAN_RENDERER.reset()


def leave_screen() -> None:
    global SCREEN_ACTIVE, ORIGINAL_TERMIOS
    if not SCREEN_ACTIVE:
        return
    SCREEN_ACTIVE = False
    if ORIGINAL_TERMIOS is not None:
        termios.tcsetattr(sys.stdin.fileno(), termios.TCSANOW, ORIGINAL_TERMIOS)
        ORIGINAL_TERMIOS = None
    print(neon.leave_alt_screen(), end="")
    sys.stdout.flush()
    SCAN_RENDERER.reset()


def choose_configs_interactively(
    config_states: list[ConfigState],
    iface: str,
    result: Discovery,
    initial_show_all: bool = False,
) -> list[int]:
    if not config_states:
        if SCREEN_ACTIVE:
            rows, cols = neon.terminal_size()
            print(neon.clear_screen(), end="")
            print(neon.header("LIVOX MID-360 AUTOCONFIG", "CONFIG", max(40, cols)), end="")
            panel_rows = [*detection_rows(iface, result), "", neon.text("未发现带有雷达 IP 的配置文件。", neon.WARNING, True)]
            for line in neon.box("DEVICE IDENTITY", panel_rows, max(40, cols)):
                print(line)
            print(neon.footer("[Q/ESC] QUIT", max(40, cols)))
        else:
            print_detection_summary(iface, result)
            print_config_table(config_states, initial_show_all)
        return []

    if not sys.stdin.isatty() or not sys.stdout.isatty():
        print_detection_summary(iface, result)
        print_config_table(config_states, initial_show_all)
        return []

    selected = [False] * (max((state.original_index for state in config_states), default=-1) + 1)
    show_all = initial_show_all
    visible_states = flatten_config_view(make_config_view(config_states, show_all))
    cursor = 0
    renderer = neon.LineDiffRenderer()

    def redraw() -> None:
        nonlocal cursor, visible_states
        visible_states = flatten_config_view(make_config_view(config_states, show_all))
        if not visible_states:
            cursor = 0
        elif cursor >= len(visible_states):
            cursor = len(visible_states) - 1
        rows, cols = neon.terminal_size()
        renderer.render(render_config_picker(config_states, iface, result, show_all, cursor, selected), rows, max(48, cols))

    original = termios.tcgetattr(sys.stdin.fileno())
    cancelled = False
    frame_clock = neon.FrameClock(1.0)
    try:
        tty.setcbreak(sys.stdin.fileno())
        redraw()
        frame_clock.mark_rendered()
        while True:
            key = neon.read_key(frame_clock.wait_timeout(0.02), 0.02)
            if key == neon.KEY_NONE:
                if frame_clock.consume_redraw(True):
                    redraw()
                    frame_clock.mark_rendered()
                continue
            if key == neon.KEY_UP:
                if visible_states:
                    cursor = len(visible_states) - 1 if cursor == 0 else cursor - 1
            elif key == neon.KEY_DOWN:
                if visible_states:
                    cursor = (cursor + 1) % len(visible_states)
            elif key == neon.KEY_SPACE:
                if visible_states:
                    original_index = visible_states[cursor].original_index
                    if original_index < len(selected):
                        selected[original_index] = not selected[original_index]
            elif key == neon.KEY_ENTER:
                break
            elif key == neon.KEY_TOGGLE_ALL:
                show_all = not show_all
            elif key in {neon.KEY_QUIT, neon.KEY_ESCAPE}:
                cancelled = True
                break
            redraw()
            frame_clock.mark_rendered()
    finally:
        if ORIGINAL_TERMIOS is None:
            termios.tcsetattr(sys.stdin.fileno(), termios.TCSANOW, original)
        print(neon.clear_screen(), end="")

    if cancelled:
        return []
    return [index for index, checked in enumerate(selected) if checked]


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Auto-discover Livox lidar IP/SN from eth discovery packets.",
    )
    parser.add_argument(
        "-i",
        "--iface",
        default="auto",
        help="Ethernet interface to sniff, or 'auto' to choose by MID360_config.json lidar IP",
    )
    parser.add_argument(
        "-t",
        "--timeout",
        type=float,
        default=8.0,
        help="passive discovery fallback timeout seconds; auto mode caps each interface to 2s",
    )
    parser.add_argument(
        "--sn-timeout",
        type=float,
        default=2.0,
        help="seconds to spend on optional post-scan packet sniffing for SN",
    )
    parser.add_argument("--no-log-sn", action="store_true", help="do not try to fill SN from recent ROS logs")
    parser.add_argument("--no-sniff-sn", action="store_true", help="do not try optional post-scan packet sniffing for SN")
    parser.add_argument("-v", "--verbose", action="store_true", help="print detailed sniff/parse diagnostics")
    parser.add_argument("--no-sudo", action="store_true", help="do not prefix tcpdump with sudo")
    parser.add_argument(
        "--config",
        action="append",
        default=None,
        help=(
            "MID360_config.json path to check/update; can be specified multiple times. "
            f"Default: {default_config_path()}"
        ),
    )
    parser.add_argument("--apply", action="store_true", help="update config files without the interactive picker")
    parser.add_argument("--yes", action="store_true", help="update config without interactive confirmation when used with --apply")
    parser.add_argument(
        "--sdk-sn",
        action="store_true",
        help="optionally query SN with an existing cached Livox SDK helper; no helper is built automatically",
    )
    parser.add_argument(
        "--no-sdk-sn",
        action="store_true",
        help="deprecated compatibility option; SDK SN query is disabled by default",
    )
    parser.add_argument(
        "--sdk-timeout",
        type=float,
        default=4.0,
        help="seconds to wait for Livox SDK2 SN callback when SDK fallback is enabled",
    )
    parser.add_argument(
        "--require-match",
        action="store_true",
        help="return non-zero when detected lidar IP differs from configured lidar IP",
    )
    parser.add_argument("--show-all", action="store_true", help="include sample/build/dist config candidates in the picker")
    parser.add_argument("--no-color", action="store_true", help="disable ANSI colors")
    args = parser.parse_args()
    init_theme(args.no_color)
    neon.set_color_enabled(not args.no_color)
    interactive_picker = not args.apply and sys.stdin.isatty() and sys.stdout.isatty()
    if interactive_picker:
        enter_screen()
        render_scan_screen("preparing config search")
    config_paths = resolve_config_paths(args.config, verbose=args.verbose)
    ifaces = candidate_ifaces(args.iface, config_paths)
    progress("starting MID360 discovery")
    if args.verbose:
        verbose_print(True, f"candidate ifaces: {ifaces}")

    iface, result = discover(ifaces, args.timeout, sudo=not args.no_sudo, verbose=args.verbose)
    if result.lidar_ip and not result.broadcast_code and not args.no_log_sn:
        log_sn = recent_log_sn(result.lidar_ip, verbose=args.verbose)
        if log_sn:
            result.broadcast_code = log_sn
            result.method = append_method(result.method, "log_sn")

    if result.lidar_ip and not result.broadcast_code and not args.no_sniff_sn:
        try:
            sniff_result = sniff(iface, args.sn_timeout, sudo=not args.no_sudo, verbose=args.verbose)
            same_lidar = not sniff_result.lidar_ip or sniff_result.lidar_ip == result.lidar_ip
            if sniff_result.broadcast_code and same_lidar:
                result.broadcast_code = sniff_result.broadcast_code
                result.method = append_method(result.method, "sniff_sn")
            if sniff_result.requested_host_ip and not result.requested_host_ip:
                result.requested_host_ip = sniff_result.requested_host_ip
            result.raw_packets += sniff_result.raw_packets
        except RuntimeError as exc:
            verbose_print(args.verbose, f"optional SN sniff skipped: {exc}")

    if result.lidar_ip and not result.broadcast_code and args.sdk_sn and not args.no_sdk_sn:
        sdk_sn = query_sn_by_sdk(
            config_paths,
            result.lidar_ip,
            result.iface_ip,
            timeout_sec=args.sdk_timeout,
            verbose=args.verbose,
        )
        if sdk_sn:
            result.broadcast_code = sdk_sn
            if result.method:
                result.method = f"{result.method}+sdk_sn"
            else:
                result.method = "sdk_sn"
    if not interactive_picker:
        print_detection_summary(iface, result)

    if not result.lidar_ip:
        leave_screen()
        print("ERROR: no MID360 lidar IP found by passive discovery or active scan", file=sys.stderr)
        return 2

    needs_update = False
    unavailable = False
    config_states: list[ConfigState] = []
    for index, path in enumerate(config_paths):
        configured_ip = read_config_lidar_ip(path)
        status = ""
        if configured_ip and configured_ip != result.lidar_ip:
            needs_update = True
            status = "mismatch"
        elif configured_ip == result.lidar_ip:
            status = "match"
        else:
            unavailable = True
            status = "unavailable"
        config_states.append(
            ConfigState(
                path=path,
                configured_ip=configured_ip,
                status=status,
                low_priority=is_low_priority_config_path(path),
                original_index=index,
            )
        )

    updated_any = False
    if args.apply:
        for state in config_states:
            if not state.path.is_file():
                print(f"ERROR: config file not found: {state.path}", file=sys.stderr)
                return 2
            if state.configured_ip == result.lidar_ip:
                continue
            should_update = args.yes or not sys.stdin.isatty()
            if not should_update:
                should_update = confirm_update(state.path, state.configured_ip, result.lidar_ip, False)
            if should_update:
                update_config(state.path, result.lidar_ip, result.iface_ip)
                updated_any = True
            else:
                print(f"skipped: {state.path}")
    elif sys.stdin.isatty():
        visible_config_states = [state for state in config_states if state.configured_ip] or config_states
        selected = choose_configs_interactively(visible_config_states, iface, result, args.show_all)
        if not selected:
            leave_screen()
            print("未选择配置文件，退出不修改。")
        for index in selected:
            state = next((item for item in config_states if item.original_index == index), None)
            if state is None:
                leave_screen()
                print(f"ERROR: invalid config selection index: {index}", file=sys.stderr)
                return 2
            if not state.path.is_file():
                leave_screen()
                print(f"ERROR: config file not found: {state.path}", file=sys.stderr)
                return 2
            update_config(state.path, result.lidar_ip, result.iface_ip)
            leave_screen()
            updated_any = True
    else:
        visible_config_states = [state for state in config_states if state.configured_ip]
        print_config_table(visible_config_states, args.show_all)
        print("非交互式终端不会修改配置；需要自动写入时请显式使用 --config PATH --apply --yes。")

    if args.require_match and (needs_update or unavailable) and not updated_any:
        leave_screen()
        print(
            "ERROR: detected lidar IP cannot be verified against selected MID360 config",
            file=sys.stderr,
        )
        return 3
    leave_screen()
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        leave_screen()
        print("Interrupted.", file=sys.stderr)
        raise SystemExit(130)
