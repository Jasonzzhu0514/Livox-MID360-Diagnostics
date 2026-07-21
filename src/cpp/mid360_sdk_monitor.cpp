#include "neon_tui.hpp"
#include "network_autobind.hpp"

#include "livox_lidar_api.h"
#include "livox_lidar_def.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <termios.h>

#ifdef LIVOX_MID360_HAS_NCURSES
#include <locale.h>
#include <ncursesw/curses.h>
#endif

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
#include <limits>
#include <mutex>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/ioctl.h>
#include <thread>
#include <utility>
#include <vector>
#include <unistd.h>

namespace {

struct NetEndpoint {
  std::string ip;
  int dst_port = -1;
  int src_port = -1;
};

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
  std::string lidar_mask;
  std::string lidar_gateway;
  NetEndpoint state_endpoint;
  NetEndpoint point_endpoint;
  NetEndpoint imu_endpoint;
  NetEndpoint ctl_endpoint;
  NetEndpoint log_endpoint;
  std::string product_info;
  std::string firmware_version;
  std::string loader_version;
  std::string hardware_version;
  std::string mac;
  std::string control_status;
  std::string status_code;
  int lidar_diag_status = -1;
  int core_temp = std::numeric_limits<int>::min();
  int environment_temp = std::numeric_limits<int>::min();
  int pcl_data_type = -1;
  int pattern_mode = -1;
  int dual_emit_en = -1;
  int frame_rate = -1;
  int fov_cfg_en = -1;
  int detect_mode = -1;
  int work_mode_after_boot = -1;
  int glass_heat = -1;
  int fusa_en = -1;
  int force_heat_en = -1;
  int esc_mode = -1;
  int pps_sync_mode = -1;
  int lidar_flash_status = -1;
  int fw_type = -1;
  int cur_glass_heat_state = -1;
  int roi_mode = -1;
  uint32_t powerup_cnt = 0;
  bool has_powerup_cnt = false;
  uint32_t blind_spot_set = 0;
  bool has_blind_spot_set = false;
  uint64_t local_time_now = 0;
  bool has_local_time_now = false;
  uint64_t last_sync_time = 0;
  bool has_last_sync_time = false;
  int64_t time_offset = 0;
  bool has_time_offset = false;
  int work_state = -1;
  int point_send_en = -1;
  int imu_data_en = -1;
  int time_sync_type = -1;
  uint32_t hms_code[8] {};
  bool has_hms_code = false;
  std::string internal_status = "waiting";
  std::chrono::steady_clock::time_point last_internal_info;
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
  bool auto_bind_livox_subnet = true;
  std::string auto_bind_ip = "192.168.1.5";
};

using InterfaceInfo = mid360_net::DiscoveryCandidate;

struct SdkSession {
  bool initialized = false;
  mid360_net::TemporaryIpv4Address temporary_host;

  ~SdkSession() {
    if (initialized) {
      LivoxLidarSdkUninit();
    }
    temporary_host.reset();
  }

  void stop() {
    if (initialized) {
      LivoxLidarSdkUninit();
      initialized = false;
    }
    temporary_host.reset();
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
std::atomic<bool> g_user_interrupted{false};
std::atomic<bool> g_interrupt_reported{false};
std::atomic<bool> g_seen_lidar{false};
std::atomic<bool> g_control_requested{false};
std::atomic<bool> g_reset_requested{false};
std::atomic<bool> g_terminal_active{false};
std::atomic<bool> g_internal_query_inflight{false};
std::chrono::steady_clock::time_point g_internal_query_started;
std::vector<std::string> g_discovery_lines;
neon::FrameClock g_discovery_frame_clock(std::chrono::milliseconds(100));
neon::LineDiffRenderer g_discovery_renderer;
neon::LineDiffRenderer g_live_renderer;
termios g_original_termios {};
bool g_has_original_termios = false;
#ifdef LIVOX_MID360_HAS_NCURSES
std::atomic<bool> g_curses_active{false};
#endif
constexpr double kCallbackTimeoutSec = 3.0;
constexpr double kTuiFrameSec = 1.0 / 20.0;
constexpr double kInternalQueryIntervalSec = 2.0;
constexpr double kInternalQueryTimeoutSec = 1.5;
constexpr int kReturnToMenuCode = 75;
constexpr int kGroupWidth = 12;
constexpr int kItemWidth = 16;
constexpr int kValueWidth = 32;
constexpr int kTableInnerWidth = kGroupWidth + kItemWidth + kValueWidth + 8;

enum ColorPair {
  kColorAccent = 1,
  kColorSuccess = 2,
  kColorWarning = 3,
  kColorDanger = 4,
  kColorMuted = 5,
  kColorText = 6,
  kColorFooter = 7,
  kColorSuccessBadge = 8,
  kColorWarningBadge = 9,
  kColorBackground = 10,
};

void leave_terminal_dashboard();
void print_discovery_dashboard(const std::string& message = "");
void refresh_discovery_dashboard(const std::string& message = "", bool heartbeat = false);
void discovery_log(const std::string& message, bool important = false);
std::string fixed_number(double value, int precision);

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
  g_user_interrupted = true;
  g_running = false;
  g_cv.notify_all();
}

void report_interrupted_once() {
  if (!g_interrupt_reported.exchange(true)) {
    std::cerr << "Interrupted.\n";
  }
}

void enter_terminal_dashboard() {
  if (!isatty(STDOUT_FILENO)) {
    return;
  }
  if (g_terminal_active.load()) {
    return;
  }
  neon::ensure_terminal_size();
#ifdef LIVOX_MID360_HAS_NCURSES
  setlocale(LC_ALL, "");
  initscr();
  cbreak();
  noecho();
  curs_set(0);
  keypad(stdscr, TRUE);
  nodelay(stdscr, TRUE);
  start_color();
  use_default_colors();
  const bool color_256 = COLORS >= 256;
  const short bg = color_256 ? 233 : -1;
  const short accent = color_256 ? 51 : COLOR_CYAN;
  const short success = color_256 ? 84 : COLOR_GREEN;
  const short warning = color_256 ? 220 : COLOR_YELLOW;
  const short danger = color_256 ? 196 : COLOR_RED;
  const short muted = color_256 ? 242 : COLOR_BLUE;
  const short text = color_256 ? 252 : COLOR_WHITE;
  init_pair(kColorAccent, accent, bg);
  init_pair(kColorSuccess, success, bg);
  init_pair(kColorWarning, warning, bg);
  init_pair(kColorDanger, danger, bg);
  init_pair(kColorMuted, muted, bg);
  init_pair(kColorText, text, bg);
  init_pair(kColorFooter, COLOR_BLACK, accent);
  init_pair(kColorSuccessBadge, COLOR_BLACK, success);
  init_pair(kColorWarningBadge, COLOR_BLACK, warning);
  init_pair(kColorBackground, text, bg);
  timeout(0);
  g_curses_active = true;
  g_terminal_active = true;
  g_discovery_frame_clock.reset();
  g_discovery_renderer.reset();
  g_live_renderer.reset();
  return;
#else
  if (isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &g_original_termios) == 0) {
    termios noecho = g_original_termios;
    noecho.c_lflag &= static_cast<unsigned>(~(ICANON | ECHO));
    noecho.c_cc[VMIN] = 1;
    noecho.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &noecho) == 0) {
      g_has_original_termios = true;
    }
  }
  std::cout << "\033[?1049h"
            << "\033[?1000l\033[?1002l\033[?1003l\033[?1006l"
            << "\033[?25l\033[48;5;233m\033[38;5;252m\033[2J\033[H" << std::flush;
  g_terminal_active = true;
  g_discovery_frame_clock.reset();
  g_discovery_renderer.reset();
  g_live_renderer.reset();
#endif
}

void enter_discovery_dashboard() {
  if (!isatty(STDOUT_FILENO)) {
    return;
  }
  if (g_terminal_active.load()) {
    return;
  }
  neon::ensure_terminal_size();
  if (isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &g_original_termios) == 0) {
    termios noecho = g_original_termios;
    noecho.c_lflag &= static_cast<unsigned>(~(ICANON | ECHO));
    noecho.c_cc[VMIN] = 1;
    noecho.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &noecho) == 0) {
      g_has_original_termios = true;
    }
  }
  std::cout << "\033[?1049h"
            << "\033[?1000l\033[?1002l\033[?1003l\033[?1006l"
            << "\033[?25l\033[48;5;233m\033[38;5;252m\033[2J\033[H" << std::flush;
  g_terminal_active = true;
  g_discovery_frame_clock.reset();
  g_discovery_renderer.reset();
  g_live_renderer.reset();
}

void leave_terminal_dashboard() {
  if (!g_terminal_active.exchange(false)) {
    return;
  }
#ifdef LIVOX_MID360_HAS_NCURSES
  if (g_curses_active.exchange(false)) {
    endwin();
  } else {
    if (g_has_original_termios) {
      tcsetattr(STDIN_FILENO, TCSANOW, &g_original_termios);
      g_has_original_termios = false;
    }
    std::cout << "\033[?1000l\033[?1002l\033[?1003l\033[?1006l\033[?25h\033[?1049l" << std::flush;
  }
#else
  if (g_has_original_termios) {
    tcsetattr(STDIN_FILENO, TCSANOW, &g_original_termios);
    g_has_original_termios = false;
  }
  std::cout << "\033[?1000l\033[?1002l\033[?1003l\033[?1006l\033[?25h\033[?1049l" << std::flush;
#endif
  g_discovery_renderer.reset();
  g_live_renderer.reset();
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

std::string json_object_value(const std::string& text, const std::string& key) {
  const std::string marker = "\"" + key + "\"";
  const size_t key_pos = text.find(marker);
  if (key_pos == std::string::npos) {
    return "";
  }
  const size_t colon = text.find(':', key_pos + marker.size());
  if (colon == std::string::npos) {
    return "";
  }
  const size_t open = text.find('{', colon + 1);
  if (open == std::string::npos) {
    return "";
  }
  int depth = 0;
  bool in_string = false;
  bool escape = false;
  for (size_t i = open; i < text.size(); ++i) {
    const char ch = text[i];
    if (escape) {
      escape = false;
      continue;
    }
    if (ch == '\\') {
      escape = in_string;
      continue;
    }
    if (ch == '"') {
      in_string = !in_string;
      continue;
    }
    if (in_string) {
      continue;
    }
    if (ch == '{') {
      ++depth;
    } else if (ch == '}') {
      --depth;
      if (depth == 0) {
        return text.substr(open, i - open + 1);
      }
    }
  }
  return "";
}

bool json_int_value(const std::string& text, const std::string& key, int& value) {
  const std::regex pattern("\"" + key + "\"\\s*:\\s*(-?\\d+)");
  std::smatch match;
  if (std::regex_search(text, match, pattern)) {
    value = std::stoi(match[1].str());
    return true;
  }
  return false;
}

bool json_int_value(const std::string& text, const std::string& key, int64_t& value) {
  const std::regex pattern("\"" + key + "\"\\s*:\\s*(-?\\d+)");
  std::smatch match;
  if (std::regex_search(text, match, pattern)) {
    value = std::stoll(match[1].str());
    return true;
  }
  return false;
}

int json_int_value(const std::string& text, const std::string& key) {
  int value = -1;
  return json_int_value(text, key, value) ? value : -1;
}

bool json_int_array_value(const std::string& text, const std::string& key, std::vector<uint32_t>& values) {
  const std::regex pattern("\"" + key + "\"\\s*:\\s*\\[([^\\]]*)\\]");
  std::smatch match;
  if (!std::regex_search(text, match, pattern)) {
    return false;
  }
  values.clear();
  const std::string body = match[1].str();
  const std::regex number_pattern("-?\\d+");
  for (std::sregex_iterator it(body.begin(), body.end(), number_pattern), end; it != end; ++it) {
    const long long value = std::stoll((*it)[0].str());
    values.push_back(static_cast<uint32_t>(std::max<long long>(0, value)));
  }
  return !values.empty();
}

std::string format_version_array(const std::vector<uint32_t>& values) {
  if (values.empty()) {
    return "";
  }
  std::ostringstream out;
  const size_t count = std::min<size_t>(4, values.size());
  for (size_t i = 0; i < count; ++i) {
    if (i > 0) {
      out << ".";
    }
    out << values[i];
  }
  return out.str();
}

std::string bytes_to_ipv4(const uint8_t* data, size_t size) {
  if (!data || size < 4) {
    return "";
  }
  std::ostringstream out;
  out << static_cast<unsigned>(data[0]) << "."
      << static_cast<unsigned>(data[1]) << "."
      << static_cast<unsigned>(data[2]) << "."
      << static_cast<unsigned>(data[3]);
  return out.str();
}

uint16_t read_u16_le(const uint8_t* data) {
  uint16_t value = 0;
  std::memcpy(&value, data, sizeof(value));
  return value;
}

void parse_lidar_ipcfg_bytes(Counters& counters, const uint8_t* data, size_t size) {
  if (!data || size < 12) {
    return;
  }
  counters.lidar_ip = bytes_to_ipv4(data, size);
  counters.lidar_mask = bytes_to_ipv4(data + 4, size - 4);
  counters.lidar_gateway = bytes_to_ipv4(data + 8, size - 8);
}

void parse_host_endpoint_bytes(NetEndpoint& endpoint, const uint8_t* data, size_t size) {
  if (!data || size < 8) {
    return;
  }
  endpoint.ip = bytes_to_ipv4(data, size);
  endpoint.dst_port = read_u16_le(data + 4);
  endpoint.src_port = read_u16_le(data + 6);
}

void parse_json_lidar_ipcfg(Counters& counters, const std::string& text) {
  const std::string object = json_object_value(text, "lidar_ipcfg");
  if (object.empty()) {
    return;
  }
  const std::string ip = json_string_value(object, "lidar_ip");
  const std::string mask = json_string_value(object, "lidar_subnet_mask");
  const std::string gateway = json_string_value(object, "lidar_gateway");
  if (!ip.empty()) {
    counters.lidar_ip = ip;
  }
  if (!mask.empty()) {
    counters.lidar_mask = mask;
  }
  if (!gateway.empty()) {
    counters.lidar_gateway = gateway;
  }
}

bool parse_json_endpoint(
    const std::string& text,
    const std::string& object_key,
    const std::string& dst_key,
    const std::string& src_key,
    NetEndpoint& endpoint) {
  const std::string object = json_object_value(text, object_key);
  if (object.empty()) {
    return false;
  }
  bool updated = false;
  const std::string ip = json_string_value(object, "ip");
  if (!ip.empty()) {
    endpoint.ip = ip;
    updated = true;
  }
  int dst = -1;
  if (json_int_value(object, dst_key, dst)) {
    endpoint.dst_port = dst;
    updated = true;
  }
  int src = -1;
  if (json_int_value(object, src_key, src)) {
    endpoint.src_port = src;
    updated = true;
  }
  return updated;
}

std::string format_version_bytes(const uint8_t* data, size_t size) {
  if (!data || size == 0) {
    return "";
  }
  std::ostringstream out;
  for (size_t i = 0; i < size; ++i) {
    if (i > 0) {
      out << ".";
    }
    out << static_cast<unsigned>(data[i]);
  }
  return out.str();
}

std::string format_mac_bytes(const uint8_t* data, size_t size) {
  if (!data || size < 6) {
    return "";
  }
  std::ostringstream out;
  out << std::hex << std::uppercase << std::setfill('0');
  for (size_t i = 0; i < 6; ++i) {
    if (i > 0) {
      out << ":";
    }
    out << std::setw(2) << static_cast<unsigned>(data[i]);
  }
  return out.str();
}

std::string format_mac_array(const std::vector<uint32_t>& values) {
  if (values.size() < 6) {
    return "";
  }
  std::ostringstream out;
  out << std::hex << std::uppercase << std::setfill('0');
  for (size_t i = 0; i < 6; ++i) {
    if (i > 0) {
      out << ":";
    }
    out << std::setw(2) << (values[i] & 0xff);
  }
  return out.str();
}

std::string format_status_bytes(const uint8_t* data, size_t size) {
  if (!data || size == 0) {
    return "";
  }
  const size_t count = std::min<size_t>(size, 32);
  std::ostringstream out;
  out << std::hex << std::uppercase << std::setfill('0');
  bool wrote = false;
  for (size_t i = 0; i < count; ++i) {
    if (data[i] == 0) {
      continue;
    }
    if (wrote) {
      out << ",";
    }
    out << "0x" << std::setw(2) << static_cast<unsigned>(data[i]);
    wrote = true;
  }
  return wrote ? out.str() : "0";
}

std::string format_status_array(const std::vector<uint32_t>& values) {
  if (values.empty()) {
    return "";
  }
  std::ostringstream out;
  out << std::hex << std::uppercase << std::setfill('0');
  bool wrote = false;
  for (uint32_t value : values) {
    if (value == 0) {
      continue;
    }
    if (wrote) {
      out << ",";
    }
    out << "0x" << std::setw(2) << (value & 0xff);
    wrote = true;
  }
  return wrote ? out.str() : "0";
}

std::string format_hms_code(const uint32_t* data, size_t count) {
  if (!data || count == 0) {
    return "";
  }
  std::ostringstream out;
  out << std::hex << std::uppercase << std::setfill('0');
  bool wrote = false;
  for (size_t i = 0; i < count; ++i) {
    if (data[i] == 0) {
      continue;
    }
    if (wrote) {
      out << ",";
    }
    out << "0x" << std::setw(8) << data[i];
    wrote = true;
  }
  return wrote ? out.str() : "0";
}

std::string work_state_text(int state) {
  switch (state) {
    case kLivoxLidarNormal:
      return "NORMAL";
    case kLivoxLidarWakeUp:
      return "WAKE_UP";
    case kLivoxLidarSleep:
      return "SLEEP";
    case kLivoxLidarError:
      return "ERROR";
    case kLivoxLidarPowerOnSelfTest:
      return "SELF_TEST";
    case kLivoxLidarMotorStarting:
      return "MOTOR_START";
    case kLivoxLidarMotorStoping:
      return "MOTOR_STOP";
    case kLivoxLidarUpgrade:
      return "UPGRADE";
    default:
      return state < 0 ? "N/A" : "UNKNOWN(" + std::to_string(state) + ")";
  }
}

std::string on_off_text(int value) {
  if (value < 0) {
    return "N/A";
  }
  return value == 0 ? "OFF" : "ON";
}

std::string temp_text(int value) {
  if (value == std::numeric_limits<int>::min()) {
    return "N/A";
  }
  if (std::abs(value) > 200) {
    return fixed_number(static_cast<double>(value) / 100.0, 1) + "C";
  }
  return std::to_string(value) + "C";
}

int temp_status_color(int value) {
  if (value == std::numeric_limits<int>::min()) {
    return kColorMuted;
  }
  const double celsius = std::abs(value) > 200 ? static_cast<double>(value) / 100.0 : static_cast<double>(value);
  if (celsius < 45.0) {
    return kColorSuccess;
  }
  if (celsius <= 60.0) {
    return kColorWarning;
  }
  return kColorDanger;
}

std::string time_sync_type_text(int value) {
  if (value < 0) {
    return "N/A";
  }
  switch (value) {
    case 0:
      return "NONE";
    case 1:
      return "PTP";
    case 2:
      return "GPS";
    case 3:
      return "PPS";
    default:
      return std::to_string(value);
  }
}

std::string firmware_text(const Counters& snapshot) {
  return snapshot.firmware_version.empty() ? "SDK2 LINK" : snapshot.firmware_version;
}

std::string internal_age_text(const Counters& snapshot, std::chrono::steady_clock::time_point now) {
  if (snapshot.last_internal_info.time_since_epoch().count() == 0) {
    return "N/A";
  }
  const double age = std::chrono::duration<double>(now - snapshot.last_internal_info).count();
  return fixed_number(std::max(0.0, age), 1) + "s ago";
}

std::string port_pair_text(const NetEndpoint& endpoint) {
  if (endpoint.dst_port < 0 && endpoint.src_port < 0) {
    return "N/A";
  }
  const std::string dst = endpoint.dst_port < 0 ? "?" : std::to_string(endpoint.dst_port);
  const std::string src = endpoint.src_port < 0 ? "?" : std::to_string(endpoint.src_port);
  return dst + "/" + src;
}

std::string endpoint_summary(const NetEndpoint& endpoint) {
  if (endpoint.ip.empty() && endpoint.dst_port < 0 && endpoint.src_port < 0) {
    return "N/A";
  }
  if (endpoint.ip.empty()) {
    return port_pair_text(endpoint);
  }
  return endpoint.ip + ":" + port_pair_text(endpoint);
}

std::string compact_endpoint_ip(const NetEndpoint& endpoint) {
  return endpoint.ip.empty() ? "N/A" : endpoint.ip;
}

std::string compact_endpoint_ports(const NetEndpoint& endpoint) {
  return port_pair_text(endpoint);
}

void parse_internal_kv(Counters& counters, const LivoxLidarKeyValueParam* kv) {
  if (!kv) {
    return;
  }
  auto copy_int32 = [&](int& target) {
    if (kv->length >= sizeof(int32_t)) {
      int32_t value = 0;
      std::memcpy(&value, kv->value, sizeof(value));
      target = value;
    }
  };
  auto copy_uint8 = [&](int& target) {
    if (kv->length >= sizeof(uint8_t)) {
      uint8_t value = 0;
      std::memcpy(&value, kv->value, sizeof(value));
      target = value;
    }
  };
  auto copy_uint16 = [&](int& target) {
    if (kv->length >= sizeof(uint16_t)) {
      uint16_t value = 0;
      std::memcpy(&value, kv->value, sizeof(value));
      target = value;
    }
  };
  auto copy_uint32 = [&](uint32_t& target, bool& present) {
    if (kv->length >= sizeof(uint32_t)) {
      uint32_t value = 0;
      std::memcpy(&value, kv->value, sizeof(value));
      target = value;
      present = true;
    }
  };
  auto copy_uint64 = [&](uint64_t& target, bool& present) {
    if (kv->length >= sizeof(uint64_t)) {
      uint64_t value = 0;
      std::memcpy(&value, kv->value, sizeof(value));
      target = value;
      present = true;
    }
  };
  auto copy_int64 = [&](int64_t& target, bool& present) {
    if (kv->length >= sizeof(int64_t)) {
      int64_t value = 0;
      std::memcpy(&value, kv->value, sizeof(value));
      target = value;
      present = true;
    }
  };

  switch (kv->key) {
    case kKeyPclDataType:
      copy_uint8(counters.pcl_data_type);
      break;
    case kKeyPatternMode:
      copy_uint8(counters.pattern_mode);
      break;
    case kKeyDualEmitEn:
      copy_uint8(counters.dual_emit_en);
      break;
    case kKeySn:
      counters.sn.assign(reinterpret_cast<const char*>(kv->value), strnlen(reinterpret_cast<const char*>(kv->value), kv->length));
      break;
    case kKeyProductInfo:
      counters.product_info.assign(
          reinterpret_cast<const char*>(kv->value),
          strnlen(reinterpret_cast<const char*>(kv->value), kv->length));
      break;
    case kKeyLidarIpCfg:
      parse_lidar_ipcfg_bytes(counters, kv->value, kv->length);
      break;
    case kKeyStateInfoHostIpCfg:
      parse_host_endpoint_bytes(counters.state_endpoint, kv->value, kv->length);
      break;
    case kKeyLidarPointDataHostIpCfg:
      parse_host_endpoint_bytes(counters.point_endpoint, kv->value, kv->length);
      break;
    case kKeyLidarImuHostIpCfg:
      parse_host_endpoint_bytes(counters.imu_endpoint, kv->value, kv->length);
      break;
    case kKeyCtlHostIpCfg:
      parse_host_endpoint_bytes(counters.ctl_endpoint, kv->value, kv->length);
      break;
    case kKeyLogHostIpCfg:
      parse_host_endpoint_bytes(counters.log_endpoint, kv->value, kv->length);
      break;
    case kKeyVersionApp:
      counters.firmware_version = format_version_bytes(kv->value, std::min<size_t>(kv->length, 4));
      break;
    case kKeyVersionLoader:
      counters.loader_version = format_version_bytes(kv->value, std::min<size_t>(kv->length, 4));
      break;
    case kKeyVersionHardware:
      counters.hardware_version = format_version_bytes(kv->value, std::min<size_t>(kv->length, 4));
      break;
    case kKeyMac:
      counters.mac = format_mac_bytes(kv->value, kv->length);
      break;
    case kKeyCurWorkState:
      copy_uint8(counters.work_state);
      break;
    case kKeyCoreTemp:
      copy_int32(counters.core_temp);
      break;
    case kKeyPowerUpCnt:
      copy_uint32(counters.powerup_cnt, counters.has_powerup_cnt);
      break;
    case kKeyLocalTimeNow:
      copy_uint64(counters.local_time_now, counters.has_local_time_now);
      break;
    case kKeyLastSyncTime:
      copy_uint64(counters.last_sync_time, counters.has_last_sync_time);
      break;
    case kKeyTimeOffset:
      copy_int64(counters.time_offset, counters.has_time_offset);
      break;
    case kKeyStatusCode:
      counters.status_code = format_status_bytes(kv->value, kv->length);
      break;
    case kKeyEnvironmentTemp:
      copy_int32(counters.environment_temp);
      break;
    case kKeyBlindSpotSet:
      copy_uint32(counters.blind_spot_set, counters.has_blind_spot_set);
      break;
    case kKeyFrameRate:
      copy_uint8(counters.frame_rate);
      break;
    case kKeyFovCfgEn:
      copy_uint8(counters.fov_cfg_en);
      break;
    case kKeyDetectMode:
      copy_uint8(counters.detect_mode);
      break;
    case kKeyWorkModeAfterBoot:
      copy_uint8(counters.work_mode_after_boot);
      break;
    case kKeyGlassHeat:
      copy_uint8(counters.glass_heat);
      break;
    case kKeyPointSendEn:
      copy_uint8(counters.point_send_en);
      break;
    case kKeyImuDataEn:
      copy_uint8(counters.imu_data_en);
      break;
    case kKeyFusaEn:
      copy_uint8(counters.fusa_en);
      break;
    case kKeyForceHeatEn:
      copy_uint8(counters.force_heat_en);
      break;
    case kKeySetEscMode:
      copy_uint8(counters.esc_mode);
      break;
    case kKeySetPpsSyncMode:
      copy_uint8(counters.pps_sync_mode);
      break;
    case kKeyTimeSyncType:
      copy_uint8(counters.time_sync_type);
      break;
    case kKeyLidarDiagStatus:
      copy_uint16(counters.lidar_diag_status);
      break;
    case kKeyLidarFlashStatus:
      copy_uint8(counters.lidar_flash_status);
      break;
    case kKeyFwType:
      copy_uint8(counters.fw_type);
      break;
    case kKeyHmsCode:
      if (kv->length >= sizeof(uint32_t)) {
        const size_t count = std::min<size_t>(8, kv->length / sizeof(uint32_t));
        std::memset(counters.hms_code, 0, sizeof(counters.hms_code));
        std::memcpy(counters.hms_code, kv->value, count * sizeof(uint32_t));
        counters.has_hms_code = true;
      }
      break;
    case kKeyCurGlassHeatState:
      copy_uint8(counters.cur_glass_heat_state);
      break;
    case kKeyRoiMode:
      copy_uint8(counters.roi_mode);
      break;
    default:
      break;
  }
}

void InternalInfoCallback(
    livox_status status,
    uint32_t handle,
    LivoxLidarDiagInternalInfoResponse* response,
    void*) {
  g_internal_query_inflight = false;
  std::lock_guard<std::mutex> lock(g_mutex);
  g_counters.handle = handle;
  if (status != kLivoxLidarStatusSuccess) {
    g_counters.internal_status =
        status == kLivoxLidarStatusTimeout ? "query timeout" : "query status " + std::to_string(status);
    g_cv.notify_all();
    return;
  }
  if (!response) {
    g_counters.internal_status = "empty response";
    g_cv.notify_all();
    return;
  }
  if (response->ret_code != 0) {
    g_counters.internal_status = "ret_code " + std::to_string(static_cast<unsigned>(response->ret_code));
    g_cv.notify_all();
    return;
  }
  uint16_t offset = 0;
  for (uint16_t i = 0; i < response->param_num; ++i) {
    if (offset + sizeof(uint16_t) * 2 > 4096) {
      break;
    }
    auto* kv = reinterpret_cast<LivoxLidarKeyValueParam*>(&response->data[offset]);
    parse_internal_kv(g_counters, kv);
    offset = static_cast<uint16_t>(offset + sizeof(uint16_t) * 2 + kv->length);
  }
  g_counters.internal_status = "query ok";
  g_counters.last_internal_info = std::chrono::steady_clock::now();
  g_cv.notify_all();
}

void maybe_query_internal_info(uint32_t handle) {
  if (handle == 0 || !g_running.load()) {
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  if (g_internal_query_inflight.load()) {
    bool expired = false;
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      if (g_internal_query_started.time_since_epoch().count() != 0) {
        const double age = std::chrono::duration<double>(now - g_internal_query_started).count();
        expired = age > kInternalQueryTimeoutSec;
      }
      if (expired) {
        g_counters.internal_status = "query timeout";
      }
    }
    if (!expired) {
      return;
    }
    g_internal_query_inflight = false;
  }
  bool expected = false;
  if (!g_internal_query_inflight.compare_exchange_strong(expected, true)) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_internal_query_started = now;
    g_counters.internal_status = "query sent";
  }
  const livox_status status = QueryLivoxLidarInternalInfo(handle, InternalInfoCallback, nullptr);
  if (status != kLivoxLidarStatusSuccess) {
    g_internal_query_inflight = false;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_counters.internal_status = "send failed " + std::to_string(status);
  }
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
  maybe_query_internal_info(handle);
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
  const std::string product_info = json_string_value(text, "product_info");
  int diag = -1;
  int core_temp = std::numeric_limits<int>::min();
  int environment_temp = std::numeric_limits<int>::min();
  int work_state = -1;
  int pcl_data_type = -1;
  int pattern_mode = -1;
  int dual_emit_en = -1;
  int frame_rate = -1;
  int fov_cfg_en = -1;
  int detect_mode = -1;
  int work_mode_after_boot = -1;
  int glass_heat = -1;
  int point_send_en = -1;
  int imu_data_en = -1;
  int fusa_en = -1;
  int force_heat_en = -1;
  int esc_mode = -1;
  int pps_sync_mode = -1;
  int time_sync_type = -1;
  int lidar_flash_status = -1;
  int fw_type = -1;
  int cur_glass_heat_state = -1;
  int roi_mode = -1;
  int powerup_cnt = -1;
  int blind_spot_set = -1;
  int64_t time_offset = 0;
  const bool has_diag = json_int_value(text, "lidar_diag_status", diag);
  const bool has_core_temp = json_int_value(text, "core_temp", core_temp);
  const bool has_environment_temp = json_int_value(text, "environment_temp", environment_temp);
  const bool has_work_state = json_int_value(text, "cur_work_state", work_state);
  const bool has_pcl_data_type = json_int_value(text, "pcl_data_type", pcl_data_type);
  const bool has_pattern_mode = json_int_value(text, "pattern_mode", pattern_mode);
  const bool has_dual_emit = json_int_value(text, "dual_emit_en", dual_emit_en);
  const bool has_frame_rate = json_int_value(text, "frame_rate", frame_rate);
  const bool has_fov_cfg_en = json_int_value(text, "fov_cfg_en", fov_cfg_en);
  const bool has_detect_mode = json_int_value(text, "detect_mode", detect_mode);
  const bool has_work_mode_after_boot = json_int_value(text, "work_mode_after_boot", work_mode_after_boot);
  const bool has_glass_heat = json_int_value(text, "glass_heat", glass_heat);
  const bool has_point_send = json_int_value(text, "point_send_en", point_send_en);
  const bool has_imu_data = json_int_value(text, "imu_data_en", imu_data_en);
  const bool has_fusa = json_int_value(text, "fusa_en", fusa_en);
  const bool has_force_heat = json_int_value(text, "force_heat_en", force_heat_en);
  const bool has_esc_mode = json_int_value(text, "esc_mode", esc_mode);
  const bool has_pps_sync = json_int_value(text, "pps_sync_mode", pps_sync_mode);
  const bool has_time_sync = json_int_value(text, "time_sync_type", time_sync_type);
  const bool has_flash_status = json_int_value(text, "lidar_flash_status", lidar_flash_status);
  const bool has_fw_type = json_int_value(text, "fw_type", fw_type);
  const bool has_glass_heat_state = json_int_value(text, "cur_glass_heat_state", cur_glass_heat_state);
  const bool has_roi_mode = json_int_value(text, "ROI_Mode", roi_mode) || json_int_value(text, "roi_mode", roi_mode);
  const bool has_powerup_cnt = json_int_value(text, "powerup_cnt", powerup_cnt);
  const bool has_blind_spot = json_int_value(text, "blind_spot_set", blind_spot_set);
  const bool has_time_offset = json_int_value(text, "time_offset", time_offset);
  std::vector<uint32_t> firmware_values;
  std::vector<uint32_t> loader_values;
  std::vector<uint32_t> hardware_values;
  std::vector<uint32_t> mac_values;
  std::vector<uint32_t> status_values;
  std::vector<uint32_t> hms_values;
  const bool has_firmware = json_int_array_value(text, "version_app", firmware_values);
  const bool has_loader = json_int_array_value(text, "version_loader", loader_values);
  const bool has_hardware = json_int_array_value(text, "version_hardware", hardware_values);
  const bool has_mac = json_int_array_value(text, "mac", mac_values);
  const bool has_status_code = json_int_array_value(text, "status_code", status_values);
  const bool has_hms = json_int_array_value(text, "hms_code", hms_values);
  const bool has_lidar_ipcfg = !json_object_value(text, "lidar_ipcfg").empty();
  const bool has_state_endpoint = !json_object_value(text, "state_info_host_ipcfg").empty();
  const bool has_point_endpoint = !json_object_value(text, "ponitcloud_host_ipcfg").empty();
  const bool has_imu_endpoint = !json_object_value(text, "imu_host_ipcfg").empty();
  const bool has_ctl_endpoint = !json_object_value(text, "ctl_host_ipcfg").empty();
  const bool has_log_endpoint = !json_object_value(text, "log_host_ipcfg").empty();
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
    if (!product_info.empty()) {
      g_counters.product_info = product_info;
    }
    if (has_diag) {
      g_counters.lidar_diag_status = diag;
    }
    if (has_core_temp) {
      g_counters.core_temp = core_temp;
    }
    if (has_environment_temp) {
      g_counters.environment_temp = environment_temp;
    }
    if (has_work_state) {
      g_counters.work_state = work_state;
    }
    if (has_pcl_data_type) {
      g_counters.pcl_data_type = pcl_data_type;
    }
    if (has_pattern_mode) {
      g_counters.pattern_mode = pattern_mode;
    }
    if (has_dual_emit) {
      g_counters.dual_emit_en = dual_emit_en;
    }
    if (has_frame_rate) {
      g_counters.frame_rate = frame_rate;
    }
    if (has_fov_cfg_en) {
      g_counters.fov_cfg_en = fov_cfg_en;
    }
    if (has_detect_mode) {
      g_counters.detect_mode = detect_mode;
    }
    if (has_work_mode_after_boot) {
      g_counters.work_mode_after_boot = work_mode_after_boot;
    }
    if (has_glass_heat) {
      g_counters.glass_heat = glass_heat;
    }
    if (has_point_send) {
      g_counters.point_send_en = point_send_en;
    }
    if (has_imu_data) {
      g_counters.imu_data_en = imu_data_en;
    }
    if (has_fusa) {
      g_counters.fusa_en = fusa_en;
    }
    if (has_force_heat) {
      g_counters.force_heat_en = force_heat_en;
    }
    if (has_esc_mode) {
      g_counters.esc_mode = esc_mode;
    }
    if (has_pps_sync) {
      g_counters.pps_sync_mode = pps_sync_mode;
    }
    if (has_time_sync) {
      g_counters.time_sync_type = time_sync_type;
    }
    if (has_flash_status) {
      g_counters.lidar_flash_status = lidar_flash_status;
    }
    if (has_fw_type) {
      g_counters.fw_type = fw_type;
    }
    if (has_glass_heat_state) {
      g_counters.cur_glass_heat_state = cur_glass_heat_state;
    }
    if (has_roi_mode) {
      g_counters.roi_mode = roi_mode;
    }
    if (has_powerup_cnt) {
      g_counters.powerup_cnt = static_cast<uint32_t>(std::max(0, powerup_cnt));
      g_counters.has_powerup_cnt = true;
    }
    if (has_blind_spot) {
      g_counters.blind_spot_set = static_cast<uint32_t>(std::max(0, blind_spot_set));
      g_counters.has_blind_spot_set = true;
    }
    if (has_time_offset) {
      g_counters.time_offset = time_offset;
      g_counters.has_time_offset = true;
    }
    if (has_firmware) {
      g_counters.firmware_version = format_version_array(firmware_values);
    }
    if (has_loader) {
      g_counters.loader_version = format_version_array(loader_values);
    }
    if (has_hardware) {
      g_counters.hardware_version = format_version_array(hardware_values);
    }
    if (has_mac) {
      const std::string mac = format_mac_array(mac_values);
      if (!mac.empty()) {
        g_counters.mac = mac;
      }
    }
    if (has_status_code) {
      g_counters.status_code = format_status_array(status_values);
    }
    if (has_hms) {
      std::memset(g_counters.hms_code, 0, sizeof(g_counters.hms_code));
      const size_t count = std::min<size_t>(8, hms_values.size());
      for (size_t i = 0; i < count; ++i) {
        g_counters.hms_code[i] = hms_values[i];
      }
      g_counters.has_hms_code = true;
    }
    if (has_lidar_ipcfg) {
      parse_json_lidar_ipcfg(g_counters, text);
    }
    if (has_state_endpoint) {
      parse_json_endpoint(text, "state_info_host_ipcfg", "dst_port", "src_port", g_counters.state_endpoint);
    }
    if (has_point_endpoint) {
      parse_json_endpoint(text, "ponitcloud_host_ipcfg", "dst_port", "src_port", g_counters.point_endpoint);
    }
    if (has_imu_endpoint) {
      parse_json_endpoint(text, "imu_host_ipcfg", "dst_port", "src_port", g_counters.imu_endpoint);
    }
    if (has_ctl_endpoint) {
      parse_json_endpoint(text, "ctl_host_ipcfg", "dst_port", "src_port", g_counters.ctl_endpoint);
    }
    if (has_log_endpoint) {
      parse_json_endpoint(text, "log_host_ipcfg", "dst_port", "src_port", g_counters.log_endpoint);
    }
    if (has_core_temp || has_environment_temp || has_work_state || has_pcl_data_type ||
        has_pattern_mode || has_dual_emit || has_frame_rate || has_fov_cfg_en ||
        has_detect_mode || has_work_mode_after_boot || has_glass_heat || has_point_send ||
        has_imu_data || has_fusa || has_force_heat || has_esc_mode || has_pps_sync ||
        has_time_sync || has_flash_status || has_fw_type || has_glass_heat_state ||
        has_roi_mode || has_powerup_cnt || has_blind_spot || has_time_offset ||
        has_firmware || has_loader || has_hardware || has_mac || has_status_code || has_hms ||
        has_lidar_ipcfg || has_state_endpoint || has_point_endpoint || has_imu_endpoint ||
        has_ctl_endpoint || has_log_endpoint) {
      g_counters.internal_status = "push";
      g_counters.last_internal_info = std::chrono::steady_clock::now();
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
std::string fixed_number(double value, int precision);
std::string format_rate(double value, const std::string& suffix);
std::string format_compact_rate(double value, const std::string& unit);

struct StatusRow {
  std::string label;
  std::string value;
  int color;
};

std::string format_count(uint64_t value) {
  if (value >= 1000000) {
    return fixed_number(static_cast<double>(value) / 1000000.0, 1) + "M";
  }
  if (value >= 1000) {
    return fixed_number(static_cast<double>(value) / 1000.0, 1) + "k";
  }
  return std::to_string(value);
}

std::string callback_age_text(const Counters& snapshot, std::chrono::steady_clock::time_point now) {
  if (snapshot.last_callback.time_since_epoch().count() == 0) {
    return "N/A";
  }
  const double age = std::chrono::duration<double>(now - snapshot.last_callback).count();
  return fixed_number(std::max(0.0, age), 1) + "s ago";
}

std::vector<StatusRow> stream_status_rows(
    const ReportValues& values,
    const Counters& snapshot,
    std::chrono::steady_clock::time_point now) {
  const std::string link = connection_text(snapshot, now);
  const bool live = link.rfind("LIVE", 0) == 0;
  return {
      {"LINK", link, live ? kColorSuccess : kColorWarning},
      {"HEALTH", snapshot.lidar_diag_status == 0 ? "OK" : health_text(snapshot.lidar_diag_status),
       snapshot.lidar_diag_status == 0 ? kColorSuccess : kColorWarning},
      {"CORE TEMP", temp_text(snapshot.core_temp), temp_status_color(snapshot.core_temp)},
      {"ENV TEMP", temp_text(snapshot.environment_temp), temp_status_color(snapshot.environment_temp)},
      {"WORK", work_state_text(snapshot.work_state),
       snapshot.work_state == kLivoxLidarNormal ? kColorSuccess : (snapshot.work_state < 0 ? kColorMuted : kColorWarning)},
      {"POINT SEND", on_off_text(snapshot.point_send_en), snapshot.point_send_en > 0 ? kColorSuccess : kColorWarning},
      {"IMU ENABLE", on_off_text(snapshot.imu_data_en), snapshot.imu_data_en > 0 ? kColorSuccess : kColorWarning},
      {"POINT RATE", format_compact_rate(values.points_per_sec, "pt/s"),
       values.points_per_sec > 520000.0 ? kColorWarning : kColorAccent},
      {"IMU RATE", fixed_number(values.imu_samples_per_sec, 1) + " Hz",
       values.imu_samples_per_sec > 0.0 ? kColorSuccess : kColorWarning},
      {"IMU PKT", format_rate(values.imu_pps, "pkt/s"), values.imu_pps > 0.0 ? kColorSuccess : kColorMuted},
      {"TRAFFIC", fixed_number(values.point_mbps, 2) + " Mbps", kColorText},
      {"TYPE", data_type_name(snapshot.last_point_type), kColorText},
      {"UDP CNT", std::to_string(snapshot.last_point_udp), kColorAccent},
      {"TIME SYNC", time_sync_type_text(snapshot.time_sync_type), snapshot.time_sync_type < 0 ? kColorMuted : kColorSuccess},
      {"HMS", snapshot.has_hms_code ? format_hms_code(snapshot.hms_code, 8) : "N/A",
       snapshot.has_hms_code ? kColorText : kColorMuted},
      {"FW", firmware_text(snapshot), snapshot.firmware_version.empty() ? kColorMuted : kColorText},
      {"HW", snapshot.hardware_version.empty() ? "N/A" : snapshot.hardware_version,
       snapshot.hardware_version.empty() ? kColorMuted : kColorText},
      {"INFO SRC", snapshot.internal_status.empty() ? "N/A" : snapshot.internal_status,
       snapshot.internal_status == "push" || snapshot.internal_status == "query ok" ? kColorSuccess : kColorMuted},
      {"INFO AGE", internal_age_text(snapshot, now), snapshot.last_internal_info.time_since_epoch().count() == 0 ? kColorMuted : kColorSuccess},
      {"LAST CB", callback_age_text(snapshot, now), live ? kColorSuccess : kColorWarning},
  };
}

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

struct TerminalSize {
  int rows = 24;
  int cols = 80;
};

TerminalSize current_terminal_size() {
  TerminalSize size;
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

std::string format_short_clock_time(std::chrono::system_clock::time_point time_point) {
  const std::time_t raw_time = std::chrono::system_clock::to_time_t(time_point);
  std::tm local_time {};
  localtime_r(&raw_time, &local_time);
  std::ostringstream out;
  out << std::put_time(&local_time, "%H:%M:%S");
  return out.str();
}

int clamp_int(int value, int low, int high) {
  return std::max(low, std::min(high, value));
}

double clamp_double(double value, double low, double high) {
  return std::max(low, std::min(high, value));
}

std::string bar(double value, double max_value, int width, bool pulse) {
  width = std::max(0, width);
  if (width == 0) {
    return "";
  }
  const double ratio = max_value <= 0.0 ? 0.0 : clamp_double(value / max_value, 0.0, 1.0);
  const int filled = clamp_int(static_cast<int>(std::round(ratio * width)), 0, width);
  std::string out;
  out.reserve(static_cast<size_t>(width));
  for (int i = 0; i < width; ++i) {
    if (i < filled) {
      out += (pulse && i == filled - 1) ? "▓" : "█";
    } else {
      out += "░";
    }
  }
  return out;
}

std::string badge(const std::string& label, const std::string& value) {
  return "[" + label + ": " + value + "]";
}

#ifdef LIVOX_MID360_HAS_NCURSES
std::string fit_text(const std::string& value, int width) {
  if (width <= 0) {
    return "";
  }
  if (static_cast<int>(value.size()) <= width) {
    return value;
  }
  if (width <= 3) {
    return value.substr(0, static_cast<size_t>(width));
  }
  return value.substr(0, static_cast<size_t>(width - 3)) + "...";
}

void put_text(int y, int x, const std::string& text, int color = 0, bool bold_text = false) {
  if (y < 0 || x < 0 || y >= LINES || x >= COLS) {
    return;
  }
  const int width = COLS - x;
  if (width <= 0) {
    return;
  }
  attr_t attrs = 0;
  if (color > 0) {
    attrs |= COLOR_PAIR(color);
  }
  if (bold_text) {
    attrs |= A_BOLD;
  }
  attron(attrs);
  mvaddnstr(y, x, fit_text(text, width).c_str(), width);
  attroff(attrs);
}

void put_text_clipped(int y, int x, const std::string& text, int max_width, int color = 0, bool bold_text = false) {
  if (y < 0 || x < 0 || y >= LINES || x >= COLS || max_width <= 0) {
    return;
  }
  const int width = std::min(max_width, COLS - x);
  if (width <= 0) {
    return;
  }
  attr_t attrs = 0;
  if (color > 0) {
    attrs |= COLOR_PAIR(color);
  }
  if (bold_text) {
    attrs |= A_BOLD;
  }
  attron(attrs);
  mvaddnstr(y, x, fit_text(text, width).c_str(), width);
  attroff(attrs);
}

void put_hline(int y, int x, int width, chtype ch, int color = 0) {
  if (y < 0 || y >= LINES || width <= 0) {
    return;
  }
  const int start = clamp_int(x, 0, std::max(0, COLS - 1));
  const int end = clamp_int(x + width, 0, COLS);
  if (end <= start) {
    return;
  }
  if (color > 0) {
    attron(COLOR_PAIR(color));
  }
  mvhline(y, start, ch, end - start);
  if (color > 0) {
    attroff(COLOR_PAIR(color));
  }
}

void draw_box(int y, int x, int height, int width, const std::string& title) {
  if (height < 3 || width < 8 || y >= LINES || x >= COLS) {
    return;
  }
  height = std::min(height, LINES - y);
  width = std::min(width, COLS - x);
  if (height < 3 || width < 8) {
    return;
  }
  attron(COLOR_PAIR(kColorAccent));
  mvaddch(y, x, ACS_ULCORNER);
  mvhline(y, x + 1, ACS_HLINE, width - 2);
  mvaddch(y, x + width - 1, ACS_URCORNER);
  mvvline(y + 1, x, ACS_VLINE, height - 2);
  mvvline(y + 1, x + width - 1, ACS_VLINE, height - 2);
  mvaddch(y + height - 1, x, ACS_LLCORNER);
  mvhline(y + height - 1, x + 1, ACS_HLINE, width - 2);
  mvaddch(y + height - 1, x + width - 1, ACS_LRCORNER);
  attroff(COLOR_PAIR(kColorAccent));
  if (!title.empty()) {
    put_text(y, x + 2, " " + title + " ", kColorAccent, true);
  }
}

void put_bar(int y, int x, double value, double max_value, int width, int color, bool pulse) {
  if (y < 0 || x < 0 || y >= LINES || x >= COLS || width <= 0) {
    return;
  }
  const int draw_width = std::min(width, COLS - x);
  const std::string text = bar(value, max_value, draw_width, pulse);
  attr_t attrs = COLOR_PAIR(color) | A_BOLD;
  attron(attrs);
  mvaddstr(y, x, text.c_str());
  attroff(attrs);
}

int health_color(const Counters& snapshot, std::chrono::steady_clock::time_point now) {
  if (connection_text(snapshot, now).rfind("LIVE", 0) != 0) {
    return kColorWarning;
  }
  if (snapshot.lidar_diag_status == 0) {
    return kColorSuccess;
  }
  return snapshot.lidar_diag_status < 0 ? kColorWarning : kColorDanger;
}

void draw_header(std::chrono::system_clock::time_point wall_time) {
  attron(COLOR_PAIR(kColorMuted));
  for (int y = 0; y < std::min(3, LINES); ++y) {
    move(y, 0);
    clrtoeol();
  }
  attroff(COLOR_PAIR(kColorMuted));
  put_text(0, 2, "📡 LIVOX MID-360 [LIVE]", kColorAccent, true);
  const std::string right = format_short_clock_time(wall_time) + "  ⚙";
  put_text(0, std::max(2, COLS - static_cast<int>(right.size()) - 2), right, kColorText, false);
  put_hline(1, 0, COLS, ACS_HLINE, kColorAccent);
  put_hline(2, 0, COLS, ACS_HLINE, kColorMuted);
}

void draw_sidebar(
    int y,
    int x,
    int height,
    int width,
    double elapsed,
    const Counters& snapshot,
    std::chrono::steady_clock::time_point now) {
  draw_box(y, x, height, width, "DEVICE IDENTITY");
  const int label_x = x + 2;
  const int value_x = x + 14;
  const int value_width = std::max(1, x + width - 1 - value_x);
  int row = y + 2;
  auto field = [&](const std::string& label, const std::string& value, int color) {
    if (row >= y + height - 2) {
      return;
    }
    put_text_clipped(row, label_x, label, std::max(1, value_x - label_x - 1), kColorText, true);
    put_text_clipped(row, value_x, value, value_width, color, true);
    row += 2;
  };
  field("SERIAL_NO", snapshot.sn.empty() ? "N/A" : snapshot.sn, kColorAccent);
  field("IP_ADDRESS", snapshot.lidar_ip.empty() ? "N/A" : snapshot.lidar_ip, kColorSuccess);
  field("FIRMWARE", firmware_text(snapshot), snapshot.firmware_version.empty() ? kColorText : kColorSuccess);
  field("UPTIME", format_duration(elapsed), kColorAccent);

  row += 1;
  const int status = health_color(snapshot, now);
  put_text(row++, label_x, badge("HEALTH", snapshot.lidar_diag_status == 0 ? "OK" : health_text(snapshot.lidar_diag_status)), status, true);
  put_text(row++, label_x, badge("MODE", "MONITOR"), kColorAccent, true);
  put_text(row++, label_x, badge("LASER", connection_text(snapshot, now).rfind("LIVE", 0) == 0 ? "ACTIVE" : "WAIT"), status, true);
  if (!snapshot.control_status.empty() && row < y + height - 1) {
    put_text(row + 1, label_x, "CTRL " + snapshot.control_status, kColorText, false);
  }
}

void draw_network_panel(int y, int x, int height, int width, const Counters& snapshot) {
  draw_box(y, x, height, width, "NETWORK");
  if (height < 5 || width < 18) {
    return;
  }
  int row = y + 2;
  const int label_x = x + 2;
  const int value_x = x + std::min(12, std::max(8, width / 3));
  const int label_width = std::max(1, value_x - label_x - 1);
  const int value_width = std::max(1, x + width - 1 - value_x);
  auto field = [&](const std::string& label, const std::string& value, int color) {
    if (row >= y + height - 1) {
      return;
    }
    put_text_clipped(row, label_x, label, label_width, kColorAccent, true);
    put_text_clipped(row, value_x, value, value_width, color, true);
    ++row;
  };
  field("LIDAR", snapshot.lidar_ip.empty() ? "N/A" : snapshot.lidar_ip, kColorSuccess);
  field("MASK", snapshot.lidar_mask.empty() ? "N/A" : snapshot.lidar_mask, snapshot.lidar_mask.empty() ? kColorMuted : kColorText);
  field("GATEWAY", snapshot.lidar_gateway.empty() ? "N/A" : snapshot.lidar_gateway,
        snapshot.lidar_gateway.empty() ? kColorMuted : kColorText);
  field("STATE", endpoint_summary(snapshot.state_endpoint), snapshot.state_endpoint.ip.empty() ? kColorMuted : kColorText);
  field("POINT", endpoint_summary(snapshot.point_endpoint), snapshot.point_endpoint.ip.empty() ? kColorMuted : kColorText);
  field("IMU", endpoint_summary(snapshot.imu_endpoint), snapshot.imu_endpoint.ip.empty() ? kColorMuted : kColorText);
  field("CTRL", endpoint_summary(snapshot.ctl_endpoint), snapshot.ctl_endpoint.ip.empty() ? kColorMuted : kColorText);
  field("LOG", endpoint_summary(snapshot.log_endpoint), snapshot.log_endpoint.ip.empty() ? kColorMuted : kColorText);
}

void draw_metric_panel(
    int y,
    int x,
    int height,
    int width,
    const std::string& title,
    const std::string& value,
    double raw_value,
    double max_value,
    const std::string& scale_unit,
    int color,
    bool pulse) {
  draw_box(y, x, height, width, title);
  if (height < 5) {
    return;
  }
  put_text(y + 2, x + 2, value, color, true);
  put_bar(y + 4, x + 2, raw_value, max_value, std::max(4, width - 4), color, pulse);
  const std::string max_label = format_scale_value(max_value, scale_unit);
  const std::string scale = "0" + std::string(std::max(1, width - static_cast<int>(max_label.size()) - 5), ' ') + max_label;
  put_text(y + 5, x + 2, scale, kColorText, false);
}

void draw_density_map(int y, int x, int height, int width, const Counters& snapshot, double point_rate) {
  draw_box(y, x, height, width, "POINT CLOUD PREVIEW");
  if (height < 5 || width < 12) {
    return;
  }
  static const char* shades = " .:*#@";
  const int inner_h = height - 3;
  const int inner_w = width - 4;
  const double energy = clamp_double(point_rate / 600000.0, 0.05, 1.0);
  const int phase = static_cast<int>((snapshot.last_point_udp + snapshot.point_packets) % 97);
  for (int row = 0; row < inner_h; ++row) {
    std::string line;
    line.reserve(static_cast<size_t>(inner_w));
    for (int col = 0; col < inner_w; ++col) {
      const double cx = (col - inner_w / 2.0) / std::max(1.0, inner_w / 2.0);
      const double cy = (row - inner_h / 2.0) / std::max(1.0, inner_h / 2.0);
      const double ring = std::sin((cx * cx + cy * cy) * 11.0 + phase * 0.07);
      const double sweep = std::cos(cx * 8.0 - phase * 0.11) * std::sin(cy * 5.0 + phase * 0.05);
      double density = (ring + sweep + 2.0) / 4.0 * energy;
      density *= 1.0 - std::min(0.7, std::sqrt(cx * cx + cy * cy) * 0.45);
      const int shade = clamp_int(static_cast<int>(density * 6.0), 0, 5);
      line.push_back(shades[shade]);
    }
    put_text(y + 2 + row, x + 2, line, row % 3 == 0 ? kColorAccent : kColorMuted, false);
  }
}

void draw_stream_status_table(
    int y,
    int x,
    int height,
    int width,
    const ReportValues& values,
    const Counters& snapshot,
    std::chrono::steady_clock::time_point now) {
  draw_box(y, x, height, width, "STREAM STATUS");
  if (height < 7 || width < 24) {
    return;
  }
  put_text(y + 2, x + 2, "ITEM", kColorText, true);
  put_text(y + 2, x + std::min(width - 10, 15), "VALUE", kColorText, true);
  put_hline(y + 3, x + 1, width - 2, ACS_HLINE, kColorMuted);

  const std::vector<StatusRow> rows = stream_status_rows(values, snapshot, now);
  const int value_x = x + std::min(width - 10, 15);
  const int label_width = std::max(1, value_x - x - 3);
  const int value_width = std::max(1, x + width - 1 - value_x);
  for (size_t i = 0; i < rows.size() && y + 4 + static_cast<int>(i) < y + height - 1; ++i) {
    const int row = y + 4 + static_cast<int>(i);
    put_text_clipped(row, x + 2, rows[i].label, label_width, kColorAccent, true);
    put_text_clipped(row, value_x, rows[i].value, value_width, rows[i].color, true);
  }
}

void draw_footer(int y) {
  if (y < 0 || y >= LINES) {
    return;
  }
  attron(COLOR_PAIR(kColorFooter) | A_BOLD);
  move(y, 0);
  clrtoeol();
  mvaddnstr(y, 1, "[F1] HELP   [F5] RESET   [F10] MENU   [CTRL+C] STOP", COLS - 2);
  attroff(COLOR_PAIR(kColorFooter) | A_BOLD);
}

void print_neon_dashboard(
    const Counters& delta,
    double interval,
    double elapsed,
    const Counters& snapshot,
    std::chrono::steady_clock::time_point now,
    const Options& options) {
  (void)options;
  const ReportValues values = make_report_values(delta, interval);
  const bool pulse = (snapshot.point_packets / 2) % 2 == 0;

  int key = getch();
  while (key != ERR) {
    if (key == 3 || key == 'q' || key == 'Q') {
      g_user_interrupted = true;
      g_running = false;
      g_cv.notify_all();
    } else if (key == KEY_F(5)) {
      g_reset_requested = true;
    }
    key = getch();
  }

  erase();
  bkgd(COLOR_PAIR(kColorBackground));
  draw_header(std::chrono::system_clock::now());
  if (LINES < 18 || COLS < 72) {
    put_text(4, 2, "Terminal too small for Neon Protocol Monitor", kColorWarning, true);
    put_text(6, 2, "Resize to at least 72x18.", kColorText, false);
    draw_footer(LINES - 1);
    refresh();
    return;
  }

  const int footer_y = LINES - 1;
  const int body_y = 3;
  const int body_h = std::max(1, footer_y - body_y);
  const int sidebar_w = clamp_int(COLS / 4, 26, 36);
  const int gap = 1;
  const int main_x = sidebar_w + gap;
  const int main_w = COLS - main_x;
  const int sidebar_top_h = clamp_int(12, 9, std::max(9, body_h - 6));
  draw_sidebar(body_y, 0, sidebar_top_h, sidebar_w, elapsed, snapshot, now);

  const int metric_h = 7;
  const int metric_w = std::max(20, (main_w - gap) / 2);
  draw_metric_panel(
      body_y,
      main_x,
      metric_h,
      metric_w,
      "POINT RATE",
      format_compact_rate(values.points_per_sec, "pt/s"),
      values.points_per_sec,
      600000.0,
      "pt/s",
      values.points_per_sec > 520000.0 ? kColorWarning : kColorAccent,
      pulse);
  draw_metric_panel(
      body_y,
      main_x + metric_w + gap,
      metric_h,
      main_w - metric_w - gap,
      "IMU FREQUENCY",
      fixed_number(values.imu_samples_per_sec, 1) + " Hz",
      values.imu_samples_per_sec,
      600.0,
      "Hz",
      values.imu_samples_per_sec < 50.0 ? kColorWarning : kColorSuccess,
      !pulse);

  const int lower_y = body_y + metric_h + 1;
  const int lower_h = std::max(5, body_h - metric_h - 1);
  const int network_y = body_y + sidebar_top_h + 1;
  const int network_h = std::max(3, footer_y - network_y);
  if (network_h >= 5) {
    draw_network_panel(network_y, 0, network_h, sidebar_w, snapshot);
  }
  const int preview_w = std::max(24, main_w * 2 / 3);
  const int stream_w = main_w - preview_w - gap;
  if (lower_h >= 5 && preview_w >= 24) {
    draw_density_map(lower_y, main_x, lower_h, preview_w, snapshot, values.points_per_sec);
  }
  if (lower_h >= 7 && stream_w >= 24) {
    draw_stream_status_table(
        lower_y,
        main_x + preview_w + gap,
        lower_h,
        stream_w,
        values,
        snapshot,
        now);
  }

  put_text(footer_y - 1, main_x + 2,
           "TRAFFIC " + fixed_number(values.point_mbps, 2) + " Mbps  TYPE " +
               data_type_name(snapshot.last_point_type) + "  UDP " + std::to_string(snapshot.last_point_udp),
           kColorText, false);
  draw_footer(footer_y);
  refresh();
}
#endif

std::string table_border(char left, char fill, char right) {
  return std::string(1, left) + std::string(kTableInnerWidth, fill) + std::string(1, right);
}

std::string ansi_color(int color, bool bold = false) {
  if (!terminal_dashboard_active()) {
    return "";
  }
  const char* code = "38;5;252";
  switch (color) {
    case 1:
      code = "38;5;51";
      break;
    case 2:
      code = "38;5;84";
      break;
    case 3:
      code = "38;5;220";
      break;
    case 4:
      code = "38;5;196";
      break;
    case 5:
      code = "38;5;242";
      break;
    default:
      break;
  }
  return std::string("\033[") + (bold ? "1;" : "") + code + "m";
}

std::string ansi_reset() {
  return terminal_dashboard_active() ? "\033[0m\033[48;5;233m" : "";
}

std::string ansi_text(const std::string& text, int color, bool bold = false) {
  return ansi_color(color, bold) + text + ansi_reset();
}

std::string repeat_text(const std::string& text, int count) {
  std::string out;
  if (count <= 0 || text.empty()) {
    return out;
  }
  out.reserve(text.size() * static_cast<size_t>(count));
  for (int i = 0; i < count; ++i) {
    out += text;
  }
  return out;
}

std::string ansi_bar(double value, double max_value, int width, int color, bool pulse) {
  return ansi_text(bar(value, max_value, width, pulse), color, true);
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

std::string ansi_fit(const std::string& value, int width) {
  return neon::fit(value, width);
}

std::string ansi_pad_right(const std::string& value, int width) {
  return neon::pad_right(value, width);
}

std::string ansi_box_top(const std::string& title, int width) {
  return ansi_text(neon::top_rule(width, title), 1, true);
}

std::string ansi_box_bottom(int width) {
  return ansi_text(neon::bottom_rule(width), 1);
}

std::string ansi_box_top_with_right(const std::string& title, int width, const std::string& right) {
  width = std::max(2, width);
  if (title.empty()) {
    return ansi_text("┌" + repeat_text("─", width - 2) + right, 1, true);
  }
  const std::string label = " " + ansi_fit(title, std::max(0, width - 4)) + " ";
  const int fill = std::max(0, width - 2 - neon::visible_length(label));
  return ansi_text("┌" + label + repeat_text("─", fill) + right, 1, true);
}

std::string ansi_box_bottom_with_right(int width, const std::string& right) {
  width = std::max(2, width);
  return ansi_text("└" + repeat_text("─", width - 2) + right, 1);
}

std::string ansi_box_top_no_left(const std::string& title, int width) {
  width = std::max(4, width);
  if (title.empty()) {
    return ansi_text(repeat_text("─", width - 2) + "┐", 1, true);
  }
  const std::string label = " " + ansi_fit(title, std::max(0, width - 4)) + " ";
  const int fill = std::max(0, width - 2 - neon::visible_length(label));
  return ansi_text(label + repeat_text("─", fill) + "┐", 1, true);
}

std::string ansi_box_bottom_no_left(int width) {
  width = std::max(4, width);
  return ansi_text(repeat_text("─", width - 2) + "┘", 1);
}

std::string ansi_box_row(const std::string& value, int width) {
  return neon::row(value, width);
}

std::string ansi_box_row_no_left(const std::string& value, int width) {
  width = std::max(4, width);
  return " " + neon::pad_right(value, width - 4) + " │";
}

std::string ansi_key_value_row(const std::string& key, const std::string& value, int width, int key_width = 11) {
  return neon::key_value_row(key, value, width, key_width);
}

std::string ansi_key_value_row_no_left(const std::string& key, const std::string& value, int width, int key_width = 11) {
  width = std::max(8, width);
  const int value_width = std::max(0, width - key_width - 5);
  return " " + neon::pad_right(key, key_width) + " " + neon::pad_right(value, value_width) + " │";
}

std::string ansi_colored_value(const std::string& value, int width, int color, bool bold = true) {
  return ansi_text(ansi_fit(value, width), color, bold);
}

std::string ansi_key_value_colored_row(
    const std::string& key,
    const std::string& value,
    int width,
    int color,
    bool bold = true,
    int key_width = 11) {
  const int value_width = std::max(0, width - key_width - 5);
  return ansi_key_value_row(key, ansi_colored_value(value, value_width, color, bold), width, key_width);
}

std::string ansi_key_value_colored_row_no_left(
    const std::string& key,
    const std::string& value,
    int width,
    int color,
    bool bold = true,
    int key_width = 11) {
  const int value_width = std::max(0, width - key_width - 5);
  return ansi_key_value_row_no_left(key, ansi_colored_value(value, value_width, color, bold), width, key_width);
}

std::string ansi_box_colored_row(const std::string& value, int width, int color, bool bold = true) {
  return ansi_box_row(ansi_colored_value(value, std::max(0, width - 4), color, bold), width);
}

std::vector<std::string> stream_status_box_rows(
    const ReportValues& values,
    const Counters& snapshot,
    std::chrono::steady_clock::time_point now,
    int width) {
  std::vector<std::string> rows;
  rows.push_back(ansi_key_value_row("ITEM", "VALUE", width, 11));
  const std::vector<StatusRow> status_rows = stream_status_rows(values, snapshot, now);
  for (const auto& item : status_rows) {
    rows.push_back(ansi_key_value_colored_row(item.label, item.value, width, item.color, true, 11));
  }
  return rows;
}

std::vector<std::string> stream_status_box_rows_no_left(
    const ReportValues& values,
    const Counters& snapshot,
    std::chrono::steady_clock::time_point now,
    int width) {
  std::vector<std::string> rows;
  rows.push_back(ansi_key_value_row_no_left("ITEM", "VALUE", width, 11));
  const std::vector<StatusRow> status_rows = stream_status_rows(values, snapshot, now);
  for (const auto& item : status_rows) {
    rows.push_back(ansi_key_value_colored_row_no_left(item.label, item.value, width, item.color, true, 11));
  }
  return rows;
}

std::vector<std::string> stream_status_box_lines(
    const ReportValues& values,
    const Counters& snapshot,
    std::chrono::steady_clock::time_point now,
    int width) {
  std::vector<std::string> rows;
  rows.push_back(ansi_box_top("STREAM STATUS", width));
  const std::vector<std::string> body = stream_status_box_rows(values, snapshot, now, width);
  rows.insert(rows.end(), body.begin(), body.end());
  rows.push_back(ansi_box_bottom(width));
  return rows;
}

std::vector<std::string> monitor_identity_rows(
    const Counters& snapshot,
    double elapsed,
    std::chrono::steady_clock::time_point now) {
  return {
      "SERIAL_NO    " + ansi_text(snapshot.sn.empty() ? "N/A" : snapshot.sn, 1, true),
      "IP_ADDRESS   " + ansi_text(snapshot.lidar_ip.empty() ? "N/A" : snapshot.lidar_ip, 2, true),
      "FIRMWARE     " + ansi_text(firmware_text(snapshot), snapshot.firmware_version.empty() ? 6 : 2, true),
      "UPTIME       " + ansi_text(format_duration(elapsed), 1, true),
      "",
      ansi_text(badge("HEALTH", snapshot.lidar_diag_status == 0 ? "OK" : health_text(snapshot.lidar_diag_status)),
                health_text(snapshot.lidar_diag_status) == "OK (diag=0)" ? 2 : 3,
                true),
      ansi_text(badge("MODE", "MONITOR"), 1, true),
      ansi_text(badge("LASER", connection_text(snapshot, now).rfind("LIVE", 0) == 0 ? "ACTIVE" : "WAIT"),
                connection_text(snapshot, now).rfind("LIVE", 0) == 0 ? 2 : 3,
                true),
  };
}

std::vector<std::string> monitor_identity_rows_compact(
    const Counters& snapshot,
    double elapsed,
    std::chrono::steady_clock::time_point now,
    int width) {
  const std::string sn = snapshot.sn.empty() ? "N/A" : snapshot.sn;
  const std::string ip = snapshot.lidar_ip.empty() ? "N/A" : snapshot.lidar_ip;
  const std::string health = snapshot.lidar_diag_status == 0 ? "OK" : health_text(snapshot.lidar_diag_status);
  const std::string laser = connection_text(snapshot, now).rfind("LIVE", 0) == 0 ? "ACTIVE" : "WAIT";
  return {
      "SN " + ansi_text(ansi_fit(sn, std::max(4, width - 17)), 1, true) + "  IP " +
          ansi_text(ansi_fit(ip, 15), 2, true),
      "UPTIME " + ansi_text(format_duration(elapsed), 1, true) + "  " +
          ansi_text(badge("HEALTH", health), health == "OK" ? 2 : 3, true),
      ansi_text(badge("MODE", "MONITOR"), 1, true) + " " +
          ansi_text(badge("LASER", laser), laser == "ACTIVE" ? 2 : 3, true),
  };
}

std::vector<std::string> network_rows(const Counters& snapshot, int width) {
  return {
      ansi_key_value_colored_row("LIDAR", snapshot.lidar_ip.empty() ? "N/A" : snapshot.lidar_ip, width, 2),
      ansi_key_value_colored_row("MASK", snapshot.lidar_mask.empty() ? "N/A" : snapshot.lidar_mask,
                                 width, snapshot.lidar_mask.empty() ? 5 : 6),
      ansi_key_value_colored_row("GATEWAY", snapshot.lidar_gateway.empty() ? "N/A" : snapshot.lidar_gateway,
                                 width, snapshot.lidar_gateway.empty() ? 5 : 6),
      ansi_key_value_colored_row("STATE", endpoint_summary(snapshot.state_endpoint),
                                 width, snapshot.state_endpoint.ip.empty() ? 5 : 6),
      ansi_key_value_colored_row("POINT", endpoint_summary(snapshot.point_endpoint),
                                 width, snapshot.point_endpoint.ip.empty() ? 5 : 6),
      ansi_key_value_colored_row("IMU", endpoint_summary(snapshot.imu_endpoint),
                                 width, snapshot.imu_endpoint.ip.empty() ? 5 : 6),
      ansi_key_value_colored_row("CTRL", endpoint_summary(snapshot.ctl_endpoint),
                                 width, snapshot.ctl_endpoint.ip.empty() ? 5 : 6),
      ansi_key_value_colored_row("LOG", endpoint_summary(snapshot.log_endpoint),
                                 width, snapshot.log_endpoint.ip.empty() ? 5 : 6),
  };
}

std::vector<std::string> network_rows_compact(const Counters& snapshot, int width) {
  return {
      "LIDAR " + ansi_text(ansi_fit(snapshot.lidar_ip.empty() ? "N/A" : snapshot.lidar_ip, std::max(4, width - 16)), 2, true) +
          "  MASK " + ansi_text(ansi_fit(snapshot.lidar_mask.empty() ? "N/A" : snapshot.lidar_mask, 15),
                                snapshot.lidar_mask.empty() ? 5 : 6, true),
      "POINT " + ansi_text(ansi_fit(compact_endpoint_ip(snapshot.point_endpoint), 15),
                           snapshot.point_endpoint.ip.empty() ? 5 : 6, true) +
          "  " + ansi_text(compact_endpoint_ports(snapshot.point_endpoint),
                           snapshot.point_endpoint.dst_port < 0 ? 5 : 6, true),
      "IMU   " + ansi_text(ansi_fit(compact_endpoint_ip(snapshot.imu_endpoint), 15),
                           snapshot.imu_endpoint.ip.empty() ? 5 : 6, true) +
          "  " + ansi_text(compact_endpoint_ports(snapshot.imu_endpoint),
                           snapshot.imu_endpoint.dst_port < 0 ? 5 : 6, true),
  };
}

std::vector<std::string> device_identity_box_rows(
    const Counters& snapshot,
    double elapsed,
    std::chrono::steady_clock::time_point now,
    int width) {
  const std::string health = snapshot.lidar_diag_status == 0 ? "OK" : health_text(snapshot.lidar_diag_status);
  const std::string laser = connection_text(snapshot, now).rfind("LIVE", 0) == 0 ? "ACTIVE" : "WAIT";
  return {
      ansi_key_value_colored_row("SERIAL_NO", snapshot.sn.empty() ? "N/A" : snapshot.sn, width, 1),
      ansi_key_value_colored_row("IP_ADDRESS", snapshot.lidar_ip.empty() ? "N/A" : snapshot.lidar_ip, width, 2),
      ansi_key_value_colored_row("FIRMWARE", firmware_text(snapshot), width, snapshot.firmware_version.empty() ? 6 : 2),
      ansi_key_value_colored_row("UPTIME", format_duration(elapsed), width, 1),
      ansi_box_colored_row(badge("HEALTH", health), width, health == "OK" ? 2 : 3),
      ansi_box_colored_row(badge("MODE", "MONITOR"), width, 1),
      ansi_box_colored_row(badge("LASER", laser), width, laser == "ACTIVE" ? 2 : 3),
  };
}

std::vector<std::string> device_identity_box(
    const Counters& snapshot,
    double elapsed,
    std::chrono::steady_clock::time_point now,
    int width) {
  std::vector<std::string> rows;
  rows.push_back(ansi_box_top("DEVICE IDENTITY", width));
  const std::vector<std::string> body = device_identity_box_rows(snapshot, elapsed, now, width);
  rows.insert(rows.end(), body.begin(), body.end());
  rows.push_back(ansi_box_bottom(width));
  return rows;
}

std::vector<std::string> boxed_lines(const std::string& title, const std::vector<std::string>& rows, int width) {
  std::vector<std::string> out;
  out.reserve(rows.size() + 2);
  out.push_back(ansi_box_top(title, width));
  for (const auto& row : rows) {
    out.push_back(ansi_box_row(row, width));
  }
  out.push_back(ansi_box_bottom(width));
  return out;
}

std::vector<std::string> metric_box(
    const std::string& title,
    const std::string& value,
    double raw_value,
    double max_value,
    const std::string& max_label,
    int color,
    bool pulse,
    int width) {
  const int bar_w = std::max(4, width - 4);
  return boxed_lines(
      title,
      {
          ansi_text(value, color, true),
          ansi_bar(raw_value, max_value, bar_w, color, pulse),
          "0" + std::string(static_cast<size_t>(std::max(1, width - 5 - static_cast<int>(max_label.size()))), ' ') + max_label,
      },
      width);
}

std::vector<std::string> compact_metric_box(
    const std::string& title,
    const std::string& value,
    double raw_value,
    double max_value,
    int color,
    bool pulse,
    int width) {
  return boxed_lines(
      title,
      {
          ansi_text(value, color, true),
          ansi_bar(raw_value, max_value, std::max(4, width - 4), color, pulse),
      },
      width);
}

std::vector<std::string> density_box(
    const Counters& snapshot,
    double point_rate,
    int width,
    int height) {
  static const char* shades = " .:*#@";
  const int inner_h = std::max(3, height - 2);
  const int inner_w = std::max(8, width - 4);
  const double energy = clamp_double(point_rate / 600000.0, 0.05, 1.0);
  const int phase = static_cast<int>((snapshot.last_point_udp + snapshot.point_packets) % 97);
  std::vector<std::string> rows;
  rows.reserve(static_cast<size_t>(inner_h));
  for (int row = 0; row < inner_h; ++row) {
    std::string line;
    line.reserve(static_cast<size_t>(inner_w));
    for (int col = 0; col < inner_w; ++col) {
      const double cx = (col - inner_w / 2.0) / std::max(1.0, inner_w / 2.0);
      const double cy = (row - inner_h / 2.0) / std::max(1.0, inner_h / 2.0);
      const double ring = std::sin((cx * cx + cy * cy) * 11.0 + phase * 0.07);
      const double sweep = std::cos(cx * 8.0 - phase * 0.11) * std::sin(cy * 5.0 + phase * 0.05);
      double density = (ring + sweep + 2.0) / 4.0 * energy;
      density *= 1.0 - std::min(0.7, std::sqrt(cx * cx + cy * cy) * 0.45);
      line.push_back(shades[clamp_int(static_cast<int>(density * 6.0), 0, 5)]);
    }
    rows.push_back(ansi_text(line, row % 3 == 0 ? 1 : 5));
  }
  return boxed_lines("POINT CLOUD PREVIEW", rows, width);
}

std::vector<std::string> stream_status_box(
    const ReportValues& values,
    const Counters& snapshot,
    std::chrono::steady_clock::time_point now,
    int width) {
  return stream_status_box_lines(values, snapshot, now, width);
}

void append_lines(std::ostringstream& out, const std::vector<std::string>& lines, int max_rows = 1000) {
  for (int i = 0; i < static_cast<int>(lines.size()) && i < max_rows; ++i) {
    out << lines[static_cast<size_t>(i)] << "\n";
  }
}

void append_lines_fit(
    std::ostringstream& out,
    const std::vector<std::string>& lines,
    int& used_rows,
    int max_rows) {
  for (int i = 0; i < static_cast<int>(lines.size()) && used_rows < max_rows; ++i) {
    out << lines[static_cast<size_t>(i)] << "\n";
    ++used_rows;
  }
}

void append_hstack(
    std::ostringstream& out,
    const std::vector<std::string>& left,
    const std::vector<std::string>& right,
    int left_width,
    int right_width,
    int max_rows = 1000) {
  const size_t rows = std::max(left.size(), right.size());
  const std::string empty_left(static_cast<size_t>(std::max(0, left_width)), ' ');
  const std::string empty_right(static_cast<size_t>(std::max(0, right_width)), ' ');
  for (size_t i = 0; i < rows && static_cast<int>(i) < max_rows; ++i) {
    out << (i < left.size() ? left[i] : empty_left)
        << " "
        << (i < right.size() ? right[i] : empty_right)
        << "\n";
  }
}

void append_hstack_fit(
    std::ostringstream& out,
    const std::vector<std::string>& left,
    const std::vector<std::string>& right,
    int left_width,
    int right_width,
    int& used_rows,
    int max_rows) {
  const size_t rows = std::max(left.size(), right.size());
  const std::string empty_left(static_cast<size_t>(std::max(0, left_width)), ' ');
  const std::string empty_right(static_cast<size_t>(std::max(0, right_width)), ' ');
  for (size_t i = 0; i < rows && used_rows < max_rows; ++i) {
    out << (i < left.size() ? left[i] : empty_left)
        << " "
        << (i < right.size() ? right[i] : empty_right)
        << "\n";
    ++used_rows;
  }
}

void print_discovery_dashboard(const std::string& message) {
  if (!terminal_dashboard_active()) {
    return;
  }
  const TerminalSize term = current_terminal_size();
  std::ostringstream out;
  const int width = std::max(40, term.cols);
  int used_rows = 3;
  out << "\033[48;5;233m\033[38;5;252m"
      << ansi_text("📡 LIVOX MID-360 [DISCOVERY]", 1, true)
      << std::string(std::max(1, width - 45), ' ')
      << ansi_text(format_short_clock_time(std::chrono::system_clock::now()) + "  ⚙", 6)
      << "\n"
      << ansi_text(repeat_text("─", width), 1)
      << "\n"
      << ansi_text(repeat_text("─", width), 5)
      << "\n";
  if (term.rows < 10 || term.cols < 58) {
    out << ansi_pad_right(ansi_text("Terminal too small for Discovery TUI", 3, true), width) << "\n"
        << ansi_pad_right("Resize to at least 58x10.", width) << "\n";
    used_rows += 2;
    while (used_rows++ < term.rows - 1) {
      out << neon::blank_line(width) << "\n";
    }
    out << "\033[48;5;51m\033[30;1m"
        << ansi_pad_right(" [CTRL+C] STOP ", width)
        << "\033[0m";
    g_discovery_renderer.render(out.str(), term.rows, width);
    return;
  }
  const int panel_w = width;
  const int max_rows = std::max(3, term.rows - 8);
  std::vector<std::string> rows;
  const size_t begin = g_discovery_lines.size() > static_cast<size_t>(max_rows)
      ? g_discovery_lines.size() - static_cast<size_t>(max_rows)
      : 0;
  for (size_t i = begin; i < g_discovery_lines.size(); ++i) {
    rows.push_back(g_discovery_lines[i]);
  }
  if (rows.empty()) {
    rows.push_back("waiting for Livox-SDK2 discovery");
  }
  append_lines_fit(out, boxed_lines("DISCOVERY", rows, panel_w), used_rows, term.rows - 2);
  if (!message.empty() && used_rows < term.rows - 1) {
    out << ansi_pad_right(ansi_text(ansi_fit(message, width), 1, true), width) << "\n";
    ++used_rows;
  }
  while (used_rows++ < term.rows - 1) {
    out << neon::blank_line(width) << "\n";
  }
  out << "\033[48;5;51m\033[30;1m"
      << ansi_pad_right(" [CTRL+C] STOP ", width)
      << "\033[0m";
  g_discovery_renderer.render(out.str(), term.rows, width);
}

void refresh_discovery_dashboard(const std::string& message, bool heartbeat) {
  if (!terminal_dashboard_active()) {
    return;
  }
  if (!message.empty()) {
    g_discovery_frame_clock.request_redraw();
  }
  if (g_discovery_frame_clock.consume_redraw(heartbeat)) {
    print_discovery_dashboard(message);
  }
}

void discovery_log(const std::string& message, bool important) {
  if (terminal_dashboard_active()) {
    g_discovery_lines.push_back((important ? "* " : "- ") + message);
    if (g_discovery_lines.size() > 128) {
      g_discovery_lines.erase(g_discovery_lines.begin());
    }
    refresh_discovery_dashboard(important ? message : "", false);
  } else {
    std::cout << "discovery: " << message << std::endl;
  }
}

void handle_terminal_input() {
#ifdef LIVOX_MID360_HAS_NCURSES
  if (g_curses_active.load()) {
    int key = getch();
    while (key != ERR) {
      if (key == 3 || key == 'q' || key == 'Q') {
        g_user_interrupted = true;
        g_running = false;
        g_cv.notify_all();
      } else if (key == KEY_F(5)) {
        g_reset_requested = true;
      }
      key = getch();
    }
  }
#endif
  bool use_ansi_input = true;
#ifdef LIVOX_MID360_HAS_NCURSES
  use_ansi_input = !g_curses_active.load();
#endif
  if (use_ansi_input) {
    neon::Key key = neon::read_key(0, 20);
    while (key != neon::Key::None) {
      if (key == neon::Key::Quit || key == neon::Key::Escape) {
        g_user_interrupted = true;
        g_running = false;
        g_cv.notify_all();
      } else if (key == neon::Key::Reset) {
        g_reset_requested = true;
      }
      key = neon::read_key(0, 20);
    }
  }
}

bool menu_return_enabled() {
  const char* value = std::getenv("LIVOX_MID360_ALLOW_MENU_RETURN");
  return value && std::strcmp(value, "1") == 0;
}

bool wait_for_result_key(bool allow_menu_return = false) {
  if (!isatty(STDIN_FILENO)) {
    return false;
  }
#ifdef LIVOX_MID360_HAS_NCURSES
  if (g_curses_active.load()) {
    nodelay(stdscr, FALSE);
    bool return_to_menu = false;
    while (true) {
      const int key = getch();
      if (allow_menu_return && (key == 'm' || key == 'M')) {
        return_to_menu = true;
        break;
      }
      if (key == '\n' || key == '\r' || key == 'q' || key == 'Q' || key == 27) {
        break;
      }
    }
    nodelay(stdscr, TRUE);
    return return_to_menu;
  }
#endif
  neon::RawTerminal raw_terminal;
  if (!raw_terminal.enter()) {
    return false;
  }
  bool return_to_menu = false;
  while (true) {
    const neon::Key key = neon::read_key(-1, 20);
    if (allow_menu_return && key == neon::Key::Menu) {
      return_to_menu = true;
      break;
    }
    if (key == neon::Key::Enter || key == neon::Key::Quit || key == neon::Key::Escape) {
      break;
    }
  }
  return return_to_menu;
}

bool print_discovery_error_screen(const std::string& title, const std::string& message) {
  if (!terminal_dashboard_active()) {
    std::cerr << "ERROR: " << message << "\n";
    return false;
  }
  const TerminalSize term = current_terminal_size();
  const int screen_rows = std::max(10, term.rows);
  const int width = std::max(58, term.cols);
  int used_rows = 3;
  std::ostringstream out;
  out << "\033[48;5;233m\033[38;5;252m\033[2J\033[H"
      << ansi_text("📡 LIVOX MID-360 [" + title + "]", 4, true)
      << std::string(std::max(1, width - 42 - static_cast<int>(title.size())), ' ')
      << ansi_text(format_short_clock_time(std::chrono::system_clock::now()) + "  ⚙", 6)
      << "\n"
      << ansi_text(repeat_text("─", width), 1)
      << "\n"
      << ansi_text(repeat_text("─", width), 4)
      << "\n";
  std::vector<std::string> status_rows = {
      "STATUS       " + ansi_text("[STATE: ERROR]", 4, true),
      "MESSAGE      " + ansi_text(message, 4, true),
      "",
      "No live MID360 callback was observed during SDK discovery.",
      "Check lidar power, Ethernet link, host IP/subnet, and firewall.",
  };
  append_lines_fit(out, boxed_lines("DISCOVERY RESULT", status_rows, width), used_rows, screen_rows - 2);
  const int max_rows = std::max(3, screen_rows - used_rows - 4);
  std::vector<std::string> trace_rows;
  const size_t begin = g_discovery_lines.size() > static_cast<size_t>(max_rows)
      ? g_discovery_lines.size() - static_cast<size_t>(max_rows)
      : 0;
  for (size_t i = begin; i < g_discovery_lines.size(); ++i) {
    trace_rows.push_back(g_discovery_lines[i]);
  }
  append_lines_fit(out, boxed_lines("LAST TRACE", trace_rows, width), used_rows, screen_rows - 1);
  while (used_rows++ < screen_rows - 1) {
    out << neon::blank_line(width) << "\n";
  }
  out << "\033[48;5;51m\033[30;1m"
      << ansi_pad_right(menu_return_enabled() ? " [M] MENU   [ENTER/Q/ESC] EXIT " : " [ENTER/Q/ESC] EXIT ", width)
      << "\033[0m";
  std::cout << out.str() << std::flush;
  return wait_for_result_key(menu_return_enabled());
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
  g_reset_requested = false;
  g_internal_query_inflight = false;
  g_internal_query_started = {};
}

void reset_stream_counters_preserving_device() {
  std::lock_guard<std::mutex> lock(g_mutex);
  Counters preserved;
  preserved.handle = g_counters.handle;
  preserved.dev_type = g_counters.dev_type;
  preserved.sn = g_counters.sn;
  preserved.lidar_ip = g_counters.lidar_ip;
  preserved.lidar_mask = g_counters.lidar_mask;
  preserved.lidar_gateway = g_counters.lidar_gateway;
  preserved.state_endpoint = g_counters.state_endpoint;
  preserved.point_endpoint = g_counters.point_endpoint;
  preserved.imu_endpoint = g_counters.imu_endpoint;
  preserved.ctl_endpoint = g_counters.ctl_endpoint;
  preserved.log_endpoint = g_counters.log_endpoint;
  preserved.product_info = g_counters.product_info;
  preserved.firmware_version = g_counters.firmware_version;
  preserved.loader_version = g_counters.loader_version;
  preserved.hardware_version = g_counters.hardware_version;
  preserved.mac = g_counters.mac;
  preserved.control_status = "stream counters reset";
  preserved.status_code = g_counters.status_code;
  preserved.lidar_diag_status = g_counters.lidar_diag_status;
  preserved.core_temp = g_counters.core_temp;
  preserved.environment_temp = g_counters.environment_temp;
  preserved.pcl_data_type = g_counters.pcl_data_type;
  preserved.pattern_mode = g_counters.pattern_mode;
  preserved.dual_emit_en = g_counters.dual_emit_en;
  preserved.frame_rate = g_counters.frame_rate;
  preserved.fov_cfg_en = g_counters.fov_cfg_en;
  preserved.detect_mode = g_counters.detect_mode;
  preserved.work_mode_after_boot = g_counters.work_mode_after_boot;
  preserved.glass_heat = g_counters.glass_heat;
  preserved.fusa_en = g_counters.fusa_en;
  preserved.force_heat_en = g_counters.force_heat_en;
  preserved.esc_mode = g_counters.esc_mode;
  preserved.pps_sync_mode = g_counters.pps_sync_mode;
  preserved.lidar_flash_status = g_counters.lidar_flash_status;
  preserved.fw_type = g_counters.fw_type;
  preserved.cur_glass_heat_state = g_counters.cur_glass_heat_state;
  preserved.roi_mode = g_counters.roi_mode;
  preserved.powerup_cnt = g_counters.powerup_cnt;
  preserved.has_powerup_cnt = g_counters.has_powerup_cnt;
  preserved.blind_spot_set = g_counters.blind_spot_set;
  preserved.has_blind_spot_set = g_counters.has_blind_spot_set;
  preserved.local_time_now = g_counters.local_time_now;
  preserved.has_local_time_now = g_counters.has_local_time_now;
  preserved.last_sync_time = g_counters.last_sync_time;
  preserved.has_last_sync_time = g_counters.has_last_sync_time;
  preserved.time_offset = g_counters.time_offset;
  preserved.has_time_offset = g_counters.has_time_offset;
  preserved.work_state = g_counters.work_state;
  preserved.point_send_en = g_counters.point_send_en;
  preserved.imu_data_en = g_counters.imu_data_en;
  preserved.time_sync_type = g_counters.time_sync_type;
  std::memcpy(preserved.hms_code, g_counters.hms_code, sizeof(preserved.hms_code));
  preserved.has_hms_code = g_counters.has_hms_code;
  preserved.internal_status = g_counters.internal_status;
  preserved.last_internal_info = g_counters.last_internal_info;
  preserved.last_callback = g_counters.last_callback;
  g_counters = preserved;
}

void register_sdk_callbacks(Options& options) {
  SetLivoxLidarPointCloudCallBack(PointCloudCallback, nullptr);
  SetLivoxLidarImuDataCallback(ImuDataCallback, nullptr);
  SetLivoxLidarInfoCallback(PushMsgCallback, &options);
  SetLivoxLidarInfoChangeCallback(LidarInfoChangeCallback, &options);
}

bool wait_for_lidar(double timeout_sec) {
  const auto deadline = std::chrono::steady_clock::now() +
      std::chrono::milliseconds(static_cast<int>(timeout_sec * 1000.0));
  auto next_repaint = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
  while (g_running.load() && !g_seen_lidar.load() && std::chrono::steady_clock::now() < deadline) {
    handle_terminal_input();
    const auto now = std::chrono::steady_clock::now();
    if (now >= next_repaint) {
      refresh_discovery_dashboard("", true);
      next_repaint = now + std::chrono::milliseconds(100);
    }
    std::unique_lock<std::mutex> lock(g_mutex);
    g_cv.wait_for(lock, std::chrono::milliseconds(20));
  }
  return g_seen_lidar.load();
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

bool init_sdk_by_discovery(Options& options, InterfaceInfo& active_iface, SdkSession& sdk) {
  const std::vector<InterfaceInfo> candidates = mid360_net::sdk_discovery_candidates(
      options.iface,
      options.host_ip,
      options.auto_bind_livox_subnet,
      options.auto_bind_ip);
  if (candidates.empty()) {
    throw std::runtime_error(
        "no usable IPv4 interfaces found; pass --host-ip, --iface, or add 192.168.1.5/24 to the lidar NIC");
  }

  std::ostringstream candidate_line;
  candidate_line << "Livox-SDK2 scan candidates:";
  for (const auto& candidate : candidates) {
    candidate_line << " " << mid360_net::describe_discovery_candidate(candidate);
  }
  discovery_log(candidate_line.str());

  for (const auto& candidate : candidates) {
    if (!g_running) {
      return false;
    }
    reset_monitor_state();
    discovery_log(
        "listening on " + mid360_net::describe_discovery_candidate(candidate) +
        " for " + fixed_number(options.discovery_timeout_sec, 1) + "s");
    mid360_net::TemporaryIpv4Address temporary_host;
    if (candidate.auto_bind_livox_subnet) {
      std::string error;
      if (!temporary_host.add(candidate.name, candidate.ip, candidate.prefix, error)) {
        discovery_log("WARN: " + error, true);
        continue;
      }
      discovery_log("temporarily added " + temporary_host.cidr() + " to " + candidate.name, true);
    }
    TempConfig temp_config = write_discovery_config(candidate);
    if (!LivoxLidarSdkInit(temp_config.path.c_str())) {
      LivoxLidarSdkUninit();
      discovery_log("WARN: LivoxLidarSdkInit discovery failed on " + mid360_net::describe_discovery_candidate(candidate), true);
      continue;
    }
    register_sdk_callbacks(options);
    if (wait_for_lidar(options.discovery_timeout_sec)) {
      if (!g_running.load()) {
        leave_terminal_dashboard();
        report_interrupted_once();
        LivoxLidarSdkUninit();
        return false;
      }
      active_iface = candidate;
      sdk.temporary_host = std::move(temporary_host);
      sdk.initialized = true;
      std::lock_guard<std::mutex> lock(g_mutex);
      discovery_log(
          "found lidar ip=" + (g_counters.lidar_ip.empty() ? std::string("N/A") : g_counters.lidar_ip) +
          " sn=" + (g_counters.sn.empty() ? std::string("N/A") : g_counters.sn) +
          " via " + mid360_net::describe_discovery_candidate(candidate),
          true);
      return true;
    }
    if (!g_running.load()) {
      leave_terminal_dashboard();
      report_interrupted_once();
      LivoxLidarSdkUninit();
      return false;
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
  const TerminalSize term = current_terminal_size();
  if (terminal_dashboard_active() && term.rows >= 18 && term.cols >= 72) {
    const int sidebar_w = std::min(36, std::max(26, term.cols / 4));
    const int main_w = std::max(40, term.cols - sidebar_w - 2);
    const int metric_w = std::max(20, (main_w - 1) / 2);
    const int preview_w = std::max(24, main_w * 2 / 3);
    const int diag_w = std::max(24, main_w - preview_w - 1);
    const int lower_h = std::max(6, term.rows - 13);
    const int map_h = std::max(3, lower_h - 3);
    const int map_w = std::max(10, preview_w - 4);
    const bool pulse = (snapshot.point_packets / 2) % 2 == 0;

    if (term.cols < 104) {
      const int point_color = values.points_per_sec > 520000.0 ? 3 : 1;
      const int imu_color = values.imu_samples_per_sec < 50.0 ? 3 : 2;
      const std::string traffic =
          "TRAFFIC " + fixed_number(values.point_mbps, 2) + " Mbps  TYPE " +
          data_type_name(snapshot.last_point_type) + "  UDP " + std::to_string(snapshot.last_point_udp);
      int compact_rows = 3;
      const int max_body_rows = std::max(3, term.rows - 2);

      std::ostringstream compact;
      compact << "\033[48;5;233m\033[38;5;252m"
              << ansi_text("📡 LIVOX MID-360 [LIVE]", 1, true)
              << std::string(std::max(1, term.cols - 40), ' ')
              << ansi_text(format_short_clock_time(std::chrono::system_clock::now()) + "  ⚙", 6)
              << "\n"
              << ansi_text(repeat_text("─", term.cols), 1)
              << "\n"
              << ansi_text(repeat_text("─", term.cols), 5)
              << "\n";

      if (term.cols >= 84) {
        const int gap = 1;
        const int left_w = (term.cols - gap) / 2;
        const int right_w = term.cols - left_w - gap;
        append_hstack_fit(
            compact,
            boxed_lines("DEVICE IDENTITY", monitor_identity_rows_compact(snapshot, elapsed, now, left_w), left_w),
            compact_metric_box("POINT RATE", format_compact_rate(values.points_per_sec, "pt/s"),
                               values.points_per_sec, 600000.0, point_color, pulse, right_w),
            left_w,
            right_w,
            compact_rows,
            max_body_rows);
        append_hstack_fit(
            compact,
            stream_status_box(values, snapshot, now, left_w),
            compact_metric_box("IMU FREQUENCY", fixed_number(values.imu_samples_per_sec, 1) + " Hz",
                               values.imu_samples_per_sec, 600.0, imu_color, !pulse, right_w),
            left_w,
            right_w,
            compact_rows,
            max_body_rows);
        const auto network = boxed_lines("NETWORK", network_rows_compact(snapshot, term.cols), term.cols);
        if (max_body_rows - compact_rows >= static_cast<int>(network.size())) {
          append_lines_fit(compact, network, compact_rows, max_body_rows);
        }
        if (max_body_rows - compact_rows >= 5) {
          append_lines_fit(
              compact,
              density_box(snapshot, values.points_per_sec, term.cols, std::max(5, max_body_rows - compact_rows)),
              compact_rows,
              max_body_rows);
        }
      } else {
        append_lines_fit(compact, boxed_lines(
                                      "DEVICE IDENTITY",
                                      monitor_identity_rows_compact(snapshot, elapsed, now, term.cols),
                                      term.cols),
                         compact_rows,
                         max_body_rows);
        const auto network = boxed_lines("NETWORK", network_rows_compact(snapshot, term.cols), term.cols);
        if (max_body_rows - compact_rows >= static_cast<int>(network.size())) {
          append_lines_fit(compact, network, compact_rows, max_body_rows);
        }
        const auto point_metric = compact_metric_box("POINT RATE", format_compact_rate(values.points_per_sec, "pt/s"),
                                                     values.points_per_sec, 600000.0, point_color, pulse, term.cols);
        if (max_body_rows - compact_rows >= static_cast<int>(point_metric.size())) {
          append_lines_fit(compact, point_metric,
                           compact_rows,
                           max_body_rows);
        }
        const auto imu_metric = compact_metric_box("IMU FREQUENCY", fixed_number(values.imu_samples_per_sec, 1) + " Hz",
                                                   values.imu_samples_per_sec, 600.0, imu_color, !pulse, term.cols);
        if (max_body_rows - compact_rows >= static_cast<int>(imu_metric.size())) {
          append_lines_fit(compact, imu_metric,
                           compact_rows,
                           max_body_rows);
        }
        const auto stream = stream_status_box(values, snapshot, now, term.cols);
        if (max_body_rows - compact_rows >= static_cast<int>(stream.size())) {
          append_lines_fit(compact, stream, compact_rows, max_body_rows);
        }
      }

      if (compact_rows < term.rows - 1) {
        compact << ansi_pad_right(ansi_text(ansi_fit(traffic, term.cols), 6), term.cols) << "\n";
        ++compact_rows;
      }
      while (compact_rows++ < term.rows - 1) {
        compact << neon::blank_line(term.cols) << "\n";
      }
      compact << "\033[48;5;51m\033[30;1m"
              << ansi_pad_right(" [F1] HELP   [F5] RESET   [F10] MENU   [CTRL+C] STOP ", term.cols)
              << "\033[0m";
      g_live_renderer.render(compact.str(), term.rows, term.cols);
      return;
    }

    std::ostringstream out;
    int used_rows = 3;
    const int max_body_rows = std::max(3, term.rows - 2);
    out << "\033[48;5;233m\033[38;5;252m"
        << ansi_text("📡 LIVOX MID-360 [LIVE]", 1, true)
        << std::string(std::max(1, term.cols - 40), ' ')
        << ansi_text(format_short_clock_time(std::chrono::system_clock::now()) + "  ⚙", 6)
        << "\n"
        << ansi_text(repeat_text("─", term.cols), 1)
        << "\n"
        << ansi_text(repeat_text("─", term.cols), 5)
        << "\n";

    const std::vector<std::string> identity_lines =
        device_identity_box(snapshot, elapsed, now, sidebar_w);
    const std::vector<std::string> point_metric_lines =
        metric_box("POINT RATE",
                   format_compact_rate(values.points_per_sec, "pt/s"),
                   values.points_per_sec,
                   600000.0,
                   "600kpt/s",
                   values.points_per_sec > 520000.0 ? 3 : 1,
                   pulse,
                   metric_w);
    const std::vector<std::string> imu_metric_lines =
        metric_box("IMU FREQUENCY",
                   fixed_number(values.imu_samples_per_sec, 1) + " Hz",
                   values.imu_samples_per_sec,
                   600.0,
                   "600Hz",
                   values.imu_samples_per_sec < 50.0 ? 3 : 2,
                   !pulse,
                   metric_w);
    const size_t top_rows = std::max({identity_lines.size(), point_metric_lines.size(), imu_metric_lines.size()});
    const std::string blank_sidebar(static_cast<size_t>(sidebar_w), ' ');
    const std::string blank_metric(static_cast<size_t>(metric_w), ' ');
    for (size_t i = 0; i < top_rows && used_rows < max_body_rows; ++i) {
      out << (i < identity_lines.size() ? identity_lines[i] : blank_sidebar)
        << " "
          << (i < point_metric_lines.size() ? point_metric_lines[i] : blank_metric)
        << " "
          << (i < imu_metric_lines.size() ? imu_metric_lines[i] : blank_metric)
        << "\n";
      ++used_rows;
    }

    const bool show_lower = used_rows + 3 < max_body_rows && preview_w >= 24 && diag_w >= 24;
    if (show_lower) {
      out << blank_sidebar
          << " "
          << ansi_box_top_with_right("POINT CLOUD PREVIEW", preview_w, "┬")
          << ansi_box_top_no_left("STREAM STATUS", diag_w)
          << "\n";
      ++used_rows;
    }
    const std::vector<std::string> network_body = network_rows(snapshot, sidebar_w);
    auto left_panel_line = [&](int row) {
      if (row == 0) {
        return ansi_box_top("NETWORK", sidebar_w);
      }
      if (row > 0 && row - 1 < static_cast<int>(network_body.size())) {
        return network_body[static_cast<size_t>(row - 1)];
      }
      return ansi_box_row("", sidebar_w);
    };
    static const char* shades = " .:*#@";
    const double energy = clamp_double(values.points_per_sec / 600000.0, 0.05, 1.0);
    const int phase = static_cast<int>((snapshot.last_point_udp + snapshot.point_packets) % 97);
    for (int row = 0; show_lower && row < map_h && used_rows < max_body_rows; ++row) {
      out << left_panel_line(row) << " ";
      std::string line;
      for (int col = 0; col < map_w; ++col) {
        const double cx = (col - map_w / 2.0) / std::max(1.0, map_w / 2.0);
        const double cy = (row - map_h / 2.0) / std::max(1.0, map_h / 2.0);
        const double ring = std::sin((cx * cx + cy * cy) * 11.0 + phase * 0.07);
        const double sweep = std::cos(cx * 8.0 - phase * 0.11) * std::sin(cy * 5.0 + phase * 0.05);
        double density = (ring + sweep + 2.0) / 4.0 * energy;
        density *= 1.0 - std::min(0.7, std::sqrt(cx * cx + cy * cy) * 0.45);
        line.push_back(shades[clamp_int(static_cast<int>(density * 6.0), 0, 5)]);
      }
      out << "│ " << ansi_text(line, row % 3 == 0 ? 1 : 5)
          << std::string(static_cast<size_t>(std::max(0, preview_w - map_w - 3)), ' ')
          << "│";
      const std::vector<std::string> status_rows = stream_status_box_rows_no_left(values, snapshot, now, diag_w);
      out << (row < static_cast<int>(status_rows.size())
                  ? status_rows[static_cast<size_t>(row)]
                  : ansi_box_row_no_left("", diag_w));
      out << "\n";
      ++used_rows;
    }
    if (show_lower && used_rows < max_body_rows) {
      out << ansi_text("└" + repeat_text("─", std::max(1, sidebar_w - 2)) + "┘", 1)
          << " "
          << ansi_box_bottom_with_right(preview_w, "┴")
          << ansi_box_bottom_no_left(diag_w)
          << "\n";
      ++used_rows;
    }
    if (used_rows < term.rows - 1) {
      out << ansi_pad_right(ansi_text("TRAFFIC " + fixed_number(values.point_mbps, 2) + " Mbps  TYPE " +
                           data_type_name(snapshot.last_point_type) + "  UDP " + std::to_string(snapshot.last_point_udp),
                       6),
                    term.cols)
          << "\n";
      ++used_rows;
    }
    while (used_rows++ < term.rows - 1) {
      out << neon::blank_line(term.cols) << "\n";
    }
    out << "\033[48;5;51m\033[30;1m"
        << ansi_pad_right(" [F1] HELP   [F5] RESET   [F10] MENU   [CTRL+C] STOP ", term.cols)
        << "\033[0m";
    g_live_renderer.render(out.str(), term.rows, term.cols);
    return;
  }
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

  print_table_row(table, "NETWORK", "mask", snapshot.lidar_mask.empty() ? "N/A" : snapshot.lidar_mask);
  print_table_row(table, "NETWORK", "gateway", snapshot.lidar_gateway.empty() ? "N/A" : snapshot.lidar_gateway);
  print_table_row(table, "NETWORK", "state", endpoint_summary(snapshot.state_endpoint));
  print_table_row(table, "NETWORK", "point", endpoint_summary(snapshot.point_endpoint));
  print_table_row(table, "NETWORK", "imu", endpoint_summary(snapshot.imu_endpoint));
  print_table_row(table, "NETWORK", "control", endpoint_summary(snapshot.ctl_endpoint));
  print_table_row(table, "NETWORK", "log", endpoint_summary(snapshot.log_endpoint));
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

  const std::string table_frame = table.str();
  if (terminal_dashboard_active()) {
    g_live_renderer.render(table_frame, term.rows, term.cols);
  } else {
    std::cout << "\033[H\033[2J" << table_frame;
    std::cout.flush();
  }
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
            << " product=" << (snapshot.product_info.empty() ? "N/A" : snapshot.product_info)
            << " handle=" << snapshot.handle
            << " dev_type=" << static_cast<unsigned>(snapshot.dev_type)
            << " ip=" << (snapshot.lidar_ip.empty() ? "N/A" : snapshot.lidar_ip)
            << " mask=" << (snapshot.lidar_mask.empty() ? "N/A" : snapshot.lidar_mask)
            << " gateway=" << (snapshot.lidar_gateway.empty() ? "N/A" : snapshot.lidar_gateway)
            << " net_state=" << endpoint_summary(snapshot.state_endpoint)
            << " net_point=" << endpoint_summary(snapshot.point_endpoint)
            << " net_imu=" << endpoint_summary(snapshot.imu_endpoint)
            << " net_control=" << endpoint_summary(snapshot.ctl_endpoint)
            << " net_log=" << endpoint_summary(snapshot.log_endpoint)
            << " link=" << connection_text(snapshot, now)
            << " health=" << health_text(snapshot.lidar_diag_status)
            << " diag=" << snapshot.lidar_diag_status
            << " core_temp=" << temp_text(snapshot.core_temp)
            << " env_temp=" << temp_text(snapshot.environment_temp)
            << " pcl_data_type=" << (snapshot.pcl_data_type < 0 ? "N/A" : data_type_name(static_cast<uint8_t>(snapshot.pcl_data_type)))
            << " pattern=" << (snapshot.pattern_mode < 0 ? "N/A" : std::to_string(snapshot.pattern_mode))
            << " dual_emit=" << on_off_text(snapshot.dual_emit_en)
            << " frame_rate=" << (snapshot.frame_rate < 0 ? "N/A" : std::to_string(snapshot.frame_rate))
            << " fov_enable=" << on_off_text(snapshot.fov_cfg_en)
            << " detect_mode=" << (snapshot.detect_mode < 0 ? "N/A" : std::to_string(snapshot.detect_mode))
            << " work=" << work_state_text(snapshot.work_state)
            << " boot_work=" << work_state_text(snapshot.work_mode_after_boot)
            << " glass_heat=" << on_off_text(snapshot.glass_heat)
            << " glass_heat_state=" << (snapshot.cur_glass_heat_state < 0 ? "N/A" : std::to_string(snapshot.cur_glass_heat_state))
            << " point_send=" << on_off_text(snapshot.point_send_en)
            << " imu_enable=" << on_off_text(snapshot.imu_data_en)
            << " fusa=" << on_off_text(snapshot.fusa_en)
            << " force_heat=" << on_off_text(snapshot.force_heat_en)
            << " esc_mode=" << (snapshot.esc_mode < 0 ? "N/A" : std::to_string(snapshot.esc_mode))
            << " pps_sync=" << (snapshot.pps_sync_mode < 0 ? "N/A" : std::to_string(snapshot.pps_sync_mode))
            << " time_sync=" << time_sync_type_text(snapshot.time_sync_type)
            << " time_sync_raw=" << snapshot.time_sync_type
            << " local_time=" << (snapshot.has_local_time_now ? std::to_string(snapshot.local_time_now) : "N/A")
            << " last_sync=" << (snapshot.has_last_sync_time ? std::to_string(snapshot.last_sync_time) : "N/A")
            << " time_offset=" << (snapshot.has_time_offset ? std::to_string(snapshot.time_offset) : "N/A")
            << " fw=" << firmware_text(snapshot)
            << " loader=" << (snapshot.loader_version.empty() ? "N/A" : snapshot.loader_version)
            << " hw=" << (snapshot.hardware_version.empty() ? "N/A" : snapshot.hardware_version)
            << " mac=" << (snapshot.mac.empty() ? "N/A" : snapshot.mac)
            << " fw_type=" << (snapshot.fw_type < 0 ? "N/A" : std::to_string(snapshot.fw_type))
            << " flash=" << (snapshot.lidar_flash_status < 0 ? "N/A" : std::to_string(snapshot.lidar_flash_status))
            << " powerups=" << (snapshot.has_powerup_cnt ? std::to_string(snapshot.powerup_cnt) : "N/A")
            << " blind_spot=" << (snapshot.has_blind_spot_set ? std::to_string(snapshot.blind_spot_set) : "N/A")
            << " roi=" << (snapshot.roi_mode < 0 ? "N/A" : std::to_string(snapshot.roi_mode))
            << " status_code=" << (snapshot.status_code.empty() ? "N/A" : snapshot.status_code)
            << " hms=" << (snapshot.has_hms_code ? format_hms_code(snapshot.hms_code, 8) : "N/A")
            << " info_src=" << (snapshot.internal_status.empty() ? "N/A" : snapshot.internal_status)
            << " info_age=" << internal_age_text(snapshot, now)
            << " points=" << format_compact_rate(values.points_per_sec, "pt/s")
            << " point_packets=" << format_rate(values.point_pps, "pkt/s")
            << " point_mbps=" << fixed_number(values.point_mbps, 2)
            << " imu=" << format_rate(values.imu_samples_per_sec, "sample/s")
            << " type=" << data_type_name(snapshot.last_point_type)
            << " frame=" << static_cast<unsigned>(snapshot.last_point_frame)
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
#ifdef LIVOX_MID360_HAS_NCURSES
    if (g_curses_active.load()) {
      print_neon_dashboard(delta, interval, elapsed, snapshot, now, options);
      return;
    }
#endif
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
      << "  --auto-bind-ip IP      temporary host IP for 192.168.1.x lidar NICs (default: 192.168.1.5)\n"
      << "  --no-auto-bind         do not temporarily add 192.168.1.x/24 to candidate NICs\n"
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
    } else if (arg == "--auto-bind-ip") {
      options.auto_bind_ip = need_value(arg);
    } else if (arg == "--no-auto-bind") {
      options.auto_bind_livox_subnet = false;
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
  if (!mid360_net::valid_ipv4(options.auto_bind_ip) || !mid360_net::livox_subnet_ip(options.auto_bind_ip)) {
    throw std::runtime_error("--auto-bind-ip must be in 192.168.1.x");
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

    if (options.config_path.empty()) {
      enter_discovery_dashboard();
      if (terminal_dashboard_active()) {
        print_discovery_dashboard("waiting for Livox-SDK2 discovery");
      }
    }

    InterfaceInfo active_iface;
    if (!options.config_path.empty()) {
      if (!init_sdk_with_config(options)) {
        if (terminal_dashboard_active()) {
          const bool return_to_menu =
              print_discovery_error_screen("SDK ERROR", "LivoxLidarSdkInit failed for " + options.config_path);
          leave_terminal_dashboard();
          if (return_to_menu) {
            LivoxLidarSdkUninit();
            return kReturnToMenuCode;
          }
        } else {
          std::cerr << "ERROR: LivoxLidarSdkInit failed for " << options.config_path << "\n";
        }
        LivoxLidarSdkUninit();
        return 2;
      }
      sdk.initialized = true;
      std::cout << "config: " << options.config_path << std::endl;
    } else {
      if (!init_sdk_by_discovery(options, active_iface, sdk)) {
        if (!g_running.load()) {
          leave_terminal_dashboard();
          report_interrupted_once();
          return 130;
        }
        if (terminal_dashboard_active()) {
          const bool return_to_menu =
              print_discovery_error_screen("NOT FOUND", "no MID360 lidar found by SDK discovery");
          leave_terminal_dashboard();
          if (return_to_menu) {
            return kReturnToMenuCode;
          }
        } else {
          std::cerr << "ERROR: no MID360 lidar found by SDK discovery\n";
        }
        return 2;
      }
    }

    g_discovery_lines.clear();
    if (terminal_dashboard_active()) {
      leave_terminal_dashboard();
    }
    enter_terminal_dashboard();
    g_live_renderer.reset();
    if (terminal_dashboard_active()) {
      std::cout << neon::clear_screen() << std::flush;
    }

    const auto started = std::chrono::steady_clock::now();
    auto last = started;
    const auto report_interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(options.interval_sec));
    const auto frame_interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(terminal_dashboard_active() ? kTuiFrameSec : options.interval_sec));
    auto next_report = started + report_interval;
    auto next_frame = started + frame_interval;
    Counters previous;
    Counters last_delta;
    double last_interval = options.interval_sec;
    auto next_internal_query = started;

    while (g_running) {
      std::unique_lock<std::mutex> lock(g_mutex);
      const auto wake_at = terminal_dashboard_active()
          ? std::min({next_report, next_frame, next_internal_query})
          : std::min(next_report, next_internal_query);
      g_cv.wait_until(lock, wake_at, [] {
        return !g_running.load() || g_reset_requested.load();
      });
      if (!g_running) {
        break;
      }
      const auto now = std::chrono::steady_clock::now();
      if (g_reset_requested.exchange(false)) {
        lock.unlock();
        reset_stream_counters_preserving_device();
        lock.lock();
        previous = g_counters;
        last_delta = Counters{};
        last = now;
        next_report = now + report_interval;
        next_frame = now;
        next_internal_query = now;
      }
      const double elapsed = std::chrono::duration<double>(now - started).count();
      Counters snapshot = g_counters;
      lock.unlock();

      handle_terminal_input();
      if (!g_running) {
        break;
      }

      if (next_internal_query <= now) {
        maybe_query_internal_info(snapshot.handle);
        while (next_internal_query <= now) {
          next_internal_query += std::chrono::duration_cast<std::chrono::steady_clock::duration>(
              std::chrono::duration<double>(kInternalQueryIntervalSec));
        }
      }

      if (next_report <= now) {
        const double interval = std::chrono::duration<double>(now - last).count();
        Counters delta;
        delta.point_packets = snapshot.point_packets - previous.point_packets;
        delta.point_units = snapshot.point_units - previous.point_units;
        delta.point_bytes = snapshot.point_bytes - previous.point_bytes;
        delta.imu_packets = snapshot.imu_packets - previous.imu_packets;
        delta.imu_units = snapshot.imu_units - previous.imu_units;
        delta.imu_bytes = snapshot.imu_bytes - previous.imu_bytes;
        last_delta = delta;
        last_interval = std::max(0.001, interval);
        previous = snapshot;
        last = now;
        while (next_report <= now) {
          next_report += report_interval;
        }

        if (!terminal_dashboard_active()) {
          print_report(last_delta, last_interval, elapsed, snapshot, now, options);
        }
      }

      if (terminal_dashboard_active() && next_frame <= now) {
        print_report(last_delta, last_interval, elapsed, snapshot, now, options);
        while (next_frame <= now) {
          next_frame += frame_interval;
        }
      }
      if (options.duration_sec > 0.0 && elapsed >= options.duration_sec) {
        break;
      }
    }

    const bool was_dashboard = terminal_dashboard_active();
    sdk.stop();
    leave_terminal_dashboard();
    if (g_user_interrupted.load()) {
      report_interrupted_once();
      return 130;
    }
    if (!was_dashboard) {
      std::cout << "stopped" << std::endl;
    }
    return 0;
  } catch (const std::exception& exc) {
    bool return_to_menu = false;
    if (terminal_dashboard_active()) {
      return_to_menu = print_discovery_error_screen("ERROR", exc.what());
    }
    leave_terminal_dashboard();
    if (return_to_menu) {
      return kReturnToMenuCode;
    }
    if (!isatty(STDOUT_FILENO)) {
      std::cerr << "ERROR: " << exc.what() << "\n";
    }
    return 2;
  }
}
