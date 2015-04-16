#include "drivers/tty/n_tty.h"

#include "errno.h"

#include "drivers/tty/driver.h"
#include "drivers/tty/ldisc.h"
#include "drivers/tty/tty.h"

#include "mm/kmalloc.h"

#include "proc/kthread.h"

#include "util/debug.h"

/* helpful macros */
#define EOFC '\x4'
#define ldisc_to_ntty(ldisc) CONTAINER_OF(ldisc, n_tty_t, ntty_ldisc)
#define TTY_BUF_SIZE_T uint8_t

static void n_tty_attach(tty_ldisc_t *ldisc, tty_device_t *tty);
static void n_tty_detach(tty_ldisc_t *ldisc, tty_device_t *tty);
static int n_tty_read(tty_ldisc_t *ldisc, void *buf, int len);
static const char *n_tty_receive_char(tty_ldisc_t *ldisc, char c);
static const char *n_tty_process_char(tty_ldisc_t *ldisc, char c);

static tty_ldisc_ops_t n_tty_ops = {.attach = n_tty_attach,
                                    .detach = n_tty_detach,
                                    .read = n_tty_read,
                                    .receive_char = n_tty_receive_char,
                                    .process_char = n_tty_process_char};

struct n_tty {
  kmutex_t rlock;
  ktqueue_t rwaitq;
  char *inbuf;
  TTY_BUF_SIZE_T rhead; // Raw head
  TTY_BUF_SIZE_T rawtail; // Raw tail
  TTY_BUF_SIZE_T ckdtail; // First uncooked character

  tty_ldisc_t ntty_ldisc;
};

// Helper access functions
char *get_rhead(struct n_tty *nt) {
  return nt->inbuf + nt->rhead;
}
char *get_rawtail(struct n_tty *nt) {
  return nt->inbuf + nt->rawtail;
}
char *get_ckdtail(struct n_tty *nt) {
  return nt->inbuf + nt->ckdtail;
}

tty_ldisc_t *n_tty_create() {
  n_tty_t *ntty = (n_tty_t *)kmalloc(sizeof(n_tty_t));
  if (NULL == ntty)
    return NULL;
  ntty->ntty_ldisc.ld_ops = &n_tty_ops;
  return &ntty->ntty_ldisc;
}

void n_tty_destroy(tty_ldisc_t *ldisc) {
  KASSERT(NULL != ldisc);
  kfree(ldisc_to_ntty(ldisc));
}

/*
 * Initialize the fields of the n_tty_t struct, allocate any memory
 * you will need later, and set the tty_ldisc field of the tty.
 */
void n_tty_attach(tty_ldisc_t *ldisc, tty_device_t *tty) {
  dbg(DBG_TERM, "\n");
  n_tty_t *nt = ldisc_to_ntty(ldisc);
  kmutex_init(&nt->rlock);
  sched_queue_init(&nt->rwaitq);

  nt->inbuf = kmalloc(sizeof(TTY_BUF_SIZE_T));
  nt->rhead = 0;
  nt->rawtail = 0;
  nt->ckdtail = 0;

  tty->tty_ldisc = ldisc;
}

/*
 * Free any memory allocated in n_tty_attach and set the tty_ldisc
 * field of the tty.
 */
void n_tty_detach(tty_ldisc_t *ldisc, tty_device_t *tty) {
  dbg(DBG_TERM, "\n");
  n_tty_t *nt = ldisc_to_ntty(ldisc);
  kfree(nt->inbuf);
  tty->tty_ldisc = NULL;
}

/*
 * Read a maximum of len bytes from the line discipline into buf. 
 *
 * If the buffer is empty, sleep until some characters appear. This might
 * be a long wait, so it's best to let the thread be cancellable.
 *
 * Then, read from the head of the buffer up to the tail, stopping at
 * len bytes or a newline character, and leaving the buffer partially
 * full if necessary. Return the number of bytes you read into the
 * buf.

 * In this function, you will be accessing the input buffer, which
 * could be modified by other threads. Make sure to make the
 * appropriate calls to ensure that no one else will modify the input
 * buffer when we are not expecting it.
 *
 * Remember to handle newline characters and CTRL-D, or ASCII 0x04,
 * properly.
 */
int n_tty_read(tty_ldisc_t *ldisc, void *buf, int len) {
  KASSERT(len >= 0);
  n_tty_t *nt = ldisc_to_ntty(ldisc);
  kmutex_lock(&nt->rlock);

  // Wait for some data
  while (nt->rhead == nt->ckdtail) {
    kmutex_unlock(&nt->rlock);
    // TODO: check sleep behavior
    sched_cancellable_sleep_on(&nt->rwaitq);
    kmutex_lock(&nt->rlock);
  }

  int read = 0;
  char *buff = buf;
  // While rhead != cooked tail
  for (; nt->rhead != nt->ckdtail && len; ++(nt->rhead)) {
    buff[read] = *get_rhead(nt);
    --len;
    ++read;
    if (buff[read-1] == '\n' || 
        buff[read-1] == '\r' || 
        buff[read-1] == 0x04) {
      ++(nt->rhead);
      break;
    }
  }
   
  kmutex_unlock(&nt->rlock);
  dbg(DBG_TERM, "rhead: %d, ckdtail: %d, rawtail: %d\n",
      nt->rhead, nt->ckdtail, nt->rawtail);
  return read;
}

/*
 * The tty subsystem calls this when the tty driver has received a
 * character. Now, the line discipline needs to store it in its read
 * buffer and move the read tail forward.
 *
 * Special cases to watch out for: backspaces (both ASCII characters
 * 0x08 and 0x7F should be treated as backspaces), newlines ('\r' or
 * '\n'), and full buffers.
 *
 * Return a null terminated string containing the characters which
 * need to be echoed to the screen. For a normal, printable character,
 * just the character to be echoed.
 */
const char *n_tty_receive_char(tty_ldisc_t *ldisc, char c) {
  n_tty_t *nt = ldisc_to_ntty(ldisc);
  dbg(DBG_TERM, "\n");
  
  char *out_string = kmalloc(2);
  KASSERT(out_string);
  out_string[0] = c;
  out_string[1] = '\0';
  
  if (c == 0x08 || c == 0x7F) { // Backspace
    if (nt->rawtail != nt->ckdtail) {
      --nt->rawtail;
      *get_rawtail(nt) = '\0';
    } else {
      out_string[0] = '\0';
      dbg(DBG_TERM, "Ignoring backspace\n");
    }
    return out_string;
  } else if (nt->rawtail + 1 != nt->rhead) {
    *get_rawtail(nt) = c;
    ++(nt->rawtail);
    dbg(DBG_TERM, "added 0x%x, new rawtail %d\n", c, nt->rawtail);
    if (c == '\r' || c == '\n' || c == 0) { // New line
      nt->ckdtail = nt->rawtail;
      sched_broadcast_on(&nt->rwaitq);
    }
  }
  return out_string;
}

/*
 * Process a character to be written to the screen.
 *
 * The only special case is '\r' and '\n'.
 */
const char *n_tty_process_char(tty_ldisc_t *ldisc, char c) {
  char *out_string = kmalloc(2);
  KASSERT(out_string);
  out_string[1] = '\0';
  // TODO: ask mentor about this "special case"
  //if (c == '\r' || c == '\n')
  //  out_string[0] = '\0';
  //else
  out_string[0] = c;
  return out_string;
}

