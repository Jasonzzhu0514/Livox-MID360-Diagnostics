#pragma once

#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace neon {

enum class Color {
  Accent,
  Success,
  Warning,
  Danger,
  Muted,
  Text,
  Footer,
};

struct Size {
  int rows = 24;
  int cols = 80;
};

enum class Key {
  None,
  Unknown,
  Enter,
  Quit,
  Escape,
  Up,
  Down,
  Left,
  Right,
  Space,
  ToggleAll,
  Reset,
  Help,
  Menu,
  Back,
};

class FrameClock {
 public:
  explicit FrameClock(std::chrono::milliseconds interval)
      : interval_(std::max(std::chrono::milliseconds(1), interval)) {
    reset();
  }

  void reset() {
    last_render_ = Clock::now() - interval_;
    dirty_ = true;
  }

  void request_redraw() {
    dirty_ = true;
  }

  bool dirty() const {
    return dirty_;
  }

  bool consume_redraw(bool heartbeat = false) {
    const auto now = Clock::now();
    if ((!dirty_ && !heartbeat) || now - last_render_ < interval_) {
      return false;
    }
    mark_rendered(now);
    return true;
  }

  void mark_rendered() {
    mark_rendered(Clock::now());
  }

  int wait_ms(int max_wait_ms) const {
    const auto now = Clock::now();
    const auto next = last_render_ + interval_;
    const auto remaining = next <= now
        ? std::chrono::milliseconds(0)
        : std::chrono::duration_cast<std::chrono::milliseconds>(next - now);
    return std::max(0, std::min(static_cast<int>(remaining.count()), std::max(0, max_wait_ms)));
  }

 private:
  using Clock = std::chrono::steady_clock;

  void mark_rendered(Clock::time_point now) {
    last_render_ = now;
    dirty_ = false;
  }

  std::chrono::milliseconds interval_;
  Clock::time_point last_render_;
  bool dirty_ = true;
};

class RawTerminal {
 public:
  RawTerminal() = default;
  RawTerminal(const RawTerminal&) = delete;
  RawTerminal& operator=(const RawTerminal&) = delete;

  ~RawTerminal() {
    restore();
  }

  bool enter(int fd = STDIN_FILENO, bool nonblocking = false) {
    if (active_) {
      return true;
    }
    fd_ = fd;
    if (!isatty(fd_) || tcgetattr(fd_, &original_) != 0) {
      return false;
    }
    termios raw = original_;
    raw.c_lflag &= static_cast<unsigned>(~(ICANON | ECHO));
    raw.c_cc[VMIN] = nonblocking ? 0 : 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(fd_, TCSANOW, &raw) != 0) {
      return false;
    }
    active_ = true;
    return true;
  }

  void restore() {
    if (!active_) {
      return;
    }
    tcsetattr(fd_, TCSANOW, &original_);
    active_ = false;
  }

  bool active() const {
    return active_;
  }

 private:
  int fd_ = STDIN_FILENO;
  termios original_ {};
  bool active_ = false;
};

inline bool read_char_timeout(char& out, int timeout_ms) {
  fd_set read_set;
  FD_ZERO(&read_set);
  FD_SET(STDIN_FILENO, &read_set);
  timeval timeout {};
  timeout.tv_sec = timeout_ms / 1000;
  timeout.tv_usec = (timeout_ms % 1000) * 1000;
  const int ready = select(STDIN_FILENO + 1, &read_set, nullptr, nullptr, timeout_ms < 0 ? nullptr : &timeout);
  return ready > 0 && ::read(STDIN_FILENO, &out, 1) == 1;
}

inline Key read_key(int timeout_ms = 0, int escape_timeout_ms = 20) {
  char ch = 0;
  if (!read_char_timeout(ch, timeout_ms)) {
    return Key::None;
  }
  if (ch == '\n' || ch == '\r') {
    return Key::Enter;
  }
  if (ch == 'q' || ch == 'Q') {
    return Key::Quit;
  }
  if (ch == ' ') {
    return Key::Space;
  }
  if (ch == 'a' || ch == 'A') {
    return Key::ToggleAll;
  }
  if (ch == 'm' || ch == 'M') {
    return Key::Menu;
  }
  if (ch == 'b' || ch == 'B') {
    return Key::Back;
  }
  if (ch == 3) {
    return Key::Quit;
  }
  if (ch != '\033') {
    return Key::Unknown;
  }

  char prefix = 0;
  if (!read_char_timeout(prefix, escape_timeout_ms)) {
    return Key::Escape;
  }
  if (prefix == 'O') {
    char code = 0;
    if (!read_char_timeout(code, escape_timeout_ms)) {
      return Key::Unknown;
    }
    switch (code) {
      case 'P':
        return Key::Help;
      case 'Q':
        return Key::Unknown;
      case 'R':
        return Key::Unknown;
      case 'S':
        return Key::Unknown;
      case 'A':
        return Key::Up;
      case 'B':
        return Key::Down;
      case 'C':
        return Key::Right;
      case 'D':
        return Key::Left;
      default:
        return Key::Unknown;
    }
  }
  if (prefix != '[') {
    return Key::Unknown;
  }

  char code = 0;
  if (!read_char_timeout(code, escape_timeout_ms)) {
    return Key::Unknown;
  }
  switch (code) {
    case 'A':
      return Key::Up;
    case 'B':
      return Key::Down;
    case 'C':
      return Key::Right;
    case 'D':
      return Key::Left;
    default:
      break;
  }
  if (code < '0' || code > '9') {
    return Key::Unknown;
  }

  std::string sequence(1, code);
  for (int i = 0; i < 8; ++i) {
    char next = 0;
    if (!read_char_timeout(next, escape_timeout_ms)) {
      break;
    }
    if (next == '~') {
      if (sequence == "11" || sequence == "1") {
        return Key::Help;
      }
      if (sequence == "15") {
        return Key::Reset;
      }
      if (sequence == "21") {
        return Key::Menu;
      }
      return Key::Unknown;
    }
    sequence.push_back(next);
  }
  return Key::Unknown;
}

inline bool& color_override() {
  static bool enabled = true;
  return enabled;
}

inline void set_color_enabled(bool enabled) {
  color_override() = enabled;
}

inline bool color_enabled() {
  if (!color_override() || !isatty(STDOUT_FILENO)) {
    return false;
  }
  const char* no_color = std::getenv("NO_COLOR");
  if (no_color && std::strlen(no_color) > 0) {
    return false;
  }
  const char* term = std::getenv("TERM");
  return term && std::strcmp(term, "dumb") != 0;
}

inline Size terminal_size() {
  Size size;
  winsize ws {};
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
    if (ws.ws_row > 0) {
      size.rows = ws.ws_row;
    }
    if (ws.ws_col > 0) {
      size.cols = ws.ws_col;
    }
  }
  return size;
}

inline bool env_is_false(const char* value) {
  if (!value || std::strlen(value) == 0) {
    return false;
  }
  std::string lowered(value);
  std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return lowered == "0" || lowered == "false" || lowered == "no" || lowered == "off";
}

inline bool parse_positive_int(const std::string& value, int& out) {
  if (value.empty()) {
    return false;
  }
  char* end = nullptr;
  const long parsed = std::strtol(value.c_str(), &end, 10);
  if (end != value.c_str() + value.size() || parsed <= 0 || parsed > 1000) {
    return false;
  }
  out = static_cast<int>(parsed);
  return true;
}

inline Size preferred_terminal_size(int default_rows = 40, int default_cols = 132) {
  Size target;
  target.rows = default_rows;
  target.cols = default_cols;

  if (const char* value = std::getenv("LIVOX_MID360_TERMINAL_SIZE")) {
    const std::string spec(value);
    const size_t separator = spec.find_first_of("xX,");
    if (separator != std::string::npos) {
      int cols = 0;
      int rows = 0;
      if (parse_positive_int(spec.substr(0, separator), cols) &&
          parse_positive_int(spec.substr(separator + 1), rows)) {
        target.cols = cols;
        target.rows = rows;
      }
    }
  }
  if (const char* value = std::getenv("LIVOX_MID360_TERMINAL_COLS")) {
    int cols = 0;
    if (parse_positive_int(value, cols)) {
      target.cols = cols;
    }
  }
  if (const char* value = std::getenv("LIVOX_MID360_TERMINAL_ROWS")) {
    int rows = 0;
    if (parse_positive_int(value, rows)) {
      target.rows = rows;
    }
  }
  return target;
}

inline void ensure_terminal_size(int min_rows = 40, int min_cols = 132) {
  if (!isatty(STDOUT_FILENO) || env_is_false(std::getenv("LIVOX_MID360_RESIZE_TERMINAL"))) {
    return;
  }
  const char* term = std::getenv("TERM");
  if (!term || std::strcmp(term, "dumb") == 0) {
    return;
  }

  const Size current = terminal_size();
  Size target = preferred_terminal_size(min_rows, min_cols);
  target.rows = std::max(target.rows, min_rows);
  target.cols = std::max(target.cols, min_cols);
  if (current.rows >= target.rows && current.cols >= target.cols) {
    return;
  }

  std::cout << "\033[8;" << target.rows << ";" << target.cols << "t" << std::flush;
  std::this_thread::sleep_for(std::chrono::milliseconds(80));
}

inline int clamp_int(int value, int low, int high) {
  return std::max(low, std::min(high, value));
}

inline double clamp_double(double value, double low, double high) {
  return std::max(low, std::min(high, value));
}

inline std::string sgr(Color color, bool bold = false, bool background = false) {
  if (!color_enabled()) {
    return "";
  }
  const char* fg = "38;5;252";
  const char* bg = "48;5;233";
  switch (color) {
    case Color::Accent:
      fg = "38;5;51";
      bg = "48;5;51";
      break;
    case Color::Success:
      fg = "38;5;84";
      bg = "48;5;84";
      break;
    case Color::Warning:
      fg = "38;5;220";
      bg = "48;5;220";
      break;
    case Color::Danger:
      fg = "38;5;196";
      bg = "48;5;196";
      break;
    case Color::Muted:
      fg = "38;5;242";
      bg = "48;5;242";
      break;
    case Color::Footer:
      fg = "38;5;16";
      bg = "48;5;51";
      break;
    case Color::Text:
      break;
  }
  std::string code = background ? bg : fg;
  return "\033[" + std::string(bold ? "1;" : "") + code + "m";
}

inline std::string reset() {
  return color_enabled() ? "\033[0m" : "";
}

inline std::string reset_bg() {
  return color_enabled() ? "\033[0m\033[48;5;233m\033[38;5;252m" : "";
}

inline std::string bg() {
  return color_enabled() ? "\033[48;5;233m\033[38;5;252m" : "";
}

inline std::string text(const std::string& value, Color color, bool bold = false) {
  return sgr(color, bold) + value + reset_bg();
}

inline std::string badge(const std::string& label, const std::string& value, Color color) {
  return text("[" + label + ": " + value + "]", color, true);
}

inline std::string repeat(const std::string& value, int count) {
  std::string out;
  if (count <= 0 || value.empty()) {
    return out;
  }
  out.reserve(value.size() * static_cast<size_t>(count));
  for (int i = 0; i < count; ++i) {
    out += value;
  }
  return out;
}

inline bool decode_utf8(const std::string& value, size_t index, uint32_t& codepoint, size_t& next) {
  const unsigned char first = static_cast<unsigned char>(value[index]);
  if (first < 0x80) {
    codepoint = first;
    next = index + 1;
    return true;
  }
  auto continuation = [&](size_t offset) {
    return index + offset < value.size() && (static_cast<unsigned char>(value[index + offset]) & 0xc0) == 0x80;
  };
  if ((first & 0xe0) == 0xc0 && continuation(1)) {
    codepoint = ((first & 0x1f) << 6) | (static_cast<unsigned char>(value[index + 1]) & 0x3f);
    next = index + 2;
    return true;
  }
  if ((first & 0xf0) == 0xe0 && continuation(1) && continuation(2)) {
    codepoint = ((first & 0x0f) << 12) |
        ((static_cast<unsigned char>(value[index + 1]) & 0x3f) << 6) |
        (static_cast<unsigned char>(value[index + 2]) & 0x3f);
    next = index + 3;
    return true;
  }
  if ((first & 0xf8) == 0xf0 && continuation(1) && continuation(2) && continuation(3)) {
    codepoint = ((first & 0x07) << 18) |
        ((static_cast<unsigned char>(value[index + 1]) & 0x3f) << 12) |
        ((static_cast<unsigned char>(value[index + 2]) & 0x3f) << 6) |
        (static_cast<unsigned char>(value[index + 3]) & 0x3f);
    next = index + 4;
    return true;
  }
  codepoint = first;
  next = index + 1;
  return false;
}

inline int codepoint_width(uint32_t cp) {
  if (cp == 0) {
    return 0;
  }
  if (cp < 32 || (cp >= 0x7f && cp < 0xa0)) {
    return 0;
  }
  if ((cp >= 0x0300 && cp <= 0x036f) ||
      (cp >= 0xfe00 && cp <= 0xfe0f)) {
    return 0;
  }
  if ((cp >= 0x1100 && cp <= 0x115f) ||
      (cp >= 0x2329 && cp <= 0x232a) ||
      (cp >= 0x2e80 && cp <= 0xa4cf) ||
      (cp >= 0xac00 && cp <= 0xd7a3) ||
      (cp >= 0xf900 && cp <= 0xfaff) ||
      (cp >= 0xfe10 && cp <= 0xfe19) ||
      (cp >= 0xfe30 && cp <= 0xfe6f) ||
      (cp >= 0xff00 && cp <= 0xff60) ||
      (cp >= 0xffe0 && cp <= 0xffe6) ||
      (cp >= 0x1f300 && cp <= 0x1faff)) {
    return 2;
  }
  return 1;
}

inline int visible_length(const std::string& value) {
  int length = 0;
  for (size_t i = 0; i < value.size(); ++i) {
    const unsigned char ch = static_cast<unsigned char>(value[i]);
    if (ch == '\033') {
      while (i < value.size() && value[i] != 'm') {
        ++i;
      }
      continue;
    }
    uint32_t cp = 0;
    size_t next = i + 1;
    decode_utf8(value, i, cp, next);
    length += codepoint_width(cp);
    i = next - 1;
  }
  return length;
}

inline std::string fit(const std::string& value, int width) {
  if (width <= 0) {
    return "";
  }
  if (visible_length(value) <= width) {
    return value;
  }
  const int target = width <= 3 ? width : width - 3;
  std::string out;
  int used = 0;
  bool copied_ansi = false;
  for (size_t i = 0; i < value.size();) {
    if (static_cast<unsigned char>(value[i]) == '\033') {
      const size_t start = i;
      while (i < value.size() && value[i] != 'm') {
        ++i;
      }
      if (i < value.size()) {
        ++i;
      }
      out += value.substr(start, i - start);
      copied_ansi = true;
      continue;
    }
    uint32_t cp = 0;
    size_t next = i + 1;
    decode_utf8(value, i, cp, next);
    const int cell_width = codepoint_width(cp);
    if (used + cell_width > target) {
      break;
    }
    out += value.substr(i, next - i);
    used += cell_width;
    i = next;
  }
  if (width > 3) {
    out += "...";
  }
  if (copied_ansi) {
    out += reset_bg();
  }
  return out;
}

inline std::string pad_right(const std::string& value, int width) {
  std::string clipped = value;
  if (visible_length(clipped) > width) {
    clipped = fit(value, width);
  }
  const int visible = visible_length(clipped);
  if (visible >= width) {
    return clipped;
  }
  return clipped + std::string(static_cast<size_t>(width - visible), ' ');
}

inline std::string pad_left(const std::string& value, int width) {
  std::string clipped = value;
  if (visible_length(clipped) > width) {
    clipped = fit(value, width);
  }
  const int visible = visible_length(clipped);
  if (visible >= width) {
    return clipped;
  }
  return std::string(static_cast<size_t>(width - visible), ' ') + clipped;
}

inline std::string top_rule(int width, const std::string& title = "") {
  width = std::max(2, width);
  if (title.empty()) {
    return "┌" + repeat("─", width - 2) + "┐";
  }
  const std::string label = " " + fit(title, std::max(0, width - 4)) + " ";
  return "┌" + label + repeat("─", std::max(0, width - 2 - visible_length(label))) + "┐";
}

inline std::string mid_rule(int width) {
  width = std::max(2, width);
  return "├" + repeat("─", width - 2) + "┤";
}

inline std::string bottom_rule(int width) {
  width = std::max(2, width);
  return "└" + repeat("─", width - 2) + "┘";
}

inline std::string row(const std::string& raw, int width) {
  width = std::max(4, width);
  return bg() + "│ " + pad_right(raw, width - 4) + " │" + reset_bg();
}

inline std::string empty_row(int width) {
  return row("", width);
}

inline std::string key_value_row(const std::string& key, const std::string& value, int width, int key_width = 13) {
  width = std::max(8, width);
  const int value_width = std::max(0, width - key_width - 5);
  return "│ " + pad_right(key, key_width) + " " + pad_right(value, value_width) + " │";
}

inline std::string bar(double value, double max_value, int width, bool pulse = false) {
  width = std::max(0, width);
  if (width == 0) {
    return "";
  }
  const double ratio = max_value <= 0.0 ? 0.0 : clamp_double(value / max_value, 0.0, 1.0);
  const int filled = clamp_int(static_cast<int>(ratio * width + 0.5), 0, width);
  std::string out;
  out.reserve(static_cast<size_t>(width * 3));
  for (int i = 0; i < width; ++i) {
    if (i < filled) {
      out += (pulse && i == filled - 1) ? "▓" : "█";
    } else {
      out += "░";
    }
  }
  return out;
}

inline std::string fixed(double value, int precision) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(precision) << value;
  return out.str();
}

inline std::string compact_rate(double value, const std::string& unit) {
  if (value >= 1000000.0) {
    return fixed(value / 1000000.0, 2) + " M" + unit;
  }
  if (value >= 1000.0) {
    return fixed(value / 1000.0, 1) + " k" + unit;
  }
  return fixed(value, 1) + " " + unit;
}

inline std::string duration(double seconds) {
  const auto total = static_cast<long long>(std::max(0.0, seconds));
  const long long hours = total / 3600;
  const long long minutes = (total % 3600) / 60;
  const long long secs = total % 60;
  std::ostringstream out;
  out << std::setfill('0') << std::setw(2) << hours << ":"
      << std::setw(2) << minutes << ":"
      << std::setw(2) << secs;
  return out.str();
}

inline std::string clock() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t raw = std::chrono::system_clock::to_time_t(now);
  std::tm local {};
  localtime_r(&raw, &local);
  std::ostringstream out;
  out << std::put_time(&local, "%H:%M:%S");
  return out.str();
}

inline bool inherited_alt_screen() {
  const char* value = std::getenv("LIVOX_MID360_ALT_SCREEN");
  return value && std::strcmp(value, "1") == 0;
}

inline std::string enter_alt_screen() {
  const std::string enter = inherited_alt_screen() ? "" : "\033[?1049h";
  const std::string input_modes = "\033[?1000l\033[?1002l\033[?1003l\033[?1006l";
  return color_enabled() ? enter + input_modes + "\033[?25l" + bg() + "\033[2J\033[H" : enter + input_modes + "\033[?25l\033[2J\033[H";
}

inline std::string leave_alt_screen() {
  return reset() + "\033[?1000l\033[?1002l\033[?1003l\033[?1006l\033[?25h\033[?1049l";
}

inline std::string clear_screen() {
  return color_enabled() ? "\033[48;5;233m\033[38;5;252m\033[2J\033[H" : "\033[2J\033[H";
}

inline std::string home() {
  return color_enabled() ? "\033[48;5;233m\033[38;5;252m\033[H" : "\033[H";
}

inline std::string blank_line(int width);

inline std::string cursor_to(int row, int col = 1) {
  row = std::max(1, row);
  col = std::max(1, col);
  return "\033[" + std::to_string(row) + ";" + std::to_string(col) + "H";
}

inline std::vector<std::string> split_rendered_lines(const std::string& frame) {
  std::vector<std::string> lines;
  std::string current;
  for (char ch : frame) {
    if (ch == '\n') {
      lines.push_back(current);
      current.clear();
    } else {
      current.push_back(ch);
    }
  }
  if (!current.empty() || frame.empty() || frame.back() == '\n') {
    lines.push_back(current);
  }
  return lines;
}

class LineDiffRenderer {
 public:
  void reset() {
    previous_.clear();
    previous_rows_ = -1;
    previous_cols_ = -1;
  }

  void render(const std::string& frame, int rows, int cols, bool force_full = false) {
    const std::vector<std::string> lines = split_rendered_lines(frame);
    const bool resized = rows != previous_rows_ || cols != previous_cols_;
    std::ostringstream out;
    if (force_full || resized || previous_.empty()) {
      out << clear_screen();
      for (size_t i = 0; i < lines.size(); ++i) {
        if (i) {
          out << "\n";
        }
        out << lines[i];
      }
    } else {
      const size_t max_lines = std::max(lines.size(), previous_.size());
      for (size_t i = 0; i < max_lines; ++i) {
        const std::string current = i < lines.size() ? lines[i] : blank_line(cols);
        const std::string previous = i < previous_.size() ? previous_[i] : std::string();
        if (current == previous) {
          continue;
        }
        out << cursor_to(static_cast<int>(i) + 1, 1) << current << "\033[K";
      }
    }
    previous_ = lines;
    previous_rows_ = rows;
    previous_cols_ = cols;
    std::cout << out.str() << std::flush;
  }

 private:
  std::vector<std::string> previous_;
  int previous_rows_ = -1;
  int previous_cols_ = -1;
};

inline std::string blank_line(int width) {
  return color_enabled() ? "\033[48;5;233m" + std::string(static_cast<size_t>(std::max(1, width)), ' ') + reset_bg()
                         : std::string(static_cast<size_t>(std::max(1, width)), ' ');
}

inline std::string footer(const std::string& keys, int width) {
  width = std::max(1, width);
  const std::string raw = " " + fit(keys, std::max(1, width - 2)) + " ";
  const std::string padded = pad_right(raw, width);
  if (!color_enabled()) {
    return padded;
  }
  return sgr(Color::Footer, true, true) + padded + reset();
}

inline void append_line(std::ostringstream& out, const std::string& line, int width, int& used_rows) {
  out << pad_right(line, width) << "\n";
  ++used_rows;
}

inline void append_lines(
    std::ostringstream& out,
    const std::vector<std::string>& lines,
    int width,
    int remaining_rows,
    int& used_rows) {
  for (int i = 0; i < static_cast<int>(lines.size()) && i < remaining_rows; ++i) {
    append_line(out, lines[static_cast<size_t>(i)], width, used_rows);
  }
}

inline void append_blank_lines(std::ostringstream& out, int count, int width, int& used_rows) {
  for (int i = 0; i < count; ++i) {
    out << blank_line(width) << "\n";
    ++used_rows;
  }
}

inline void append_footer_at_bottom(
    std::ostringstream& out,
    const std::string& keys,
    int rows,
    int width,
    int& used_rows) {
  append_blank_lines(out, std::max(0, rows - used_rows - 1), width, used_rows);
  out << pad_right(footer(keys, width), width);
  ++used_rows;
}

inline std::string header(const std::string& title, const std::string& mode, int width) {
  width = std::max(20, width);
  const std::string left = "📡 " + title + " [" + mode + "]";
  const std::string right = clock() + "  ⚙";
  const int spaces = std::max(1, width - visible_length(left) - visible_length(right));
  std::ostringstream out;
  out << text(left, Color::Accent, true)
      << std::string(static_cast<size_t>(spaces), ' ')
      << text(right, Color::Text)
      << "\n"
      << text(repeat("─", width), Color::Accent)
      << "\n"
      << text(repeat("─", width), Color::Muted)
      << "\n";
  return out.str();
}

inline std::vector<std::string> box(const std::string& title, const std::vector<std::string>& rows, int width) {
  std::vector<std::string> out;
  out.reserve(rows.size() + 2);
  out.push_back(text(top_rule(width, title), Color::Accent, true));
  for (const auto& item : rows) {
    out.push_back(row(item, width));
  }
  out.push_back(text(bottom_rule(width), Color::Accent));
  return out;
}

inline std::vector<std::string> hstack(
    const std::vector<std::string>& left,
    const std::vector<std::string>& right,
    int left_width,
    int right_width,
    int gap = 1) {
  std::vector<std::string> out;
  const size_t rows = std::max(left.size(), right.size());
  out.reserve(rows);
  const std::string empty_left(static_cast<size_t>(std::max(0, left_width)), ' ');
  const std::string empty_right(static_cast<size_t>(std::max(0, right_width)), ' ');
  const std::string spacer(static_cast<size_t>(std::max(0, gap)), ' ');
  for (size_t i = 0; i < rows; ++i) {
    out.push_back((i < left.size() ? left[i] : empty_left) + spacer + (i < right.size() ? right[i] : empty_right));
  }
  return out;
}

}  // namespace neon
