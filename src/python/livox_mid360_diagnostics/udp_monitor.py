#!/usr/bin/env python3
"""Monitor Livox MID360 UDP point/IMU packets without ROS.

This tool does not discover or configure a lidar. It only binds local UDP ports
from a MID360_config.json file and prints packet/point rates when packets arrive.
"""

from __future__ import annotations

import argparse
import json
import os
import select
import socket
import struct
import sys
import termios
import time
import tty
from dataclasses import dataclass
from pathlib import Path

try:
    from . import neon_tui as neon
except ImportError:  # pragma: no cover - supports direct script execution.
    import neon_tui as neon


CONFIG_ENV_VARS = ("LIVOX_MID360_CONFIG", "MID360_CONFIG")
PACKET_HEADER = struct.Struct("<BHHHHBBB12sI8s")
PACKET_HEADER_SIZE = PACKET_HEADER.size
POINT_SIZES = {
    0: 24,  # LivoxLidarImuRawPoint: 6 floats
    1: 14,  # high precision cartesian point
    2: 8,   # low precision cartesian point
    3: 10,  # spherical point
}
DATA_TYPE_NAMES = {
    0: "imu",
    1: "cartesian_high",
    2: "cartesian_low",
    3: "spherical",
}


def expand_path(raw_path: str | os.PathLike[str]) -> Path:
    return Path(os.path.expandvars(str(raw_path))).expanduser()


def split_env_paths(value: str) -> list[Path]:
    return [expand_path(item) for item in value.split(os.pathsep) if item.strip()]


def env_config_path() -> str | None:
    for name in CONFIG_ENV_VARS:
        value = os.environ.get(name)
        if value:
            return str(split_env_paths(value)[0])
    local_config = Path(__file__).resolve().parents[2] / "config" / "MID360_config.local.json"
    if local_config.is_file():
        return str(local_config)
    return None


@dataclass
class StreamStats:
    name: str
    port: int
    packets: int = 0
    units: int = 0
    bytes: int = 0
    last_data_type: int | None = None
    last_frame: int | None = None
    last_udp_count: int | None = None
    total_packets: int = 0
    total_units: int = 0
    total_bytes: int = 0


def load_ports(config_path: Path) -> dict[str, int]:
    data = json.loads(config_path.read_text(encoding="utf-8"))
    host_net_info = data.get("MID360", {}).get("host_net_info", {})
    if isinstance(host_net_info, list):
        host_net_info = host_net_info[0] if host_net_info and isinstance(host_net_info[0], dict) else {}
    if not isinstance(host_net_info, dict):
        raise ValueError(f"invalid MID360.host_net_info in {config_path}")

    ports: dict[str, int] = {}
    for stream, key in (("point", "point_data_port"), ("imu", "imu_data_port"), ("push", "push_msg_port")):
        value = host_net_info.get(key)
        if isinstance(value, int) and value > 0:
            ports[stream] = value
    if not ports:
        raise ValueError(f"no usable host UDP ports found in {config_path}")
    return ports


def bind_socket(host: str, port: int, receive_buffer: int) -> socket.socket:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, receive_buffer)
    sock.bind((host, port))
    sock.setblocking(False)
    return sock


def parse_packet(payload: bytes) -> tuple[int | None, int | None, int | None, int | None]:
    if len(payload) < PACKET_HEADER_SIZE:
        return None, None, None, None
    fields = PACKET_HEADER.unpack_from(payload)
    dot_num = fields[3]
    udp_count = fields[4]
    frame_count = fields[5]
    data_type = fields[6]
    if dot_num == 0:
        point_size = POINT_SIZES.get(data_type)
        if point_size:
            dot_num = max(0, (len(payload) - PACKET_HEADER_SIZE) // point_size)
    return data_type, dot_num, frame_count, udp_count


def format_report(stats: dict[int, StreamStats], elapsed: float, interval: float) -> str:
    parts = []
    for item in stats.values():
        pps = item.packets / interval
        ups = item.units / interval
        mbps = item.bytes * 8.0 / interval / 1_000_000.0
        dtype = "N/A"
        if item.last_data_type is not None:
            dtype = DATA_TYPE_NAMES.get(item.last_data_type, str(item.last_data_type))
        unit_name = "samples/s" if item.name == "imu" else "points/s"
        parts.append(
            f"{item.name}: port={item.port} packets={item.packets} "
            f"rate={pps:.1f} pkt/s {unit_name}={ups:.0f} Mbps={mbps:.2f} "
            f"type={dtype} frame={item.last_frame if item.last_frame is not None else 'N/A'} "
            f"udp={item.last_udp_count if item.last_udp_count is not None else 'N/A'}"
        )
        item.packets = 0
        item.units = 0
        item.bytes = 0
    timestamp = time.strftime("%H:%M:%S")
    return f"[{timestamp}] elapsed={elapsed:.1f}s " + " | ".join(parts)


def stream_rate_rows(stats: dict[int, StreamStats], interval: float) -> list[str]:
    rows: list[str] = []
    for item in stats.values():
        pps = item.packets / max(0.001, interval)
        ups = item.units / max(0.001, interval)
        mbps = item.bytes * 8.0 / max(0.001, interval) / 1_000_000.0
        dtype = "N/A" if item.last_data_type is None else DATA_TYPE_NAMES.get(item.last_data_type, str(item.last_data_type))
        unit = "sample/s" if item.name == "imu" else "pt/s"
        color = neon.SUCCESS if item.name == "imu" else neon.ACCENT
        rows.append(
            f"{neon.pad_right(item.name.upper(), 8)} udp/{item.port:<5} "
            f"{neon.text(neon.compact_rate(ups, unit), color, True)} "
            f"pkts={pps:.1f}/s mbps={mbps:.2f} type={dtype} udp={item.last_udp_count if item.last_udp_count is not None else 'N/A'}"
        )
    return rows


def render_dashboard(stats: dict[int, StreamStats], config_path: Path, elapsed: float, interval: float, args: argparse.Namespace) -> str:
    rows, cols = neon.terminal_size()
    width = max(48, cols)
    out = [neon.clear_screen(), neon.header("LIVOX MID-360 UDP", "MONITOR", width)]
    used_rows = 3
    if rows < 14 or cols < 62:
        used_rows += neon.append_line(out, neon.text("Terminal too small for UDP Monitor TUI", neon.WARNING, True), width)
        used_rows += neon.append_line(out, "Resize to at least 62x14.", width)
        neon.append_footer_at_bottom(out, "[CTRL+C/Q] STOP", rows, width, used_rows)
        return "".join(out)

    identity = [
        "CONFIG       " + neon.fit(str(config_path), 48),
        "BIND         " + args.bind,
        "UPTIME       " + neon.text(neon.duration(elapsed), neon.ACCENT, True),
        "",
        neon.badge("HEALTH", "LISTENING", neon.SUCCESS),
        neon.badge("MODE", "UDP", neon.ACCENT),
        neon.badge("UI", "ADAPTIVE", neon.SUCCESS),
    ]
    total_packets = sum(item.total_packets for item in stats.values())
    total_units = sum(item.total_units for item in stats.values())
    total_mbps = sum(item.bytes for item in stats.values()) * 8.0 / max(0.001, interval) / 1_000_000.0
    stream_rows = [
        "TOTAL_PKTS   " + neon.text(str(total_packets), neon.SUCCESS, True),
        "TOTAL_UNITS  " + neon.text(str(total_units), neon.ACCENT, True),
        "TRAFFIC      " + f"{total_mbps:.2f} Mbps",
        "LOAD         " + neon.text(neon.bar(total_mbps, 100.0, max(8, width - 20), True), neon.ACCENT, True),
        "",
        *stream_rate_rows(stats, interval),
    ]
    if cols >= 104:
        left_w = min(44, max(33, cols // 3))
        right_w = max(58, cols - left_w - 1)
        used_rows += neon.append_lines(
            out,
            neon.hstack(neon.box("DEVICE IDENTITY", identity, left_w), neon.box("UDP STREAMS", stream_rows, right_w), left_w, right_w),
            width,
            rows - used_rows - 1,
        )
    else:
        identity_lines = neon.box("DEVICE IDENTITY", identity, width)
        stream_lines = neon.box("UDP STREAMS", stream_rows, width)
        if rows - used_rows - 1 >= len(identity_lines) + 3:
            used_rows += neon.append_lines(out, identity_lines, width, rows - used_rows - 1)
        used_rows += neon.append_lines(out, stream_lines, width, rows - used_rows - 1)
    neon.append_footer_at_bottom(out, "[CTRL+C/Q] STOP", rows, width, used_rows)
    return "".join(out)


def poll_stdin_quit() -> bool:
    readable, _, _ = select.select([sys.stdin], [], [], 0)
    if not readable:
        return False
    ch = sys.stdin.read(1)
    return ch in {"q", "Q", "\x03"}


def monitor(args: argparse.Namespace) -> int:
    config_path = expand_path(args.config)
    ports = load_ports(config_path)
    if args.point_port is not None:
        ports["point"] = args.point_port
    if args.imu_port is not None:
        ports["imu"] = args.imu_port
    if args.no_push:
        ports.pop("push", None)

    sockets: dict[socket.socket, StreamStats] = {}
    for name, port in ports.items():
        sock = bind_socket(args.bind, port, args.receive_buffer)
        sockets[sock] = StreamStats(name=name, port=port)

    interactive = sys.stdin.isatty() and sys.stdout.isatty()
    original_termios = None
    if interactive:
        original_termios = termios.tcgetattr(sys.stdin.fileno())
        tty.setcbreak(sys.stdin.fileno())
        print(neon.enter_alt_screen(), end="", flush=True)
    else:
        print(f"config: {config_path}")
        print(f"listening: {', '.join(f'{item.name}=udp/{item.port}' for item in sockets.values())}")
        print("Press Ctrl-C to stop.")

    started = time.monotonic()
    last_report = started
    deadline = started + args.duration if args.duration > 0 else None
    try:
        if interactive:
            print(render_dashboard({id(sock): stat for sock, stat in sockets.items()}, config_path, 0.0, args.interval, args), end="", flush=True)
        while True:
            now = time.monotonic()
            if deadline is not None and now >= deadline:
                break

            timeout = min(0.25, max(0.0, args.interval - (now - last_report)))
            readable, _, _ = select.select(list(sockets.keys()), [], [], timeout)
            for sock in readable:
                try:
                    payload, _addr = sock.recvfrom(args.max_packet)
                except BlockingIOError:
                    continue
                stat = sockets[sock]
                stat.packets += 1
                stat.bytes += len(payload)
                stat.total_packets += 1
                stat.total_bytes += len(payload)
                if stat.name in {"point", "imu"}:
                    data_type, units, frame_count, udp_count = parse_packet(payload)
                    stat.last_data_type = data_type
                    stat.last_frame = frame_count
                    stat.last_udp_count = udp_count
                    stat.units += units or 0
                    stat.total_units += units or 0

            if interactive and poll_stdin_quit():
                break

            now = time.monotonic()
            if now - last_report >= args.interval:
                interval = now - last_report
                if interactive:
                    print(render_dashboard({id(sock): stat for sock, stat in sockets.items()}, config_path, now - started, interval, args), end="", flush=True)
                    for stat in sockets.values():
                        stat.packets = 0
                        stat.units = 0
                        stat.bytes = 0
                else:
                    print(format_report({id(sock): stat for sock, stat in sockets.items()}, now - started, interval), flush=True)
                last_report = now
    finally:
        if interactive:
            if original_termios is not None:
                termios.tcsetattr(sys.stdin.fileno(), termios.TCSANOW, original_termios)
            print(neon.leave_alt_screen(), end="", flush=True)
    print("stopped")
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Monitor live MID360 UDP point/IMU packet rates.")
    parser.add_argument(
        "--config",
        default=env_config_path(),
        required=env_config_path() is None,
        help="MID360_config.json path; can also be set with LIVOX_MID360_CONFIG",
    )
    parser.add_argument("--bind", default="0.0.0.0", help="local address to bind")
    parser.add_argument("--point-port", type=int, default=None, help="override point_data_port")
    parser.add_argument("--imu-port", type=int, default=None, help="override imu_data_port")
    parser.add_argument("--no-push", action="store_true", help="do not bind push_msg_port")
    parser.add_argument("--interval", type=float, default=1.0, help="report interval seconds")
    parser.add_argument("--duration", type=float, default=0.0, help="stop after N seconds; 0 means forever")
    parser.add_argument("--receive-buffer", type=int, default=4 * 1024 * 1024, help="UDP receive buffer bytes")
    parser.add_argument("--max-packet", type=int, default=65535, help="maximum UDP datagram bytes to read")
    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    try:
        return monitor(args)
    except KeyboardInterrupt:
        print("\nstopped")
        return 130
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
