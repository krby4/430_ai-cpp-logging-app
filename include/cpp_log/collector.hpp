#pragma once

#include "cpp_log/storage.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cpp_log {

struct ProcessCounterSnapshot {
  int pid{};
  std::uint64_t start_ticks{};
  std::uint64_t total_ticks{};
  std::string command;
};

struct CollectedWrite {
  Sample sample;
  HostBaseline baseline;
  std::vector<ProcessBaseline> processes;
};

std::optional<ProcessCounterSnapshot> parse_process_stat(int pid,
                                                          std::string_view contents);
CollectedWrite collect_write_sample(const std::optional<HostBaseline>& previous_host,
                                    const std::vector<ProcessBaseline>& previous_processes,
                                    std::int64_t captured_at);

}  // namespace cpp_log
