#include "../include/list.h"
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

/*
 * Worker: parses a buffer slice using the goto FSM.
 * Each worker has its own arena + word_buf, and writes into
 * disjoint slices of the shared result — no locks needed.
 */
void *parse_worker(void *arg)
{
    work_unit_t *wu = (work_unit_t *)arg;

    const void *jt_parser_fsm[6] = {&&parse_token, &&parse_qualifier,
                                    &&parse_delimiter, &&parse_qualifier,
                                    &&parse_new_line, &&parse_qualifier};
    const void *jt_qualifier[2] = {&&worker_loop_start, &&parse_update_status};

    const char *buf = wu->buf;
    int buf_len = wu->buf_len;
    char d = wu->d;
    char q = wu->q;
    char nl = wu->nl;

    word_buf_t curr_word = {.len = 0};
    arena_t *arena = wu->arena;
    csv_result_t *result = wu->out;
    char *word;

    int r_idx = wu->row_start;
    int cell_idx = wu->cell_start;
    int w_idx = 0;
    int pos = 0;

    char token = 0;
    char independent_is_qualifier = 0;
    char is_qualifier = 0;
    char is_delimiter = 0;
    char is_endofline = 0;
    char in_qualifier = 0;

worker_loop_start:;
    const void *jt_eof[2] = {&&parse_classify, &&worker_done};
    goto *jt_eof[pos >= buf_len];

parse_classify:
    token = buf[pos++];

    independent_is_qualifier = !(token ^ q);
    is_qualifier = independent_is_qualifier | in_qualifier;
    is_delimiter = (!(token ^ d)) << 1;
    is_endofline = (!(token ^ nl)) << 2;

    goto *jt_parser_fsm[(is_delimiter | is_qualifier | is_endofline)];

parse_token:
    word_push(&curr_word, token);
    goto worker_loop_start;

parse_delimiter:
    word = word_flush(arena, &curr_word);
    result->cells[cell_idx++] = word;
    ++w_idx;
    goto worker_loop_start;

parse_qualifier:
    goto *jt_qualifier[independent_is_qualifier];

parse_update_status:
    in_qualifier ^= independent_is_qualifier;
    goto worker_loop_start;

parse_new_line:
    word = word_flush(arena, &curr_word);
    result->cells[cell_idx++] = word;
    result->cols[r_idx] = w_idx + 1;
    ++r_idx;
    w_idx = 0;
    goto worker_loop_start;

worker_done:;
    /* handle last row if no trailing newline */
    const void *jt_end[2] = {&&worker_exit, &&worker_finalize};
    goto *jt_end[!!(curr_word.len + w_idx)];

worker_finalize:
    word = word_flush(arena, &curr_word);
    result->cells[cell_idx++] = word;
    result->cols[r_idx] = w_idx + 1;
    ++r_idx;

worker_exit:
    wu->n_rows = r_idx - wu->row_start;
    return NULL;
}

/*
 * MODE 0 — Single-threaded: mmap + direct parse, no scan, no threads.
 * MODE 1 — Distributor-as-worker (2 cores): scan, spawn 1 thread, parse half locally.
 * MODE 2 — Distributor + slaves (>2 cores): scan, spawn N threads, distributor only dispatches.
 *
 * Mode is selected by: min(n_workers - 1, 2)
 *   n_workers=1 → mode 0
 *   n_workers=2 → mode 1
 *   n_workers≥3 → mode 2
 */
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

    /* select mode: 0 = single, 1 = dist-as-worker, 2 = dist+slaves */
    int clamped = n_workers - 1;
    clamped = clamped * (clamped < 2) + 2 * (clamped >= 2); /* min(clamped, 2) */
    const void *jt_mode[3] = {&&mode_single, &&mode_dist_worker, &&mode_dist_slaves};
    goto *jt_mode[clamped];

/* ===== MODE 0: Single-threaded — no scan, no threads ===== */
mode_single:;
    clock_gettime(CLOCK_MONOTONIC, &t2); /* no scan phase */

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
    goto mode_done;

/* ===== MODE 1: Distributor-as-worker (2 cores) ===== */
mode_dist_worker:;
    n_workers = 2;
    goto mode_scan;

/* ===== MODE 2: Distributor + slaves (>2 cores) ===== */
mode_dist_slaves:;
    goto mode_scan;

/* ===== Shared scan + dispatch for modes 1 and 2 ===== */
mode_scan:;
{
    static int row_offsets[MAX_ROWS + 1];
    static int cells_before_row[MAX_ROWS + 1];
    int n_rows = 0;
    int in_q = 0;
    int col_count = 1;
    row_offsets[0] = 0;
    cells_before_row[0] = 0;

    int i = 0;
    int remaining = file_len & 3;
    int aligned_len = file_len - remaining;

#define SCAN_BYTE(IDX)                            \
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
        n_rows += _is_nl;                         \
    } while (0)

scan_loop:;
    const void *jt_scan[2] = {&&scan_body, &&scan_tail};
    goto *jt_scan[i >= aligned_len];

scan_body:
    SCAN_BYTE(i);
    SCAN_BYTE(i + 1);
    SCAN_BYTE(i + 2);
    SCAN_BYTE(i + 3);
    i += 4;
    goto *jt_scan[i >= aligned_len];

scan_tail:;
    const void *jt_tail_tbl[2] = {&&scan_tail_byte, &&scan_trail_check};
    goto *jt_tail_tbl[i >= file_len];

scan_tail_byte:
    SCAN_BYTE(i);
    ++i;
    goto *jt_tail_tbl[i >= file_len];

#undef SCAN_BYTE

scan_trail_check:;
    const void *jt_trail[2] = {&&dist_no_trail, &&dist_trail};
    goto *jt_trail[file_len > 0 && data[file_len - 1] != nl];

dist_trail:
    row_offsets[n_rows + 1] = file_len;
    cells_before_row[n_rows + 1] = cells_before_row[n_rows] + col_count;
    ++n_rows;

dist_no_trail:;

    /* clamp workers to row count */
    const void *jt_clamp[2] = {&&dist_no_clamp, &&dist_clamp};
    goto *jt_clamp[n_workers > n_rows];
dist_clamp:
    n_workers = n_rows;
dist_no_clamp:;

    clock_gettime(CLOCK_MONOTONIC, &t2);

    /* init arenas */
    int _ai = 0;
arena_init_loop:;
    const void *jt_arena[2] = {&&arena_init_done, &&arena_init_body};
    goto *jt_arena[_ai < n_workers];
arena_init_body:
    arenas[_ai].pos = 0;
    ++_ai;
    goto *jt_arena[_ai < n_workers];
arena_init_done:;

    /* build work units */
    work_unit_t units[MAX_WORKERS];
    pthread_t threads[MAX_WORKERS];
    int rows_per_worker = n_rows / n_workers;
    int _remainder = n_rows % n_workers;
    int row_cursor = 0;
    int _wi = 0;

unit_init_loop:;
    const void *jt_unit[2] = {&&unit_init_done, &&unit_init_body};
    goto *jt_unit[_wi < n_workers];
unit_init_body:;
    int my_rows = rows_per_worker + (_wi < _remainder);
    units[_wi].buf = &data[row_offsets[row_cursor]];
    units[_wi].buf_len = row_offsets[row_cursor + my_rows] - row_offsets[row_cursor];
    units[_wi].row_start = row_cursor;
    units[_wi].cell_start = cells_before_row[row_cursor];
    units[_wi].n_rows = 0;
    units[_wi].d = d;
    units[_wi].q = q;
    units[_wi].nl = nl;
    units[_wi].out = &result;
    units[_wi].arena = &arenas[_wi];
    row_cursor += my_rows;
    ++_wi;
    goto *jt_unit[_wi < n_workers];
unit_init_done:;

    clock_gettime(CLOCK_MONOTONIC, &t3);

    /* dispatch based on mode */
    const void *jt_dispatch[3] = {&&mode_done /* unused */, &&dispatch_as_worker, &&dispatch_slaves};
    goto *jt_dispatch[clamped];

    /* --- MODE 1: spawn threads for workers 1..N-1, run worker 0 locally --- */
dispatch_as_worker:;
    int _dw_li = 1;
dw_launch_loop:;
    const void *jt_dw_launch[2] = {&&dw_launch_done, &&dw_launch_body};
    goto *jt_dw_launch[_dw_li < n_workers];
dw_launch_body:
    pthread_create(&threads[_dw_li], NULL, parse_worker, &units[_dw_li]);
    ++_dw_li;
    goto *jt_dw_launch[_dw_li < n_workers];
dw_launch_done:;

    /* distributor parses worker 0's chunk directly */
    parse_worker(&units[0]);

    /* join spawned threads */
    int _dw_ji = 1;
dw_join_loop:;
    const void *jt_dw_join[2] = {&&dw_join_done, &&dw_join_body};
    goto *jt_dw_join[_dw_ji < n_workers];
dw_join_body:
    pthread_join(threads[_dw_ji], NULL);
    ++_dw_ji;
    goto *jt_dw_join[_dw_ji < n_workers];
dw_join_done:;

    result.n_rows = n_rows;
    goto mode_done;

    /* --- MODE 2: spawn all threads, distributor only dispatches --- */
dispatch_slaves:;
    int _li = 0;
launch_loop:;
    const void *jt_launch[2] = {&&launch_done, &&launch_body};
    goto *jt_launch[_li < n_workers];
launch_body:
    pthread_create(&threads[_li], NULL, parse_worker, &units[_li]);
    ++_li;
    goto *jt_launch[_li < n_workers];
launch_done:;

    int _ji = 0;
join_loop:;
    const void *jt_join[2] = {&&join_done, &&join_body};
    goto *jt_join[_ji < n_workers];
join_body:
    pthread_join(threads[_ji], NULL);
    ++_ji;
    goto *jt_join[_ji < n_workers];
join_done:;

    result.n_rows = n_rows;
}

mode_done:;
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
  const void *jt_arg[2] = {&&no_workers_arg, &&has_workers_arg};
  goto *jt_arg[argc == 3];
has_workers_arg:
  workers = atoi(argv[2]);
no_workers_arg:;

  parse_timing_t timing;
  csv_result_t *result = parse_csv(argv[1], DELIMITER, QUALIFIER, EOL, workers, &timing);

  const char *mode_names[3] = {"single-threaded", "distributor-as-worker", "distributor+slaves"};
  int mode_idx = (workers - 1);
  mode_idx = mode_idx * (mode_idx < 2) + 2 * (mode_idx >= 2);

#ifndef BENCHMARK
  printf("Parsed Values (%d rows):\n", result->n_rows);
  int idx = 0;
  for (int i = 0; i < result->n_rows; ++i)
  {
      printf("  row %d (%d cols):", i, result->cols[i]);
      for (int j = 0; j < result->cols[i]; ++j)
      {
          printf(" %s", result->cells[idx++]);
      }
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
