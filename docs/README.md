# Events Extension for DuckDB

A DuckDB extension that hooks into database events and sends JSON-formatted notifications to external programs via stdin. Created by [Query.Farm](https://query.farm).

## Installation

**`events` is a [DuckDB Community Extension](https://github.com/duckdb/community-extensions).**

You can install and load it using:

```sql
INSTALL events FROM community;
LOAD events;
```

## How It Works

The extension captures database activities in real-time and delivers them to external programs for processing. This enables:

- **Audit logging**: Track who ran what queries and when
- **Monitoring**: Watch for errors, slow queries, or suspicious activity
- **Integration**: Feed database activity into observability platforms, SIEMs, or custom analytics
- **Debugging**: Understand query execution flow during development

The Events extension intercepts DuckDB's internal events (queries, transactions, connections) and delivers them as JSON to any program that can read from stdin.

## When Would I Use This?

### Audit Logging

Log all database activity to a file for compliance or debugging:

```sql
LOAD events;
SET events_destination = '/usr/local/bin/log-to-file.sh';
SET events_types = ['query_begin', 'query_end'];

-- All queries are now logged
SELECT * FROM sensitive_data;
```

### Error Monitoring

Send error events to your monitoring system:

```sql
LOAD events;
SET events_destination = '/usr/local/bin/error-alerter.py';
SET events_types = ['query_end'];  -- query_end includes error information

-- Failed queries generate events with error details
SELECT * FROM nonexistent_table;
-- Event includes: has_error=true, error_message, error_type
```

### Transaction Tracking

Monitor transaction lifecycle for debugging or analytics:

```sql
LOAD events;
SET events_destination = '/usr/local/bin/txn-tracker.py';
SET events_types = ['transaction_begin', 'transaction_commit', 'transaction_rollback'];

BEGIN;
INSERT INTO orders VALUES (1, 'widget', 99.99);
COMMIT;
-- Events capture transaction_id, timestamps, and commit/rollback status
```

### High-Throughput Scenarios

For production workloads where latency matters, use async mode:

```sql
LOAD events;
SET events_async = true;  -- Fire and forget
SET events_destination = '/usr/local/bin/event-handler.py';

-- Events are delivered without blocking queries
SELECT * FROM large_table;
```

## Configuration

| Setting | Type | Default | Description |
|---------|------|---------|-------------|
| `events_destination` | VARCHAR | (none) | Path to program that receives events via stdin |
| `events_async` | BOOLEAN | false | Fire-and-forget delivery (doesn't wait for handler) |
| `events_types` | LIST | ['query_begin', 'query_end'] | Which event types to capture |

### Available Event Types

- `connection_opened` - New connection established
- `connection_closed` - Connection terminated
- `query_begin` - Query starts executing
- `query_end` - Query completes (with error info if failed)
- `transaction_begin` - Transaction starts
- `transaction_commit` - Transaction committed
- `transaction_rollback` - Transaction rolled back

For detailed event schemas and examples, see [EVENTS.md](EVENTS.md).

## Event Format

All events are JSON objects with common fields:

```json
{
  "event": "query_end",
  "timestamp": "2026-01-12T15:30:45.123Z",
  "database_path": "/data/analytics.duckdb",
  "connection_id": 1,
  "process_id": 12345,
  "query_id": 42,
  "has_error": false
}
```

Events are delivered one at a time to the handler's stdin, making it easy to process with any language.

## Event Handler Examples

### Bash (append to log file)

```bash
#!/bin/bash
cat >> /var/log/duckdb_events.jsonl
```

### Python (filter and forward)

```python
#!/usr/bin/env python3
import sys
import json

event = json.load(sys.stdin)

if event.get('has_error'):
    # Forward errors to alerting system
    print(json.dumps(event), file=sys.stderr)
```

### Usage

```sql
SET events_destination = '/path/to/handler.py';
SELECT * FROM my_table;  -- Event sent to handler
```

## Fun: Nyan Cat Query Music

![Nyan Cat](https://upload.wikimedia.org/wikipedia/en/e/ed/Nyan_cat_250px_frame.PNG)

Want to hear the [Nyan Cat](https://dn720700.ca.archive.org/0/items/NyanCatoriginal/Nyan%20Cat%20%5Boriginal%5D.mp3) theme while your queries run? Download [play-query-song.py](https://gist.github.com/rustyconover/4f9178c386c240c5cd37aa1306432591) - it plays music when a query starts and stops when it completes. Works on macOS, Linux, and Windows.

```bash
curl -o play-query-song.py https://gist.githubusercontent.com/rustyconover/4f9178c386c240c5cd37aa1306432591/raw/play-query-song.py
chmod +x play-query-song.py
```

```sql
LOAD events;
SET events_destination = '/path/to/play-query-song.py';
SET events_types = ['query_begin', 'query_end'];

-- Enjoy the music while this runs
SELECT count(*) FROM range(50000000);
```

The script automatically downloads the audio file on first use.

## Credits

1. This extension was created by [Query.Farm](https://query.farm).

2. Built using the [DuckDB Extension Template](https://github.com/duckdb/extension-template).

## Building

### Build Steps

```sh
# Clone with submodules
git clone --recurse-submodules https://github.com/Query-Farm/duckdb-events.git

# Build (ninja + ccache recommended for faster rebuilds)
GEN=ninja make
```

The main binaries that will be built are:

```sh
./build/release/duckdb
./build/release/test/unittest
./build/release/extension/events/events.duckdb_extension
```

- `duckdb` is the DuckDB shell with the extension pre-loaded
- `unittest` is the test runner with the extension linked in
- `events.duckdb_extension` is the loadable binary for distribution

### Running the Extension

```sh
./build/release/duckdb
```

```sql
D SET events_destination = '/bin/cat';
D SELECT 42;
-- JSON event is printed to stdout by /bin/cat
```

### Running Tests

```sh
make test
```

## Debugging with DuckDB Logging

The extension logs internal operations using DuckDB's logging system:

```sql
LOAD events;
CALL enable_logging('Events');

SET events_destination = '/bin/echo';
SELECT 1;

SELECT event, info FROM duckdb_logs_parsed('Events');
```

This shows handler process start/exit events with exit codes, useful for debugging handler issues.
