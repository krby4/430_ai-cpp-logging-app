# Implementation Plan: Ubuntu System Monitor

## Overview

Implement `cpp-log`, a C++20 Ubuntu-only command-line collector.  `write`
persists one transactional SQLite sample; `report [daily|weekly|monthly]`
generates a stable Markdown report.  The implementation follows the approved
specification and ADR-001.

## Dependency Order

```text
Pure delta/report helpers
        |
Linux /proc + filesystem readers ---- SQLite schema and transactions
        |                                  |
        +------------- write command -------+
                                           |
                         calendar aggregation + Markdown reports
                                           |
                                    systemd templates / smoke test
```

## Tasks

### Task 1: Build and pure metric primitives

**Acceptance criteria:** CPU counter, interface-set delta, reset detection,
and report bucketing are deterministic and unit tested.

**Verification:** CTest unit executable passes.

### Task 2: SQLite sample store and retention

**Acceptance criteria:** schema is idempotent; a transaction stores samples and
baselines together; a failed transaction changes neither; 183-day cleanup only
removes historical samples older than the cutoff.

**Verification:** temporary-database integration tests pass.

### Task 3: Linux collector and `write` command

**Acceptance criteria:** parse `/proc/stat`, `/proc/meminfo`, `/proc/net/dev`,
`/proc/<pid>/stat`, `/proc/<pid>/status`, boot ID, and `statvfs("/")` safely;
handle disappearing processes and all reset cases as specified.

**Verification:** unit parser tests plus two local `write` invocations into a
temporary database.

### Checkpoint: Collection

- [ ] Build succeeds with warnings enabled.
- [ ] Unit/integration tests pass.
- [ ] First sample has `NULL` deltas; second valid sample has persisted data.

### Task 4: Calendar aggregation and Markdown reporting

**Acceptance criteria:** UTC records map to current local daily/ISO-week/month
windows; missing values render as gaps; aggregate values and sparklines are
correct; derived report files overwrite only their stable period filename.

**Verification:** deterministic report tests with a temporary database and
fixed timezone/clock inputs.

### Task 5: CLI, systemd, and operational documentation

**Acceptance criteria:** invalid commands fail cleanly; `write` and all report
modes work; service/timer examples invoke `write` once each minute as
`User=keith`; README documents build, deployment, and report locations.

**Verification:** CTest, a container/local smoke test, and `systemd-analyze
verify` when systemd tooling is available.

## Risks and Mitigations

| Risk | Mitigation |
| --- | --- |
| Reboot/counter reset | Persist boot ID; write `NULL` and rebaseline. |
| Timer/manual concurrent writers | `BEGIN IMMEDIATE`, 10-second busy timeout, atomic transaction. |
| Process races/permissions | Skip unreadable processes and mark partial scans. |
| Interface changes | Store per-interface identity; write `NULL` ingress and rebaseline. |
| DST/local-calendar ambiguity | Store UTC; apply local timezone only in report queries/rendering. |
| Database growth | Delete samples older than 183 days on successful writes; prune stale baselines. |
