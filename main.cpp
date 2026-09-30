#include "cpp_log/collector.hpp"
#include "cpp_log/report.hpp"
#include "cpp_log/storage.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

constexpr const char* kLogDirectory = "/srv/services/cpp-log/logs";
constexpr const char* kDatabasePath = "/srv/services/cpp-log/logs/monitor.db";
constexpr const char* kReportDirectory = "/srv/services/cpp-log/logs/reports";
constexpr std::int64_t kRetentionSeconds = 183LL * 24 * 60 * 60;

std::int64_t utc_now() {
  return std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
}

void write_sample() {
  std::filesystem::create_directories(kLogDirectory);
  cpp_log::Database database(kDatabasePath);
  database.initialize();
  database.begin_write();
  try {
    const auto now = utc_now();
    const auto collected = cpp_log::collect_write_sample(
        database.load_host_baseline(), database.load_process_baselines(), now);
    database.persist(collected.sample, collected.baseline, collected.processes);
    database.prune_samples_before(now - kRetentionSeconds);
    database.commit();
    std::cout << "Stored monitor sample at " << now << '\n';
  } catch (...) {
    database.rollback();
    throw;
  }
}

cpp_log::ReportPeriod parse_period(const std::string& value) {
  if (value == "daily") {
    return cpp_log::ReportPeriod::daily;
  }
  if (value == "weekly") {
    return cpp_log::ReportPeriod::weekly;
  }
  if (value == "monthly") {
    return cpp_log::ReportPeriod::monthly;
  }
  throw std::invalid_argument("report period must be daily, weekly, or monthly");
}

void write_report(cpp_log::ReportPeriod period) {
  std::filesystem::create_directories(kReportDirectory);
  cpp_log::Database database(kDatabasePath);
  database.initialize();
  const auto window = cpp_log::report_window(period, utc_now());
  const auto report_path = std::filesystem::path(kReportDirectory) / window.filename;
  std::ofstream output(report_path, std::ios::trunc);
  if (!output) {
    throw std::runtime_error("cannot write report: " + report_path.string());
  }
  output << cpp_log::render_report(window, database.samples_between(window.start, window.end));
  if (!output) {
    throw std::runtime_error("cannot finish report: " + report_path.string());
  }
  std::cout << report_path << '\n';
}

void print_usage(const char* program) {
  std::cerr << "Usage: " << program << " write | report [daily|weekly|monthly]\n";
}

}  // namespace

int main(int argc, char* argv[]) {
  try {
    if (argc == 2 && std::string(argv[1]) == "write") {
      write_sample();
      return 0;
    }
    if ((argc == 2 || argc == 3) && std::string(argv[1]) == "report") {
      write_report(argc == 3 ? parse_period(argv[2]) : cpp_log::ReportPeriod::weekly);
      return 0;
    }
    print_usage(argv[0]);
    return 2;
  } catch (const std::exception& error) {
    std::cerr << "cpp-log: " << error.what() << '\n';
    return 1;
  }
}
