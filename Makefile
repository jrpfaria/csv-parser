CC = gcc
CFLAGS = -I include -pthread -O3
SRC = src/parser.c src/list.c

.PHONY: all clean bench test report graph csv

all: parser

parser: $(SRC) include/list.h
	$(CC) $(CFLAGS) -o $@ $(SRC)

csv:
	python3 bench/gen_csv.py csv/

test:
	bash tests/run_tests.sh

bench:
	bash bench/bench.sh

report:
	python3 bench/report.py --no-pdf

graph:
	python3 bench/report.py --pdf-only

clean:
	rm -f parser parser_bench bench/parser_bench bench/parser_timing bench/bench_*.csv \
	      bench/bench_results.txt bench/bench_report.txt bench/bench_report.csv bench/bench_phases.csv bench/bench_report.pdf
