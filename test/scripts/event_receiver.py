#!/usr/bin/env python3
"""
Event receiver for DuckDB events extension.
Reads JSON event from stdin and appends it to a file.
"""

import sys
import os

def main():
    # Get output file from environment variable or use default
    output_file = os.environ.get('EVENTS_OUTPUT_FILE', '/tmp/duckdb_events.jsonl')

    # Read all input from stdin
    data = sys.stdin.read()

    if data.strip():
        # Append to output file (one JSON object per line - JSONL format)
        with open(output_file, 'a') as f:
            # Remove any newlines from the JSON and write as single line
            f.write(data.strip().replace('\n', '') + '\n')

    sys.exit(0)

if __name__ == '__main__':
    main()
