#define CAML_INTERNALS

#include "caml/domains_table.h"
#include "caml/memory.h"
#include "caml/signals.h"

/* invariant: all domain-indexed tables have allocated space for
   caml_max_domains entries */
void alloc_domains_table (
  struct domains_table *tbl, asize_t element_size)
{
  void *new_table;

  tbl->size = caml_max_domains;
  new_table = (void *) caml_stat_alloc_noexc(tbl->size *
                                             element_size);
  if (new_table == NULL) caml_fatal_error ("not enough memory to allocate domain table");
  if (tbl->base != NULL) caml_stat_free (tbl->base);
  tbl->base = new_table;
  tbl->ptr = tbl->base;
  tbl->end = tbl->base + tbl->size * element_size;
}

void realloc_domains_table(
  struct domains_table *tbl, asize_t element_size)
{
  CAMLassert (tbl->ptr <= tbl->end);
  CAMLassert (caml_max_domains > tbl->size);

  if (tbl->base == NULL){
    alloc_domains_table (tbl, element_size);
  } else {
    asize_t sz;
    asize_t cur_ptr = tbl->ptr - tbl->base;

    tbl->size = caml_max_domains;
    sz = tbl->size * element_size;
    tbl->base = caml_stat_resize_noexc (tbl->base, sz);
    if (tbl->base == NULL){
      caml_fatal_error ("failed to resize domains table");
    }
    tbl->end = tbl->base + tbl->size * element_size;
    tbl->ptr = tbl->base + cur_ptr;
  }
}


/* per-domain tables */
struct per_domain_table_list {
  struct per_domain_table_list* next;
  struct domains_table* table;
  asize_t element_size;
  char* name;
};
static struct per_domain_table_list* per_domain_table_list = NULL;

/* precondition: the table [table] is not already in the per-domain table list.
   we assume [name] is a static string, and will not deallocate it.
 */
void caml_create_per_domain_table(struct domains_table* table,
                                        asize_t element_size, char* name)
{
  struct per_domain_table_list* l;

  #ifdef DEBUG
  // check that [table] does not already occur in the list.
  for (l = per_domain_table_list; l != NULL; l = l->next) {
    CAMLassert(l->table != table);
  }
  #endif

  l =
    caml_stat_alloc(sizeof(struct per_domain_table_list));

  alloc_domains_table(table, element_size);

  l->table = table;
  l->next = per_domain_table_list;
  l->element_size = element_size;
  l->name = name;
  per_domain_table_list = l;
}

/* precondition: the table [to_remove] belongs to the per-domain table list. */
/*
void caml_remove_per_domain_table(struct domains_table* to_remove)
{
  struct per_domain_table_list* l = per_domain_table_list;
  if (l->table == to_remove) {
    per_domain_table_list = l->next;
  }
  for (; l != NULL; l = l->next) {
    if (l->next->table == to_remove) {
      l->next = l->next->next;
      return;
    }
  }
  CAMLassert(0); // check that [to_remove] in fact occurred in the list.
  return;
}
*/

static void grow_per_domain_table(const struct domains_table* table, asize_t element_size, int capacity)
{
  // FIXME ideally we use the name here for logging/debugging
  realloc_domains_table ((struct domains_table*) table, element_size);
}

void caml_grow_per_domain_tables(int capacity) {
  struct per_domain_table_list * l = per_domain_table_list;
  for (; l != NULL; l = l->next) {
    grow_per_domain_table(l->table, l->element_size, capacity);
  }
}