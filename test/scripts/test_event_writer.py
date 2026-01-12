#!/usr/bin/env python3
"""
Test event writer for DuckDB events extension SQL tests.
Reads JSON event from stdin and appends it to a specified file.

Usage: test_event_writer.py [--clear] <output_file>
  --clear: Truncate the file before writing (optional)
"""

import sys
import os

def main():
    args = sys.argv[1:]
    clear_file = False

    if '--clear' in args:
        clear_file = True
        args.remove('--clear')

    if len(args) < 1:
        print("Usage: test_event_writer.py [--clear] <output_file>", file=sys.stderr)
        sys.exit(1)

    output_file = args[0]

    # Read all input from stdin
    data = sys.stdin.read()

    if data.strip():
        # Write mode: 'w' to truncate if --clear, otherwise 'a' to append
        mode = 'w' if clear_file else 'a'
        with open(output_file, mode) as f:
            # Remove any newlines from the JSON and write as single line
            f.write(data.strip().replace('\n', '') + '\n')

    sys.exit(0)

if __name__ == '__main__':
    main()
