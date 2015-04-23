#include "types.h"
#include "globals.h"
#include "kernel.h"
#include "errno.h"

#include "util/debug.h"

#include "proc/proc.h"

#include "mm/mm.h"
#include "mm/mman.h"
#include "mm/page.h"
#include "mm/mmobj.h"
#include "mm/pframe.h"
#include "mm/pagetable.h"

#include "vm/pagefault.h"
#include "vm/vmmap.h"

/*
 * This gets called by _pt_fault_handler in mm/pagetable.c The
 * calling function has already done a lot of error checking for
 * us. In particular it has checked that we are not page faulting
 * while in kernel mode. Make sure you understand why an
 * unexpected page fault in kernel mode is bad in Weenix. You
 * should probably read the _pt_fault_handler function to get a
 * sense of what it is doing.
 *
 * Before you can do anything you need to find the vmarea that
 * contains the address that was faulted on. Make sure to check
 * the permissions on the area to see if the process has
 * permission to do [cause]. If either of these checks does not
 * pass kill the offending process, setting its exit status to
 * EFAULT (normally we would send the SIGSEGV signal, however
 * Weenix does not support signals).
 *
 * Now it is time to find the correct page. Make sure that if the
 * user writes to the page it will be handled correctly. This
 * includes your shadow objects' copy-on-write magic working
 * correctly.
 *
 * Finally call pt_map to have the new mapping placed into the
 * appropriate page table.
 *
 * @param vaddr the address that was accessed to cause the fault
 *
 * @param cause this is the type of operation on the memory
 *              address which caused the fault, possible values
 *              can be found in pagefault.h
 */
void handle_pagefault(uintptr_t vaddr, uint32_t cause) {
  dbg(DBG_VM, "vaddr: %p\n", vaddr);
  char out[1024];
  vmmap_mapping_info(curproc->p_vmmap, &out, 1024);
  dbg(DBG_VM, "\n%.*s\n", 1024, &out);
  uint32_t pn = ADDR_TO_PN(vaddr);
  vmarea_t *vma = vmmap_lookup(curproc->p_vmmap, pn);
  if (!vma) { // Page not mapped
    dbg(DBG_VM, "SEGFAULT!\n");
    proc_kill(curproc, EFAULT);
    return;
  }
  // Check permissions
  if (((cause & FAULT_WRITE) && !(PROT_WRITE & vma->vma_prot)) ||
      ((cause & FAULT_EXEC) && !(PROT_EXEC & vma->vma_prot))) {
    dbg(DBG_VM, "SEGFAULT!\n");
    proc_kill(curproc, EFAULT);
    return;
  }
  pframe_t *pf;
  // TODO: lookup page (for shadow objects)
  int status = pframe_get(vma->vma_obj, 
      pn - vma->vma_start + vma->vma_off, &pf);
  // TODO: check flags, handle copy on write
  int flags = PD_PRESENT | PD_USER;
  if (PROT_WRITE & vma->vma_prot) flags |= PD_WRITE;
  if (cause & FAULT_WRITE) pframe_dirty(pf);
  if (pt_map(curproc->p_pagedir, PAGE_ALIGN_DOWN(vaddr), 
        pt_virt_to_phys((uintptr_t)pf->pf_addr), flags, flags)) {
    proc_kill(curproc, ENOMEM);
  }
}
