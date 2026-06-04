from __future__ import annotations

import sys
import termios
import tty

from . import autoconfig, neon_tui as neon, udp_monitor


MENU_ITEMS = [
    ("autoconfig", "CONFIG", "发现雷达并选择要更新的配置文件"),
    ("udp-monitor", "UDP", "被动监听配置里的 UDP 端口"),
    ("quit", "EXIT", "退出诊断入口"),
]


def usage() -> None:
    print("usage: livox-mid360-diagnostics <autoconfig|udp-monitor> [args...]")
    print()
    print("commands:")
    print("  autoconfig   discover MID360 IP/SN and optionally update MID360_config.json")
    print("  udp-monitor  passively listen on configured UDP ports and print packet/point rates")


def render_menu(cursor: int) -> str:
    rows, cols = neon.terminal_size()
    width = max(20, cols - 1)
    used_rows = 3
    out = [neon.header("LIVOX MID-360 DIAGNOSTICS", "PYTHON", width)]
    if rows < 14 or cols < 58:
        used_rows += neon.append_line(out, neon.text("Terminal too small for Neon Protocol TUI", neon.WARNING, True), width)
        used_rows += neon.append_line(out, "Resize to at least 58x14.", width)
        neon.append_footer_at_bottom(out, "[ENTER] SELECT   [Q/ESC] QUIT", rows, width, used_rows)
        return "".join(out)

    identity = [
        "PROFILE      " + neon.text("Neon Protocol", neon.SUCCESS, True),
        "RUNTIME      Python CLI",
        "CONTROL      passive tools only",
        "",
        neon.badge("HEALTH", "READY", neon.SUCCESS),
        neon.badge("MODE", "MENU", neon.ACCENT),
        neon.badge("UI", "ADAPTIVE", neon.SUCCESS),
    ]
    commands = ["上下方向键移动，回车确认，q 或 Esc 退出。", ""]
    for index, (command, badge, description) in enumerate(MENU_ITEMS):
        marker = neon.text("▶", neon.ACCENT, True) if index == cursor else " "
        command_text = neon.text(neon.pad_right(command, 12), neon.ACCENT if index == cursor else neon.TEXT, index == cursor)
        badge_text = neon.text(neon.pad_right(badge, 8), neon.SUCCESS if index == cursor else neon.MUTED, True)
        commands.append(f"{marker} {command_text} {badge_text} {description}")

    if width >= 96:
        left_w = min(42, max(31, width // 3))
        right_w = max(40, width - left_w - 1)
        for line in neon.hstack(neon.box("DEVICE IDENTITY", identity, left_w), neon.box("COMMAND ROUTER", commands, right_w), left_w, right_w):
            used_rows += neon.append_line(out, line, width)
    else:
        for line in neon.box("DEVICE IDENTITY", identity, width):
            used_rows += neon.append_line(out, line, width)
        for line in neon.box("COMMAND ROUTER", commands, width):
            used_rows += neon.append_line(out, line, width)
    neon.append_footer_at_bottom(out, "[↑/↓] MOVE   [ENTER] SELECT   [Q/ESC] QUIT", rows, width, used_rows)
    return "".join(out)


def choose_command_menu() -> str:
    if not sys.stdin.isatty() or not sys.stdout.isatty():
        usage()
        return "quit"

    original = termios.tcgetattr(sys.stdin.fileno())
    cursor = 0
    frame_clock = neon.FrameClock(1.0)
    renderer = neon.LineDiffRenderer()

    def redraw() -> None:
        rows, cols = neon.terminal_size()
        renderer.render(render_menu(cursor), rows, max(20, cols - 1))
        frame_clock.mark_rendered()

    try:
        tty.setcbreak(sys.stdin.fileno())
        print(neon.enter_alt_screen(), end="", flush=True)
        redraw()
        while True:
            key = neon.read_key(frame_clock.wait_timeout(0.02), 0.02)
            if key == neon.KEY_NONE:
                if frame_clock.consume_redraw(True):
                    redraw()
                continue
            if key == neon.KEY_UP:
                cursor = len(MENU_ITEMS) - 1 if cursor == 0 else cursor - 1
            elif key == neon.KEY_DOWN:
                cursor = (cursor + 1) % len(MENU_ITEMS)
            elif key == neon.KEY_ENTER:
                return MENU_ITEMS[cursor][0]
            elif key in {neon.KEY_QUIT, neon.KEY_ESCAPE}:
                return "quit"
            redraw()
    finally:
        termios.tcsetattr(sys.stdin.fileno(), termios.TCSANOW, original)
        print(neon.leave_alt_screen(), end="", flush=True)


def main() -> int:
    if len(sys.argv) < 2 or sys.argv[1] in {"-h", "--help"}:
        if len(sys.argv) >= 2:
            usage()
            return 0
        command = choose_command_menu()
        if command == "quit":
            return 0 if sys.stdin.isatty() and sys.stdout.isatty() else 2
        sys.argv = [sys.argv[0]]
    else:
        command = sys.argv[1]
        sys.argv = [sys.argv[0], *sys.argv[2:]]

    try:
        if command in {"autoconfig", "config"}:
            return autoconfig.main()
        if command in {"udp-monitor", "monitor"}:
            return udp_monitor.main()
    except KeyboardInterrupt:
        if command in {"autoconfig", "config"}:
            autoconfig.leave_screen()
        print("Interrupted.", file=sys.stderr)
        return 130

    print(f"unknown command: {command}", file=sys.stderr)
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
