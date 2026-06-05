#!/bin/bash
# Comparative benchmark: csv-parser vs known CSV parsers
# Compares: our parser (1/4/8 workers), libcsv (C), xsv (Rust), mlr (Go),
#           Python csv, cut+awk (Unix baseline)
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$SCRIPT_DIR"

RESULTS_FILE="$SCRIPT_DIR/compare_results.txt"
NRUNS_SMALL=200    # for files < 1MB
NRUNS_LARGE=50     # for files >= 1MB

echo "=== Comparative CSV Parser Benchmark ==="
echo "Date: $(date -Iseconds)"
echo "CPU:  $(grep 'model name' /proc/cpuinfo | head -1 | cut -d: -f2 | xargs)"
echo "Cores: $(nproc)"
echo ""

# --- Compile parsers ---
echo "=== Compiling ==="
gcc -O2 -DBENCHMARK -pthread -I"$ROOT_DIR/include" -o fsm_scalar_cmp \
    "$ROOT_DIR/src/fsm/parser.c" "$ROOT_DIR/src/csv_common.c"
gcc -O2 -DBENCHMARK -pthread -I"$ROOT_DIR/include" -o fsm_simd_cmp \
    "$ROOT_DIR/src/fsm/parser-simd.c" "$ROOT_DIR/src/csv_common.c"
gcc -O2 -DBENCHMARK -pthread -I"$ROOT_DIR/include" -o control_scalar_cmp \
    "$ROOT_DIR/src/control/parser.c" "$ROOT_DIR/src/csv_common.c"
gcc -O2 -DBENCHMARK -pthread -I"$ROOT_DIR/include" -o control_simd_cmp \
    "$ROOT_DIR/src/control/parser-simd.c" "$ROOT_DIR/src/csv_common.c"
gcc -O2 -DBENCHMARK -pthread -I"$ROOT_DIR/include" -o lut_scalar_cmp \
    "$ROOT_DIR/src/lut/parser.c" "$ROOT_DIR/src/csv_common.c" "$ROOT_DIR/src/csv_lut.c"
gcc -O2 -DBENCHMARK -pthread -I"$ROOT_DIR/include" -o lut_simd_cmp \
    "$ROOT_DIR/src/lut/parser-simd.c" "$ROOT_DIR/src/csv_common.c" "$ROOT_DIR/src/csv_lut.c"
gcc -O2 -o libcsv_bench libcsv_bench.c -lcsv
echo "Done"
echo ""

# --- Generate test files if not present ---
for f in bench_1k.csv bench_10k.csv bench_100k.csv; do
    [ -f "$f" ] || python3 gen_csv.py
done

# --- Results header ---
> "$RESULTS_FILE"
cat >> "$RESULTS_FILE" << HEADER
# Comparative CSV parser benchmark
# date: $(date -Iseconds)
# cpu: $(grep 'model name' /proc/cpuinfo | head -1 | cut -d: -f2 | xargs)
# cores: $(nproc)
# format: file\tparser\tbest_s\tavg_s\tfile_size_bytes
HEADER

# --- Benchmark function: runs N times, reports best + avg ---
run_bench() {
    local name="$1"
    local cmd="$2"
    local nruns="$3"
    local fname="$4"

    local best=""
    local total="0"

    for i in $(seq 1 "$nruns"); do
        secs=$( { time eval "$cmd" > /dev/null 2>&1; } 2>&1 \
            | grep real | awk '{print $2}' | sed 's/m/*60+/;s/s//' | bc -l)
        total=$(echo "$total + $secs" | bc -l)
        if [ -z "$best" ] || (( $(echo "$secs < $best" | bc -l) )); then
            best=$secs
        fi
    done

    local avg=$(echo "$total / $nruns" | bc -l)
    local fsize=$(stat -c%s "$fname")
    printf "  %-24s best: %8.4fs  avg: %8.4fs\n" "$name" "$best" "$avg"
    printf '%s\t%s\t%.6f\t%.6f\t%s\n' "$fname" "$name" "$best" "$avg" "$fsize" >> "$RESULTS_FILE"
}

echo "=== Running benchmarks ==="

for f in bench_1k.csv bench_10k.csv bench_100k.csv; do
    fsize=$(du -h "$f" | cut -f1)
    rows=$(wc -l < "$f")
    file_bytes=$(stat -c%s "$f")

    # Select iteration count based on file size
    nruns=$NRUNS_SMALL
    [ "$file_bytes" -gt 1000000 ] && nruns=$NRUNS_LARGE

    printf "\n--- %s (%s rows, %s, %d runs) ---\n" "$f" "$rows" "$fsize" "$nruns"

    # FSM parser variants
    run_bench "fsm-scalar (1w)"        "./fsm_scalar_cmp '$f' 1" "$nruns" "$f"
    run_bench "fsm-scalar (4w)"        "./fsm_scalar_cmp '$f' 4" "$nruns" "$f"
    run_bench "fsm-scalar (8w)"        "./fsm_scalar_cmp '$f' 8" "$nruns" "$f"
    run_bench "fsm-simd (1w)"          "./fsm_simd_cmp '$f' 1"   "$nruns" "$f"
    run_bench "fsm-simd (4w)"          "./fsm_simd_cmp '$f' 4"   "$nruns" "$f"
    run_bench "fsm-simd (8w)"          "./fsm_simd_cmp '$f' 8"   "$nruns" "$f"

    # Control parser variants
    run_bench "control-scalar (1w)"    "./control_scalar_cmp '$f' 1" "$nruns" "$f"
    run_bench "control-scalar (4w)"    "./control_scalar_cmp '$f' 4" "$nruns" "$f"
    run_bench "control-scalar (8w)"    "./control_scalar_cmp '$f' 8" "$nruns" "$f"
    run_bench "control-simd (1w)"      "./control_simd_cmp '$f' 1"   "$nruns" "$f"
    run_bench "control-simd (4w)"      "./control_simd_cmp '$f' 4"   "$nruns" "$f"
    run_bench "control-simd (8w)"      "./control_simd_cmp '$f' 8"   "$nruns" "$f"

    # LUT parser variants
    run_bench "lut-scalar (1w)"        "./lut_scalar_cmp '$f' 1" "$nruns" "$f"
    run_bench "lut-scalar (4w)"        "./lut_scalar_cmp '$f' 4" "$nruns" "$f"
    run_bench "lut-scalar (8w)"        "./lut_scalar_cmp '$f' 8" "$nruns" "$f"
    run_bench "lut-simd (1w)"          "./lut_simd_cmp '$f' 1"   "$nruns" "$f"
    run_bench "lut-simd (4w)"          "./lut_simd_cmp '$f' 4"   "$nruns" "$f"
    run_bench "lut-simd (8w)"          "./lut_simd_cmp '$f' 8"   "$nruns" "$f"

    # libcsv (C library)
    run_bench "libcsv (C)"             "./libcsv_bench '$f'" "$nruns" "$f"

    # xsv (Rust) — count rows (forces full parse)
    run_bench "xsv count (Rust)"       "xsv count '$f'"      "$nruns" "$f"

    # xsv stats — computes per-column stats (heavier workload)
    run_bench "xsv stats (Rust)"       "xsv stats '$f'"      "$nruns" "$f"

    # miller — count rows
    run_bench "mlr count (Go)"         "mlr --csv count-distinct -f '' '$f'" "$nruns" "$f"

    # Python csv module
    run_bench "Python csv"             "python3 python_bench.py '$f'" "$nruns" "$f"

    # Unix cut (baseline — splits only, no quote handling)
    run_bench "cut -d, (Unix)"         "cut -d, -f1 '$f'"    "$nruns" "$f"
done

echo ""
echo "Results written to: $RESULTS_FILE"

# --- Generate comparison report ---
python3 - "$RESULTS_FILE" << 'PYEOF'
import sys, os
from collections import defaultdict

path = sys.argv[1]
out_dir = os.path.dirname(path)

records = []
meta = {}
with open(path) as f:
    for line in f:
        line = line.strip()
        if line.startswith("# date:"):
            meta["date"] = line.split(":", 1)[1].strip()
        if line.startswith("# cpu:"):
            meta["cpu"] = line.split(":", 1)[1].strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split("\t")
        if len(parts) < 5:
            continue
        records.append({
            "file": parts[0],
            "parser": parts[1],
            "best_s": float(parts[2]),
            "avg_s": float(parts[3]),
            "size": int(parts[4]),
        })

if not records:
    print("No records found")
    sys.exit(1)

by_file = defaultdict(list)
for r in records:
    by_file[r["file"]].append(r)

report_path = os.path.join(out_dir, "compare_report.txt")
with open(report_path, "w") as f:
    f.write("=" * 80 + "\n")
    f.write("  CSV Parser Comparison Benchmark\n")
    if "date" in meta:
        f.write(f"  Date: {meta['date']}\n")
    if "cpu" in meta:
        f.write(f"  CPU:  {meta['cpu']}\n")
    f.write("=" * 80 + "\n")

    for fname in ["bench_1k.csv", "bench_10k.csv", "bench_100k.csv"]:
        if fname not in by_file:
            continue
        recs = by_file[fname]
        size_mb = recs[0]["size"] / (1024 * 1024)

        f.write(f"\n--- {fname} ({size_mb:.1f} MB) ---\n")
        f.write(f"{'Parser':<28} {'Best':>10} {'Avg':>10} {'Speedup':>10}\n")
        f.write("-" * 60 + "\n")

        # Speedup relative to slowest (slowest=1.00x, fastest=highest)
        slowest_best = max(r["best_s"] for r in recs)

        # Sort by best time (fastest first)
        recs_sorted = sorted(recs, key=lambda r: r["best_s"])
        for r in recs_sorted:
            sp = slowest_best / r["best_s"] if r["best_s"] > 0 else 0
            marker = " <-- fastest" if r == recs_sorted[0] else ""
            f.write(f"{r['parser']:<28} {r['best_s']:>9.4f}s {r['avg_s']:>9.4f}s "
                    f"{sp:>9.2f}x{marker}\n")
        f.write("-" * 60 + "\n")

        # Throughput
        f.write("\n  Throughput (MB/s, best time):\n")
        for r in recs_sorted:
            tp = size_mb / r["best_s"] if r["best_s"] > 0 else 0
            f.write(f"    {r['parser']:<28} {tp:>8.1f} MB/s\n")
        f.write("\n")

    f.write("=" * 80 + "\n")
    f.write("  Notes:\n")
    f.write("  - fsm-*: computed goto FSM + mmap + pthreads\n")
    f.write("  - control-*: if/while + likely/unlikely + mmap + pthreads\n")
    f.write("  - lut-*: lookup-table class dispatch + mmap + pthreads\n")
    f.write("  - *-simd: SSE2-accelerated parsing (16 bytes/cycle)\n")
    f.write("  - libcsv: C library (libcsv3), fread-based, single-threaded\n")
    f.write("  - xsv: BurntSushi's Rust CSV toolkit (single-threaded)\n")
    f.write("  - mlr: Miller, Go-based CSV processor\n")
    f.write("  - Python csv: stdlib csv.reader\n")
    f.write("  - cut: Unix coreutils (no quote handling, baseline)\n")
    f.write("  - 'Speedup' is relative to the fastest parser (1.00x)\n")
    f.write("=" * 80 + "\n")

print(f"\n  Comparison report: {report_path}")
with open(report_path) as f:
    print(f.read())
PYEOF

rm -f fsm_scalar_cmp fsm_simd_cmp control_scalar_cmp control_simd_cmp
rm -f lut_scalar_cmp lut_simd_cmp libcsv_bench
