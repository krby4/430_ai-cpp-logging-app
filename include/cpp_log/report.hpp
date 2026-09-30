#pragma once

#include "cpp_log/storage.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace cpp_log {

enum class ReportPeriod { daily, weekly, monthly };

struct ReportWindow {
  std::int64_t start{};
  std::int64_t end{};
  std::string label;
  std::string filename;
  bool hourly_buckets{};
};

std::string markdown_escape(const std::string& value);
std::string sparkline(const std::vector<std::optional<double>>& values);
ReportWindow report_window(ReportPeriod period, std::int64_t now);
std::string render_report(const ReportWindow& window, const std::vector<Sample>& samples);

}  // namespace cpp_log
