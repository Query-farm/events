# Building the extension

```sh
# Clone with submodules
git clone --branch v2.0 --recurse-submodules git@github.com:Query-farm/events.git
cd events

# Build (ninja + ccache recommended for faster rebuilds)
GEN=ninja make
```

The main binaries that will be built are:

```sh
./build/release/duckdb
./build/release/test/unittest
./build/release/extension/events/events.duckdb_extension
```

- `duckdb` is the DuckDB shell with a local extension repository
- `unittest` is the SQLLogicTest runner, which loads the extension via `require events`
- `events.duckdb_extension` is the loadable binary for distribution

## Running the Extension

```sh
./build/release/duckdb -unsigned
```

```sql
LOAD 'build/release/extension/events/events.duckdb_extension';
SET events_destination = '/bin/cat';
SELECT 42;
-- JSON event is printed to stdout by /bin/cat
```

## Running Tests

```sh
QUERY_FARM_TELEMETRY_OPT_OUT=1 make test
```

This runs both the SQL tests and the Python CLI integration suite. Python 3 must
be on `PATH`; no Python packages are required. Integration tests explicitly load
the built extension and cover callback correlation, prepared statements, large
payloads, quoting, failed receivers, asynchronous delivery, and external-access
settings. Invalid JSON and unexpected CLI failures fail the suite.

To run only the integration suite, or test another build:

```sh
python3 test/scripts/test_events.py --duckdb build/release/duckdb -v
GEN=ninja make debug
QUERY_FARM_TELEMETRY_OPT_OUT=1 make test_debug
```

The submodules pin DuckDB 2.0.0 and the same extension build tools as `crypto`.
The Makefile sets the release version explicitly so local extension metadata
matches the released ABI. The GitHub workflow builds and tests native platforms
daily, then signs and deploys successful builds to
`https://duckdb-extensions.query.farm`. WebAssembly is excluded because event
delivery requires spawning processes. A manual `dry_run` verifies signing
without uploading artifacts.

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
