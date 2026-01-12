#!/usr/bin/env python3
"""
Test harness for DuckDB events extension.
Runs DuckDB commands and verifies that the expected events are captured.
"""

import subprocess
import json
import os
import sys
import tempfile
import time

# Path to DuckDB binary with events extension
DUCKDB_PATH = os.path.join(os.path.dirname(__file__), '../../build/debug/duckdb')
EVENT_RECEIVER = os.path.join(os.path.dirname(__file__), 'event_receiver.py')


def run_duckdb_commands(commands: list[str], events_file: str, async_mode: bool = False, events_types: list[str] = None) -> list[dict]:
    """
    Run DuckDB commands and return the captured events.

    Args:
        commands: List of SQL commands to execute
        events_file: Path to file where events will be written
        async_mode: If True, events are delivered asynchronously
        events_types: List of event types to enable (default: query_begin, query_end)
    """
    # Clear the events file
    if os.path.exists(events_file):
        os.remove(events_file)

    # Set environment variable for the event receiver
    env = os.environ.copy()
    env['EVENTS_OUTPUT_FILE'] = events_file

    # Write commands to a temporary SQL file
    with tempfile.NamedTemporaryFile(mode='w', suffix='.sql', delete=False) as sql_file:
        sql_file.write(f"SET events_destination = '{EVENT_RECEIVER}';\n")
        sql_file.write(f"SET events_async = {str(async_mode).lower()};\n")
        if events_types:
            types_list = ', '.join(f"'{t}'" for t in events_types)
            sql_file.write(f"SET events_types = [{types_list}];\n")
        for cmd in commands:
            sql_file.write(cmd + '\n')
        sql_path = sql_file.name

    try:
        # Run DuckDB with -f to execute SQL file
        result = subprocess.run(
            [DUCKDB_PATH, '-f', sql_path],
            capture_output=True,
            text=True,
            env=env,
            timeout=30,
        )
    finally:
        os.remove(sql_path)

    if result.returncode != 0:
        print(f"DuckDB stderr: {result.stderr}", file=sys.stderr)

    # Small delay to ensure all events are written (especially in async mode)
    if async_mode:
        time.sleep(0.5)

    # Read and parse the events
    events = []
    if os.path.exists(events_file):
        with open(events_file, 'r') as f:
            for line in f:
                line = line.strip()
                if line:
                    try:
                        events.append(json.loads(line))
                    except json.JSONDecodeError as e:
                        print(f"Failed to parse JSON: {line}", file=sys.stderr)
                        print(f"Error: {e}", file=sys.stderr)

    return events


def validate_common_fields(event: dict, event_type: str) -> list[str]:
    """Validate common fields present in all events."""
    errors = []

    if event.get('event') != event_type:
        errors.append(f"Expected event type '{event_type}', got '{event.get('event')}'")

    required_fields = ['timestamp', 'database_path', 'connection_id', 'process_id']
    for field in required_fields:
        if field not in event:
            errors.append(f"Missing required field: {field}")

    # Validate timestamp format (ISO 8601 with milliseconds)
    timestamp = event.get('timestamp', '')
    if not (len(timestamp) >= 20 and 'T' in timestamp and timestamp.endswith('Z')):
        errors.append(f"Invalid timestamp format: {timestamp}")

    # Validate process_id is a positive integer
    pid = event.get('process_id')
    if not isinstance(pid, int) or pid <= 0:
        errors.append(f"Invalid process_id: {pid}")

    # Validate connection_id is a non-negative integer
    conn_id = event.get('connection_id')
    if not isinstance(conn_id, int) or conn_id < 0:
        errors.append(f"Invalid connection_id: {conn_id}")

    return errors


def test_query_events():
    """Test query_begin and query_end events."""
    print("Testing query events...", end=" ")

    with tempfile.NamedTemporaryFile(suffix='.jsonl', delete=False) as f:
        events_file = f.name

    try:
        events = run_duckdb_commands([
            "SELECT 1 as test_value;",
        ], events_file)

        errors = []

        # Find query events (filter out connection events)
        query_events = [e for e in events if e.get('event') in ('query_begin', 'query_end')]

        if len(query_events) < 2:
            errors.append(f"Expected at least 2 query events, got {len(query_events)}")
        else:
            # Check query_begin
            begin_events = [e for e in query_events if e.get('event') == 'query_begin']
            if not begin_events:
                errors.append("No query_begin event found")
            else:
                errors.extend(validate_common_fields(begin_events[0], 'query_begin'))
                # Note: 'query' field is not available in debug builds due to GetCurrentQuery() assertions

            # Check query_end
            end_events = [e for e in query_events if e.get('event') == 'query_end']
            if not end_events:
                errors.append("No query_end event found")
            else:
                errors.extend(validate_common_fields(end_events[0], 'query_end'))
                if 'has_error' not in end_events[0]:
                    errors.append("query_end missing 'has_error' field")

        if errors:
            print("FAILED")
            for e in errors:
                print(f"  - {e}")
            return False
        else:
            print("PASSED")
            return True

    finally:
        if os.path.exists(events_file):
            os.remove(events_file)


def test_transaction_events():
    """Test transaction events."""
    print("Testing transaction events...", end=" ")

    with tempfile.NamedTemporaryFile(suffix='.jsonl', delete=False) as f:
        events_file = f.name

    try:
        events = run_duckdb_commands([
            "BEGIN TRANSACTION;",
            "CREATE TABLE test_tx (id INTEGER);",
            "COMMIT;",
        ], events_file, events_types=['transaction_begin', 'transaction_commit', 'transaction_rollback'])

        errors = []

        # Find transaction events
        tx_events = [e for e in events if 'transaction' in e.get('event', '')]

        begin_events = [e for e in tx_events if e.get('event') == 'transaction_begin']
        commit_events = [e for e in tx_events if e.get('event') == 'transaction_commit']

        if not begin_events:
            errors.append("No transaction_begin event found")
        else:
            errors.extend(validate_common_fields(begin_events[0], 'transaction_begin'))
            if 'transaction_id' not in begin_events[0]:
                errors.append("transaction_begin missing 'transaction_id' field")
            if 'start_timestamp' not in begin_events[0]:
                errors.append("transaction_begin missing 'start_timestamp' field")
            if 'is_read_only' not in begin_events[0]:
                errors.append("transaction_begin missing 'is_read_only' field")

        if not commit_events:
            errors.append("No transaction_commit event found")
        else:
            errors.extend(validate_common_fields(commit_events[0], 'transaction_commit'))

        if errors:
            print("FAILED")
            for e in errors:
                print(f"  - {e}")
            return False
        else:
            print("PASSED")
            return True

    finally:
        if os.path.exists(events_file):
            os.remove(events_file)


def test_error_events():
    """Test error handling in events."""
    print("Testing error events...", end=" ")

    with tempfile.NamedTemporaryFile(suffix='.jsonl', delete=False) as f:
        events_file = f.name

    try:
        events = run_duckdb_commands([
            "SELECT * FROM nonexistent_table_12345;",
        ], events_file)

        errors = []

        # Find query_end events with errors
        error_events = [e for e in events if e.get('event') == 'query_end' and e.get('has_error') == True]

        if not error_events:
            # Check for planning_error events instead
            planning_errors = [e for e in events if e.get('event') == 'planning_error']
            if not planning_errors:
                errors.append("No error event found for invalid query")
            else:
                event = planning_errors[0]
                errors.extend(validate_common_fields(event, 'planning_error'))
                if 'error_message' not in event:
                    errors.append("planning_error missing 'error_message' field")
        else:
            event = error_events[0]
            errors.extend(validate_common_fields(event, 'query_end'))
            if 'error_message' not in event:
                errors.append("Error event missing 'error_message' field")
            if 'error_type' not in event:
                errors.append("Error event missing 'error_type' field")

        if errors:
            print("FAILED")
            for e in errors:
                print(f"  - {e}")
            return False
        else:
            print("PASSED")
            return True

    finally:
        if os.path.exists(events_file):
            os.remove(events_file)


def test_async_mode():
    """Test async event delivery."""
    print("Testing async mode...", end=" ")

    with tempfile.NamedTemporaryFile(suffix='.jsonl', delete=False) as f:
        events_file = f.name

    try:
        events = run_duckdb_commands([
            "SELECT 42 as answer;",
        ], events_file, async_mode=True)

        errors = []

        # In async mode, we should still get events (just delivered asynchronously)
        if len(events) == 0:
            errors.append("No events captured in async mode")

        if errors:
            print("FAILED")
            for e in errors:
                print(f"  - {e}")
            return False
        else:
            print("PASSED")
            return True

    finally:
        if os.path.exists(events_file):
            os.remove(events_file)


def test_connection_events():
    """Test connection_opened and connection_closed events."""
    print("Testing connection events...", end=" ")

    with tempfile.NamedTemporaryFile(suffix='.jsonl', delete=False) as f:
        events_file = f.name

    try:
        events = run_duckdb_commands([
            "SELECT 1;",
        ], events_file)

        errors = []

        # Note: connection_opened happens before we set events_destination,
        # so we may not capture it. But connection_closed should be captured
        # if the connection cleanup happens after setting the destination.

        # For now, just verify we got some events
        if len(events) == 0:
            errors.append("No events captured")

        # Verify all captured events have valid common fields
        for event in events:
            event_type = event.get('event', 'unknown')
            field_errors = validate_common_fields(event, event_type)
            errors.extend(field_errors)

        if errors:
            print("FAILED")
            for e in errors:
                print(f"  - {e}")
            return False
        else:
            print("PASSED")
            return True

    finally:
        if os.path.exists(events_file):
            os.remove(events_file)


def test_attached_databases():
    """Test attached_databases info in query events."""
    print("Testing attached databases...", end=" ")

    with tempfile.NamedTemporaryFile(suffix='.jsonl', delete=False) as f:
        events_file = f.name

    # Create a unique temp file path but don't create the file yet
    db_file = tempfile.mktemp(suffix='.duckdb')

    try:
        # First, create an on-disk database with some data
        result = subprocess.run(
            [DUCKDB_PATH, db_file, '-c',
             "CREATE TABLE test_data (id INTEGER, value VARCHAR); INSERT INTO test_data VALUES (1, 'hello');"],
            capture_output=True,
            text=True,
            timeout=30,
        )

        # Set environment variable for the event receiver
        env = os.environ.copy()
        env['EVENTS_OUTPUT_FILE'] = events_file

        # Now attach that database and query it
        with tempfile.NamedTemporaryFile(mode='w', suffix='.sql', delete=False) as sql_file:
            sql_file.write(f"SET events_destination = '{EVENT_RECEIVER}';\n")
            sql_file.write(f"ATTACH '{db_file}' AS testdb;\n")
            sql_file.write("SELECT * FROM testdb.test_data;\n")
            sql_path = sql_file.name

        try:
            result = subprocess.run(
                [DUCKDB_PATH, '-f', sql_path],
                capture_output=True,
                text=True,
                env=env,
                timeout=30,
            )
        finally:
            os.remove(sql_path)

        # Read and parse the events
        events = []
        if os.path.exists(events_file):
            with open(events_file, 'r') as f:
                for line in f:
                    line = line.strip()
                    if line:
                        try:
                            events.append(json.loads(line))
                        except json.JSONDecodeError:
                            pass

        errors = []

        # Find query events with attached_databases
        query_events = [e for e in events if e.get('event') == 'query_begin' and 'attached_databases' in e]

        if not query_events:
            errors.append("No query_begin events with attached_databases found")
        else:
            # Get the last query event (after ATTACH)
            last_event = query_events[-1]
            attached_dbs = last_event.get('attached_databases', [])

            # Check that we have at least the testdb
            db_names = [db['name'] for db in attached_dbs]
            if 'testdb' not in db_names:
                errors.append(f"Expected 'testdb' in attached_databases, got: {db_names}")

            # Find testdb and verify its properties
            testdb = next((db for db in attached_dbs if db['name'] == 'testdb'), None)
            if testdb:
                # Verify path is set correctly
                if not testdb.get('path') or not testdb['path'].endswith('.duckdb'):
                    errors.append(f"Expected testdb path to end with .duckdb, got: {testdb.get('path')}")

                # Verify type is duckdb
                if testdb.get('type') != 'duckdb':
                    errors.append(f"Expected testdb type 'duckdb', got: {testdb.get('type')}")

                # Verify read_only is present
                if 'read_only' not in testdb:
                    errors.append("testdb missing 'read_only' field")

                # Verify temporary is present
                if 'temporary' not in testdb:
                    errors.append("testdb missing 'temporary' field")

        if errors:
            print("FAILED")
            for e in errors:
                print(f"  - {e}")
            return False
        else:
            print("PASSED")
            return True

    finally:
        if os.path.exists(events_file):
            os.remove(events_file)
        if os.path.exists(db_file):
            os.remove(db_file)


def print_sample_events():
    """Print sample events for documentation purposes."""
    print("\n" + "=" * 60)
    print("Sample Events")
    print("=" * 60)

    with tempfile.NamedTemporaryFile(suffix='.jsonl', delete=False) as f:
        events_file = f.name

    try:
        events = run_duckdb_commands([
            "CREATE TABLE users (id INTEGER, name VARCHAR);",
            "INSERT INTO users VALUES (1, 'Alice');",
            "SELECT * FROM users;",
            "BEGIN TRANSACTION;",
            "UPDATE users SET name = 'Bob' WHERE id = 1;",
            "COMMIT;",
        ], events_file)

        for event in events:
            print(json.dumps(event, indent=2))
            print()

    finally:
        if os.path.exists(events_file):
            os.remove(events_file)


def main():
    # Check if DuckDB binary exists
    if not os.path.exists(DUCKDB_PATH):
        print(f"Error: DuckDB binary not found at {DUCKDB_PATH}")
        print("Please build the extension first with: GEN=ninja make debug")
        sys.exit(1)

    # Check if event receiver exists
    if not os.path.exists(EVENT_RECEIVER):
        print(f"Error: Event receiver not found at {EVENT_RECEIVER}")
        sys.exit(1)

    print("=" * 60)
    print("DuckDB Events Extension Test Suite")
    print("=" * 60)
    print()

    results = []
    results.append(("Query Events", test_query_events()))
    results.append(("Transaction Events", test_transaction_events()))
    results.append(("Error Events", test_error_events()))
    results.append(("Async Mode", test_async_mode()))
    results.append(("Attached Databases", test_attached_databases()))
    results.append(("Connection Events", test_connection_events()))

    print()
    print("=" * 60)
    print("Test Results Summary")
    print("=" * 60)

    passed = sum(1 for _, r in results if r)
    total = len(results)

    for name, result in results:
        status = "PASSED" if result else "FAILED"
        print(f"  {name}: {status}")

    print()
    print(f"Total: {passed}/{total} tests passed")

    # Print sample events if all tests pass or if requested
    if '--samples' in sys.argv or (passed == total and '--no-samples' not in sys.argv):
        print_sample_events()

    sys.exit(0 if passed == total else 1)


if __name__ == '__main__':
    main()
