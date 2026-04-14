#include "../include/list.h"

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

/* int main(int argc, char **argv) {
  if (argc < 2) {
    printf("Usage: %s <word1> .. <wordN>\n", argv[0]);
    exit(1);
  }

  for (int word_i = 1; word_i < argc; ++word_i) {
    list_t word = {NULL, 0}; // fresh list every iteration

    for (int i = 0; argv[word_i][i] != '\0'; ++i) {
      list_add_node(&word, argv[word_i][i]); // <-- FIXED index
    }

    char *w = get_w(&word);
    if (w) {
      printf("%s\n", w);
      free(w); // free string
    }

    list_free(&word, word.elem); // free list nodes
  }
} */
