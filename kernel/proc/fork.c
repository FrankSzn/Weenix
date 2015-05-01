#include "types.h"
#include "globals.h"
#include "errno.h"

#include "util/debug.h"
#include "util/string.h"

#include "proc/proc.h"
#include "proc/kthread.h"

#include "mm/mm.h"
#include "mm/mman.h"
#include "mm/page.h"
#include "mm/pframe.h"
#include "mm/mmobj.h"
#include "mm/pagetable.h"
#include "mm/tlb.h"

#include "fs/file.h"
#include "fs/vnode.h"

#include "vm/shadow.h"
#include "vm/vmmap.h"

#include "api/exec.h"

#include "main/interrupt.h"

/* Pushes the appropriate things onto the kernel stack of a newly forked thread
 * so that it can begin execution in userland_entry.
 * regs: registers the new thread should have on execution
 * kstack: location of the new thread's kernel stack
 * Returns the new stack pointer on success. */
static uint32_t fork_setup_stack(const regs_t *regs, void *kstack) {
  /* Pointer argument and dummy return address, and userland dummy return
   * address */
  uint32_t esp =
      ((uint32_t)kstack) + DEFAULT_STACK_SIZE - (sizeof(regs_t) + 12);
  *(void **)(esp + 4) = (void *)(esp + 8); /* Set the argument to point to
                                              location of struct on stack */
  memcpy((void *)(esp + 8), regs, sizeof(regs_t)); /* Copy over struct */
  return esp;
}

/*
 * The implementation of fork(2). Once this works,
 * you're practically home free. This is what the
 * entirety of Weenix has been leading up to.
 * Go forth and conquer.
 */
int do_fork(struct regs *regs) {
  dbg(DBG_FORK, "\n");
  // Set up new proc
  proc_t *new_proc = proc_create("");
  KASSERT(new_proc);
  memcpy(&new_proc->p_comm, curproc->p_comm, PROC_NAME_LEN);
  new_proc->p_status = curproc->p_status;
  new_proc->p_state = curproc->p_state;
  new_proc->p_pagedir = pt_create_pagedir();
  KASSERT(new_proc->p_pagedir);
  new_proc->p_brk = curproc->p_brk;
  new_proc->p_start_brk = curproc->p_start_brk;
  new_proc->p_cwd = curproc->p_cwd;
  new_proc->p_vmmap = vmmap_clone(curproc->p_vmmap);

  // Increment refcounts
  dbg(DBG_FORK, "\n");
  memcpy(&new_proc->p_files, &curproc->p_files, sizeof(file_t*)*NFILES);
  for (int i = 0; i < NFILES; ++i) {
    if (new_proc->p_files[i]) fref(new_proc->p_files[i]);
  }
  vref(curproc->p_cwd);
  KASSERT(curproc->p_vmmap != new_proc->p_vmmap);

  // Set up shadow objects for private objects
  dbg(DBG_FORK, "\n");
  vmarea_t *vma;
  vmarea_t *vma2;
  list_t *list = &curproc->p_vmmap->vmm_list;
  list_t *list2 = &new_proc->p_vmmap->vmm_list;
  list_link_t *link;
  list_link_t *link2;
  for (link = list->l_next, link2 = list2->l_next;
      link != list && link2 != list2; 
      link = link->l_next, link2 = link2->l_next) {
    vma = list_item(link, vmarea_t, vma_plink);
    vma2 = list_item(link2, vmarea_t, vma_plink);
    if (vma->vma_flags & MAP_PRIVATE) { 
      // Set up shadow objects
      mmobj_t *shadow = shadow_create();
      mmobj_t *shadow2 = shadow_create();
      KASSERT(shadow && shadow2);
      mmobj_t *bottom = mmobj_bottom_obj(vma->vma_obj);
      KASSERT(bottom && !bottom->mmo_shadowed);
      shadow->mmo_un.mmo_bottom_obj = bottom;
      shadow2->mmo_un.mmo_bottom_obj = bottom;
      shadow->mmo_shadowed = vma->vma_obj;
      shadow2->mmo_shadowed = vma->vma_obj;
      vma->vma_obj = shadow;
      vma2->vma_obj = shadow2;
      list_insert_tail(&bottom->mmo_un.mmo_vmas, &vma2->vma_olink);
      dbg(DBG_FORK, "shadowed 0x%p with 0x%p and 0x%p\n",
          vma->vma_obj, shadow, shadow2);
    }
  }
  //dbginfo(DBG_VMMAP, vmmap_mapping_info, curproc->p_vmmap);
  //dbginfo(DBG_VMMAP, vmmap_mapping_info, new_proc->p_vmmap);

  // Unmap pages and flush caches
  dbg(DBG_FORK, "\n");
  pt_unmap_range(curproc->p_pagedir, USER_MEM_LOW, USER_MEM_HIGH);
  tlb_flush_all();

  // Set up new thread context
  dbg(DBG_FORK, "\n");
  kthread_t *new_thr = kthread_clone(curthr); 
  KASSERT(new_thr);
  list_insert_tail(&new_proc->p_threads, &new_thr->kt_plink);
  new_thr->kt_proc = new_proc;

  new_thr->kt_ctx.c_eip = (uint32_t)&userland_entry;
  regs->r_eax = 0; // TODO: check this
  new_thr->kt_ctx.c_esp = fork_setup_stack(regs, new_thr->kt_kstack);
  new_thr->kt_ctx.c_pdptr = new_proc->p_pagedir;
  new_thr->kt_ctx.c_kstack = (uintptr_t)new_thr->kt_kstack + DEFAULT_STACK_SIZE;
  new_thr->kt_ctx.c_kstacksz = DEFAULT_STACK_SIZE;

  dbg(DBG_FORK, "new proc: %d\n", new_proc->p_pid);
  sched_make_runnable(new_thr);

  return new_proc->p_pid;
}
