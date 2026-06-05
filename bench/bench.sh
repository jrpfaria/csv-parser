#!/bin/bash
# Benchmark both scalar and SIMD parsers across generated files
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
gcc -O2 -DBENCHMARK -pthread -I"$ROOT_DIR/include" -o fsm_scalar_bench \
    "$ROOT_DIR/src/fsm/parser.c" "$ROOT_DIR/src/csv_common.c"
gcc -O2 -DBENCHMARK -pthread -I"$ROOT_DIR/include" -o fsm_simd_bench \
    "$ROOT_DIR/src/fsm/parser-simd.c" "$ROOT_DIR/src/csv_common.c"
gcc -O2 -DBENCHMARK -pthread -I"$ROOT_DIR/include" -o control_scalar_bench \
    "$ROOT_DIR/src/control/parser.c" "$ROOT_DIR/src/csv_common.c"
gcc -O2 -DBENCHMARK -pthread -I"$ROOT_DIR/include" -o control_simd_bench \
    "$ROOT_DIR/src/control/parser-simd.c" "$ROOT_DIR/src/csv_common.c"
gcc -O2 -DBENCHMARK -pthread -I"$ROOT_DIR/include" -o lut_scalar_bench \
    "$ROOT_DIR/src/lut/parser.c" "$ROOT_DIR/src/csv_common.c" "$ROOT_DIR/src/csv_lut.c"
gcc -O2 -DBENCHMARK -pthread -I"$ROOT_DIR/include" -o lut_simd_bench \
    "$ROOT_DIR/src/lut/parser-simd.c" "$ROOT_DIR/src/csv_common.c" "$ROOT_DIR/src/csv_lut.c"

gcc -O2 -pthread -I"$ROOT_DIR/include" -o fsm_scalar_timing \
    "$ROOT_DIR/src/fsm/parser.c" "$ROOT_DIR/src/csv_common.c"
gcc -O2 -pthread -I"$ROOT_DIR/include" -o fsm_simd_timing \
    "$ROOT_DIR/src/fsm/parser-simd.c" "$ROOT_DIR/src/csv_common.c"
gcc -O2 -pthread -I"$ROOT_DIR/include" -o control_scalar_timing \
    "$ROOT_DIR/src/control/parser.c" "$ROOT_DIR/src/csv_common.c"
gcc -O2 -pthread -I"$ROOT_DIR/include" -o control_simd_timing \
    "$ROOT_DIR/src/control/parser-simd.c" "$ROOT_DIR/src/csv_common.c"
gcc -O2 -pthread -I"$ROOT_DIR/include" -o lut_scalar_timing \
    "$ROOT_DIR/src/lut/parser.c" "$ROOT_DIR/src/csv_common.c" "$ROOT_DIR/src/csv_lut.c"
gcc -O2 -pthread -I"$ROOT_DIR/include" -o lut_simd_timing \
    "$ROOT_DIR/src/lut/parser-simd.c" "$ROOT_DIR/src/csv_common.c" "$ROOT_DIR/src/csv_lut.c"
echo "Done"

# Clear results file
> "$RESULTS_FILE"
cat >> "$RESULTS_FILE" << HEADER
# csv-parser benchmark results
# date: $(date -Iseconds)
# format: file,parser,workers,mode,best_wall_s,avg_wall_s,mmap_s,scan_s,dispatch_s,parse_s,total_s
HEADER

echo ""
echo "=== Benchmarks (scaled runs, best & avg of N) ==="

for f in bench_100.csv bench_1k.csv bench_10k.csv bench_100k.csv; do
    rows=$(wc -l < "$f")
    size=$(du -h "$f" | cut -f1)

    case "$f" in
        bench_100.csv)   NRUNS=1000 ;;
        bench_1k.csv)    NRUNS=500  ;;
        bench_10k.csv)   NRUNS=200  ;;
        bench_100k.csv)  NRUNS=100  ;;
        *)               NRUNS=100  ;;
    esac

    printf "\n--- %s (%s lines, %s) — %d runs ---\n" "$f" "$rows" "$size" "$NRUNS"

    for parser_info in \
        "fsm-scalar:fsm_scalar_bench:fsm_scalar_timing" \
        "fsm-simd:fsm_simd_bench:fsm_simd_timing" \
        "control-scalar:control_scalar_bench:control_scalar_timing" \
        "control-simd:control_simd_bench:control_simd_timing" \
        "lut-scalar:lut_scalar_bench:lut_scalar_timing" \
        "lut-simd:lut_simd_bench:lut_simd_timing"; do
        IFS=: read -r plabel pbench ptiming <<< "$parser_info"

        for w in 1 2 4 6 8; do
            mode_name="single"
            [ "$w" = "2" ] && mode_name="dist-as-worker"
            [ "$w" -ge "3" ] && mode_name="dist+slaves($w)"

            printf "  [%s w=%d] " "$plabel" "$w"
            best=""
            total_secs="0"
            for i in $(seq 1 "$NRUNS"); do
                t=$( { time "./$pbench" "$f" "$w" > /dev/null; } 2>&1 | grep real | awk '{print $2}')
                secs=$(echo "$t" | sed 's/m/*60+/;s/s//' | bc -l)
                total_secs=$(echo "$total_secs + $secs" | bc -l)
                if [ -z "$best" ] || (( $(echo "$secs < $best" | bc -l) )); then
                    best=$secs
                fi
            done
            avg=$(echo "$total_secs / $NRUNS" | bc -l)
            printf "best: %.4fs  avg: %.4fs\n" "$best" "$avg"

            timing=$("./$ptiming" "$f" "$w" 2>&1 | grep -E '^\s+(mmap|scan|dispatch|parse|total):' | awk '{print $2}' | sed 's/s//' | tr '\n' ',')
            echo "${f},${plabel},${w},${mode_name},$(printf '%.6f' "$best"),$(printf '%.6f' "$avg"),${timing%,}" >> "$RESULTS_FILE"
        done
    done
done

echo ""
echo "Results written to: $RESULTS_FILE"
rm -f fsm_scalar_timing fsm_simd_timing control_scalar_timing control_simd_timing
rm -f lut_scalar_timing lut_simd_timing
