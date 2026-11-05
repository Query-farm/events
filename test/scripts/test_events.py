#!/usr/bin/env python3
"""Exercise the built, loadable extension through DuckDB's CLI (standard library only)."""

import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]
DUCKDB = Path(os.environ.get("DUCKDB_PATH", ROOT / "build/release/duckdb"))
EXTENSION = None


def sql_string(value):
    return "'" + str(value).replace("'", "''") + "'"


def command_line(*args):
    args = [str(arg) for arg in args]
    return subprocess.list2cmdline(args) if os.name == "nt" else shlex.join(args)


class EventsTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="events test ")
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name)
        self.events_file = self.path / "captured events.jsonl"
        self.receiver = self.path / "event receiver.py"
        self.receiver.write_text(
            "import json, pathlib, sys, time\n"
            "event = json.load(sys.stdin)\n"
            "if '--wait-for' in sys.argv:\n"
            "    gate = pathlib.Path(sys.argv[sys.argv.index('--wait-for') + 1])\n"
            "    deadline = time.monotonic() + 15\n"
            "    while not gate.exists():\n"
            "        if time.monotonic() > deadline: raise TimeoutError('gate not opened')\n"
            "        time.sleep(0.01)\n"
            "if '--expect-empty' in sys.argv:\n"
            "    assert sys.argv[-1] == '', repr(sys.argv)\n"
            "    event['empty_argument_received'] = True\n"
            "with open(sys.argv[1], 'a', encoding='utf-8') as output:\n"
            "    output.write(json.dumps(event, ensure_ascii=False) + '\\n')\n",
            encoding="utf-8",
        )
        self.destination = command_line(sys.executable, self.receiver, self.events_file)

    def read_events(self):
        if not self.events_file.exists():
            return []
        # Invalid or truncated JSON is a test failure, never silently ignored.
        return [json.loads(line) for line in self.events_file.read_text(encoding="utf-8").splitlines()]

    def run_sql(
        self,
        statements,
        *,
        types=("query_begin", "query_end"),
        async_mode=False,
        session="integration-雪",
        destination=None,
        returncode=0
    ):
        extension = EXTENSION or DUCKDB.parent / "extension/events/events.duckdb_extension"
        if not extension.exists() and EXTENSION is None:
            # The CI artifact test jobs restore binaries plus a local repository.
            candidates = list((DUCKDB.parent / "repository").glob("**/events.duckdb_extension"))
            self.assertEqual(len(candidates), 1, "Expected one events extension in the CI repository")
            extension = candidates[0]
        sql = [
            "LOAD " + sql_string(extension) + ";",
            "SET events_types = [" + ", ".join(map(sql_string, types)) + "];",
            "SET events_async = " + str(async_mode).lower() + ";",
            "SET events_session_name = " + sql_string(session) + ";",
            "SET events_destination = " + sql_string(destination or self.destination) + ";",
            *statements,
            "SET events_destination = '';",
        ]
        env = dict(os.environ, QUERY_FARM_TELEMETRY_OPT_OUT="1", PYTHONUTF8="1")
        # Files avoid waiting for asynchronous receivers that inherit stdout/stderr.
        with tempfile.TemporaryFile() as stdout, tempfile.TemporaryFile() as stderr:
            result = subprocess.run(
                [str(DUCKDB), "-unsigned", "-no-agent", "-batch", "-init", os.devnull, "-csv", "-noheader"],
                input="\n".join(sql).encode("utf-8"),
                stdout=stdout,
                stderr=stderr,
                env=env,
                timeout=10,
            )
            stdout.seek(0)
            stderr.seek(0)
            output = stdout.read().decode("utf-8")
            errors = stderr.read().decode("utf-8")
        self.assertEqual(result.returncode, returncode, errors)
        return output, errors

    def test_query_correlation_and_common_fields(self):
        self.run_sql(["SELECT 42;", "SELECT 84;"])
        events = self.read_events()
        begins = [e for e in events if e["event"] == "query_begin"]
        ends = [e for e in events if e["event"] == "query_end"]
        self.assertEqual(len(begins), 3)  # Two SELECTs plus disabling the destination.
        self.assertEqual(len(ends), 3)  # Enabling the destination plus two SELECTs.
        self.assertEqual(len({e["query_id"] for e in ends}), 3)
        self.assertEqual(len({e["query_id"] for e in begins} & {e["query_id"] for e in ends}), 2)
        for event in events:
            self.assertEqual(event["session_name"], "integration-雪")
            self.assertGreater(event["process_id"], 0)
            self.assertGreaterEqual(event["connection_id"], 0)
            self.assertRegex(event["timestamp"], r"^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}Z$")
            self.assertIn("database_path", event)
            self.assertIn("attached_databases", event)
        self.assertTrue(all(not e["has_error"] for e in ends))

    def test_error_has_one_completion(self):
        _, stderr = self.run_sql(["SELECT * FROM events_missing_table;"], returncode=1)
        self.assertIn("events_missing_table", stderr)
        ends = [e for e in self.read_events() if e["event"] == "query_end"]
        failures = [e for e in ends if e["has_error"]]
        self.assertEqual(len(failures), 1)
        self.assertEqual(failures[0]["error_type"], "Catalog")
        self.assertEqual(len(ends), len({e["query_id"] for e in ends}))

    def test_rollback_emitted_once(self):
        self.run_sql(["BEGIN;", "SELECT 1;", "ROLLBACK;"], types=("transaction_rollback",))
        events = self.read_events()
        self.assertEqual(len(events), 1)
        self.assertFalse(events[0]["has_error"])
        self.assertGreater(events[0]["transaction_id"], 0)

    def test_prepared_parameters_and_planning_error(self):
        self.run_sql(
            [
                "PREPARE e AS SELECT $value::INTEGER;",
                "EXECUTE e(value := 42);",
                "EXECUTE e(value := 84);",
                "SELECT * FROM events_missing_table;",
            ],
            types=("execute_prepared", "rebind_prepared_statement", "planning_error", "finalize_prepare"),
            returncode=1,
        )
        events = self.read_events()
        for event_type in ("execute_prepared", "rebind_prepared_statement"):
            matches = [e for e in events if e["event"] == event_type]
            self.assertEqual([e["parameters"]["value"] for e in matches], ["42", "84"])
        self.assertEqual(sum(e["event"] == "planning_error" for e in events), 1)
        self.assertTrue(any(e["event"] == "finalize_prepare" for e in events))

    def test_large_unicode_payload(self):
        session = 'large-雪-"\\\n' * 20000
        self.run_sql(["SELECT 42;"], types=("query_end",), session=session)
        events = self.read_events()
        self.assertEqual(len(events), 2)
        self.assertTrue(all(e["session_name"] == session for e in events))

    @unittest.skipIf(os.name == "nt", "POSIX command tokenization")
    def test_quoted_paths_and_empty_argument(self):
        destination = command_line(sys.executable, self.receiver, self.events_file, "--expect-empty", "")
        self.run_sql(["SELECT 42;"], types=("query_end",), destination=destination)
        events = self.read_events()
        self.assertEqual(len(events), 2)
        self.assertTrue(all(e["empty_argument_received"] for e in events))

    def test_early_receiver_exit_preserves_queries(self):
        destination = command_line(sys.executable, "-c", "import sys; sys.exit(0)")
        stdout, _ = self.run_sql(["SELECT 42;"], session="x" * 200000, destination=destination)
        self.assertEqual(stdout.strip(), "42")

    def test_missing_receiver_preserves_queries(self):
        stdout, _ = self.run_sql(
            ["SELECT 42;"], session="x" * 200000, destination=command_line(self.path / "missing executable")
        )
        self.assertEqual(stdout.strip(), "42")

    def test_async_delivery_does_not_wait_for_receiver_exit(self):
        gate = self.path / "release receivers"
        destination = command_line(sys.executable, self.receiver, self.events_file, "--wait-for", gate)
        try:
            self.run_sql(["SELECT 42;", "SELECT 84;"], types=("query_end",), async_mode=True, destination=destination)
        finally:
            gate.touch()
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            if len(self.read_events()) == 3:
                break
            time.sleep(0.02)
        self.assertEqual(len(self.read_events()), 3)

    def test_external_access_disables_existing_destination(self):
        self.run_sql(["SELECT 42;", "SET enable_external_access = false;", "SELECT 84;"], types=("query_end",))
        self.assertEqual(len(self.read_events()), 2)

    def test_external_access_rejects_new_destination(self):
        _, stderr = self.run_sql(
            [
                "SET enable_external_access = false;",
                "SET events_destination = " + sql_string(self.destination) + ";",
            ],
            returncode=1,
        )
        self.assertIn("requires enable_external_access=true", stderr)

    @unittest.skipIf(os.name == "nt", "POSIX command tokenization")
    def test_malformed_command_is_rejected(self):
        for destination in ("'unfinished", "python3 \\", "   "):
            with self.subTest(destination=destination):
                _, stderr = self.run_sql([], destination=destination, returncode=1)
                self.assertIn("events_destination", stderr)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--duckdb", type=Path, default=DUCKDB)
    parser.add_argument("--extension", type=Path)
    args, remaining = parser.parse_known_args()
    DUCKDB = args.duckdb.resolve()
    if not DUCKDB.exists() and DUCKDB.with_suffix(".exe").exists():
        DUCKDB = DUCKDB.with_suffix(".exe")
    EXTENSION = args.extension.resolve() if args.extension else None
    unittest.main(argv=[sys.argv[0], *remaining])
