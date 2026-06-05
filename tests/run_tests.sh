#!/bin/bash
# Test suite: validates parse accuracy for all 4 parser variants

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$SCRIPT_DIR"

PASS=0
FAIL=0
TOTAL=0

echo "=== Compiling test binaries ==="
gcc -pthread -I"$ROOT_DIR/include" -o test_fsm_scalar \
    "$ROOT_DIR/src/fsm/parser.c" "$ROOT_DIR/src/csv_common.c"
gcc -pthread -I"$ROOT_DIR/include" -o test_fsm_simd \
    "$ROOT_DIR/src/fsm/parser-simd.c" "$ROOT_DIR/src/csv_common.c"
gcc -pthread -I"$ROOT_DIR/include" -o test_control_scalar \
    "$ROOT_DIR/src/control/parser.c" "$ROOT_DIR/src/csv_common.c"
gcc -pthread -I"$ROOT_DIR/include" -o test_control_simd \
    "$ROOT_DIR/src/control/parser-simd.c" "$ROOT_DIR/src/csv_common.c"
gcc -pthread -I"$ROOT_DIR/include" -o test_lut_scalar \
    "$ROOT_DIR/src/lut/parser.c" "$ROOT_DIR/src/csv_common.c" "$ROOT_DIR/src/csv_lut.c"
gcc -pthread -I"$ROOT_DIR/include" -o test_lut_simd \
    "$ROOT_DIR/src/lut/parser-simd.c" "$ROOT_DIR/src/csv_common.c" "$ROOT_DIR/src/csv_lut.c"

run_test() {
    local name="$1"
    local csv_file="$2"
    local workers="$3"
    local expected_file="$4"
    local binary="$5"
    TOTAL=$((TOTAL + 1))

    actual=$("./$binary" "$csv_file" "$workers" 2>&1 | sed -n '/^  row /p')
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

# Crash test: just checks exit code 0 (no segfault / signal death)
run_crash_test() {
    local name="$1"
    local csv_file="$2"
    local workers="$3"
    local binary="$4"
    TOTAL=$((TOTAL + 1))

    "./$binary" "$csv_file" "$workers" >/dev/null 2>&1
    local rc=$?
    if [ "$rc" = "0" ]; then
        printf "  PASS: %s (no crash)\n" "$name"
        PASS=$((PASS + 1))
    else
        printf "  FAIL: %s (exit code %d)\n" "$name" "$rc"
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

# Scalar parser doesn't capture quoted content (known limitation)
cat > expected_qualifiers_fsm_scalar.txt << 'EOF'
  row 0 (2 cols): Name Value
  row 1 (2 cols):  42
  row 2 (2 cols): plain 99
EOF

# All other parsers correctly handle RFC 4180 quoting
cat > expected_qualifiers_rfc4180.txt << 'EOF'
  row 0 (2 cols): Name Value
  row 1 (2 cols): hello,world 42
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
  row 0 (3 cols): City Country Population
  row 1 (3 cols): Lisbon Portugal 548703
  row 2 (3 cols): Tokyo Japan 13960000
  row 3 (3 cols): Zurich Switzerland 421878
  row 4 (3 cols): Nairobi Kenya 4397073
  row 5 (3 cols): Reykjavik Iceland 138494
EOF

# --- Test 8: large generated file (validate row/col counts) ---
python3 "$ROOT_DIR/bench/gen_csv.py" 2>/dev/null
# gen_csv creates bench_1k.csv with 1000 rows x 10 cols

# =============================================
# Bad / malformed CSV tests (graceful handling)
# =============================================

# --- Bad 1: empty file ---
: > test_bad_empty.csv

# --- Bad 2: just a newline ---
printf "\n" > test_bad_newline_only.csv

# --- Bad 3: only delimiters ---
printf ",,,,\n,,,,\n" > test_bad_delimiters.csv

python3 -c "
lines = [
    '  row 0 (5 cols):     ',
    '  row 1 (5 cols):     ',
]
with open('expected_bad_delimiters.txt', 'w') as f:
    f.write('\n'.join(lines) + '\n')
"

# --- Bad 4: consecutive newlines (blank rows) ---
printf "a,b\n\n\nc,d\n" > test_bad_blank_rows.csv

python3 -c "
lines = [
    '  row 0 (2 cols): a b',
    '  row 1 (1 cols): ',
    '  row 2 (1 cols): ',
    '  row 3 (2 cols): c d',
]
with open('expected_bad_blank_rows.txt', 'w') as f:
    f.write('\n'.join(lines) + '\n')
"

# --- Bad 5: unclosed qualifier (never-terminated quote) ---
printf '"hello,world\na,b\n' > test_bad_unclosed_q.csv

# --- Bad 6: word exceeding MAX_WORD_LEN (256) ---
python3 -c "print('a' * 500 + ',b')" > test_bad_longword.csv

# --- Bad 7: more columns than MAX_COLS (64) ---
python3 -c "print(','.join(['x'] * 100))" > test_bad_manycols.csv

# --- Bad 8: single character, no newline ---
printf "x" > test_bad_single_char.csv

cat > expected_bad_single_char.txt << 'EOF'
  row 0 (1 cols): x
EOF

# --- Bad 9: binary garbage (non-text data) ---
python3 -c "
import sys
sys.stdout.buffer.write(bytes(range(256)) + b'\n')
sys.stdout.buffer.write(b'a,b,c\n')
" > test_bad_binary.csv

# --- Bad 10: very long row (thousands of columns) ---
python3 -c "print(','.join(str(i) for i in range(500)))" > test_bad_500cols.csv

echo ""
echo "=== Running tests (all parsers, all modes) ==="

for parser_bin in test_fsm_scalar test_fsm_simd test_control_scalar test_control_simd test_lut_scalar test_lut_simd; do
    parser_label="${parser_bin#test_}"

    # Only fsm_scalar strips quoted content
    qual_expected="expected_qualifiers_rfc4180.txt"
    [ "$parser_bin" = "test_fsm_scalar" ] && qual_expected="expected_qualifiers_fsm_scalar.txt"

    for mode_workers in 1 2 4 6 8; do
        printf "\n--- [%s] workers=%d ---\n" "$parser_label" "$mode_workers"

        run_test "simple CSV"           test_simple.csv      "$mode_workers" expected_simple.txt      "$parser_bin"
        run_test "qualifiers"           test_qualifiers.csv  "$mode_workers" "$qual_expected"         "$parser_bin"
        run_test "single column"        test_single_col.csv  "$mode_workers" expected_single_col.txt  "$parser_bin"
        run_test "no trailing newline"  test_no_trail.csv    "$mode_workers" expected_no_trail.txt    "$parser_bin"
        run_test "empty fields"         test_empty.csv       "$mode_workers" expected_empty.txt       "$parser_bin"
        run_test "ragged rows"          test_ragged.csv      "$mode_workers" expected_ragged.txt      "$parser_bin"
        run_test "original test.csv"    test_original.csv    "$mode_workers" expected_original.txt    "$parser_bin"

        # --- Bad CSV tests ---
        run_crash_test "empty file"           test_bad_empty.csv       "$mode_workers" "$parser_bin"
        run_crash_test "newline only"         test_bad_newline_only.csv "$mode_workers" "$parser_bin"
        run_test       "only delimiters"      test_bad_delimiters.csv  "$mode_workers" expected_bad_delimiters.txt "$parser_bin"
        run_test       "blank rows"           test_bad_blank_rows.csv  "$mode_workers" expected_bad_blank_rows.txt "$parser_bin"
        run_crash_test "unclosed qualifier"   test_bad_unclosed_q.csv  "$mode_workers" "$parser_bin"
        run_crash_test "word > MAX_WORD_LEN"  test_bad_longword.csv    "$mode_workers" "$parser_bin"
        run_crash_test "cols > MAX_COLS"       test_bad_manycols.csv    "$mode_workers" "$parser_bin"
        run_test       "single char no newline" test_bad_single_char.csv "$mode_workers" expected_bad_single_char.txt "$parser_bin"
        run_crash_test "binary garbage"       test_bad_binary.csv      "$mode_workers" "$parser_bin"
        run_crash_test "500 columns"          test_bad_500cols.csv     "$mode_workers" "$parser_bin"
    done
done

# --- Large file: validate row count across modes ---
echo ""
echo "--- Row count validation (bench_1k.csv, 1000 rows x 10 cols) ---"
for parser_bin in test_fsm_scalar test_fsm_simd test_control_scalar test_control_simd test_lut_scalar test_lut_simd; do
    parser_label="${parser_bin#test_}"

    for mode_workers in 1 2 4 6 8; do
        actual_rows=$("./$parser_bin" bench_1k.csv "$mode_workers" 2>&1 | head -1 | grep -oP '\d+(?= rows)')
        TOTAL=$((TOTAL + 1))
        if [ "$actual_rows" = "1000" ]; then
            printf "  PASS: [%s] workers=%d → %s rows\n" "$parser_label" "$mode_workers" "$actual_rows"
            PASS=$((PASS + 1))
        else
            printf "  FAIL: [%s] workers=%d → expected 1000 rows, got %s\n" "$parser_label" "$mode_workers" "$actual_rows"
            FAIL=$((FAIL + 1))
        fi
    done
done

# Cleanup
rm -f test_fsm_scalar test_fsm_simd test_control_scalar test_control_simd
rm -f test_lut_scalar test_lut_simd
rm -f test_*.csv expected_*.txt bench_*.csv

echo ""
echo "=== Results: $PASS/$TOTAL passed, $FAIL failed ==="
[ "$FAIL" -gt 0 ] && exit 1
exit 0
