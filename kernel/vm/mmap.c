#include "globals.h"
#include "errno.h"
#include "types.h"

#include "mm/mm.h"
#include "mm/tlb.h"
#include "mm/mman.h"
#include "mm/page.h"

#include "proc/proc.h"

#include "util/string.h"
#include "util/debug.h"

#include "fs/vnode.h"
#include "fs/vfs.h"
#include "fs/file.h"
#include "fs/stat.h"

#include "vm/vmmap.h"
#include "vm/mmap.h"

/*
 * This function implements the mmap(2) syscall, but only
 * supports the MAP_SHARED, MAP_PRIVATE, MAP_FIXED, and
 * MAP_ANON flags.
 *
 * Add a mapping to the current process's address space.
 * You need to do some error checking; see the ERRORS section
 * of the manpage for the problems you should anticipate.
 * After error checking most of the work of this function is
 * done by vmmap_map(), but remember to clear the TLB.
 */
int do_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off,
            void **ret) {
  dbg(DBG_VM, "addr: %p\n", addr);
  if (!PAGE_ALIGNED(addr) || !PAGE_ALIGNED(off) || !len) {
    dbg(DBG_VM, "error len: %d off: %d\n", len, off);
    return -EINVAL;
  }
  if (flags & MAP_FIXED && (addr < (void *)USER_MEM_LOW || addr >= (void *)USER_MEM_HIGH)) {
    dbg(DBG_VM, "error\n");
    return -EINVAL;
  }
  if (((flags & MAP_PRIVATE) && (flags & MAP_SHARED)) || 
      (!(flags & MAP_PRIVATE) && !(flags & MAP_SHARED))) {
    dbg(DBG_VM, "error\n");
    return -EINVAL;
  }
  // Set up file
  file_t *file;
  if (flags & MAP_ANON) {
    file = NULL;
  } else {
    file = fget(fd);
    if (!file) {
      dbg(DBG_VM, "error\n");
      return -EBADF;
    }
    if (!(file->f_mode & FMODE_READ) ||
        ((flags & MAP_SHARED) && (prot & PROT_WRITE) && 
         !(file->f_mode & FMODE_WRITE)) ||
        ((prot & PROT_WRITE) && (file->f_mode == FMODE_APPEND))) {
      dbg(DBG_VM, "error\n");
      fput(file);
      return -EACCES;
    }
  }

  vmarea_t *new_area = NULL;
  int status = vmmap_map(curproc->p_vmmap, file ? file->f_vnode : NULL, 
      ADDR_TO_PN(addr), (len-1) / PAGE_SIZE + 1, prot, 
      flags, off, VMMAP_DIR_HILO, &new_area);
  if (new_area) {
    pt_unmap_range(curproc->p_pagedir, new_area->vma_start << PAGE_SHIFT, 
        new_area->vma_end << PAGE_SHIFT);
    tlb_flush_range(PN_TO_ADDR(new_area->vma_start), 
        new_area->vma_end - new_area->vma_start);
  }
  if (file) fput(file);
  if (!status) *ret = PN_TO_ADDR(new_area->vma_start);
  return status;
}

/*
 * This function implements the munmap(2) syscall.
 *
 * As with do_mmap() it should perform the required error checking,
 * before calling upon vmmap_remove() to do most of the work.
 * Remember to clear the TLB.
 */
int do_munmap(void *addr, size_t len) {
  dbg(DBG_VM, "addr: %p len: %d\n", addr, len);
  if (!PAGE_ALIGNED(addr) || !len)
      return -EINVAL;
  if (addr < (void *)USER_MEM_LOW || addr >= (void *)USER_MEM_HIGH)
    return -EINVAL;
  int lopage = (uint32_t)addr/PAGE_SIZE;
  int npages = (len-1) / PAGE_SIZE + 1;
  if (lopage + npages > USER_MEM_HIGH >> PAGE_SHIFT)
    return -EINVAL;
  int status = vmmap_remove(curproc->p_vmmap, lopage, npages);
  // Clear caches
  pt_unmap_range(curproc->p_pagedir, lopage * PAGE_SIZE, 
      (lopage + npages) * PAGE_SIZE);
  tlb_flush_range(lopage * PAGE_SIZE, npages);
  return status;
}
