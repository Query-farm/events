# Testing this extension

Build with `GEN=ninja make release`, then run:

```sh
QUERY_FARM_TELEMETRY_OPT_OUT=1 make test
```

The test target runs the Python CLI integration suite and all SQLLogicTests in
`sql/`. `make test_debug` tests a debug build. Both suites require Python 3 on
`PATH`; neither needs third-party Python packages.

The SQL tests cover settings, filtering, query/transaction/connection callbacks,
planning errors, prepared parameters, and session isolation. The integration
suite loads the binary extension and additionally checks large Unicode payloads,
command quoting, early receiver exits, missing executables, asynchronous
delivery, and external-access restrictions.

Run integration tests separately with:

```sh
python3 test/scripts/test_events.py --duckdb build/release/duckdb -v
```

CI runs these same Makefile targets for both fresh builds and restored artifacts.
