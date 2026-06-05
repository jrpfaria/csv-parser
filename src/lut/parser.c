#include "../../include/csv_common.h"
#include "../../include/csv_lut.h"
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

#define DELIMITER ','
#define QUALIFIER '"'
#define EOL '\n'
#define NUM_WORKERS 4

#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

/* --- Timing helpers --- */
typedef struct {
    double mmap_s;
    double scan_s;
    double dispatch_s;
    double parse_s;
    double total_s;
} parse_timing_t;

static double ts_diff(struct timespec *a, struct timespec *b) {
    return (b->tv_sec - a->tv_sec) + (b->tv_nsec - a->tv_nsec) * 1e-9;
}

void *parse_worker(void *arg)
{
    work_unit_t *wu = (work_unit_t *)arg;

    const char *buf = wu->buf;
    int buf_len = wu->buf_len;
    char d = wu->d;
    char q = wu->q;
    char nl = wu->nl;

    unsigned char lut_local[CSV_LUT_SIZE];
    const unsigned char *class_lut =
        csv_lut_resolve(lut_local, (unsigned char)d, (unsigned char)q,
                        (unsigned char)nl);

    word_buf_t curr_word = {.len = 0};
    arena_t *arena = wu->arena;
    csv_result_t *result = wu->out;
    char *word;

    int r_idx = wu->row_start;
    int cell_idx = wu->cell_start;
    int w_idx = 0;
    int pos = 0;
    int in_qualifier = 0;

    while (likely(pos < buf_len)) {
        unsigned char token = (unsigned char)buf[pos++];
        unsigned char cls = class_lut[csv_lut_index(token)];

        if (unlikely(cls & CSV_LUT_QUALIFIER)) {
            in_qualifier ^= 1;
            continue;
        }

        if (unlikely(in_qualifier)) {
            word_push(&curr_word, (char)token);
            continue;
        }

        if (likely(!(cls & (CSV_LUT_DELIMITER | CSV_LUT_NEWLINE)))) {
            word_push(&curr_word, (char)token);
        } else if (cls & CSV_LUT_DELIMITER) {
            word = word_flush(arena, &curr_word);
            result->cells[cell_idx] = word;
            cell_idx += (cell_idx < MAX_ROWS * MAX_COLS - 1);
            ++w_idx;
        } else {
            word = word_flush(arena, &curr_word);
            result->cells[cell_idx] = word;
            cell_idx += (cell_idx < MAX_ROWS * MAX_COLS - 1);
            result->cols[r_idx] = w_idx + 1;
            r_idx += (r_idx < MAX_ROWS - 1);
            w_idx = 0;
        }
    }

    if (unlikely(curr_word.len + w_idx)) {
        word = word_flush(arena, &curr_word);
        result->cells[cell_idx] = word;
        cell_idx += (cell_idx < MAX_ROWS * MAX_COLS - 1);
        result->cols[r_idx] = w_idx + 1;
        r_idx += (r_idx < MAX_ROWS - 1);
    }

    wu->n_rows = r_idx - wu->row_start;
    return NULL;
}

csv_result_t *parse_csv(const char *filename, char d, char q, char nl,
                        int n_workers, parse_timing_t *timing)
{
    struct timespec t0, t1, t2, t3, t4;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    int fd = open(filename, O_RDONLY);
    struct stat st;
    fstat(fd, &st);
    int file_len = st.st_size;
    const char *data = mmap(NULL, file_len, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);

    clock_gettime(CLOCK_MONOTONIC, &t1);

    static arena_t arenas[MAX_WORKERS];
    static csv_result_t result;
    result.n_rows = 0;

    int mode = n_workers - 1;
    if (mode > 2) mode = 2;

    if (mode == 0) {
        clock_gettime(CLOCK_MONOTONIC, &t2);

        arenas[0].pos = 0;
        work_unit_t single_wu;
        single_wu.buf = data;
        single_wu.buf_len = file_len;
        single_wu.row_start = 0;
        single_wu.cell_start = 0;
        single_wu.n_rows = 0;
        single_wu.d = d;
        single_wu.q = q;
        single_wu.nl = nl;
        single_wu.out = &result;
        single_wu.arena = &arenas[0];

        clock_gettime(CLOCK_MONOTONIC, &t3);
        parse_worker(&single_wu);
        result.n_rows = single_wu.n_rows;
    } else {
        if (mode == 1) n_workers = 2;

        static int row_offsets[MAX_ROWS + 2];
        static int cells_before_row[MAX_ROWS + 2];
        int n_rows = 0;
        int in_q = 0;
        int col_count = 1;
        row_offsets[0] = 0;
        cells_before_row[0] = 0;

        int remaining = file_len & 3;
        int aligned_len = file_len - remaining;

#define SCAN_BYTE(IDX)                        \
do                                            \
{                                             \
    char _c = data[(IDX)];                    \
    int _is_q = !(_c ^ q);                    \
    in_q ^= _is_q;                            \
    col_count += (!(_c ^ d)) & !in_q;         \
    int _is_nl = (!(_c ^ nl)) & !in_q;        \
    row_offsets[n_rows + 1] = (IDX) + 1;      \
    cells_before_row[n_rows + 1] =            \
        cells_before_row[n_rows] + col_count; \
    col_count = col_count * !_is_nl + _is_nl; \
    n_rows += _is_nl & (n_rows < MAX_ROWS);   \
} while (0)

        for (int i = 0; i < aligned_len; i += 4) {
            SCAN_BYTE(i);
            SCAN_BYTE(i + 1);
            SCAN_BYTE(i + 2);
            SCAN_BYTE(i + 3);
        }
        for (int i = aligned_len; i < file_len; i++) {
            SCAN_BYTE(i);
        }
#undef SCAN_BYTE

        if (file_len > 0 && data[file_len - 1] != nl) {
            row_offsets[n_rows + 1] = file_len;
            cells_before_row[n_rows + 1] = cells_before_row[n_rows] + col_count;
            if (n_rows < MAX_ROWS) n_rows++;
        }

        if (n_workers > n_rows) n_workers = n_rows;

        if (unlikely(n_workers == 0)) {
            result.n_rows = 0;
            clock_gettime(CLOCK_MONOTONIC, &t2);
            t3 = t2;
            goto done;
        }

        clock_gettime(CLOCK_MONOTONIC, &t2);

        for (int i = 0; i < n_workers; i++)
            arenas[i].pos = 0;

        work_unit_t units[MAX_WORKERS];
        pthread_t threads[MAX_WORKERS];
        int rows_per_worker = n_rows / n_workers;
        int _remainder = n_rows % n_workers;
        int row_cursor = 0;

        for (int i = 0; i < n_workers; i++) {
            int my_rows = rows_per_worker + (i < _remainder);
            units[i].buf = &data[row_offsets[row_cursor]];
            units[i].buf_len = row_offsets[row_cursor + my_rows] - row_offsets[row_cursor];
            units[i].row_start = row_cursor;
            units[i].cell_start = cells_before_row[row_cursor];
            units[i].n_rows = 0;
            units[i].d = d;
            units[i].q = q;
            units[i].nl = nl;
            units[i].out = &result;
            units[i].arena = &arenas[i];
            row_cursor += my_rows;
        }

        clock_gettime(CLOCK_MONOTONIC, &t3);

        if (mode == 1) {
            for (int i = 1; i < n_workers; i++)
                pthread_create(&threads[i], NULL, parse_worker, &units[i]);
            parse_worker(&units[0]);
            for (int i = 1; i < n_workers; i++)
                pthread_join(threads[i], NULL);
        } else {
            for (int i = 0; i < n_workers; i++)
                pthread_create(&threads[i], NULL, parse_worker, &units[i]);
            for (int i = 0; i < n_workers; i++)
                pthread_join(threads[i], NULL);
        }

        result.n_rows = n_rows;
    }

done:;
    clock_gettime(CLOCK_MONOTONIC, &t4);
    munmap((void *)data, file_len);

    timing->mmap_s = ts_diff(&t0, &t1);
    timing->scan_s = ts_diff(&t1, &t2);
    timing->dispatch_s = ts_diff(&t2, &t3);
    timing->parse_s = ts_diff(&t3, &t4);
    timing->total_s = ts_diff(&t0, &t4);

    return &result;
}

int main(int argc, char **argv) {
    if (argc < 2 || argc > 3) {
        printf("Usage: %s <CSV> [workers]\n", argv[0]);
        exit(1);
    }

    int workers = NUM_WORKERS;
    if (argc == 3)
        workers = atoi(argv[2]);

    parse_timing_t timing;
    csv_result_t *result = parse_csv(argv[1], DELIMITER, QUALIFIER, EOL, workers, &timing);

    const char *mode_names[3] = {"single-threaded", "distributor-as-worker", "distributor+slaves"};
    int mode_idx = (workers - 1);
    if (mode_idx > 2) mode_idx = 2;

#ifndef BENCHMARK
    printf("Parsed Values (%d rows):\n", result->n_rows);
    int idx = 0;
    for (int i = 0; i < result->n_rows; ++i) {
        printf("  row %d (%d cols):", i, result->cols[i]);
        for (int j = 0; j < result->cols[i]; ++j)
            printf(" %s", result->cells[idx++]);
        printf("\n");
    }
#endif

    printf("\n--- Timing [mode: %s, workers: %d] ---\n", mode_names[mode_idx], workers);
    printf("  mmap:     %9.6fs\n", timing.mmap_s);
    printf("  scan:     %9.6fs\n", timing.scan_s);
    printf("  dispatch: %9.6fs\n", timing.dispatch_s);
    printf("  parse:    %9.6fs\n", timing.parse_s);
    printf("  total:    %9.6fs\n", timing.total_s);

    exit(0);
}
