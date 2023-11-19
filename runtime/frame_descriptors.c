/**************************************************************************/
/*                                                                        */
/*                                 OCaml                                  */
/*                                                                        */
/*      KC Sivaramakrishnan, Indian Institute of Technology, Madras       */
/*                   Tom Kelly, OCaml Labs Consultancy                    */
/*                 Stephen Dolan, University of Cambridge                 */
/*                                                                        */
/*   Copyright 2019 Indian Institute of Technology, Madras                */
/*   Copyright 2021 OCaml Labs Consultancy Ltd                            */
/*   Copyright 2019 University of Cambridge                               */
/*                                                                        */
/*   All rights reserved.  This file is distributed under the terms of    */
/*   the GNU Lesser General Public License version 2.1, with the          */
/*   special exception on linking described in the file LICENSE.          */
/*                                                                        */
/**************************************************************************/

#define CAML_INTERNALS

#include "caml/platform.h"
#include "caml/frame_descriptors.h"
#include "caml/major_gc.h" /* for caml_major_cycles_completed */
#include "caml/memory.h"
#include "caml/fail.h"
#include "caml/shared_heap.h"
#include <stddef.h>

ATOMIC_PTR_TYPEDEF(frame_descr, atomic_frame_ptr)

struct caml_frame_descrs {
  int num_descr;
  int mask;
  atomic_frame_ptr* descriptors;
  caml_frametable_list *frametables;
};
/* Let us call 'capacity' the length of the descriptors array.

   We maintain the following invariants:
     capacity = mask + 1
     capacity = 0 || Is_power_of_2(capacity)
     num_desc <= 2 * num_descr <= capacity

   For an extensible array we would maintain
      num_desc <= capacity,
    but this is a linear-problem hash table, we need to ensure that
    free slots are frequent enough, so we use a twice-larger capacity:
      num_desc * 2 <= capacity

   We keep the list of frametables that was used to build the hashtable.
   We use it when rebuilding the table after resizing.
*/

/* Defined in code generated by ocamlopt */
extern intnat * caml_frametable[];

/* Note: [cur] is bound by this macro */
#define iter_list(list,cur) \
  for (caml_frametable_list *cur = list; cur != NULL; cur = cur->next)

static frame_descr * next_frame_descr(frame_descr * d) {
  unsigned char num_allocs = 0, *p;
  CAMLassert(d->retaddr >= 4096);
  if (!frame_return_to_C(d)) {
    /* Skip to end of live_ofs */
    p = (unsigned char*)&d->live_ofs[d->num_live];
    /* Skip alloc_lengths if present */
    if (frame_has_allocs(d)) {
      num_allocs = *p;
      p += num_allocs + 1;
    }
    /* Skip debug info if present */
    if (frame_has_debug(d)) {
      /* Align to 32 bits */
      p = Align_to(p, uint32_t);
      p += sizeof(uint32_t) * (frame_has_allocs(d) ? num_allocs : 1);
    }
    /* Align to word size */
    p = Align_to(p, void*);
    return ((frame_descr*) p);
  } else {
    /* This marks the top of an ML stack chunk. Skip over empty
     * frame descriptor */
    /* Skip to address of zero-sized live_ofs */
    CAMLassert(d->num_live == 0);
    p = (unsigned char*)&d->live_ofs[0];
    /* Align to word size */
    p = Align_to(p, void*);
    return ((frame_descr*) p);
  }
}

static intnat count_descriptors(caml_frametable_list *list) {
  intnat num_descr = 0;
  iter_list(list,cur) {
    num_descr += *((intnat*) cur->frametable);
  }
  return num_descr;
}

static caml_frametable_list* frametables_list_tail(caml_frametable_list *list) {
  caml_frametable_list *tail = NULL;
  iter_list(list,cur) {
    tail = cur;
  }
  return tail;
}

static int capacity(caml_frame_descrs table) {
  int capacity = table.mask + 1;
  CAMLassert(capacity == 0 || Is_power_of_2(capacity));
  return capacity;
}

/* placeholder entry equivalent to a free place (NULL) */
/* retaddr = disjoint from any valid return address; */
static frame_descr dummy_descr = { (uintnat) &dummy_descr };

static void fill_hashtable(
  caml_frame_descrs *table, caml_frametable_list *new_frametables)
{
  iter_list(new_frametables,cur) {
    intnat * tbl = (intnat*) cur->frametable;
    intnat len = *tbl;
    frame_descr * d = (frame_descr *)(tbl + 1);
    for (intnat j = 0; j < len; j++) {
      uintnat h = Hash_retaddr(d->retaddr, table->mask);
      frame_descr * e = atomic_load_relaxed(table->descriptors + h);
      while (e != NULL && e != &dummy_descr) {
        h = (h+1) & table->mask;
        e = atomic_load_relaxed(table->descriptors + h);
      }
      atomic_store_relaxed(table->descriptors + h, d);
      d = next_frame_descr(d);
    }
  }

  atomic_thread_fence(memory_order_release);
}

static void realloc_frame_descriptors(
  caml_frame_descrs *table,
  caml_frametable_list *new_frametables)
{
  intnat num_descr = count_descriptors(new_frametables);

  intnat tblsize = 4;
  while (tblsize < 2 * num_descr) tblsize *= 2;

  table->num_descr = num_descr;
  table->mask = tblsize - 1;

  if (table->descriptors != NULL) caml_stat_free(table->descriptors);
  table->descriptors = (atomic_frame_ptr*)
    caml_stat_alloc_noexc(tblsize * sizeof(atomic_frame_ptr));
  if (table->descriptors == NULL) caml_raise_out_of_memory();

  for (intnat i = 0; i < tblsize; i++)
    atomic_store_relaxed(table->descriptors + i, ATOMIC_PTR_INIT(NULL));

  fill_hashtable(table, new_frametables);

  table->frametables = new_frametables;
}

/* the global shared hashtable of frame_descr */
/* each entry is an atomic pointers */
/* free places can be either NULL or the dummy entry */
/* reallocation is protected by STW sections */
/* a mutex makes concurrent modifications impossible */
/* concurrent read/write are allowed since each intermediate
   modified state is a valid state from a reading point of view */
static caml_frame_descrs current_frame_descrs = { 0, -1, NULL, NULL };
static caml_plat_mutex frame_mutex = PTHREAD_MUTEX_INITIALIZER;
static atomic_uintnat frame_readers = ATOMIC_UINTNAT_INIT(0);

static caml_frametable_list *cons(
  intnat *frametable, caml_frametable_list *tl)
{
  caml_frametable_list *li = caml_stat_alloc(sizeof(caml_frametable_list));
  li->frametable = frametable;
  li->next = tl;
  return li;
}

void caml_init_frame_descriptors(void)
{
  caml_frametable_list *frametables = NULL;
  for (int i = 0; caml_frametable[i] != 0; i++)
    frametables = cons(caml_frametable[i], frametables);

  CAMLassert(frametables != NULL);

  /* `init_frame_descriptors` is called from `init_gc`, before
     any mutator can run. We can mutate [current_frame_descrs]
     at will. */
  realloc_frame_descriptors(&current_frame_descrs, frametables);
  caml_plat_mutex_init(&frame_mutex);
}

typedef struct {
  caml_frame_descrs *table;
  caml_frametable_list *new_frametables;
  caml_frametable_list *tail;
  intnat increase;
} realloc_request;

static void realloc_frame_descriptors_from_stw_single(
    caml_domain_state* domain,
    void* data,
    int participating_count,
    caml_domain_state** participating)
{
  barrier_status b = caml_global_barrier_begin ();

  if (caml_global_barrier_is_final(b)) {
    realloc_request *request = data;
    caml_frame_descrs *table = request->table;
    caml_frametable_list *new_frametables = request->new_frametables;
    caml_frametable_list *tail = request->tail;
    intnat tblsize = capacity(*table), increase = request->increase;

    if(tblsize < (table->num_descr + increase) * 2) {
      /* Merge both lists */
      tail->next = table->frametables;

      realloc_frame_descriptors(table, new_frametables);
    } else {
      table->num_descr += increase;
      fill_hashtable(table, new_frametables);
      tail->next = table->frametables;
      table->frametables = new_frametables;
    }
  }

  caml_global_barrier_end(b);
}

static void add_frame_descriptors(
  caml_frame_descrs *table,
  caml_frametable_list *new_frametables)
{
  CAMLassert(new_frametables != NULL);

  caml_frametable_list *tail = frametables_list_tail(new_frametables);
  intnat increase = count_descriptors(new_frametables);
  intnat tblsize = capacity(*table);

  caml_plat_lock(&frame_mutex);

  /* The size of the hashtable is a power of 2 that must remain
     greater or equal to 2 times the number of descriptors. */

  /* Reallocate the caml_frame_descriptor table if it is too small */
  if(tblsize < (table->num_descr + increase) * 2) {
    caml_plat_unlock(&frame_mutex);
    realloc_request request = { table, new_frametables, tail, increase };
    do {} while (!caml_try_run_on_all_domains(
                 &realloc_frame_descriptors_from_stw_single, &request, 0));
  } else {
    table->num_descr += increase;
    fill_hashtable(table, new_frametables);
    tail->next = table->frametables;
    table->frametables = new_frametables;
    caml_plat_unlock(&frame_mutex);
  }
}

void caml_register_frametables(void ** frametables, int ntables)
{
  caml_frametable_list *new_frametables = NULL;
  for (int i = 0; i < ntables; i++)
    new_frametables = cons((intnat*)frametables[i], new_frametables);

  add_frame_descriptors(&current_frame_descrs, new_frametables);
}

void caml_register_frametable(void * frametables)
{
  caml_register_frametables(&frametables, 1);
}

static void invalid_entry(caml_frame_descrs * fds, frame_descr * e)
{
  frame_descr * d;
  uintnat h;

  h = Hash_retaddr(e->retaddr, fds->mask);
  while (1) {
    d = atomic_load_relaxed(fds->descriptors + h);
    if (d == e) {
      atomic_store_relaxed(fds->descriptors + h, &dummy_descr);
      /* placeholder that should not disturb caml_find_frame_descr */
      return;
    }
    h = (h+1) & fds->mask;
  }
}

static void remove_frame_descriptors(
  caml_frame_descrs * fds, void ** frametables, int ntables)
{
  intnat * frametable, len, decrease = 0;
  frame_descr * descr;
  caml_frametable_list ** previous;

  caml_plat_lock(&frame_mutex);

  for (int i = 0; i < ntables; i++) {
    frametable = frametables[i];
    len = *frametable;
    descr = (frame_descr *)(frametable + 1);
    for (intnat j = 0; j < len; j++) {
      invalid_entry(fds, descr);
      descr = next_frame_descr(descr);
    }
    decrease += len;
  }

  fds->num_descr -= decrease;

  previous = &(fds->frametables);

  iter_list(fds->frametables, current) {
  resume:
    for (int i = 0; i < ntables; i++) {
      if (current->frametable == frametables[i]) {
        *previous = current->next;
        caml_stat_free(current);
        ntables--;
        if (ntables == 0) goto release;
        current = *previous;
        frametables[i] = frametables[ntables];
        goto resume;
      }
    }
    previous = &(current->next);
  }

 release:
  caml_plat_unlock(&frame_mutex);

  /* wait for all readers to finish their work */
  /* this is to completely remove the worst ever "possible" scenario
     where a reader got the old value of the frame_descr address,
     is interrupted before checking the ret_addr field, to finally
     access the field while the memory block has now been freed by
     the caller of caml_unregister_frametables function... */
  /* still, concurrent read / remove must be extremely rare in practice
     so we should not be waiting too long */
  SPIN_WAIT {
    uintnat v = atomic_load_acquire(&frame_readers);
    if (CAMLlikely(v == 0)) break;
  }
}

void caml_unregister_frametables(void ** frametables, int ntables)
{
  remove_frame_descriptors(&current_frame_descrs, frametables, ntables);
}

void caml_unregister_frametable(void * frametables)
{
  caml_unregister_frametables(&frametables, 1);
}

caml_frame_descrs* caml_get_frame_descrs(void)
{
  return &current_frame_descrs;
}

frame_descr* caml_find_frame_descr(caml_frame_descrs * fds, uintnat pc)
{
  frame_descr * d;
  uintnat h;

  atomic_thread_fence(memory_order_acquire);

  atomic_fetch_add(&frame_readers, 1);

  h = Hash_retaddr(pc, fds->mask);
  while (1) {
    d = atomic_load_relaxed(fds->descriptors + h);
    if (d == NULL) break; /* can happen if some code compiled without -g */
    if (d->retaddr == pc) break;
    h = (h+1) & fds->mask;
  }

  atomic_fetch_sub(&frame_readers, 1);

  return d;
}
