#include "neon_tui.hpp"

#include <limits.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

#include <array>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#ifndef LIVOX_MID360_DIAGNOSTICS_VERSION
#define LIVOX_MID360_DIAGNOSTICS_VERSION "unknown"
#endif

namespace {

std::string executable_dir() {
  char buffer[PATH_MAX] = {};
  const ssize_t size = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
  if (size > 0) {
    buffer[size] = '\0';
    std::string path(buffer);
    const std::string::size_type slash = path.find_last_of('/');
    if (slash != std::string::npos) {
      return path.substr(0, slash);
    }
  }
  return ".";
}

struct MenuItem {
  std::string command;
  std::string description;
  std::string badge;
};

bool g_menu_alt_handoff = false;

void usage(const char* argv0) {
  std::cout
      << "usage: " << argv0 << " [autoconfig|monitor|dump] [args...]\n"
      << "\n"
      << "C++ entry point for Livox MID360 diagnostics.\n"
      << "For dump, LIVOX_MID360_CONFIG is added automatically when set.\n"
      << "Monitor discovers the lidar by default and does not consume the configured lidar IP.\n"
      << "Run without arguments to open the interactive menu.\n"
      << "\n"
      << "commands:\n"
      << "  autoconfig   discover lidar IP/SN and optionally update MID360_config.json\n"
      << "  monitor      show Livox-SDK2 point cloud and IMU callback rates\n"
      << "  dump         decode Livox-SDK2 callbacks to point/IMU CSV files\n";
}

std::vector<std::string> menu_identity_rows() {
  return {
      "VERSION      " + neon::text(LIVOX_MID360_DIAGNOSTICS_VERSION, neon::Color::Accent, true),
      "PROFILE      " + neon::text("Neon Protocol", neon::Color::Success, true),
      "DISCOVERY    SDK first, no env lidar IP default",
      "",
      neon::badge("HEALTH", "READY", neon::Color::Success),
      neon::badge("MODE", "MENU", neon::Color::Accent),
      neon::badge("UI", "ADAPTIVE", neon::Color::Success),
  };
}

std::vector<std::string> menu_command_rows(const std::array<MenuItem, 4>& items, size_t cursor, int width) {
  std::vector<std::string> rows;
  rows.push_back("上下方向键移动，回车确认，q 或 Esc 退出。");
  rows.push_back("");
  for (size_t i = 0; i < items.size(); ++i) {
    const auto& item = items[i];
    const bool active = i == cursor;
    const std::string marker = active ? neon::text("▶", neon::Color::Accent, true) : " ";
    const std::string command = neon::text(neon::pad_right(item.command, 12), active ? neon::Color::Accent : neon::Color::Text, active);
    const std::string badge = neon::text(neon::pad_right(item.badge, 8), active ? neon::Color::Success : neon::Color::Muted, true);
    const int desc_width = std::max(8, width - 31);
    rows.push_back(marker + " " + command + " " + badge + " " + neon::fit(item.description, desc_width));
  }
  return rows;
}

std::string render_menu(const std::array<MenuItem, 4>& items, size_t cursor) {
  const neon::Size term = neon::terminal_size();
  const int width = std::max(20, term.cols - 1);
  int used_rows = 0;
  std::ostringstream out;
  out << neon::clear_screen();
  out << neon::header("LIVOX MID-360 DIAGNOSTICS", "MENU", width);
  used_rows += 3;
  if (term.rows < 16 || term.cols < 58) {
    neon::append_line(out, neon::text("Terminal too small for Neon Protocol TUI", neon::Color::Warning, true), width, used_rows);
    neon::append_line(out, "Resize to at least 58x16.", width, used_rows);
    neon::append_footer_at_bottom(out, "[ENTER] SELECT   [Q/ESC] QUIT", term.rows, width, used_rows);
    return out.str();
  }

  const int body_rows = std::max(1, term.rows - used_rows - 1);
  if (width >= 96) {
    const int gap = 1;
    const int left_width = neon::clamp_int(width / 3, 31, 42);
    const int right_width = std::max(40, width - left_width - gap);
    auto left = neon::box("DEVICE IDENTITY", menu_identity_rows(), left_width);
    auto right = neon::box("COMMAND ROUTER", menu_command_rows(items, cursor, right_width), right_width);
    auto rows = neon::hstack(left, right, left_width, right_width, gap);
    for (int i = 0; i < std::min<int>(body_rows - 1, rows.size()); ++i) {
      neon::append_line(out, rows[static_cast<size_t>(i)], width, used_rows);
    }
  } else {
    const int panel_width = width;
    auto identity = neon::box("DEVICE IDENTITY", menu_identity_rows(), panel_width);
    auto commands = neon::box("COMMAND ROUTER", menu_command_rows(items, cursor, panel_width), panel_width);
    int used = 0;
    for (const auto& row : identity) {
      if (++used >= body_rows - 1) {
        break;
      }
      neon::append_line(out, row, width, used_rows);
    }
    if (used < body_rows - 1) {
      neon::append_line(out, "", width, used_rows);
      ++used;
    }
    for (const auto& row : commands) {
      if (++used >= body_rows - 1) {
        break;
      }
      neon::append_line(out, row, width, used_rows);
    }
  }
  neon::append_footer_at_bottom(out, "[↑/↓] MOVE   [ENTER] SELECT   [Q/ESC] QUIT", term.rows, width, used_rows);
  return out.str();
}

bool command_uses_live_tui(const std::string& command) {
  return command == "monitor" || command == "sdk-monitor";
}

std::string choose_command_menu() {
  const std::array<MenuItem, 4> items = {{
      {"autoconfig", "发现雷达并选择要更新的配置文件", "CONFIG"},
      {"monitor", "发现雷达并查看实时 SDK 回调状态", "LIVE"},
      {"dump", "短时采样并导出点云/IMU CSV", "CSV"},
      {"quit", "退出诊断入口", "EXIT"},
  }};
  if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
    usage("livox_mid360_diagnostics");
    return "quit";
  }

  termios original {};
  if (tcgetattr(STDIN_FILENO, &original) != 0) {
    usage("livox_mid360_diagnostics");
    return "quit";
  }
  termios raw = original;
  raw.c_lflag &= static_cast<unsigned>(~(ICANON | ECHO));
  raw.c_cc[VMIN] = 1;
  raw.c_cc[VTIME] = 0;
  if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) {
    usage("livox_mid360_diagnostics");
    return "quit";
  }

  size_t cursor = 0;
  bool done = false;
  auto redraw = [&]() {
    std::cout << render_menu(items, cursor);
    std::cout.flush();
  };
  auto read_char_timeout = [](char& out, int timeout_ms) {
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(STDIN_FILENO, &read_set);
    timeval timeout {};
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;
    const int ready = select(STDIN_FILENO + 1, &read_set, nullptr, nullptr, &timeout);
    return ready > 0 && ::read(STDIN_FILENO, &out, 1) == 1;
  };

  std::cout << neon::enter_alt_screen() << std::flush;
  redraw();
  while (!done) {
    char ch = 0;
    if (::read(STDIN_FILENO, &ch, 1) != 1) {
      cursor = items.size() - 1;
      break;
    }
    if (ch == '\033') {
      char left = 0;
      char right = 0;
      if (read_char_timeout(left, 50) && read_char_timeout(right, 50) && left == '[') {
        if (right == 'A') {
          cursor = cursor == 0 ? items.size() - 1 : cursor - 1;
        } else if (right == 'B') {
          cursor = (cursor + 1) % items.size();
        }
      } else {
        cursor = items.size() - 1;
        done = true;
      }
    } else if (ch == '\n' || ch == '\r') {
      done = true;
    } else if (ch == 'q' || ch == 'Q') {
      cursor = items.size() - 1;
      done = true;
    }
    if (!done) {
      redraw();
    }
  }
  const std::string selected = items[cursor].command;
  g_menu_alt_handoff = command_uses_live_tui(selected);
  tcsetattr(STDIN_FILENO, TCSANOW, &original);
  if (!g_menu_alt_handoff) {
    std::cout << neon::leave_alt_screen() << std::flush;
  }
  return selected;
}

std::string target_for_command(const std::string& command) {
  if (command == "autoconfig" || command == "config") {
    return "mid360_config_tool";
  }
  if (command == "monitor" || command == "sdk-monitor") {
    return "mid360_sdk_monitor";
  }
  if (command == "dump" || command == "sdk-dump") {
    return "mid360_sdk_dump";
  }
  return "";
}

bool has_config_arg(const std::vector<std::string>& args) {
  for (const std::string& arg : args) {
    if (arg == "--config" || arg.rfind("--config=", 0) == 0) {
      return true;
    }
  }
  return false;
}

bool file_exists(const std::string& path) {
  std::ifstream in(path);
  return in.good();
}

bool command_needs_config(const std::string& command) {
  return command == "dump" || command == "sdk-dump";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 2 && std::strcmp(argv[1], "--version") == 0) {
    std::cout << "Livox MID360 Diagnostics " << LIVOX_MID360_DIAGNOSTICS_VERSION << "\n";
    return 0;
  }
  if (argc >= 2 && (std::strcmp(argv[1], "-h") == 0 || std::strcmp(argv[1], "--help") == 0)) {
    usage(argv[0]);
    return 0;
  }

  const std::string command = argc < 2 ? choose_command_menu() : argv[1];
  if (command == "quit") {
    return 0;
  }
  const std::string target_name = target_for_command(command);
  if (target_name.empty()) {
    if (g_menu_alt_handoff) {
      std::cout << neon::leave_alt_screen() << std::flush;
      g_menu_alt_handoff = false;
    }
    std::cerr << "ERROR: unknown command: " << command << "\n";
    usage(argv[0]);
    return 2;
  }

  const std::string target_path = executable_dir() + "/" + target_name;
  if (access(target_path.c_str(), X_OK) != 0) {
    if (g_menu_alt_handoff) {
      std::cout << neon::leave_alt_screen() << std::flush;
      g_menu_alt_handoff = false;
    }
    std::cerr << "ERROR: required executable not found: " << target_path << "\n";
    if (target_name == "mid360_sdk_monitor" || target_name == "mid360_sdk_dump") {
      std::cerr << "Run ./scripts/build_cpp_with_sdk2.sh to build SDK commands.\n";
    }
    return 2;
  }

  std::vector<std::string> child_args;
  child_args.push_back(target_path);
  for (int i = 2; i < argc; ++i) {
    child_args.push_back(argv[i]);
  }
  if (argc < 2 && command == "dump") {
    child_args.push_back("--duration");
    child_args.push_back("10");
    child_args.push_back("--points");
    child_args.push_back("mid360_points.csv");
    child_args.push_back("--imu");
    child_args.push_back("mid360_imu.csv");
  }
  const char* config_path = std::getenv("LIVOX_MID360_CONFIG");
  if (command_needs_config(command) && !has_config_arg(child_args) && config_path && std::strlen(config_path) > 0) {
    child_args.push_back("--config");
    child_args.push_back(config_path);
  } else if (command_needs_config(command) && !has_config_arg(child_args) && file_exists("config/MID360_config.local.json")) {
    child_args.push_back("--config");
    child_args.push_back("config/MID360_config.local.json");
  }

  std::vector<char*> raw_args;
  raw_args.reserve(child_args.size() + 1);
  for (std::string& arg : child_args) {
    raw_args.push_back(arg.data());
  }
  raw_args.push_back(nullptr);

  if (g_menu_alt_handoff) {
    setenv("LIVOX_MID360_ALT_SCREEN", "1", 1);
  }
  execv(target_path.c_str(), raw_args.data());
  if (g_menu_alt_handoff) {
    std::cout << neon::leave_alt_screen() << std::flush;
  }
  std::cerr << "ERROR: failed to execute " << target_path << "\n";
  return 2;
}
