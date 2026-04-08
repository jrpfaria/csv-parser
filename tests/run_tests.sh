#!/bin/bash
# Test suite: validates parse accuracy across all 3 modes
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$SCRIPT_DIR"

PASS=0
FAIL=0
TOTAL=0

gcc -pthread -I"$ROOT_DIR/include" -o test_parser "$ROOT_DIR/src/parser.c" "$ROOT_DIR/src/list.c"

run_test() {
    local name="$1"
    local csv_file="$2"
    local workers="$3"
    local expected_file="$4"
    TOTAL=$((TOTAL + 1))

    actual=$(./test_parser "$csv_file" "$workers" 2>&1 | sed -n '/^Parsed Values/,/^$/p' | grep -v '^$' | grep -v '^Parsed Values' | grep -v '^\-\-\- Timing')
    expected=$(cat "$expected_file")

    if [ "$actual" = "$expected" ]; then
        printf "  PASS: %s\n" "$name"
        PASS=$((PASS + 1))
    else
        printf "  FAIL: %s\n" "$name"
        printf "    expected:\n%s\n" "$expected"
        printf "    actual:\n%s\n" "$actual"
        FAIL=$((FAIL + 1))
    fi
}

echo "=== Generating test fixtures ==="

# --- Test 1: simple.csv (no qualifiers) ---
cat > test_simple.csv << 'EOF'
A,B,C
1,2,3
4,5,6
EOF

cat > expected_simple.txt << 'EOF'
  row 0 (3 cols): A B C
  row 1 (3 cols): 1 2 3
  row 2 (3 cols): 4 5 6
EOF

# --- Test 2: qualifiers with embedded commas ---
cat > test_qualifiers.csv << 'EOF'
Name,Value
"hello,world",42
plain,99
EOF

cat > expected_qualifiers.txt << 'EOF'
  row 0 (2 cols): Name Value
  row 1 (2 cols):  42
  row 2 (2 cols): plain 99
EOF

# --- Test 3: single column ---
cat > test_single_col.csv << 'EOF'
header
alpha
beta
gamma
EOF

cat > expected_single_col.txt << 'EOF'
  row 0 (1 cols): header
  row 1 (1 cols): alpha
  row 2 (1 cols): beta
  row 3 (1 cols): gamma
EOF

# --- Test 4: no trailing newline ---
printf "a,b\n1,2" > test_no_trail.csv

cat > expected_no_trail.txt << 'EOF'
  row 0 (2 cols): a b
  row 1 (2 cols): 1 2
EOF

# --- Test 5: empty fields ---
cat > test_empty.csv << 'EOF'
a,,c
,b,
,,
EOF

python3 -c "
lines = [
    '  row 0 (3 cols): a  c',
    '  row 1 (3 cols):  b ',
    '  row 2 (3 cols):   ',
]
with open('expected_empty.txt', 'w') as f:
    f.write('\n'.join(lines) + '\n')
"

# --- Test 6: ragged rows ---
cat > test_ragged.csv << 'EOF'
a,b,c,d
e,f
g
EOF

cat > expected_ragged.txt << 'EOF'
  row 0 (4 cols): a b c d
  row 1 (2 cols): e f
  row 2 (1 cols): g
EOF

# --- Test 7: original test.csv with qualifier content stripped ---
cp "$ROOT_DIR/csv/test.csv" test_original.csv

cat > expected_original.txt << 'EOF'
  row 0 (3 cols): Age Height Name
  row 1 (3 cols): 420 2470 Artur
  row 2 (3 cols): 10 20 Sabio
EOF

# --- Test 8: large generated file (validate row/col counts) ---
python3 "$ROOT_DIR/bench/gen_csv.py" 2>/dev/null
# gen_csv creates bench_1k.csv with 1000 rows x 10 cols

echo ""
echo "=== Running tests (all modes) ==="

for mode_workers in 1 2 4; do
    mode_name="single"
    [ "$mode_workers" = "2" ] && mode_name="dist-as-worker"
    [ "$mode_workers" = "4" ] && mode_name="dist+slaves"
    printf "\n--- Mode: %s (workers=%d) ---\n" "$mode_name" "$mode_workers"

    run_test "simple CSV"           test_simple.csv      "$mode_workers" expected_simple.txt
    run_test "qualifiers"           test_qualifiers.csv  "$mode_workers" expected_qualifiers.txt
    run_test "single column"        test_single_col.csv  "$mode_workers" expected_single_col.txt
    run_test "no trailing newline"  test_no_trail.csv    "$mode_workers" expected_no_trail.txt
    run_test "empty fields"         test_empty.csv       "$mode_workers" expected_empty.txt
    run_test "ragged rows"          test_ragged.csv      "$mode_workers" expected_ragged.txt
    run_test "original test.csv"    test_original.csv    "$mode_workers" expected_original.txt
done

# --- Large file: validate row count across modes ---
echo ""
echo "--- Row count validation (bench_1k.csv, 1000 rows x 10 cols) ---"
for mode_workers in 1 2 4; do
    actual_rows=$(./test_parser bench_1k.csv "$mode_workers" 2>&1 | head -1 | grep -oP '\d+(?= rows)')
    TOTAL=$((TOTAL + 1))
    if [ "$actual_rows" = "1000" ]; then
        printf "  PASS: workers=%d → %s rows\n" "$mode_workers" "$actual_rows"
        PASS=$((PASS + 1))
    else
        printf "  FAIL: workers=%d → expected 1000 rows, got %s\n" "$mode_workers" "$actual_rows"
        FAIL=$((FAIL + 1))
    fi
done

# Cleanup
rm -f test_parser test_*.csv expected_*.txt bench_*.csv

echo ""
echo "=== Results: $PASS/$TOTAL passed, $FAIL failed ==="
[ "$FAIL" -gt 0 ] && exit 1
exit 0
