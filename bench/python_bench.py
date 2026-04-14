#!/usr/bin/env python3
"""
Benchmark wrapper: reads a CSV using Python's csv module and counts rows.
Usage: python3 python_bench.py <file>
"""
import csv
import sys
import time

def main():
    path = sys.argv[1]
    t0 = time.monotonic()
    row_count = 0
    field_count = 0
    with open(path, newline='') as f:
        reader = csv.reader(f)
        for row in reader:
            row_count += 1
            field_count += len(row)
    elapsed = time.monotonic() - t0
    print(f"{row_count} rows, {field_count} fields in {elapsed:.6f}s")

if __name__ == "__main__":
    main()
