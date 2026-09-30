# cpp-log

`cpp-log` is a Linux-only C++20 monitor for one Ubuntu Server. It stores one
sample per `write` invocation in SQLite and renders on-demand Markdown reports.

## Commands

```bash
cpp-log write
cpp-log report            # current ISO week (default)
cpp-log report daily      # current local calendar day
cpp-log report monthly    # current local calendar month
```

The database is `/srv/services/cpp-log/logs/monitor.db`. Reports are generated
only when requested under `/srv/services/cpp-log/logs/reports/`, overwriting the
stable filename for their active period.

## Build and test

Ubuntu requires `g++`, `cmake`, and `libsqlite3-dev`.

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

## systemd setup

Build/install the executable at `/srv/services/cpp-log/bin/cpp-log`, ensure the
operating user can write `/srv/services/cpp-log/logs`, then copy the unit files:

```bash
sudo cp systemd/cpp-log.service systemd/cpp-log.timer /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now cpp-log.timer
```

The supplied example runs as `User=keith`; replace `User=` and `Group=` with
the deployment user before enabling it. The timer runs every minute and can
catch up after a reboot. Counter resets become explicit report gaps rather than
false zeroes.

## Getting Started

This repository is compatible with [cpp-container](https://github.com/ChicoState/cpp-container). If not already built on your machine, clone and build it.

Run the container:

```bash
docker run -v "$(pwd)":/usr/src -it cpp-container
```

Run the application interactively in a shell:

```bash
docker run -v "$(pwd)":/usr/src -it cpp-container sh
```

## Structure

* `.agents` - AI agent configurations and skills (in `/skills` subdirectory) for this project
* `.` - The root directory contains the C++ code for the application as well as necessary scripts
* `specs` - Specification documentation
* `tests` - Test code
