#include "cpp_log/collector.hpp"

#include <sys/statvfs.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace cpp_log {
namespace {

constexpr unsigned long kInterfaceUp = 0x1;
constexpr unsigned long kInterfaceLoopback = 0x8;

std::string trim(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    return {};
  }
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

std::string read_file(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("cannot read " + path.string());
  }
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::vector<std::string> fields(std::string_view value) {
  std::istringstream input{std::string(value)};
  std::vector<std::string> result;
  std::string field;
  while (input >> field) {
    result.push_back(field);
  }
  return result;
}

std::uint64_t as_uint64(const std::string& value) {
  std::size_t parsed = 0;
  const auto result = std::stoull(value, &parsed, 10);
  if (parsed != value.size()) {
    throw std::runtime_error("invalid unsigned counter: " + value);
  }
  return result;
}

CpuCounters read_cpu_counters() {
  std::istringstream input(read_file("/proc/stat"));
  std::string line;
  if (!std::getline(input, line)) {
    throw std::runtime_error("/proc/stat is empty");
  }
  const auto values = fields(line);
  if (values.size() < 6 || values.front() != "cpu") {
    throw std::runtime_error("invalid aggregate CPU counters");
  }

  std::uint64_t total = 0;
  for (std::size_t index = 1; index < std::min<std::size_t>(values.size(), 9); ++index) {
    total += as_uint64(values[index]);
  }
  return {total, as_uint64(values[4]) + as_uint64(values[5])};
}

std::pair<std::uint64_t, std::uint64_t> read_memory_bytes() {
  std::istringstream input(read_file("/proc/meminfo"));
  std::string key;
  std::uint64_t value = 0;
  std::string unit;
  std::uint64_t total = 0;
  std::uint64_t available = 0;
  while (input >> key >> value >> unit) {
    if (key == "MemTotal:") {
      total = value * 1024;
    } else if (key == "MemAvailable:") {
      available = value * 1024;
    }
  }
  if (total == 0 || available > total) {
    throw std::runtime_error("invalid memory counters");
  }
  return {total, available};
}

double root_disk_percent() {
  struct statvfs filesystem_stats {};
  if (statvfs("/", &filesystem_stats) != 0 || filesystem_stats.f_blocks == 0) {
    throw std::runtime_error("cannot read root filesystem statistics");
  }
  const auto used = filesystem_stats.f_blocks - filesystem_stats.f_bavail;
  return 100.0 * static_cast<double>(used) /
         static_cast<double>(filesystem_stats.f_blocks);
}

std::vector<InterfaceCounter> read_interfaces() {
  std::istringstream input(read_file("/proc/net/dev"));
  std::string line;
  std::getline(input, line);
  std::getline(input, line);
  std::vector<InterfaceCounter> interfaces;
  while (std::getline(input, line)) {
    const auto separator = line.find(':');
    if (separator == std::string::npos) {
      continue;
    }
    const std::string name = trim(line.substr(0, separator));
    const auto counters = fields(line.substr(separator + 1));
    if (name.empty() || counters.empty()) {
      continue;
    }
    try {
      const auto flags = std::stoul(trim(read_file("/sys/class/net/" + name + "/flags")),
                                    nullptr, 0);
      if ((flags & kInterfaceUp) == 0 || (flags & kInterfaceLoopback) != 0) {
        continue;
      }
      const auto index = std::stoi(trim(read_file("/sys/class/net/" + name + "/ifindex")));
      interfaces.push_back({name, index, as_uint64(counters.front())});
    } catch (const std::exception&) {
      // Interfaces are volatile; ignore one that disappears while being read.
    }
  }
  std::sort(interfaces.begin(), interfaces.end(),
            [](const auto& left, const auto& right) {
              return std::tie(left.name, left.index) < std::tie(right.name, right.index);
            });
  return interfaces;
}

std::optional<std::uint64_t> read_rss_bytes(int pid) {
  std::ifstream input("/proc/" + std::to_string(pid) + "/status");
  if (!input) {
    return std::nullopt;
  }
  std::string key;
  std::uint64_t value = 0;
  std::string unit;
  while (input >> key >> value >> unit) {
    if (key == "VmRSS:") {
      return value * 1024;
    }
  }
  return std::nullopt;
}

bool numeric_name(const std::string& name) {
  return !name.empty() && std::all_of(name.begin(), name.end(), [](unsigned char character) {
    return std::isdigit(character) != 0;
  });
}

struct ProcessScan {
  std::vector<ProcessCounterSnapshot> counters;
  std::optional<ProcessUsage> top_memory;
  bool partial{};
};

ProcessScan scan_processes(std::uint64_t total_memory) {
  ProcessScan scan;
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator("/proc", error)) {
    const auto name = entry.path().filename().string();
    if (error || !numeric_name(name)) {
      continue;
    }
    try {
      const int pid = std::stoi(name);
      const auto parsed = parse_process_stat(pid, read_file(entry.path() / "stat"));
      if (!parsed.has_value()) {
        scan.partial = true;
        continue;
      }
      scan.counters.push_back(*parsed);
      const auto rss = read_rss_bytes(pid);
      if (!rss.has_value()) {
        scan.partial = true;
        continue;
      }
      const double memory_percent = 100.0 * static_cast<double>(*rss) /
                                    static_cast<double>(total_memory);
      ProcessUsage candidate{pid, parsed->start_ticks, parsed->command, memory_percent, *rss};
      if (!scan.top_memory.has_value() || candidate.rss_bytes > scan.top_memory->rss_bytes) {
        scan.top_memory = std::move(candidate);
      }
    } catch (const std::exception&) {
      scan.partial = true;
    }
  }
  if (error) {
    scan.partial = true;
  }
  return scan;
}

}  // namespace

std::optional<ProcessCounterSnapshot> parse_process_stat(int pid,
                                                          std::string_view contents) {
  const auto command_open = contents.find('(');
  const auto command_close = contents.rfind(')');
  if (command_open == std::string_view::npos || command_close == std::string_view::npos ||
      command_close <= command_open) {
    return std::nullopt;
  }
  try {
    const auto stat_fields = fields(contents.substr(command_close + 1));
    if (stat_fields.size() < 20) {
      return std::nullopt;
    }
    return ProcessCounterSnapshot{pid,
                                  as_uint64(stat_fields[19]),
                                  as_uint64(stat_fields[11]) + as_uint64(stat_fields[12]),
                                  std::string(contents.substr(command_open + 1,
                                                              command_close - command_open - 1))};
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

CollectedWrite collect_write_sample(const std::optional<HostBaseline>& previous_host,
                                    const std::vector<ProcessBaseline>& previous_processes,
                                    std::int64_t captured_at) {
  const std::string boot_id = trim(read_file("/proc/sys/kernel/random/boot_id"));
  const CpuCounters current_cpu = read_cpu_counters();
  const auto [memory_total, memory_available] = read_memory_bytes();
  const auto current_interfaces = read_interfaces();
  const bool same_boot = previous_host.has_value() && previous_host->boot_id == boot_id;
  const auto cpu = same_boot ? cpu_percent(previous_host->cpu, current_cpu) : std::nullopt;
  const auto ingress = same_boot
      ? network_delta(previous_host->interfaces, current_interfaces)
      : std::nullopt;

  ProcessScan process_scan = scan_processes(memory_total);
  std::map<std::pair<int, std::uint64_t>, std::uint64_t> previous_by_process;
  if (same_boot) {
    for (const auto& process : previous_processes) {
      previous_by_process[{process.pid, process.start_ticks}] = process.total_ticks;
    }
  }

  std::optional<ProcessUsage> top_cpu;
  if (cpu.has_value() && same_boot) {
    const auto host_delta = current_cpu.total_ticks - previous_host->cpu.total_ticks;
    for (const auto& process : process_scan.counters) {
      const auto previous = previous_by_process.find({process.pid, process.start_ticks});
      if (previous == previous_by_process.end() || process.total_ticks < previous->second) {
        continue;
      }
      const double process_percent = 100.0 *
          static_cast<double>(process.total_ticks - previous->second) /
          static_cast<double>(host_delta);
      ProcessUsage candidate{process.pid, process.start_ticks, process.command,
                             std::min(process_percent, 100.0), 0};
      if (!top_cpu.has_value() || candidate.value > top_cpu->value) {
        top_cpu = std::move(candidate);
      }
    }
  }

  std::vector<ProcessBaseline> current_processes;
  current_processes.reserve(process_scan.counters.size());
  for (const auto& process : process_scan.counters) {
    current_processes.push_back({process.pid, process.start_ticks, process.total_ticks});
  }

  Sample sample;
  sample.captured_at = captured_at;
  sample.cpu_percent = cpu;
  sample.memory_percent = 100.0 * static_cast<double>(memory_total - memory_available) /
                          static_cast<double>(memory_total);
  sample.root_disk_percent = root_disk_percent();
  sample.ingress_bytes = ingress;
  sample.top_cpu = top_cpu;
  sample.top_memory = process_scan.top_memory;
  sample.partial_process_scan = process_scan.partial;
  return {std::move(sample), {boot_id, current_cpu, current_interfaces}, std::move(current_processes)};
}

}  // namespace cpp_log
