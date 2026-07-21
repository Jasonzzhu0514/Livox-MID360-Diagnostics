#include "neon_tui.hpp"

#include <limits.h>
#include <unistd.h>

#include <array>
#include <chrono>
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

int run_mid360_config_tool(int argc, char** argv);
#ifdef LIVOX_MID360_HAS_SDK_TOOLS
int run_mid360_sdk_monitor(int argc, char** argv);
int run_mid360_sdk_dump(int argc, char** argv);
int run_mid360_sdk_preview(int argc, char** argv);
#endif

namespace {

constexpr int kReturnToMenuCode = 75;

struct MenuItem {
  std::string command;
  std::string description;
  std::string badge;
};

void usage(const char* argv0) {
  std::cout
      << "usage: " << argv0 << " [autoconfig|monitor|preview|dump] [args...]\n"
      << "\n"
      << "C++ entry point for Livox MID360 diagnostics.\n"
      << "For dump, LIVOX_MID360_CONFIG is added automatically when set.\n"
      << "Monitor discovers the lidar by default and does not consume the configured lidar IP.\n"
      << "Run without arguments to open the interactive menu.\n"
      << "\n"
      << "commands:\n"
      << "  autoconfig   discover lidar IP/SN and optionally update MID360_config.json\n"
      << "  monitor      show Livox-SDK2 point cloud and IMU callback rates\n"
      << "  preview      stream binary point cloud frames for the GUI preview\n"
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

std::vector<std::string> menu_command_rows(const std::array<MenuItem, 5>& items, size_t cursor, int width) {
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

std::string render_menu(const std::array<MenuItem, 5>& items, size_t cursor) {
  const neon::Size term = neon::terminal_size();
  const int width = std::max(20, term.cols - 1);
  int used_rows = 0;
  std::ostringstream out;
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

std::string choose_command_menu() {
  const std::array<MenuItem, 5> items = {{
      {"autoconfig", "发现雷达并选择要更新的配置文件", "CONFIG"},
      {"monitor", "发现雷达并查看实时 SDK 回调状态", "LIVE"},
      {"preview", "为 GUI 点云画布输出实时点云帧", "3D"},
      {"dump", "短时采样并导出点云/IMU CSV", "CSV"},
      {"quit", "退出诊断入口", "EXIT"},
  }};
  if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
    usage("livox_mid360_diagnostics");
    return "quit";
  }
  neon::ensure_terminal_size();

  neon::RawTerminal raw_terminal;
  if (!raw_terminal.enter()) {
    usage("livox_mid360_diagnostics");
    return "quit";
  }

  size_t cursor = 0;
  bool done = false;
  neon::FrameClock frame_clock(std::chrono::milliseconds(1000));
  neon::LineDiffRenderer renderer;
  auto redraw = [&]() {
    const neon::Size term = neon::terminal_size();
    renderer.render(render_menu(items, cursor), term.rows, std::max(20, term.cols - 1));
    frame_clock.mark_rendered();
  };

  std::cout << neon::enter_alt_screen() << std::flush;
  redraw();
  while (!done) {
    const int wait_ms = frame_clock.wait_ms(20);
    const neon::Key key = neon::read_key(wait_ms, 20);
    if (key == neon::Key::None) {
      if (frame_clock.consume_redraw(true)) {
        redraw();
      }
      continue;
    } else if (key == neon::Key::Up) {
      cursor = cursor == 0 ? items.size() - 1 : cursor - 1;
    } else if (key == neon::Key::Down) {
      cursor = (cursor + 1) % items.size();
    } else if (key == neon::Key::Enter) {
      done = true;
    } else if (key == neon::Key::Quit || key == neon::Key::Escape) {
      cursor = items.size() - 1;
      done = true;
    }
    if (!done) {
      redraw();
    }
  }
  const std::string selected = items[cursor].command;
  raw_terminal.restore();
  std::cout << neon::leave_alt_screen() << std::flush;
  return selected;
}

void wait_for_exit_key() {
  if (!isatty(STDIN_FILENO)) {
    return;
  }
  neon::RawTerminal raw_terminal;
  if (!raw_terminal.enter()) {
    return;
  }
  while (true) {
    const neon::Key key = neon::read_key(-1, 20);
    if (key == neon::Key::Enter || key == neon::Key::Quit || key == neon::Key::Escape) {
      break;
    }
  }
}

void show_error_screen(const std::string& title, const std::vector<std::string>& messages) {
  if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
    for (const auto& message : messages) {
      std::cerr << "ERROR: " << message << "\n";
    }
    return;
  }
  const neon::Size term = neon::terminal_size();
  const int rows = std::max(10, term.rows);
  const int width = std::max(58, term.cols);
  int used_rows = 3;
  std::ostringstream out;
  out << neon::enter_alt_screen()
      << neon::clear_screen()
      << neon::header("LIVOX MID-360 DIAGNOSTICS", title, width);
  std::vector<std::string> status_rows = {
      "STATUS       " + neon::badge("STATE", "ERROR", neon::Color::Danger),
      "",
  };
  status_rows.insert(status_rows.end(), messages.begin(), messages.end());
  neon::append_lines(out, neon::box("COMMAND RESULT", status_rows, width), width, rows - used_rows - 1, used_rows);
  neon::append_footer_at_bottom(out, "[ENTER/Q/ESC] EXIT", rows, width, used_rows);
  std::cout << out.str() << std::flush;
  wait_for_exit_key();
  std::cout << neon::leave_alt_screen() << std::flush;
}

bool has_config_arg(const std::vector<std::string>& args) {
  for (const std::string& arg : args) {
    if (arg == "--config" || arg.rfind("--config=", 0) == 0) {
      return true;
    }
  }
  return false;
}

bool has_help_arg(const std::vector<std::string>& args) {
  for (const std::string& arg : args) {
    if (arg == "-h" || arg == "--help") {
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

bool known_command(const std::string& command) {
  return command == "autoconfig" || command == "config" ||
      command == "monitor" || command == "sdk-monitor" ||
      command == "preview" || command == "sdk-preview" ||
      command == "dump" || command == "sdk-dump";
}

std::string child_argv0(const std::string& command) {
  if (command == "autoconfig" || command == "config") {
    return "mid360_config_tool";
  }
  if (command == "monitor" || command == "sdk-monitor") {
    return "mid360_sdk_monitor";
  }
  if (command == "dump" || command == "sdk-dump") {
    return "mid360_sdk_dump";
  }
  if (command == "preview" || command == "sdk-preview") {
    return "mid360_sdk_preview";
  }
  return command;
}

int run_command(const std::string& command, int argc, std::vector<char*>& raw_args) {
  if (command == "autoconfig" || command == "config") {
    return run_mid360_config_tool(argc, raw_args.data());
  }
#ifdef LIVOX_MID360_HAS_SDK_TOOLS
  if (command == "monitor" || command == "sdk-monitor") {
    return run_mid360_sdk_monitor(argc, raw_args.data());
  }
  if (command == "dump" || command == "sdk-dump") {
    return run_mid360_sdk_dump(argc, raw_args.data());
  }
  if (command == "preview" || command == "sdk-preview") {
    return run_mid360_sdk_preview(argc, raw_args.data());
  }
#else
  if (command == "monitor" || command == "sdk-monitor" || command == "dump" || command == "sdk-dump" ||
      command == "preview" || command == "sdk-preview") {
    std::cerr << "ERROR: SDK command is unavailable; rebuild with Livox-SDK2.\n";
    return 2;
  }
#endif
  std::cerr << "ERROR: unknown command: " << command << "\n";
  return 2;
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

  const bool interactive_menu = argc < 2;
  while (true) {
    const std::string command = interactive_menu ? choose_command_menu() : argv[1];
    if (command == "quit") {
      return 0;
    }
    if (!known_command(command)) {
      show_error_screen("ERROR", {"unknown command: " + command});
      return 2;
    }

    std::vector<std::string> child_args;
    child_args.push_back(child_argv0(command));
    for (int i = 2; i < argc; ++i) {
      child_args.push_back(argv[i]);
    }
    if (interactive_menu && command == "dump") {
      child_args.push_back("--duration");
      child_args.push_back("10");
      child_args.push_back("--points");
      child_args.push_back("mid360_points.csv");
      child_args.push_back("--imu");
      child_args.push_back("mid360_imu.csv");
    }
    const char* config_path = std::getenv("LIVOX_MID360_CONFIG");
    const bool needs_config = command_needs_config(command) && !has_help_arg(child_args);
    if (needs_config && !has_config_arg(child_args) && config_path && std::strlen(config_path) > 0) {
      child_args.push_back("--config");
      child_args.push_back(config_path);
    } else if (needs_config && !has_config_arg(child_args) && file_exists("config/MID360_config.local.json")) {
      child_args.push_back("--config");
      child_args.push_back("config/MID360_config.local.json");
    } else if (needs_config && !has_config_arg(child_args)) {
      show_error_screen(
          "DUMP",
          {
              "dump needs a MID360_config.json path.",
              "Use monitor for normal validation.",
              "For CSV dump, pass --config PATH or set LIVOX_MID360_CONFIG.",
          });
      return 2;
    }

    std::vector<char*> raw_args;
    raw_args.reserve(child_args.size() + 1);
    for (std::string& arg : child_args) {
      raw_args.push_back(arg.data());
    }
    raw_args.push_back(nullptr);

    if (interactive_menu) {
      setenv("LIVOX_MID360_ALLOW_MENU_RETURN", "1", 1);
    }
    const int result = run_command(command, static_cast<int>(child_args.size()), raw_args);
    if (interactive_menu) {
      unsetenv("LIVOX_MID360_ALLOW_MENU_RETURN");
    }
    if (interactive_menu && result == kReturnToMenuCode) {
      continue;
    }
    return result == kReturnToMenuCode ? 2 : result;
  }
}
