#!/bin/bash
# Benchmark the CSV parser across generated files
# Outputs machine-readable results to bench_results.txt
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$SCRIPT_DIR"

RESULTS_FILE="$SCRIPT_DIR/bench_results.txt"

echo "=== Running tests first ==="
bash "$ROOT_DIR/tests/run_tests.sh"
echo ""

echo "=== Generating test CSVs ==="
python3 gen_csv.py

echo ""
echo "=== Compiling ==="
gcc -O2 -DBENCHMARK -pthread -I"$ROOT_DIR/include" -o parser_bench "$ROOT_DIR/src/parser.c" "$ROOT_DIR/src/list.c"
gcc -O2 -pthread -I"$ROOT_DIR/include" -o parser_timing "$ROOT_DIR/src/parser.c" "$ROOT_DIR/src/list.c"
echo "Done"

# Clear results file
> "$RESULTS_FILE"
echo "# csv-parser benchmark results" >> "$RESULTS_FILE"
echo "# date: $(date -Iseconds)" >> "$RESULTS_FILE"
echo "# format: file,workers,mode,best_wall_s,avg_wall_s,mmap_s,scan_s,dispatch_s,parse_s,total_s" >> "$RESULTS_FILE"

echo ""
echo "=== Benchmarks (scaled runs, best & avg of N) ==="
echo "=== Modes: 1=single, 2=dist-as-worker, 4/6/8=dist+slaves ==="

for f in bench_100.csv bench_1k.csv bench_10k.csv bench_100k.csv; do
    rows=$(wc -l < "$f")
    size=$(du -h "$f" | cut -f1)

    # Scale iterations: small files get more runs, large files fewer
    case "$f" in
        bench_100.csv)   NRUNS=1000 ;;
        bench_1k.csv)    NRUNS=500  ;;
        bench_10k.csv)   NRUNS=200  ;;
        bench_100k.csv)  NRUNS=100  ;;
        *)               NRUNS=100  ;;
    esac

    printf "\n--- %s (%s lines, %s) — %d runs ---\n" "$f" "$rows" "$size" "$NRUNS"

    for w in 1 2 4 6 8; do
        mode_name="single"
        [ "$w" = "2" ] && mode_name="dist-as-worker"
        [ "$w" -ge "3" ] && mode_name="dist+slaves($w)"

        printf "  [workers=%d] " "$w"
        best=""
        total_secs="0"
        for i in $(seq 1 "$NRUNS"); do
            t=$( { time ./parser_bench "$f" "$w" > /dev/null; } 2>&1 | grep real | awk '{print $2}')
            secs=$(echo "$t" | sed 's/m/*60+/;s/s//' | bc -l)
            total_secs=$(echo "$total_secs + $secs" | bc -l)
            if [ -z "$best" ] || (( $(echo "$secs < $best" | bc -l) )); then
                best=$secs
            fi
        done
        avg=$(echo "$total_secs / $NRUNS" | bc -l)
        printf "best: %.4fs  avg: %.4fs\n" "$best" "$avg"

        # get detailed timing from a single run
        timing=$(./parser_timing "$f" "$w" 2>&1 | grep -E '^\s+(mmap|scan|dispatch|parse|total):' | awk '{print $2}' | sed 's/s//' | tr '\n' ',')
        echo "${f},${w},${mode_name},$(printf '%.6f' "$best"),$(printf '%.6f' "$avg"),${timing%,}" >> "$RESULTS_FILE"
    done
done

echo ""
echo "Results written to: $RESULTS_FILE"
rm -f parser_timing
