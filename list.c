#include "list.h"

// TODO: Handle mem cleanup
char *get_w(list_t *l) {
  const void *mlc_jump_table[2] = {&&gw_mlc_success, &&gw_mlc_failure};

  list_node_t *curr = l->head;
  int size = l->elem;

  char *word = malloc((size + 1) * sizeof(char));

  goto *mlc_jump_table[word == NULL];

gw_mlc_failure:
  printf("list_utils: get_w(): malloc failed");
  return NULL;

gw_mlc_success:
  for (int i = 0; i < size; ++i) {
    word[i] = curr->v;
    curr = curr->n;
  }

  word[size] = '\0';

  return word;
}

void list_add_node(list_t *l, char c) {
  /* store the reference to the last element to avoid
   * iterating the list all the time */
  static list_node_t *last_in = NULL;

  const void *mlc_jump_table[2] = {&&lan_mlc_success, &&lan_mlc_failure};

  list_node_t *new_node = malloc(sizeof(list_node_t));

  goto *mlc_jump_table[new_node == NULL];

lan_mlc_failure:
  printf("list_utils: list_add_node(): malloc failed\n");
  return;

lan_mlc_success:
  new_node->v = c;
  new_node->n = NULL;

  const void *lan_jump_table[2] = {&&lan_subseq_elem, &&lan_first_elem};

  goto *lan_jump_table[l->head == NULL];

lan_first_elem:
  /* ensure that last_in is reset for new lists */
  last_in = NULL;
  l->head = new_node;
  goto lan_end;

lan_subseq_elem:
  last_in->n = new_node;

lan_end:
  last_in = new_node;
  l->elem++;
}

void list_free(list_t *l, int n) {
  list_node_t *curr = l->head;

  while (curr != NULL) {
    list_node_t *next = curr->n;
    free(curr);
    curr = next;
  }

  l->head = NULL;
  l->elem = 0;
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
