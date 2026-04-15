#include "../../include/csv_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <string.h>
#include <emmintrin.h> /* SSE2 */

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

static inline void word_push_bulk(word_buf_t *w, const char *src, int n)
{
    int space = MAX_WORD_LEN - 1 - w->len;
    int copy = n < space ? n : space;
    if (unlikely(copy <= 0)) return;
    memcpy(w->buf + w->len, src, copy);
    w->len += copy;
}

/* ============================================================
 * SSE2 helpers — branched versions (if/else instead of goto)
 * ============================================================ */

static inline int find_special(const char *buf, int len,
                                char d, char q, char nl)
{
    __m128i vd  = _mm_set1_epi8(d);
    __m128i vq  = _mm_set1_epi8(q);
    __m128i vnl = _mm_set1_epi8(nl);

    int i = 0;
    int simd_end = len - 15;

    while (likely(i < simd_end)) {
        __m128i chunk = _mm_loadu_si128((const __m128i *)(buf + i));
        __m128i m = _mm_or_si128(
            _mm_cmpeq_epi8(chunk, vd),
            _mm_or_si128(
                _mm_cmpeq_epi8(chunk, vq),
                _mm_cmpeq_epi8(chunk, vnl)));
        int mask = _mm_movemask_epi8(m);
        if (likely(mask))
            return i + __builtin_ctz(mask);
        i += 16;
    }

    /* scalar tail */
    while (i < len) {
        char c = buf[i];
        if (unlikely(c == d || c == q || c == nl))
            return i;
        ++i;
    }

    return len;
}

static inline int find_qualifier(const char *buf, int len, char q)
{
    __m128i vq = _mm_set1_epi8(q);

    int i = 0;
    int simd_end = len - 15;

    while (likely(i < simd_end)) {
        __m128i chunk = _mm_loadu_si128((const __m128i *)(buf + i));
        int mask = _mm_movemask_epi8(_mm_cmpeq_epi8(chunk, vq));
        if (likely(mask))
            return i + __builtin_ctz(mask);
        i += 16;
    }

    while (i < len) {
        if (unlikely(buf[i] == q))
            return i;
        ++i;
    }

    return len;
}

/* ============================================================
 * SIMD parse worker — branched version
 * ============================================================ */
void *parse_worker(void *arg)
{
    work_unit_t *wu = (work_unit_t *)arg;
    const char *buf = wu->buf;
    int buf_len     = wu->buf_len;
    char d  = wu->d;
    char q  = wu->q;
    char nl = wu->nl;

    word_buf_t curr_word = {.len = 0};
    arena_t *arena     = wu->arena;
    csv_result_t *result = wu->out;

    int r_idx    = wu->row_start;
    int cell_idx = wu->cell_start;
    int w_idx    = 0;
    int pos      = 0;

    int in_qualifier = 0;

    while (likely(pos < buf_len)) {
        if (unlikely(in_qualifier)) {
            /* scan for closing quote */
            int next = find_qualifier(buf + pos, buf_len - pos, q);
            if (likely(next > 0))
                word_push_bulk(&curr_word, buf + pos, next);
            pos += next;

            if (unlikely(pos >= buf_len))
                break;

            pos++; /* consume closing quote */

            /* RFC 4180: doubled quote ("") → literal quote */
            if (pos < buf_len && buf[pos] == q) {
                word_push(&curr_word, q);
                pos++;
                /* stay in qualifier */
            } else {
                in_qualifier = 0;
            }
        } else {
            /* scan for next special character */
            int next = find_special(buf + pos, buf_len - pos, d, q, nl);
            if (likely(next > 0))
                word_push_bulk(&curr_word, buf + pos, next);
            pos += next;

            if (unlikely(pos >= buf_len))
                break;

            char c = buf[pos++];
            if (likely(c == d)) {
                /* delimiter */
                char *word = word_flush(arena, &curr_word);
                result->cells[cell_idx] = word;
                cell_idx += (cell_idx < MAX_ROWS * MAX_COLS - 1);
                ++w_idx;
            } else if (c == nl) {
                /* newline */
                char *word = word_flush(arena, &curr_word);
                result->cells[cell_idx] = word;
                cell_idx += (cell_idx < MAX_ROWS * MAX_COLS - 1);
                result->cols[r_idx] = w_idx + 1;
                r_idx += (r_idx < MAX_ROWS - 1);
                w_idx = 0;
            } else {
                /* qualifier — enter quoted state */
                in_qualifier = 1;
            }
        }
    }

    /* handle last row if no trailing newline */
    if (unlikely(curr_word.len + w_idx)) {
        char *word = word_flush(arena, &curr_word);
        result->cells[cell_idx] = word;
        cell_idx += (cell_idx < MAX_ROWS * MAX_COLS - 1);
        result->cols[r_idx] = w_idx + 1;
        r_idx += (r_idx < MAX_ROWS - 1);
    }

    wu->n_rows = r_idx - wu->row_start;
    return NULL;
}

/* Fast newline finder — branched version */
static inline int find_next_newline(const char *buf, int len, char nl)
{
    __m128i vnl = _mm_set1_epi8(nl);
    int i = 0;
    int simd_end = len - 15;

    while (likely(i < simd_end)) {
        __m128i chunk = _mm_loadu_si128((const __m128i *)(buf + i));
        int mask = _mm_movemask_epi8(_mm_cmpeq_epi8(chunk, vnl));
        if (likely(mask))
            return i + __builtin_ctz(mask) + 1;
        i += 16;
    }

    while (i < len) {
        if (unlikely(buf[i] == nl))
            return i + 1;
        ++i;
    }

    return len;
}

/* ============================================================
 * parse_csv — branched version
 * ============================================================ */
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

    if (n_workers <= 1) {
        /* MODE 0: single-threaded */
        clock_gettime(CLOCK_MONOTONIC, &t2);

        arenas[0].pos = 0;
        work_unit_t single_wu;
        single_wu.buf       = data;
        single_wu.buf_len   = file_len;
        single_wu.row_start = 0;
        single_wu.cell_start = 0;
        single_wu.n_rows    = 0;
        single_wu.d  = d;
        single_wu.q  = q;
        single_wu.nl = nl;
        single_wu.out   = &result;
        single_wu.arena = &arenas[0];

        clock_gettime(CLOCK_MONOTONIC, &t3);
        parse_worker(&single_wu);
        result.n_rows = single_wu.n_rows;
    } else {
        /* MODE 1+: byte-range partitioning */
        if (unlikely(file_len == 0)) {
            result.n_rows = 0;
            clock_gettime(CLOCK_MONOTONIC, &t2);
            t3 = t2;
            goto done;
        }

        clock_gettime(CLOCK_MONOTONIC, &t2);

        work_unit_t units[MAX_WORKERS];
        pthread_t threads[MAX_WORKERS];

        int rows_per_slot = MAX_ROWS / n_workers;
        int cells_per_slot = rows_per_slot * MAX_COLS;

        /* compute byte boundaries aligned to newlines */
        int boundaries[MAX_WORKERS + 1];
        boundaries[0] = 0;
        boundaries[n_workers] = file_len;

        for (int i = 1; i < n_workers; i++) {
            int raw = (int)((long long)file_len * i / n_workers);
            boundaries[i] = raw + find_next_newline(data + raw, file_len - raw, nl);
        }

        /* init arenas + build work units */
        for (int i = 0; i < n_workers; i++) {
            arenas[i].pos = 0;
            units[i].buf       = &data[boundaries[i]];
            units[i].buf_len   = boundaries[i + 1] - boundaries[i];
            units[i].row_start = i * rows_per_slot;
            units[i].cell_start = i * cells_per_slot;
            units[i].n_rows    = 0;
            units[i].d  = d;
            units[i].q  = q;
            units[i].nl = nl;
            units[i].out   = &result;
            units[i].arena = &arenas[i];
        }

        clock_gettime(CLOCK_MONOTONIC, &t3);

        /* spawn workers 1..N-1, run worker 0 locally */
        for (int i = 1; i < n_workers; i++)
            pthread_create(&threads[i], NULL, parse_worker, &units[i]);

        parse_worker(&units[0]);

        for (int i = 1; i < n_workers; i++)
            pthread_join(threads[i], NULL);

        /* compact results */
        int total_rows = units[0].n_rows;
        int total_cells = 0;
        for (int ri = 0; ri < total_rows; ri++)
            total_cells += result.cols[ri];

        for (int ci = 1; ci < n_workers; ci++) {
            int src_row  = ci * rows_per_slot;
            int src_cell = ci * cells_per_slot;
            int wrows = units[ci].n_rows;

            int src_cells_total = 0;
            for (int rj = 0; rj < wrows; rj++)
                src_cells_total += result.cols[src_row + rj];

            memmove(&result.cols[total_rows], &result.cols[src_row],
                    wrows * sizeof(int));
            memmove(&result.cells[total_cells], &result.cells[src_cell],
                    src_cells_total * sizeof(char *));

            total_rows += wrows;
            total_cells += src_cells_total;
        }

        result.n_rows = total_rows;
    }

done:;
    clock_gettime(CLOCK_MONOTONIC, &t4);
    munmap((void *)data, file_len);

    timing->mmap_s     = ts_diff(&t0, &t1);
    timing->scan_s     = ts_diff(&t1, &t2);
    timing->dispatch_s = ts_diff(&t2, &t3);
    timing->parse_s    = ts_diff(&t3, &t4);
    timing->total_s    = ts_diff(&t0, &t4);

    return &result;
}

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 3) {
        printf("Usage: %s <CSV> [workers]\n", argv[0]);
        exit(1);
    }

    int workers = NUM_WORKERS;
    if (argc == 3)
        workers = atoi(argv[2]);

    parse_timing_t timing;
    csv_result_t *result = parse_csv(argv[1], DELIMITER, QUALIFIER, EOL,
                                     workers, &timing);

    const char *mode_names[2] = {"single-threaded", "byte-range-parallel"};
    int mode_idx = (workers > 1);

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

    printf("\n--- Timing [mode: %s, workers: %d] ---\n",
           mode_names[mode_idx], workers);
    printf("  mmap:     %9.6fs\n", timing.mmap_s);
    printf("  scan:     %9.6fs\n", timing.scan_s);
    printf("  dispatch: %9.6fs\n", timing.dispatch_s);
    printf("  parse:    %9.6fs\n", timing.parse_s);
    printf("  total:    %9.6fs\n", timing.total_s);

    exit(0);
}
