# Events Extension for DuckDB

A DuckDB extension that hooks into database events and sends JSON-formatted notifications to external programs via stdin.

## Project Structure

```
src/
├── events_extension.cpp         # Main extension code with hooks and event handling
├── events_logging.cpp           # Custom DuckDB log type for the extension
├── query_farm_telemetry.cpp     # Anonymous usage telemetry (opt-out available)
└── include/
    ├── events_extension.hpp     # Extension class declaration
    ├── events_logging.hpp       # EventsLogType declaration
    └── query_farm_telemetry.hpp # Telemetry function declaration

test/
├── sql/events.test              # SQL-based tests
└── scripts/
    ├── event_receiver.py        # Test event handler script
    ├── test_events.py           # Event integration tests
    └── test_event_writer.py     # Event writer tests
```

## How It Works

The extension registers hooks into DuckDB's `ClientContextState` system to intercept database events. When an event occurs:

1. Event data is serialized to JSON using **yyjson** (DuckDB's bundled JSON library)
2. The configured external program is spawned
3. JSON is written to the program's stdin, then stdin is closed
4. In sync mode (default), DuckDB waits for the program to exit and logs the exit code
5. In async mode, DuckDB fires and forgets

## Settings

| Setting | Type | Default | Description |
|---------|------|---------|-------------|
| `events_destination` | VARCHAR | NULL | Path to the external program that receives events |
| `events_async` | BOOLEAN | false | If true, events are delivered asynchronously (fire and forget) |
| `events_types` | LIST(VARCHAR) | `['query_begin', 'query_end']` | List of event types to send |

### Usage

```sql
-- Set the destination program
SET events_destination = '/path/to/handler';

-- Optional: enable async mode
SET events_async = true;

-- Optional: customize which events to receive
SET events_types = ['query_begin', 'query_end', 'transaction_commit'];
```

## Event Types

The extension hooks into these DuckDB events:

| Event | When Fired | Extra Fields |
|-------|------------|--------------|
| `connection_opened` | New connection established | - |
| `connection_closed` | Connection terminated | - |
| `query_begin` | Query starts executing | - |
| `query_end` | Query completes | `has_error`, `error_message`, `error_type` |
| `transaction_begin` | Transaction starts | `transaction_id`, `start_timestamp`, `is_read_only` |
| `transaction_commit` | Transaction commits | `transaction_id`, `start_timestamp`, `is_read_only` |
| `transaction_rollback` | Transaction rolls back | `transaction_id`, `start_timestamp`, `is_read_only`, error info |
| `planning_error` | Query planning fails | `error_message`, `error_type`, `statement_type` |
| `finalize_prepare` | Prepared statement finalized | `statement_type` |
| `execute_prepared` | Prepared statement executed | - |
| `rebind_prepared_statement` | Prepared statement rebound | - |

See [docs/EVENTS.md](docs/EVENTS.md) for detailed documentation with examples.

## JSON Format

All events include these common fields:

```json
{
  "event": "query_begin",
  "timestamp": "2026-01-12T15:30:45.123Z",
  "database_path": "/path/to/db.duckdb",
  "connection_id": 1,
  "process_id": 12345
}
```

- `timestamp`: ISO 8601 format with milliseconds
- `database_path`: File path or `:memory:` for in-memory databases
- `connection_id`: DuckDB's internal connection identifier
- `process_id`: OS process ID of the DuckDB process

Query events (`query_begin`, `query_end`, `execute_prepared`, `rebind_prepared_statement`) also include:
- `query_id`: Unique identifier for the query within this connection
- `transaction_id`: Current transaction ID (0 if no active transaction)
- `attached_databases`: Array of attached databases with name, path, type, read_only, and temporary flags

## Building

```bash
# Debug build with ninja (recommended for development)
GEN=ninja make debug

# Release build
GEN=ninja make release

# Run tests
make test
```

## Key DuckDB APIs Used

### Extension Registration
- `ExtensionLoader` - Registers functions and settings
- `DBConfig::AddExtensionOption()` - Registers custom settings
- `ExtensionCallback` - Hooks for connection open/close

### Client Context Hooks
- `ClientContextState` - Base class for query/transaction hooks
- `RegisteredStateManager::Insert()` - Registers state per connection

### Settings
- `ClientConfig::set_variables` - Stores per-connection settings
- `ClientContext::TryGetCurrentSetting()` - Retrieves setting values
- `set_option_callback_t` - Callback signature: `void(ClientContext&, SetScope, Value&)`

### Logging
- `DUCKDB_LOG(context, LogType, message, {{"key", "value"}})` - Structured logging macro
- `LogType` - Custom log type base class (see `events_logging.hpp`)

### JSON (yyjson)
- Namespace: `duckdb_yyjson`
- `yyjson_mut_doc_new()`, `yyjson_mut_obj()`, `yyjson_mut_write()`, etc.
- Always `free()` the string from `yyjson_mut_write()` and call `yyjson_mut_doc_free()`

### Context Information
- `context.GetCurrentQuery()` - Current SQL query string
- `context.GetConnectionId()` - Connection identifier
- `DBConfig::GetConfig(context).options.database_path` - Database file path
- `MetaTransaction` - Transaction ID, start timestamp, read-only status

## Testing

SQL tests are in `test/sql/`. Run with:

```bash
make test
```

The test runner binary is at `build/debug/test/unittest`.

## Telemetry

The extension sends anonymous usage telemetry to Query Farm on load (extension name, version, DuckDB platform/version). This helps track extension adoption.

To opt out, set the environment variable:
```bash
export QUERY_FARM_TELEMETRY_OPT_OUT=1
```

Telemetry requires the `httpfs` extension to be available; if not present, no telemetry is sent.

## Example Event Handler

Simple bash script to log events:

```bash
#!/bin/bash
cat >> /tmp/duckdb_events.log
```

Python handler:

```python
#!/usr/bin/env python3
import sys
import json

data = json.load(sys.stdin)
print(f"Event: {data['event']}", file=sys.stderr)
```
