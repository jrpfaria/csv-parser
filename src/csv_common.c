#include "../include/csv_common.h"

void word_push(word_buf_t *w, char c)
{
    w->buf[w->len] = c;
    w->len += (w->len < MAX_WORD_LEN - 1);
}

void word_reset(word_buf_t *w)
{
    w->len = 0;
}

char *word_flush(arena_t *a, word_buf_t *w)
{
    int need = w->len + 1;
    int safe = (a->pos + need) <= ARENA_SIZE;
    char *dest = &a->data[a->pos * safe];
    memcpy(dest, w->buf, w->len);
    dest[w->len] = '\0';
    a->pos += need * safe;
    w->len = 0;
    return dest;
}
