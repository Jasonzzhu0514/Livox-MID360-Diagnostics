"""Small ANSI Neon Protocol helpers for interactive terminal views."""

from __future__ import annotations

import os
import select
import shutil
import sys
import time
import unicodedata
import fcntl
import struct
import termios


ACCENT = "51"
SUCCESS = "84"
WARNING = "220"
DANGER = "196"
MUTED = "242"
TEXT = "252"
BG = "233"

COLOR_ENABLED = True

KEY_NONE = "none"
KEY_UNKNOWN = "unknown"
KEY_ENTER = "enter"
KEY_QUIT = "quit"
KEY_ESCAPE = "escape"
KEY_UP = "up"
KEY_DOWN = "down"
KEY_LEFT = "left"
KEY_RIGHT = "right"
KEY_SPACE = "space"
KEY_TOGGLE_ALL = "toggle_all"
KEY_RESET = "reset"
KEY_HELP = "help"
KEY_MENU = "menu"


class FrameClock:
    def __init__(self, interval: float) -> None:
        self.interval = max(0.001, interval)
        self.reset()

    def reset(self) -> None:
        self.last_render = time.monotonic() - self.interval
        self.dirty = True

    def request_redraw(self) -> None:
        self.dirty = True

    def consume_redraw(self, heartbeat: bool = False) -> bool:
        now = time.monotonic()
        if (not self.dirty and not heartbeat) or now - self.last_render < self.interval:
            return False
        self.mark_rendered(now)
        return True

    def mark_rendered(self, now: float | None = None) -> None:
        self.last_render = time.monotonic() if now is None else now
        self.dirty = False

    def wait_timeout(self, maximum: float) -> float:
        remaining = self.interval - (time.monotonic() - self.last_render)
        return max(0.0, min(maximum, remaining))


def set_color_enabled(enabled: bool) -> None:
    global COLOR_ENABLED
    COLOR_ENABLED = enabled


def read_char(timeout: float | None = 0.0) -> str:
    if timeout is not None:
        readable, _, _ = select.select([sys.stdin], [], [], timeout)
        if not readable:
            return ""
    return sys.stdin.read(1)


def read_key(timeout: float | None = 0.0, escape_timeout: float = 0.02) -> str:
    ch = read_char(timeout)
    if not ch:
        return KEY_NONE
    if ch in ("\n", "\r"):
        return KEY_ENTER
    if ch in {"q", "Q", "\x03"}:
        return KEY_QUIT
    if ch == " ":
        return KEY_SPACE
    if ch in {"a", "A"}:
        return KEY_TOGGLE_ALL
    if ch != "\x1b":
        return KEY_UNKNOWN

    prefix = read_char(escape_timeout)
    if not prefix:
        return KEY_ESCAPE
    if prefix == "O":
        code = read_char(escape_timeout)
        return {
            "P": KEY_HELP,
            "A": KEY_UP,
            "B": KEY_DOWN,
            "C": KEY_RIGHT,
            "D": KEY_LEFT,
        }.get(code, KEY_UNKNOWN)
    if prefix != "[":
        return KEY_UNKNOWN

    code = read_char(escape_timeout)
    if code == "A":
        return KEY_UP
    if code == "B":
        return KEY_DOWN
    if code == "C":
        return KEY_RIGHT
    if code == "D":
        return KEY_LEFT
    if not code.isdigit():
        return KEY_UNKNOWN

    sequence = code
    for _ in range(8):
        nxt = read_char(escape_timeout)
        if not nxt:
            break
        if nxt == "~":
            if sequence in {"1", "11"}:
                return KEY_HELP
            if sequence == "15":
                return KEY_RESET
            if sequence == "21":
                return KEY_MENU
            return KEY_UNKNOWN
        sequence += nxt
    return KEY_UNKNOWN


def color_enabled() -> bool:
    return COLOR_ENABLED and sys.stdout.isatty() and bool(os.environ.get("TERM")) and os.environ.get("TERM") != "dumb" and not os.environ.get("NO_COLOR")


def sgr(color: str, bold: bool = False, background: bool = False) -> str:
    if not color_enabled():
        return ""
    prefix = "48;5;" if background else "38;5;"
    return f"\033[{ '1;' if bold else ''}{prefix}{color}m"


def reset() -> str:
    return "\033[0m" if color_enabled() else ""


def reset_bg() -> str:
    return f"\033[0m\033[48;5;{BG}m\033[38;5;{TEXT}m" if color_enabled() else ""


def bg() -> str:
    return f"\033[48;5;{BG}m\033[38;5;{TEXT}m" if color_enabled() else ""


def text(value: str, color: str = TEXT, bold: bool = False) -> str:
    return f"{sgr(color, bold)}{value}{reset_bg()}"


def visible_len(value: str) -> int:
    length = 0
    i = 0
    while i < len(value):
        if value[i] == "\033":
            while i < len(value) and value[i] != "m":
                i += 1
            i += 1
            continue
        ch = value[i]
        if unicodedata.combining(ch):
            i += 1
            continue
        length += 2 if unicodedata.east_asian_width(ch) in {"W", "F"} or ord(ch) >= 0x1F300 else 1
        i += 1
    return length


def fit(value: str, width: int) -> str:
    if width <= 0:
        return ""
    if visible_len(value) <= width:
        return value
    target = width if width <= 3 else width - 3
    out: list[str] = []
    used = 0
    i = 0
    while i < len(value):
        if value[i] == "\033":
            start = i
            while i < len(value) and value[i] != "m":
                i += 1
            if i < len(value):
                i += 1
            out.append(value[start:i])
            continue
        ch = value[i]
        cell = 0 if unicodedata.combining(ch) else 2 if unicodedata.east_asian_width(ch) in {"W", "F"} or ord(ch) >= 0x1F300 else 1
        if used + cell > target:
            break
        out.append(ch)
        used += cell
        i += 1
    if width > 3:
        out.append("...")
    return "".join(out)


def pad_right(value: str, width: int) -> str:
    clipped = fit(value, width)
    return clipped + (" " * max(0, width - visible_len(clipped)))


def pad_left(value: str, width: int) -> str:
    clipped = fit(value, width)
    return (" " * max(0, width - visible_len(clipped))) + clipped


def terminal_size() -> tuple[int, int]:
    try:
        packed = fcntl.ioctl(sys.stdout.fileno(), termios.TIOCGWINSZ, struct.pack("HHHH", 0, 0, 0, 0))
        rows, cols, _, _ = struct.unpack("HHHH", packed)
        if rows > 0 and cols > 0:
            return rows, cols
    except OSError:
        pass
    size = shutil.get_terminal_size((80, 24))
    return size.lines, size.columns


def clear_screen() -> str:
    if color_enabled():
        return f"\033[48;5;{BG}m\033[38;5;{TEXT}m\033[2J\033[H"
    return "\033[2J\033[H"


def home() -> str:
    if color_enabled():
        return f"\033[48;5;{BG}m\033[38;5;{TEXT}m\033[H"
    return "\033[H"


def cursor_to(row: int, col: int = 1) -> str:
    return f"\033[{max(1, row)};{max(1, col)}H"


def split_rendered_lines(frame: str) -> list[str]:
    lines = frame.split("\n")
    if lines and lines[-1] == "":
        return lines
    return lines or [""]


class LineDiffRenderer:
    def __init__(self) -> None:
        self.previous: list[str] = []
        self.previous_rows = -1
        self.previous_cols = -1

    def reset(self) -> None:
        self.previous = []
        self.previous_rows = -1
        self.previous_cols = -1

    def render(self, frame: str, rows: int, cols: int, force_full: bool = False) -> None:
        lines = split_rendered_lines(frame)
        resized = rows != self.previous_rows or cols != self.previous_cols
        if force_full or resized or not self.previous:
            out = [clear_screen()]
            out.extend(line if index == 0 else "\n" + line for index, line in enumerate(lines))
        else:
            out = []
            max_lines = max(len(lines), len(self.previous))
            for index in range(max_lines):
                current = lines[index] if index < len(lines) else blank_line(cols)
                previous = self.previous[index] if index < len(self.previous) else ""
                if current != previous:
                    out.append(cursor_to(index + 1, 1) + current + "\033[K")
        self.previous = lines
        self.previous_rows = rows
        self.previous_cols = cols
        print("".join(out), end="", flush=True)


def enter_alt_screen() -> str:
    if color_enabled():
        return f"\033[?1049h\033[?25l{bg()}\033[2J\033[H"
    return "\033[?1049h\033[?25l\033[2J\033[H"


def leave_alt_screen() -> str:
    return reset() + "\033[?25h\033[?1049l"


def header(title: str, mode: str, width: int) -> str:
    left = f"📡 {title} [{mode}]"
    right = time.strftime("%H:%M:%S") + "  ⚙"
    spaces = max(1, width - visible_len(left) - visible_len(right))
    return (
        text(left, ACCENT, True)
        + (" " * spaces)
        + text(right, TEXT)
        + "\n"
        + text("─" * width, ACCENT)
        + "\n"
        + text("─" * width, MUTED)
        + "\n"
    )


def top_rule(width: int, title: str = "") -> str:
    width = max(2, width)
    if not title:
        return "┌" + ("─" * (width - 2)) + "┐"
    label = " " + fit(title, width - 4) + " "
    return "┌" + label + ("─" * max(0, width - 2 - visible_len(label))) + "┐"


def bottom_rule(width: int) -> str:
    width = max(2, width)
    return "└" + ("─" * (width - 2)) + "┘"


def row(value: str, width: int) -> str:
    return bg() + "│ " + pad_right(value, max(0, width - 4)) + " │" + reset_bg()


def box(title: str, rows: list[str], width: int) -> list[str]:
    return [text(top_rule(width, title), ACCENT, True), *[row(item, width) for item in rows], text(bottom_rule(width), ACCENT)]


def hstack(left: list[str], right: list[str], left_width: int, right_width: int, gap: int = 1) -> list[str]:
    count = max(len(left), len(right))
    left_empty = " " * left_width
    right_empty = " " * right_width
    spacer = " " * gap
    return [
        (left[i] if i < len(left) else left_empty) + spacer + (right[i] if i < len(right) else right_empty)
        for i in range(count)
    ]


def bar(value: float, maximum: float, width: int, pulse: bool = False) -> str:
    if width <= 0:
        return ""
    ratio = 0.0 if maximum <= 0 else max(0.0, min(1.0, value / maximum))
    filled = round(ratio * width)
    chars = []
    for index in range(width):
        if index < filled:
            chars.append("▓" if pulse and index == filled - 1 else "█")
        else:
            chars.append("░")
    return "".join(chars)


def badge(label: str, value: str, color: str = ACCENT) -> str:
    return text(f"[{label}: {value}]", color, True)


def footer(keys: str, width: int) -> str:
    raw = " " + fit(keys, max(1, width - 2)) + " "
    padded = pad_right(raw, width)
    if not color_enabled():
        return padded
    return sgr(ACCENT, True, True) + f"\033[38;5;16m{padded}" + reset()


def blank_line(width: int) -> str:
    width = max(1, width)
    if color_enabled():
        return f"\033[48;5;{BG}m" + (" " * width) + reset_bg()
    return " " * width


def append_line(out: list[str], line: str, width: int) -> int:
    out.append(pad_right(line, width) + "\n")
    return 1


def append_lines(out: list[str], lines: list[str], width: int, remaining_rows: int) -> int:
    used = 0
    for line in lines:
        if used >= remaining_rows:
            break
        out.append(pad_right(line, width) + "\n")
        used += 1
    return used


def append_footer_at_bottom(out: list[str], keys: str, rows: int, width: int, used_rows: int) -> None:
    for _ in range(max(0, rows - used_rows - 1)):
        out.append(blank_line(width) + "\n")
        used_rows += 1
    out.append(pad_right(footer(keys, width), width))


def duration(seconds: float) -> str:
    total = max(0, int(seconds))
    return f"{total // 3600:02d}:{(total % 3600) // 60:02d}:{total % 60:02d}"


def compact_rate(value: float, unit: str) -> str:
    if value >= 1_000_000:
        return f"{value / 1_000_000:.2f} M{unit}"
    if value >= 1_000:
        return f"{value / 1_000:.1f} k{unit}"
    return f"{value:.1f} {unit}"
