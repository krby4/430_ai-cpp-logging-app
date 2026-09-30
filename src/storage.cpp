#include "cpp_log/storage.hpp"

#include <sqlite3.h>

#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace cpp_log {
namespace {

[[noreturn]] void throw_sqlite(sqlite3* connection, const std::string& context) {
  throw std::runtime_error(context + ": " + sqlite3_errmsg(connection));
}

void execute(sqlite3* connection, const char* sql) {
  char* error = nullptr;
  if (sqlite3_exec(connection, sql, nullptr, nullptr, &error) != SQLITE_OK) {
    const std::string message = error == nullptr ? sqlite3_errmsg(connection) : error;
    sqlite3_free(error);
    throw std::runtime_error(message);
  }
}

class Statement {
 public:
  Statement(sqlite3* connection, const char* sql) : connection_(connection) {
    if (sqlite3_prepare_v2(connection, sql, -1, &statement_, nullptr) != SQLITE_OK) {
      throw_sqlite(connection, "prepare statement");
    }
  }

  ~Statement() { sqlite3_finalize(statement_); }

  void bind(int index, std::int64_t value) {
    if (sqlite3_bind_int64(statement_, index, value) != SQLITE_OK) {
      throw_sqlite(connection_, "bind integer");
    }
  }

  void bind(int index, double value) {
    if (sqlite3_bind_double(statement_, index, value) != SQLITE_OK) {
      throw_sqlite(connection_, "bind number");
    }
  }

  void bind(int index, const std::string& value) {
    if (sqlite3_bind_text(statement_, index, value.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK) {
      throw_sqlite(connection_, "bind text");
    }
  }

  void bind_null(int index) {
    if (sqlite3_bind_null(statement_, index) != SQLITE_OK) {
      throw_sqlite(connection_, "bind null");
    }
  }

  bool step_row() {
    const int result = sqlite3_step(statement_);
    if (result == SQLITE_ROW) {
      return true;
    }
    if (result != SQLITE_DONE) {
      throw_sqlite(connection_, "step statement");
    }
    return false;
  }

  void step_done() {
    if (sqlite3_step(statement_) != SQLITE_DONE) {
      throw_sqlite(connection_, "step statement");
    }
  }

  [[nodiscard]] sqlite3_stmt* get() const { return statement_; }

 private:
  sqlite3* connection_{};
  sqlite3_stmt* statement_{};
};

template <typename T>
void bind_optional(Statement& statement, int index, const std::optional<T>& value) {
  if (value.has_value()) {
    if constexpr (std::is_integral_v<T>) {
      statement.bind(index, static_cast<std::int64_t>(*value));
    } else {
      statement.bind(index, *value);
    }
  } else {
    statement.bind_null(index);
  }
}

void bind_process(Statement& statement, int first_index,
                  const std::optional<ProcessUsage>& process, bool memory) {
  if (!process.has_value()) {
    for (int index = first_index; index < first_index + (memory ? 4 : 4); ++index) {
      statement.bind_null(index);
    }
    return;
  }
  statement.bind(first_index, static_cast<std::int64_t>(process->pid));
  if (!memory) {
    statement.bind(first_index + 1, static_cast<std::int64_t>(process->start_ticks));
  }
  statement.bind(first_index + (memory ? 1 : 2), process->command);
  if (memory) {
    statement.bind(first_index + 2, static_cast<std::int64_t>(process->rss_bytes));
    statement.bind(first_index + 3, process->value);
  } else {
    statement.bind(first_index + 3, process->value);
  }
}

std::optional<double> optional_double(sqlite3_stmt* statement, int index) {
  if (sqlite3_column_type(statement, index) == SQLITE_NULL) {
    return std::nullopt;
  }
  return sqlite3_column_double(statement, index);
}

std::optional<std::uint64_t> optional_uint64(sqlite3_stmt* statement, int index) {
  if (sqlite3_column_type(statement, index) == SQLITE_NULL) {
    return std::nullopt;
  }
  return static_cast<std::uint64_t>(sqlite3_column_int64(statement, index));
}

}  // namespace

Database::Database(const std::string& path) {
  if (sqlite3_open_v2(path.c_str(), &connection_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                      nullptr) != SQLITE_OK) {
    const std::string message = connection_ == nullptr ? "unable to open database" : sqlite3_errmsg(connection_);
    sqlite3_close(connection_);
    connection_ = nullptr;
    throw std::runtime_error(message);
  }
  sqlite3_busy_timeout(connection_, 10'000);
}

Database::~Database() {
  rollback();
  sqlite3_close(connection_);
}

void Database::initialize() {
  execute(connection_, "PRAGMA journal_mode=WAL;");
  execute(connection_, "PRAGMA foreign_keys=ON;");
  execute(connection_,
          "CREATE TABLE IF NOT EXISTS samples ("
          "id INTEGER PRIMARY KEY, captured_at INTEGER NOT NULL, cpu_percent REAL, "
          "memory_percent REAL NOT NULL, root_disk_percent REAL NOT NULL, ingress_bytes INTEGER, "
          "top_cpu_pid INTEGER, top_cpu_start_ticks INTEGER, top_cpu_command TEXT, top_cpu_percent REAL, "
          "top_memory_pid INTEGER, top_memory_command TEXT, top_memory_rss_bytes INTEGER, top_memory_percent REAL, "
          "partial_process_scan INTEGER NOT NULL);");
  execute(connection_, "CREATE INDEX IF NOT EXISTS samples_captured_at ON samples(captured_at);");
  execute(connection_,
          "CREATE TABLE IF NOT EXISTS host_baseline ("
          "id INTEGER PRIMARY KEY CHECK (id = 1), boot_id TEXT NOT NULL, "
          "cpu_total_ticks INTEGER NOT NULL, cpu_idle_ticks INTEGER NOT NULL);");
  execute(connection_,
          "CREATE TABLE IF NOT EXISTS interface_baselines ("
          "name TEXT NOT NULL, interface_index INTEGER NOT NULL, received_bytes INTEGER NOT NULL, "
          "PRIMARY KEY(name, interface_index));");
  execute(connection_,
          "CREATE TABLE IF NOT EXISTS process_baselines ("
          "pid INTEGER NOT NULL, start_ticks INTEGER NOT NULL, total_ticks INTEGER NOT NULL, "
          "PRIMARY KEY(pid, start_ticks));");
}

void Database::begin_write() {
  if (in_write_transaction_) {
    throw std::logic_error("write transaction already active");
  }
  execute(connection_, "BEGIN IMMEDIATE;");
  in_write_transaction_ = true;
}

void Database::commit() {
  if (!in_write_transaction_) {
    throw std::logic_error("no write transaction to commit");
  }
  execute(connection_, "COMMIT;");
  in_write_transaction_ = false;
}

void Database::rollback() noexcept {
  if (!in_write_transaction_) {
    return;
  }
  sqlite3_exec(connection_, "ROLLBACK;", nullptr, nullptr, nullptr);
  in_write_transaction_ = false;
}

bool Database::in_write_transaction() const { return in_write_transaction_; }

std::optional<HostBaseline> Database::load_host_baseline() const {
  Statement host(connection_,
                 "SELECT boot_id, cpu_total_ticks, cpu_idle_ticks FROM host_baseline WHERE id = 1;");
  if (!host.step_row()) {
    return std::nullopt;
  }

  HostBaseline baseline;
  baseline.boot_id = reinterpret_cast<const char*>(sqlite3_column_text(host.get(), 0));
  baseline.cpu = {static_cast<std::uint64_t>(sqlite3_column_int64(host.get(), 1)),
                  static_cast<std::uint64_t>(sqlite3_column_int64(host.get(), 2))};

  Statement interfaces(connection_,
                       "SELECT name, interface_index, received_bytes FROM interface_baselines ORDER BY name;");
  while (interfaces.step_row()) {
    baseline.interfaces.push_back({reinterpret_cast<const char*>(sqlite3_column_text(interfaces.get(), 0)),
                                   sqlite3_column_int(interfaces.get(), 1),
                                   static_cast<std::uint64_t>(sqlite3_column_int64(interfaces.get(), 2))});
  }
  return baseline;
}

std::vector<ProcessBaseline> Database::load_process_baselines() const {
  Statement statement(connection_,
                      "SELECT pid, start_ticks, total_ticks FROM process_baselines;");
  std::vector<ProcessBaseline> baselines;
  while (statement.step_row()) {
    baselines.push_back({sqlite3_column_int(statement.get(), 0),
                         static_cast<std::uint64_t>(sqlite3_column_int64(statement.get(), 1)),
                         static_cast<std::uint64_t>(sqlite3_column_int64(statement.get(), 2))});
  }
  return baselines;
}

void Database::persist(const Sample& sample, const HostBaseline& baseline,
                       const std::vector<ProcessBaseline>& processes) {
  if (!in_write_transaction_) {
    throw std::logic_error("persist requires active write transaction");
  }
  Statement insert(connection_,
      "INSERT INTO samples (captured_at, cpu_percent, memory_percent, root_disk_percent, ingress_bytes, "
      "top_cpu_pid, top_cpu_start_ticks, top_cpu_command, top_cpu_percent, "
      "top_memory_pid, top_memory_command, top_memory_rss_bytes, top_memory_percent, partial_process_scan) "
      "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);");
  insert.bind(1, sample.captured_at);
  bind_optional(insert, 2, sample.cpu_percent);
  insert.bind(3, sample.memory_percent);
  insert.bind(4, sample.root_disk_percent);
  bind_optional(insert, 5, sample.ingress_bytes);
  bind_process(insert, 6, sample.top_cpu, false);
  bind_process(insert, 10, sample.top_memory, true);
  insert.bind(14, static_cast<std::int64_t>(sample.partial_process_scan));
  insert.step_done();

  Statement host(connection_,
      "INSERT INTO host_baseline (id, boot_id, cpu_total_ticks, cpu_idle_ticks) VALUES (1, ?, ?, ?) "
      "ON CONFLICT(id) DO UPDATE SET boot_id = excluded.boot_id, cpu_total_ticks = excluded.cpu_total_ticks, "
      "cpu_idle_ticks = excluded.cpu_idle_ticks;");
  host.bind(1, baseline.boot_id);
  host.bind(2, static_cast<std::int64_t>(baseline.cpu.total_ticks));
  host.bind(3, static_cast<std::int64_t>(baseline.cpu.idle_ticks));
  host.step_done();

  execute(connection_, "DELETE FROM interface_baselines;");
  Statement interface_insert(connection_,
      "INSERT INTO interface_baselines (name, interface_index, received_bytes) VALUES (?, ?, ?);");
  for (const auto& interface : baseline.interfaces) {
    interface_insert.bind(1, interface.name);
    interface_insert.bind(2, static_cast<std::int64_t>(interface.index));
    interface_insert.bind(3, static_cast<std::int64_t>(interface.received_bytes));
    interface_insert.step_done();
    sqlite3_reset(interface_insert.get());
    sqlite3_clear_bindings(interface_insert.get());
  }

  execute(connection_, "DELETE FROM process_baselines;");
  Statement process_insert(connection_,
      "INSERT INTO process_baselines (pid, start_ticks, total_ticks) VALUES (?, ?, ?);");
  for (const auto& process : processes) {
    process_insert.bind(1, static_cast<std::int64_t>(process.pid));
    process_insert.bind(2, static_cast<std::int64_t>(process.start_ticks));
    process_insert.bind(3, static_cast<std::int64_t>(process.total_ticks));
    process_insert.step_done();
    sqlite3_reset(process_insert.get());
    sqlite3_clear_bindings(process_insert.get());
  }
}

void Database::prune_samples_before(std::int64_t cutoff) {
  if (!in_write_transaction_) {
    throw std::logic_error("prune requires active write transaction");
  }
  Statement statement(connection_, "DELETE FROM samples WHERE captured_at < ?;");
  statement.bind(1, cutoff);
  statement.step_done();
}

int Database::sample_count() const {
  Statement statement(connection_, "SELECT COUNT(*) FROM samples;");
  statement.step_row();
  return sqlite3_column_int(statement.get(), 0);
}

std::optional<Sample> Database::latest_sample() const {
  Statement statement(connection_,
      "SELECT captured_at, cpu_percent, memory_percent, root_disk_percent, ingress_bytes, partial_process_scan "
      "FROM samples ORDER BY id DESC LIMIT 1;");
  if (!statement.step_row()) {
    return std::nullopt;
  }
  Sample sample;
  sample.captured_at = sqlite3_column_int64(statement.get(), 0);
  sample.cpu_percent = optional_double(statement.get(), 1);
  sample.memory_percent = sqlite3_column_double(statement.get(), 2);
  sample.root_disk_percent = sqlite3_column_double(statement.get(), 3);
  sample.ingress_bytes = optional_uint64(statement.get(), 4);
  sample.partial_process_scan = sqlite3_column_int(statement.get(), 5) != 0;
  return sample;
}

std::vector<Sample> Database::samples_between(std::int64_t start, std::int64_t end) const {
  Statement statement(connection_,
      "SELECT captured_at, cpu_percent, memory_percent, root_disk_percent, ingress_bytes, "
      "top_cpu_pid, top_cpu_start_ticks, top_cpu_command, top_cpu_percent, "
      "top_memory_pid, top_memory_command, top_memory_rss_bytes, top_memory_percent, partial_process_scan "
      "FROM samples WHERE captured_at >= ? AND captured_at < ? ORDER BY captured_at, id;");
  statement.bind(1, start);
  statement.bind(2, end);

  std::vector<Sample> samples;
  while (statement.step_row()) {
    Sample sample;
    sample.captured_at = sqlite3_column_int64(statement.get(), 0);
    sample.cpu_percent = optional_double(statement.get(), 1);
    sample.memory_percent = sqlite3_column_double(statement.get(), 2);
    sample.root_disk_percent = sqlite3_column_double(statement.get(), 3);
    sample.ingress_bytes = optional_uint64(statement.get(), 4);
    if (sqlite3_column_type(statement.get(), 5) != SQLITE_NULL) {
      sample.top_cpu = ProcessUsage{
          sqlite3_column_int(statement.get(), 5),
          static_cast<std::uint64_t>(sqlite3_column_int64(statement.get(), 6)),
          reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 7)),
          sqlite3_column_double(statement.get(), 8), 0};
    }
    if (sqlite3_column_type(statement.get(), 9) != SQLITE_NULL) {
      sample.top_memory = ProcessUsage{
          sqlite3_column_int(statement.get(), 9), 0,
          reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 10)),
          sqlite3_column_double(statement.get(), 12),
          static_cast<std::uint64_t>(sqlite3_column_int64(statement.get(), 11))};
    }
    sample.partial_process_scan = sqlite3_column_int(statement.get(), 13) != 0;
    samples.push_back(std::move(sample));
  }
  return samples;
}

}  // namespace cpp_log
