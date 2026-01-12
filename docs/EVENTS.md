# DuckDB Events Extension - Event Reference

This document describes all events emitted by the DuckDB Events extension, their fields, and provides examples.

## Overview

The Events extension hooks into DuckDB's internal event system and sends JSON-formatted notifications to an external program via stdin. Events are delivered either synchronously (default) or asynchronously based on the `events_async` setting.

## Configuration

```sql
-- Set the path to your event handler program
SET events_destination = '/path/to/handler';

-- Optional: enable async delivery (fire and forget)
SET events_async = true;

-- Optional: specify which event types to send (default: query_begin, query_end)
SET events_types = ['query_begin', 'query_end', 'transaction_begin', 'transaction_commit'];
```

### Settings

| Setting | Type | Default | Description |
|---------|------|---------|-------------|
| `events_destination` | VARCHAR | (none) | Path to the program that receives event notifications via stdin |
| `events_async` | BOOLEAN | false | If true, events are delivered asynchronously (fire and forget) |
| `events_types` | LIST(VARCHAR) | ['query_begin', 'query_end'] | List of event types to send. See Event Types below for valid values. |

## Common Fields

All events include these common fields:

| Field | Type | Description |
|-------|------|-------------|
| `event` | string | The event type name |
| `timestamp` | string | ISO 8601 timestamp with milliseconds (e.g., `2026-01-12T15:30:45.123Z`) |
| `database_path` | string | Path to the database file, or `:memory:` for in-memory databases |
| `connection_id` | integer | DuckDB's internal connection identifier |
| `process_id` | integer | Operating system process ID of the DuckDB process |

## Event Types

### connection_opened

Emitted when a new database connection is established.

**Additional Fields:** None

**Example:**
```json
{
  "event": "connection_opened",
  "timestamp": "2026-01-12T14:35:24.491Z",
  "database_path": ":memory:",
  "connection_id": 1,
  "process_id": 29483
}
```

---

### connection_closed

Emitted when a database connection is closed.

**Additional Fields:** None

**Example:**
```json
{
  "event": "connection_closed",
  "timestamp": "2026-01-12T14:36:51.353Z",
  "database_path": ":memory:",
  "connection_id": 2,
  "process_id": 29483
}
```

---

### query_begin

Emitted when a query starts executing.

**Additional Fields:**

| Field | Type | Description |
|-------|------|-------------|
| `query_id` | integer | Unique identifier for this query within the connection (correlates with `query_end`) |
| `transaction_id` | integer | Current transaction identifier (0 if no active transaction) |
| `attached_databases` | array | List of currently attached databases (see below) |

**Attached Database Object:**

| Field | Type | Description |
|-------|------|-------------|
| `name` | string | Database name/alias |
| `path` | string | File path (empty for in-memory databases) |
| `type` | string | Database type: `duckdb` for native databases, `external` for storage extensions |
| `read_only` | boolean | Whether the database is read-only |
| `temporary` | boolean | Whether the database is temporary |

**Example:**
```json
{
  "event": "query_begin",
  "timestamp": "2026-01-12T14:35:24.593Z",
  "database_path": ":memory:",
  "connection_id": 2,
  "process_id": 29483,
  "query_id": 1,
  "transaction_id": 2,
  "attached_databases": [
    {"name": "mydb", "path": "/data/mydb.duckdb", "type": "duckdb", "read_only": false, "temporary": false},
    {"name": "memory", "path": "", "type": "duckdb", "read_only": false, "temporary": false},
    {"name": "temp", "path": "", "type": "duckdb", "read_only": false, "temporary": true}
  ]
}
```

---

### query_end

Emitted when a query finishes executing (successfully or with an error).

**Additional Fields:**

| Field | Type | Description |
|-------|------|-------------|
| `query_id` | integer | Unique identifier for this query within the connection (correlates with `query_begin`) |
| `transaction_id` | integer | Current transaction identifier (0 if no active transaction) |
| `has_error` | boolean | Whether the query failed |
| `error_message` | string | Error message (only present if `has_error` is true) |
| `error_type` | string | Error type/category (only present if `has_error` is true) |
| `attached_databases` | array | List of currently attached databases (same format as `query_begin`) |

**Example (success):**
```json
{
  "event": "query_end",
  "timestamp": "2026-01-12T14:36:50.565Z",
  "database_path": ":memory:",
  "connection_id": 2,
  "process_id": 30150,
  "query_id": 1,
  "transaction_id": 2,
  "has_error": false,
  "attached_databases": [
    {"name": "memory", "path": "", "type": "duckdb", "read_only": false, "temporary": false},
    {"name": "temp", "path": "", "type": "duckdb", "read_only": false, "temporary": true}
  ]
}
```

**Example (error):**
```json
{
  "event": "query_end",
  "timestamp": "2026-01-12T14:36:50.565Z",
  "database_path": ":memory:",
  "connection_id": 2,
  "process_id": 30150,
  "query_id": 1,
  "transaction_id": 0,
  "has_error": true,
  "error_message": "Table with name 'users' does not exist!",
  "error_type": "Catalog",
  "attached_databases": [
    {"name": "memory", "path": "", "type": "duckdb", "read_only": false, "temporary": false},
    {"name": "temp", "path": "", "type": "duckdb", "read_only": false, "temporary": true}
  ]
}
```

---

### transaction_begin

Emitted when a transaction starts.

**Additional Fields:**

| Field | Type | Description |
|-------|------|-------------|
| `transaction_id` | integer | Global transaction identifier |
| `start_timestamp` | integer | Internal timestamp when the transaction started |
| `is_read_only` | boolean | Whether the transaction is read-only |

**Example:**
```json
{
  "event": "transaction_begin",
  "timestamp": "2026-01-12T14:36:50.7Z",
  "database_path": ":memory:",
  "connection_id": 2,
  "process_id": 30150,
  "transaction_id": 2,
  "start_timestamp": 1768228610007169,
  "is_read_only": false
}
```

---

### transaction_commit

Emitted when a transaction is successfully committed.

**Additional Fields:**

| Field | Type | Description |
|-------|------|-------------|
| `transaction_id` | integer | Global transaction identifier |
| `start_timestamp` | integer | Internal timestamp when the transaction started |
| `is_read_only` | boolean | Whether the transaction was read-only |

**Example:**
```json
{
  "event": "transaction_commit",
  "timestamp": "2026-01-12T14:36:49.881Z",
  "database_path": ":memory:",
  "connection_id": 2,
  "process_id": 30150,
  "transaction_id": 1,
  "start_timestamp": 1768228609877273,
  "is_read_only": false
}
```

---

### transaction_rollback

Emitted when a transaction is rolled back.

**Additional Fields:**

| Field | Type | Description |
|-------|------|-------------|
| `transaction_id` | integer | Global transaction identifier |
| `start_timestamp` | integer | Internal timestamp when the transaction started |
| `is_read_only` | boolean | Whether the transaction was read-only |
| `has_error` | boolean | Whether the rollback was due to an error |
| `error_message` | string | Error message (only present if `has_error` is true) |
| `error_type` | string | Error type/category (only present if `has_error` is true) |

**Example (explicit rollback):**
```json
{
  "event": "transaction_rollback",
  "timestamp": "2026-01-12T14:36:50.500Z",
  "database_path": ":memory:",
  "connection_id": 2,
  "process_id": 30150,
  "transaction_id": 3,
  "start_timestamp": 1768228610215409,
  "is_read_only": false,
  "has_error": false
}
```

**Example (error rollback):**
```json
{
  "event": "transaction_rollback",
  "timestamp": "2026-01-12T14:36:50.500Z",
  "database_path": ":memory:",
  "connection_id": 2,
  "process_id": 30150,
  "transaction_id": 3,
  "start_timestamp": 1768228610215409,
  "is_read_only": false,
  "has_error": true,
  "error_message": "Constraint violation",
  "error_type": "Constraint"
}
```

---

### planning_error

Emitted when query planning/preparation fails.

**Additional Fields:**

| Field | Type | Description |
|-------|------|-------------|
| `error_message` | string | The error message |
| `error_type` | string | Error type/category |
| `statement_type` | string | Type of SQL statement (e.g., `SELECT`, `INSERT`) |

**Example:**
```json
{
  "event": "planning_error",
  "timestamp": "2026-01-12T14:36:50.200Z",
  "database_path": ":memory:",
  "connection_id": 2,
  "process_id": 30150,
  "error_message": "Table 'users' does not exist",
  "error_type": "Catalog",
  "statement_type": "SELECT_NODE"
}
```

---

### finalize_prepare

Emitted when a prepared statement is finalized.

**Additional Fields:**

| Field | Type | Description |
|-------|------|-------------|
| `statement_type` | string | Type of SQL statement |

**Example:**
```json
{
  "event": "finalize_prepare",
  "timestamp": "2026-01-12T14:36:50.300Z",
  "database_path": ":memory:",
  "connection_id": 2,
  "process_id": 30150,
  "statement_type": "SELECT_NODE"
}
```

---

### execute_prepared

Emitted when a prepared statement is executed (typically via C API).

**Additional Fields:**

| Field | Type | Description |
|-------|------|-------------|
| `query_id` | integer | Unique identifier for this query within the connection |
| `transaction_id` | integer | Current transaction identifier (0 if no active transaction) |
| `statement_type` | string | Type of SQL statement (e.g., `SELECT`, `INSERT`) |
| `parameters` | object | Bound parameter values (if any) |

**Example:**
```json
{
  "event": "execute_prepared",
  "timestamp": "2026-01-12T14:36:50.400Z",
  "database_path": ":memory:",
  "connection_id": 2,
  "process_id": 30150,
  "query_id": 3,
  "transaction_id": 2,
  "statement_type": "SELECT",
  "parameters": {"1": "42"}
}
```

---

### rebind_prepared_statement

Emitted when a prepared statement is rebound with new parameters (e.g., via SQL `EXECUTE` statement).

**Additional Fields:**

| Field | Type | Description |
|-------|------|-------------|
| `query_id` | integer | Unique identifier for this query within the connection |
| `transaction_id` | integer | Current transaction identifier (0 if no active transaction) |
| `statement_type` | string | Type of SQL statement (e.g., `SELECT`, `INSERT`) |
| `parameters` | object | Bound parameter values keyed by parameter name/position |

**Example:**
```json
{
  "event": "rebind_prepared_statement",
  "timestamp": "2026-01-12T14:36:50.450Z",
  "database_path": ":memory:",
  "connection_id": 2,
  "process_id": 30150,
  "query_id": 5,
  "transaction_id": 5,
  "statement_type": "SELECT",
  "parameters": {"1": "1"}
}
```

---

## Event Handler Example

### Bash (simple logging)

```bash
#!/bin/bash
# Save as: /path/to/event_handler.sh
cat >> /var/log/duckdb_events.jsonl
```

### Python (with filtering)

```python
#!/usr/bin/env python3
"""
Event handler that logs query events to stderr.
"""
import sys
import json

data = json.load(sys.stdin)
event_type = data.get('event', 'unknown')

if event_type in ('query_begin', 'query_end'):
    timestamp = data.get('timestamp', 'N/A')
    has_error = data.get('has_error', False)
    status = 'ERROR' if has_error else 'OK'
    print(f"[{timestamp}] {event_type}: {status}", file=sys.stderr)
```

### Usage

```sql
SET events_destination = '/path/to/event_handler.py';

-- Run queries - events will be sent to the handler
SELECT * FROM my_table;
```

## Notes

1. **Synchronous vs Asynchronous**: By default, events are delivered synchronously, meaning DuckDB waits for the external program to complete before continuing. Set `events_async = true` for fire-and-forget delivery.

2. **Performance**: In synchronous mode, slow event handlers will impact query performance. Consider async mode for production workloads with latency-sensitive requirements.

3. **JSON Lines Format**: When writing events to a file, each event is a single JSON object on its own line (JSONL format), making it easy to process with standard tools.

4. **Process Spawning**: A new process is spawned for each event. For high-throughput scenarios, consider using async mode or implementing a persistent daemon that receives events.
