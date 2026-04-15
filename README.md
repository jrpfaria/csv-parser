# csv-parser

A high-performance CSV parser written in C, exploring two fundamentally
different control flow strategies: computed `goto` jump tables vs conventional
`if`/`while` with compiler branch hints. All hot-path memory operations use
static flat buffers with zero heap allocation. Parsing is parallelized across
worker threads with no mutual exclusion.

Four parser variants are provided:

| Variant | Control Flow | Parse Strategy |
|---|---|---|
| **`goto-parser`** | Computed `goto` jump tables | Byte-at-a-time, 6-entry FSM |
| **`goto-parser-simd`** | Computed `goto` jump tables | SSE2 16-byte bulk scanning |
| **`branched-parser`** | `if`/`while` + `likely`/`unlikely` | Byte-at-a-time |
| **`branched-parser-simd`** | `if`/`while` + `likely`/`unlikely` | SSE2 16-byte bulk scanning |

## Architecture

### Scalar parser (`parser`)

```
┌──────────────────────────────────────────────────────┐
│                    Distributor                       │
│  1. mmap() the file                                  │
│  2. Single-pass scan: find row offsets + cell counts │
│  3. Assign disjoint row ranges to N workers          │
└────────┬──────────┬──────────┬──────────┬────────────┘
         │          │          │          │
    ┌────▼───┐ ┌────▼───┐ ┌────▼───┐ ┌────▼───┐
    │Worker 0│ │Worker 1│ │Worker 2│ │Worker 3│
    │arena[0]│ │arena[1]│ │arena[2]│ │arena[3]│
    └────┬───┘ └────┬───┘ └────┬───┘ └────┬───┘
         └──────────┴──────────┴──────────┘
                         │
             ┌───────────▼───────────┐
             │   csv_result_t        │
             │  (shared, no locks    │
             │   — disjoint writes)  |
             └───────────────────────┘
```

The scalar parser uses a 4x-unrolled `SCAN_BYTE` macro to find row boundaries
in the distributor, then dispatches pre-computed row ranges to workers. Each
worker runs a byte-at-a-time goto FSM that classifies characters via XOR +
bit-shift into a 6-entry jump table.

### SIMD parser (`parser-simd`)

```
┌────────────────────────────────────────────────────────────┐
│                  Byte-Range Partitioner                    │
│  1. mmap() the file                                        │
│  2. Split file into N equal byte ranges                    │
│  3. SSE2 find_next_newline() to align each boundary        │
│     (16 bytes/cycle — NO full-file scan)                   │
│  4. Each worker gets: {buf_ptr, byte_count, row_slot}      │
└────┬──────────┬──────────┬──────────┬──────────────────────┘
     │          │          │          │
┌────▼───┐ ┌────▼───┐ ┌────▼───┐ ┌────▼───┐
│Worker 0│ │Worker 1│ │Worker 2│ │Worker 3│
│ SSE2   │ │ SSE2   │ │ SSE2   │ │ SSE2   │
│ parse  │ │ parse  │ │ parse  │ │ parse  │
│arena[0]│ │arena[1]│ │arena[2]│ │arena[3]│
└────┬───┘ └────┬───┘ └────┬───┘ └────┬───┘
     └──────────┴──────────┴──────────┘
                      │
            ┌─────────▼─────────┐
            │   csv_result_t    │
            │  compact after    │
            │  join (memmove)   │
            └───────────────────┘
```

The SIMD parser eliminates the distributor's full-file scan entirely. Instead
of pre-scanning all row boundaries (which is a serial bottleneck), the file is
split into N equal byte ranges. Each worker finds the nearest newline to align
its start position using a fast SSE2 scan (16 bytes/cycle). Workers parse
independently into private row/cell slots, then results are compacted into
contiguous rows via `memmove` after all threads join.

**Modes:**
- **1 worker:** Direct SIMD parse, no scan, no threads.
- **2+ workers:** Byte-range split → SSE2 boundary alignment → parallel
  parse → compact.

### Directory Structure

```
csv-parser/
├── Makefile
├── README.md
├── requirements.txt              # matplotlib for PDF reports
├── include/
│   └── csv_common.h              # type definitions + configurable limits
├── src/
│   ├── csv_common.c              # word_push, word_flush, arena ops
│   ├── goto/
│   │   ├── parser.c              # scalar goto-FSM parser
│   │   └── parser-simd.c         # SSE2 goto parser
│   └── branched/
│       ├── parser.c              # scalar branched parser
│       └── parser-simd.c         # SSE2 branched parser
├── csv/
│   └── test.csv                  # small test file
├── tests/
│   └── run_tests.sh              # 360-test suite (4 parsers × 5 workers × 17+1 cases)
└── bench/
    ├── bench.sh                  # per-config benchmarks (best + avg)
    ├── compare.sh                # comparative benchmark vs external parsers
    ├── report.py                 # text + PDF report generator
    ├── gen_csv.py                # CSV file generator (4–32 char fields)
    ├── libcsv_bench.c            # C wrapper for libcsv benchmarking
    └── python_bench.py           # Python csv.reader benchmark wrapper
```

---

## Design Choices

### 1. Computed Goto FSM (no `if`/`switch`)

All branching uses GCC's computed `goto` extension (`&&label` +
`goto *table[index]`). The scalar parser FSM encodes character classes as
bit flags:

| Flag | Bit | Value |
|---|---|---|
| Token (default) | — | `0` |
| Qualifier (`"`) | 0 | `1` |
| Delimiter (`,`) | 1 | `2` |
| (qualifier + delimiter) | 0+1 | `3` |
| Newline (`\n`) | 2 | `4` |
| (qualifier + newline) | 0+2 | `5` |

The jump table at index `(is_delimiter | is_qualifier | is_endofline)` maps
directly to one of 6 labels. Character classification uses XOR + NOT:

```c
is_delimiter = (!(token ^ d)) << 1;   // bit 1
is_endofline = (!(token ^ nl)) << 2;  // bit 2
```

### 2. SSE2 SIMD Acceleration (`parser-simd`)

The SIMD parser replaces the byte-at-a-time FSM with two SSE2-accelerated
states:

- **`state_normal`**: `find_special()` loads 16 bytes via `_mm_loadu_si128`,
  compares against delimiter, qualifier, and newline simultaneously using
  `_mm_cmpeq_epi8`, ORs the three masks, then extracts a bitmask with
  `_mm_movemask_epi8`. `__builtin_ctz()` gives the offset of the first
  special character. All plain text before it is bulk-copied via
  `word_push_bulk()` (a single `memcpy`).

- **`state_in_qualifier`**: `find_qualifier()` scans 16 bytes at a time
  for only the quote character. Quoted content is also bulk-copied.
  RFC 4180 doubled-quote escaping (`""` → `"`) is handled at the transition.

This converts ~10 goto jumps per field (one per character) into 1 SIMD scan +
1 memcpy per field.

### 3. Byte-Range Partitioning (zero-scan threading)

The scalar parser requires a full-file scan phase to find row boundaries before
dispatching work. On a 20 MB file, this scan takes ~30ms — nearly as long as
the parse itself — creating a serial bottleneck that prevents scaling.

The SIMD parser eliminates this entirely:

1. Split the file into N equal byte ranges.
2. For each boundary, call `find_next_newline()` — a pure SSE2 scan for `\n`
   (processes 16 bytes per cycle). Cost: ~0.02ms total vs 30ms for the full
   scan.
3. Each worker gets a byte range and writes into a private region of
   `csv_result_t` (row index = `worker_id × rows_per_slot`).
4. After all workers join, a single-pass `memmove` compacts the results into
   contiguous rows.

### 4. Flat Buffers + Arena Allocator (zero malloc)

- **`word_buf_t`**: Fixed `char[256]` buffer. `O(1)` append, `O(1)` reset.
- **`arena_t`**: 32 MB bump allocator per worker. `word_flush()` = `memcpy` +
  bump. No `malloc`, no `free`, no fragmentation.
- **`csv_result_t`**: Flat `char*[MAX_ROWS × MAX_COLS]` with `cols[MAX_ROWS]`.

All overflow points use branchless saturation guards:
```c
w->len += (w->len < MAX_WORD_LEN - 1);            // word_push
cell_idx += (cell_idx < MAX_ROWS * MAX_COLS - 1); // cell overflow
r_idx += (r_idx < MAX_ROWS - 1);                  // row overflow
```

### 5. mmap + No Locks

The file is memory-mapped and accessed as `const char *`. Workers read from
non-overlapping byte ranges and write to non-overlapping result regions.
No mutex, no atomic, no lock — disjoint access by design.

---

## Configurable Limits

All limits are `#define`s in `include/csv_common.h`:

| Define | Default | Purpose |
|---|---|---|
| `MAX_WORD_LEN` | 256 | Maximum characters per cell |
| `ARENA_SIZE` | 32 MB | Bump allocator size per worker |
| `MAX_COLS` | 64 | Maximum columns per row |
| `MAX_ROWS` | 256K | Maximum total rows |
| `MAX_WORKERS` | 8 | Maximum worker threads |
| `NUM_WORKERS` | 4 | Default worker count |

---

## Building

```bash
make                       # builds all 4 variants
make goto-parser           # scalar goto-FSM only
make goto-parser-simd      # SSE2 goto parser
make branched-parser       # scalar branched parser
make branched-parser-simd  # SSE2 branched parser
```

Requires GCC with computed goto support and SSE2 (baseline for all x86-64).

## Usage

```bash
# Goto variants
./goto-parser file.csv
./goto-parser-simd file.csv 8

# Branched variants
./branched-parser file.csv
./branched-parser-simd file.csv 8
```

---

## Testing

```bash
make test
```

Runs 360 tests: 4 parsers × 17 test cases × 5 worker configurations (1, 2, 4, 6, 8),
plus 20 row-count validations. Includes correctness tests, edge cases, and bad CSV
handling (empty files, binary garbage, unclosed qualifiers, column/row overflow, etc.).

---

## Benchmarks

### Quick Benchmark

```bash
make bench        # per-config benchmark (all worker counts, best + avg)
make report       # text report
make graph        # PDF with speedup charts (requires matplotlib)
```

### Comparative Benchmark

```bash
make compare      # vs libcsv, xsv, Miller, Python csv, cut
```

Compares against:
- **libcsv** — C library (libcsv3), fread-based, single-threaded
- **xsv** — BurntSushi's Rust CSV toolkit (SIMD-optimized, single-threaded)
- **Miller (mlr)** — Go-based CSV processor
- **Python csv** — stdlib csv.reader
- **cut** — Unix coreutils (no quote handling, raw I/O baseline)

### Results (100K rows, 20 MB, Intel i7-1355U)

| Parser | Best | Throughput | Speedup |
|---|---|---|---|
| **branched-simd (8w)** | 0.013s | 1,603 MB/s | 19.31x |
| **branched-simd (4w)** | 0.015s | 1,387 MB/s | 16.73x |
| *goto-simd (8w)* | 0.019s | 1,095 MB/s | 13.21x |
| *goto-simd (4w)* | 0.023s | 904 MB/s | 10.91x |
| **branched-simd (1w)** | 0.028s | 743 MB/s | 8.96x |
| cut (Unix) | 0.031s | 649 MB/s | 8.10x |
| xsv count (Rust) | 0.033s | 610 MB/s | 7.61x |
| *goto-simd (1w)* | 0.041s | 507 MB/s | 6.12x |
| **branched-scalar (8w)** | 0.058s | 359 MB/s | 4.33x |
| **branched-scalar (1w)** | 0.061s | 341 MB/s | 4.11x |
| libcsv (C) | 0.062s | 325 MB/s | 4.05x |
| *goto-scalar (8w)* | 0.071s | 293 MB/s | 3.54x |
| *goto-scalar (1w)* | 0.090s | 231 MB/s | 2.79x |
| xsv stats (Rust) | 0.136s | 148 MB/s | 1.85x |
| Python csv | 0.153s | 132 MB/s | 1.64x |
| Miller (Go) | 0.251s | 80 MB/s | 1.00x |

Speedup is relative to the slowest parser (1.00x = Miller).

### What Makes the SIMD Parser Fast

1. **16 bytes/cycle scanning** instead of 1 byte/cycle classification.
2. **Bulk memcpy** of plain text instead of per-character `word_push`.
3. **Zero-scan threading** — byte-range partitioning eliminates the 30ms
   serial scan phase that bottlenecked the scalar parser's multi-worker modes.
4. **Near memory-bandwidth ceiling** at 1,603 MB/s with 8 workers (branched).

### Goto vs Branched Comparison (100K rows, `-O3`)

| Variant | 1w | 2w | 4w | 6w | 8w |
|---|---|---|---|---|---|
| goto-scalar | 89.6ms | 93.4ms | 82.6ms | 70.9ms | 70.8ms |
| **branched-scalar** | **61.2ms** | **75.6ms** | **65.4ms** | **62.8ms** | **57.5ms** |
| goto-simd | 41.1ms | 27.4ms | 22.7ms | 23.7ms | 18.5ms |
| **branched-simd** | **28.2ms** | **17.0ms** | **15.3ms** | **15.0ms** | **13.1ms** |

The branched versions consistently outperform the goto variants by ~1.3–1.5x.

**Why branched wins on modern x86:**
- `goto *table[index]` compiles to an indirect `jmp *%rax` — the CPU's branch
  target buffer has limited capacity for indirect targets.
- With `if`/`while`, the branch predictor achieves >99% accuracy because
  ~95% of CSV bytes are plain tokens (the hot path is trivially predictable).
- `likely`/`unlikely` hints give the compiler explicit layout guidance for
  fall-through vs taken branches.
- Structured control flow enables `-O3` optimizations (loop unrolling, block
  reordering, register allocation) that are opaque to the optimizer with
  indirect gotos.

**When goto would win:** On embedded platforms (Cortex-M, Xtensa) with simple
or no branch predictors, the goto FSM's flat-cost indirect jump would likely
match or beat the branched version, since there's no sophisticated predictor
to exploit.

---

## Purpose

This project is a **case study in computed `goto` and performance-driven C
development**. The goal was never to build a production CSV library — it was to
take a single language feature (GCC's `&&label` computed goto) and explore how
far it could be pushed as the sole control flow mechanism in a real parsing
workload.

The project was developed iteratively, with each phase designed to surface a
specific performance bottleneck and force a concrete solution:

1. **Linked lists + goto FSM** — Correct parsing, but per-character heap
   allocation dominated runtime. Exposed the cost of `malloc` in a hot loop.
2. **Flat buffers + arena allocator** — Eliminated heap allocation entirely.
   Showed that memory layout matters more than algorithmic cleverness.
3. **mmap + threaded workers** — Removed I/O overhead, parallelized parsing.
   Revealed that the serial scan phase was the scaling bottleneck.
4. **SIMD vectorization** — Replaced byte-at-a-time classification with
   16-byte bulk scanning. Demonstrated the gap between "fast scalar" and
   "hardware-accelerated."
5. **Byte-range partitioning** — Eliminated the scan phase entirely. Showed
   that architecture changes (how work is divided) can outweigh micro-
   optimizations (how fast each byte is processed).
6. **Goto vs branched comparison** — Rewrote both parsers using conventional
   `if`/`while` with `likely`/`unlikely` hints. Discovered that on modern
   x86 with deep pipelines and sophisticated branch predictors, structured
   control flow consistently outperforms computed gotos by ~1.3–1.5x —
   because the CPU predicts the hot path with >99% accuracy, while indirect
   jumps remain opaque to the BTB.

Each step produced measurable before/after benchmarks, making the tradeoffs
concrete rather than theoretical. The comparative benchmarks against
established parsers (xsv, libcsv, Miller) provide external reference points
for where this approach lands in practice.

## Authorship

The original work in this project is the **finite state machine design** for
CSV parsing — encoding delimiter, qualifier, and newline as bit-flag character
classes and using computed `goto` jump tables to eliminate all conditional
branching from the parse loop. That core FSM concept and the initial
linked-list-based parser were written by hand.

All subsequent optimizations — flat buffers, arena allocators, mmap, threaded
distribution, SIMD vectorization, byte-range partitioning, branched parser
variants, benchmarking infrastructure — were implemented with AI-assisted
development (GitHub Copilot). Architecture and design decisions directed by
the author; implementation assisted by LLM tooling.
