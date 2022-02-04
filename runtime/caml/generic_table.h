//TODO: copyright header

#ifndef CAML_GENERIC_TABLE_H
#define CAML_GENERIC_TABLE_H

#include "misc.h"

#define CAML_TABLE_STRUCT(t) { \
  t *base;                     \
  t *end;                      \
  t *threshold;                \
  t *ptr;                      \
  t *limit;                    \
  asize_t size;                \
  asize_t reserve;             \
}

struct generic_table CAML_TABLE_STRUCT(char);

void alloc_generic_table (struct generic_table *tbl, asize_t sz,
                                 asize_t rsv, asize_t element_size);
void realloc_generic_table (struct generic_table *tbl, asize_t element_size,
 char * msg_intr_int, char *msg_threshold, char *msg_growing, char *msg_error);
// TODO: turn into C macro that takes the type?
Caml_inline void *generic_table_get (struct generic_table *tbl,
                                      asize_t index, asize_t element_size)
{
  if (index > tbl->size ) {
    caml_fatal_error ("generic table - index out of bounds");
  }
  return tbl->base + index * element_size;
}

#endif /* CAML_GENERIC_TABLE_H */