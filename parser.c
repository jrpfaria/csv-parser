// #include "list.h"
#include "list.h"
#include <stdio.h>
#include <stdlib.h>

#define DELIMITER ','
#define QUALIFIER '"'
#define EOL '\n'

char **parse_csv(FILE *fp, char d, char q, char nl) {

  /* Jump Table Logic :
   * All states are mutually exclusive, i.e. it is not possible for a character
   * to be both delimiter and token at the same time. Additionally, if inside a
   * qualifier, everything is forced to be ignored, no matter what.
   *
   * Therefore, the simplest way to compute the branching is by assigning a
   * power of 2 to each of the states. The simplest way to achieve this is by
   * spending the extra 4 bytes in memory by adding a NULL field inside the jump
   * table.
   *
   * */
  const void *jump_table_parser_fsm[5] = {&&parse_token, &&parse_delimiter,
                                          &&parse_qualifier, NULL,
                                          &&parse_new_line};
  const void *jump_table_qualifier[2] = {&&parse_not_in_qualifier,
                                         &&parser_loop_start};

  char **parsed_values;
  char token = 0;
  char is_delimiter = 0;
  char is_qualifier = 0;
  char is_endofline = 0;
  char in_qualifier = 0;

  int max_size = 0; /* stores the largest word size */
  list_t curr_word = {NULL, 0};
  char *word;

parser_loop_start:
  token = fgetc(fp);

  if (feof(fp)) {
    goto parser_loop_end;
  }

  is_delimiter = !(token ^ d);
  is_qualifier = (!(token ^ q) << 1) * in_qualifier;
  is_endofline = !(token ^ nl) << 2;

  printf("%c", token * !(is_delimiter) * !(in_qualifier) * !(is_qualifier));

  goto *jump_table_qualifier[in_qualifier * is_qualifier];

parse_not_in_qualifier:
  goto *jump_table_parser_fsm[(is_delimiter + is_qualifier + is_endofline)];

parse_token:
  list_add_node(&curr_word, token);
  goto parser_loop_start;

parse_delimiter:
  word = get_w(&curr_word); /* store complete word */

parse_qualifier:
  /* logic to ensure qualifiers get ignored properly */
  in_qualifier ^= is_qualifier;

parse_new_line:
  /* increment word row counter to store words in next index */

parser_loopback:
  goto parser_loop_start;

parser_loop_end:

  return parsed_values;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    printf("Usage: %s <CSV>\n", argv[0]);
    exit(1);
  }

  FILE *csv_fp = fopen(argv[1], "r");
  if (csv_fp == NULL) {
    printf("Error: File %s not found.\n", argv[1]);
    exit(1);
  }

  parse_csv(csv_fp, DELIMITER, QUALIFIER, EOL);

  fclose(csv_fp);

  exit(0);
}
