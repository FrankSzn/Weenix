#include "globals.h"
#include "errno.h"

#include "util/string.h"
#include "util/debug.h"

#include "mm/mmobj.h"
#include "mm/pframe.h"
#include "mm/mm.h"
#include "mm/page.h"
#include "mm/slab.h"
#include "mm/tlb.h"

int anon_count = 0; /* for debugging/verification purposes */

static slab_allocator_t *anon_allocator;

static void anon_ref(mmobj_t *o);
static void anon_put(mmobj_t *o);
static int anon_lookuppage(mmobj_t *o, uint32_t pagenum, int forwrite,
                           pframe_t **pf);
static int anon_fillpage(mmobj_t *o, pframe_t *pf);
static int anon_dirtypage(mmobj_t *o, pframe_t *pf);
static int anon_cleanpage(mmobj_t *o, pframe_t *pf);

static mmobj_ops_t anon_mmobj_ops = {.ref = anon_ref,
                                     .put = anon_put,
                                     .lookuppage = anon_lookuppage,
                                     .fillpage = anon_fillpage,
                                     .dirtypage = anon_dirtypage,
                                     .cleanpage = anon_cleanpage};

/*
 * This function is called at boot time to initialize the
 * anonymous page sub system. Currently it only initializes the
 * anon_allocator object.
 */
void anon_init() { 
  anon_allocator = slab_allocator_create("anon", sizeof(mmobj_t));
  KASSERT(NULL != anon_allocator && "failed to create anon allocator!");
}

/*
 * You'll want to use the anon_allocator to allocate the mmobj to
 * return, then initialize it. Take a look in mm/mmobj.h for
 * definitions which can be of use here. Make sure your initial
 * reference count is correct.
 */
mmobj_t *anon_create() {
  mmobj_t *anon = (mmobj_t *)slab_obj_alloc(anon_allocator);
  if (anon) {
    mmobj_init(anon, &anon_mmobj_ops);
    ++anon->mmo_refcount; // initial refcount is 1
  }
  return anon;
}

/* Implementation of mmobj entry points: */

/*
 * Increment the reference count on the object.
 */
static void anon_ref(mmobj_t *o) { 
  dbg(DBG_ANON, "up to %d\n", ++o->mmo_refcount);
}

/*
 * Decrement the reference count on the object. If, however, the
 * reference count on the object reaches the number of resident
 * pages of the object, we can conclude that the object is no
 * longer in use and, since it is an anonymous object, it will
 * never be used again. You should unpin and uncache all of the
 * object's pages and then free the object itself.
 */
static void anon_put(mmobj_t *o) {
  KASSERT(o->mmo_refcount > 0);
  dbg(DBG_ANON, "mmobj %p down to %d, respages %d\n", 
      o, o->mmo_refcount - 1, o->mmo_nrespages);
  if (o->mmo_refcount -1 == o->mmo_nrespages) {
    pframe_t *pf;
    list_iterate_begin(&o->mmo_respages, pf, pframe_t, pf_olink) {
      pframe_unpin(pf);
      pframe_free(pf);
    } list_iterate_end();
    --o->mmo_refcount;
    KASSERT(0 == o->mmo_nrespages);
    slab_obj_free(anon_allocator, o);
  }
}

/* Get the corresponding page from the mmobj. No special handling is
 * required. */
static int anon_lookuppage(mmobj_t *o, uint32_t pagenum, int forwrite,
    pframe_t **pf) {
  dbg(DBG_ANON, "o: %p\n", o);
  KASSERT(NULL != pf);
  KASSERT(NULL != o);
  return pframe_get(o, pagenum, pf);
}

/* The following three functions should not be difficult. */

static int anon_fillpage(mmobj_t *o, pframe_t *pf) {
  dbg(DBG_ANON, "o: %p\n", o);
  KASSERT(pframe_is_busy(pf));
  pframe_pin(pf); // TODO: is this correct?
  memset(pf->pf_addr, 0, PAGE_SIZE);
  return 0;
}

static int anon_dirtypage(mmobj_t *o, pframe_t *pf) {
  dbg(DBG_ANON, "\n");
  return 0;
}

static int anon_cleanpage(mmobj_t *o, pframe_t *pf) {
  return 0;
}
