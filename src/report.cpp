#include "cpp_log/report.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <utility>

namespace cpp_log {
namespace {

std::tm local_time(std::time_t value) {
  std::tm result {};
  localtime_r(&value, &result);
  return result;
}

std::string format_time(const std::tm& value, const char* format) {
  std::array<char, 64> buffer {};
  std::strftime(buffer.data(), buffer.size(), format, &value);
  return buffer.data();
}

std::int64_t floor_bucket(std::int64_t timestamp, bool hourly) {
  std::time_t value = timestamp;
  std::tm local = local_time(value);
  local.tm_sec = 0;
  local.tm_min = 0;
  if (!hourly) {
    local.tm_hour = 0;
  }
  return static_cast<std::int64_t>(std::mktime(&local));
}

std::vector<std::int64_t> bucket_starts(const ReportWindow& window) {
  std::vector<std::int64_t> buckets;
  std::time_t current = window.start;
  while (current < window.end) {
    buckets.push_back(static_cast<std::int64_t>(current));
    std::tm next = local_time(current);
    if (window.hourly_buckets) {
      ++next.tm_hour;
    } else {
      ++next.tm_mday;
      next.tm_hour = 0;
    }
    const std::time_t normalized = std::mktime(&next);
    if (normalized <= current) {
      current += window.hourly_buckets ? 3600 : 86400;
    } else {
      current = normalized;
    }
  }
  return buckets;
}

std::string number(double value, int precision = 1) {
  std::ostringstream output;
  output << std::fixed << std::setprecision(precision) << value;
  return output.str();
}

struct Aggregate {
  double minimum{};
  double average{};
  double maximum{};
  std::size_t count{};
};

Aggregate aggregate(const std::vector<std::optional<double>>& values) {
  Aggregate result;
  double total = 0;
  for (const auto& value : values) {
    if (!value.has_value()) {
      continue;
    }
    if (result.count == 0) {
      result.minimum = *value;
      result.maximum = *value;
    } else {
      result.minimum = std::min(result.minimum, *value);
      result.maximum = std::max(result.maximum, *value);
    }
    total += *value;
    ++result.count;
  }
  if (result.count != 0) {
    result.average = total / static_cast<double>(result.count);
  }
  return result;
}

std::string aggregate_cells(const Aggregate& value) {
  if (value.count == 0) {
    return "— | — | —";
  }
  return number(value.minimum) + " | " + number(value.average) + " | " +
         number(value.maximum);
}

template <typename Getter>
std::vector<std::optional<double>> bucket_average(
    const std::vector<std::int64_t>& buckets,
    const std::map<std::int64_t, std::vector<const Sample*>>& samples_by_bucket,
    Getter getter) {
  std::vector<std::optional<double>> result;
  for (const auto bucket : buckets) {
    const auto it = samples_by_bucket.find(bucket);
    double total = 0;
    std::size_t count = 0;
    if (it != samples_by_bucket.end()) {
      for (const Sample* sample : it->second) {
        const auto value = getter(*sample);
        if (value.has_value()) {
          total += *value;
          ++count;
        }
      }
    }
    result.push_back(count == 0 ? std::nullopt
                                : std::optional<double>(total / static_cast<double>(count)));
  }
  return result;
}

std::vector<std::optional<double>> bucket_ingress(
    const std::vector<std::int64_t>& buckets,
    const std::map<std::int64_t, std::vector<const Sample*>>& samples_by_bucket) {
  std::vector<std::optional<double>> result;
  for (const auto bucket : buckets) {
    const auto it = samples_by_bucket.find(bucket);
    double total = 0;
    bool found = false;
    if (it != samples_by_bucket.end()) {
      for (const Sample* sample : it->second) {
        if (sample->ingress_bytes.has_value()) {
          total += static_cast<double>(*sample->ingress_bytes);
          found = true;
        }
      }
    }
    result.push_back(found ? std::optional<double>(total) : std::nullopt);
  }
  return result;
}

}  // namespace

std::string markdown_escape(const std::string& value) {
  std::string result;
  for (const unsigned char character : value) {
    if (character == '|' || character == '\\') {
      result.push_back('\\');
      result.push_back(static_cast<char>(character));
    } else if (character == '\n' || character == '\r' || character == '\t') {
      result.push_back(' ');
    } else if (std::iscntrl(character) != 0) {
      result.push_back('?');
    } else {
      result.push_back(static_cast<char>(character));
    }
  }
  return result;
}

std::string sparkline(const std::vector<std::optional<double>>& values) {
  static constexpr std::array<const char*, 8> kBars = {"▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
  std::optional<double> minimum;
  std::optional<double> maximum;
  for (const auto& value : values) {
    if (!value.has_value()) {
      continue;
    }
    minimum = minimum.has_value() ? std::min(*minimum, *value) : value;
    maximum = maximum.has_value() ? std::max(*maximum, *value) : value;
  }

  std::string result;
  for (const auto& value : values) {
    if (!value.has_value()) {
      result += "·";
      continue;
    }
    const double range = *maximum - *minimum;
    const auto index = range == 0.0
        ? 4
        : static_cast<std::size_t>(std::lround(((*value - *minimum) / range) * 7.0));
    result += kBars.at(std::min<std::size_t>(index, 7));
  }
  return result;
}

ReportWindow report_window(ReportPeriod period, std::int64_t now) {
  std::time_t now_time = now;
  std::tm start = local_time(now_time);
  start.tm_sec = 0;
  start.tm_min = 0;
  start.tm_hour = 0;

  ReportWindow window;
  if (period == ReportPeriod::daily) {
    const auto name = format_time(start, "%F");
    window.label = "Daily report: " + name;
    window.filename = "daily-" + name + ".md";
    window.hourly_buckets = true;
  } else if (period == ReportPeriod::weekly) {
    start.tm_mday -= (start.tm_wday + 6) % 7;
    std::mktime(&start);
    const auto name = format_time(start, "%G-W%V");
    window.label = "Weekly report: " + name;
    window.filename = "weekly-" + name + ".md";
  } else {
    start.tm_mday = 1;
    std::mktime(&start);
    const auto name = format_time(start, "%Y-%m");
    window.label = "Monthly report: " + name;
    window.filename = "monthly-" + name + ".md";
  }
  window.start = static_cast<std::int64_t>(std::mktime(&start));
  window.end = now + 1;
  return window;
}

std::string render_report(const ReportWindow& window, const std::vector<Sample>& samples) {
  std::map<std::int64_t, std::vector<const Sample*>> samples_by_bucket;
  std::vector<std::optional<double>> cpu;
  std::vector<std::optional<double>> memory;
  std::vector<std::optional<double>> disk;
  std::vector<std::optional<double>> ingress;
  std::optional<ProcessUsage> peak_cpu;
  std::optional<ProcessUsage> peak_memory;

  for (const auto& sample : samples) {
    samples_by_bucket[floor_bucket(sample.captured_at, window.hourly_buckets)].push_back(&sample);
    cpu.push_back(sample.cpu_percent);
    memory.push_back(sample.memory_percent);
    disk.push_back(sample.root_disk_percent);
    ingress.push_back(sample.ingress_bytes.has_value()
                      ? std::optional<double>(static_cast<double>(*sample.ingress_bytes))
                      : std::nullopt);
    if (sample.top_cpu.has_value() &&
        (!peak_cpu.has_value() || sample.top_cpu->value > peak_cpu->value)) {
      peak_cpu = sample.top_cpu;
    }
    if (sample.top_memory.has_value() &&
        (!peak_memory.has_value() || sample.top_memory->rss_bytes > peak_memory->rss_bytes)) {
      peak_memory = sample.top_memory;
    }
  }

  const auto buckets = bucket_starts(window);
  const auto cpu_graph = bucket_average(buckets, samples_by_bucket,
      [](const Sample& sample) { return sample.cpu_percent; });
  const auto memory_graph = bucket_average(buckets, samples_by_bucket,
      [](const Sample& sample) { return std::optional<double>(sample.memory_percent); });
  const auto disk_graph = bucket_average(buckets, samples_by_bucket,
      [](const Sample& sample) { return std::optional<double>(sample.root_disk_percent); });
  const auto ingress_graph = bucket_ingress(buckets, samples_by_bucket);
  const auto ingress_total = aggregate(ingress);

  std::ostringstream output;
  output << "# " << window.label << "\n\n";
  output << "Samples: " << samples.size() << ". Missing CPU samples: "
         << (samples.size() - aggregate(cpu).count) << ". Missing ingress samples: "
         << (samples.size() - ingress_total.count) << ".\n\n";
  output << "## Host summary\n\n";
  output << "| Metric | Min | Average | Max |\n| --- | ---: | ---: | ---: |\n";
  output << "| CPU (%) | " << aggregate_cells(aggregate(cpu)) << " |\n";
  output << "| Memory (%) | " << aggregate_cells(aggregate(memory)) << " |\n";
  output << "| Root disk (%) | " << aggregate_cells(aggregate(disk)) << " |\n\n";
  output << "Ingress total: " << number(ingress_total.count == 0 ? 0.0 : ingress_total.average * ingress_total.count, 0)
         << " bytes.\n\n";
  output << "## " << (window.hourly_buckets ? "Hourly" : "Daily") << " trends\n\n";
  output << "- CPU: " << sparkline(cpu_graph) << "\n";
  output << "- Memory: " << sparkline(memory_graph) << "\n";
  output << "- Root disk: " << sparkline(disk_graph) << "\n";
  output << "- Ingress: " << sparkline(ingress_graph) << "\n\n";
  output << "`·` indicates a bucket with no value.\n\n";
  output << "## Peak processes\n\n";
  if (peak_cpu.has_value()) {
    output << "- CPU: `" << peak_cpu->pid << "` " << markdown_escape(peak_cpu->command)
           << " at " << number(peak_cpu->value) << "%\n";
  } else {
    output << "- CPU: —\n";
  }
  if (peak_memory.has_value()) {
    output << "- Memory: `" << peak_memory->pid << "` " << markdown_escape(peak_memory->command)
           << " at " << number(peak_memory->value) << "% (" << peak_memory->rss_bytes
           << " bytes RSS)\n";
  } else {
    output << "- Memory: —\n";
  }
  return output.str();
}

}  // namespace cpp_log
