#include "livox_lidar_api.h"
#include "livox_lidar_def.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <csignal>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

constexpr double kPi = 3.14159265358979323846;

struct Options {
  std::string config_path;
  std::string points_path = "mid360_points.csv";
  std::string imu_path;
  uint64_t max_points = 0;
  double duration_sec = 0.0;
  double interval_sec = 1.0;
  bool set_normal_mode = false;
  bool enable_point_send = false;
  bool enable_imu = false;
  bool quiet_sdk_log = true;
};

struct Stats {
  uint64_t point_packets = 0;
  uint64_t points = 0;
  uint64_t imu_packets = 0;
  uint64_t imu_samples = 0;
  uint64_t unsupported_packets = 0;
  uint32_t handle = 0;
  std::string sn;
  std::string lidar_ip;
};

Options g_options;
Stats g_stats;
std::ofstream g_points;
std::ofstream g_imu;
std::mutex g_mutex;
std::condition_variable g_cv;
std::atomic<bool> g_running{true};
std::atomic<bool> g_control_requested{false};

void stop(int) {
  g_running = false;
  g_cv.notify_all();
}

uint64_t packet_timestamp_ns(const LivoxLidarEthernetPacket* packet) {
  uint64_t value = 0;
  std::memcpy(&value, packet->timestamp, sizeof(value));
  return value;
}

double packet_time_interval_s(const LivoxLidarEthernetPacket* packet) {
  return static_cast<double>(packet->time_interval) * 0.1e-6;
}

void write_point_row(
    uint32_t handle,
    const LivoxLidarEthernetPacket* packet,
    uint32_t index,
    double x,
    double y,
    double z,
    uint8_t reflectivity,
    uint8_t tag,
    const char* data_type_name) {
  const uint64_t base_time = packet_timestamp_ns(packet);
  const double offset_s = static_cast<double>(index) * packet_time_interval_s(packet);
  g_points << base_time << ','
           << std::fixed << std::setprecision(9) << offset_s << ','
           << handle << ','
           << static_cast<unsigned>(packet->data_type) << ','
           << data_type_name << ','
           << static_cast<unsigned>(packet->frame_cnt) << ','
           << packet->udp_cnt << ','
           << index << ','
           << std::setprecision(6) << x << ','
           << y << ','
           << z << ','
           << static_cast<unsigned>(reflectivity) << ','
           << static_cast<unsigned>(tag) << '\n';
}

void decode_cartesian_high(uint32_t handle, LivoxLidarEthernetPacket* packet) {
  auto* raw = reinterpret_cast<LivoxLidarCartesianHighRawPoint*>(packet->data);
  for (uint32_t i = 0; i < packet->dot_num && g_running; ++i) {
    write_point_row(
        handle,
        packet,
        i,
        static_cast<double>(raw[i].x) / 1000.0,
        static_cast<double>(raw[i].y) / 1000.0,
        static_cast<double>(raw[i].z) / 1000.0,
        raw[i].reflectivity,
        raw[i].tag,
        "cartesian_high");
    g_stats.points += 1;
    if (g_options.max_points > 0 && g_stats.points >= g_options.max_points) {
      g_running = false;
      g_cv.notify_all();
      break;
    }
  }
}

void decode_cartesian_low(uint32_t handle, LivoxLidarEthernetPacket* packet) {
  auto* raw = reinterpret_cast<LivoxLidarCartesianLowRawPoint*>(packet->data);
  for (uint32_t i = 0; i < packet->dot_num && g_running; ++i) {
    write_point_row(
        handle,
        packet,
        i,
        static_cast<double>(raw[i].x) / 100.0,
        static_cast<double>(raw[i].y) / 100.0,
        static_cast<double>(raw[i].z) / 100.0,
        raw[i].reflectivity,
        raw[i].tag,
        "cartesian_low");
    g_stats.points += 1;
    if (g_options.max_points > 0 && g_stats.points >= g_options.max_points) {
      g_running = false;
      g_cv.notify_all();
      break;
    }
  }
}

void decode_spherical(uint32_t handle, LivoxLidarEthernetPacket* packet) {
  auto* raw = reinterpret_cast<LivoxLidarSpherPoint*>(packet->data);
  for (uint32_t i = 0; i < packet->dot_num && g_running; ++i) {
    const double radius = static_cast<double>(raw[i].depth) / 1000.0;
    const double theta = static_cast<double>(raw[i].theta) / 100.0 / 180.0 * kPi;
    const double phi = static_cast<double>(raw[i].phi) / 100.0 / 180.0 * kPi;
    const double x = radius * std::sin(theta) * std::cos(phi);
    const double y = radius * std::sin(theta) * std::sin(phi);
    const double z = radius * std::cos(theta);
    write_point_row(handle, packet, i, x, y, z, raw[i].reflectivity, raw[i].tag, "spherical");
    g_stats.points += 1;
    if (g_options.max_points > 0 && g_stats.points >= g_options.max_points) {
      g_running = false;
      g_cv.notify_all();
      break;
    }
  }
}

void point_cloud_callback(const uint32_t handle, const uint8_t dev_type, LivoxLidarEthernetPacket* packet, void*) {
  (void)dev_type;
  if (!packet || !g_points) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  g_stats.handle = handle;
  g_stats.point_packets += 1;
  switch (packet->data_type) {
    case kLivoxLidarCartesianCoordinateHighData:
      decode_cartesian_high(handle, packet);
      break;
    case kLivoxLidarCartesianCoordinateLowData:
      decode_cartesian_low(handle, packet);
      break;
    case kLivoxLidarSphericalCoordinateData:
      decode_spherical(handle, packet);
      break;
    default:
      g_stats.unsupported_packets += 1;
      break;
  }
}

void imu_callback(const uint32_t handle, const uint8_t dev_type, LivoxLidarEthernetPacket* packet, void*) {
  (void)dev_type;
  if (!packet || !g_imu) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  g_stats.handle = handle;
  g_stats.imu_packets += 1;
  auto* raw = reinterpret_cast<LivoxLidarImuRawPoint*>(packet->data);
  for (uint32_t i = 0; i < packet->dot_num; ++i) {
    const uint64_t base_time = packet_timestamp_ns(packet);
    const double offset_s = static_cast<double>(i) * packet_time_interval_s(packet);
    g_imu << base_time << ','
          << std::fixed << std::setprecision(9) << offset_s << ','
          << handle << ','
          << static_cast<unsigned>(packet->frame_cnt) << ','
          << packet->udp_cnt << ','
          << i << ','
          << std::setprecision(8)
          << raw[i].gyro_x << ','
          << raw[i].gyro_y << ','
          << raw[i].gyro_z << ','
          << raw[i].acc_x << ','
          << raw[i].acc_y << ','
          << raw[i].acc_z << '\n';
    g_stats.imu_samples += 1;
  }
}

void async_control_callback(livox_status status, uint32_t handle, LivoxLidarAsyncControlResponse* response, void*) {
  std::lock_guard<std::mutex> lock(g_mutex);
  std::cout << "control_response: handle=" << handle << " status=" << status;
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

void request_controls(uint32_t handle) {
  if (!g_options.set_normal_mode && !g_options.enable_point_send && !g_options.enable_imu) {
    return;
  }
  bool expected = false;
  if (!g_control_requested.compare_exchange_strong(expected, true)) {
    return;
  }
  if (g_options.set_normal_mode) {
    SetLivoxLidarWorkMode(handle, kLivoxLidarNormal, async_control_callback, nullptr);
  }
  if (g_options.enable_point_send) {
    EnableLivoxLidarPointSend(handle, async_control_callback, nullptr);
  }
  if (g_options.enable_imu) {
    EnableLivoxLidarImuData(handle, async_control_callback, nullptr);
  }
}

void lidar_info_change_callback(const uint32_t handle, const LivoxLidarInfo* info, void*) {
  if (!info) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_stats.handle = handle;
    g_stats.sn = info->sn;
    g_stats.lidar_ip = info->lidar_ip;
    std::cout << "lidar: handle=" << handle
              << " dev_type=" << static_cast<unsigned>(info->dev_type)
              << " sn=" << info->sn
              << " ip=" << info->lidar_ip << std::endl;
  }
  request_controls(handle);
}

void push_msg_callback(const uint32_t handle, const uint8_t dev_type, const char* info, void*) {
  (void)dev_type;
  if (!info) {
    return;
  }
  const std::string text(info);
  const std::string sn = json_string_value(text, "sn");
  const std::string lidar_ip = json_string_value(text, "lidar_ip");
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_stats.handle = handle;
    if (!sn.empty()) {
      g_stats.sn = sn;
    }
    if (!lidar_ip.empty()) {
      g_stats.lidar_ip = lidar_ip;
    }
  }
  request_controls(handle);
}

void print_report(const Stats& previous, const Stats& now, double interval, double elapsed) {
  const uint64_t packet_delta = now.point_packets - previous.point_packets;
  const uint64_t point_delta = now.points - previous.points;
  const uint64_t imu_packet_delta = now.imu_packets - previous.imu_packets;
  const uint64_t imu_delta = now.imu_samples - previous.imu_samples;
  std::cout << "elapsed=" << std::fixed << std::setprecision(1) << elapsed << "s"
            << " handle=" << now.handle
            << " sn=" << (now.sn.empty() ? "N/A" : now.sn)
            << " ip=" << (now.lidar_ip.empty() ? "N/A" : now.lidar_ip)
            << " point_packets=" << packet_delta
            << " point_rate=" << std::setprecision(1) << (point_delta / interval) << " pt/s"
            << " total_points=" << now.points
            << " imu_packets=" << imu_packet_delta
            << " imu_rate=" << std::setprecision(1) << (imu_delta / interval) << " sample/s"
            << " unsupported_packets=" << now.unsupported_packets
            << std::endl;
}

void usage(const char* argv0) {
  std::cout
      << "usage: " << argv0 << " --config MID360_config.json --points OUT.csv [options]\n"
      << "\n"
      << "Dump decoded Livox-SDK2 point cloud/IMU callbacks without ROS.\n"
      << "\n"
      << "options:\n"
      << "  --config PATH          MID360_config.json path\n"
      << "  --points PATH          point CSV output path (default: mid360_points.csv)\n"
      << "  --imu PATH             optional IMU CSV output path\n"
      << "  --max-points N         stop after N decoded points; 0 means no limit\n"
      << "  --duration SEC         stop after N seconds; 0 means forever\n"
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
    } else if (arg == "--points") {
      options.points_path = need_value(arg);
    } else if (arg == "--imu") {
      options.imu_path = need_value(arg);
    } else if (arg == "--max-points") {
      options.max_points = static_cast<uint64_t>(std::stoull(need_value(arg)));
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
  if (options.points_path.empty()) {
    throw std::runtime_error("--points cannot be empty");
  }
  if (options.interval_sec <= 0.0) {
    throw std::runtime_error("--interval must be positive");
  }
  return options;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    g_options = parse_args(argc, argv);
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);

    g_points.open(g_options.points_path, std::ios::out | std::ios::trunc);
    if (!g_points) {
      throw std::runtime_error("failed to open point output: " + g_options.points_path);
    }
    g_points << "timestamp_ns,offset_s,handle,data_type,data_type_name,frame_cnt,udp_cnt,point_index,x_m,y_m,z_m,reflectivity,tag\n";

    if (!g_options.imu_path.empty()) {
      g_imu.open(g_options.imu_path, std::ios::out | std::ios::trunc);
      if (!g_imu) {
        throw std::runtime_error("failed to open IMU output: " + g_options.imu_path);
      }
      g_imu << "timestamp_ns,offset_s,handle,frame_cnt,udp_cnt,sample_index,gyro_x,gyro_y,gyro_z,acc_x,acc_y,acc_z\n";
    }

    if (g_options.quiet_sdk_log) {
      DisableLivoxSdkConsoleLogger();
    }
    if (!LivoxLidarSdkInit(g_options.config_path.c_str())) {
      LivoxLidarSdkUninit();
      throw std::runtime_error("LivoxLidarSdkInit failed for " + g_options.config_path);
    }

    SetLivoxLidarPointCloudCallBack(point_cloud_callback, nullptr);
    SetLivoxLidarImuDataCallback(imu_callback, nullptr);
    SetLivoxLidarInfoCallback(push_msg_callback, nullptr);
    SetLivoxLidarInfoChangeCallback(lidar_info_change_callback, nullptr);

    std::cout << "config: " << g_options.config_path << "\n";
    std::cout << "points_csv: " << g_options.points_path << "\n";
    if (g_imu) {
      std::cout << "imu_csv: " << g_options.imu_path << "\n";
    }
    std::cout << "waiting for SDK callbacks; press Ctrl-C to stop." << std::endl;

    const auto started = std::chrono::steady_clock::now();
    auto last = started;
    Stats previous;
    while (g_running) {
      std::unique_lock<std::mutex> lock(g_mutex);
      g_cv.wait_for(lock, std::chrono::milliseconds(static_cast<int>(g_options.interval_sec * 1000.0)));
      const auto now_time = std::chrono::steady_clock::now();
      const double interval = std::chrono::duration<double>(now_time - last).count();
      const double elapsed = std::chrono::duration<double>(now_time - started).count();
      Stats snapshot = g_stats;
      lock.unlock();

      print_report(previous, snapshot, std::max(0.001, interval), elapsed);
      previous = snapshot;
      last = now_time;
      if (g_options.duration_sec > 0.0 && elapsed >= g_options.duration_sec) {
        break;
      }
    }

    LivoxLidarSdkUninit();
    g_points.flush();
    if (g_imu) {
      g_imu.flush();
    }
    std::cout << "stopped. wrote points=" << g_stats.points
              << " imu_samples=" << g_stats.imu_samples << std::endl;
    return 0;
  } catch (const std::exception& exc) {
    std::cerr << "ERROR: " << exc.what() << "\n";
    return 2;
  }
}
