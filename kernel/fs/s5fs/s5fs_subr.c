/*
 *   FILE: s5fs_subr.c
 * AUTHOR: afenn
 *  DESCR:
 *  $Id: s5fs_subr.c,v 1.1.2.1 2006/06/04 01:02:15 afenn Exp $
 */

#include "kernel.h"
#include "util/debug.h"
#include "mm/kmalloc.h"
#include "globals.h"
#include "proc/sched.h"
#include "proc/kmutex.h"
#include "errno.h"
#include "util/string.h"
#include "util/printf.h"
#include "mm/pframe.h"
#include "mm/mmobj.h"
#include "drivers/dev.h"
#include "drivers/blockdev.h"
#include "fs/stat.h"
#include "fs/vfs.h"
#include "fs/vnode.h"
#include "fs/s5fs/s5fs_subr.h"
#include "fs/s5fs/s5fs.h"
#include "mm/mm.h"
#include "mm/page.h"

#define dprintf(...) dbg(DBG_S5FS, __VA_ARGS__)

#define s5_dirty_super(fs)                                                     \
  do {                                                                         \
    pframe_t *p;                                                               \
    int err;                                                                   \
    pframe_get(S5FS_TO_VMOBJ(fs), S5_SUPER_BLOCK, &p);                         \
    KASSERT(p);                                                                \
    err = pframe_dirty(p);                                                     \
    KASSERT(!err && "shouldn\'t fail for a page belonging "                    \
                    "to a block device");                                      \
  } while (0)

static void s5_free_block(s5fs_t *fs, int block);
static int s5_alloc_block(s5fs_t *);

/*
 * Return the disk-block number for the given seek pointer (aka file
 * position).
 *
 * If the seek pointer refers to a sparse block, and alloc is false,
 * then return 0. If the seek pointer refers to a sparse block, and
 * alloc is true, then allocate a new disk block (and make the inode
 * point to it) and return it.
 *
 * Be sure to handle indirect blocks!
 *
 * If there is an error, return -errno.
 *
 * You probably want to use pframe_get, pframe_pin, pframe_unpin, pframe_dirty.
 */
int s5_seek_to_block(vnode_t *vnode, off_t seekptr, int alloc) {
  dbg(DBG_S5FS, "vno: %d seekptr: %d\n", vnode->vn_vno, seekptr);
  s5_inode_t *inode = VNODE_TO_S5INODE(vnode);
  KASSERT(inode);
  KASSERT(curthr == vnode->vn_mutex.km_holder);
  blocknum_t block_index = S5_DATA_BLOCK(seekptr);
  uint32_t blocknum;
  int indirect = 0; // Bool, true if indirect block
  pframe_t *pframe;
  if (block_index < S5_NDIRECT_BLOCKS) { // Direct blocks
    blocknum = inode->s5_direct_blocks[block_index];
  } else { // Indirect blocks
    indirect = 1;
    block_index -= S5_NDIRECT_BLOCKS;
    if (block_index >= S5_NIDIRECT_BLOCKS)
      return -EFBIG;
    int status = pframe_get(S5FS_TO_VMOBJ(VNODE_TO_S5FS(vnode)), 
        inode->s5_indirect_block, &pframe);
    if (status) return status;
    blocknum = ((uint32_t *)pframe->pf_addr)[block_index];
  }
  if (!blocknum) { // Blocknum is zero, must be sparse
    if (!alloc) // Can't allocate a new block, return zero
      return 0;
    // Allocate new block
    // TODO: pin here
    blocknum = s5_alloc_block(VNODE_TO_S5FS(vnode));
    if (blocknum <= 0) {
      return blocknum;
    }
    if (indirect) { // New indirect block
      pframe_dirty(pframe);
      ((uint32_t *)pframe->pf_addr)[block_index] = blocknum;
    } else { // New direct block
      s5_dirty_inode(VNODE_TO_S5FS(vnode), inode);
      inode->s5_direct_blocks[block_index] = blocknum;
    }
  }
  return blocknum;
}

/*
 * Locks the mutex for the whole file system
 */
static void lock_s5(s5fs_t *fs) { kmutex_lock(&fs->s5f_mutex); }

/*
 * Unlocks the mutex for the whole file system
 */
static void unlock_s5(s5fs_t *fs) { kmutex_unlock(&fs->s5f_mutex); }

/* Abstraction for both reading and writing files
 * @write a boolean indicating whether to write (1) or read (0)
 * */
int s5_file_op(struct vnode *vnode, const off_t seek, char *buf, size_t len, int write) {
  KASSERT(vnode->vn_mode == S_IFDIR || vnode->vn_mode == S_IFREG);
  s5_inode_t *inode = VNODE_TO_S5INODE(vnode);
  KASSERT(seek >= 0);
  KASSERT(inode);
  KASSERT(curthr == vnode->vn_mutex.km_holder);
  //dbg(DBG_S5FS, "vno: %d, seek: %d len: %d filelen: %d\n", vnode->vn_vno, 
  //    seek, len, inode->s5_size);
  pframe_t *pframe;
  size_t ndone_total = 0;
  while (len) {
    blocknum_t blocknum = S5_DATA_BLOCK(seek + ndone_total);
    int status = pframe_get(&vnode->vn_mmobj, blocknum, &pframe);
    
    if (status) {
      dbg(DBG_S5FS, "pframe_get error: %d\n", status);
      return ndone_total;
    }
    size_t offset = S5_DATA_OFFSET(seek + ndone_total);
    size_t ndone = MIN(len, S5_BLOCK_SIZE - offset);
    // Can't read past end of file
    if (!write)
      ndone = MIN(ndone, inode->s5_size - seek - ndone_total);
    // Check for reading after file end
    if (!write && seek + ndone_total >= inode->s5_size) {
      dbg(DBG_S5FS, "read past end of file\n");
      return ndone_total;
    }

    // Do the operation on this page
    // TODO: zero out allocated sparse block
    if (write) {
      pframe_pin(pframe);
      pframe_dirty(pframe);
      memcpy(pframe->pf_addr + offset, buf + ndone_total, ndone);
      pframe_unpin(pframe);
    } else {
      memcpy(buf + ndone_total, pframe->pf_addr + offset, ndone);
    }
    // Update counters
    len -= ndone;
    ndone_total += ndone;
  }
  // Update file size
  uint32_t new_size = seek + ndone_total;
  if (write && new_size > inode->s5_size) {
    KASSERT(vnode->vn_len == inode->s5_size);
    dbg(DBG_S5FS, "old size: %d new size: %d\n", inode->s5_size, new_size);
    inode->s5_size = new_size; 
    vnode->vn_len = new_size;
    s5_dirty_inode(VNODE_TO_S5FS(vnode), inode);
  }
  dbg(DBG_S5FS, "did %d bytes\n", ndone_total);
  return ndone_total;
}

/*
 * Write len bytes to the given inode, starting at seek bytes from the
 * beginning of the inode. On success, return the number of bytes
 * actually written (which should be 'len', unless there's only enough
 * room for a partial write); on failure, return -errno.
 *
 * This function should allow writing to files or directories, treating
 * them identically.
 *
 * Writing to a sparse block of the file should cause that block to be
 * allocated. Writing past the end of the file should increase the size
 * of the file. Blocks between the end and where you start writing will
 * be sparse. In addition, bytes between where the old end of the file was and
 * the beginning of where you start writing should also be null.
 *
 * You cannot count on the contents of a block being null. Thus, if the seek
 * offset is not block aligned, be sure to set to null everything from where the
 * file ended to where the write begins.
 *
 * Do not call s5_seek_to_block() directly from this function. You will
 * use the vnode's pframe functions, which will eventually result in a
 * call to s5_seek_to_block().
 *
 * You will need pframe_dirty(), pframe_get(), memcpy().
 */
int s5_write_file(vnode_t *vnode, off_t seek, const char *bytes, size_t len) {
  dbg(DBG_S5FS, "vno: %d\n", vnode->vn_vno);
  return s5_file_op(vnode, seek, (char *)bytes, len, 1);
}

/*
 * Read up to len bytes from the given inode, starting at seek bytes
 * from the beginning of the inode. On success, return the number of
 * bytes actually read, or 0 if the end of the file has been reached; on
 * failure, return -errno.
 *
 * This function should allow reading from files or directories,
 * treating them identically.
 *
 * Reading from a sparse block of the file should act like reading
 * zeros; it should not cause the sparse blocks to be allocated.
 *
 * Similarly as in s5_write_file(), do not call s5_seek_to_block()
 * directly from this function.
 *
 * If the region to be read would extend past the end of the file, less
 * data will be read than was requested.
 *
 * You probably want to use pframe_get(), memcpy().
 */
int s5_read_file(struct vnode *vnode, off_t seek, char *dest, size_t len) {
  //dbg(DBG_S5FS, "\n");
  return s5_file_op(vnode, seek, dest, len, 0);
}


/*
 * Allocate a new disk-block off the block free list and return it. If
 * there are no free blocks, return -ENOSPC.
 *
 * This will not initialize the contents of an allocated block; these
 * contents are undefined.
 *
 * If the super block's s5s_nfree is 0, you need to refill
 * s5s_free_blocks and reset s5s_nfree.  You need to read the contents
 * of this page using the pframe system in order to obtain the next set of
 * free block numbers.
 *
 * Don't forget to dirty the appropriate blocks!
 *
 * You'll probably want to use lock_s5(), unlock_s5(), pframe_get(),
 * and s5_dirty_super()
 */
static int s5_alloc_block(s5fs_t *fs) {
  dbg(DBG_S5FS, "\n");
  lock_s5(fs);
  s5_super_t *super = fs->s5f_super;
  s5_dirty_super(fs); // Dirty super
  if (super->s5s_nfree) { // Use next entry in super block
    --super->s5s_nfree;
    unlock_s5(fs);
    return super->s5s_free_blocks[super->s5s_nfree];
  } else { // Super block exhausted
    int next = super->s5s_free_blocks[S5_NBLKS_PER_FNODE-1];
    if (next == -1) {
      dbg(DBG_S5FS, "out of free blocks!\n");
      unlock_s5(fs);
      return -ENOSPC;
    }
    // Copy contents of next block on list
    pframe_t *pframe;
    int status = pframe_get(S5FS_TO_VMOBJ(fs), next, &pframe);
    if (status) {
      dbg(DBG_S5FS, "pframe_get returned %d\n", status);
      unlock_s5(fs);
      return status;
    }
    uint32_t *freeblocks = (uint32_t *) pframe->pf_addr;
    for (int i = 0; i < S5_NBLKS_PER_FNODE; ++i) {
      super->s5s_free_blocks[i] = freeblocks[i];
    }
    super->s5s_nfree = S5_NBLKS_PER_FNODE - 1;
    unlock_s5(fs);
    return next; // Return emptied out next block
  }
}

/*
 * Given a filesystem and a block number, frees the given block in the
 * filesystem.
 *
 * This function may potentially block.
 *
 * The caller is responsible for ensuring that the block being placed on
 * the free list is actually free and is not resident.
 */
static void s5_free_block(s5fs_t *fs, int blockno) {
  s5_super_t *s = fs->s5f_super;

  lock_s5(fs);

  KASSERT(S5_NBLKS_PER_FNODE > s->s5s_nfree);

  if ((S5_NBLKS_PER_FNODE - 1) == s->s5s_nfree) {
    /* get the pframe where we will store the free block nums */
    pframe_t *prev_free_blocks = NULL;
    KASSERT(fs->s5f_bdev);
    pframe_get(&fs->s5f_bdev->bd_mmobj, blockno, &prev_free_blocks);
    KASSERT(prev_free_blocks->pf_addr);

    /* copy from the superblock to the new block on disk */
    memcpy(prev_free_blocks->pf_addr, (void *)(s->s5s_free_blocks),
           S5_NBLKS_PER_FNODE * sizeof(int));
    pframe_dirty(prev_free_blocks);

    /* reset s->s5s_nfree and s->s5s_free_blocks */
    s->s5s_nfree = 0;
    s->s5s_free_blocks[S5_NBLKS_PER_FNODE - 1] = blockno;
  } else {
    s->s5s_free_blocks[s->s5s_nfree++] = blockno;
  }

  s5_dirty_super(fs);

  unlock_s5(fs);
}

/*
 * Creates a new inode from the free list and initializes its fields.
 * Uses S5_INODE_BLOCK to get the page from which to create the inode
 *
 * This function may block.
 */
int s5_alloc_inode(fs_t *fs, uint16_t type, devid_t devid) {
  s5fs_t *s5fs = FS_TO_S5FS(fs);
  pframe_t *inodep;
  s5_inode_t *inode;
  int ret = -1;

  KASSERT((S5_TYPE_DATA == type) || (S5_TYPE_DIR == type) ||
          (S5_TYPE_CHR == type) || (S5_TYPE_BLK == type));

  lock_s5(s5fs);

  if (s5fs->s5f_super->s5s_free_inode == (uint32_t)-1) {
    unlock_s5(s5fs);
    return -ENOSPC;
  }

  pframe_get(&s5fs->s5f_bdev->bd_mmobj,
             S5_INODE_BLOCK(s5fs->s5f_super->s5s_free_inode), &inodep);
  KASSERT(inodep);

  inode = (s5_inode_t *)(inodep->pf_addr) +
          S5_INODE_OFFSET(s5fs->s5f_super->s5s_free_inode);

  KASSERT(inode->s5_number == s5fs->s5f_super->s5s_free_inode);

  ret = inode->s5_number;

  /* reset s5s_free_inode; remove the inode from the inode free list: */
  s5fs->s5f_super->s5s_free_inode = inode->s5_next_free;
  pframe_pin(inodep);
  s5_dirty_super(s5fs);
  pframe_unpin(inodep);

  /* init the newly-allocated inode: */
  inode->s5_size = 0;
  inode->s5_type = type;
  inode->s5_linkcount = 0;
  memset(inode->s5_direct_blocks, 0, S5_NDIRECT_BLOCKS * sizeof(int));
  if ((S5_TYPE_CHR == type) || (S5_TYPE_BLK == type))
    inode->s5_indirect_block = devid;
  else
    inode->s5_indirect_block = 0;

  s5_dirty_inode(s5fs, inode);

  unlock_s5(s5fs);

  return ret;
}

/*
 * Free an inode by freeing its disk blocks and putting it back on the
 * inode free list.
 *
 * You should also reset the inode to an unused state (eg. zero-ing its
 * list of blocks and setting its type to S5_FREE_TYPE).
 *
 * Don't forget to free the indirect block if it exists.
 *
 * You probably want to use s5_free_block().
 */
void s5_free_inode(vnode_t *vnode) {
  KASSERT(curthr == vnode->vn_mutex.km_holder);
  uint32_t i;
  s5_inode_t *inode = VNODE_TO_S5INODE(vnode);
  s5fs_t *fs = VNODE_TO_S5FS(vnode);

  KASSERT((S5_TYPE_DATA == inode->s5_type) || (S5_TYPE_DIR == inode->s5_type) ||
          (S5_TYPE_CHR == inode->s5_type) || (S5_TYPE_BLK == inode->s5_type));

  /* free any direct blocks */
  for (i = 0; i < S5_NDIRECT_BLOCKS; ++i) {
    if (inode->s5_direct_blocks[i]) {
      dprintf("freeing block %d\n", inode->s5_direct_blocks[i]);
      s5_free_block(fs, inode->s5_direct_blocks[i]);

      s5_dirty_inode(fs, inode);
      inode->s5_direct_blocks[i] = 0;
    }
  }

  if (((S5_TYPE_DATA == inode->s5_type) || (S5_TYPE_DIR == inode->s5_type)) &&
      inode->s5_indirect_block) {
    pframe_t *ibp;
    uint32_t *b;

    pframe_get(S5FS_TO_VMOBJ(fs), (unsigned)inode->s5_indirect_block, &ibp);
    KASSERT(ibp && "because never fails for block_device "
                   "vm_objects");
    pframe_pin(ibp);

    b = (uint32_t *)(ibp->pf_addr);
    for (i = 0; i < S5_NIDIRECT_BLOCKS; ++i) {
      KASSERT(b[i] != inode->s5_indirect_block);
      if (b[i])
        s5_free_block(fs, b[i]);
    }

    pframe_unpin(ibp);

    s5_free_block(fs, inode->s5_indirect_block);
  }

  inode->s5_indirect_block = 0;
  inode->s5_type = S5_TYPE_FREE;
  s5_dirty_inode(fs, inode);

  lock_s5(fs);
  inode->s5_next_free = fs->s5f_super->s5s_free_inode;
  fs->s5f_super->s5s_free_inode = inode->s5_number;
  unlock_s5(fs);

  s5_dirty_inode(fs, inode);
  s5_dirty_super(fs);
}

/*
 * Locate the directory entry in the given inode with the given name,
 * and return its inode number. If there is no entry with the given
 * name, return -ENOENT.
 *
 * You'll probably want to use s5_read_file and name_match
 *
 * You can either read one dirent at a time or optimize and read more.
 * Either is fine.
 */
int s5_find_dirent(vnode_t *vnode, const char *name, size_t namelen) {
  dbg(DBG_S5FS, "\n");
  KASSERT(curthr == vnode->vn_mutex.km_holder);
  KASSERT(name);
  KASSERT(namelen);
  size_t seek = 0;
  s5_dirent_t dirent;
  int nread = s5_read_file(vnode, seek, (char *)&dirent, sizeof(s5_dirent_t));
  while (nread) {
    KASSERT(nread == sizeof(s5_dirent_t));
    seek += nread;
    if (name_match(dirent.s5d_name, name, namelen))
      return dirent.s5d_inode;
    nread = s5_read_file(vnode, seek, (char *)&dirent, sizeof(s5_dirent_t));
  }
  return -ENOENT;
}

/*
 * Locate the directory entry in the given inode with the given name,
 * and delete it. If there is no entry with the given name, return
 * -ENOENT.
 *
 * In order to ensure that the directory entries are contiguous in the
 * directory file, you will need to move the last directory entry into
 * the remove dirent's place.
 *
 * When this function returns, the inode refcount on the removed file
 * should be decremented.
 *
 * It would be a nice extension to free blocks from the end of the
 * directory file which are no longer needed.
 *
 * Don't forget to dirty appropriate blocks!
 *
 * You probably want to use vget(), vput(), s5_read_file(),
 * s5_write_file(), and s5_dirty_inode().
 */
int s5_remove_dirent(vnode_t *vnode, const char *name, size_t namelen) {
  dbg(DBG_S5FS, "\n");
  KASSERT(curthr == vnode->vn_mutex.km_holder);
  size_t fpos = 0; // Position in file
  s5_dirent_t dirent;
  s5_dirent_t last_dirent; // Remember last non-zero dirent
  int nread = s5_read_file(vnode, fpos, (char *)&dirent, sizeof(s5_dirent_t));
  size_t found_pos = -1; // Position of found directory entry
  // Find matching directory entry
  while (nread && dirent.s5d_name[0]) {
    KASSERT(nread == sizeof(s5_dirent_t));
    if (name_match(dirent.s5d_name, name, namelen)) {
      found_pos = fpos;
      vnode_t *vn = vget(vnode->vn_fs, dirent.s5d_inode);
      s5_dirty_inode(VNODE_TO_S5FS(vn), VNODE_TO_S5INODE(vn));
      --VNODE_TO_S5INODE(vn)->s5_linkcount;
      vput(vn);
    }
    fpos += nread;
    last_dirent = dirent;
    nread = s5_read_file(vnode, fpos, (char *)&dirent, sizeof(s5_dirent_t));
  }
  if (found_pos == -1)
    return -ENOENT;
  // Replace found entry with last entry
  int nwrite = s5_write_file(vnode, found_pos, (char *)&last_dirent, 
      sizeof(s5_dirent_t));
  KASSERT(nwrite == sizeof(s5_dirent_t));
  // Zero out last entry
  memset((void *)&dirent, 0, sizeof(s5_dirent_t));
  nwrite = s5_write_file(vnode, fpos - sizeof(s5_dirent_t), (char *)&dirent, 
      sizeof(s5_dirent_t));
  KASSERT(nwrite == sizeof(s5_dirent_t));
  // Decrease file size
  s5_inode_t *inode = VNODE_TO_S5INODE(vnode);
  KASSERT(inode->s5_size >= sizeof(s5_dirent_t));
  KASSERT(vnode->vn_len == inode->s5_size);
  s5_dirty_inode(VNODE_TO_S5FS(vnode), inode);
  inode->s5_size -= sizeof(s5_dirent_t);
  vnode->vn_len -= sizeof(s5_dirent_t);
  return 0;
}

/*
 * Create a new directory entry in directory 'parent' with the given name, which
 * refers to the same file as 'child'.
 *
 * When this function returns, the inode refcount on the file that was linked to
 * should be incremented.
 *
 * Remember to increment the ref counts appropriately
 *
 * You probably want to use s5_find_dirent(), s5_write_file(), and
 *s5_dirty_inode().
 */
int s5_link(vnode_t *parent, vnode_t *child, const char *name, size_t namelen) {
  dbg(DBG_S5FS, "%.*s\n", namelen, name);
  KASSERT(curthr == parent->vn_mutex.km_holder);
  KASSERT(curthr == child->vn_mutex.km_holder);
  size_t fpos = 0; // Position in file
  s5_dirent_t dirent;
  int nread = s5_read_file(parent, fpos, (char *)&dirent, sizeof(s5_dirent_t));
  // Find last directory entry
  while (nread && dirent.s5d_name[0]) {
    KASSERT(nread == sizeof(s5_dirent_t));
    fpos += nread;
    nread = s5_read_file(parent, fpos, (char *)&dirent, sizeof(s5_dirent_t));
  }
  // Add new entry at end
  strncpy(dirent.s5d_name, name, namelen); // Copy name
  dirent.s5d_name[namelen] = '\0';
  dirent.s5d_inode = child->vn_vno; // Copy inode number
  int nwrite = s5_write_file(parent, fpos, (char *)&dirent, 
      sizeof(s5_dirent_t));
  if (nwrite != sizeof(s5_dirent_t)) {
    dbg(DBG_S5FS, "too many links!\n");
    return -EMLINK;
  }
  // Increment refcount
  if (parent != child) { // Increment if not a self link
    s5_dirty_inode(VNODE_TO_S5FS(child), VNODE_TO_S5INODE(child));
    ++VNODE_TO_S5INODE(child)->s5_linkcount;
  }
  return 0;
}

/*
 * Return the number of blocks that this inode has allocated on disk.
 * This should include the indirect block, but not include sparse
 * blocks.
 *
 * This is only used by s5fs_stat().
 *
 * You'll probably want to use pframe_get().
 */
int s5_inode_blocks(vnode_t *vnode) {
  dbg(DBG_S5FS, "\n");
  KASSERT(curthr == vnode->vn_mutex.km_holder);
  int blocks = 0;
  // Get inode
  s5_inode_t *inode = VNODE_TO_S5INODE(vnode);
  // Look in the inode
  for (int i = 0; i < S5_NDIRECT_BLOCKS; ++i) { 
    if (inode->s5_direct_blocks[i])
      ++blocks;
  }
  // Look in the indirect block
  if (inode->s5_indirect_block) {
    for (uint32_t i = 0; i < S5_NIDIRECT_BLOCKS; ++i) {
      pframe_t *pframe;
      int status = pframe_get(S5FS_TO_VMOBJ(VNODE_TO_S5FS(vnode)), 
          inode->s5_indirect_block, &pframe);
      if (status) {
        dbg(DBG_S5FS, "pframe_get returned %d\n", status);
        return status;
      }
      if (((uint32_t *)pframe->pf_addr)[i])
        ++blocks;
    }
  }
  return blocks;
}
