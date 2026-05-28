#include "livox_lidar_api.h"
#include "livox_lidar_def.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <unistd.h>

namespace {

struct Counters {
  uint64_t point_packets = 0;
  uint64_t point_units = 0;
  uint64_t point_bytes = 0;
  uint64_t imu_packets = 0;
  uint64_t imu_units = 0;
  uint64_t imu_bytes = 0;
  uint8_t last_point_type = 255;
  uint8_t last_point_frame = 0;
  uint16_t last_point_udp = 0;
  uint32_t handle = 0;
  uint8_t dev_type = 0;
  std::string sn;
  std::string lidar_ip;
  std::string control_status;
  int lidar_diag_status = -1;
  std::chrono::steady_clock::time_point last_callback;
};

struct Options {
  std::string config_path;
  std::string iface = "auto";
  std::string host_ip;
  double discovery_timeout_sec = 5.0;
  double duration_sec = 0.0;
  double interval_sec = 1.0;
  bool enable_point_send = false;
  bool set_normal_mode = false;
  bool enable_imu = false;
  bool quiet_sdk_log = true;
};

struct InterfaceInfo {
  std::string name;
  std::string ip;
  int prefix = 0;
};

struct SdkSession {
  bool initialized = false;

  ~SdkSession() {
    if (initialized) {
      LivoxLidarSdkUninit();
    }
  }

  void stop() {
    if (initialized) {
      LivoxLidarSdkUninit();
      initialized = false;
    }
  }
};

struct TempConfig {
  std::string path;

  TempConfig() = default;
  explicit TempConfig(std::string value) : path(std::move(value)) {}
  TempConfig(const TempConfig&) = delete;
  TempConfig& operator=(const TempConfig&) = delete;

  TempConfig(TempConfig&& other) noexcept : path(std::move(other.path)) {
    other.path.clear();
  }

  TempConfig& operator=(TempConfig&& other) noexcept {
    if (this != &other) {
      if (!path.empty()) {
        std::remove(path.c_str());
      }
      path = std::move(other.path);
      other.path.clear();
    }
    return *this;
  }

  ~TempConfig() {
    if (!path.empty()) {
      std::remove(path.c_str());
    }
  }
};

std::mutex g_mutex;
std::condition_variable g_cv;
Counters g_counters;
std::atomic<bool> g_running{true};
std::atomic<bool> g_seen_lidar{false};
std::atomic<bool> g_control_requested{false};
std::atomic<bool> g_terminal_active{false};
constexpr double kCallbackTimeoutSec = 3.0;
constexpr int kGroupWidth = 12;
constexpr int kItemWidth = 16;
constexpr int kValueWidth = 32;
constexpr int kTableInnerWidth = kGroupWidth + kItemWidth + kValueWidth + 8;

bool valid_ipv4(const std::string& ip) {
  in_addr addr {};
  return inet_pton(AF_INET, ip.c_str(), &addr) == 1;
}

bool should_skip_iface(const std::string& name, unsigned int flags) {
  if ((flags & IFF_UP) == 0 || (flags & IFF_LOOPBACK) != 0) {
    return true;
  }
  return name == "lo" ||
         name.rfind("docker", 0) == 0 ||
         name.rfind("br-", 0) == 0 ||
         name.rfind("veth", 0) == 0 ||
         name.rfind("virbr", 0) == 0;
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
    if (should_skip_iface(name, item->ifa_flags)) {
      continue;
    }

    char addr[INET_ADDRSTRLEN] {};
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

std::vector<InterfaceInfo> discovery_candidates(const Options& options) {
  if (!options.host_ip.empty()) {
    if (!valid_ipv4(options.host_ip)) {
      throw std::runtime_error("invalid --host-ip: " + options.host_ip);
    }
    for (const auto& iface : list_ipv4_interfaces()) {
      if (iface.ip == options.host_ip) {
        return {iface};
      }
    }
    return {{"manual", options.host_ip, 32}};
  }

  std::vector<InterfaceInfo> interfaces = list_ipv4_interfaces();
  if (options.iface != "auto") {
    auto found = std::find_if(interfaces.begin(), interfaces.end(), [&](const InterfaceInfo& item) {
      return item.name == options.iface;
    });
    if (found == interfaces.end()) {
      throw std::runtime_error("interface has no usable IPv4 address: " + options.iface);
    }
    return {*found};
  }

  std::stable_sort(interfaces.begin(), interfaces.end(), [](const InterfaceInfo& a, const InterfaceInfo& b) {
    const auto score = [](const InterfaceInfo& item) {
      int value = 0;
      if (item.name.rfind("eth", 0) == 0 || item.name.rfind("en", 0) == 0) {
        value += 20;
      }
      if (item.ip.rfind("192.168.1.", 0) == 0) {
        value += 10;
      }
      return value;
    };
    const int as = score(a);
    const int bs = score(b);
    if (as != bs) {
      return as > bs;
    }
    return a.name < b.name;
  });
  return interfaces;
}

const char* data_type_name(uint8_t type) {
  switch (type) {
    case kLivoxLidarImuData:
      return "imu";
    case kLivoxLidarCartesianCoordinateHighData:
      return "cartesian_high";
    case kLivoxLidarCartesianCoordinateLowData:
      return "cartesian_low";
    case kLivoxLidarSphericalCoordinateData:
      return "spherical";
    default:
      return "unknown";
  }
}

void signal_handler(int) {
  g_running = false;
  g_cv.notify_all();
}

void enter_terminal_dashboard() {
  if (!isatty(STDOUT_FILENO)) {
    return;
  }
  std::cout << "\033[?1049h\033[?25l\033[H\033[2J" << std::flush;
  g_terminal_active = true;
}

void leave_terminal_dashboard() {
  if (!g_terminal_active.exchange(false)) {
    return;
  }
  std::cout << "\033[?25h\033[?1049l" << std::flush;
}

bool terminal_dashboard_active() {
  return g_terminal_active.load();
}

void AsyncControlCallback(livox_status status, uint32_t handle, LivoxLidarAsyncControlResponse* response, void*) {
  std::lock_guard<std::mutex> lock(g_mutex);
  std::ostringstream message;
  message << "handle=" << handle << " status=" << status;
  if (response) {
    message << " ret_code=" << static_cast<unsigned>(response->ret_code)
            << " error_key=" << response->error_key;
  }
  g_counters.control_status = message.str();
}

std::string json_string_value(const std::string& text, const std::string& key) {
  const std::regex pattern("\"" + key + "\"\\s*:\\s*\"([^\"]*)\"");
  std::smatch match;
  if (std::regex_search(text, match, pattern)) {
    return match[1].str();
  }
  return "";
}

int json_int_value(const std::string& text, const std::string& key) {
  const std::regex pattern("\"" + key + "\"\\s*:\\s*(-?\\d+)");
  std::smatch match;
  if (std::regex_search(text, match, pattern)) {
    return std::stoi(match[1].str());
  }
  return -1;
}

void RequestControls(uint32_t handle, const Options& options) {
  if (!options.set_normal_mode && !options.enable_point_send && !options.enable_imu) {
    return;
  }
  bool expected = false;
  if (!g_control_requested.compare_exchange_strong(expected, true)) {
    return;
  }
  if (options.set_normal_mode) {
    SetLivoxLidarWorkMode(handle, kLivoxLidarNormal, AsyncControlCallback, nullptr);
  }
  if (options.enable_point_send) {
    EnableLivoxLidarPointSend(handle, AsyncControlCallback, nullptr);
  }
  if (options.enable_imu) {
    EnableLivoxLidarImuData(handle, AsyncControlCallback, nullptr);
  }
}

void LidarInfoChangeCallback(const uint32_t handle, const LivoxLidarInfo* info, void* client_data) {
  auto* options = static_cast<Options*>(client_data);
  if (!info) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_counters.handle = handle;
    g_counters.dev_type = info->dev_type;
    g_counters.sn = info->sn;
    g_counters.lidar_ip = info->lidar_ip;
    g_counters.last_callback = std::chrono::steady_clock::now();
  }
  g_seen_lidar = true;
  g_cv.notify_all();
  RequestControls(handle, *options);
}

void PointCloudCallback(const uint32_t handle, const uint8_t dev_type, LivoxLidarEthernetPacket* data, void*) {
  if (!data) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  g_counters.handle = handle;
  g_counters.dev_type = dev_type;
  g_counters.point_packets += 1;
  g_counters.point_units += data->dot_num;
  g_counters.point_bytes += data->length;
  g_counters.last_point_type = data->data_type;
  g_counters.last_point_frame = data->frame_cnt;
  g_counters.last_point_udp = data->udp_cnt;
  g_counters.last_callback = std::chrono::steady_clock::now();
}

void ImuDataCallback(const uint32_t handle, const uint8_t dev_type, LivoxLidarEthernetPacket* data, void*) {
  if (!data) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  g_counters.handle = handle;
  g_counters.dev_type = dev_type;
  g_counters.imu_packets += 1;
  g_counters.imu_units += data->dot_num;
  g_counters.imu_bytes += data->length;
  g_counters.last_callback = std::chrono::steady_clock::now();
}

void PushMsgCallback(const uint32_t handle, const uint8_t dev_type, const char* info, void* client_data) {
  if (!info) {
    return;
  }
  const std::string text(info);
  const std::string sn = json_string_value(text, "sn");
  const std::string lidar_ip = json_string_value(text, "lidar_ip");
  const int diag = json_int_value(text, "lidar_diag_status");
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_counters.handle = handle;
    g_counters.dev_type = dev_type;
    if (!sn.empty()) {
      g_counters.sn = sn;
    }
    if (!lidar_ip.empty()) {
      g_counters.lidar_ip = lidar_ip;
    }
    if (diag >= 0) {
      g_counters.lidar_diag_status = diag;
    }
    g_counters.last_callback = std::chrono::steady_clock::now();
  }
  g_seen_lidar = true;
  g_cv.notify_all();
  if (client_data) {
    RequestControls(handle, *static_cast<Options*>(client_data));
  }
}

struct ReportValues {
  double point_pps = 0.0;
  double points_per_sec = 0.0;
  double point_mbps = 0.0;
  double imu_pps = 0.0;
  double imu_samples_per_sec = 0.0;
  double imu_mbps = 0.0;
};

ReportValues make_report_values(const Counters& delta, double interval) {
  ReportValues values;
  values.point_pps = delta.point_packets / interval;
  values.points_per_sec = delta.point_units / interval;
  values.point_mbps = delta.point_bytes * 8.0 / interval / 1000000.0;
  values.imu_pps = delta.imu_packets / interval;
  values.imu_samples_per_sec = delta.imu_units / interval;
  values.imu_mbps = delta.imu_bytes * 8.0 / interval / 1000000.0;
  return values;
}

std::string health_text(int diag);
std::string connection_text(const Counters& snapshot, std::chrono::steady_clock::time_point now);

std::string fixed_number(double value, int precision) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(precision) << value;
  return out.str();
}

std::string format_scale_value(double value, const std::string& unit) {
  if (value >= 1000000.0) {
    return fixed_number(value / 1000000.0, 0) + "M" + unit;
  }
  if (value >= 1000.0) {
    return fixed_number(value / 1000.0, 0) + "k" + unit;
  }
  return fixed_number(value, 0) + unit;
}


std::string format_rate(double value, const std::string& suffix) {
  return fixed_number(value, 1) + " " + suffix;
}

std::string format_compact_rate(double value, const std::string& unit) {
  if (value >= 1000000.0) {
    return fixed_number(value / 1000000.0, 2) + " M" + unit;
  }
  if (value >= 1000.0) {
    return fixed_number(value / 1000.0, 1) + " k" + unit;
  }
  return fixed_number(value, 1) + " " + unit;
}

std::string format_duration(double seconds) {
  const auto total = static_cast<long long>(seconds);
  const long long hours = total / 3600;
  const long long minutes = (total % 3600) / 60;
  const long long secs = total % 60;
  std::ostringstream out;
  out << std::setfill('0') << std::setw(2) << hours << ":"
      << std::setw(2) << minutes << ":"
      << std::setw(2) << secs;
  return out.str();
}

std::string format_clock_time(std::chrono::system_clock::time_point time_point) {
  const std::time_t raw_time = std::chrono::system_clock::to_time_t(time_point);
  std::tm local_time {};
  localtime_r(&raw_time, &local_time);
  std::ostringstream out;
  out << std::put_time(&local_time, "%Y-%m-%d %H:%M:%S");
  return out.str();
}


std::string table_border(char left, char fill, char right) {
  return std::string(1, left) + std::string(kTableInnerWidth, fill) + std::string(1, right);
}


std::string fit_cell(const std::string& value, int width) {
  if (static_cast<int>(value.size()) <= width) {
    return value;
  }
  if (width <= 3) {
    return value.substr(0, static_cast<size_t>(width));
  }
  return value.substr(0, static_cast<size_t>(width - 3)) + "...";
}

void print_cell(std::ostream& out, const std::string& value, int width) {
  out << std::setw(width) << std::left << fit_cell(value, width) << std::right;
}

void print_table_row(
    std::ostream& out,
    const std::string& group,
    const std::string& item,
    const std::string& value) {
  out << "| ";
  print_cell(out, group, kGroupWidth);
  out << " | ";
  print_cell(out, item, kItemWidth);
  out << " | ";
  print_cell(out, value, kValueWidth);
  out << " |\n";
}

void print_table_title(std::ostream& out, const std::string& title) {
  const std::string label = fit_cell(title, kTableInnerWidth);
  const int padding = std::max(0, kTableInnerWidth - static_cast<int>(label.size()));
  const int left = padding / 2;
  const int right = padding - left;
  out << "|" << std::string(static_cast<size_t>(left), ' ')
      << label
      << std::string(static_cast<size_t>(right), ' ') << "|\n";
}

std::string health_text(int diag) {
  if (diag < 0) {
    return "N/A";
  }
  if (diag == 0) {
    return "OK (diag=0)";
  }
  return "WARN (diag=" + std::to_string(diag) + ")";
}

std::string connection_text(const Counters& snapshot, std::chrono::steady_clock::time_point now) {
  if (snapshot.last_callback.time_since_epoch().count() == 0) {
    return "WAITING";
  }
  const double age = std::chrono::duration<double>(now - snapshot.last_callback).count();
  if (age > kCallbackTimeoutSec) {
    return "LOST (no callbacks >" + fixed_number(kCallbackTimeoutSec, 0) + "s)";
  }
  return "LIVE";
}

void reset_monitor_state() {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_counters = Counters{};
  g_seen_lidar = false;
  g_control_requested = false;
}


void register_sdk_callbacks(Options& options) {
  SetLivoxLidarPointCloudCallBack(PointCloudCallback, nullptr);
  SetLivoxLidarImuDataCallback(ImuDataCallback, nullptr);
  SetLivoxLidarInfoCallback(PushMsgCallback, &options);
  SetLivoxLidarInfoChangeCallback(LidarInfoChangeCallback, &options);
}

bool wait_for_lidar(double timeout_sec) {
  std::unique_lock<std::mutex> lock(g_mutex);
  return g_cv.wait_for(
      lock,
      std::chrono::milliseconds(static_cast<int>(timeout_sec * 1000.0)),
      [] { return !g_running.load() || g_seen_lidar.load(); }) &&
      g_seen_lidar.load();
}

std::string describe_candidate(const InterfaceInfo& iface) {
  if (iface.name.empty() || iface.name == "manual") {
    return iface.ip;
  }
  return iface.name + " (" + iface.ip + "/" + std::to_string(iface.prefix) + ")";
}

TempConfig write_discovery_config(const InterfaceInfo& iface) {
  char path_template[] = "/tmp/livox_mid360_monitor_XXXXXX.json";
  const int fd = mkstemps(path_template, 5);
  if (fd < 0) {
    throw std::runtime_error("failed to create temporary SDK config");
  }
  close(fd);

  TempConfig config;
  config.path = path_template;
  std::ofstream out(config.path, std::ios::out | std::ios::trunc);
  if (!out) {
    throw std::runtime_error("failed to write temporary SDK config: " + config.path);
  }

  auto write_lidar_block = [&](const char* key) {
    out << "  \"" << key << "\": {\n"
        << "    \"lidar_net_info\": {\n"
        << "      \"cmd_data_port\": 56100,\n"
        << "      \"push_msg_port\": 56200,\n"
        << "      \"point_data_port\": 56300,\n"
        << "      \"imu_data_port\": 56400,\n"
        << "      \"log_data_port\": 56500\n"
        << "    },\n"
        << "    \"host_net_info\": [\n"
        << "      {\n"
        << "        \"host_ip\": \"" << iface.ip << "\",\n"
        << "        \"cmd_data_port\": 56101,\n"
        << "        \"push_msg_port\": 56201,\n"
        << "        \"point_data_port\": 56301,\n"
        << "        \"imu_data_port\": 56401,\n"
        << "        \"log_data_port\": 56501\n"
        << "      }\n"
        << "    ]\n"
        << "  }";
  };

  out << "{\n"
      << "  \"master_sdk\": true,\n"
      << "  \"lidar_log_enable\": false,\n"
      << "  \"lidar_log_cache_size_MB\": 0,\n"
      << "  \"lidar_log_path\": \"./\",\n";
  write_lidar_block("MID360");
  out << ",\n";
  write_lidar_block("Mid360s");
  out << "\n}\n";
  out.close();
  if (!out) {
    throw std::runtime_error("failed to finish temporary SDK config: " + config.path);
  }
  return config;
}

bool init_sdk_with_config(Options& options) {
  reset_monitor_state();
  if (!LivoxLidarSdkInit(options.config_path.c_str())) {
    return false;
  }
  register_sdk_callbacks(options);
  return true;
}

bool init_sdk_by_discovery(Options& options, InterfaceInfo& active_iface) {
  const std::vector<InterfaceInfo> candidates = discovery_candidates(options);
  if (candidates.empty()) {
    throw std::runtime_error("no usable IPv4 interfaces found; pass --host-ip or --iface");
  }

  std::cout << "discovery: Livox-SDK2 scan candidates:";
  for (const auto& candidate : candidates) {
    std::cout << " " << describe_candidate(candidate);
  }
  std::cout << "\n";

  for (const auto& candidate : candidates) {
    if (!g_running) {
      return false;
    }
    reset_monitor_state();
    std::cout << "discovery: listening on " << describe_candidate(candidate)
              << " for " << fixed_number(options.discovery_timeout_sec, 1) << "s" << std::endl;
    TempConfig temp_config = write_discovery_config(candidate);
    if (!LivoxLidarSdkInit(temp_config.path.c_str())) {
      LivoxLidarSdkUninit();
      std::cerr << "WARN: LivoxLidarSdkInit discovery failed on "
                << describe_candidate(candidate) << "\n";
      continue;
    }
    register_sdk_callbacks(options);
    if (wait_for_lidar(options.discovery_timeout_sec)) {
      active_iface = candidate;
      std::lock_guard<std::mutex> lock(g_mutex);
      std::cout << "discovery: found lidar"
                << " ip=" << (g_counters.lidar_ip.empty() ? "N/A" : g_counters.lidar_ip)
                << " sn=" << (g_counters.sn.empty() ? "N/A" : g_counters.sn)
                << " via " << describe_candidate(candidate) << std::endl;
      return true;
    }
    LivoxLidarSdkUninit();
  }
  return false;
}

void print_dashboard(
    const Counters& delta,
    double interval,
    double elapsed,
    const Counters& snapshot,
    std::chrono::steady_clock::time_point now,
    const Options& options) {
  const ReportValues values = make_report_values(delta, interval);
  std::ostringstream table;

  table << table_border('x', '=', 'x') << "\n";
  print_table_title(table, "Livox MID360 Monitor");
  table << table_border('|', '-', '|') << "\n";
  print_table_row(table, "GROUP", "ITEM", "VALUE");
  table << table_border('|', '-', '|') << "\n";

  print_table_row(table, "OVERVIEW", "time", format_clock_time(std::chrono::system_clock::now()));
  print_table_row(table, "OVERVIEW", "duration", format_duration(elapsed));
  print_table_row(table, "OVERVIEW", "mode", "monitor");
  print_table_row(table, "OVERVIEW", "source", options.config_path.empty() ? "sdk discovery" : "config");
  table << table_border('|', '-', '|') << "\n";

  print_table_row(table, "DEVICE", "status", connection_text(snapshot, now));
  print_table_row(table, "DEVICE", "health", health_text(snapshot.lidar_diag_status));
  print_table_row(table, "DEVICE", "sn", snapshot.sn.empty() ? "N/A" : snapshot.sn);
  print_table_row(table, "DEVICE", "ip", snapshot.lidar_ip.empty() ? "N/A" : snapshot.lidar_ip);
  print_table_row(table, "DEVICE", "handle", std::to_string(snapshot.handle));
  table << table_border('|', '-', '|') << "\n";

  print_table_row(table, "POINT", "packets", format_rate(values.point_pps, "pkt/s"));
  print_table_row(table, "POINT", "points", format_compact_rate(values.points_per_sec, "pt/s"));
  print_table_row(table, "POINT", "traffic", fixed_number(values.point_mbps, 2) + " Mbps");
  print_table_row(table, "POINT", "type", data_type_name(snapshot.last_point_type));
  print_table_row(table, "POINT", "frame", std::to_string(static_cast<unsigned>(snapshot.last_point_frame)));
  print_table_row(table, "POINT", "udp", std::to_string(snapshot.last_point_udp));
  table << table_border('|', '-', '|') << "\n";

  print_table_row(table, "IMU", "packets", format_rate(values.imu_pps, "pkt/s"));
  print_table_row(table, "IMU", "samples", format_rate(values.imu_samples_per_sec, "sample/s"));
  print_table_row(table, "IMU", "traffic", fixed_number(values.imu_mbps, 2) + " Mbps");
  table << table_border('|', '-', '|') << "\n";
  print_table_row(table, "CONTROL", "stop", "Ctrl-C");
  table << table_border('x', '=', 'x') << "\n";

  std::cout << "\033[H\033[2J" << table.str();
  std::cout.flush();
}

void print_line_report(
    const Counters& delta,
    double interval,
    double elapsed,
    const Counters& snapshot,
    std::chrono::steady_clock::time_point now) {
  const ReportValues values = make_report_values(delta, interval);
  std::cout << "elapsed=" << format_duration(elapsed)
            << " sn=" << (snapshot.sn.empty() ? "N/A" : snapshot.sn)
            << " ip=" << (snapshot.lidar_ip.empty() ? "N/A" : snapshot.lidar_ip)
            << " link=" << connection_text(snapshot, now)
            << " health=" << health_text(snapshot.lidar_diag_status)
            << " points=" << format_compact_rate(values.points_per_sec, "pt/s")
            << " point_packets=" << format_rate(values.point_pps, "pkt/s")
            << " point_mbps=" << fixed_number(values.point_mbps, 2)
            << " imu=" << format_rate(values.imu_samples_per_sec, "sample/s")
            << " type=" << data_type_name(snapshot.last_point_type)
            << " udp=" << snapshot.last_point_udp
            << std::endl;
}

void print_report(
    const Counters& delta,
    double interval,
    double elapsed,
    const Counters& snapshot,
    std::chrono::steady_clock::time_point now,
    const Options& options) {
  if (terminal_dashboard_active()) {
    print_dashboard(delta, interval, elapsed, snapshot, now, options);
    return;
  }
  print_line_report(delta, interval, elapsed, snapshot, now);
}

void usage(const char* argv0) {
  std::cout
      << "usage: " << argv0 << " [options]\n"
      << "\n"
      << "Live terminal monitor for Livox-SDK2 point cloud and IMU callbacks.\n"
      << "By default it discovers the lidar through Livox-SDK2 instead of using a configured lidar IP.\n"
      << "\n"
      << "options:\n"
      << "  --config PATH          optional MID360_config.json path; disables discovery mode\n"
      << "  -i, --iface IFACE      interface to use for SDK discovery, or auto (default: auto)\n"
      << "  --host-ip IP           host IPv4 address to bind for SDK discovery\n"
      << "  -t, --timeout SEC      per-interface discovery timeout (default: 5)\n"
      << "  --duration SEC         stop after N seconds; 0 means forever (default: 0)\n"
      << "  --interval SEC         report interval seconds (default: 1)\n"
      << "  --set-normal-mode      request kLivoxLidarNormal after lidar appears\n"
      << "  --enable-point-send    request point sending after lidar appears\n"
      << "  --enable-imu           request IMU sending after lidar appears\n"
      << "  --sdk-log              keep SDK console logger enabled\n"
      << "  -h, --help             show this help\n";
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
    } else if (arg == "--config") {
      options.config_path = need_value(arg);
    } else if (arg == "-i" || arg == "--iface") {
      options.iface = need_value(arg);
    } else if (arg == "--host-ip") {
      options.host_ip = need_value(arg);
    } else if (arg == "-t" || arg == "--timeout" || arg == "--discovery-timeout") {
      options.discovery_timeout_sec = std::stod(need_value(arg));
    } else if (arg == "--duration") {
      options.duration_sec = std::stod(need_value(arg));
    } else if (arg == "--interval") {
      options.interval_sec = std::stod(need_value(arg));
    } else if (arg == "--set-normal-mode") {
      options.set_normal_mode = true;
    } else if (arg == "--enable-point-send") {
      options.enable_point_send = true;
    } else if (arg == "--enable-imu") {
      options.enable_imu = true;
    } else if (arg == "--sdk-log") {
      options.quiet_sdk_log = false;
    } else {
      throw std::runtime_error("unknown argument: " + arg);
    }
  }
  if (!options.host_ip.empty() && options.iface != "auto") {
    throw std::runtime_error("--host-ip and --iface are mutually exclusive");
  }
  if (options.discovery_timeout_sec <= 0.0) {
    throw std::runtime_error("--timeout must be positive");
  }
  if (options.interval_sec <= 0.0) {
    throw std::runtime_error("--interval must be positive");
  }
  return options;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    Options options = parse_args(argc, argv);
    SdkSession sdk;
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    if (options.quiet_sdk_log) {
      DisableLivoxSdkConsoleLogger();
    }

    InterfaceInfo active_iface;
    if (!options.config_path.empty()) {
      if (!init_sdk_with_config(options)) {
        std::cerr << "ERROR: LivoxLidarSdkInit failed for " << options.config_path << "\n";
        LivoxLidarSdkUninit();
        return 2;
      }
      sdk.initialized = true;
      std::cout << "config: " << options.config_path << std::endl;
    } else {
      if (!init_sdk_by_discovery(options, active_iface)) {
        std::cerr << "ERROR: no MID360 lidar found by SDK discovery\n";
        return 2;
      }
      sdk.initialized = true;
    }

    enter_terminal_dashboard();

    const auto started = std::chrono::steady_clock::now();
    auto last = started;
    const auto report_interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(options.interval_sec));
    auto next_report = started + report_interval;
    Counters previous;

    while (g_running) {
      std::unique_lock<std::mutex> lock(g_mutex);
      g_cv.wait_until(lock, next_report, [] { return !g_running.load(); });
      if (!g_running) {
        break;
      }
      const auto now = std::chrono::steady_clock::now();
      const double interval = std::chrono::duration<double>(now - last).count();
      const double elapsed = std::chrono::duration<double>(now - started).count();
      Counters snapshot = g_counters;
      lock.unlock();
      while (next_report <= now) {
        next_report += report_interval;
      }

      Counters delta;
      delta.point_packets = snapshot.point_packets - previous.point_packets;
      delta.point_units = snapshot.point_units - previous.point_units;
      delta.point_bytes = snapshot.point_bytes - previous.point_bytes;
      delta.imu_packets = snapshot.imu_packets - previous.imu_packets;
      delta.imu_units = snapshot.imu_units - previous.imu_units;
      delta.imu_bytes = snapshot.imu_bytes - previous.imu_bytes;
      print_report(delta, std::max(0.001, interval), elapsed, snapshot, now, options);

      previous = snapshot;
      last = now;
      if (options.duration_sec > 0.0 && elapsed >= options.duration_sec) {
        break;
      }
    }

    sdk.stop();
    leave_terminal_dashboard();
    std::cout << "stopped" << std::endl;
    return 0;
  } catch (const std::exception& exc) {
    leave_terminal_dashboard();
    std::cerr << "ERROR: " << exc.what() << "\n";
    return 2;
  }
}
