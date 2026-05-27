#include <limits.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

#include <array>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
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

void usage(const char* argv0) {
  std::cout
      << "usage: " << argv0 << " [autoconfig|monitor|dump] [args...]\n"
      << "\n"
      << "C++ entry point for Livox MID360 diagnostics.\n"
      << "For monitor/dump, LIVOX_MID360_CONFIG is added automatically when set.\n"
      << "Run without arguments to open the interactive menu.\n"
      << "\n"
      << "commands:\n"
      << "  autoconfig   discover lidar IP/SN and optionally update MID360_config.json\n"
      << "  monitor      show Livox-SDK2 point cloud and IMU callback rates\n"
      << "  dump         decode Livox-SDK2 callbacks to point/IMU CSV files\n";
}

std::string choose_command_menu() {
  const std::array<std::pair<const char*, const char*>, 4> items = {{
      {"autoconfig", "发现雷达并选择要更新的配置文件"},
      {"monitor", "查看实时 SDK 回调状态"},
      {"dump", "短时采样导出 CSV"},
      {"quit", "退出"},
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
    std::cout << "\033[?1049h\033[?25l\033[H\033[2J";
    std::cout << "Livox MID360 Diagnostics\n";
    std::cout << "========================\n\n";
    std::cout << "上下方向键移动，回车确认，q 或 Esc 退出。\n\n";
    for (size_t i = 0; i < items.size(); ++i) {
      std::cout << (i == cursor ? "> " : "  ")
                << std::left << std::setw(12) << items[i].first
                << " - " << items[i].second << "\n";
    }
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
  tcsetattr(STDIN_FILENO, TCSANOW, &original);
  std::cout << "\033[?25h\033[?1049l" << std::flush;
  return items[cursor].first;
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
  return command == "monitor" || command == "sdk-monitor" || command == "dump" || command == "sdk-dump";
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
    std::cerr << "ERROR: unknown command: " << command << "\n";
    usage(argv[0]);
    return 2;
  }

  const std::string target_path = executable_dir() + "/" + target_name;
  if (access(target_path.c_str(), X_OK) != 0) {
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

  execv(target_path.c_str(), raw_args.data());
  std::cerr << "ERROR: failed to execute " << target_path << "\n";
  return 2;
}
