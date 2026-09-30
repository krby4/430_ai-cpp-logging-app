#pragma once

#include "cpp_log/monitor.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace cpp_log {

struct ProcessUsage {
  int pid{};
  std::uint64_t start_ticks{};
  std::string command;
  double value{};
  std::uint64_t rss_bytes{};
};

struct Sample {
  std::int64_t captured_at{};
  std::optional<double> cpu_percent;
  double memory_percent{};
  double root_disk_percent{};
  std::optional<std::uint64_t> ingress_bytes;
  std::optional<ProcessUsage> top_cpu;
  std::optional<ProcessUsage> top_memory;
  bool partial_process_scan{};
};

struct HostBaseline {
  std::string boot_id;
  CpuCounters cpu;
  std::vector<InterfaceCounter> interfaces;
};

struct ProcessBaseline {
  int pid{};
  std::uint64_t start_ticks{};
  std::uint64_t total_ticks{};
};

class Database {
 public:
  explicit Database(const std::string& path);
  ~Database();
  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;

  void initialize();
  void begin_write();
  void commit();
  void rollback() noexcept;
  [[nodiscard]] bool in_write_transaction() const;

  [[nodiscard]] std::optional<HostBaseline> load_host_baseline() const;
  [[nodiscard]] std::vector<ProcessBaseline> load_process_baselines() const;
  void persist(const Sample& sample, const HostBaseline& baseline,
               const std::vector<ProcessBaseline>& processes);
  void prune_samples_before(std::int64_t cutoff);

  [[nodiscard]] int sample_count() const;
  [[nodiscard]] std::optional<Sample> latest_sample() const;

 private:
  sqlite3* connection_{};
  bool in_write_transaction_{};
};

}  // namespace cpp_log
