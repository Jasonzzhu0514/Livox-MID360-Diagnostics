#include "network_autobind.hpp"

#include "livox_lidar_api.h"
#include "livox_lidar_def.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr uint32_t kDefaultPreviewPointLimit = 0;
constexpr uint32_t kMockPreviewPointCount = 6000;

struct Options {
  std::string config_path;
  std::string iface = "auto";
  std::string host_ip;
  double discovery_timeout_sec = 5.0;
  double duration_sec = 0.0;
  double min_frame_interval_sec = 0.08;
  uint32_t max_points_per_frame = kDefaultPreviewPointLimit;
  bool mock = false;
  bool set_normal_mode = true;
  bool enable_point_send = true;
  bool quiet_sdk_log = true;
  bool auto_bind_livox_subnet = true;
  std::string auto_bind_ip = "192.168.1.5";
};

using InterfaceInfo = mid360_net::DiscoveryCandidate;

struct TempConfig {
  std::string path;

  TempConfig() = default;
  TempConfig(const TempConfig&) = delete;
  TempConfig& operator=(const TempConfig&) = delete;

  TempConfig(TempConfig&& other) noexcept : path(std::move(other.path)) {
    other.path.clear();
  }

  TempConfig& operator=(TempConfig&& other) noexcept {
    if (this != &other) {
      reset();
      path = std::move(other.path);
      other.path.clear();
    }
    return *this;
  }

  ~TempConfig() {
    reset();
  }

  void reset() {
    if (!path.empty()) {
      std::remove(path.c_str());
      path.clear();
    }
  }
};

struct SdkSession {
  bool initialized = false;
  mid360_net::TemporaryIpv4Address temporary_host;

  ~SdkSession() {
    stop();
  }

  void stop() {
    if (initialized) {
      LivoxLidarSdkUninit();
      initialized = false;
    }
    temporary_host.reset();
  }
};

struct DeviceState {
  uint32_t handle = 0;
  uint8_t dev_type = 0;
  std::string sn;
  std::string lidar_ip;
};

struct FrameAccumulator {
  std::chrono::steady_clock::time_point started_at;
  uint32_t handle = 0;
  uint8_t data_type = 0;
  uint8_t frame = 0;
  uint16_t udp = 0;
  uint64_t source_point_count = 0;
  bool has_bounds = false;
  float min_x = 0.0f;
  float min_y = 0.0f;
  float min_z = 0.0f;
  float max_x = 0.0f;
  float max_y = 0.0f;
  float max_z = 0.0f;
  std::vector<float> points;

  void reset() {
    started_at = {};
    handle = 0;
    data_type = 0;
    frame = 0;
    udp = 0;
    source_point_count = 0;
    has_bounds = false;
    points.clear();
  }
};

Options g_options;
std::mutex g_mutex;
std::condition_variable g_cv;
std::atomic<bool> g_running{true};
std::atomic<bool> g_seen_lidar{false};
std::atomic<bool> g_control_requested{false};
uint64_t g_seq = 0;
uint64_t g_packets = 0;
uint64_t g_points_sent = 0;
uint64_t g_source_points = 0;
uint64_t g_unsupported_packets = 0;
DeviceState g_device;
FrameAccumulator g_frame_accumulator;

void stop_signal(int) {
  g_running = false;
  g_cv.notify_all();
}

std::string fixed_number(double value, int precision) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(precision) << value;
  return out.str();
}

const char* data_type_name(uint8_t type) {
  switch (type) {
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

TempConfig write_discovery_config(const InterfaceInfo& iface) {
  char path_template[] = "/tmp/livox_mid360_preview_XXXXXX.json";
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

void request_controls(uint32_t handle) {
  if (!g_options.set_normal_mode && !g_options.enable_point_send) {
    return;
  }
  bool expected = false;
  if (!g_control_requested.compare_exchange_strong(expected, true)) {
    return;
  }
  if (g_options.set_normal_mode) {
    SetLivoxLidarWorkMode(handle, kLivoxLidarNormal, nullptr, nullptr);
  }
  if (g_options.enable_point_send) {
    EnableLivoxLidarPointSend(handle, nullptr, nullptr);
  }
}

void lidar_info_change_callback(const uint32_t handle, const LivoxLidarInfo* info, void*) {
  if (!info) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_device.handle = handle;
    g_device.dev_type = info->dev_type;
    g_device.sn = info->sn;
    g_device.lidar_ip = info->lidar_ip;
  }
  g_seen_lidar = true;
  request_controls(handle);
  g_cv.notify_all();
}

void push_msg_callback(const uint32_t handle, const uint8_t dev_type, const char* info, void*) {
  (void)dev_type;
  (void)info;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_device.handle = handle;
  }
  g_seen_lidar = true;
  request_controls(handle);
  g_cv.notify_all();
}

void update_bounds(
    float x,
    float y,
    float z,
    bool& has_bounds,
    float& min_x,
    float& min_y,
    float& min_z,
    float& max_x,
    float& max_y,
    float& max_z) {
  if (!has_bounds) {
    min_x = max_x = x;
    min_y = max_y = y;
    min_z = max_z = z;
    has_bounds = true;
    return;
  }
  min_x = std::min(min_x, x);
  min_y = std::min(min_y, y);
  min_z = std::min(min_z, z);
  max_x = std::max(max_x, x);
  max_y = std::max(max_y, y);
  max_z = std::max(max_z, z);
}

void append_point(
    std::vector<float>& points,
    float x,
    float y,
    float z,
    float intensity,
    bool& has_bounds,
    float& min_x,
    float& min_y,
    float& min_z,
    float& max_x,
    float& max_y,
    float& max_z) {
  points.push_back(x);
  points.push_back(y);
  points.push_back(z);
  points.push_back(intensity);
  update_bounds(x, y, z, has_bounds, min_x, min_y, min_z, max_x, max_y, max_z);
}

template <typename Writer>
void decode_packet(
    LivoxLidarEthernetPacket* packet,
    Writer writer,
    std::vector<float>& points,
    bool& has_bounds,
    float& min_x,
    float& min_y,
    float& min_z,
    float& max_x,
    float& max_y,
    float& max_z) {
  if (!packet || packet->dot_num == 0) {
    return;
  }
  points.reserve(points.size() + static_cast<size_t>(packet->dot_num) * 4);
  for (uint32_t i = 0; i < packet->dot_num; ++i) {
    writer(points, i, has_bounds, min_x, min_y, min_z, max_x, max_y, max_z);
  }
}

std::vector<float> sample_frame_points(const std::vector<float>& source) {
  const size_t source_point_count = source.size() / 4;
  if (source_point_count == 0) {
    return {};
  }
  const size_t point_limit = std::max<size_t>(1, g_options.max_points_per_frame);
  const size_t sample_every = std::max<size_t>(1, (source_point_count + point_limit - 1) / point_limit);
  std::vector<float> sampled;
  sampled.reserve(((source_point_count + sample_every - 1) / sample_every) * 4);
  for (size_t point_index = 0; point_index < source_point_count; point_index += sample_every) {
    const size_t offset = point_index * 4;
    sampled.push_back(source[offset]);
    sampled.push_back(source[offset + 1]);
    sampled.push_back(source[offset + 2]);
    sampled.push_back(source[offset + 3]);
  }
  return sampled;
}

void write_u32_le(std::ostream& out, uint32_t value) {
  out.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

void emit_frame(const FrameAccumulator& frame, const std::vector<float>& points) {
  if (points.empty()) {
    return;
  }
  const uint64_t seq = ++g_seq;
  g_points_sent += points.size() / 4;
  std::ostringstream header;
  header << "{\"type\":\"cloud\""
         << ",\"source\":\"raw\""
         << ",\"seq\":" << seq
         << ",\"handle\":" << frame.handle
         << ",\"point_count\":" << (points.size() / 4)
         << ",\"source_point_count\":" << frame.source_point_count
         << ",\"sample_every\":"
         << std::max<size_t>(1, (frame.source_point_count + (points.size() / 4) - 1) / (points.size() / 4))
         << ",\"stride\":4"
         << ",\"layout\":\"float32_xyzi\""
         << ",\"has_intensity\":true"
         << ",\"intensity_range\":[0,255]"
         << ",\"data_type\":\"" << data_type_name(frame.data_type) << "\""
         << ",\"frame\":" << static_cast<unsigned>(frame.frame)
         << ",\"udp\":" << frame.udp;
  if (frame.has_bounds) {
    header << ",\"bounds\":{\"min\":["
           << frame.min_x << "," << frame.min_y << "," << frame.min_z
           << "],\"max\":["
           << frame.max_x << "," << frame.max_y << "," << frame.max_z
           << "]}";
  }
  header << "}";

  const std::string header_json = header.str();
  const uint32_t header_len = static_cast<uint32_t>(header_json.size());
  const uint32_t data_offset = 8u + header_len;

  std::cout.write(reinterpret_cast<const char*>(&header_len), sizeof(header_len));
  std::cout.write(reinterpret_cast<const char*>(&data_offset), sizeof(data_offset));
  std::cout.write(header_json.data(), static_cast<std::streamsize>(header_json.size()));
  std::cout.write(reinterpret_cast<const char*>(points.data()), static_cast<std::streamsize>(points.size() * sizeof(float)));
  std::cout.flush();
}

void run_mock_stream() {
  std::cerr << "preview: mock point cloud stream\n";
  const auto started = std::chrono::steady_clock::now();
  const auto interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
      std::chrono::duration<double>(g_options.min_frame_interval_sec));
  const uint32_t point_count = g_options.max_points_per_frame == 0
      ? kMockPreviewPointCount
      : std::min<uint32_t>(g_options.max_points_per_frame, kMockPreviewPointCount);
  uint16_t udp = 0;
  uint8_t frame = 0;
  while (g_running.load()) {
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    if (g_options.duration_sec > 0.0 && elapsed >= g_options.duration_sec) {
      break;
    }

    std::vector<float> points;
    points.reserve(static_cast<size_t>(point_count) * 4);
    bool has_bounds = false;
    float min_x = 0.0f;
    float min_y = 0.0f;
    float min_z = 0.0f;
    float max_x = 0.0f;
    float max_y = 0.0f;
    float max_z = 0.0f;
    const float phase = static_cast<float>(elapsed * 1.6);
    for (uint32_t i = 0; i < point_count; ++i) {
      const float t = static_cast<float>(i) / static_cast<float>(std::max<uint32_t>(1, point_count - 1));
      const float angle = t * 18.8495559f + phase;
      const float radius = 1.5f + 5.5f * t;
      const float x = std::cos(angle) * radius;
      const float y = std::sin(angle) * radius;
      const float z = std::sin(t * 25.132741f + phase) * 0.7f + t * 1.8f;
      const float intensity = std::fmod(t * 255.0f + elapsed * 40.0f, 255.0f);
      append_point(points, x, y, z, intensity, has_bounds, min_x, min_y, min_z, max_x, max_y, max_z);
    }
    FrameAccumulator frame_info;
    frame_info.handle = 0;
    frame_info.data_type = kLivoxLidarCartesianCoordinateHighData;
    frame_info.frame = frame++;
    frame_info.udp = udp++;
    frame_info.source_point_count = point_count;
    frame_info.has_bounds = has_bounds;
    frame_info.min_x = min_x;
    frame_info.min_y = min_y;
    frame_info.min_z = min_z;
    frame_info.max_x = max_x;
    frame_info.max_y = max_y;
    frame_info.max_z = max_z;
    emit_frame(frame_info, points);
    std::this_thread::sleep_for(interval);
  }
  std::cerr << "preview: mock stopped emitted_points=" << g_points_sent << "\n";
}

void point_cloud_callback(const uint32_t handle, const uint8_t dev_type, LivoxLidarEthernetPacket* packet, void*) {
  (void)dev_type;
  if (!packet || !g_running.load()) {
    return;
  }

  std::lock_guard<std::mutex> lock(g_mutex);
  g_packets += 1;
  g_source_points += packet->dot_num;
  g_device.handle = handle;
  g_seen_lidar = true;
  request_controls(handle);

  const auto now = std::chrono::steady_clock::now();

  switch (packet->data_type) {
    case kLivoxLidarCartesianCoordinateHighData: {
      auto* raw = reinterpret_cast<LivoxLidarCartesianHighRawPoint*>(packet->data);
      decode_packet(packet, [&](std::vector<float>& out, uint32_t i, bool& bounds,
                                float& mnx, float& mny, float& mnz, float& mxx, float& mxy, float& mxz) {
        append_point(out,
                     static_cast<float>(raw[i].x) / 1000.0f,
                     static_cast<float>(raw[i].y) / 1000.0f,
                     static_cast<float>(raw[i].z) / 1000.0f,
                     static_cast<float>(raw[i].reflectivity),
                     bounds, mnx, mny, mnz, mxx, mxy, mxz);
      }, g_frame_accumulator.points, g_frame_accumulator.has_bounds,
         g_frame_accumulator.min_x, g_frame_accumulator.min_y, g_frame_accumulator.min_z,
         g_frame_accumulator.max_x, g_frame_accumulator.max_y, g_frame_accumulator.max_z);
      break;
    }
    case kLivoxLidarCartesianCoordinateLowData: {
      auto* raw = reinterpret_cast<LivoxLidarCartesianLowRawPoint*>(packet->data);
      decode_packet(packet, [&](std::vector<float>& out, uint32_t i, bool& bounds,
                                float& mnx, float& mny, float& mnz, float& mxx, float& mxy, float& mxz) {
        append_point(out,
                     static_cast<float>(raw[i].x) / 100.0f,
                     static_cast<float>(raw[i].y) / 100.0f,
                     static_cast<float>(raw[i].z) / 100.0f,
                     static_cast<float>(raw[i].reflectivity),
                     bounds, mnx, mny, mnz, mxx, mxy, mxz);
      }, g_frame_accumulator.points, g_frame_accumulator.has_bounds,
         g_frame_accumulator.min_x, g_frame_accumulator.min_y, g_frame_accumulator.min_z,
         g_frame_accumulator.max_x, g_frame_accumulator.max_y, g_frame_accumulator.max_z);
      break;
    }
    case kLivoxLidarSphericalCoordinateData: {
      auto* raw = reinterpret_cast<LivoxLidarSpherPoint*>(packet->data);
      decode_packet(packet, [&](std::vector<float>& out, uint32_t i, bool& bounds,
                                float& mnx, float& mny, float& mnz, float& mxx, float& mxy, float& mxz) {
        const float radius = static_cast<float>(raw[i].depth) / 1000.0f;
        const float theta = static_cast<float>(raw[i].theta) / 100.0f / 180.0f * static_cast<float>(kPi);
        const float phi = static_cast<float>(raw[i].phi) / 100.0f / 180.0f * static_cast<float>(kPi);
        append_point(out,
                     radius * std::sin(theta) * std::cos(phi),
                     radius * std::sin(theta) * std::sin(phi),
                     radius * std::cos(theta),
                     static_cast<float>(raw[i].reflectivity),
                     bounds, mnx, mny, mnz, mxx, mxy, mxz);
      }, g_frame_accumulator.points, g_frame_accumulator.has_bounds,
         g_frame_accumulator.min_x, g_frame_accumulator.min_y, g_frame_accumulator.min_z,
         g_frame_accumulator.max_x, g_frame_accumulator.max_y, g_frame_accumulator.max_z);
      break;
    }
    default:
      g_unsupported_packets += 1;
      g_cv.notify_all();
      return;
  }

  if (g_frame_accumulator.started_at.time_since_epoch().count() == 0) {
    g_frame_accumulator.started_at = now;
  }
  g_frame_accumulator.handle = handle;
  g_frame_accumulator.data_type = packet->data_type;
  g_frame_accumulator.frame = packet->frame_cnt;
  g_frame_accumulator.udp = packet->udp_cnt;
  g_frame_accumulator.source_point_count += packet->dot_num;

  const double frame_age = std::chrono::duration<double>(now - g_frame_accumulator.started_at).count();
  if (frame_age >= g_options.min_frame_interval_sec) {
    if (g_options.max_points_per_frame == 0) {
      emit_frame(g_frame_accumulator, g_frame_accumulator.points);
    } else {
      const std::vector<float> points = sample_frame_points(g_frame_accumulator.points);
      emit_frame(g_frame_accumulator, points);
    }
    g_frame_accumulator.reset();
  }
  g_cv.notify_all();
}

void register_callbacks() {
  SetLivoxLidarPointCloudCallBack(point_cloud_callback, nullptr);
  SetLivoxLidarInfoCallback(push_msg_callback, nullptr);
  SetLivoxLidarInfoChangeCallback(lidar_info_change_callback, nullptr);
}

bool wait_for_lidar(double timeout_sec) {
  const auto deadline = std::chrono::steady_clock::now() +
      std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(timeout_sec));
  std::unique_lock<std::mutex> lock(g_mutex);
  while (g_running.load() && !g_seen_lidar.load()) {
    if (g_cv.wait_until(lock, deadline) == std::cv_status::timeout) {
      break;
    }
  }
  return g_seen_lidar.load();
}

bool init_sdk_with_config(const Options& options, SdkSession& sdk) {
  if (!LivoxLidarSdkInit(options.config_path.c_str())) {
    return false;
  }
  sdk.initialized = true;
  register_callbacks();
  std::cerr << "preview: config=" << options.config_path << "\n";
  return true;
}

bool init_sdk_by_discovery(const Options& options, InterfaceInfo& active_iface, SdkSession& sdk) {
  const std::vector<InterfaceInfo> candidates = mid360_net::sdk_discovery_candidates(
      options.iface,
      options.host_ip,
      options.auto_bind_livox_subnet,
      options.auto_bind_ip);
  if (candidates.empty()) {
    throw std::runtime_error(
        "no usable IPv4 interfaces found; pass --host-ip, --iface, or add 192.168.1.5/24 to the lidar NIC");
  }
  std::cerr << "preview: discovery candidates";
  for (const auto& candidate : candidates) {
    std::cerr << " " << mid360_net::describe_discovery_candidate(candidate);
  }
  std::cerr << "\n";

  for (const auto& candidate : candidates) {
    if (!g_running.load()) {
      return false;
    }
    g_seen_lidar = false;
    g_control_requested = false;
    std::cerr << "preview: listening on " << mid360_net::describe_discovery_candidate(candidate)
              << " for " << fixed_number(options.discovery_timeout_sec, 1) << "s\n";
    mid360_net::TemporaryIpv4Address temporary_host;
    if (candidate.auto_bind_livox_subnet) {
      std::string error;
      if (!temporary_host.add(candidate.name, candidate.ip, candidate.prefix, error)) {
        std::cerr << "preview: WARN " << error << "\n";
        continue;
      }
      std::cerr << "preview: temporarily added " << temporary_host.cidr() << " to " << candidate.name << "\n";
    }

    TempConfig temp_config = write_discovery_config(candidate);
    if (!LivoxLidarSdkInit(temp_config.path.c_str())) {
      LivoxLidarSdkUninit();
      std::cerr << "preview: WARN LivoxLidarSdkInit failed on " << mid360_net::describe_discovery_candidate(candidate) << "\n";
      continue;
    }
    sdk.initialized = true;
    register_callbacks();
    if (wait_for_lidar(options.discovery_timeout_sec)) {
      active_iface = candidate;
      sdk.temporary_host = std::move(temporary_host);
      std::lock_guard<std::mutex> lock(g_mutex);
      std::cerr << "preview: found lidar ip="
                << (g_device.lidar_ip.empty() ? std::string("N/A") : g_device.lidar_ip)
                << " sn=" << (g_device.sn.empty() ? std::string("N/A") : g_device.sn)
                << " via " << mid360_net::describe_discovery_candidate(candidate) << "\n";
      return true;
    }
    sdk.stop();
  }
  return false;
}

void usage(const char* argv0) {
  std::cerr
      << "usage: " << argv0 << " [options]\n"
      << "\n"
      << "Stream Livox-SDK2 point cloud frames to stdout for the GUI preview.\n"
      << "stdout is binary; status messages are written to stderr.\n"
      << "\n"
      << "options:\n"
      << "  --config PATH               optional MID360_config.json path; disables discovery mode\n"
      << "  -i, --iface IFACE           interface to use for SDK discovery, or auto (default: auto)\n"
      << "  --host-ip IP                host IPv4 address to bind for SDK discovery\n"
      << "  --auto-bind-ip IP           temporary host IP for 192.168.1.x lidar NICs (default: 192.168.1.5)\n"
      << "  --no-auto-bind              do not temporarily add 192.168.1.x/24 to candidate NICs\n"
      << "  -t, --timeout SEC           per-interface discovery timeout (default: 5)\n"
      << "  --duration SEC              stop after N seconds; 0 means forever (default: 0)\n"
      << "  --interval SEC              minimum frame interval seconds (default: 0.08)\n"
      << "  --max-points-per-frame N    sampled points per emitted frame; 0 emits all points (default: 0)\n"
      << "  --mock                      emit synthetic frames for parser/UI testing\n"
      << "  --no-set-normal-mode        do not request normal work mode after lidar appears\n"
      << "  --no-enable-point-send      do not request point sending after lidar appears\n"
      << "  --sdk-log                   keep SDK console logger enabled\n"
      << "  -h, --help                  show this help\n";
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
      options.min_frame_interval_sec = std::stod(need_value(arg));
    } else if (arg == "--max-points-per-frame") {
      options.max_points_per_frame = static_cast<uint32_t>(std::stoul(need_value(arg)));
    } else if (arg == "--mock") {
      options.mock = true;
    } else if (arg == "--no-set-normal-mode") {
      options.set_normal_mode = false;
    } else if (arg == "--no-enable-point-send") {
      options.enable_point_send = false;
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
  if (options.min_frame_interval_sec <= 0.0) {
    throw std::runtime_error("--interval must be positive");
  }
  return options;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    g_options = parse_args(argc, argv);
    std::signal(SIGINT, stop_signal);
    std::signal(SIGTERM, stop_signal);

    if (g_options.quiet_sdk_log) {
      DisableLivoxSdkConsoleLogger();
    }

    if (g_options.mock) {
      run_mock_stream();
      return 0;
    }

    SdkSession sdk;
    InterfaceInfo active_iface;
    if (!g_options.config_path.empty()) {
      if (!init_sdk_with_config(g_options, sdk)) {
        throw std::runtime_error("LivoxLidarSdkInit failed for " + g_options.config_path);
      }
    } else if (!init_sdk_by_discovery(g_options, active_iface, sdk)) {
      if (!g_running.load()) {
        return 130;
      }
      throw std::runtime_error("no MID360 lidar found by SDK discovery");
    }

    std::cerr << "preview: streaming point cloud frames\n";
    const auto started = std::chrono::steady_clock::now();
    while (g_running.load()) {
      std::unique_lock<std::mutex> lock(g_mutex);
      g_cv.wait_for(lock, std::chrono::milliseconds(250));
      const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
      if (g_options.duration_sec > 0.0 && elapsed >= g_options.duration_sec) {
        break;
      }
    }

    sdk.stop();
    std::cerr << "preview: stopped packets=" << g_packets
              << " source_points=" << g_source_points
              << " emitted_points=" << g_points_sent
              << " unsupported_packets=" << g_unsupported_packets << "\n";
    return 0;
  } catch (const std::exception& exc) {
    std::cerr << "ERROR: " << exc.what() << "\n";
    return 2;
  }
}
