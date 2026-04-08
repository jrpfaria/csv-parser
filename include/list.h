#ifndef __LIST_UTILS_H__
#define __LIST_UTILS_H__

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Configurable limits — tune for your target */
#define MAX_WORD_LEN 256
#define ARENA_SIZE (4 * 1024 * 1024) /* 4 MB per worker */
#define MAX_COLS 64
#define MAX_ROWS (256 * 1024) /* 256K rows total */
#define MAX_WORKERS 8

/* Flat word buffer: O(1) append, O(1) reset */
typedef struct
{
    char buf[MAX_WORD_LEN];
    int len;
} word_buf_t;

/* Bump allocator for string storage — no per-word malloc/free */
typedef struct
{
    char data[ARENA_SIZE];
    int pos;
} arena_t;

/* CSV result with static storage */
typedef struct
{
    char *cells[MAX_ROWS * MAX_COLS]; /* flat array, row-major */
    int cols[MAX_ROWS];
    int n_rows;
} csv_result_t;

/* Work unit assigned by the distributor to a worker */
typedef struct
{
    const char *buf;   /* pointer into mmap'd data — start of first row */
    int buf_len;       /* number of bytes to parse */
    int row_start;     /* which row index to start writing at in result */
    int cell_start;    /* which cell index to start writing at in result */
    int n_rows;        /* number of rows this worker should produce */
    char d;            /* delimiter */
    char q;            /* qualifier */
    char nl;           /* newline */
    csv_result_t *out; /* shared result (workers write to disjoint slices) */
    arena_t *arena;    /* per-worker arena */
} work_unit_t;

/*
 * Appends a character to the word buffer
 * @w: word buffer
 * @c: character to append
 */
void word_push(word_buf_t *w, char c);

/*
 * Resets the word buffer (O(1))
 * @w: word buffer
 */
void word_reset(word_buf_t *w);

/*
 * Copies word buffer contents into the arena and returns a pointer
 * to the null-terminated string. O(n) where n = word length.
 * @a: arena allocator
 * @w: word buffer to flush
 */
char *word_flush(arena_t *a, word_buf_t *w);

/*
 * Worker entry point — parses a buffer slice into the result.
 * Designed to be called via pthread_create.
 * @arg: pointer to a work_unit_t
 */
void *parse_worker(void *arg);

#endif
