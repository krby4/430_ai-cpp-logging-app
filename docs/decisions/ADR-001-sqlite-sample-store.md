# ADR-001: Use SQLite for monitor samples and baselines

## Status

Accepted

## Context

The monitor records a sample per minute on one Ubuntu Server and must create
aggregate Markdown reports without CSV files or a separate service.  Counter
deltas require durable previous values and concurrent timer/manual writes must
not race.

## Decision

Use a single SQLite database at the configured log path.  Store sample history
for 183 days and keep disposable host, interface, and process baselines in
separate tables.  Use WAL mode, parameterized statements, and `BEGIN IMMEDIATE`
with a ten-second busy timeout for every `write` operation.

## Consequences

SQLite keeps deployment to one file and makes report aggregation local.  Only
one writer can run at a time; a contending writer fails rather than calculating
against stale state.  Reports use read transactions and do not block a writer
in normal WAL operation.
