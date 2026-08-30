# Building the extension

```sh
# Clone with submodules
git clone --recurse-submodules git@github.com:Query-farm/events.git

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

## Running the Extension

```sh
./build/release/duckdb
```

```sql
D SET events_destination = '/bin/cat';
D SELECT 42;
-- JSON event is printed to stdout by /bin/cat
```

## Running Tests

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
