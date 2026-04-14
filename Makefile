CC = gcc
CFLAGS = -I include -pthread -O3
SRC = src/parser.c src/list.c
SRC_SIMD = src/parser-simd.c src/list.c
HDRS = include/list.h

.PHONY: all clean test csv bench report graph compare

all: parser parser-simd

parser: $(SRC) $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(SRC)

parser-simd: $(SRC_SIMD) $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(SRC_SIMD)

csv:
	python3 bench/gen_csv.py csv/

test: parser parser-simd
	bash tests/run_tests.sh

bench: parser parser-simd
	bash bench/bench.sh

report:
	python3 bench/report.py --no-pdf

graph:
	python3 bench/report.py --pdf-only

compare:
	bash bench/compare.sh

clean:
	rm -f parser parser-simd
	rm -f bench/parser_bench bench/parser_simd_bench bench/parser_timing bench/parser_simd_timing
	rm -f bench/parser_cmp bench/parser_simd_cmp bench/libcsv_bench
	rm -f bench/bench_*.csv bench/bench_results.txt bench/bench_report.txt
	rm -f bench/bench_report.csv bench/bench_phases.csv bench/bench_report.pdf
	rm -f bench/compare_results.txt bench/compare_report.txt
	rm -f tests/test_parser tests/test_parser_simd tests/test_*.csv tests/expected_*.txt
