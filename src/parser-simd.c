#include "../include/list.h"
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <string.h>
#include <emmintrin.h> /* SSE2 — baseline for all x86-64 */

#define DELIMITER ','
#define QUALIFIER '"'
#define EOL '\n'
#define NUM_WORKERS 4

/* --- Timing helpers (same as original) --- */
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

/* ============================================================
 * Bulk word push — replaces per-byte word_push with memcpy.
 * Copies up to (MAX_WORD_LEN-1 - w->len) bytes.
 * ============================================================ */
static inline void word_push_bulk(word_buf_t *w, const char *src, int n)
{
    int space = MAX_WORD_LEN - 1 - w->len;
    int copy = n < space ? n : space;
    copy *= (copy > 0);           /* branchless clamp to 0 */
    memcpy(w->buf + w->len, src, copy);
    w->len += copy;
}

/* ============================================================
 * SSE2 helpers: scan 16 bytes at a time for special characters.
 * ============================================================ */

/*
 * Find first byte matching delimiter, qualifier, or newline.
 * Used when NOT inside a quoted field.
 * Returns offset from buf, or len if none found.
 */
static inline int find_special(const char *buf, int len,
                                char d, char q, char nl)
{
    __m128i vd  = _mm_set1_epi8(d);
    __m128i vq  = _mm_set1_epi8(q);
    __m128i vnl = _mm_set1_epi8(nl);

    int i = 0;
    int simd_end = len - 15;

    goto simd_check;
simd_body:;
    {
        __m128i chunk = _mm_loadu_si128((const __m128i *)(buf + i));
        __m128i m = _mm_or_si128(
            _mm_cmpeq_epi8(chunk, vd),
            _mm_or_si128(
                _mm_cmpeq_epi8(chunk, vq),
                _mm_cmpeq_epi8(chunk, vnl)));
        int mask = _mm_movemask_epi8(m);
        const void *jt_hit[2] = {&&simd_next, &&simd_found};
        goto *jt_hit[!!mask];
    simd_found:
        return i + __builtin_ctz(mask);
    simd_next:
        i += 16;
    }
simd_check:;
    const void *jt_simd[2] = {&&scalar_check, &&simd_body};
    goto *jt_simd[i < simd_end];

    /* scalar tail */
scalar_body:;
    {
        char c = buf[i];
        const void *jt_match[2] = {&&scalar_next, &&scalar_found};
        goto *jt_match[c == d || c == q || c == nl];
    scalar_found:
        return i;
    scalar_next:
        ++i;
    }
scalar_check:;
    const void *jt_scalar[2] = {&&not_found, &&scalar_body};
    goto *jt_scalar[i < len];

not_found:
    return len;
}

/*
 * Find first qualifier (quote) byte.
 * Used when INSIDE a quoted field.
 * Returns offset from buf, or len if none found.
 */
static inline int find_qualifier(const char *buf, int len, char q)
{
    __m128i vq = _mm_set1_epi8(q);

    int i = 0;
    int simd_end = len - 15;

    goto q_simd_check;
q_simd_body:;
    {
        __m128i chunk = _mm_loadu_si128((const __m128i *)(buf + i));
        int mask = _mm_movemask_epi8(_mm_cmpeq_epi8(chunk, vq));
        const void *jt_hit[2] = {&&q_simd_next, &&q_simd_found};
        goto *jt_hit[!!mask];
    q_simd_found:
        return i + __builtin_ctz(mask);
    q_simd_next:
        i += 16;
    }
q_simd_check:;
    const void *jt_q[2] = {&&q_scalar_check, &&q_simd_body};
    goto *jt_q[i < simd_end];

q_scalar_body:;
    {
        const void *jt_qm[2] = {&&q_scalar_next, &&q_scalar_found};
        goto *jt_qm[buf[i] == q];
    q_scalar_found:
        return i;
    q_scalar_next:
        ++i;
    }
q_scalar_check:;
    const void *jt_qs[2] = {&&q_not_found, &&q_scalar_body};
    goto *jt_qs[i < len];

q_not_found:
    return len;
}

/* ============================================================
 * SIMD-accelerated parse worker.
 *
 * Two-state goto FSM:
 *   state_normal:       SIMD-scan for {delimiter, qualifier, newline}
 *   state_in_qualifier: SIMD-scan for {qualifier} only
 *
 * Plain text is bulk-copied via memcpy instead of byte-at-a-time.
 * Properly handles RFC 4180 doubled-quote escaping ("" → ").
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

    goto state_normal;

/* ---- State: outside qualifier ---- */
state_normal:;
    {
        const void *jt_eof[2] = {&&normal_scan, &&worker_done};
        goto *jt_eof[pos >= buf_len];
    }

normal_scan:;
    {
        int next = find_special(buf + pos, buf_len - pos, d, q, nl);

        /* bulk-copy plain text before the special byte */
        const void *jt_copy[2] = {&&normal_classify, &&normal_bulk};
        goto *jt_copy[next > 0];
    normal_bulk:
        word_push_bulk(&curr_word, buf + pos, next);
    normal_classify:
        pos += next;
    }
    {
        const void *jt_eof2[2] = {&&normal_dispatch, &&worker_done};
        goto *jt_eof2[pos >= buf_len];
    }

normal_dispatch:;
    {
        char c = buf[pos++];
        const void *jt_class[2] = {&&check_qual, &&handle_delimiter};
        goto *jt_class[c == d];
    check_qual:;
        const void *jt_class2[2] = {&&handle_newline, &&handle_qualifier_enter};
        goto *jt_class2[c == q];
    }

handle_delimiter:;
    {
        char *word = word_flush(arena, &curr_word);
        result->cells[cell_idx] = word;
        cell_idx += (cell_idx < MAX_ROWS * MAX_COLS - 1);
        ++w_idx;
        goto state_normal;
    }

handle_newline:;
    {
        char *word = word_flush(arena, &curr_word);
        result->cells[cell_idx] = word;
        cell_idx += (cell_idx < MAX_ROWS * MAX_COLS - 1);
        result->cols[r_idx] = w_idx + 1;
        r_idx += (r_idx < MAX_ROWS - 1);
        w_idx = 0;
        goto state_normal;
    }

handle_qualifier_enter:;
    goto state_in_qualifier;

/* ---- State: inside qualifier ---- */
state_in_qualifier:;
    {
        const void *jt_eof3[2] = {&&qual_scan, &&worker_done};
        goto *jt_eof3[pos >= buf_len];
    }

qual_scan:;
    {
        int next = find_qualifier(buf + pos, buf_len - pos, q);

        /* bulk-copy quoted content */
        const void *jt_qcopy[2] = {&&qual_found, &&qual_bulk};
        goto *jt_qcopy[next > 0];
    qual_bulk:
        word_push_bulk(&curr_word, buf + pos, next);
    qual_found:
        pos += next;
    }
    {
        const void *jt_eof4[2] = {&&qual_consume, &&worker_done};
        goto *jt_eof4[pos >= buf_len];
    }

qual_consume:
    pos++; /* consume the closing quote */

    /* RFC 4180: doubled quote ("") → literal quote character */
    {
        const void *jt_dq[2] = {&&state_normal, &&qual_doubled};
        goto *jt_dq[pos < buf_len && buf[pos] == q];
    qual_doubled:
        word_push(&curr_word, q);
        pos++;
        goto state_in_qualifier;
    }

/* ---- End of buffer ---- */
worker_done:;
    {
        const void *jt_end[2] = {&&worker_exit, &&worker_finalize};
        goto *jt_end[!!(curr_word.len + w_idx)];
    }

worker_finalize:;
    {
        char *word = word_flush(arena, &curr_word);
        result->cells[cell_idx] = word;
        cell_idx += (cell_idx < MAX_ROWS * MAX_COLS - 1);
        result->cols[r_idx] = w_idx + 1;
        r_idx += (r_idx < MAX_ROWS - 1);
    }

worker_exit:
    wu->n_rows = r_idx - wu->row_start;
    return NULL;
}

/* ============================================================
 * Fast SIMD newline finder — find the first '\n' byte.
 * Used for byte-range boundary alignment.
 * Returns offset past the newline, or len if none found.
 *
 * Note: this does NOT track quote state. For CSV files with
 * quoted multi-line fields, the two-pass speculative approach
 * (like xsv uses) would be needed. For the common case (no
 * embedded newlines in quoted fields), this is correct and fast.
 * ============================================================ */
static inline int find_next_newline(const char *buf, int len, char nl)
{
    __m128i vnl = _mm_set1_epi8(nl);
    int i = 0;
    int simd_end = len - 15;

    goto fnl_check;
fnl_body:;
    {
        __m128i chunk = _mm_loadu_si128((const __m128i *)(buf + i));
        int mask = _mm_movemask_epi8(_mm_cmpeq_epi8(chunk, vnl));
        const void *jt_h[2] = {&&fnl_next, &&fnl_found};
        goto *jt_h[!!mask];
    fnl_found:
        return i + __builtin_ctz(mask) + 1; /* +1 to skip past the newline */
    fnl_next:
        i += 16;
    }
fnl_check:;
    {
        const void *jt_s[2] = {&&fnl_tail_check, &&fnl_body};
        goto *jt_s[i < simd_end];
    }

fnl_tail:;
    {
        const void *jt_tnl[2] = {&&fnl_tail_next, &&fnl_tail_found};
        goto *jt_tnl[buf[i] == nl];
    fnl_tail_found:
        return i + 1;
    fnl_tail_next:
        ++i;
    }
fnl_tail_check:;
    {
        const void *jt_t[2] = {&&fnl_not_found, &&fnl_tail};
        goto *jt_t[i < len];
    }

fnl_not_found:
    return len;
}

/* ============================================================
 * parse_csv — 2-mode driver
 *
 * MODE 0: single-threaded (direct SIMD parse, no scan)
 * MODE 1+: byte-range partitioning (no scan, workers self-align)
 *
 * For multi-worker: split file into N byte ranges. Each worker
 * finds the first newline to align to, then parses from there.
 * Workers write into private row/cell regions, then we stitch
 * the results together after joining. NO scan phase needed.
 * ============================================================ */
csv_result_t *parse_csv(const char *filename, char d, char q, char nl,
                        int n_workers, parse_timing_t *timing)
{
    struct timespec t0, t1, t2, t3, t4;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    /* mmap the file */
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

    /* MODE 0 (single) vs MODE 1+ (byte-range parallel) */
    const void *jt_mode[2] = {&&mode_single, &&mode_parallel};
    goto *jt_mode[n_workers > 1];

/* ===== MODE 0: single-threaded ===== */
mode_single:;
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
    goto mode_done;

/* ===== MODE 1+: byte-range partitioning (no scan) ===== */
mode_parallel:;
{
    /* empty file guard */
    const void *jt_empty[2] = {&&mode_done_zero, &&para_proceed};
    goto *jt_empty[file_len > 0];
mode_done_zero:
    result.n_rows = 0;
    clock_gettime(CLOCK_MONOTONIC, &t2);
    t3 = t2;
    goto mode_done;
para_proceed:;

    clock_gettime(CLOCK_MONOTONIC, &t2); /* no scan phase */

    /*
     * Each worker gets ~file_len/n_workers bytes.
     * Worker 0 starts at byte 0.
     * Worker i (i>0) starts at the first newline after (file_len*i/n_workers).
     * Each worker writes into its own row/cell region:
     *   row_start  = i * (MAX_ROWS / n_workers)
     *   cell_start = i * (MAX_ROWS * MAX_COLS / n_workers)
     * After joining, we compact the result.
     */
    work_unit_t units[MAX_WORKERS];
    pthread_t threads[MAX_WORKERS];

    int rows_per_slot = MAX_ROWS / n_workers;
    int cells_per_slot = rows_per_slot * MAX_COLS;

    /* compute byte boundaries — align to newlines */
    int boundaries[MAX_WORKERS + 1];
    boundaries[0] = 0;
    boundaries[n_workers] = file_len;

    /* find split points */
    int _bi = 1;
boundary_loop:;
    {
        const void *jt_bl[2] = {&&boundary_done, &&boundary_body};
        goto *jt_bl[_bi < n_workers];
    boundary_body:;
        {
            int raw = (int)((long long)file_len * _bi / n_workers);
            /* find next newline from the raw split point */
            int aligned = raw + find_next_newline(
                data + raw, file_len - raw, nl);
            boundaries[_bi] = aligned;
            ++_bi;
            goto *jt_bl[_bi < n_workers];
        }
    boundary_done:;
    }

    /* init arenas + build work units */
    int _wi = 0;
para_unit_loop:;
    {
        const void *jt_pu[2] = {&&para_unit_done, &&para_unit_body};
        goto *jt_pu[_wi < n_workers];
    para_unit_body:;
        {
            arenas[_wi].pos = 0;
            units[_wi].buf       = &data[boundaries[_wi]];
            units[_wi].buf_len   = boundaries[_wi + 1] - boundaries[_wi];
            units[_wi].row_start = _wi * rows_per_slot;
            units[_wi].cell_start = _wi * cells_per_slot;
            units[_wi].n_rows    = 0;
            units[_wi].d  = d;
            units[_wi].q  = q;
            units[_wi].nl = nl;
            units[_wi].out   = &result;
            units[_wi].arena = &arenas[_wi];
            ++_wi;
            goto *jt_pu[_wi < n_workers];
        }
    para_unit_done:;
    }

    clock_gettime(CLOCK_MONOTONIC, &t3);

    /* spawn all workers (worker 0 runs locally) */
    int _li = 1;
para_launch_loop:;
    {
        const void *jt_pl[2] = {&&para_launch_done, &&para_launch_body};
        goto *jt_pl[_li < n_workers];
    para_launch_body:
        pthread_create(&threads[_li], NULL, parse_worker, &units[_li]);
        ++_li;
        goto *jt_pl[_li < n_workers];
    para_launch_done:;
    }

    /* run worker 0 on this thread */
    parse_worker(&units[0]);

    /* join */
    int _ji = 1;
para_join_loop:;
    {
        const void *jt_pj[2] = {&&para_join_done, &&para_join_body};
        goto *jt_pj[_ji < n_workers];
    para_join_body:
        pthread_join(threads[_ji], NULL);
        ++_ji;
        goto *jt_pj[_ji < n_workers];
    para_join_done:;
    }

    /*
     * Compact results: each worker wrote into its own region.
     * Worker 0's rows are already at position 0.
     * Maintain a running cell count for O(n) compaction.
     */
    int total_rows = units[0].n_rows;

    /* count cells in worker 0's output */
    int total_cells = 0;
    {
        int _ri = 0;
    w0_count_loop:;
        const void *jt_w0[2] = {&&w0_count_done, &&w0_count_body};
        goto *jt_w0[_ri < total_rows];
    w0_count_body:
        total_cells += result.cols[_ri];
        ++_ri;
        goto *jt_w0[_ri < total_rows];
    w0_count_done:;
    }

    int _ci = 1;
compact_loop:;
    {
        const void *jt_cl[2] = {&&compact_done, &&compact_body};
        goto *jt_cl[_ci < n_workers];
    compact_body:;
        {
            int src_row  = _ci * rows_per_slot;
            int src_cell = _ci * cells_per_slot;
            int dst_row  = total_rows;
            int wrows = units[_ci].n_rows;

            /* count source cells */
            int src_cells_total = 0;
            int _rj = 0;
        count_src_cells:;
            {
                const void *jt_csc[2] = {&&count_src_done, &&count_src_body};
                goto *jt_csc[_rj < wrows];
            count_src_body:
                src_cells_total += result.cols[src_row + _rj];
                ++_rj;
                goto *jt_csc[_rj < wrows];
            count_src_done:;
            }

            /* move cols */
            memmove(&result.cols[dst_row], &result.cols[src_row],
                    wrows * sizeof(int));

            /* move cells using running total_cells as destination */
            memmove(&result.cells[total_cells],
                    &result.cells[src_cell],
                    src_cells_total * sizeof(char *));

            total_rows += wrows;
            total_cells += src_cells_total;
            ++_ci;
            goto *jt_cl[_ci < n_workers];
        }
    compact_done:;
    }

    result.n_rows = total_rows;
}

mode_done:;
    clock_gettime(CLOCK_MONOTONIC, &t4);
    munmap((void *)data, file_len);

    timing->mmap_s     = ts_diff(&t0, &t1);
    timing->scan_s     = ts_diff(&t1, &t2);
    timing->dispatch_s = ts_diff(&t2, &t3);
    timing->parse_s    = ts_diff(&t3, &t4);
    timing->total_s    = ts_diff(&t0, &t4);

    return &result;
}

/* ============================================================ */
int main(int argc, char **argv)
{
    if (argc < 2 || argc > 3) {
        printf("Usage: %s <CSV> [workers]\n", argv[0]);
        exit(1);
    }

    int workers = NUM_WORKERS;
    const void *jt_arg[2] = {&&no_workers_arg, &&has_workers_arg};
    goto *jt_arg[argc == 3];
has_workers_arg:
    workers = atoi(argv[2]);
no_workers_arg:;

    parse_timing_t timing;
    csv_result_t *result = parse_csv(argv[1], DELIMITER, QUALIFIER, EOL,
                                     workers, &timing);

    const char *mode_names[2] = {"single-threaded", "byte-range-parallel"};
    int mode_idx = (workers > 1);

#ifndef BENCHMARK
    printf("Parsed Values (%d rows):\n", result->n_rows);
    int idx = 0;
    int i = 0;
print_loop:;
    {
        const void *jt_p[2] = {&&print_done, &&print_row};
        goto *jt_p[i < result->n_rows];
    print_row:
        printf("  row %d (%d cols):", i, result->cols[i]);
        int j = 0;
    col_loop:;
        {
            const void *jt_c[2] = {&&col_done, &&col_print};
            goto *jt_c[j < result->cols[i]];
        col_print:
            printf(" %s", result->cells[idx++]);
            ++j;
            goto *jt_c[j < result->cols[i]];
        col_done:;
        }
        printf("\n");
        ++i;
        goto *jt_p[i < result->n_rows];
    print_done:;
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
