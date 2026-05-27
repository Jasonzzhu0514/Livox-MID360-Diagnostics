from __future__ import annotations

import sys

from . import autoconfig, udp_monitor


def main() -> int:
    if len(sys.argv) < 2 or sys.argv[1] in {"-h", "--help"}:
        print("usage: livox-mid360-diagnostics <autoconfig|udp-monitor> [args...]")
        print()
        print("commands:")
        print("  autoconfig   discover MID360 IP/SN and optionally update MID360_config.json")
        print("  udp-monitor  passively listen on configured UDP ports and print packet/point rates")
        return 0 if len(sys.argv) >= 2 else 2

    command = sys.argv[1]
    sys.argv = [sys.argv[0], *sys.argv[2:]]
    if command in {"autoconfig", "config"}:
        return autoconfig.main()
    if command in {"udp-monitor", "monitor"}:
        return udp_monitor.main()

    print(f"unknown command: {command}", file=sys.stderr)
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
