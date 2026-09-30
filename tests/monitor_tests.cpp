#include "cpp_log/monitor.hpp"

#include <cmath>
#include <cstdlib>
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

}  // namespace

int main() {
  test_cpu_percent_uses_non_idle_delta();
  test_cpu_percent_rejects_counter_reset();
  test_network_delta_sums_matching_interfaces();
  test_network_delta_rejects_interface_changes_and_resets();
  std::cout << "monitor tests passed\n";
}
