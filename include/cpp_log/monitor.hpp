#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace cpp_log {

struct CpuCounters {
  std::uint64_t total_ticks{};
  std::uint64_t idle_ticks{};
};

struct InterfaceCounter {
  std::string name;
  int index{};
  std::uint64_t received_bytes{};
};

std::optional<double> cpu_percent(const CpuCounters& previous,
                                  const CpuCounters& current);

std::optional<std::uint64_t> network_delta(
    const std::vector<InterfaceCounter>& previous,
    const std::vector<InterfaceCounter>& current);

}  // namespace cpp_log
