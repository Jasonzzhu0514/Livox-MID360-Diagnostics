#pragma once

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace mid360_net {

struct Ipv4Info {
  std::string name;
  std::string ip;
  int prefix = 0;
};

struct LinkInfo {
  std::string name;
  bool lower_up = false;
};

inline bool ethernet_like_name(const std::string& name) {
  return name.rfind("eth", 0) == 0 || name.rfind("en", 0) == 0;
}

inline bool skip_iface(const std::string& name, unsigned int flags) {
  if ((flags & IFF_UP) == 0 || (flags & IFF_LOOPBACK) != 0) {
    return true;
  }
  return name == "lo" ||
         name.rfind("docker", 0) == 0 ||
         name.rfind("br-", 0) == 0 ||
         name.rfind("veth", 0) == 0 ||
         name.rfind("virbr", 0) == 0;
}

inline bool valid_ipv4(const std::string& ip) {
  in_addr addr {};
  return inet_pton(AF_INET, ip.c_str(), &addr) == 1;
}

inline bool livox_subnet_ip(const std::string& ip) {
  return ip.rfind("192.168.1.", 0) == 0;
}

inline std::vector<Ipv4Info> list_ipv4_interfaces() {
  std::vector<Ipv4Info> interfaces;
  ifaddrs* ifaddr = nullptr;
  if (getifaddrs(&ifaddr) != 0) {
    return interfaces;
  }
  for (ifaddrs* item = ifaddr; item != nullptr; item = item->ifa_next) {
    if (!item->ifa_addr || item->ifa_addr->sa_family != AF_INET) {
      continue;
    }
    const std::string name = item->ifa_name ? item->ifa_name : "";
    if (name.empty() || skip_iface(name, item->ifa_flags)) {
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

inline std::vector<LinkInfo> list_link_interfaces() {
  std::map<std::string, LinkInfo> links;
  ifaddrs* ifaddr = nullptr;
  if (getifaddrs(&ifaddr) != 0) {
    return {};
  }
  for (ifaddrs* item = ifaddr; item != nullptr; item = item->ifa_next) {
    const std::string name = item->ifa_name ? item->ifa_name : "";
    if (name.empty() || skip_iface(name, item->ifa_flags)) {
      continue;
    }
    auto& link = links[name];
    link.name = name;
    bool lower_up = (item->ifa_flags & IFF_RUNNING) != 0;
#ifdef IFF_LOWER_UP
    lower_up = lower_up || (item->ifa_flags & IFF_LOWER_UP) != 0;
#endif
    link.lower_up = link.lower_up || lower_up;
  }
  freeifaddrs(ifaddr);

  std::vector<LinkInfo> out;
  for (const auto& item : links) {
    out.push_back(item.second);
  }
  return out;
}

inline bool iface_has_ip(const std::string& iface, const std::string& ip) {
  for (const auto& item : list_ipv4_interfaces()) {
    if (item.name == iface && item.ip == ip) {
      return true;
    }
  }
  return false;
}

inline bool iface_has_livox_subnet_ip(const std::string& iface) {
  for (const auto& item : list_ipv4_interfaces()) {
    if (item.name == iface && livox_subnet_ip(item.ip)) {
      return true;
    }
  }
  return false;
}

inline bool iface_has_any_ipv4(const std::string& iface) {
  for (const auto& item : list_ipv4_interfaces()) {
    if (item.name == iface) {
      return true;
    }
  }
  return false;
}

inline bool ip_assigned_anywhere(const std::string& ip) {
  for (const auto& item : list_ipv4_interfaces()) {
    if (item.ip == ip) {
      return true;
    }
  }
  return false;
}

inline bool any_livox_subnet_ip(const std::vector<Ipv4Info>& interfaces) {
  return std::any_of(interfaces.begin(), interfaces.end(), [](const Ipv4Info& item) {
    return livox_subnet_ip(item.ip);
  });
}

inline int link_priority(const LinkInfo& item) {
  int value = 0;
  if (!iface_has_any_ipv4(item.name)) {
    value += 40;
  }
  if (item.lower_up) {
    value += 30;
  }
  if (ethernet_like_name(item.name)) {
    value += 20;
  }
  return value;
}

inline std::vector<LinkInfo> sorted_candidate_links() {
  std::vector<LinkInfo> links = list_link_interfaces();
  std::stable_sort(links.begin(), links.end(), [](const LinkInfo& a, const LinkInfo& b) {
    const int as = link_priority(a);
    const int bs = link_priority(b);
    if (as != bs) {
      return as > bs;
    }
    return a.name < b.name;
  });
  return links;
}

inline std::string choose_livox_host_ip(const std::string& preferred = "192.168.1.5") {
  if (livox_subnet_ip(preferred) && !ip_assigned_anywhere(preferred)) {
    return preferred;
  }
  const std::vector<int> preferred_hosts = {5, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120, 130, 140, 150, 160, 170, 180, 190, 200, 210, 220, 230, 240, 250};
  for (int host : preferred_hosts) {
    const std::string candidate = "192.168.1." + std::to_string(host);
    if (!ip_assigned_anywhere(candidate)) {
      return candidate;
    }
  }
  for (int host = 2; host <= 254; ++host) {
    const std::string candidate = "192.168.1." + std::to_string(host);
    if (!ip_assigned_anywhere(candidate)) {
      return candidate;
    }
  }
  return preferred;
}

inline int run_command(const std::vector<std::string>& args) {
  if (args.empty()) {
    return 127;
  }
  const pid_t pid = fork();
  if (pid < 0) {
    return 127;
  }
  if (pid == 0) {
    const int null_fd = open("/dev/null", O_WRONLY);
    if (null_fd >= 0) {
      dup2(null_fd, STDOUT_FILENO);
      dup2(null_fd, STDERR_FILENO);
      close(null_fd);
    }
    std::vector<char*> raw_args;
    raw_args.reserve(args.size() + 1);
    for (const auto& arg : args) {
      raw_args.push_back(const_cast<char*>(arg.c_str()));
    }
    raw_args.push_back(nullptr);
    execvp(raw_args[0], raw_args.data());
    _exit(127);
  }
  int status = 0;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) {
      return 127;
    }
  }
  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  if (WIFSIGNALED(status)) {
    return 128 + WTERMSIG(status);
  }
  return 127;
}

class TemporaryIpv4Address {
 public:
  TemporaryIpv4Address() = default;
  TemporaryIpv4Address(const TemporaryIpv4Address&) = delete;
  TemporaryIpv4Address& operator=(const TemporaryIpv4Address&) = delete;

  TemporaryIpv4Address(TemporaryIpv4Address&& other) noexcept {
    move_from(std::move(other));
  }

  TemporaryIpv4Address& operator=(TemporaryIpv4Address&& other) noexcept {
    if (this != &other) {
      reset();
      move_from(std::move(other));
    }
    return *this;
  }

  ~TemporaryIpv4Address() {
    reset();
  }

  bool add(const std::string& iface, const std::string& ip, int prefix, std::string& error) {
    reset();
    if (iface.empty() || !valid_ipv4(ip) || prefix <= 0 || prefix > 32) {
      error = "invalid temporary IPv4 request";
      return false;
    }
    iface_ = iface;
    ip_ = ip;
    prefix_ = prefix;

    if (iface_has_ip(iface_, ip_)) {
      error.clear();
      return true;
    }

    const std::string cidr = ip_ + "/" + std::to_string(prefix_);
    int status = run_command({"ip", "addr", "add", cidr, "dev", iface_});
    if (status == 0) {
      active_ = true;
      use_sudo_ = false;
      error.clear();
      return true;
    }

    if (!iface_has_ip(iface_, ip_) && geteuid() != 0) {
      status = run_command({"sudo", "-n", "ip", "addr", "add", cidr, "dev", iface_});
      if (status == 0) {
        active_ = true;
        use_sudo_ = true;
        error.clear();
        return true;
      }
    }

    if (iface_has_ip(iface_, ip_)) {
      error.clear();
      return true;
    }

    error = "failed to add " + cidr + " to " + iface_ +
        "; run: sudo ip addr add " + cidr + " dev " + iface_;
    iface_.clear();
    ip_.clear();
    prefix_ = 0;
    return false;
  }

  void reset() {
    if (!active_) {
      return;
    }
    const std::string cidr = ip_ + "/" + std::to_string(prefix_);
    if (use_sudo_) {
      run_command({"sudo", "-n", "ip", "addr", "del", cidr, "dev", iface_});
    } else {
      run_command({"ip", "addr", "del", cidr, "dev", iface_});
    }
    active_ = false;
    use_sudo_ = false;
    iface_.clear();
    ip_.clear();
    prefix_ = 0;
  }

  bool active() const {
    return active_;
  }

  std::string cidr() const {
    return ip_.empty() ? std::string() : ip_ + "/" + std::to_string(prefix_);
  }

 private:
  void move_from(TemporaryIpv4Address&& other) {
    iface_ = std::move(other.iface_);
    ip_ = std::move(other.ip_);
    prefix_ = other.prefix_;
    active_ = other.active_;
    use_sudo_ = other.use_sudo_;
    other.prefix_ = 0;
    other.active_ = false;
    other.use_sudo_ = false;
  }

  std::string iface_;
  std::string ip_;
  int prefix_ = 0;
  bool active_ = false;
  bool use_sudo_ = false;
};

}  // namespace mid360_net
