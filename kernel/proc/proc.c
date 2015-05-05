#include "kernel.h"
#include "config.h"
#include "globals.h"
#include "errno.h"

#include "util/debug.h"
#include "util/list.h"
#include "util/string.h"
#include "util/printf.h"

#include "proc/kthread.h"
#include "proc/proc.h"
#include "proc/sched.h"
#include "proc/proc.h"

#include "mm/slab.h"
#include "mm/page.h"
#include "mm/mmobj.h"
#include "mm/mm.h"
#include "mm/mman.h"

#include "vm/vmmap.h"

#include "fs/vfs.h"
#include "fs/vfs_syscall.h"
#include "fs/vnode.h"
#include "fs/file.h"

proc_t *curproc = NULL; /* global */
static slab_allocator_t *proc_allocator = NULL;

static list_t _proc_list;
static proc_t *proc_initproc = NULL; /* Pointer to the init process (PID 1) */

void proc_init() {
  list_init(&_proc_list);
  proc_allocator = slab_allocator_create("proc", sizeof(proc_t));
  KASSERT(proc_allocator != NULL);
}

static pid_t next_pid = 0;

/**
 * schedule_cancelable_sleep(p_wait);
 * Returns the next available PID.
 *
 * Note: Where n is the number of running processes, this algorithm is
 * worst case O(n^2). As long as PIDs never wrap around it is O(n).
 *
 * @return the next available PID
 */
static int _proc_getid() {
  proc_t *p;
  pid_t pid = next_pid;
  while (1) {
  failed:
    list_iterate_begin(&_proc_list, p, proc_t, p_list_link) {
      // dbg(DBG_INIT, "pid %d taken\n", p->p_pid);
      if (p->p_pid == pid) {
        if ((pid = (pid + 1) % PROC_MAX_COUNT) == next_pid) {
          return -1;
        } else {
          goto failed;
        }
      }
    }
    list_iterate_end();
    next_pid = (pid + 1) % PROC_MAX_COUNT;
    return pid;
  }
}

/*
 * The new process, although it isn't really running since it has no
 * threads, should be in the PROC_RUNNING state.
 *
 * Don't forget to set proc_initproc when you create the init
 * process. You will need to be able to reference the init process
 * when reparenting processes to the init process.
 */
proc_t *proc_create(char *name) {
  dbg(DBG_INIT, "creating proc %s\n", name);
  // Allocate new proc
  proc_t *new_proc = slab_obj_alloc(proc_allocator);
  // Set PID
  new_proc->p_pid = _proc_getid();
  dbg(DBG_INIT, "got pid %d\n", new_proc->p_pid);
  KASSERT(new_proc->p_pid >= 0);
  // Set name
  strncpy(new_proc->p_comm, name, PROC_NAME_LEN);
  // Initialize thread and children lists
  list_init(&new_proc->p_threads);
  list_init(&new_proc->p_children);
  list_link_init(&new_proc->p_list_link);
  list_link_init(&new_proc->p_child_link);
  new_proc->p_pproc = curproc;
  new_proc->p_status = 0;
  new_proc->p_state = PROC_RUNNING;
  sched_queue_init(&new_proc->p_wait);

  // Handle init case
  if (new_proc->p_pid == PID_INIT)
    proc_initproc = new_proc;

  // Add to tail of process list
  list_insert_tail(&_proc_list, &new_proc->p_list_link);
  // Add to parent's child list
  if (curproc)
    list_insert_tail(&curproc->p_children, &new_proc->p_child_link);

  new_proc->p_pagedir = pt_create_pagedir();

  for (int i = 0; i < NFILES; ++i)
    new_proc->p_files[i] = NULL; // open files
  if (vfs_root_vn) 
    vref(vfs_root_vn);
  else 
    dbg(DBG_VFS, "proc %s unable to vref\n", name);
  new_proc->p_cwd = vfs_root_vn; // current working dir 

  // Fields unset:
  new_proc->p_vmmap = vmmap_create();

  dbg(DBG_INIT, "returning proc %s\n", name);
  return new_proc;
}

/**
 * Cleans up as much as the process as can be done from within the
 * process. This involves:
 *    - Closing all open files (VFS)
 *    - Cleaning up VM mappings (VM)
 *    - Waking up its parent if it is waiting
 *    - Reparenting any children to the init process
 *    - Setting its status and state appropriately
 *
 * The parent will finish destroying the process within do_waitpid (make
 * sure you understand why it cannot be done here). Until the parent
 * finishes destroying it, the process is informally called a 'zombie'
 * process.
 *
 * This is also where any children of the current process should be
 * reparented to the init process (unless, of course, the current
 * process is the init process. However, the init process should not
 * have any children at the time it exits).
 *
 * Note: You do _NOT_ have to special case the idle process. It should
 * never exit this way.
 *
 * @param status the status to exit the process with
 */
void proc_cleanup(int status) {
  dbg(DBG_PROC, "proc %d\n", curproc->p_pid);
  // Set exit status
  curproc->p_status = status;

  // Handle init case
  if (curproc->p_pid == PID_INIT)
    KASSERT(list_empty(&curproc->p_children));
  else {
    // Add process's children to init's children
    proc_t *child;
    list_iterate_begin(&curproc->p_children, child, proc_t, p_child_link) {
      list_remove(&child->p_child_link);
      list_insert_tail(&proc_initproc->p_children, &child->p_child_link);
    }
    list_iterate_end();
  }

  list_remove(&curproc->p_list_link);

  // Wake parent
  sched_broadcast_on(&curproc->p_pproc->p_wait);

  // Clean up files
  for (int i = 0; i < NFILES; ++i) {
    if (curproc->p_files[i])
      do_close(i);
  }
  if (curproc->p_cwd) vput(curproc->p_cwd);

  vmmap_destroy(curproc->p_vmmap);
}

/*
 * This has nothing to do with signals and kill(1).
 *
 * Calling this on the current process is equivalent to calling
 * do_exit().
 *
 * In Weenix, this is only called from proc_kill_all.
 */
void proc_kill(proc_t *p, int status) {
  KASSERT(p->p_state == PROC_RUNNING || p->p_state == PROC_DEAD);
  // cancel all threads
  if (curproc == p) {
    do_exit(status);
  } else {
    kthread_t *iterator;
    list_iterate_begin(&p->p_threads, iterator, kthread_t, kt_plink) {
      kthread_cancel(iterator, (void *)status);
    }
    list_iterate_end();
  }
}

/*
 * Remember, proc_kill on the current process will _NOT_ return.
 * Don't kill direct children of the idle process.
 *
 * In Weenix, this is only called by sys_halt.
 */
void proc_kill_all() {
  list_link_t *link;
  list_t *idle_children = &proc_lookup(PID_IDLE)->p_children;
  proc_t *proc;
  list_iterate_begin(&_proc_list, proc, proc_t, p_list_link) {
    KASSERT(proc->p_state == PROC_RUNNING || proc->p_state == PROC_DEAD);
    // Skip current process, idle, and init
    if (proc->p_pid == curproc->p_pid || 
        proc->p_pid == PID_IDLE ||
        proc->p_pid == PID_INIT)
      continue;
    int pid = proc->p_pid;
    int is_idle_child = 0;
    // Ensure pid doesn't match an idle child pid
    for (link = idle_children->l_next; link != idle_children;
         link = link->l_next) {
      proc_t *idle_child_proc = list_item(link, proc_t, p_child_link);
      if (idle_child_proc->p_pid == pid) {
        is_idle_child = 1;
        break;
      }
    }
    if (!is_idle_child) {
      proc_kill(proc, 0);
    }
  }
  list_iterate_end();
  do_exit(0);
}

proc_t *proc_lookup(int pid) {
  proc_t *p;
  list_iterate_begin(&_proc_list, p, proc_t, p_list_link) {
    if (p->p_pid == pid) {
      return p;
    }
  }
  list_iterate_end();
  return NULL;
}

list_t *proc_list() { return &_proc_list; }

/*
 * This function is only called from kthread_exit.
 *
 * Unless you are implementing MTP, this just means that the process
 * needs to be cleaned up and a new thread needs to be scheduled to
 * run. If you are implementing MTP, a single thread exiting does not
 * necessarily mean that the process should be exited.
 */
void proc_thread_exited(void *retval) {
  dbg(DBG_PROC, "pid %d\n", curproc->p_pid);
  proc_cleanup((int)retval);
  curthr->kt_state = KT_EXITED;
  curproc->p_state = PROC_DEAD;
  sched_switch();
}

/* If pid is -1 dispose of one of the exited children of the current
 * process and return its exit status in the status argument, or if
 * all children of this process are still running, then this function
 * blocks on its own p_wait queue until one exits.
 *
 * If pid is greater than 0 and the given pid is a child of the
 * current process then wait for the given pid to exit and dispose
 * of it.
 *
 * If the current process has no children, or the given pid is not
 * a child of the current process return -ECHILD.
 *
 * Pids other than -1 and positive numbers are not supported.
 * Options other than 0 are not supported.
 */
pid_t do_waitpid(pid_t pid, int options, int *status) {
  dbg(DBG_PROC, "pid: %d\n", pid);
  KASSERT(!options);
  KASSERT(pid >= -1);

  dbginfo(DBG_VMMAP, proc_list_info, NULL);
  // Error if no children
  if (list_empty(&curproc->p_children))
    return -ECHILD;
  int found = 0;
  while (1) {
    // Iterate through children and reap dead ones
    proc_t *child;
    list_iterate_begin(&curproc->p_children, child, proc_t, p_child_link) {
      if (pid == -1 || (pid > 0 && child->p_pid == pid)) {
        found = 1;
        if (child->p_state == PROC_DEAD) {
          KASSERT(!list_empty(&child->p_threads));
          kthread_t *thread =
              list_item(child->p_threads.l_next, kthread_t, kt_plink);
          KASSERT(thread->kt_state == KT_EXITED);
          if (status)
            *status = child->p_status;
          list_remove(&child->p_child_link);
          pid_t pid = child->p_pid;
          kthread_destroy(thread);
          pt_destroy_pagedir(child->p_pagedir);
          slab_obj_free(proc_allocator, child);
          return pid;
        }
      }
    }
    list_iterate_end();
    if (!found)
      return -ECHILD;
    sched_cancellable_sleep_on(&curproc->p_wait);
  }
}

/*
 * Cancel all threads, join with them, and exit from the current
 * thread.
 *
 * @param status the exit status of the process
 */
void do_exit(int status) {
  kthread_t *iterator;
  list_iterate_begin(&curproc->p_threads, iterator, kthread_t, kt_plink) {
    kthread_cancel(iterator, (void *)status);
  }
  list_iterate_end();
#ifdef __MTP__
  list_iterate_begin(&curproc->p_threads, iterator, kthread_t, kt_plink) {
    kthread_join(iterator, (void *)status);
  }
  list_iterate_end();
#endif
  kthread_exit((void *)status);
}

size_t proc_info(const void *arg, char *buf, size_t osize) {
  const proc_t *p = (proc_t *)arg;
  size_t size = osize;
  proc_t *child;

  KASSERT(NULL != p);
  KASSERT(NULL != buf);

  iprintf(&buf, &size, "pid:          %i\n", p->p_pid);
  iprintf(&buf, &size, "name:         %s\n", p->p_comm);
  if (NULL != p->p_pproc) {
    iprintf(&buf, &size, "parent:       %i (%s)\n", p->p_pproc->p_pid,
            p->p_pproc->p_comm);
  } else {
    iprintf(&buf, &size, "parent:       -\n");
  }

#ifdef __MTP__
  int count = 0;
  kthread_t *kthr;
  list_iterate_begin(&p->p_threads, kthr, kthread_t, kt_plink) { ++count; }
  list_iterate_end();
  iprintf(&buf, &size, "thread count: %i\n", count);
#endif

  if (list_empty(&p->p_children)) {
    iprintf(&buf, &size, "children:     -\n");
  } else {
    iprintf(&buf, &size, "children:\n");
  }
  list_iterate_begin(&p->p_children, child, proc_t, p_child_link) {
    iprintf(&buf, &size, "     %i (%s)\n", child->p_pid, child->p_comm);
  }
  list_iterate_end();

  iprintf(&buf, &size, "status:       %i\n", p->p_status);
  iprintf(&buf, &size, "state:        %i\n", p->p_state);

#ifdef __VFS__
#ifdef __GETCWD__
  if (NULL != p->p_cwd) {
    char cwd[256];
    lookup_dirpath(p->p_cwd, cwd, sizeof(cwd));
    iprintf(&buf, &size, "cwd:          %-s\n", cwd);
  } else {
    iprintf(&buf, &size, "cwd:          -\n");
  }
#endif /* __GETCWD__ */
#endif

#ifdef __VM__
  iprintf(&buf, &size, "start brk:    0x%p\n", p->p_start_brk);
  iprintf(&buf, &size, "brk:          0x%p\n", p->p_brk);
#endif

  return size;
}

size_t proc_list_info(const void *arg, char *buf, size_t osize) {
  size_t size = osize;
  proc_t *p;

  KASSERT(NULL == arg);
  KASSERT(NULL != buf);

#if defined(__VFS__) && defined(__GETCWD__)
  iprintf(&buf, &size, "%5s %-13s %-18s %-s\n", "PID", "NAME", "PARENT", "CWD");
#else
  iprintf(&buf, &size, "%5s %-13s %-s\n", "PID", "NAME", "PARENT");
#endif

  list_iterate_begin(&_proc_list, p, proc_t, p_list_link) {
    char parent[64];
    if (NULL != p->p_pproc) {
      snprintf(parent, sizeof(parent), "%3i (%s)", p->p_pproc->p_pid,
               p->p_pproc->p_comm);
    } else {
      snprintf(parent, sizeof(parent), "  -");
    }

#if defined(__VFS__) && defined(__GETCWD__)
    if (NULL != p->p_cwd) {
      char cwd[256];
      lookup_dirpath(p->p_cwd, cwd, sizeof(cwd));
      iprintf(&buf, &size, " %3i  %-13s %-18s %-s\n", p->p_pid, p->p_comm,
              parent, cwd);
    } else {
      iprintf(&buf, &size, " %3i  %-13s %-18s -\n", p->p_pid, p->p_comm,
              parent);
    }
#else
    iprintf(&buf, &size, " %3i  %-13s %-s\n", p->p_pid, p->p_comm, parent);
#endif
  }
  list_iterate_end();
  return size;
}
