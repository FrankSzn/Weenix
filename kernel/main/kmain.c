#include "types.h"
#include "globals.h"
#include "kernel.h"

#include "util/gdb.h"
#include "util/init.h"
#include "util/debug.h"
#include "util/string.h"
#include "util/printf.h"

#include "mm/mm.h"
#include "mm/page.h"
#include "mm/pagetable.h"
#include "mm/pframe.h"

#include "vm/vmmap.h"
#include "vm/shadowd.h"
#include "vm/shadow.h"
#include "vm/anon.h"

#include "main/acpi.h"
#include "main/apic.h"
#include "main/interrupt.h"
#include "main/gdt.h"

#include "proc/sched.h"
#include "proc/proc.h"
#include "proc/kthread.h"

#include "drivers/dev.h"
#include "drivers/blockdev.h"
#include "drivers/disk/ata.h"
#include "drivers/tty/virtterm.h"
#include "drivers/pci.h"

#include "api/exec.h"
#include "api/syscall.h"

#include "fs/vfs.h"
#include "fs/vnode.h"
#include "fs/vfs_syscall.h"
#include "fs/fcntl.h"
#include "fs/stat.h"

#include "test/kshell/kshell.h"

#include "errno.h"

GDB_DEFINE_HOOK(boot)
GDB_DEFINE_HOOK(initialized)
GDB_DEFINE_HOOK(shutdown)

static void *bootstrap(int arg1, void *arg2);
static void *idleproc_run(int arg1, void *arg2);
static kthread_t *initproc_create(void);
static void *initproc_run(int arg1, void *arg2);
static void hard_shutdown(void);

static context_t bootstrap_context;
int vfstest_main(int argc, char **argv); 

int test_procs(kshell_t *ks, int argc, char **argv);
int test_drivers(kshell_t *ks, int argc, char **argv);
int test_vfs(kshell_t *ks, int argc, char **argv);

/**
 * This is the first real C function ever called. It performs a lot of
 * hardware-specific initialization, then creates a pseudo-context to
 * execute the bootstrap function in.
 */
void kmain() {
  GDB_CALL_HOOK(boot);

  dbg_init();
  dbgq(DBG_CORE, "Kernel binary:\n");
  dbgq(DBG_CORE, "  text: 0x%p-0x%p\n", &kernel_start_text, &kernel_end_text);
  dbgq(DBG_CORE, "  data: 0x%p-0x%p\n", &kernel_start_data, &kernel_end_data);
  dbgq(DBG_CORE, "  bss:  0x%p-0x%p\n", &kernel_start_bss, &kernel_end_bss);

  page_init();

  pt_init();
  slab_init();
  pframe_init();

  acpi_init();
  apic_init();
  pci_init();
  intr_init();

  gdt_init();

/* initialize slab allocators */
#ifdef __VM__
  anon_init();
  shadow_init();
#endif
  vmmap_init();
  proc_init();
  kthread_init();

dbg(DBG_INIT, "starting drivers code\n");
#ifdef __DRIVERS__
  bytedev_init();
  blockdev_init();
#endif
dbg(DBG_INIT, "drivers started\n");

  void *bstack = page_alloc();
  pagedir_t *bpdir = pt_get();
  KASSERT(NULL != bstack && "Ran out of memory while booting.");
  context_setup(&bootstrap_context, bootstrap, 0, NULL, bstack, PAGE_SIZE,
                bpdir);
  context_make_active(&bootstrap_context);

  panic("\nReturned to kmain()!!!\n");
}

/**
 * This function is called from kmain, however it is not running in a
 * thread context yet. It should create the idle process which will
 * start executing idleproc_run() in a real thread context.  To start
 * executing in the new process's context call context_make_active(),
 * passing in the appropriate context. This function should _NOT_
 * return.
 *
 * Note: Don't forget to set curproc and curthr appropriately.
 *
 * @param arg1 the first argument (unused)
 * @param arg2 the second argument (unused)
 */
static void *bootstrap(int arg1, void *arg2) {
  dbg(DBG_INIT, "starting bootstrap\n");
  /* necessary to finalize page table information */
  pt_template_init();

  curproc = proc_create("idle");
  curthr = kthread_create(curproc, idleproc_run, arg1, arg2);
  curthr->kt_state = KT_RUN;
  dbg(DBG_INIT, "switching to idle\n");
  context_make_active(&curthr->kt_ctx);

  panic("weenix returned to bootstrap()!!! BAD!!!\n");
  return NULL;
}

/**
 * Once we're inside of idleproc_run(), we are executing in the context of the
 * first process-- a real context, so we can finally begin running
 * meaningful code.
 *
 * This is the body of process 0. It should initialize all that we didn't
 * already initialize in kmain(), launch the init process (initproc_run),
 * wait for the init process to exit, then halt the machine.
 *
 * @param arg1 the first argument (unused)
 * @param arg2 the second argument (unused)
 */
static void *idleproc_run(int arg1, void *arg2) {
  /* create init proc */
  kthread_t *initthr = initproc_create();
  init_call_all();
  GDB_CALL_HOOK(initialized);

/* Create other kernel threads (in order) */

#ifdef __VFS__
/* Once you have VFS remember to set the current working directory
 * of the idle and init processes */
  initthr->kt_proc->p_cwd = vfs_root_vn;
  curproc->p_cwd = vfs_root_vn;
  vref(vfs_root_vn); // Want to start with two references
  vref(vfs_root_vn); // Want to start with two references

/* Here you need to make the null, zero, and tty devices using mknod */
/* You can't do this until you have VFS, check the include/drivers/dev.h
 * file for macros with the device ID's you will need to pass to mknod */
  do_mkdir("/dev");
  do_mknod("/dev/null", S_IFCHR, MKDEVID(1,0));
  do_mknod("/dev/zero", S_IFCHR, MKDEVID(1,1));
  do_mknod("/dev/tty0", S_IFCHR, MKDEVID(2,0));
  do_mknod("/dev/tty1", S_IFCHR, MKDEVID(2,1));
  do_mknod("/dev/sda", S_IFBLK, MKDEVID(1,0));
#endif

  /* Finally, enable interrupts (we want to make sure interrupts
   * are enabled AFTER all drivers are initialized) */
  intr_enable();

  /* Run initproc */
  sched_make_runnable(initthr);
  /* Now wait for it */
  int status;
  dbg(DBG_INIT, "waiting on init\n");
  pid_t child = do_waitpid(-1, 0, &status);
  KASSERT(PID_INIT == child);

#ifdef __MTP__
  kthread_reapd_shutdown();
#endif

#ifdef __SHADOWD__
  /* wait for shadowd to shutdown */
  shadowd_shutdown();
#endif

#ifdef __VFS__
  /* Shutdown the vfs: */
  dbg_print("weenix: vfs shutdown...\n");
  vput(curproc->p_cwd);
  if (vfs_shutdown())
    panic("vfs shutdown FAILED!!\n");

#endif

/* Shutdown the pframe system */
#ifdef __S5FS__
  pframe_shutdown();
#endif

  dbg_print("\nweenix: halted cleanly!\n");
  GDB_CALL_HOOK(shutdown);
  hard_shutdown();
  return NULL;
}

/**
 * This function, called by the idle process (within 'idleproc_run'), creates
 *the
 * process commonly refered to as the "init" process, which should have PID 1.
 *
 * The init process should contain a thread which begins execution in
 * initproc_run().
 *
 * @return a pointer to a newly created thread which will execute
 * initproc_run when it begins executing
 */
static kthread_t *initproc_create(void) {
  proc_t *init_p = proc_create("init");
  KASSERT(init_p->p_pid == PID_INIT);
  return kthread_create(init_p, initproc_run, 0, NULL);
}

void *aquire_mutex(int arg1, void *arg2) {
  for (int i = 0; i < 5; ++i) {
    kmutex_lock((kmutex_t *) arg2);
    dbg_print("Thread %d aquired mutex!\n", arg1);
    kmutex_unlock((kmutex_t *) arg2);
  }
  return NULL;
}

int fib(int n) {
  if (n == 0) return 0;
  if (n == 1) return 1;
  return fib(n-1) + fib(n-2);
}

void *calc_fib(int arg1, void *arg2) {
  return (void *) fib(arg1);
}

/**
 * The init thread's function changes depending on how far along your Weenix is
 * developed. Before VM/FI, you'll probably just want to have this run whatever
 * tests you've written (possibly in a new process). After VM/FI, you'll just
 * exec "/sbin/init".
 *
 * Both arguments are unused.
 *
 * @param arg1 the first argument (unused)
 * @param arg2 the second argument (unused)
 */
static void *initproc_run(int arg1, void *arg2) {
  dbg(DBG_INIT, "init running\n");

  kshell_add_command("procs", &test_procs, "test procs");
  kshell_add_command("drivers", &test_drivers, "test drivers");
  kshell_add_command("vfs", &test_vfs, "test vfs");

  int err = 0;
  kshell_t *ksh = kshell_create(0);
  KASSERT(ksh && "kshell_create failed");

  while ((err = kshell_execute_next(ksh)) > 0);
  KASSERT(!err && "kshell exited with error");
  kshell_destroy(ksh);

  return NULL;
}

int test_procs(kshell_t *ks, int argc, char **argv) {
  dbg(DBG_INIT, "Creating test1 thread\n");
  proc_t *proc1 = proc_create("test1");
  kthread_t *thread1 = kthread_create(proc1, &calc_fib, 10, NULL);
  sched_make_runnable(thread1);

  dbg(DBG_INIT, "Waiting on test1\n");
  dbg_print("%d\n", do_waitpid(proc1->p_pid, 0, NULL));

  dbg(DBG_INIT, "Creating threads\n");
  const int nthreads = 30;
  pid_t pids[nthreads];
  kmutex_t km;
  kmutex_init(&km);
  for (int i = 0; i < nthreads; ++i) {
    dbg(DBG_INIT, "Creating thread %d\n", i);
    proc_t *p = proc_create("test");
    pids[i] = p->p_pid;
    kthread_t *thread = kthread_create(p, &aquire_mutex, i, &km);
    sched_make_runnable(thread);
  }
  // Either proc_kill_all or wait them all
  // proc_kill_all();
  dbg(DBG_INIT, "Waiting on threads\n");
  for (int i = nthreads - 1; i >= 0; --i) {
    do_waitpid(pids[i], 0, NULL);
  }

  dbg(DBG_INIT, "Testing waitpid edge cases\n");
  // Wait with no child processes
  do_waitpid(-1, 0, NULL);
  do_waitpid(1208312, 0, NULL);

  // TODO: test several child processes terminating out of order

  return 0;
}

void *run_echo(int argc, void *argv) {
  dbg(DBG_TERM, "thread %d starting\n", argc);
  int fd = do_open("/dev/tty0", O_RDWR);
  char *buff[100];
  for (int i = 0; i < 2; ++i) {
    dbg(DBG_TERM, "thread %d reading\n", argc);
    int read_count = do_read(fd, buff, 100);
    dbg(DBG_TERM, "thread %d writing\n", argc);
    do_write(fd, buff, read_count);
  }
  do_close(fd);
  return NULL;
}

void *test_disk(int argc, void *argv) {
  dbg(DBG_DISK, "thread %d writing block\n", argc);
  blockdev_t *disk = blockdev_lookup(MKDEVID(1,0));
  char *data = page_alloc();
  data[0] = argc;
  KASSERT(!disk->bd_ops->write_block(disk, data, argc, 1));
  page_free(data);
  dbg(DBG_DISK, "succesful write by thread %d\n", argc);
  return NULL;
}

int test_drivers(kshell_t *ks, int argc, char **argv) {
  //dbg(DBG_TERM, "two threads reading/writing to tty0\n");
  //proc_t *p1 = proc_create("p1");
  //proc_t *p2 = proc_create("p2");
  //sched_make_runnable(kthread_create(p1, &run_echo, 1, NULL));
  //sched_make_runnable(kthread_create(p2, &run_echo, 2, NULL));
  //do_waitpid(-1, 0, 0);
  //do_waitpid(-1, 0, 0);
  
  // TODO: read and write more than terminal buffer size
  
  // multiple threads reading/writing to disk
  dbg(DBG_DISK, "starting disk test\n");
  const int ata_test_size = 10;
  for (int i = 0; i < ata_test_size; ++i) {
    char name[2];
    name[0] = i + '0';
    name[1] = '\0';
    proc_t *p = proc_create((char *) &name);
    sched_make_runnable(kthread_create(p, &test_disk, i, NULL));
  }
  while (-ECHILD != do_waitpid(-1, 0, NULL));
  dbg(DBG_DISK, "verifying written data\n");
  char *data = page_alloc();
  blockdev_t *disk = blockdev_lookup(MKDEVID(1,0));
  for (int i = 0; i < ata_test_size; ++i) {
    KASSERT(!disk->bd_ops->read_block(disk, data, i, 1));
    KASSERT(data[0] == i);
  }
  page_free(data);
  dbg(DBG_DISK, "disk test passed!\n");

  return 0;
}

int test_vfs(kshell_t *ks, int argc, char **argv) {
  vfstest_main(1, NULL);
  return 0;
}

/**
 * Clears all interrupts and halts, meaning that we will never run
 * again.
 */
static void hard_shutdown() {
#ifdef __DRIVERS__
  vt_print_shutdown();
#endif
  __asm__ volatile("cli; hlt");
}
