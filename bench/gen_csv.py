#!/usr/bin/env python3
"""Generate CSV test files of various sizes."""
import sys
import random
import string

def rand_word(min_len=1, max_len=12):
    length = random.randint(min_len, max_len)
    return ''.join(random.choices(string.ascii_lowercase, k=length))

def gen_csv(rows, cols, filename, quoted_pct=0.1):
    with open(filename, 'w') as f:
        # header
        f.write(','.join(f'col{j}' for j in range(cols)) + '\n')
        for i in range(rows - 1):
            fields = []
            for j in range(cols):
                w = rand_word()
                if random.random() < quoted_pct:
                    w = f'"{w},{rand_word()}"'
                fields.append(w)
            f.write(','.join(fields) + '\n')

if __name__ == '__main__':
    configs = [
        (100,    5,  'bench_100.csv'),
        (1000,   10, 'bench_1k.csv'),
        (10000,  10, 'bench_10k.csv'),
        (100000, 10, 'bench_100k.csv'),
    ]
    random.seed(42)
    for rows, cols, name in configs:
        gen_csv(rows, cols, name)
        print(f'{name}: {rows} rows x {cols} cols')
