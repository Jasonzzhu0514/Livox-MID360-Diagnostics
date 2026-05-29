#include "neon_tui.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <rapidjson/filewritestream.h>
#include <rapidjson/prettywriter.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <limits>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <limits.h>

namespace {

constexpr int kDiscoveryPort = 56000;
constexpr int kMinActiveScanScore = 100;
const std::array<const char*, 1> kLivoxMacPrefixes = {"8c:58:23"};

struct Options {
  std::string iface = "auto";
  double timeout_sec = 8.0;
  bool apply = false;
  bool yes = false;
  bool no_sudo = false;
  bool verbose = false;
  bool require_match = false;
  bool show_all = false;
  bool no_color = false;
  std::vector<std::string> configs;
};

struct InterfaceInfo {
  std::string name;
  std::string ip;
  int prefix = 0;
};

struct Discovery {
  std::string lidar_ip;
  std::string requested_host_ip;
  std::string iface_ip;
  int raw_packets = 0;
  std::string method;
};

struct ConfigState {
  std::string path;
  std::string configured_ip;
  std::string status;
  bool low_priority = false;
  size_t original_index = 0;
};

struct ConfigView {
  std::vector<ConfigState> recommended;
  std::vector<ConfigState> matched;
  std::vector<ConfigState> other;
  size_t hidden_count = 0;
};

struct DetectionSummary {
  std::string iface;
  Discovery result;
};

std::vector<std::string> detection_rows(const DetectionSummary& summary);

struct Theme {
  bool enabled = false;
  std::string reset;
  std::string bold;
  std::string dim;
  std::string red;
  std::string green;
  std::string yellow;
  std::string blue;
  std::string cyan;
};

const std::array<const char*, 2> kMid360ConfigKeys = {"MID360", "Mid360s"};
bool g_screen_active = false;
std::vector<std::string> g_scan_lines;
Theme g_theme;
volatile std::sig_atomic_t g_interrupted = 0;
termios g_original_termios {};
bool g_has_original_termios = false;

std::string home_dir() {
  const char* home = std::getenv("HOME");
  return home ? home : ".";
}

std::string trim(const std::string& value) {
  const auto begin = std::find_if_not(value.begin(), value.end(), [](unsigned char ch) {
    return std::isspace(ch);
  });
  const auto end = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) {
    return std::isspace(ch);
  }).base();
  if (begin >= end) {
    return "";
  }
  return std::string(begin, end);
}

std::string lower_copy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

std::string real_path_or_self(const std::string& path) {
  char buffer[PATH_MAX] = {};
  if (realpath(path.c_str(), buffer)) {
    return buffer;
  }
  return path;
}

bool should_enable_color(bool no_color) {
  if (no_color || !isatty(STDOUT_FILENO)) {
    return false;
  }
  const char* no_color_env = std::getenv("NO_COLOR");
  if (no_color_env && std::strlen(no_color_env) > 0) {
    return false;
  }
  const char* term = std::getenv("TERM");
  return term && std::strcmp(term, "dumb") != 0;
}

void init_theme(bool no_color) {
  if (!should_enable_color(no_color)) {
    g_theme = Theme{};
    return;
  }
  g_theme.enabled = true;
  g_theme.reset = "\033[0m";
  g_theme.bold = "\033[1m";
  g_theme.dim = "\033[2m";
  g_theme.red = "\033[31m";
  g_theme.green = "\033[32m";
  g_theme.yellow = "\033[33m";
  g_theme.blue = "\033[34m";
  g_theme.cyan = "\033[36m";
}

std::string colorize(const std::string& text, const std::string& color) {
  if (!g_theme.enabled || color.empty()) {
    return text;
  }
  return color + text + g_theme.reset;
}

std::string bold(const std::string& text) {
  return colorize(text, g_theme.bold);
}

std::string dim(const std::string& text) {
  return colorize(text, g_theme.dim);
}

std::string green(const std::string& text) {
  return colorize(text, g_theme.green);
}

std::string yellow(const std::string& text) {
  return colorize(text, g_theme.yellow);
}

std::string red(const std::string& text) {
  return colorize(text, g_theme.red);
}

std::string blue(const std::string& text) {
  return colorize(text, g_theme.blue);
}

std::string cyan(const std::string& text) {
  return colorize(text, g_theme.cyan);
}

std::string status_label(const std::string& status) {
  if (status == "mismatch") {
    return yellow(status);
  }
  if (status == "match") {
    return green(status);
  }
  if (status == "unavailable") {
    return dim(status);
  }
  return status;
}

std::string path_display(const std::string& path) {
  const std::string absolute = real_path_or_self(path);
  const std::string cwd = real_path_or_self(".");
  const std::string home = real_path_or_self(home_dir());
  if (absolute == cwd) {
    return ".";
  }
  if (absolute.rfind(cwd + "/", 0) == 0) {
    return absolute.substr(cwd.size() + 1);
  }
  if (!home.empty() && absolute == home) {
    return "~";
  }
  if (!home.empty() && absolute.rfind(home + "/", 0) == 0) {
    return "~/" + absolute.substr(home.size() + 1);
  }
  return path;
}

std::string compact_path(const std::string& path, size_t max_width = 76) {
  const std::string display = path_display(path);
  if (display.size() <= max_width) {
    return display;
  }
  if (max_width <= 10) {
    return display.substr(0, max_width);
  }
  const size_t tail_width = max_width - 4;
  return ".../" + display.substr(display.size() - tail_width);
}

bool is_low_priority_config_path(const std::string& path) {
  const std::string lower = lower_copy(path_display(path));
  return lower.find("/external/") != std::string::npos ||
         lower.rfind("external/", 0) == 0 ||
         lower.find("/samples/") != std::string::npos ||
         lower.rfind("samples/", 0) == 0 ||
         lower.find("/thirdparty/") != std::string::npos ||
         lower.rfind("thirdparty/", 0) == 0 ||
         lower.find("/3rdparty/") != std::string::npos ||
         lower.rfind("3rdparty/", 0) == 0 ||
         lower.find("/.runtime/") != std::string::npos ||
         lower.rfind(".runtime/", 0) == 0 ||
         lower.find("/examples/") != std::string::npos ||
         lower.rfind("examples/", 0) == 0 ||
         lower.find("/build/") != std::string::npos ||
         lower.rfind("build/", 0) == 0 ||
         lower.find("/dist/") != std::string::npos ||
         lower.rfind("dist/", 0) == 0;
}

std::vector<std::string> split_paths(const std::string& value) {
  std::vector<std::string> paths;
  std::string item;
  std::istringstream stream(value);
  while (std::getline(stream, item, ':')) {
    if (!item.empty()) {
      paths.push_back(item);
    }
  }
  return paths;
}

std::vector<std::string> env_paths(std::initializer_list<const char*> names) {
  std::vector<std::string> paths;
  for (const char* name : names) {
    const char* value = std::getenv(name);
    if (!value || std::strlen(value) == 0) {
      continue;
    }
    std::vector<std::string> split = split_paths(value);
    paths.insert(paths.end(), split.begin(), split.end());
  }
  return paths;
}

std::vector<std::string> config_search_roots() {
  std::vector<std::string> roots = env_paths({"LIVOX_MID360_SEARCH_ROOTS"});
  if (roots.empty()) {
    roots.push_back(".");
    roots.push_back(home_dir());
  }
  return roots;
}

bool file_exists(const std::string& path) {
  struct stat info {};
  return stat(path.c_str(), &info) == 0;
}

bool regular_file_exists(const std::string& path) {
  struct stat info {};
  return stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}

bool directory_exists(const std::string& path) {
  struct stat info {};
  return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

std::string run_text(const std::vector<std::string>& args, double timeout_sec = 0.0) {
  std::ostringstream command;
  if (timeout_sec > 0.0) {
    command << "timeout " << timeout_sec << " ";
  }
  for (size_t i = 0; i < args.size(); ++i) {
    if (i) {
      command << " ";
    }
    command << "'";
    for (char ch : args[i]) {
      if (ch == '\'') {
        command << "'\\''";
      } else {
        command << ch;
      }
    }
    command << "'";
  }
  command << " 2>&1";

  FILE* pipe = popen(command.str().c_str(), "r");
  if (!pipe) {
    return "";
  }
  std::string output;
  std::array<char, 4096> buffer{};
  while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
    output += buffer.data();
  }
  pclose(pipe);
  return output;
}

void debug(const Options& options, const std::string& message) {
  if (options.verbose) {
    std::cerr << "[debug] " << message << "\n";
  }
}

void handle_interrupt(int) {
  g_interrupted = 1;
}

void throw_if_interrupted() {
  if (g_interrupted) {
    throw std::runtime_error("interrupted");
  }
}

void progress(const std::string& message) {
  throw_if_interrupted();
  const std::string line = (g_screen_active ? neon::text("扫描", neon::Color::Accent, true) : cyan("扫描")) + " " + message;
  if (g_screen_active) {
    g_scan_lines.push_back(line);
    if (g_scan_lines.size() > 128) {
      g_scan_lines.erase(g_scan_lines.begin());
    }
    const neon::Size term = neon::terminal_size();
    std::ostringstream out;
    out << neon::clear_screen()
        << neon::header("LIVOX MID-360 AUTOCONFIG", "SCAN", std::max(40, term.cols));
    const int panel_w = std::max(40, term.cols);
    const int max_lines = std::max(3, term.rows - 8);
    std::vector<std::string> rows;
    const size_t begin = g_scan_lines.size() > static_cast<size_t>(max_lines)
        ? g_scan_lines.size() - static_cast<size_t>(max_lines)
        : 0;
    for (size_t i = begin; i < g_scan_lines.size(); ++i) {
      rows.push_back(g_scan_lines[i]);
    }
    if (rows.empty()) {
      rows.push_back("waiting for discovery output");
    }
    int used_rows = 3;
    neon::append_lines(out, neon::box("DISCOVERY TRACE", rows, panel_w), panel_w, term.rows - used_rows - 1, used_rows);
    neon::append_footer_at_bottom(out, "[CTRL+C] STOP", term.rows, panel_w, used_rows);
    std::cout << out.str();
    std::cout.flush();
  } else {
    std::cout << line << std::endl;
  }
}

void render_scan_screen(const std::string& message) {
  progress(message);
}

std::vector<std::string> split_lines(const std::string& text) {
  std::vector<std::string> lines;
  std::istringstream stream(text);
  std::string line;
  while (std::getline(stream, line)) {
    lines.push_back(line);
  }
  return lines;
}

std::string read_file(const std::string& path) {
  std::ifstream in(path);
  if (!in) {
    throw std::runtime_error("cannot read file: " + path);
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

rapidjson::Document load_json(const std::string& path) {
  const std::string text = read_file(path);
  rapidjson::Document doc;
  doc.Parse(text.c_str());
  if (doc.HasParseError()) {
    std::ostringstream err;
    err << "JSON parse error in " << path << " at offset " << doc.GetErrorOffset() << ": "
        << rapidjson::GetParseError_En(doc.GetParseError());
    throw std::runtime_error(err.str());
  }
  if (!doc.IsObject()) {
    throw std::runtime_error("JSON root is not an object: " + path);
  }
  return doc;
}

bool write_json_atomic(const std::string& path, const rapidjson::Document& doc) {
  const std::string tmp_path = path + ".tmp";
  FILE* fp = std::fopen(tmp_path.c_str(), "wb");
  if (!fp) {
    return false;
  }
  char write_buffer[65536];
  rapidjson::FileWriteStream os(fp, write_buffer, sizeof(write_buffer));
  rapidjson::PrettyWriter<rapidjson::FileWriteStream> writer(os);
  writer.SetIndent(' ', 2);
  doc.Accept(writer);
  std::fputc('\n', fp);
  std::fclose(fp);
  if (std::rename(tmp_path.c_str(), path.c_str()) != 0) {
    std::remove(tmp_path.c_str());
    return false;
  }
  return true;
}

std::string read_config_lidar_ip(const std::string& path) {
  if (!regular_file_exists(path)) {
    return "";
  }
  rapidjson::Document doc = load_json(path);
  if (doc.HasMember("lidar_configs") && doc["lidar_configs"].IsArray() && !doc["lidar_configs"].Empty()) {
    const auto& first = doc["lidar_configs"][0];
    if (first.IsObject() && first.HasMember("ip") && first["ip"].IsString()) {
      return first["ip"].GetString();
    }
  }
  for (const char* key : kMid360ConfigKeys) {
    if (!doc.HasMember(key) || !doc[key].IsObject()) {
      continue;
    }
    const auto& lidar_object = doc[key];
    if (!lidar_object.HasMember("host_net_info") || !lidar_object["host_net_info"].IsArray() ||
        lidar_object["host_net_info"].Empty()) {
      continue;
    }
    const auto& first_host = lidar_object["host_net_info"][0];
    if (!first_host.IsObject() || !first_host.HasMember("lidar_ip") || !first_host["lidar_ip"].IsArray() ||
        first_host["lidar_ip"].Empty() || !first_host["lidar_ip"][0].IsString()) {
      continue;
    }
    return first_host["lidar_ip"][0].GetString();
  }
  return "";
}

bool is_mid360_config_file(const std::string& path) {
  if (!regular_file_exists(path)) {
    return false;
  }
  try {
    rapidjson::Document doc = load_json(path);
    if (doc.HasMember("lidar_configs") && doc["lidar_configs"].IsArray()) {
      return true;
    }
    for (const char* key : kMid360ConfigKeys) {
      if (doc.HasMember(key) && doc[key].IsObject() && doc[key].HasMember("host_net_info")) {
        return true;
      }
    }
  } catch (const std::exception&) {
    return false;
  }
  return false;
}

void set_string_member(
    rapidjson::Value& object,
    const char* key,
    const std::string& value,
    rapidjson::Document::AllocatorType& allocator) {
  rapidjson::Value json_value;
  json_value.SetString(value.c_str(), static_cast<rapidjson::SizeType>(value.size()), allocator);
  if (object.HasMember(key)) {
    object[key] = json_value;
  } else {
    object.AddMember(rapidjson::Value(key, allocator), json_value, allocator);
  }
}

bool update_config(const std::string& path, const std::string& lidar_ip, const std::string& host_ip) {
  rapidjson::Document doc = load_json(path);
  auto& allocator = doc.GetAllocator();
  bool has_sdk2_host_array = false;
  for (const char* key : kMid360ConfigKeys) {
    if (doc.HasMember(key) && doc[key].IsObject() &&
        doc[key].HasMember("host_net_info") && doc[key]["host_net_info"].IsArray()) {
      has_sdk2_host_array = true;
    }
  }

  if (!doc.HasMember("lidar_configs") && !has_sdk2_host_array) {
    rapidjson::Value arr(rapidjson::kArrayType);
    arr.PushBack(rapidjson::Value(rapidjson::kObjectType), allocator);
    doc.AddMember("lidar_configs", arr, allocator);
  }
  if (doc.HasMember("lidar_configs") && doc["lidar_configs"].IsArray()) {
    auto& configs = doc["lidar_configs"];
    if (configs.Empty()) {
      configs.PushBack(rapidjson::Value(rapidjson::kObjectType), allocator);
    }
    if (!configs[0].IsObject()) {
      configs[0].SetObject();
    }
    rapidjson::Value value;
    value.SetString(lidar_ip.c_str(), static_cast<rapidjson::SizeType>(lidar_ip.size()), allocator);
    if (configs[0].HasMember("ip")) {
      configs[0]["ip"] = value;
    } else {
      configs[0].AddMember("ip", value, allocator);
    }
  }
  if (!host_ip.empty()) {
    for (const char* lidar_key : kMid360ConfigKeys) {
      if (doc.HasMember(lidar_key) && doc[lidar_key].IsObject() &&
          doc[lidar_key].HasMember("host_net_info") && doc[lidar_key]["host_net_info"].IsObject()) {
        auto& host = doc[lidar_key]["host_net_info"];
        for (const char* ip_key : {"cmd_data_ip", "push_msg_ip", "point_data_ip", "imu_data_ip"}) {
          if (host.HasMember(ip_key)) {
            set_string_member(host, ip_key, host_ip, allocator);
          }
        }
      }
    }
  }
  for (const char* key : kMid360ConfigKeys) {
    if (!doc.HasMember(key) || !doc[key].IsObject() ||
        !doc[key].HasMember("host_net_info") || !doc[key]["host_net_info"].IsArray()) {
      continue;
    }
    auto& hosts = doc[key]["host_net_info"];
    if (hosts.Empty()) {
      hosts.PushBack(rapidjson::Value(rapidjson::kObjectType), allocator);
    }
    if (!hosts[0].IsObject()) {
      hosts[0].SetObject();
    }
    auto& first_host = hosts[0];
    if (!first_host.HasMember("lidar_ip") || !first_host["lidar_ip"].IsArray()) {
      rapidjson::Value arr(rapidjson::kArrayType);
      first_host.AddMember("lidar_ip", arr, allocator);
    }
    auto& lidar_ips = first_host["lidar_ip"];
    rapidjson::Value value;
    value.SetString(lidar_ip.c_str(), static_cast<rapidjson::SizeType>(lidar_ip.size()), allocator);
    if (lidar_ips.Empty()) {
      lidar_ips.PushBack(value, allocator);
    } else {
      lidar_ips[0] = value;
    }
    if (!host_ip.empty()) {
      set_string_member(first_host, "host_ip", host_ip, allocator);
    }
    if (first_host.HasMember("multicast_ip")) {
      first_host.RemoveMember("multicast_ip");
    }
  }
  return write_json_atomic(path, doc);
}

uint32_t ip_to_u32(const std::string& ip) {
  in_addr addr{};
  if (inet_pton(AF_INET, ip.c_str(), &addr) != 1) {
    return 0;
  }
  return ntohl(addr.s_addr);
}

bool ip_in_network(const std::string& ip, const std::string& host_ip, int prefix) {
  if (prefix <= 0 || prefix > 32) {
    return false;
  }
  uint32_t addr = ip_to_u32(ip);
  uint32_t host = ip_to_u32(host_ip);
  if (!addr || !host) {
    return false;
  }
  const uint32_t mask = prefix == 32 ? 0xffffffffu : (0xffffffffu << (32 - prefix));
  return (addr & mask) == (host & mask);
}

std::vector<InterfaceInfo> list_ipv4_interfaces() {
  std::vector<InterfaceInfo> interfaces;
  ifaddrs* ifaddr = nullptr;
  if (getifaddrs(&ifaddr) != 0) {
    return interfaces;
  }
  for (ifaddrs* item = ifaddr; item != nullptr; item = item->ifa_next) {
    if (!item->ifa_addr || item->ifa_addr->sa_family != AF_INET) {
      continue;
    }
    const std::string name = item->ifa_name;
    if (name == "lo" || name.rfind("docker", 0) == 0) {
      continue;
    }
    char addr[INET_ADDRSTRLEN]{};
    auto* sin = reinterpret_cast<sockaddr_in*>(item->ifa_addr);
    inet_ntop(AF_INET, &sin->sin_addr, addr, sizeof(addr));

    int prefix = 24;
    if (item->ifa_netmask) {
      auto* mask = reinterpret_cast<sockaddr_in*>(item->ifa_netmask);
      prefix = __builtin_popcount(ntohl(mask->sin_addr.s_addr));
    }
    interfaces.push_back({name, addr, prefix});
  }
  freeifaddrs(ifaddr);
  return interfaces;
}

std::string iface_ipv4(const std::string& iface) {
  for (const auto& item : list_ipv4_interfaces()) {
    if (item.name == iface) {
      return item.ip;
    }
  }
  return "";
}

std::vector<std::string> discover_config_paths(const Options& options) {
  std::set<std::string> seen;
  std::vector<std::string> found;
  for (const std::string& root : config_search_roots()) {
    throw_if_interrupted();
    if (!directory_exists(root)) {
      continue;
    }
    if (g_screen_active) {
      progress("searching config files under " + root);
    }
    const std::string output = run_text(
        {
            "find",
            root,
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
        },
        12.0);
    for (const auto& line : split_lines(output)) {
      if (!line.empty() && is_mid360_config_file(line)) {
        const std::string key = real_path_or_self(line);
        if (seen.insert(key).second) {
          found.push_back(line);
        }
      }
    }
  }
  std::sort(found.begin(), found.end(), [](const std::string& a, const std::string& b) {
    const auto rank = [](const std::string& path) {
      const std::string lower = lower_copy(path);
      if (lower.find("livox-mid360-diagnostics-prebuilt/config/") != std::string::npos) {
        return 0;
      }
      if (lower.find("/config/") != std::string::npos) {
        return 1;
      }
      if (lower.find("/external/") != std::string::npos || lower.find("/samples/") != std::string::npos) {
        return 3;
      }
      if (lower.find("/dist/") != std::string::npos || lower.find("/build/") != std::string::npos) {
        return 4;
      }
      return 2;
    };
    const int ar = rank(a);
    const int br = rank(b);
    if (ar != br) {
      return ar < br;
    }
    return a < b;
  });
  debug(options, "discovered configs=" + std::to_string(found.size()));
  return found;
}

std::vector<std::string> resolve_config_paths(const Options& options) {
  if (!options.configs.empty()) {
    return options.configs;
  }
  std::vector<std::string> discovered = discover_config_paths(options);
  if (!discovered.empty()) {
    return discovered;
  }
  std::vector<std::string> configured = env_paths({"LIVOX_MID360_CONFIG", "MID360_CONFIG"});
  if (!configured.empty()) {
    return configured;
  }
  const std::string local_config = "config/MID360_config.local.json";
  if (regular_file_exists(local_config)) {
    return {local_config};
  }
  return {"MID360_config.json"};
}

std::vector<std::string> candidate_ifaces(const Options& options, const std::vector<std::string>& config_paths) {
  if (options.iface != "auto") {
    return {options.iface};
  }
  const auto interfaces = list_ipv4_interfaces();
  std::vector<std::string> candidates;
  for (const auto& path : config_paths) {
    const std::string lidar_ip = read_config_lidar_ip(path);
    if (lidar_ip.empty()) {
      continue;
    }
    for (const auto& iface : interfaces) {
      if (ip_in_network(lidar_ip, iface.ip, iface.prefix) &&
          std::find(candidates.begin(), candidates.end(), iface.name) == candidates.end()) {
        candidates.push_back(iface.name);
      }
    }
  }
  for (const auto& iface : interfaces) {
    if ((iface.name.rfind("eth", 0) == 0 || iface.name.rfind("en", 0) == 0) &&
        std::find(candidates.begin(), candidates.end(), iface.name) == candidates.end()) {
      candidates.push_back(iface.name);
    }
  }
  for (const auto& iface : interfaces) {
    if (std::find(candidates.begin(), candidates.end(), iface.name) == candidates.end()) {
      candidates.push_back(iface.name);
    }
  }
  if (candidates.empty()) {
    candidates.push_back("eth0");
  }
  return candidates;
}

std::vector<std::string> host_ips_for_iface(const std::string& iface) {
  std::vector<std::string> hosts;
  std::string host_ip;
  int prefix = 24;
  for (const auto& item : list_ipv4_interfaces()) {
    if (item.name == iface) {
      host_ip = item.ip;
      prefix = item.prefix;
      break;
    }
  }
  if (host_ip.empty()) {
    return hosts;
  }
  uint32_t host = ip_to_u32(host_ip);
  if (!host) {
    return hosts;
  }
  if (prefix < 24) {
    prefix = 24;
  }
  uint32_t mask = prefix == 32 ? 0xffffffffu : (0xffffffffu << (32 - prefix));
  uint32_t network = host & mask;
  uint32_t broadcast = network | ~mask;
  for (uint32_t ip = network + 1; ip < broadcast; ++ip) {
    if (ip == host) {
      continue;
    }
    in_addr addr{};
    addr.s_addr = htonl(ip);
    char text[INET_ADDRSTRLEN]{};
    inet_ntop(AF_INET, &addr, text, sizeof(text));
    hosts.emplace_back(text);
  }
  return hosts;
}

std::vector<std::string> gateway_ips(const std::string& iface) {
  std::vector<std::string> gateways;
  const std::string output = run_text({"ip", "route", "show", "dev", iface}, 2.0);
  std::regex pattern(R"(\bvia\s+(\d+\.\d+\.\d+\.\d+))");
  for (auto it = std::sregex_iterator(output.begin(), output.end(), pattern); it != std::sregex_iterator(); ++it) {
    gateways.push_back((*it)[1].str());
  }
  return gateways;
}

struct Neighbor {
  std::string ip;
  std::string mac;
  std::string state;
  int ttl = -1;
  int score = 0;
};

std::vector<Neighbor> neighbor_entries(const std::string& iface) {
  const std::string output = run_text({"ip", "neigh", "show", "dev", iface}, 2.0);
  std::vector<Neighbor> entries;
  for (const auto& line : split_lines(output)) {
    std::istringstream stream(line);
    std::vector<std::string> parts;
    std::string token;
    while (stream >> token) {
      parts.push_back(token);
    }
    auto lladdr = std::find(parts.begin(), parts.end(), "lladdr");
    if (lladdr == parts.end() || std::next(lladdr) == parts.end()) {
      continue;
    }
    std::string state = parts.back();
    if (state == "FAILED" || state == "INCOMPLETE") {
      continue;
    }
    std::string mac = *std::next(lladdr);
    std::transform(mac.begin(), mac.end(), mac.begin(), [](unsigned char ch) { return std::tolower(ch); });
    entries.push_back({parts[0], mac, state});
  }
  return entries;
}

int ping_ttl(const std::string& iface, const std::string& ip) {
  const std::string output = run_text({"ping", "-I", iface, "-c", "1", "-W", "1", ip}, 1.5);
  std::regex ttl_regex(R"(\bttl=(\d+))", std::regex::icase);
  std::smatch match;
  if (std::regex_search(output, match, ttl_regex)) {
    return std::stoi(match[1].str());
  }
  return -1;
}

Discovery active_scan(const std::string& iface, const Options& options) {
  Discovery result;
  result.iface_ip = iface_ipv4(iface);
  result.method = "active_scan";
  const auto hosts = host_ips_for_iface(iface);
  if (hosts.empty()) {
    debug(options, "active scan skipped: no IPv4 network for " + iface);
    return result;
  }
  progress("active scan on " + iface + " hosts=" + std::to_string(hosts.size()));

  std::vector<std::thread> workers;
  size_t cursor = 0;
  const size_t worker_count = std::min<size_t>(64, hosts.size());
  std::mutex cursor_mutex;
  for (size_t i = 0; i < worker_count; ++i) {
    workers.emplace_back([&]() {
      while (true) {
        std::string ip;
        {
          std::lock_guard<std::mutex> lock(cursor_mutex);
          if (cursor >= hosts.size()) {
            return;
          }
          ip = hosts[cursor++];
        }
        run_text({"ping", "-I", iface, "-c", "1", "-W", "1", ip}, 1.2);
      }
    });
  }
  for (auto& worker : workers) {
    worker.join();
  }

  const auto gateways = gateway_ips(iface);
  auto entries = neighbor_entries(iface);
  for (auto& entry : entries) {
    if (entry.ip == result.iface_ip ||
        std::find(gateways.begin(), gateways.end(), entry.ip) != gateways.end()) {
      entry.score = -1;
      continue;
    }
    entry.ttl = ping_ttl(iface, entry.ip);
    for (const char* prefix : kLivoxMacPrefixes) {
      if (entry.mac.rfind(prefix, 0) == 0) {
        entry.score += 100;
      }
    }
    if (entry.ttl >= 200) {
      entry.score += 40;
    }
    if (entry.ip.rfind("192.168.1.", 0) == 0) {
      entry.score += 5;
    }
  }
  entries.erase(std::remove_if(entries.begin(), entries.end(), [](const Neighbor& item) {
    return item.score < 0;
  }), entries.end());
  std::sort(entries.begin(), entries.end(), [](const Neighbor& a, const Neighbor& b) {
    if (a.score != b.score) {
      return a.score > b.score;
    }
    return a.state == "REACHABLE";
  });

  if (!entries.empty()) {
    std::ostringstream preview;
    for (size_t i = 0; i < std::min<size_t>(entries.size(), 5); ++i) {
      if (i) {
        preview << ", ";
      }
      preview << entries[i].ip << " ttl=" << (entries[i].ttl >= 0 ? std::to_string(entries[i].ttl) : "N/A")
              << " mac=" << entries[i].mac << " score=" << entries[i].score;
    }
    progress("active scan candidates: " + preview.str());
  }
  if (!entries.empty() && entries.front().score >= kMinActiveScanScore) {
    result.lidar_ip = entries.front().ip;
  } else if (!entries.empty()) {
    progress("active scan found online devices, but none match known MID360/Livox signatures");
  }
  return result;
}

Discovery sniff(const std::string& iface, double timeout_sec, const Options& options) {
  Discovery result;
  result.iface_ip = iface_ipv4(iface);

  std::string tcpdump = "/usr/sbin/tcpdump";
  if (!file_exists(tcpdump)) {
    tcpdump = "tcpdump";
  }
  std::vector<std::string> command;
  if (!options.no_sudo && geteuid() != 0) {
    command.push_back("sudo");
  }
  command.push_back(tcpdump);
  command.push_back("-lni");
  command.push_back(iface);
  command.push_back("(udp and port 56000) or arp");

  progress("listening on " + iface + " for Livox discovery packets (" + std::to_string(timeout_sec) + "s)");
  const std::string output = run_text(command, timeout_sec);
  std::regex udp_regex(R"(\bIP\s+(\d+\.\d+\.\d+\.\d+)\.56000\s+>)");
  std::regex arp_regex(R"(who-has\s+(\d+\.\d+\.\d+\.\d+)\s+tell\s+(\d+\.\d+\.\d+\.\d+))");
  for (const auto& line : split_lines(output)) {
    std::smatch match;
    if (std::regex_search(line, match, udp_regex)) {
      const std::string source_ip = match[1].str();
      if (source_ip != result.iface_ip) {
        result.lidar_ip = source_ip;
        result.method = "livox_discovery";
        result.raw_packets += 1;
      }
    }
    if (line.find("ARP,") != std::string::npos && std::regex_search(line, match, arp_regex)) {
      result.requested_host_ip = match[1].str();
      result.lidar_ip = match[2].str();
      result.method = "arp_observed";
    }
  }
  return result;
}

std::pair<std::string, Discovery> discover(const std::vector<std::string>& ifaces, const Options& options) {
  Discovery last;
  progress("candidate interfaces: " + [&]() {
    std::ostringstream out;
    for (size_t i = 0; i < ifaces.size(); ++i) {
      if (i) {
        out << ", ";
      }
      out << ifaces[i];
    }
    return out.str();
  }());
  for (const auto& iface : ifaces) {
    progress("checking interface " + iface + " by active scan");
    Discovery result = active_scan(iface, options);
    if (!result.lidar_ip.empty()) {
      progress("found lidar by active scan on " + iface + ": " + result.lidar_ip);
      return {iface, result};
    }
    last = result;
  }
  const double per_iface_timeout = ifaces.size() == 1 ? options.timeout_sec : std::max(1.5, std::min(options.timeout_sec, 2.0));
  for (const auto& iface : ifaces) {
    progress("checking interface " + iface + " by passive discovery fallback");
    Discovery result = sniff(iface, per_iface_timeout, options);
    if (!result.lidar_ip.empty()) {
      progress("found lidar by passive discovery on " + iface + ": " + result.lidar_ip);
      return {iface, result};
    }
    last = result;
  }
  return {ifaces.empty() ? "eth0" : ifaces.back(), last};
}

std::vector<size_t> parse_selection(const std::string& answer, size_t count) {
  std::vector<size_t> selected;
  const std::string normalized = [&]() {
    std::string out = answer;
    for (char& ch : out) {
      if (ch == ',' || ch == ';') {
        ch = ' ';
      }
    }
    return trim(out);
  }();
  if (normalized.empty()) {
    return selected;
  }
  if (lower_copy(normalized) == "all" || normalized == "*") {
    selected.resize(count);
    for (size_t i = 0; i < count; ++i) {
      selected[i] = i;
    }
    return selected;
  }

  std::set<size_t> unique;
  std::istringstream stream(normalized);
  std::string token;
  while (stream >> token) {
    const auto dash = token.find('-');
    auto add_index = [&](size_t value) {
      if (value == 0 || value > count) {
        throw std::runtime_error("selection out of range: " + std::to_string(value));
      }
      unique.insert(value - 1);
    };
    if (dash != std::string::npos) {
      const size_t begin = static_cast<size_t>(std::stoul(token.substr(0, dash)));
      const size_t end = static_cast<size_t>(std::stoul(token.substr(dash + 1)));
      if (begin > end) {
        throw std::runtime_error("invalid selection range: " + token);
      }
      for (size_t value = begin; value <= end; ++value) {
        add_index(value);
      }
    } else {
      add_index(static_cast<size_t>(std::stoul(token)));
    }
  }
  selected.assign(unique.begin(), unique.end());
  return selected;
}

ConfigView make_config_view(const std::vector<ConfigState>& states, bool show_all) {
  ConfigView view;
  const bool has_normal_priority = std::any_of(states.begin(), states.end(), [](const ConfigState& state) {
    return !state.low_priority;
  });
  for (const auto& state : states) {
    if (state.low_priority && !show_all && has_normal_priority) {
      view.hidden_count += 1;
      continue;
    }
    if (state.status == "mismatch") {
      view.recommended.push_back(state);
    } else if (state.status == "match") {
      view.matched.push_back(state);
    } else {
      view.other.push_back(state);
    }
  }
  return view;
}

std::vector<ConfigState> flatten_config_view(const ConfigView& view) {
  std::vector<ConfigState> out;
  out.insert(out.end(), view.recommended.begin(), view.recommended.end());
  out.insert(out.end(), view.matched.begin(), view.matched.end());
  out.insert(out.end(), view.other.begin(), view.other.end());
  return out;
}

size_t checked_size_for_states(const std::vector<ConfigState>& states) {
  size_t size = states.size();
  for (const auto& state : states) {
    size = std::max(size, state.original_index + 1);
  }
  return size;
}

std::vector<size_t> map_visible_selection_to_original(
    const std::vector<ConfigState>& states,
    const std::vector<size_t>& selected) {
  std::vector<size_t> out;
  out.reserve(selected.size());
  for (size_t index : selected) {
    if (index >= states.size()) {
      throw std::runtime_error("selection out of range: " + std::to_string(index + 1));
    }
    out.push_back(states[index].original_index);
  }
  return out;
}

std::string status_plain(const std::string& status) {
  if (status == "mismatch") {
    return "UPDATE";
  }
  if (status == "match") {
    return "MATCH";
  }
  if (status == "unavailable") {
    return "NO-IP";
  }
  return status.empty() ? "N/A" : status;
}

neon::Color status_color_for(const std::string& status) {
  if (status == "mismatch") {
    return neon::Color::Warning;
  }
  if (status == "match") {
    return neon::Color::Success;
  }
  if (status == "unavailable") {
    return neon::Color::Muted;
  }
  return neon::Color::Text;
}

std::vector<std::string> visible_group_labels(const ConfigView& view) {
  std::vector<std::string> labels;
  labels.insert(labels.end(), view.recommended.size(), "推荐更新");
  labels.insert(labels.end(), view.matched.size(), "已匹配");
  labels.insert(labels.end(), view.other.size(), "其它候选");
  return labels;
}

std::vector<std::string> render_candidate_rows(
    const std::vector<ConfigState>& visible_states,
    const std::vector<std::string>& labels,
    const std::vector<bool>& checked,
    size_t cursor,
    size_t first,
    size_t max_rows,
    int width) {
  std::vector<std::string> rows;
  rows.reserve(max_rows + 4);
  rows.push_back("上下方向键移动，空格选择/取消，回车确认，a 显示/隐藏低优先级候选，q 或 Esc 退出。");
  rows.push_back("");
  const int path_width = std::max(10, width - 39);
  for (size_t i = first; i < visible_states.size() && rows.size() < max_rows + 2; ++i) {
    const auto& state = visible_states[i];
    const bool active = i == cursor;
    const bool is_checked = state.original_index < checked.size() && checked[state.original_index];
    const std::string marker = active ? neon::text("▶", neon::Color::Accent, true) : " ";
    const std::string check = is_checked ? neon::text("[x]", neon::Color::Success, true) : "[ ]";
    const std::string index = neon::pad_left(std::to_string(i + 1), 2);
    const std::string group = neon::text(neon::pad_right(i < labels.size() ? labels[i] : "候选", 8), neon::Color::Muted, true);
    const std::string status = neon::text(neon::pad_right(status_plain(state.status), 7), status_color_for(state.status), true);
    rows.push_back(marker + " " + check + " " + index + " " + group + " " + status + " " +
                   neon::fit(compact_path(state.path, static_cast<size_t>(path_width)), path_width));
  }
  if (visible_states.empty()) {
    rows.push_back(neon::text("当前可见列表为空。", neon::Color::Warning, true));
  }
  return rows;
}

std::string render_config_picker(
    const std::vector<ConfigState>& states,
    const DetectionSummary& summary,
    bool show_all,
    size_t cursor,
    const std::vector<bool>& checked) {
  const ConfigView view = make_config_view(states, show_all);
  const std::vector<ConfigState> visible_states = flatten_config_view(view);
  const std::vector<std::string> labels = visible_group_labels(view);
  const neon::Size term = neon::terminal_size();
  const int width = std::max(48, term.cols);
  std::ostringstream out;
  out << neon::clear_screen()
      << neon::header("LIVOX MID-360 AUTOCONFIG", "CONFIG", width);
  int used_rows = 3;
  if (term.rows < 16 || term.cols < 58) {
    neon::append_line(out, neon::text("Terminal too small for Autoconfig TUI", neon::Color::Warning, true), width, used_rows);
    neon::append_line(out, "Resize to at least 58x16.", width, used_rows);
    neon::append_footer_at_bottom(out, "[ENTER] APPLY   [Q/ESC] QUIT", term.rows, width, used_rows);
    return out.str();
  }

  const size_t selected_count = static_cast<size_t>(std::count(checked.begin(), checked.end(), true));
  const std::string selection_line = "VISIBLE " + std::to_string(visible_states.size()) +
      " / TOTAL " + std::to_string(states.size()) +
      " / SELECTED " + std::to_string(selected_count);
  const std::string hidden_line = view.hidden_count == 0
      ? "LOW PRIORITY shown"
      : "LOW PRIORITY folded: " + std::to_string(view.hidden_count) + " (press a)";

  if (term.cols >= 104) {
    const int gap = 1;
    const int left_w = neon::clamp_int(term.cols / 3, 33, 44);
    const int right_w = std::max(58, term.cols - left_w - gap);
    const int max_candidate_rows = std::max(1, term.rows - used_rows - 6);
    size_t first = 0;
    if (!visible_states.empty() && cursor >= static_cast<size_t>(max_candidate_rows)) {
      first = cursor - static_cast<size_t>(max_candidate_rows) + 1;
    }
    std::vector<std::string> left_rows = detection_rows(summary);
    left_rows.push_back("");
    left_rows.push_back(selection_line);
    left_rows.push_back(hidden_line);
    auto left = neon::box("DEVICE IDENTITY", left_rows, left_w);
    auto right = neon::box(
        "CONFIG CANDIDATES",
        render_candidate_rows(visible_states, labels, checked, cursor, first, static_cast<size_t>(max_candidate_rows), right_w),
        right_w);
    neon::append_lines(out, neon::hstack(left, right, left_w, right_w, gap), width, term.rows - used_rows - 2, used_rows);
  } else {
    const int panel_w = width;
    const int detection_budget = std::min(7, std::max(3, term.rows / 4));
    std::vector<std::string> top_rows = detection_rows(summary);
    if (static_cast<int>(top_rows.size()) > detection_budget) {
      top_rows.resize(static_cast<size_t>(detection_budget));
    }
    top_rows.push_back(selection_line);
    top_rows.push_back(hidden_line);
    const auto identity = neon::box("DEVICE IDENTITY", top_rows, panel_w);
    if (term.rows - used_rows - 2 >= static_cast<int>(identity.size()) + 5) {
      neon::append_lines(out, identity, width, term.rows - used_rows - 2, used_rows);
    }
    const int max_candidate_rows = std::max(1, term.rows - used_rows - 6);
    size_t first = 0;
    if (!visible_states.empty() && cursor >= static_cast<size_t>(max_candidate_rows)) {
      first = cursor - static_cast<size_t>(max_candidate_rows) + 1;
    }
    neon::append_lines(
        out,
        neon::box(
            "CONFIG CANDIDATES",
            render_candidate_rows(visible_states, labels, checked, cursor, first, static_cast<size_t>(max_candidate_rows), panel_w),
            panel_w),
        width,
        term.rows - used_rows - 2,
        used_rows);
  }
  if (!visible_states.empty() && used_rows < term.rows - 1) {
    neon::append_line(
        out,
        neon::text("PATH ", neon::Color::Muted, true) + neon::fit(visible_states[cursor].path, std::max(8, width - 5)),
        width,
        used_rows);
  }
  neon::append_footer_at_bottom(out, "[↑/↓] MOVE   [SPACE] SELECT   [A] LOW-PRIORITY   [ENTER] APPLY   [Q/ESC] QUIT", term.rows, width, used_rows);
  return out.str();
}

void print_config_entry(size_t index, const ConfigState& state, bool cursor, bool checked) {
  std::cout << (cursor ? cyan("> ") : "  ")
            << "[" << (checked ? green("x") : " ") << "] "
            << "[" << index << "] " << compact_path(state.path) << "\n"
            << "      current=" << (state.configured_ip.empty() ? dim("N/A") : state.configured_ip)
            << "  status=" << status_label(state.status);
  if (state.low_priority) {
    std::cout << "  " << dim("sample/low-priority");
  }
  std::cout << "\n";
}

void print_config_section(
    const std::string& title,
    const std::vector<ConfigState>& states,
    size_t& index,
    size_t cursor,
    const std::vector<bool>* checked) {
  if (states.empty()) {
    return;
  }
  std::cout << "\n" << bold(title) << "\n";
  for (const auto& state : states) {
    const bool is_cursor = index == cursor;
    const bool is_checked = checked &&
        state.original_index < checked->size() &&
        (*checked)[state.original_index];
    print_config_entry(index + 1, state, is_cursor, is_checked);
    index += 1;
  }
}

void print_hidden_hint(size_t hidden_count) {
  if (hidden_count == 0) {
    return;
  }
  std::cout << "\n" << dim("其它低优先级候选已折叠: " + std::to_string(hidden_count) + " 个，按 a 显示全部。") << "\n";
}

void print_config_table(const std::vector<ConfigState>& states, bool show_all = false) {
  std::cout << "\n" << bold("可更新的 MID360 配置文件") << "\n";
  if (states.empty()) {
    std::cout << "  未发现带有雷达 IP 的配置文件。\n";
    return;
  }
  const ConfigView view = make_config_view(states, show_all);
  size_t index = 0;
  print_config_section("推荐更新", view.recommended, index, std::numeric_limits<size_t>::max(), nullptr);
  print_config_section("已匹配", view.matched, index, std::numeric_limits<size_t>::max(), nullptr);
  print_config_section("其它候选", view.other, index, std::numeric_limits<size_t>::max(), nullptr);
  if (index == 0) {
    std::cout << "  当前可见列表为空。\n";
  }
  print_hidden_hint(view.hidden_count);
}

std::string detection_value(const std::string& value) {
  return value.empty() ? "N/A" : value;
}

std::vector<std::string> detection_rows(const DetectionSummary& summary) {
  const auto& result = summary.result;
  return {
      "IFACE        " + neon::text(detection_value(summary.iface), neon::Color::Accent, true),
      "IFACE_IP     " + detection_value(result.iface_ip),
      "LIDAR_IP     " + neon::text(detection_value(result.lidar_ip), result.lidar_ip.empty() ? neon::Color::Danger : neon::Color::Success, true),
      "ARP_HOST     " + detection_value(result.requested_host_ip),
      "PACKETS      " + std::to_string(result.raw_packets),
      "METHOD       " + detection_value(result.method),
      "",
      neon::badge("HEALTH", result.lidar_ip.empty() ? "WAIT" : "FOUND", result.lidar_ip.empty() ? neon::Color::Warning : neon::Color::Success),
      neon::badge("MODE", "AUTOCONFIG", neon::Color::Accent),
  };
}

void print_detection_summary(const DetectionSummary& summary) {
  const auto& result = summary.result;
  std::cout << bold("检测结果") << "\n";
  std::cout << "  iface:           " << cyan(summary.iface) << "\n";
  std::cout << "  iface_ip:        " << (result.iface_ip.empty() ? "N/A" : result.iface_ip) << "\n";
  std::cout << "  lidar_ip:        " << (result.lidar_ip.empty() ? red("N/A") : green(result.lidar_ip)) << "\n";
  std::cout << "  broadcast_code:  N/A\n";
  std::cout << "  arp_host_ip:     " << (result.requested_host_ip.empty() ? "N/A" : result.requested_host_ip) << "\n";
  std::cout << "  discovery_pkts:  " << result.raw_packets << "\n";
  std::cout << "  detect_method:   " << (result.method.empty() ? "N/A" : result.method) << "\n";
}

void enter_screen() {
  if (!isatty(STDOUT_FILENO)) {
    return;
  }
  if (isatty(STDIN_FILENO) && !g_has_original_termios && tcgetattr(STDIN_FILENO, &g_original_termios) == 0) {
    termios raw = g_original_termios;
    raw.c_lflag &= static_cast<unsigned>(~(ICANON | ECHO));
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0) {
      g_has_original_termios = true;
    }
  }
  std::cout << neon::enter_alt_screen() << std::flush;
  g_screen_active = true;
}

void leave_screen() {
  if (!g_screen_active) {
    return;
  }
  g_screen_active = false;
  if (g_has_original_termios) {
    tcsetattr(STDIN_FILENO, TCSANOW, &g_original_termios);
    g_has_original_termios = false;
  }
  std::cout << neon::leave_alt_screen() << std::flush;
}

std::vector<size_t> choose_configs_interactively(
    const std::vector<ConfigState>& states,
    const DetectionSummary& summary,
    bool initial_show_all) {
  if (states.empty()) {
    if (g_screen_active) {
      const neon::Size term = neon::terminal_size();
      std::ostringstream out;
      out << neon::clear_screen()
          << neon::header("LIVOX MID-360 AUTOCONFIG", "CONFIG", std::max(40, term.cols));
      std::vector<std::string> rows = detection_rows(summary);
      rows.push_back("");
      rows.push_back(neon::text("未发现带有雷达 IP 的配置文件。", neon::Color::Warning, true));
      for (const auto& row : neon::box("DEVICE IDENTITY", rows, std::max(40, term.cols))) {
        out << row << "\n";
      }
      out << neon::footer("[Q/ESC] QUIT", std::max(40, term.cols)) << "\n";
      std::cout << out.str() << std::flush;
    } else {
      print_detection_summary(summary);
      print_config_table(states, initial_show_all);
    }
    return {};
  }

  termios original {};
  if (tcgetattr(STDIN_FILENO, &original) != 0) {
    print_detection_summary(summary);
    print_config_table(states, initial_show_all);
    std::cout << "\n输入要修改的编号，支持空格/逗号/范围，例如: 1 3 或 1-3；输入 all 全选；直接回车退出不修改。\n";
    std::cout << "选择: ";
    std::string answer;
    std::getline(std::cin, answer);
    answer = trim(answer);
    if (answer.empty()) {
      return {};
    }
    return map_visible_selection_to_original(states, parse_selection(answer, states.size()));
  }
  termios raw = original;
  raw.c_lflag &= static_cast<unsigned>(~(ICANON | ECHO));
  raw.c_cc[VMIN] = 1;
  raw.c_cc[VTIME] = 0;
  if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) {
    print_config_table(states, initial_show_all);
    return {};
  }

  size_t cursor = 0;
  bool show_all = initial_show_all;
  std::vector<ConfigState> visible_states = flatten_config_view(make_config_view(states, show_all));
  std::vector<bool> checked(checked_size_for_states(states), false);
  bool done = false;
  bool cancelled = false;

  auto redraw = [&]() {
    visible_states = flatten_config_view(make_config_view(states, show_all));
    if (visible_states.empty()) {
      cursor = 0;
    } else if (cursor >= visible_states.size()) {
      cursor = visible_states.size() - 1;
    }
    std::cout << render_config_picker(states, summary, show_all, cursor, checked);
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
      cancelled = true;
      break;
    }
    if (ch == '\033') {
      char left = 0;
      char right = 0;
      if (read_char_timeout(left, 50) && read_char_timeout(right, 50) && left == '[') {
        if (right == 'A') {
          if (!visible_states.empty()) {
            cursor = cursor == 0 ? visible_states.size() - 1 : cursor - 1;
          }
        } else if (right == 'B') {
          if (!visible_states.empty()) {
            cursor = (cursor + 1) % visible_states.size();
          }
        }
      } else {
        cancelled = true;
        done = true;
      }
    } else if (ch == ' ') {
      if (!visible_states.empty()) {
        const size_t original_index = visible_states[cursor].original_index;
        if (original_index < checked.size()) {
          checked[original_index] = !checked[original_index];
        }
      }
    } else if (ch == '\n' || ch == '\r') {
      done = true;
    } else if (ch == 'a' || ch == 'A') {
      show_all = !show_all;
    } else if (ch == 'q' || ch == 'Q') {
      cancelled = true;
      done = true;
    }
    if (!done) {
      redraw();
    }
  }
  if (!g_has_original_termios) {
    tcsetattr(STDIN_FILENO, TCSANOW, &original);
  }
  std::cout << neon::clear_screen();
  if (cancelled) {
    return {};
  }
  std::vector<size_t> selected;
  for (size_t i = 0; i < checked.size(); ++i) {
    if (checked[i]) {
      selected.push_back(i);
    }
  }
  return selected;
}

void usage(const char* argv0) {
  std::cout
      << "usage: " << argv0 << " [options]\n"
      << "\n"
      << "Discover a Livox MID360-like lidar and optionally update MID360_config.json.\n"
      << "\n"
      << "options:\n"
      << "  -i, --iface IFACE       interface to scan, or auto (default: auto)\n"
      << "  -t, --timeout SEC       passive discovery timeout (default: 8)\n"
      << "  --config PATH           MID360_config.json path; can be repeated\n"
      << "                          also supports LIVOX_MID360_CONFIG or MID360_CONFIG\n"
      << "  --apply                 update config files without the interactive picker\n"
      << "  --yes                   do not prompt when used with --apply\n"
      << "  --require-match         return non-zero if config is unavailable/mismatched\n"
      << "  --show-all              include sample/build/dist config candidates in the picker\n"
      << "  --no-color              disable ANSI colors\n"
      << "  --no-sudo               do not prefix tcpdump with sudo\n"
      << "  -v, --verbose           print debug output\n"
      << "  -h, --help              show this help\n";
}

Options parse_args(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto need_value = [&](const std::string& name) -> std::string {
      if (i + 1 >= argc) {
        throw std::runtime_error("missing value for " + name);
      }
      return argv[++i];
    };
    if (arg == "-h" || arg == "--help") {
      usage(argv[0]);
      std::exit(0);
    } else if (arg == "-i" || arg == "--iface") {
      options.iface = need_value(arg);
    } else if (arg == "-t" || arg == "--timeout") {
      options.timeout_sec = std::stod(need_value(arg));
    } else if (arg == "--config") {
      options.configs.push_back(need_value(arg));
    } else if (arg == "--apply") {
      options.apply = true;
    } else if (arg == "--yes") {
      options.yes = true;
    } else if (arg == "--no-sudo") {
      options.no_sudo = true;
    } else if (arg == "-v" || arg == "--verbose") {
      options.verbose = true;
    } else if (arg == "--require-match") {
      options.require_match = true;
    } else if (arg == "--show-all") {
      options.show_all = true;
    } else if (arg == "--no-color") {
      options.no_color = true;
    } else {
      throw std::runtime_error("unknown argument: " + arg);
    }
  }
  return options;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    std::signal(SIGINT, handle_interrupt);
    const Options options = parse_args(argc, argv);
    init_theme(options.no_color);
    neon::set_color_enabled(!options.no_color);
    const bool interactive_picker = !options.apply && isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
    if (interactive_picker) {
      enter_screen();
      render_scan_screen("preparing config search");
    }
    const auto config_paths = resolve_config_paths(options);
    throw_if_interrupted();
    const auto ifaces = candidate_ifaces(options, config_paths);

    progress("starting MID360 discovery");
    const auto [iface, result] = discover(ifaces, options);
    throw_if_interrupted();
    const DetectionSummary summary{iface, result};
    if (!interactive_picker) {
      print_detection_summary(summary);
    }

    if (result.lidar_ip.empty()) {
      leave_screen();
      std::cerr << "ERROR: no MID360 lidar IP found by passive discovery or active scan\n";
      return 2;
    }

    bool needs_update = false;
    bool unavailable = false;
    std::vector<ConfigState> states;
    for (const auto& path : config_paths) {
      std::string configured_ip;
      try {
        configured_ip = read_config_lidar_ip(path);
      } catch (const std::exception& exc) {
        leave_screen();
        std::cerr << "ERROR: " << exc.what() << "\n";
        return 2;
      }
      std::string status;
      if (!configured_ip.empty() && configured_ip != result.lidar_ip) {
        needs_update = true;
        status = "mismatch";
      } else if (configured_ip == result.lidar_ip) {
        status = "match";
      } else {
        unavailable = true;
        status = "unavailable";
      }
      states.push_back({path, configured_ip, status, is_low_priority_config_path(path), states.size()});
    }

    bool updated_any = false;
    if (options.apply) {
      for (const auto& state : states) {
        if (!regular_file_exists(state.path)) {
          std::cerr << "ERROR: config file not found: " << state.path << "\n";
          return 2;
        }
        if (state.configured_ip == result.lidar_ip) {
          continue;
        }
        bool should_update = options.yes || !isatty(STDIN_FILENO);
        if (!should_update) {
          std::cout << "Update " << state.path << " lidar IP from "
                    << (state.configured_ip.empty() ? "N/A" : state.configured_ip) << " to " << result.lidar_ip << "? [y/N]: ";
          std::string answer;
          std::getline(std::cin, answer);
          should_update = answer == "y" || answer == "yes" || answer == "Y" || answer == "YES";
        }
        if (should_update) {
          if (!update_config(state.path, result.lidar_ip, result.iface_ip)) {
            std::cerr << "ERROR: failed to update " << state.path << "\n";
            return 2;
          }
          std::cout << "updated: " << state.path << "\n";
          updated_any = true;
        } else {
          std::cout << "skipped: " << state.path << "\n";
        }
      }
    } else if (isatty(STDIN_FILENO)) {
      std::vector<ConfigState> visible_states;
      for (const auto& state : states) {
        if (!state.configured_ip.empty()) {
          visible_states.push_back(state);
        }
      }
      if (visible_states.empty()) {
        visible_states = states;
      }
      const std::vector<size_t> selected = choose_configs_interactively(visible_states, summary, options.show_all);
      if (selected.empty()) {
        leave_screen();
        std::cout << "未选择配置文件，退出不修改。\n";
      }
      for (size_t index : selected) {
        const auto found = std::find_if(states.begin(), states.end(), [&](const ConfigState& state) {
          return state.original_index == index;
        });
        if (found == states.end()) {
          leave_screen();
          std::cerr << "ERROR: invalid config selection index: " << index << "\n";
          return 2;
        }
        const auto& state = *found;
        if (!regular_file_exists(state.path)) {
          leave_screen();
          std::cerr << "ERROR: config file not found: " << state.path << "\n";
          return 2;
        }
        if (!update_config(state.path, result.lidar_ip, result.iface_ip)) {
          leave_screen();
          std::cerr << "ERROR: failed to update " << state.path << "\n";
          return 2;
        }
        leave_screen();
        std::cout << "updated: " << state.path << "\n";
        updated_any = true;
      }
    } else {
      std::vector<ConfigState> visible_states;
      for (const auto& state : states) {
        if (!state.configured_ip.empty()) {
          visible_states.push_back(state);
        }
      }
      print_config_table(visible_states, options.show_all);
      std::cout << "非交互式终端不会修改配置；需要自动写入时请显式使用 --config PATH --apply --yes。\n";
    }

    if (options.require_match && (needs_update || unavailable) && !updated_any) {
      leave_screen();
      std::cerr << "ERROR: detected lidar IP cannot be verified against selected MID360 config\n";
      return 3;
    }
    leave_screen();
    return 0;
  } catch (const std::exception& exc) {
    leave_screen();
    if (std::string(exc.what()) == "interrupted") {
      std::cerr << "Interrupted.\n";
      return 130;
    }
    std::cerr << "ERROR: " << exc.what() << "\n";
    return 2;
  }
}
