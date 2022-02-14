/**************************************************************************/
/*                                                                        */
/*                                 OCaml                                  */
/*                                                                        */
/*      KC Sivaramakrishnan, Indian Institute of Technology, Madras       */
/*                 Stephen Dolan, University of Cambridge                 */
/*                   Tom Kelly, OCaml Labs Consultancy                    */
/*                                                                        */
/*   Copyright 2021 OCaml Labs Consultancy Ltd                            */
/*   Copyright 2019 Indian Institute of Technology, Madras                */
/*   Copyright 2019 University of Cambridge                               */
/*                                                                        */
/*   All rights reserved.  This file is distributed under the terms of    */
/*   the GNU Lesser General Public License version 2.1, with the          */
/*   special exception on linking described in the file LICENSE.          */
/*                                                                        */
/**************************************************************************/

#define CAML_INTERNALS

#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>
#include "caml/alloc.h"
#include "caml/backtrace.h"
#include "caml/callback.h"
#include "caml/domain.h"
#include "caml/domain_state.h"
#include "caml/eventlog.h"
#include "caml/fail.h"
#include "caml/fiber.h"
#include "caml/finalise.h"
#include "caml/generic_table.h"
#include "caml/gc_ctrl.h"
#include "caml/globroots.h"
#include "caml/intext.h"
#include "caml/major_gc.h"
#include "caml/minor_gc.h"
#include "caml/memory.h"
#include "caml/osdeps.h"
#include "caml/platform.h"
#include "caml/shared_heap.h"
#include "caml/signals.h"
#include "caml/startup.h"
#include "caml/sync.h"
#include "caml/weak.h"


/* From a runtime perspective, domains must handle stop-the-world (STW)
   sections, during which:
    - they are within a section no mutator code is running
    - all domains will execute the section in parallel
    - barriers are provided to know all domains have reached the
      same stage within a section

   Stop-the-world sections are used to handle duties such as:
    - minor GC
    - major GC to trigger major state machine phase changes

   Two invariants for STW sections:
    - domains only execute mutator code if in the stop-the-world set
    - domains in the stop-the-world set guarantee to service the sections
*/

/* The main C-stack for a domain can enter a blocking call.
   In this scenario a 'backup thread' will become responsible for
   servicing the STW sections on behalf of the domain. Care is needed
   to hand off duties for servicing STW sections between the main
   pthread and the backup pthread when caml_enter_blocking_section
   and caml_leave_blocking_section are called.

   When the state for the backup thread is BT_IN_BLOCKING_SECTION
   the backup thread will service the STW section.

   The state machine for the backup thread (and its transitions)
   are:

           BT_INIT  <---------------------------------------+
              |                                             |
   (install_backup_thread)                                  |
       [main pthread]                                       |
              |                                             |
              v                                             |
       BT_ENTERING_OCAML  <-----------------+               |
              |                             |               |
(caml_enter_blocking_section)               |               |
       [main pthread]                       |               |
              |                             |               |
              |                             |               |
              |               (caml_leave_blocking_section) |
              |                      [main pthread]         |
              v                             |               |
    BT_IN_BLOCKING_SECTION  ----------------+               |
              |                                             |
     (domain_terminate)                                     |
       [main pthread]                                       |
              |                                             |
              v                                             |
        BT_TERMINATE                               (backup_thread_func)
              |                                      [backup pthread]
              |                                             |
              +---------------------------------------------+

 */
#define BT_IN_BLOCKING_SECTION 0
#define BT_ENTERING_OCAML 1
#define BT_TERMINATE 2
#define BT_INIT 3

/* control of STW interrupts */
struct interruptor {
  atomic_uintnat* interrupt_word;
  caml_plat_mutex lock;
  caml_plat_cond cond;

  int running;
  int terminating;
  /* unlike the domain ID, this ID number is not reused */
  uintnat unique_id;

  atomic_uintnat interrupt_pending;
};

struct dom_internal {
  /* readonly fields, initialised and never modified */
  int id;
  caml_domain_state* state;
  struct interruptor interruptor;

  /* backup thread */
  int backup_thread_running;
  pthread_t backup_thread;
  atomic_uintnat backup_thread_msg;
  caml_plat_mutex domain_lock;
  caml_plat_cond domain_cond;

  /* readonly */
  uintnat minor_heap_area;
  uintnat minor_heap_area_end;
};
typedef struct dom_internal dom_internal;

struct caml_domain_state_ptr_table CAML_TABLE_STRUCT(caml_domain_state*);

static struct {
  atomic_uintnat domains_still_running;
  atomic_uintnat num_domains_still_processing;
  void (*callback)(caml_domain_state*,
                   void*,
                   int participating_count,
                   caml_domain_state** others_participating);
  void* data;
  void (*enter_spin_callback)(caml_domain_state*, void*);
  void* enter_spin_data;

  /* barrier state */
  int num_domains;
  atomic_uintnat barrier;

  /* length caml_max_domains */
  struct caml_domain_state_ptr_table participating;
} stw_request = {
  ATOMIC_UINTNAT_INIT(0),
  ATOMIC_UINTNAT_INIT(0),
  NULL,
  NULL,
  NULL,
  NULL,
  0,
  ATOMIC_UINTNAT_INIT(0),
  {0},
};

Caml_inline caml_domain_state** get_stw_participating_domain_ptr
                                                          (asize_t index)
{
  return generic_table_get((struct generic_table*) &stw_request.participating,
                            index, sizeof (caml_domain_state*));
}

static caml_plat_mutex all_domains_lock = CAML_PLAT_MUTEX_INITIALIZER;
static caml_plat_cond all_domains_cond =
    CAML_PLAT_COND_INITIALIZER(&all_domains_lock);
static atomic_uintnat /* dom_internal* */ stw_leader = 0;

struct dom_internal_table CAML_TABLE_STRUCT(struct dom_internal);
static struct dom_internal_table all_domains;
Caml_inline struct dom_internal* get_domain (asize_t index)
{
  return generic_table_get((struct generic_table*) &all_domains,
                            index, sizeof (struct dom_internal));
}

CAMLexport atomic_uintnat caml_num_domains_running;



/* maximum number of domains */
uintnat caml_max_domains;
/* size of the virtual memory reservation for the minor heap, per domain */
uintnat caml_minor_heap_max_wsz;
/*
  The amount of memory reserved for all minor heaps of all domains is
  caml_max_domains * caml_minor_heap_max_wsz. Individual domains can allocate
  smaller minor heaps, but when a domain calls Gc.set to allocate a bigger minor
  heap than this reservation, we perform a new virtual memory reservation based
  on the increased minor heap size.

  New domains are created with a minor heap of size caml_minor_heap_max_wsz.

  Similarly, when we run out of domains at domain creation, or a domain calls
  Gc.set to change the number of max_domains, we update caml_max_domains and
  perform a new virtual memory reservation.

  To perform a new virtual memory reservation for the heaps, we stop the world
  and do a minor collection on all domains.
*/

CAMLexport uintnat caml_minor_heaps_base;
CAMLexport uintnat caml_minor_heaps_end;
static __thread dom_internal* domain_self;


struct dom_internal_ptr_table CAML_TABLE_STRUCT(struct dom_internal*);
/*
 * This structure is protected by all_domains_lock
 * [0, participating_domains) are all the domains taking part in STW sections
 * [participating_domains, max_domains) are all those domains free to be used
 */
static struct {
  int participating_domains;
  struct dom_internal_ptr_table domains; /* length caml_max_domains */
} stw_domains = {
  0,
  {0},
};

Caml_inline struct dom_internal** get_stw_domain_ptr (asize_t index)
{
  return generic_table_get((struct generic_table*) &stw_domains.domains,
                            index, sizeof (struct dom_internal*));
}

static void add_to_stw_domains(dom_internal* dom) {
  int i;
  dom_internal** p;

  CAMLassert(stw_domains.participating_domains < caml_max_domains);
  for(i=stw_domains.participating_domains; *get_stw_domain_ptr(i)!=dom; ++i) {
    CAMLassert(i<caml_max_domains);
  }

  /* swap passed domain with domain at stw_domains.participating_domains */
  p = get_stw_domain_ptr(stw_domains.participating_domains);
  dom = *p;
  *p = *get_stw_domain_ptr(i);
  *get_stw_domain_ptr(i) = dom;
  stw_domains.participating_domains++;
}

static void remove_from_stw_domains(dom_internal* dom) {
  int i;
  dom_internal** p;

  for(i=0; *get_stw_domain_ptr(i)!=dom; ++i) {
    CAMLassert(i<caml_max_domains);
  }
  CAMLassert(i < stw_domains.participating_domains);

  /* swap passed domain to first free domain */
  stw_domains.participating_domains--;
  p = get_stw_domain_ptr(stw_domains.participating_domains);
  *get_stw_domain_ptr(i) = *p;
  *p = dom;
}

static dom_internal* next_free_domain() {
  if (stw_domains.participating_domains == caml_max_domains)
    return NULL;
  /* TODO: here we discover that there is no next free domain and we need to
   increase caml_max_domains and make more space */

  CAMLassert(stw_domains.participating_domains < caml_max_domains);
  return *get_stw_domain_ptr(stw_domains.participating_domains);
}

#ifdef __APPLE__
/* OSX has issues with dynamic loading + exported TLS.
   This is slower but works */
CAMLexport pthread_key_t caml_domain_state_key;
static pthread_once_t key_once = PTHREAD_ONCE_INIT;

static void caml_make_domain_state_key (void)
{
  (void) pthread_key_create (&caml_domain_state_key, NULL);
}

void caml_init_domain_state_key (void)
{
  pthread_once(&key_once, caml_make_domain_state_key);
}

#else
CAMLexport __thread caml_domain_state* Caml_state;
#endif

/* Interrupt functions */
static const uintnat INTERRUPT_MAGIC = (uintnat)(-1);

Caml_inline void interrupt_domain(struct interruptor* s) {
  atomic_store_rel(s->interrupt_word, INTERRUPT_MAGIC);
}

int caml_incoming_interrupts_queued(void)
{
  return atomic_load_acq(&domain_self->interruptor.interrupt_pending);
}

/* must NOT be called with s->lock held */
static void stw_handler(caml_domain_state* domain);
static uintnat handle_incoming(struct interruptor* s)
{
  uintnat handled = atomic_load_acq(&s->interrupt_pending);
  CAMLassert (s->running);
  if (handled) {
    atomic_store_rel(&s->interrupt_pending, 0);

    stw_handler(domain_self->state);
  }
  return handled;
}

static void handle_incoming_otherwise_relax (struct interruptor* self)
{
  if (!handle_incoming(self))
    cpu_relax();
}

void caml_handle_incoming_interrupts(void)
{
  handle_incoming(&domain_self->interruptor);
}

int caml_send_interrupt(struct interruptor* target)
{
  /* signal that there is an interrupt pending */
  CAMLassert(!atomic_load_acq(&target->interrupt_pending));
  atomic_store_rel(&target->interrupt_pending, 1);

  /* Signal the condition variable, in case the target is
     itself waiting for an interrupt to be processed elsewhere */
  caml_plat_lock(&target->lock);
  caml_plat_broadcast(&target->cond); // OPT before/after unlock? elide?
  caml_plat_unlock(&target->lock);

  interrupt_domain(target);

  return 1;
}

static void caml_wait_interrupt_serviced(struct interruptor* target)
{
  int i;

  /* Often, interrupt handlers are fast, so spin for a bit before waiting */
  for (i=0; i<1000; i++) {
    if (!atomic_load_acq(&target->interrupt_pending)) {
      return;
    }
    cpu_relax();
  }

  {
    SPIN_WAIT {
      if (!atomic_load_acq(&target->interrupt_pending))
        return;
    }
  }
}

#define MAX_DOMAIN_NAME_LENGTH 16
void caml_domain_set_name(char *name)
{
  char thread_name[MAX_DOMAIN_NAME_LENGTH];
  snprintf(thread_name, MAX_DOMAIN_NAME_LENGTH,
           "%s%d", name, Caml_state->id);
  caml_thread_setname(thread_name);
}

asize_t caml_norm_minor_heap_size (intnat wsize)
{
  asize_t bs;
  if (wsize < Minor_heap_min) wsize = Minor_heap_min;
  bs = caml_mem_round_up_pages(Bsize_wsize (wsize));

  return Wsize_bsize(bs);
}

void caml_free_minor_heap() {
  caml_domain_state* domain_state = Caml_state;

  CAMLassert(domain_state->young_ptr == domain_state->young_end);

  /* free old minor heap.
     instead of unmapping the heap, we decommit it, so there's
     no race whereby other code could attempt to reuse the memory. */
  caml_mem_decommit(
      (void*)domain_self->minor_heap_area,
      domain_self->minor_heap_area_end - domain_self->minor_heap_area);

  //domain_state->minor_heap_wsz = wsize;

  domain_state->young_start = NULL;
  domain_state->young_end = NULL;
  atomic_store_rel(&domain_state->young_limit,
                   (uintnat) domain_state->young_start);
  domain_state->young_ptr = domain_state->young_end;
}

int caml_allocate_minor_heap(asize_t wsize) {
  caml_domain_state* domain_state = Caml_state;
  
  wsize = caml_norm_minor_heap_size(wsize);

  if (!caml_mem_commit(
          (void*)domain_self->minor_heap_area, Bsize_wsize(wsize))) {
    return -1;
  }

#ifdef DEBUG
  {
    uintnat* p = (uintnat*)domain_self->minor_heap_area;
    for (;
         p < (uintnat*)(domain_self->minor_heap_area + Bsize_wsize(wsize));
         p++)
      *p = Debug_uninit_align;
  }
#endif

  domain_state->minor_heap_wsz = wsize;

  domain_state->young_start = (value*)domain_self->minor_heap_area;
  domain_state->young_end =
      (value*)(domain_self->minor_heap_area + Bsize_wsize(wsize));
  atomic_store_rel(&domain_state->young_limit,
                   (uintnat) domain_state->young_start);
  domain_state->young_ptr = domain_state->young_end;
  return 0;
}

int caml_reallocate_minor_heap(asize_t wsize)
{
  caml_free_minor_heap();

  return caml_allocate_minor_heap(wsize);
}

/* This variable is owned by [all_domains_lock]. */
static uintnat next_domain_unique_id = 0;

/* Precondition: you must own [all_domains_lock].

   Specification:
   - returns 0 on the first call
     (we want the main domain to have unique_id 0)
   - returns distinct ids unless there is an overflow
   - never returns 0 again, even in presence of overflow.
 */
static uintnat fresh_domain_unique_id(void) {
    uintnat next = next_domain_unique_id++;

    /* On 32-bit systems, there is a risk of wraparound of the unique
       id counter. We have decided to let that happen and live with
       it, but we still ensure that id 0 is not reused, to avoid
       having new domains believe that they are the main domain. */
    if (next_domain_unique_id == 0)
      next_domain_unique_id++;

    return next;
}


/* must be run on the domain's thread */
static void create_domain(uintnat initial_minor_heap_wsize) {
  dom_internal* d = 0;
  caml_domain_state* domain_state;
  struct interruptor* s;

  CAMLassert (domain_self == 0);

  /* take the all_domains_lock so that we can alter the STW participant
     set atomically */
  caml_plat_lock(&all_domains_lock);

  /* wait until any in-progress STW sections end */
  while (atomic_load_acq(&stw_leader))
    caml_plat_wait(&all_domains_cond);

  d = next_free_domain();

  if (d == NULL)
    goto domain_init_complete;

  s = &d->interruptor;
  CAMLassert(!s->running);
  CAMLassert(!s->interrupt_pending);

  domain_self = d;

  /* If the chosen domain slot has not been previously used, allocate a fresh
   * domain state. Otherwise, reuse it. Reusing the slot ensures that the GC
   * stats are not lost. */
  if (d->state == NULL) {
    /* FIXME: Never freed. Not clear when to. */
    domain_state = (caml_domain_state*)
      caml_stat_calloc_noexc(1, sizeof(caml_domain_state));
    if (domain_state == NULL)
      goto domain_init_complete;
    d->state = domain_state;
  } else {
    domain_state = d->state;
  }

  SET_Caml_state((void*)domain_state);

  s->unique_id = fresh_domain_unique_id();
  s->interrupt_word = &domain_state->young_limit;
  s->running = 1;
  atomic_fetch_add(&caml_num_domains_running, 1);

  caml_plat_lock(&d->domain_lock);

  domain_state->id = d->id;
  domain_state->unique_id = d->interruptor.unique_id;
  CAMLassert(!d->interruptor.interrupt_pending);

  domain_state->extra_heap_resources = 0.0;
  domain_state->extra_heap_resources_minor = 0.0;

  domain_state->dependent_size = 0;
  domain_state->dependent_allocated = 0;

  if (caml_init_signal_stack() < 0) {
    goto init_signal_stack_failure;
  }

  domain_state->young_start = domain_state->young_end =
    domain_state->young_ptr = 0;
  domain_state->minor_tables = caml_alloc_minor_tables();
  if(domain_state->minor_tables == NULL) {
    goto alloc_minor_tables_failure;
  }

  d->state->shared_heap = caml_init_shared_heap();
  if(d->state->shared_heap == NULL) {
    goto init_shared_heap_failure;
  }

  if (caml_init_major_gc(domain_state) < 0) {
    goto init_major_gc_failure;
  }

  if(caml_reallocate_minor_heap(initial_minor_heap_wsize) < 0) {
    goto reallocate_minor_heap_failure;
  }

  domain_state->dls_root = Val_unit;
  caml_register_generational_global_root(&domain_state->dls_root);

  domain_state->stack_cache = caml_alloc_stack_cache();
  if(domain_state->stack_cache == NULL) {
    goto create_stack_cache_failure;
  }

  domain_state->extern_state = NULL;

  domain_state->intern_state = NULL;

  domain_state->current_stack =
      caml_alloc_main_stack(Stack_size / sizeof(value));
  if(domain_state->current_stack == NULL) {
    goto alloc_main_stack_failure;
  }

  domain_state->c_stack = NULL;
  domain_state->exn_handler = NULL;

  domain_state->gc_regs_buckets = NULL;
  domain_state->gc_regs = NULL;

  domain_state->allocated_words = 0;
  domain_state->swept_words = 0;

  domain_state->local_roots = NULL;

  domain_state->backtrace_buffer = NULL;
  domain_state->backtrace_last_exn = Val_unit;
  domain_state->backtrace_active = 0;
  caml_register_generational_global_root(&domain_state->backtrace_last_exn);

  domain_state->compare_unordered = 0;
  domain_state->oo_next_id_local = 0;

  domain_state->requested_major_slice = 0;
  domain_state->requested_minor_gc = 0;
  domain_state->requested_external_interrupt = 0;

  domain_state->parser_trace = 0;

  if (caml_params->backtrace_enabled) {
    caml_record_backtraces(1);
  }

#ifndef NATIVE_CODE
  domain_state->external_raise = NULL;
  domain_state->trap_sp_off = 1;
  domain_state->trap_barrier_off = 0;
#endif

  add_to_stw_domains(domain_self);
  goto domain_init_complete;

alloc_main_stack_failure:
create_stack_cache_failure:
  caml_remove_generational_global_root(&domain_state->dls_root);
reallocate_minor_heap_failure:
  caml_teardown_major_gc();
init_major_gc_failure:
  caml_teardown_shared_heap(d->state->shared_heap);
init_shared_heap_failure:
  caml_free_minor_tables(domain_state->minor_tables);
  domain_state->minor_tables = NULL;
alloc_minor_tables_failure:
  caml_free_signal_stack();
init_signal_stack_failure:
  domain_self = NULL;


domain_init_complete:
  caml_plat_unlock(&all_domains_lock);
}

CAMLexport void caml_reset_domain_lock(void)
{
  dom_internal* self = domain_self;
  // This is only used to reset the domain_lock state on fork.
  caml_plat_mutex_init(&self->domain_lock);
  caml_plat_cond_init(&self->domain_cond, &self->domain_lock);

  return;
}

/* per-domain tables */

/* Per-domain tables */
struct per_domain_table_list {
  struct per_domain_table_list* next;
  struct generic_table* table;
  asize_t element_size;
  char* name;
};
static struct per_domain_table_list* per_domain_table_list = NULL;

/* precondition: the table [table] is not already in the per-domain table list.
   we assume [name] is a static string, and will not deallocate it.
 */
void caml_register_per_domain_table(struct generic_table * table,
                                        asize_t element_size, char * name)
{
  struct per_domain_table_list* l =
    caml_stat_alloc(sizeof(struct per_domain_table_list));

  // FIXME: consider growing the table here if it fails
  CAMLassert(table->size >= caml_max_domains);

  l->table = table;
  l->next = per_domain_table_list;
  l->element_size = element_size;
  l->name = name;
  per_domain_table_list = l;
  #ifdef DEBUG
  // check that [table] does not occur in the rest of the list.
  for (l = per_domain_table_list->next; l != NULL; l = l->next) {
    CAMLassert(l->table != table);
  }
  #endif
}

/* precondition: the table [to_remove] belongs to the per-domain table list. */
void caml_remove_per_domain_table(struct generic_table *to_remove)
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

static void grow_per_domain_table(const struct generic_table *table, asize_t element_size, int capacity)
{
  // FIXME ideally we would need to use the name here
  realloc_generic_table
    ((struct generic_table *) table, sizeof (struct caml_custom_elt),
     EC_C_REQUEST_GROW_PER_DOMAIN_TABLE,
     "per-domain table threshold crossed\n",
     "Growing per-domain table to %" ARCH_INTNAT_PRINTF_FORMAT "dk bytes\n",
     "per-domain table overflow");
}

void caml_grow_per_domain_tables(int capacity) {
  struct per_domain_table_list * l = per_domain_table_list;
  for (; l != NULL; l = l->next) {
    grow_per_domain_table(l->table, l->element_size, capacity);
  }
}

/* minor heap initialization and resizing */

uintnat get_minor_heap_reservation_size() {
  /* sanity check configuration */
  if (caml_mem_round_up_pages(Bsize_wsize(caml_minor_heap_max_wsz))
          != Bsize_wsize(caml_minor_heap_max_wsz))
    caml_fatal_error("minor_heap_max misconfigured for this platform");

  return (uintnat)Bsize_wsize(caml_minor_heap_max_wsz) 
                                          * caml_max_domains;
}

void reserve_minor_heaps(uintnat size) {
  void* heaps_base;

  /* reserve memory space for minor heaps */
  heaps_base = caml_mem_map(size, caml_sys_pagesize,
                                         1 /* reserve_only */);
  if (heaps_base == NULL)
    caml_fatal_error("Not enough heap memory to reserve minor heaps");

  caml_minor_heaps_base = (uintnat) heaps_base;
  caml_minor_heaps_end = (uintnat) heaps_base + size;
}

void unreserve_minor_heaps() {
  uintnat size;

  size = caml_minor_heaps_end - caml_minor_heaps_base;
  caml_mem_unmap((void *) caml_minor_heaps_base, size);
}

static void caml_stw_resize_minor_heap_and_update_max_domains(
                                       caml_domain_state* domain, void* unused,
                                       int participating_count,
                                       caml_domain_state** participating) {
  barrier_status b;
  uintnat size;

  caml_empty_minor_heap_no_major_slice_from_stw(domain, unused,
                                            participating_count, participating);
  
  caml_free_minor_heap();

  b = caml_global_barrier_begin ();
  if (caml_global_barrier_is_final(b)) {
    CAML_EV_BEGIN(EV_DOMAIN_RESIZE_HEAP_RESERVATION);
    unreserve_minor_heaps();
    size = get_minor_heap_reservation_size();
    reserve_minor_heaps(size);
    CAML_EV_END(EV_DOMAIN_RESIZE_HEAP_RESERVATION);

    CAML_EV_BEGIN(EV_DOMAIN_CHANGE_MAX_DOMAINS);
// TODO: resize tables
    CAML_EV_END(EV_DOMAIN_CHANGE_MAX_DOMAINS);
  }
  
  caml_global_barrier_end(b);

  caml_allocate_minor_heap(Caml_state->minor_heap_wsz);
}

void caml_update_minor_heap_max_and_max_domains(uintnat minor_heap_wsz,
        uintnat max_domains) {
  CAMLassert(minor_heap_wsz > caml_minor_heap_max_wsz);

  // FIXME: unclear if that breaks
  caml_minor_heap_max_wsz = minor_heap_wsz;
  caml_max_domains = max_domains;

  caml_try_run_on_all_domains(
    &caml_stw_resize_minor_heap_and_update_max_domains, NULL, 0);
}

void caml_init_domains() {
  int i;
  //void* heaps_base;
  uintnat heaps_size;

  heaps_size = get_minor_heap_reservation_size();
  reserve_minor_heaps(heaps_size);

  // TODO: maybe we don't want to use our per-domain table machinery
  // here to avoid changing there inside a STW critical region.
  // Sabine suggests hardcoding a fixed limit (4096?) for:
  // - stw_request
  // - stw_domains
  // TODO: what to do about all_domains?
  if (stw_request.participating.base == NULL) {
    alloc_generic_table ((struct generic_table *) &stw_request.participating,
                       caml_max_domains,
                       0,
                       sizeof (struct caml_domain_state*));
  }
  
  if (all_domains.base == NULL) {
    alloc_generic_table ((struct generic_table *) &all_domains,
                       caml_max_domains,
                       0,
                       sizeof (struct dom_internal));
  }
  if (stw_domains.domains.base == NULL) {
    alloc_generic_table ((struct generic_table *) &stw_domains.domains,
                       caml_max_domains,
                       0,
                       sizeof (struct dom_internal*));
  }

  for (i = 0; i < caml_max_domains; i++) {
    struct dom_internal* dom = get_domain(i);
    uintnat domain_minor_heap_base;

    *get_stw_domain_ptr(i) = dom;

    dom->id = i;

    dom->interruptor.interrupt_word = 0;
    caml_plat_mutex_init(&dom->interruptor.lock);
    caml_plat_cond_init(&dom->interruptor.cond,
                        &dom->interruptor.lock);
    dom->interruptor.running = 0;
    dom->interruptor.terminating = 0;
    dom->interruptor.unique_id = 0;
    dom->interruptor.interrupt_pending = 0;

    caml_plat_mutex_init(&dom->domain_lock);
    caml_plat_cond_init(&dom->domain_cond, &dom->domain_lock);
    dom->backup_thread_running = 0;
    dom->backup_thread_msg = BT_INIT;

    domain_minor_heap_base = caml_minor_heaps_base +
      (uintnat)Bsize_wsize(caml_minor_heap_max_wsz) * (uintnat)i;
    dom->minor_heap_area = domain_minor_heap_base;
    dom->minor_heap_area_end =
         domain_minor_heap_base + Bsize_wsize(caml_minor_heap_max_wsz);
  }

  create_domain(caml_minor_heap_max_wsz);
  if (!domain_self) caml_fatal_error("Failed to create main domain");
  CAMLassert (domain_self->state->unique_id == 0);

  caml_init_signal_handling();

  CAML_EVENTLOG_INIT();
  caml_domain_set_name("Domain");
}

void caml_init_domain_self(int domain_id) {
  CAMLassert (domain_id >= 0 && domain_id < caml_max_domains);
  domain_self = get_domain(domain_id);
  SET_Caml_state(domain_self->state);
}

enum domain_status { Dom_starting, Dom_started, Dom_failed };

struct domain_ml_values {
  value callback;
  value mutex;
  /* this mutex is taken when a domain starts and released when it terminates
    which provides a simple way to block domains attempting to join this domain
   */
};

static void init_domain_ml_values(
  struct domain_ml_values* ml_values,
  value callback,
  value mutex)
{
  ml_values->callback = callback;
  ml_values->mutex = mutex;
  caml_register_generational_global_root(&ml_values->callback);
  caml_register_generational_global_root(&ml_values->mutex);
}

static void free_domain_ml_values(struct domain_ml_values* ml_values) {
  caml_remove_generational_global_root(&ml_values->callback);
  caml_remove_generational_global_root(&ml_values->mutex);
  caml_stat_free(ml_values);
}

/* This is the structure of the data exchanged between the parent
   domain and child domain during domain_spawn. Some fields are 'in'
   parameters, passed from the parent to the child, others are 'out'
   parameters returned to the parent by the child.
*/
struct domain_startup_params {
  struct interruptor* parent; /* in */
  enum domain_status status; /* in+out:
                                parent and child synchronize on this value. */
  struct domain_ml_values* ml_values; /* in */
  dom_internal* newdom; /* out */
  uintnat unique_id; /* out */
#ifndef _WIN32
  /* signal mask to set after it is safe to do so */
  sigset_t* mask; /* in */
#endif
};

static void* backup_thread_func(void* v)
{
  dom_internal* di = (dom_internal*)v;
  uintnat msg;
  struct interruptor* s = &di->interruptor;

  domain_self = di;
  SET_Caml_state((void*)(di->state));

  caml_domain_set_name("Backup");

  CAML_EVENTLOG_IS_BACKUP_THREAD();

  /* TODO: how does the backup thread interact with the eventlog infra?
   * caml_ev_tag_self_as_backup_thread(); */

  msg = atomic_load_acq (&di->backup_thread_msg);
  while (msg != BT_TERMINATE) {
    CAMLassert (msg <= BT_TERMINATE);
    switch (msg) {
      case BT_IN_BLOCKING_SECTION:
        /* Handle interrupts on behalf of the main thread:
         *  - must hold domain_lock to handle interrupts
         *  - need to guarantee no blocking so that backup thread
         *    can be signalled from caml_leave_blocking_section
         */
        if (caml_incoming_interrupts_queued()) {
          if (caml_plat_try_lock(&di->domain_lock)) {
            caml_handle_incoming_interrupts();
            caml_plat_unlock(&di->domain_lock);
          }
        }
        /* Wait safely if there is nothing to do.
         * Will be woken from caml_leave_blocking_section
         */
        caml_plat_lock(&s->lock);
        msg = atomic_load_acq (&di->backup_thread_msg);
        if (msg == BT_IN_BLOCKING_SECTION &&
            !caml_incoming_interrupts_queued())
          caml_plat_wait(&s->cond);
        caml_plat_unlock(&s->lock);
        break;
      case BT_ENTERING_OCAML:
        /* Main thread wants to enter OCaml
         * Will be woken from caml_bt_exit_ocaml
         * or domain_terminate
         */
        caml_plat_lock(&di->domain_lock);
        msg = atomic_load_acq (&di->backup_thread_msg);
        if (msg == BT_ENTERING_OCAML)
          caml_plat_wait(&di->domain_cond);
        caml_plat_unlock(&di->domain_lock);
        break;
      default:
        cpu_relax();
        break;
    };
    msg = atomic_load_acq (&di->backup_thread_msg);
  }

  /* doing terminate */
  atomic_store_rel(&di->backup_thread_msg, BT_INIT);

  return 0;
}

static void install_backup_thread (dom_internal* di)
{
  int err;
#ifndef _WIN32
  sigset_t mask, old_mask;
#endif

  if (di->backup_thread_running == 0) {
    CAMLassert (di->backup_thread_msg == BT_INIT || /* Using fresh domain */
            di->backup_thread_msg == BT_TERMINATE); /* Reusing domain */

    while (atomic_load_acq(&di->backup_thread_msg) != BT_INIT) {
      /* Give a chance for backup thread on this domain to terminate */
      caml_plat_unlock (&di->domain_lock);
      cpu_relax ();
      caml_plat_lock (&di->domain_lock);
    }

#ifndef _WIN32
    /* No signals on the backup thread */
    sigfillset(&mask);
    pthread_sigmask(SIG_BLOCK, &mask, &old_mask);
#endif

    atomic_store_rel(&di->backup_thread_msg, BT_ENTERING_OCAML);
    err = pthread_create(&di->backup_thread, 0, backup_thread_func, (void*)di);

#ifndef _WIN32
    pthread_sigmask(SIG_SETMASK, &old_mask, NULL);
#endif

    if (err)
      caml_failwith("failed to create domain backup thread");
    di->backup_thread_running = 1;
    pthread_detach(di->backup_thread);
  }
}

static void caml_domain_stop_default(void)
{
  return;
}

static void caml_domain_external_interrupt_hook_default(void)
{
  return;
}

CAMLexport void (*caml_domain_stop_hook)(void) =
   caml_domain_stop_default;

CAMLexport void (*caml_domain_external_interrupt_hook)(void) =
   caml_domain_external_interrupt_hook_default;

static void domain_terminate();

static void* domain_thread_func(void* v)
{
  sync_mutex terminate_mutex = NULL;
  struct domain_startup_params* p = v;
  struct domain_ml_values *ml_values = p->ml_values;
#ifndef _WIN32
  sigset_t mask = *(p->mask);
#endif

  create_domain(caml_minor_heap_max_wsz);
  /* this domain is now part of the STW participant set */
  p->newdom = domain_self;

  /* handshake with the parent domain */
  caml_plat_lock(&p->parent->lock);
  if (domain_self) {
    /* this domain is part of STW sections, so can read ml_values */
    terminate_mutex = Mutex_val(ml_values->mutex);
    /* we lock terminate_mutex here and unlock when the domain is torn down
      this provides a simple block for domains attempting to join
      NB: terminate_mutex will not be moved by the garbage collector
      as it is not an OCaml block. ml_values->mutex is registered as
      a global root and keeps the mutex custom memory alive with
      the garbage collector. */
    caml_mutex_lock(terminate_mutex);
    p->status = Dom_started;
    p->unique_id = domain_self->interruptor.unique_id;
  } else {
    p->status = Dom_failed;
  }
  caml_plat_broadcast(&p->parent->cond);
  caml_plat_unlock(&p->parent->lock);
  /* Cannot access p below here. */

  if (domain_self) {
    install_backup_thread(domain_self);

#ifndef _WIN32
    /* It is now safe for us to handle signals */
    pthread_sigmask(SIG_SETMASK, &mask, NULL);
#endif

    caml_gc_log("Domain starting (unique_id = %"ARCH_INTNAT_PRINTF_FORMAT"u)",
                domain_self->interruptor.unique_id);
    caml_domain_set_name("Domain");
    caml_callback(ml_values->callback, Val_unit);
    domain_terminate();
    /* Joining domains will lock/unlock the terminate_mutex so this unlock will
       release them if any domains are waiting. */
    caml_mutex_unlock(terminate_mutex);
    free_domain_ml_values(ml_values);
  } else {
    caml_gc_log("Failed to create domain.");
  }
  return 0;
}

CAMLprim value caml_domain_spawn(value callback, value mutex)
{
  CAMLparam2 (callback, mutex);
  struct domain_startup_params p;
  pthread_t th;
  int err;
#ifndef _WIN32
  sigset_t mask, old_mask;
#endif

  CAML_EV_BEGIN(EV_DOMAIN_SPAWN);
  p.parent = &domain_self->interruptor;
  p.status = Dom_starting;

  p.ml_values =
      (struct domain_ml_values*) caml_stat_alloc_noexc(
                                    sizeof(struct domain_ml_values));
  if (!p.ml_values) {
    caml_failwith("failed to create ml values for domain thread");
  }
  init_domain_ml_values(p.ml_values, callback, mutex);

/* We block all signals while we spawn the new domain. This is because
   pthread_create inherits the current signals set, and we want to avoid a
   signal handler being triggered in the new domain before the domain_state is
   fully populated. */
#ifndef _WIN32
  /* FIXME Spawning threads -> unix.c/win32.c */
  sigfillset(&mask);
  pthread_sigmask(SIG_BLOCK, &mask, &old_mask);
  p.mask = &old_mask;
#endif
  err = pthread_create(&th, 0, domain_thread_func, (void*)&p);
#ifndef _WIN32
  /* We can restore the signal mask we had initially now. */
  pthread_sigmask(SIG_SETMASK, &old_mask, NULL);
#endif

  if (err) {
    caml_failwith("failed to create domain thread.");
  }

  /* While waiting for the child thread to start up, we need to service any
     stop-the-world requests as they come in. */
  caml_plat_lock(&domain_self->interruptor.lock);
  while (p.status == Dom_starting) {
    if (caml_incoming_interrupts_queued()) {
      caml_plat_unlock(&domain_self->interruptor.lock);
      handle_incoming(&domain_self->interruptor);
      caml_plat_lock(&domain_self->interruptor.lock);
    } else {
      caml_plat_wait(&domain_self->interruptor.cond);
    }
  }
  caml_plat_unlock(&domain_self->interruptor.lock);

  if (p.status == Dom_started) {
    /* successfully created a domain.
       p.ml_values is now owned by that domain */
    pthread_detach(th);
  } else {
    CAMLassert (p.status == Dom_failed);
    /* failed */
    pthread_join(th, 0);
    free_domain_ml_values(p.ml_values);
    caml_failwith("failed to allocate domain");
  }
  /* When domain 0 first spawns a domain, the backup thread is not active, we
     ensure it is started here. */
  install_backup_thread(domain_self);
  CAML_EV_END(EV_DOMAIN_SPAWN);
  CAMLreturn (Val_long(p.unique_id));
}

CAMLprim value caml_ml_domain_id(value unit)
{
  CAMLnoalloc;
  return Val_long(domain_self->interruptor.unique_id);
}

CAMLprim value caml_ml_domain_unique_token (value unit)
{
  return Val_unit;
}

/* sense-reversing barrier */
#define BARRIER_SENSE_BIT 0x100000

barrier_status caml_global_barrier_begin(void)
{
  uintnat b = 1 + atomic_fetch_add(&stw_request.barrier, 1);
  return b;
}

int caml_global_barrier_is_final(barrier_status b)
{
  return ((b & ~BARRIER_SENSE_BIT) == stw_request.num_domains);
}

void caml_global_barrier_end(barrier_status b)
{
  uintnat sense = b & BARRIER_SENSE_BIT;
  if (caml_global_barrier_is_final(b)) {
    /* last domain into the barrier, flip sense */
    atomic_store_rel(&stw_request.barrier, sense ^ BARRIER_SENSE_BIT);
  } else {
    /* wait until another domain flips the sense */
    SPIN_WAIT {
      uintnat barrier = atomic_load_acq(&stw_request.barrier);
      if ((barrier & BARRIER_SENSE_BIT) != sense) break;
    }
  }
}

void caml_global_barrier(void)
{
  barrier_status b = caml_global_barrier_begin();
  caml_global_barrier_end(b);
}

int caml_global_barrier_num_domains(void)
{
  return stw_request.num_domains;
}

static void decrement_stw_domains_still_processing(void)
{
  /* we check if we are the last to leave a stw section
     if so, clear the stw_leader to allow the new stw sections to start.
   */
  intnat am_last =
      atomic_fetch_add(&stw_request.num_domains_still_processing, -1) == 1;

  if( am_last ) {
    /* release the STW lock to allow new STW sections */
    caml_plat_lock(&all_domains_lock);
    atomic_store_rel(&stw_leader, 0);
    caml_plat_broadcast(&all_domains_cond);
    caml_gc_log("clearing stw leader");
    caml_plat_unlock(&all_domains_lock);
  }
}

static void caml_poll_gc_work(void);
static void stw_handler(caml_domain_state* domain)
{
  CAML_EV_BEGIN(EV_STW_HANDLER);
  CAML_EV_BEGIN(EV_STW_API_BARRIER);
  {
    SPIN_WAIT {
      if (atomic_load_acq(&stw_request.domains_still_running) == 0)
        break;

      if (stw_request.enter_spin_callback)
        stw_request.enter_spin_callback(domain, stw_request.enter_spin_data);
    }
  }
  CAML_EV_END(EV_STW_API_BARRIER);

  #ifdef DEBUG
  Caml_state->inside_stw_handler = 1;
  #endif
  stw_request.callback(
      domain,
      stw_request.data,
      stw_request.num_domains,
      stw_request.participating.base);
  #ifdef DEBUG
  Caml_state->inside_stw_handler = 0;
  #endif

  decrement_stw_domains_still_processing();

  CAML_EV_END(EV_STW_HANDLER);

  /* poll the GC to check for deferred work
     we do this here because blocking or waiting threads only execute
     the interrupt handler and do not poll for deferred work*/
  caml_poll_gc_work();
}


#ifdef DEBUG
int caml_domain_is_in_stw(void) {
  return Caml_state->inside_stw_handler;
}
#endif

int caml_try_run_on_all_domains_with_spin_work(
  void (*handler)(caml_domain_state*, void*, int, caml_domain_state**),
  void* data,
  void (*leader_setup)(caml_domain_state*),
  void (*enter_spin_callback)(caml_domain_state*, void*),
  void* enter_spin_data)
{
  int i;
  caml_domain_state* domain_state = domain_self->state;

  caml_gc_log("requesting STW");

  /* Don't touch the lock if there's already a stw leader
    OR we can't get the lock */
  if (atomic_load_acq(&stw_leader) ||
      !caml_plat_try_lock(&all_domains_lock)) {
    caml_handle_incoming_interrupts();
    return 0;
  }

  /* see if there is a stw_leader already */
  if (atomic_load_acq(&stw_leader)) {
    caml_plat_unlock(&all_domains_lock);
    caml_handle_incoming_interrupts();
    return 0;
  }

  /* we have the lock and can claim the stw_leader */
  atomic_store_rel(&stw_leader, (uintnat)domain_self);

  CAML_EV_BEGIN(EV_STW_LEADER);
  caml_gc_log("causing STW");

  /* setup all fields for this stw_request, must have those needed
     for domains waiting at the enter spin barrier */
  stw_request.enter_spin_callback = enter_spin_callback;
  stw_request.enter_spin_data = enter_spin_data;
  stw_request.callback = handler;
  stw_request.data = data;
  atomic_store_rel(&stw_request.barrier, 0);
  atomic_store_rel(&stw_request.domains_still_running, 1);
  stw_request.num_domains = stw_domains.participating_domains;
  atomic_store_rel(&stw_request.num_domains_still_processing,
                     stw_domains.participating_domains);

  if( leader_setup ) {
    leader_setup(domain_state);
  }

#ifdef DEBUG
  {
    int domains_participating = 0;
    for(i=0; i<caml_max_domains; i++) {
      if(get_domain(i)->interruptor.running)
        domains_participating++;
    }
    CAMLassert(domains_participating == stw_domains.participating_domains);
    CAMLassert(domains_participating > 0);
  }
#endif

  /* Next, interrupt all domains */
  for(i = 0; i < stw_domains.participating_domains; i++) {
    caml_domain_state* d = (*get_stw_domain_ptr(i))->state;
    *get_stw_participating_domain_ptr(i) = d;
    if (d != domain_state) {
      caml_send_interrupt(&(*get_stw_domain_ptr(i))->interruptor);
    } else {
      CAMLassert(!domain_self->interruptor.interrupt_pending);
    }
  }

  /* domains now know they are part of the STW */
  caml_plat_unlock(&all_domains_lock);

  for(i = 0; i < stw_request.num_domains; i++) {
    int id = (*get_stw_participating_domain_ptr(i))->id;
    caml_wait_interrupt_serviced(&get_domain(id)->interruptor);
  }

  /* release from the enter barrier */
  atomic_store_rel(&stw_request.domains_still_running, 0);

  #ifdef DEBUG
  domain_state->inside_stw_handler = 1;
  #endif
  handler(domain_state, data,
          stw_request.num_domains, stw_request.participating.base);
  #ifdef DEBUG
  domain_state->inside_stw_handler = 0;
  #endif

  decrement_stw_domains_still_processing();

  CAML_EV_END(EV_STW_LEADER);

  return 1;
}

int caml_try_run_on_all_domains(
  void (*handler)(caml_domain_state*, void*, int, caml_domain_state**),
  void* data,
  void (*leader_setup)(caml_domain_state*))
{
  return
      caml_try_run_on_all_domains_with_spin_work(handler,
                                                 data,
                                                 leader_setup, 0, 0);
}

void caml_interrupt_self(void) {
  interrupt_domain(&domain_self->interruptor);
}

static void caml_poll_gc_work(void)
{
  CAMLalloc_point_here;

  if (((uintnat)Caml_state->young_ptr - Bhsize_wosize(Max_young_wosize) <
       (uintnat)Caml_state->young_start) ||
      Caml_state->requested_minor_gc) {
    /* out of minor heap or collection forced */
    CAML_EV_BEGIN(EV_MINOR);
    Caml_state->requested_minor_gc = 0;
    caml_empty_minor_heaps_once();
    CAML_EV_END(EV_MINOR);

    /* FIXME: a domain will only ever call finalizers if its minor
      heap triggers the minor collection
      Care may be needed with finalizers running when the domain
      is waiting in a blocking section and serviced by the backup
      thread.
      */
    CAML_EV_BEGIN(EV_MINOR_FINALIZED);
    caml_final_do_calls();
    CAML_EV_END(EV_MINOR_FINALIZED);
  }

  if (Caml_state->requested_major_slice) {
    CAML_EV_BEGIN(EV_MAJOR);
    Caml_state->requested_major_slice = 0;
    caml_major_collection_slice(AUTO_TRIGGERED_MAJOR_SLICE);
    CAML_EV_END(EV_MAJOR);
  }

  if (atomic_load_acq(
          (atomic_uintnat*)&Caml_state->requested_external_interrupt)) {
    caml_domain_external_interrupt_hook();
  }

}

CAMLexport int caml_check_pending_actions (void)
{
  atomic_uintnat* young_limit = domain_self->interruptor.interrupt_word;

  return atomic_load_acq(young_limit) == INTERRUPT_MAGIC;
}

static void handle_gc_interrupt() {
  atomic_uintnat* young_limit = domain_self->interruptor.interrupt_word;
  CAMLalloc_point_here;

  CAML_EV_BEGIN(EV_INTERRUPT_GC);
  if (caml_check_pending_actions()) {
    /* interrupt */
    CAML_EV_BEGIN(EV_INTERRUPT_REMOTE);
    while (caml_check_pending_actions()) {
      uintnat i = INTERRUPT_MAGIC;
      atomic_compare_exchange_strong(
          young_limit, &i, (uintnat)Caml_state->young_start);
    }
    caml_handle_incoming_interrupts();
    CAML_EV_END(EV_INTERRUPT_REMOTE);
  }

  caml_poll_gc_work();

  CAML_EV_END(EV_INTERRUPT_GC);
}

CAMLexport void caml_process_pending_actions(void)
{
  handle_gc_interrupt();
  caml_process_pending_signals();
}

void caml_handle_gc_interrupt_no_async_exceptions(void)
{
  handle_gc_interrupt();
}

void caml_handle_gc_interrupt(void)
{
  handle_gc_interrupt();
}

CAMLexport int caml_bt_is_in_blocking_section(void)
{
  dom_internal* self = domain_self;
  uintnat status = atomic_load_acq(&self->backup_thread_msg);
  if (status == BT_IN_BLOCKING_SECTION)
    return 1;
  else
    return 0;

}

CAMLexport intnat caml_domain_is_multicore (void)
{
  dom_internal *self = domain_self;
  return (!caml_domain_alone() || self->backup_thread_running);
}

CAMLexport void caml_acquire_domain_lock(void)
{
  dom_internal* self = domain_self;
  caml_plat_lock(&self->domain_lock);
}

CAMLexport void caml_bt_enter_ocaml(void)
{
  dom_internal* self = domain_self;

  CAMLassert(caml_domain_alone() || self->backup_thread_running);

  if (self->backup_thread_running) {
    atomic_store_rel(&self->backup_thread_msg, BT_ENTERING_OCAML);
  }
}

CAMLexport void caml_release_domain_lock(void)
{
  dom_internal* self = domain_self;
  caml_plat_unlock(&self->domain_lock);
}

CAMLexport void caml_bt_exit_ocaml(void)
{
  dom_internal* self = domain_self;

  CAMLassert(caml_domain_alone() || self->backup_thread_running);

  if (self->backup_thread_running) {
    atomic_store_rel(&self->backup_thread_msg, BT_IN_BLOCKING_SECTION);
    /* Wakeup backup thread if it is sleeping */
    caml_plat_signal(&self->domain_cond);
  }
}

/* default handler for unix_fork, will be called by unix_fork. */
static void caml_atfork_default(void) {
  caml_reset_domain_lock();
  caml_acquire_domain_lock();
}

CAMLexport void (*caml_atfork_hook)(void) = caml_atfork_default;

static void handover_ephemerons(caml_domain_state* domain_state)
{
  if (domain_state->ephe_info->todo == 0 &&
      domain_state->ephe_info->live == 0)
    return;

  caml_add_to_orphaned_ephe_list(domain_state->ephe_info);
  CAMLassert (domain_state->ephe_info->live == 0);
  CAMLassert (domain_state->ephe_info->todo == 0);
}

static void handover_finalisers(caml_domain_state* domain_state)
{
  struct caml_final_info* f = domain_state->final_info;

  if (f->todo_head != NULL || f->first.size != 0 || f->last.size != 0) {
    /* have some final structures */
    if (caml_gc_phase != Phase_sweep_and_mark_main) {
      /* Force a major GC cycle to simplify constraints for
       * handing over finalisers. */
      caml_finish_major_cycle();
      CAMLassert(caml_gc_phase == Phase_sweep_and_mark_main);
    }
    caml_add_orphaned_finalisers (f);
    /* Create a dummy final info */
    domain_state->final_info = caml_alloc_final_info();
  }
  caml_final_domain_terminate(domain_state);
}

int caml_domain_is_terminating (void)
{
  struct interruptor* s = &domain_self->interruptor;
  return s->terminating;
}

static void domain_terminate (void)
{
  caml_domain_state* domain_state = domain_self->state;
  struct interruptor* s = &domain_self->interruptor;
  int finished = 0;

  caml_gc_log("Domain terminating");
  s->terminating = 1;

  // run the domain termination hook
  caml_domain_stop_hook();

  while (!finished) {
    caml_orphan_allocated_words();
    caml_finish_sweeping();

    caml_empty_minor_heaps_once();

    caml_finish_marking();
    handover_ephemerons(domain_state);
    handover_finalisers(domain_state);

    /* take the all_domains_lock to try and exit the STW participant set
       without racing with a STW section being triggered */
    caml_plat_lock(&all_domains_lock);

    /* The interaction of termination and major GC is quite subtle.
     *
     * At the end of the major GC, we decide the number of domains to mark and
     * sweep for the next cycle. If a STW section has been started, it will
     * require this domain to participate, which in turn could involve a
     * major GC cycle. This would then require finish marking and sweeping
     * again in order to decrement the globals [num_domains_to_mark] and
     * [num_domains_to_sweep] (see major_gc.c).
     */

    if (!caml_incoming_interrupts_queued() &&
        domain_state->marking_done &&
        domain_state->sweeping_done) {

      finished = 1;
      s->terminating = 0;
      s->running = 0;

      /* Remove this domain from stw_domains */
      remove_from_stw_domains(domain_self);

      /* signal the interruptor condition variable
       * because the backup thread may be waiting on it
       */
      caml_plat_lock(&s->lock);
      caml_plat_broadcast(&s->cond);
      caml_plat_unlock(&s->lock);

      CAMLassert (domain_self->backup_thread_running);
      domain_self->backup_thread_running = 0;
    }
    caml_plat_unlock(&all_domains_lock);
  }
  /* We can not touch domain_self->interruptor after here
     because it may be reused */
  caml_sample_gc_collect(domain_state);
  caml_remove_generational_global_root(&domain_state->dls_root);
  caml_remove_generational_global_root(&domain_state->backtrace_last_exn);

  caml_stat_free(domain_state->final_info);
  caml_stat_free(domain_state->ephe_info);
  caml_free_intern_state();
  caml_free_extern_state();
  caml_teardown_major_gc();
  CAML_EVENTLOG_TEARDOWN();
  caml_teardown_shared_heap(domain_state->shared_heap);
  domain_state->shared_heap = 0;
  caml_free_minor_tables(domain_state->minor_tables);
  domain_state->minor_tables = 0;
  caml_free_signal_stack();

  if(domain_state->current_stack != NULL) {
    caml_free_stack(domain_state->current_stack);
  }

  /* signal the domain termination to the backup thread
     NB: for a program with no additional domains, the backup thread
     will not have been started */
  atomic_store_rel(&domain_self->backup_thread_msg, BT_TERMINATE);
  caml_plat_signal(&domain_self->domain_cond);
  caml_plat_unlock(&domain_self->domain_lock);

  caml_plat_assert_all_locks_unlocked();
  /* This is the last thing we do because we need to be able to rely
     on caml_domain_alone (which uses caml_num_domains_running) in at least
     the shared_heap lockfree fast paths */
  atomic_fetch_add(&caml_num_domains_running, -1);
}

CAMLprim value caml_ml_domain_cpu_relax(value t)
{
  struct interruptor* self = &domain_self->interruptor;
  handle_incoming_otherwise_relax (self);
  return Val_unit;
}

CAMLprim value caml_domain_dls_set(value t)
{
  CAMLnoalloc;
  caml_modify_generational_global_root(&Caml_state->dls_root, t);
  return Val_unit;
}

CAMLprim value caml_domain_dls_get(value unused)
{
  CAMLnoalloc;
  return Caml_state->dls_root;
}

CAMLprim value caml_ml_domain_set_name(value name)
{
  CAMLparam1(name);

  if (caml_string_length(name) >= MAX_DOMAIN_NAME_LENGTH)
    caml_invalid_argument("caml_ml_domain_set_name");
  caml_thread_setname(String_val(name));
  CAMLreturn(Val_unit);
}
