#include "cpp_log/collector.hpp"
#include "cpp_log/monitor.hpp"
#include "cpp_log/report.hpp"
#include "cpp_log/storage.hpp"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

void expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

void test_cpu_percent_uses_non_idle_delta() {
  const auto percent = cpp_log::cpu_percent({100, 40}, {200, 90});

  expect(percent.has_value(), "CPU delta should be available");
  expect(std::abs(*percent - 50.0) < 0.0001,
         "CPU percent should use non-idle ticks divided by total ticks");
}

void test_cpu_percent_rejects_counter_reset() {
  const auto percent = cpp_log::cpu_percent({200, 90}, {100, 40});

  expect(!percent.has_value(), "counter reset should produce a missing CPU value");
}

void test_network_delta_sums_matching_interfaces() {
  const std::vector<cpp_log::InterfaceCounter> previous = {
      {"eth0", 2, 100}, {"eth1", 3, 250}};
  const std::vector<cpp_log::InterfaceCounter> current = {
      {"eth0", 2, 160}, {"eth1", 3, 300}};

  const auto bytes = cpp_log::network_delta(previous, current);

  expect(bytes.has_value(), "unchanged interfaces should produce a delta");
  expect(*bytes == 110, "network delta should sum each interface delta");
}

void test_network_delta_rejects_interface_changes_and_resets() {
  const std::vector<cpp_log::InterfaceCounter> previous = {{"eth0", 2, 100}};

  expect(!cpp_log::network_delta(previous, {{"eth0", 2, 50}}).has_value(),
         "counter reset should produce missing ingress");
  expect(!cpp_log::network_delta(previous, {{"eth0", 2, 160}, {"eth1", 3, 1}})
              .has_value(),
         "interface-set change should produce missing ingress");
}

void test_database_commits_samples_and_prunes_expired_history() {
  const auto path = std::filesystem::temp_directory_path() / "cpp-log-test.db";
  std::filesystem::remove(path);
  cpp_log::Database database(path.string());
  database.initialize();
  database.begin_write();
  database.persist({100, 10.0, 20.0, 30.0, 40, std::nullopt, std::nullopt, false},
                   {"boot-a", {100, 40}, {{"eth0", 2, 50}}}, {});
  database.commit();

  expect(database.sample_count() == 1, "committed sample should be queryable");
  expect(database.latest_sample()->ingress_bytes == 40,
         "optional ingress should round-trip through SQLite");
  expect(database.samples_between(0, 200).size() == 1,
         "report query should return samples inside its UTC window");

  database.begin_write();
  database.prune_samples_before(101);
  database.commit();
  expect(database.sample_count() == 0,
         "retention cleanup should remove only samples older than cutoff");
  std::filesystem::remove(path);
}

void test_process_stat_parser_handles_spaces_in_command_name() {
  const auto parsed = cpp_log::parse_process_stat(
      7, "7 (worker pool) S 0 0 0 0 0 0 0 0 0 0 11 12 0 0 0 0 0 0 99");

  expect(parsed.has_value(), "valid proc stat data should parse");
  expect(parsed->command == "worker pool", "parser should preserve command spaces");
  expect(parsed->total_ticks == 23, "parser should add user and system ticks");
  expect(parsed->start_ticks == 99, "parser should read field 22 after the command");
}

void test_sparkline_keeps_missing_data_visible() {
  const std::string graph = cpp_log::sparkline({1.0, 2.0, std::nullopt, 4.0});

  expect(graph == "▁▃·█", "sparkline should scale values and render missing buckets as gaps");
}

void test_markdown_escape_neutralizes_table_delimiters_and_newlines() {
  expect(cpp_log::markdown_escape("worker|name\n") == "worker\\|name ",
         "process names must not inject Markdown table cells or lines");
}

}  // namespace

int main() {
  test_cpu_percent_uses_non_idle_delta();
  test_cpu_percent_rejects_counter_reset();
  test_network_delta_sums_matching_interfaces();
  test_network_delta_rejects_interface_changes_and_resets();
  test_database_commits_samples_and_prunes_expired_history();
  test_process_stat_parser_handles_spaces_in_command_name();
  test_sparkline_keeps_missing_data_visible();
  test_markdown_escape_neutralizes_table_delimiters_and_newlines();
  std::cout << "monitor tests passed\n";
}
