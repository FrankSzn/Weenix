#include "kernel.h"
#include "errno.h"
#include "globals.h"

#include "vm/vmmap.h"
#include "vm/shadow.h"
#include "vm/anon.h"

#include "proc/proc.h"

#include "util/debug.h"
#include "util/list.h"
#include "util/string.h"
#include "util/printf.h"

#include "fs/vnode.h"
#include "fs/file.h"
#include "fs/fcntl.h"
#include "fs/vfs_syscall.h"

#include "mm/slab.h"
#include "mm/page.h"
#include "mm/mm.h"
#include "mm/mman.h"
#include "mm/mmobj.h"

static slab_allocator_t *vmmap_allocator;
static slab_allocator_t *vmarea_allocator;

void vmmap_init(void) {
  vmmap_allocator = slab_allocator_create("vmmap", sizeof(vmmap_t));
  KASSERT(NULL != vmmap_allocator && "failed to create vmmap allocator!");
  vmarea_allocator = slab_allocator_create("vmarea", sizeof(vmarea_t));
  KASSERT(NULL != vmarea_allocator && "failed to create vmarea allocator!");
}

vmarea_t *vmarea_alloc(void) {
  vmarea_t *newvma = (vmarea_t *)slab_obj_alloc(vmarea_allocator);
  if (newvma) {
    newvma->vma_vmmap = NULL;
    list_link_init(&newvma->vma_olink);
    list_link_init(&newvma->vma_plink);
  }
  return newvma;
}

void vmarea_free(vmarea_t *vma) {
  KASSERT(NULL != vma);
  slab_obj_free(vmarea_allocator, vma);
}

/* Create a new vmmap, which has no vmareas and does
 * not refer to a process. */
vmmap_t *vmmap_create(void) {
  dbg(DBG_VMMAP, "\n");
  vmmap_t *vmmap = slab_obj_alloc(vmmap_allocator);
  KASSERT(vmmap);
  list_init(&vmmap->vmm_list);
  vmmap->vmm_proc = NULL;
  return vmmap;
}

/* Removes all vmareas from the address space and frees the
 * vmmap struct. */
void vmmap_destroy(vmmap_t *map) { 
  dbg(DBG_VMMAP, "\n");
  KASSERT(map);
  vmarea_t *vma;
  list_iterate_begin(&map->vmm_list, vma, vmarea_t, vma_plink) {
    dbg(DBG_VMMAP, "0x%p %p\n", vma->vma_obj, vma->vma_obj->mmo_shadowed);
    KASSERT(list_link_is_linked(&vma->vma_olink));
    vma->vma_obj->mmo_ops->put(vma->vma_obj);
    list_remove(&vma->vma_olink);
    list_remove(&vma->vma_plink);
    vmarea_free(vma);
  } list_iterate_end();
  slab_obj_free(vmmap_allocator, map);
}

/* Add a vmarea to an address space. Assumes (i.e. asserts to some extent)
 * the vmarea is valid.  This involves finding where to put it in the list
 * of VM areas, and adding it. Don't forget to set the vma_vmmap for the
 * area. */
void vmmap_insert(vmmap_t *map, vmarea_t *newvma) {
  dbg(DBG_VMMAP, "map: %p start: %p\n", map, (void *)(newvma->vma_start * PAGE_SIZE));
  vmarea_t *vma;
  list_iterate_begin(&map->vmm_list, vma, vmarea_t, vma_plink) {
    if (newvma->vma_start < vma->vma_start) {
      dbg(DBG_VMMAP, "inserting before: %p\n", (void *)(vma->vma_start * PAGE_SIZE));
      list_insert_before(&vma->vma_plink, &newvma->vma_plink);
      goto end;
    }
  } list_iterate_end();
  list_insert_tail(&map->vmm_list, &newvma->vma_plink);
end:
  newvma->vma_vmmap = map;
}

/* Find a contiguous range of free virtual pages of length npages in
 * the given address space. Returns starting vfn for the range,
 * without altering the map. Returns -1 if no such range exists.
 *
 * Your algorithm should be first fit. If dir is VMMAP_DIR_HILO, you
 * should find a gap as high in the address space as possible; if dir
 * is VMMAP_DIR_LOHI, the gap should be as low as possible. */
int vmmap_find_range(vmmap_t *map, uint32_t npages, int dir) {
  dbg(DBG_VMMAP, "npages: %d\n", npages);
  KASSERT(dir == VMMAP_DIR_LOHI || dir == VMMAP_DIR_HILO);
  KASSERT(map);
  KASSERT(npages);
  vmarea_t *vma;
  // Walk the list, checking if there is enough space between regions
  if (dir == VMMAP_DIR_LOHI) { // Low as possible
    size_t low = ADDR_TO_PN(USER_MEM_LOW);
    list_iterate_begin(&map->vmm_list, vma, vmarea_t, vma_plink) {
      size_t high = vma->vma_start; 
      if ((high - low) >= npages)
        return low;
      low = vma->vma_end;
    } list_iterate_end();
    if ((ADDR_TO_PN(USER_MEM_HIGH) - low) >= npages)
      return low;
  } else { // High as possible
    size_t high = ADDR_TO_PN(USER_MEM_HIGH);
    list_iterate_reverse(&map->vmm_list, vma, vmarea_t, vma_plink) {
      size_t low = vma->vma_end;
      if ((high - low) >= npages) {
        return high - npages;
      }
      high = vma->vma_start;
    } list_iterate_end();
    if ((high - ADDR_TO_PN(USER_MEM_LOW)) >= npages)
      return high - npages;
  }
  dbg(DBG_VMMAP, "no region large enough!\n");
  return -1;
}

/* Find the vm_area that vfn lies in. Simply scan the address space
 * looking for a vma whose range covers vfn. If the page is unmapped,
 * return NULL. */
// CONVENTION: page number, not address
vmarea_t *vmmap_lookup(vmmap_t *map, uint32_t vfn) {
  //dbg(DBG_VMMAP, "map: 0x%p vaddr: 0x%p\n", map, vfn*PAGE_SIZE);
  vmarea_t *vma;
  list_iterate_begin(&map->vmm_list, vma, vmarea_t, vma_plink) {
    if (vma->vma_start <= vfn && vfn < vma->vma_end)
      return vma;
  } list_iterate_end();
  //dbg(DBG_VMMAP, "not found!\n");
  return NULL;
}

/* Allocates a new vmmap containing a new vmarea for each area in the
 * given map. The areas should have no mmobjs set yet. Returns pointer
 * to the new vmmap on success, NULL on failure. This function is
 * called when implementing fork(2). */
vmmap_t *vmmap_clone(vmmap_t *map) {
  dbg(DBG_VMMAP, "\n");
  vmmap_t *new_map = vmmap_create();
  KASSERT(new_map);
  new_map->vmm_proc = map->vmm_proc;
  vmarea_t *vma;
  list_iterate_begin(&map->vmm_list, vma, vmarea_t, vma_plink) {
    vmarea_t *new_vma = vmarea_alloc();
    KASSERT(new_vma);
    memcpy(new_vma, vma, sizeof(vmarea_t));
    new_vma->vma_vmmap = new_map;
    vma->vma_obj->mmo_ops->ref(vma->vma_obj); // Incr. refcount
    list_insert_tail(&new_map->vmm_list, &new_vma->vma_plink);
  } list_iterate_end();
  return new_map;
}

/* Insert a mapping into the map starting at lopage for npages pages.
 * If lopage is zero, we will find a range of virtual addresses in the
 * process that is big enough, by using vmmap_find_range with the same
 * dir argument.  If lopage is non-zero and the specified region
 * contains another mapping that mapping should be unmapped.
 *
 * If file is NULL an anon mmobj will be used to create a mapping
 * of 0's.  If file is non-null that vnode's file will be mapped in
 * for the given range.  Use the vnode's mmap operation to get the
 * mmobj for the file; do not assume it is file->vn_obj. Make sure all
 * of the area's fields except for vma_obj have been set before
 * calling mmap.
 *
 * If MAP_PRIVATE is specified set up a shadow object for the mmobj.
 *
 * All of the input to this function should be valid (KASSERT!).
 * See mmap(2) for for description of legal input.
 * Note that off should be page aligned.
 *
 * Be very careful about the order operations are performed in here. Some
 * operation are impossible to undo and should be saved until there
 * is no chance of failure.
 *
 * If 'new' is non-NULL a pointer to the new vmarea_t should be stored in it.
 */
int vmmap_map(vmmap_t *map, vnode_t *file, uint32_t lopage, uint32_t npages,
              int prot, int flags, off_t off, int dir, vmarea_t **new) {
  dbg(DBG_VMMAP, "lopage: 0x%p npages: %d\n", (void *)lopage, npages);
  if (file) dbg(DBG_VMMAP, "vno %d\n", file->vn_vno);
  // Validate input
  KASSERT(flags & MAP_PRIVATE || flags & MAP_SHARED);
  KASSERT(PAGE_ALIGNED(off));
  KASSERT(npages);
  vmarea_t *new_area;
  int unmap = 0;
  if (!lopage) { // Find a region big enough
    int status = vmmap_find_range(map, npages, dir);
    if (status == -1) {
      dbg(DBG_VMMAP, "error\n");
      return -ENOMEM; 
    }
    lopage = status;
    KASSERT(vmmap_lookup(map, lopage) == NULL);
  } else { // Unmap overlapping area if one exists
    unmap = 1;
  }
  // Initialize new vmarea
  new_area = vmarea_alloc();
  KASSERT(new_area);
  new_area->vma_prot = prot;
  new_area->vma_start = lopage;
  new_area->vma_end = lopage + npages;
  new_area->vma_off = off;
  new_area->vma_flags = flags;

  if (file) {
    if (flags & MAP_PRIVATE) { // Private mapping (shadow object)
      mmobj_t *shadow_obj = shadow_create();
      dbg(DBG_VMMAP, "allocated shadow object: 0x%p\n", shadow_obj);
      KASSERT(shadow_obj);
      new_area->vma_obj = shadow_obj;
      mmobj_t *file_obj;
      file->vn_ops->mmap(file, new_area, &file_obj);
      KASSERT(file_obj);
      shadow_obj->mmo_shadowed = file_obj;
      shadow_obj->mmo_un.mmo_bottom_obj = file_obj; // link to bottom
    } else { // Not a shadow object
      file->vn_ops->mmap(file, new_area, &new_area->vma_obj);
    }
  } else { // No file (anonymous)
    new_area->vma_obj = anon_create();
  }
  KASSERT(new_area->vma_obj);
  // Link to vmareas with the same bottom object
  list_insert_tail(mmobj_bottom_vmas(new_area->vma_obj), &new_area->vma_olink);

  if (unmap) {
    vmarea_t *existing;
    if ((existing = vmmap_lookup(map, lopage)))
      vmmap_remove(map, lopage, npages);
    KASSERT(vmmap_lookup(map, lopage) == NULL);
  }

  vmmap_insert(map, new_area);

  dbginfo(DBG_VMMAP, vmmap_mapping_info, curproc->p_vmmap);

  if (new) *new = new_area;

  return 0;
}

/*
 * We have no guarantee that the region of the address space being
 * unmapped will play nicely with our list of vmareas.
 *
 * You must iterate over each vmarea that is partially or wholly covered
 * by the address range [addr ... addr+len). The vm-area will fall into one
 * of four cases, as illustrated below:
 *
 * key:
 *          [             ]   Existing VM Area
 *        *******             Region to be unmapped
 *
 * Case 1:  [   ******    ]
 * The region to be unmapped lies completely inside the vmarea. We need to
 * split the old vmarea into two vmareas. be sure to increment the
 * reference count to the file associated with the vmarea.
 *
 * Case 2:  [      *******]**
 * The region overlaps the end of the vmarea. Just shorten the length of
 * the mapping.
 *
 * Case 3: *[*****        ]
 * The region overlaps the beginning of the vmarea. Move the beginning of
 * the mapping (remember to update vma_off), and shorten its length.
 *
 * Case 4: *[*************]**
 * The region completely contains the vmarea. Remove the vmarea from the
 * list.
 */
int vmmap_remove(vmmap_t *map, const uint32_t lopage, const uint32_t npages) {
  uint32_t highpage = lopage + npages;
  dbg(DBG_VMMAP, "0x%p - 0x%p\n", (void *)(lopage*PAGE_SIZE), (void *)(highpage*PAGE_SIZE));
  KASSERT(npages);
  vmarea_t *vma;


  list_iterate_begin(&map->vmm_list, vma, vmarea_t, vma_plink) {
    dbg(DBG_VMMAP, "0x%p - 0x%p\n", (void *)(vma->vma_start*PAGE_SIZE), 
        (void *)(vma->vma_end*PAGE_SIZE));
    if (vma->vma_start < lopage) {
      if (highpage < vma->vma_end) { // Case 1, split area
        dbg(DBG_VMMAP, "case 1\n");
        vmarea_t *new_vma = vmarea_alloc();
        KASSERT(new_vma);
        memcpy(new_vma, vma, sizeof(vmarea_t));
        new_vma->vma_obj->mmo_ops->ref(new_vma->vma_obj);
        vma->vma_end = lopage;
        new_vma->vma_start = highpage;
        vmmap_insert(map, new_vma);
        KASSERT(vma->vma_end - vma->vma_start);
        KASSERT(new_vma->vma_end - new_vma->vma_start);
      } else if (lopage < vma->vma_end) { // Case 2
        dbg(DBG_VMMAP, "case 2\n");
        vma->vma_end = lopage;
        KASSERT(vma->vma_end - vma->vma_start);
      }
    } else if (vma->vma_start <= highpage && highpage < vma->vma_end) { // Case 3
      dbg(DBG_VMMAP, "case 3\n");
      vma->vma_off -= highpage - vma->vma_start;
      vma->vma_start = highpage;
      KASSERT(vma->vma_end - vma->vma_start);
    } else if (vma->vma_end <= highpage) { // Case 4
      dbg(DBG_VMMAP, "case 4\n");
      vma->vma_obj->mmo_ops->put(vma->vma_obj);
      list_remove(&vma->vma_plink);
      list_remove(&vma->vma_olink);
      vmarea_free(vma); 
    }
  } list_iterate_end();
  KASSERT(!vmmap_lookup(map, lopage));
  KASSERT(!vmmap_lookup(map, lopage + npages - 1));
  return 0;
}

/*
 * Returns 1 if the given address space has no mappings for the
 * given range, 0 otherwise.
 */
int vmmap_is_range_empty(vmmap_t *map, uint32_t startvfn, uint32_t npages) {
  //dbg(DBG_VMMAP, "startvfn: %d npages: %d\n", startvfn, npages);
  uint32_t endvfn = startvfn + npages;
  vmarea_t *vma;
  list_iterate_begin(&map->vmm_list, vma, vmarea_t, vma_plink) {
    /* Cases:
     * range contains either end of vma
     * vma contains either end of range */
    if ((startvfn <= vma->vma_start && vma->vma_start < endvfn) || 
        (startvfn < vma->vma_end && vma->vma_end <= endvfn) ||
        (vma->vma_start <= startvfn && startvfn < vma->vma_end))
      return 0;
  } list_iterate_end();
  return 1;
}

// Abstraction for reading and writing
int vmmap_iop(vmmap_t *map, const void *vaddr, void *buf, size_t count, int write) {
  dbg(DBG_VMMAP, "vaddr: %p count: %d\n", vaddr, count);
  int ndone_total = 0;
  while (count) {
    // Find the page
    uint32_t pagenum = ADDR_TO_PN((char *)vaddr + ndone_total);
    vmarea_t *vma = vmmap_lookup(map, pagenum);
    KASSERT(vma);
    pframe_t *pframe;
    pframe_lookup(vma->vma_obj, pagenum - vma->vma_start + vma->vma_off, 
        write, &pframe);
    KASSERT(pframe && pframe->pf_addr);

    size_t offset = PAGE_OFFSET((char *)vaddr + ndone_total);
    size_t ndone = MIN(count, PAGE_SIZE - offset);

    // Do the operation on this page
    if (write) {
      pframe_dirty(pframe);
      memcpy((char *)pframe->pf_addr + offset, (char *)buf + ndone_total, ndone);
    } else {
      memcpy((char *)buf + ndone_total, (char *)pframe->pf_addr + offset, ndone);
    }
    // Update counters
    count -= ndone;
    ndone_total += ndone;
  }
  //dbg(DBG_VMMAP, "did %d bytes\n", ndone_total);
  return 0;
}

/* Read into 'buf' from the virtual address space of 'map' starting at
 * 'vaddr' for size 'count'. To do so, you will want to find the vmareas
 * to read from, then find the pframes within those vmareas corresponding
 * to the virtual addresses you want to read, and then read from the
 * physical memory that pframe points to. You should not check permissions
 * of the areas. Assume (KASSERT) that all the areas you are accessing exist.
 * Returns 0 on success, -errno on error.
 */
int vmmap_read(vmmap_t *map, const void *vaddr, void *buf, size_t count) {
  return vmmap_iop(map, (void *)vaddr, buf, count, 0);
}

/* Write from 'buf' into the virtual address space of 'map' starting at
 * 'vaddr' for size 'count'. To do this, you will need to find the correct
 * vmareas to write into, then find the correct pframes within those vmareas,
 * and finally write into the physical addresses that those pframes correspond
 * to. You should not check permissions of the areas you use. Assume (KASSERT)
 * that all the areas you are accessing exist. Remember to dirty pages!
 * Returns 0 on success, -errno on error.
 */
int vmmap_write(vmmap_t *map, void *vaddr, const void *buf, size_t count) {
  return vmmap_iop(map, vaddr, (void *)buf, count, 1);
}

/* a debugging routine: dumps the mappings of the given address space. */
size_t vmmap_mapping_info(const void *vmmap, char *buf, size_t osize) {
  KASSERT(0 < osize);
  KASSERT(NULL != buf);
  KASSERT(NULL != vmmap);

  vmmap_t *map = (vmmap_t *)vmmap;
  vmarea_t *vma;
  ssize_t size = (ssize_t)osize;

  int len = snprintf(buf, size, "%21s %5s %7s %8s %10s %12s\n", "VADDR RANGE",
                     "PROT", "FLAGS", "MMOBJ", "OFFSET", "VFN RANGE");

  list_iterate_begin(&map->vmm_list, vma, vmarea_t, vma_plink) {
    size -= len;
    buf += len;
    if (0 >= size) {
      goto end;
    }

    len =
        snprintf(buf, size, "%#.8x-%#.8x  %c%c%c  %7s 0x%p %#.5x %#.5x-%#.5x\n",
                 vma->vma_start << PAGE_SHIFT, vma->vma_end << PAGE_SHIFT,
                 (vma->vma_prot & PROT_READ ? 'r' : '-'),
                 (vma->vma_prot & PROT_WRITE ? 'w' : '-'),
                 (vma->vma_prot & PROT_EXEC ? 'x' : '-'),
                 (vma->vma_flags & MAP_SHARED ? " SHARED" : "PRIVATE"),
                 vma->vma_obj, vma->vma_off, vma->vma_start, vma->vma_end);
  }
  list_iterate_end();

end:
  if (size <= 0) {
    size = osize;
    buf[osize - 1] = '\0';
  }
  /*
  KASSERT(0 <= size);
  if (0 == size) {
          size++;
          buf--;
          buf[0] = '\0';
  }
  */
  return osize - size;
}
