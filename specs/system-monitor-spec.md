# Spec: Ubuntu System Monitor

## Objective

Build a small C++ command-line application for one Ubuntu Server.  A systemd
timer invokes its `write` mode once per minute.  Each invocation captures a
host sample and persists it in SQLite; it does not create CSV files.  Its
`report` mode writes a self-contained Markdown aggregate report using Unicode
sparklines.

The operator needs an inexpensive historical view of server pressure without
running a separate monitoring service.  The feature is successful when the
timer can collect samples unattended and the operator can generate useful
current-day, current-week, or current-month reports from the database.

## Confirmed Design Decisions

- The executable is `cpp-log`, using SQLite's system C API (`libsqlite3`).
- The default database is `/srv/services/cpp-log/logs/monitor.db`; reports go
  to `/srv/services/cpp-log/logs/reports/` and are generated only on demand.
- The supplied systemd examples are system units with `User=keith`; a
  deployment replaces that value with its operating user.  The application
  neither creates users nor elevates privileges.
- SQLite timestamps are UTC.  Reports convert timestamps to the server's local
  timezone and use ISO weeks (Monday through Sunday).
- Missing/reset baselines, boot changes, and active-interface changes produce
  `NULL` CPU or ingress values, never invented zeroes.  Reports expose and
  exclude such gaps from numeric aggregates.
- The application serializes writes through SQLite with a ten-second busy
  timeout.  A writer that cannot acquire the lock fails without committing a
  sample or advancing baselines.
- Historical samples are retained for 183 days.  Baseline-only process and
  interface entries are pruned as soon as they disappear.

## Commands

```text
cpp-log write
cpp-log report
cpp-log report daily
cpp-log report weekly
cpp-log report monthly
```

`write` creates or migrates the SQLite schema, obtains one sample, and exits
nonzero with a useful error if `/proc`, the root filesystem, or the database
cannot be read/written.  `report` defaults to `weekly`; its optional period is
one of `daily`, `weekly`, or `monthly`.  It writes a Markdown file and prints
that path to stdout.  Invalid modes or periods print usage and exit nonzero.

Build and test commands to establish with the implementation:

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

The target Ubuntu image must provide a C++ compiler, CMake, and SQLite's
development package (`libsqlite3-dev`).

## Collected Data

Each successful `write` stores a timestamp and these metrics:

| Metric | Definition |
| --- | --- |
| CPU usage | Host busy CPU percentage over the interval since the preceding sample, computed from `/proc/stat`. |
| Memory usage | Used physical memory percentage, derived from `/proc/meminfo` using `MemTotal - MemAvailable`. |
| Root disk usage | Used capacity percentage for `/`, from `statvfs`. |
| Network ingress | Received-byte delta since the previous successful sample, summed across the same active, non-loopback interface set. |
| Top CPU process | Process with the greatest CPU usage over the interval: PID, start time, command name, and CPU percentage. |
| Top memory process | Process with the greatest resident memory at collection time: PID, command name, RSS bytes, and memory percentage. |

The application maintains private SQLite baseline state for host counters,
interfaces, and per-process CPU counters.  A process baseline is keyed by PID
plus process start time to avoid PID-reuse errors.  A boot-ID change, counter
regression, or changed active-interface set resets the relevant baseline and
makes its affected metric `NULL`.  This internal state is not report data.

## Reporting

The report covers the current calendar period at execution time:

- `daily`: hourly buckets from midnight through the current hour.
- `weekly` (default): daily buckets from the ISO-week Monday through today.
- `monthly`: daily buckets from the first day of the month through today.

Every report includes the covered date range, sample count, missing-data count,
and min/average/max for CPU, memory, and root-disk usage.  It reports a total
for ingress, summed into its hourly/daily graph buckets.  It also identifies the single
sample with the highest CPU process percentage and the one with the largest
memory process percentage.  It includes one Unicode sparkline per host metric
using the stated bucket size.  No images, web server, CSV, or external charting
tool is required.

## Tech Stack and Project Structure

```text
main.cpp                 -> temporary entry point; replaced by application wiring
CMakeLists.txt           -> CMake targets and SQLite discovery
src/                     -> CLI, collectors, SQLite repository, report formatter
include/cpp_log/         -> public internal headers and data models
tests/                   -> CTest-backed unit/integration tests
systemd/                 -> cpp-log.service and cpp-log.timer units
specs/                   -> this specification and implementation plan
```

Use C++20, the standard library, POSIX/Linux interfaces, and the SQLite C API.
The monitor is Linux-specific by design; portability to other operating systems
is not a goal.

## Code Style

Use value types for samples, explicit error results at I/O boundaries, and
small dependency-injected interfaces for filesystem/proc and clock access in
tests.  Keep parsing and rendering separate from persistence.

```cpp
struct HostSample {
  std::chrono::sys_seconds captured_at;
  double cpu_percent;
  double memory_percent;
  double root_disk_percent;
  std::uint64_t ingress_bytes;
};

Result<HostSample> collect_host_sample(const CounterBaseline& previous);
```

Names use `snake_case` for functions and variables, `PascalCase` for types,
and `kPascalCase` for constants.  SQL statements are parameterized; paths,
database errors, and `/proc` data are validated before use.

## Testing Strategy

Use CTest with a small C++ test executable and no additional test framework.

- Unit tests: `/proc` parsing, counter deltas, PID-reuse matching, SQL mapping,
  calendar window boundaries, aggregation, Markdown escaping, and sparkline
  bucketing/rendering.
- Integration tests: a temporary SQLite database verifies schema creation,
  append behavior, report queries, and generated report contents.
- Manual smoke test on Ubuntu: run `write` twice, run each report period, then
  verify database data and the Markdown file.  Enable the systemd timer and
  confirm a second scheduled sample is stored.

## Boundaries

- Always: validate mode/period inputs, parameterize SQLite statements, use a
  ten-second SQLite writer lock and transactions for each persisted sample and
  its baselines, test before handoff, and preserve database data on failures.
- Ask first: change the database location or retention policy, add a third
  party dependency, alter systemd service permissions, or add support for
  more filesystems/interfaces.
- Never: emit CSV files, store credentials, run with broader privileges than
  needed, collect process command
  arguments/environment values.

## Success Criteria

- `cpp-log write` persists exactly one complete sample and its process fields
  in `/srv/services/cpp-log/logs/monitor.db`.
- A first run creates the database/schema; later runs append without replacing
  historical samples.
- `cpp-log report`, `cpp-log report daily`, and `cpp-log report monthly`
  produce correctly bounded Markdown reports; explicit `weekly` matches the
  default.
- Reports contain aggregate statistics and correctly sized Unicode sparklines.
- The supplied systemd timer runs `write` once per minute under `User=keith`
  (to be changed for deployment), with write access to the target directory.

## Out of Scope

CSV output, a live dashboard, alerts, remote telemetry, non-Linux support,
automated data retention, disk metrics beyond `/`, graphics files, and raw
per-minute report dumps are out of scope.
