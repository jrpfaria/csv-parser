CC = gcc
CFLAGS = -I include -pthread
SRC = src/parser.c src/list.c

.PHONY: all clean bench test report

all: parser

parser: $(SRC) include/list.h
	$(CC) $(CFLAGS) -o $@ $(SRC)

test:
	bash tests/run_tests.sh

bench:
	bash bench/bench.sh

report:
	python3 bench/report.py

clean:
	rm -f parser parser_bench bench/parser_bench bench/parser_timing bench/bench_*.csv \
	      bench/bench_results.txt bench/bench_report.txt bench/bench_report.csv bench/bench_phases.csv
