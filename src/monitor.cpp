#include "cpp_log/monitor.hpp"

#include <algorithm>
#include <map>

namespace cpp_log {

std::optional<double> cpu_percent(const CpuCounters& previous,
                                  const CpuCounters& current) {
  if (current.total_ticks <= previous.total_ticks ||
      current.idle_ticks < previous.idle_ticks) {
    return std::nullopt;
  }

  const auto total_delta = current.total_ticks - previous.total_ticks;
  const auto idle_delta = current.idle_ticks - previous.idle_ticks;
  if (idle_delta > total_delta) {
    return std::nullopt;
  }

  const auto busy_delta = total_delta - idle_delta;
  return 100.0 * static_cast<double>(busy_delta) /
         static_cast<double>(total_delta);
}

std::optional<std::uint64_t> network_delta(
    const std::vector<InterfaceCounter>& previous,
    const std::vector<InterfaceCounter>& current) {
  if (previous.size() != current.size()) {
    return std::nullopt;
  }

  std::map<std::pair<std::string, int>, std::uint64_t> previous_by_interface;
  for (const auto& interface : previous) {
    previous_by_interface[{interface.name, interface.index}] =
        interface.received_bytes;
  }

  std::uint64_t total_delta = 0;
  for (const auto& interface : current) {
    const auto key = std::pair{interface.name, interface.index};
    const auto previous_it = previous_by_interface.find(key);
    if (previous_it == previous_by_interface.end() ||
        interface.received_bytes < previous_it->second) {
      return std::nullopt;
    }
    total_delta += interface.received_bytes - previous_it->second;
  }

  return total_delta;
}

}  // namespace cpp_log
