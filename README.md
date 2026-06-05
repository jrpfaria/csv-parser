# csv-parser

A high-performance CSV parser written in C, organized around three parsing approaches:

- `fsm`: computed `goto` finite-state machine dispatch
- `control`: conventional `if`/`while` control flow with branch hints
- `lut`: lookup-table character classification layered on top of the current parser structure

Each approach ships in both scalar and SIMD variants, for a total of six binaries. Hot-path memory stays on flat static buffers and per-worker arenas, and all parallel modes avoid locks by writing to disjoint output regions.

## Variants

| Binary | Approach | Scalar/SIMD | Core idea |
|---|---|---|---|
| `fsm-parser` | FSM | Scalar | Computed `goto` dispatch on per-byte character classes |
| `fsm-parser-simd` | FSM | SIMD | Computed `goto` structure with SSE2 bulk scanning |
| `control-parser` | Control | Scalar | Conventional branches with `likely`/`unlikely` |
| `control-parser-simd` | Control | SIMD | Branch-friendly SSE2 parser |
| `lut-parser` | LUT | Scalar | Table-driven character classification |
| `lut-parser-simd` | LUT | SIMD | LUT classification plus SSE2 scanning |

## Approach Summary

| Approach | Pros | Cons |
|---|---|---|
| `fsm` | Explicit FSM, direct mapping from parser states to code labels, good as a control-flow case study | Weakest performer on this x86 target, especially in scalar mode; indirect jumps are harder for the branch predictor |
| `control` | Best general-purpose baseline, simple to follow, consistently strong scalar and SIMD results | Scalar path still pays the row-scan/distribution cost; less "novel" than the FSM version |
| `lut` | Clean character classification, easiest way to swap delimiter/qualifier/newline handling, fastest large-file SIMD result here | Adds an extra table lookup and more moving parts; scalar gains over control are modest |

## Architecture

### Scalar path

The scalar parsers use a distributor/worker model:

1. `mmap()` the file.
2. Scan row boundaries and cell offsets.
3. Split rows across workers.
4. Parse each worker slice into a shared `csv_result_t` with disjoint writes.

This path is used by:

- `fsm-parser`
- `control-parser`
- `lut-parser`

### SIMD path

The SIMD parsers avoid the full-file scan bottleneck:

1. `mmap()` the file.
2. Split the input into byte ranges.
3. Use SSE2 newline finding to align boundaries.
4. Parse independently per worker.
5. Compact private worker regions back into contiguous output.

This path is used by:

- `fsm-parser-simd`
- `control-parser-simd`
- `lut-parser-simd`

### LUT classification

The LUT implementation centralizes character-class setup in:

- `include/csv_lut.h`
- `src/csv_lut.c`

The default table is defined with designated initializers in a fixed `255`-entry array so delimiter, qualifier, and newline mappings stay explicit and easy to reconfigure.

## Directory Structure

```text
csv-parser/
├── Makefile
├── README.md
├── requirements.txt
├── include/
│   ├── csv_common.h
│   └── csv_lut.h
├── src/
│   ├── csv_common.c
│   ├── csv_lut.c
│   ├── control/
│   │   ├── parser.c
│   │   └── parser-simd.c
│   ├── fsm/
│   │   ├── parser.c
│   │   └── parser-simd.c
│   └── lut/
│       ├── parser.c
│       └── parser-simd.c
├── csv/
│   └── test.csv
├── tests/
│   └── run_tests.sh
└── bench/
    ├── bench.sh
    ├── compare.sh
    ├── gen_csv.py
    ├── libcsv_bench.c
    ├── python_bench.py
    └── report.py
```

## Configurable Limits

All fixed-size limits live in `include/csv_common.h`.

| Define | Default | Purpose |
|---|---|---|
| `MAX_WORD_LEN` | 256 | Maximum characters per cell |
| `ARENA_SIZE` | 32 MB | Arena size per worker |
| `MAX_COLS` | 64 | Maximum columns per row |
| `MAX_ROWS` | 256K | Maximum total rows |
| `MAX_WORKERS` | 8 | Maximum worker threads |

## Building

```bash
make
make fsm-parser
make fsm-parser-simd
make control-parser
make control-parser-simd
make lut-parser
make lut-parser-simd
```

Requires GCC with computed `goto` support and SSE2 support.

## Usage

```bash
./fsm-parser file.csv
./fsm-parser-simd file.csv 8

./control-parser file.csv
./control-parser-simd file.csv 8

./lut-parser file.csv
./lut-parser-simd file.csv 8
```

## Testing

```bash
make test
```

The test suite now runs 540 checks total:

- 6 parsers
- 17 fixture/crash cases
- 5 worker configurations: `1`, `2`, `4`, `6`, `8`
- 30 additional row-count validations

## Benchmarks

### Generate benchmark data

```bash
make bench
make report
make graph
make compare
```

Generated outputs:

- `bench/bench_results.txt`
- `bench/bench_report.txt`
- `bench/bench_report.csv`
- `bench/bench_phases.csv`
- `bench/bench_report.pdf`
- `bench/compare_results.txt`
- `bench/compare_report.txt`

### Internal comparison: LUT vs Control vs FSM

The table below uses the 100K-row benchmark sweep from `bench/bench_report.txt` and shows best wall-clock time in milliseconds.

#### Scalar

| Approach | 1w | 2w | 4w | 6w | 8w |
|---|---|---|---|---|---|
| `fsm-scalar` | 91 | 101 | 85 | 75 | 68 |
| `control-scalar` | 66 | 86 | 70 | 60 | 61 |
| `lut-scalar` | 62 | 81 | 73 | 66 | 62 |

#### SIMD

| Approach | 1w | 2w | 4w | 6w | 8w |
|---|---|---|---|---|---|
| `fsm-simd` | 40 | 29 | 25 | 22 | 20 |
| `control-simd` | 31 | 22 | 19 | 17 | 16 |
| `lut-simd` | 31 | 22 | 19 | 17 | 16 |

What this says in practice:

- `control` and `lut` dominate the scalar runs; `lut` is best at 1 worker, while `control` has the best 6-worker scalar point.
- `lut` matches or edges out `control` on the large-file runs and produces the best 8-worker comparative result.
- `fsm` remains useful as the "pure computed-goto" reference, but it is consistently behind the other two on this machine.

### Main comparative results

The 1K-file benchmark is mostly at the timer floor, so the most meaningful comparison is the 100K-row, 20.1 MB dataset on the Intel i7-1355U. To keep the table fair, this summary includes:

- the top overall entries
- the single-worker versions of each local approach
- a few external baselines

| Parser | Workers | Best | Throughput |
|---|---|---|---|
| `lut-simd` | 8 | 0.016s | 1258.3 MB/s |
| `control-simd` | 8 | 0.017s | 1184.3 MB/s |
| `fsm-simd` | 8 | 0.020s | 1006.6 MB/s |
| `control-simd` | 1 | 0.031s | 649.4 MB/s |
| `lut-simd` | 1 | 0.032s | 629.1 MB/s |
| `xsv count (Rust)` | 1 | 0.036s | 559.2 MB/s |
| `fsm-simd` | 1 | 0.042s | 479.3 MB/s |
| `libcsv (C)` | 1 | 0.062s | 324.7 MB/s |
| `control-scalar` | 1 | 0.063s | 319.6 MB/s |
| `lut-scalar` | 1 | 0.066s | 305.0 MB/s |
| `fsm-scalar` | 1 | 0.092s | 218.8 MB/s |
| `Python csv` | 1 | 0.158s | 127.4 MB/s |
| `mlr count (Go)` | 1 | 0.248s | 81.2 MB/s |

Observations:

- The fastest result in this repo is `lut-simd` at 8 workers.
- For fair single-threaded SIMD comparison, `control-simd` and `lut-simd` are effectively tied and both beat `fsm-simd`.
- For scalar single-worker parsing, `control-scalar` and `lut-scalar` are close, and both are well ahead of `fsm-scalar`.
- `xsv count` remains a strong external baseline, but the local SIMD parsers win clearly on the large file.

### Why the SIMD variants win

1. They scan 16 bytes at a time with SSE2 instead of classifying every byte in scalar code.
2. They bulk-copy long token runs rather than pushing one character at a time.
3. They avoid the scalar distributor scan bottleneck by partitioning work on byte ranges.
4. They scale well enough on this CPU to move into the memory-bandwidth-limited region on larger files.

## Purpose

This project is a performance case study more than a production CSV library. The point is to compare how control-flow style, character classification strategy, memory layout, and SIMD partitioning interact in one workload.

The progression in this repository is:

1. Finite-state-machine dispatch with computed `goto`
2. Flat buffers and arena allocation
3. `mmap()` plus threaded parsing
4. SIMD scanning and byte-range partitioning
5. Conventional branch-based and LUT-based rewrites for direct comparison

## Authorship

The original parser idea in this project is the CSV finite-state-machine design built around explicit delimiter, qualifier, and newline handling. The repository was then extended iteratively with arena allocation, threaded parsing, SIMD variants, the control-flow rewrite, the LUT classification layer, and the benchmark/reporting tooling.