// #include "list.h"
#include <stdio.h>
#include <stdlib.h>

#define DELIMITER ','
#define QUALIFIER '"'

void parse_csv(FILE *fp, char d, char q) {
  // char **parsed_values;

  char token = 0;
  char is_delimiter = 0;
  char is_qualifier = 0;
  char in_qualifier = 0;

  // static int curr_len = 0;
  // static list_t curr_word = (list_t){0};

  while (1) {
    token = fgetc(fp);

    if (feof(fp)) {
      break;
    }

    is_delimiter = !(token ^ d);
    is_qualifier = !(token ^ q);

    in_qualifier ^= is_qualifier;

    printf("%c", token * !(is_delimiter) * !(in_qualifier) * !(is_qualifier));
  }

  // return parsed_values;
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

  parse_csv(csv_fp, DELIMITER, QUALIFIER);

  fclose(csv_fp);

  exit(0);
}
