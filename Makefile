CC = gcc
CFLAGS = -I include -pthread -O3
COMMON = src/csv_common.c
HDRS = include/csv_common.h

# Goto-based parsers
SRC_GOTO_SCALAR = src/goto/parser.c $(COMMON)
SRC_GOTO_SIMD   = src/goto/parser-simd.c $(COMMON)

# Branched parsers (if/while + likely/unlikely)
SRC_BR_SCALAR   = src/branched/parser.c $(COMMON)
SRC_BR_SIMD     = src/branched/parser-simd.c $(COMMON)

.PHONY: all clean test csv bench report graph compare

all: goto-parser goto-parser-simd branched-parser branched-parser-simd

goto-parser: $(SRC_GOTO_SCALAR) $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(SRC_GOTO_SCALAR)

goto-parser-simd: $(SRC_GOTO_SIMD) $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(SRC_GOTO_SIMD)

branched-parser: $(SRC_BR_SCALAR) $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(SRC_BR_SCALAR)

branched-parser-simd: $(SRC_BR_SIMD) $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(SRC_BR_SIMD)

csv:
	python3 bench/gen_csv.py csv/

test: all
	bash tests/run_tests.sh

bench: all
	bash bench/bench.sh

report:
	python3 bench/report.py --no-pdf

graph:
	python3 bench/report.py --pdf-only

compare:
	bash bench/compare.sh

clean:
	rm -f goto-parser goto-parser-simd branched-parser branched-parser-simd
	rm -f bench/*_bench bench/*_timing bench/*_cmp bench/libcsv_bench
	rm -f bench/bench_*.csv bench/bench_results.txt bench/bench_report.txt
	rm -f bench/bench_report.csv bench/bench_phases.csv bench/bench_report.pdf
	rm -f bench/compare_results.txt bench/compare_report.txt
	rm -f tests/test_* tests/expected_*.txt tests/bench_*.csv
