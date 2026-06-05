CC = gcc
CFLAGS = -I include -pthread -O3
COMMON = src/csv_common.c
LUT_COMMON = src/csv_lut.c
HDRS = include/csv_common.h

# FSM parsers (computed goto)
SRC_FSM_SCALAR = src/fsm/parser.c $(COMMON)
SRC_FSM_SIMD   = src/fsm/parser-simd.c $(COMMON)

# Control parsers (if/while + likely/unlikely)
SRC_CTRL_SCALAR = src/control/parser.c $(COMMON)
SRC_CTRL_SIMD   = src/control/parser-simd.c $(COMMON)

# LUT parsers (table-driven classification)
SRC_LUT_SCALAR = src/lut/parser.c $(COMMON) $(LUT_COMMON)
SRC_LUT_SIMD   = src/lut/parser-simd.c $(COMMON) $(LUT_COMMON)

.PHONY: all clean test csv bench report graph compare

all: fsm-parser fsm-parser-simd control-parser control-parser-simd lut-parser lut-parser-simd

fsm-parser: $(SRC_FSM_SCALAR) $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(SRC_FSM_SCALAR)

fsm-parser-simd: $(SRC_FSM_SIMD) $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(SRC_FSM_SIMD)

control-parser: $(SRC_CTRL_SCALAR) $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(SRC_CTRL_SCALAR)

control-parser-simd: $(SRC_CTRL_SIMD) $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(SRC_CTRL_SIMD)

lut-parser: $(SRC_LUT_SCALAR) $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(SRC_LUT_SCALAR)

lut-parser-simd: $(SRC_LUT_SIMD) $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(SRC_LUT_SIMD)

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
	rm -f fsm-parser fsm-parser-simd control-parser control-parser-simd lut-parser lut-parser-simd
	rm -f bench/*_bench bench/*_timing bench/*_cmp bench/libcsv_bench
	rm -f bench/bench_*.csv bench/bench_results.txt bench/bench_report.txt
	rm -f bench/bench_report.csv bench/bench_phases.csv bench/bench_report.pdf
	rm -f bench/compare_results.txt bench/compare_report.txt
	rm -f tests/test_* tests/expected_*.txt tests/bench_*.csv
