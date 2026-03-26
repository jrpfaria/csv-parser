#ifndef __LIST_UTILS_H__
#define __LIST_UTILS_H__

#include <stdio.h>
#include <stdlib.h>

typedef struct list_node_t list_node_t;
typedef struct list_t list_t;

struct list_node_t {
  char v;
  list_node_t *n;
};

struct list_t {
  list_node_t *head;
  int elem;
};

/*
 * Adds a new element to the provided list
 * @l: target list
 * @c: character to be added
 */
void list_add_node(list_t *l, char c);

/*
 * Reads a complete word from the list of characters
 * @l: reference to the list containing the word to extract
 */
char *get_w(list_t *l);

/*
 * Frees the whole memory used up by the provided list
 * @l: list to be freed
 * @n: number of elements to free
 */
void list_free(list_t *l, int n);

#endif
