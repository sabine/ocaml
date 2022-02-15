/**************************************************************************/
/*                                                                        */
/*                                 OCaml                                  */
/*                                                                        */
/*              Damien Doligez, projet Para, INRIA Rocquencourt           */
/*                                                                        */
/*   Copyright 1996 Institut National de Recherche en Informatique et     */
/*     en Automatique.                                                    */
/*                                                                        */
/*   All rights reserved.  This file is distributed under the terms of    */
/*   the GNU Lesser General Public License version 2.1, with the          */
/*   special exception on linking described in the file LICENSE.          */
/*                                                                        */
/**************************************************************************/

#ifndef CAML_GENERIC_TABLE_H
#define CAML_GENERIC_TABLE_H

#include "misc.h"
#include "eventlog.h"

#define CAML_DOMAINS_TABLE_STRUCT(t) { \
  t *base;                     \
  t *end;                      \
  t *ptr;                      \
  asize_t size;                \
}

struct domains_table CAML_DOMAINS_TABLE_STRUCT(char);

extern void alloc_domains_table
  (struct domains_table *tbl, asize_t element_size);

extern void realloc_domains_table
  (struct domains_table *tbl, asize_t element_size);

Caml_inline void *domains_table_get (struct domains_table *tbl,
                                      asize_t index, asize_t element_size)
{
  if (index > tbl->size ) {
    caml_fatal_error ("domains table - index out of bounds");
  }
  return tbl->base + index * element_size;
}


void caml_create_per_domain_table(struct domains_table* table,
                                        asize_t element_size, char* name);
//void caml_remove_per_domain_table(struct domains_table*);
void caml_grow_per_domain_tables(int);


#endif /* CAML_GENERIC_TABLE_H */
