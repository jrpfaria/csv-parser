/*
 * libcsv benchmark wrapper — parses a CSV and counts rows/fields.
 * Usage: ./libcsv_bench <file>
 * Outputs row count + timing to match our parser's format.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <csv.h>

static int row_count = 0;
static int field_count = 0;

static void cb_field(void *data, size_t len, void *ctx) {
    field_count++;
}

static void cb_row(int ch, void *ctx) {
    row_count++;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "Usage: %s <csv>\n", argv[0]); return 1; }

    FILE *fp = fopen(argv[1], "rb");
    if (!fp) { perror("fopen"); return 1; }

    struct csv_parser p;
    csv_init(&p, CSV_APPEND_NULL);

    char buf[65536];
    size_t bytes;

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    while ((bytes = fread(buf, 1, sizeof(buf), fp)) > 0) {
        csv_parse(&p, buf, bytes, cb_field, cb_row, NULL);
    }
    csv_fini(&p, cb_field, cb_row, NULL);

    clock_gettime(CLOCK_MONOTONIC, &t1);

    double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) * 1e-9;
    printf("%d rows, %d fields in %.6fs\n", row_count, field_count, elapsed);

    csv_free(&p);
    fclose(fp);
    return 0;
}
