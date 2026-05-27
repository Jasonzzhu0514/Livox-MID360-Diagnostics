#include "livox_lidar_api.h"
#include "livox_lidar_def.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
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
  int lidar_diag_status = -1;
  std::chrono::steady_clock::time_point last_callback;
};

struct Options {
  std::string config_path;
  double duration_sec = 0.0;
  double interval_sec = 1.0;
  bool enable_point_send = false;
  bool set_normal_mode = false;
  bool enable_imu = false;
  bool quiet_sdk_log = true;
};

std::mutex g_mutex;
std::condition_variable g_cv;
Counters g_counters;
std::atomic<bool> g_running{true};
std::atomic<bool> g_seen_lidar{false};
std::atomic<bool> g_control_requested{false};
std::atomic<bool> g_terminal_active{false};
constexpr double kCallbackTimeoutSec = 3.0;

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

void AsyncControlCallback(livox_status status, uint32_t handle, LivoxLidarAsyncControlResponse* response, void*) {
  std::lock_guard<std::mutex> lock(g_mutex);
  std::cout << "control_response: handle=" << handle
            << " status=" << status;
  if (response) {
    std::cout << " ret_code=" << static_cast<unsigned>(response->ret_code)
              << " error_key=" << response->error_key;
  }
  std::cout << std::endl;
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

std::string fixed_number(double value, int precision) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(precision) << value;
  return out.str();
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
  if (hours > 0) {
    out << hours << "h ";
  }
  if (hours > 0 || minutes > 0) {
    out << minutes << "m ";
  }
  out << secs << "s";
  return out.str();
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

void print_dashboard(
    const Counters& delta,
    double interval,
    double elapsed,
    const Counters& snapshot,
    std::chrono::steady_clock::time_point now) {
  const double point_pps = delta.point_packets / interval;
  const ReportValues values = make_report_values(delta, interval);

  std::cout << "\033[H";
  std::cout << "Livox MID360 Monitor\n";
  std::cout << "====================\n\n";
  std::cout << "Device\n";
  std::cout << "  SN      : " << (snapshot.sn.empty() ? "N/A" : snapshot.sn) << "\n";
  std::cout << "  IP      : " << (snapshot.lidar_ip.empty() ? "N/A" : snapshot.lidar_ip) << "\n";
  std::cout << "  Handle  : " << snapshot.handle << "\n";
  std::cout << "  Link    : " << connection_text(snapshot, now) << "\n";
  std::cout << "  Health  : " << health_text(snapshot.lidar_diag_status) << "\n\n";

  std::cout << "Runtime\n";
  std::cout << "  Elapsed : " << format_duration(elapsed) << "\n";
  std::cout << "  Type    : " << data_type_name(snapshot.last_point_type)
            << "    Frame: " << static_cast<unsigned>(snapshot.last_point_frame)
            << "    UDP: " << snapshot.last_point_udp << "\n\n";

  std::cout << "Point Cloud\n";
  std::cout << "  Packets : " << std::setw(14) << format_rate(values.point_pps, "pkt/s")
            << "    last=" << delta.point_packets << "\n";
  std::cout << "  Points  : " << std::setw(14) << format_compact_rate(values.points_per_sec, "pt/s") << "\n";
  std::cout << "  Traffic : " << std::setw(14) << (fixed_number(values.point_mbps, 2) + " Mbps") << "\n\n";

  std::cout << "IMU\n";
  std::cout << "  Packets : " << std::setw(14) << format_rate(values.imu_pps, "pkt/s")
            << "    last=" << delta.imu_packets << "\n";
  std::cout << "  Samples : " << std::setw(14) << format_rate(values.imu_samples_per_sec, "sample/s") << "\n";
  std::cout << "  Traffic : " << std::setw(14) << (fixed_number(values.imu_mbps, 2) + " Mbps") << "\n\n";

  std::cout << "Press Ctrl-C to stop.\n";
  std::cout.flush();
}

void usage(const char* argv0) {
  std::cout
      << "usage: " << argv0 << " --config MID360_config.json [options]\n"
      << "\n"
      << "Live terminal monitor for Livox-SDK2 point cloud and IMU callbacks.\n"
      << "It initializes the SDK only when explicitly run.\n"
      << "\n"
      << "options:\n"
      << "  --config PATH          MID360_config.json path\n"
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
  if (options.config_path.empty()) {
    throw std::runtime_error("--config is required");
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
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    enter_terminal_dashboard();

    if (options.quiet_sdk_log) {
      DisableLivoxSdkConsoleLogger();
    }
    if (!LivoxLidarSdkInit(options.config_path.c_str())) {
      leave_terminal_dashboard();
      std::cerr << "ERROR: LivoxLidarSdkInit failed for " << options.config_path << "\n";
      LivoxLidarSdkUninit();
      return 2;
    }

    SetLivoxLidarPointCloudCallBack(PointCloudCallback, nullptr);
    SetLivoxLidarImuDataCallback(ImuDataCallback, nullptr);
    SetLivoxLidarInfoCallback(PushMsgCallback, &options);
    SetLivoxLidarInfoChangeCallback(LidarInfoChangeCallback, &options);

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
      print_dashboard(delta, std::max(0.001, interval), elapsed, snapshot, now);

      previous = snapshot;
      last = now;
      if (options.duration_sec > 0.0 && elapsed >= options.duration_sec) {
        break;
      }
    }

    LivoxLidarSdkUninit();
    leave_terminal_dashboard();
    std::cout << "stopped" << std::endl;
    return 0;
  } catch (const std::exception& exc) {
    leave_terminal_dashboard();
    std::cerr << "ERROR: " << exc.what() << "\n";
    return 2;
  }
}
