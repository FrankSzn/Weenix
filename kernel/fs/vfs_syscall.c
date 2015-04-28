/*
 *  FILE: vfs_syscall.c
 *  AUTH: mcc | jal
 *  DESC:
 *  DATE: Wed Apr  8 02:46:19 1998
 *  $Id: vfs_syscall.c,v 1.9.2.2 2006/06/04 01:02:32 afenn Exp $
 */

#include "kernel.h"
#include "errno.h"
#include "globals.h"
#include "fs/vfs.h"
#include "fs/file.h"
#include "fs/vnode.h"
#include "fs/vfs_syscall.h"
#include "fs/open.h"
#include "fs/fcntl.h"
#include "fs/lseek.h"
#include "mm/kmalloc.h"
#include "util/string.h"
#include "util/printf.h"
#include "fs/stat.h"
#include "util/debug.h"

/* To read a file:
 *      o fget(fd)
 *      o call its virtual read f_op
 *      o update f_pos
 *      o fput() it
 *      o return the number of bytes read, or an error
 *
 * Error cases you must handle for this function at the VFS level:
 *      o EBADF
 *        fd is not a valid file descriptor or is not open for reading.
 *      o EISDIR
 *        fd refers to a directory.
 *
 * In all cases, be sure you do not leak file refcounts by returning before
 * you fput() a file that you fget()'ed.
 */
int do_read(int fd, void *buf, size_t nbytes) {
  dbg(DBG_VFS, "\n");
  file_t *f = fget(fd);
  if (!f || !(f->f_mode & FMODE_READ)) {
    if (f) fput(f);
    return -EBADF;
  }
  if (f->f_vnode->vn_mode == S_IFDIR) {
    fput(f);
    return -EISDIR;
  }
  int out = f->f_vnode->vn_ops->read(f->f_vnode, f->f_pos, buf, nbytes);
  f->f_pos += out;
  fput(f);
  return out;
}

/* Very similar to do_read.  Check f_mode to be sure the file is writable.  If
 * f_mode & FMODE_APPEND, do_lseek() to the end of the file, call the write
 * f_op, and fput the file.  As always, be mindful of refcount leaks.
 *
 * Error cases you must handle for this function at the VFS level:
 *      o EBADF
 *        fd is not a valid file descriptor or is not open for writing.
 */
int do_write(int fd, const void *buf, size_t nbytes) {
  dbg(DBG_VFS, "fd: %d n: %d buf: %.*s\n", fd, nbytes, nbytes, buf);
  file_t *f = fget(fd);
  if (!f || !(f->f_mode & FMODE_WRITE)) {
    if (f) fput(f);
    return -EBADF;
  }
  // Seek to end if appending
  if (f->f_mode & FMODE_APPEND)
    do_lseek(fd, 0, SEEK_END);
  int out = f->f_vnode->vn_ops->write(f->f_vnode, f->f_pos, buf, nbytes);
  f->f_pos += out;
  fput(f);
  return out;
}

/*
 * Zero curproc->p_files[fd], and fput() the file. Return 0 on success
 *
 * Error cases you must handle for this function at the VFS level:
 *      o EBADF
 *        fd isn't a valid open file descriptor.
 */
int do_close(int fd) {
  dbg(DBG_VFS, "fd: %d\n", fd);
  if (fd < 0 || fd >= NFILES) return -EBADF;
  file_t *f = curproc->p_files[fd];
  if (!f) {
    dbg(DBG_VFS, "bad fd\n");
    return -EBADF;
  }
  curproc->p_files[fd] = NULL;
  fput(f);
  return 0;
}

/* To dup a file:
 *      o fget(fd) to up fd's refcount
 *      o get_empty_fd()
 *      o point the new fd to the same file_t* as the given fd
 *      o return the new file descriptor
 *
 * Don't fput() the fd unless something goes wrong.  Since we are creating
 * another reference to the file_t*, we want to up the refcount.
 *
 * Error cases you must handle for this function at the VFS level:
 *      o EBADF
 *        fd isn't an open file descriptor.
 *      o EMFILE
 *        The process already has the maximum number of file descriptors open
 *        and tried to open a new one.
 */
int do_dup(int fd) {
  dbg(DBG_VFS, "fd: %d\n", fd);
  if (fd == -1) return -EBADF;
  file_t *f = fget(fd);
  if (!f) return -EBADF;
  int new_fd = get_empty_fd(curproc);
  if (new_fd == -EMFILE) {
    fput(f);
    return -EMFILE;
  }
  curproc->p_files[new_fd] = f;
  return new_fd;
}

/* Same as do_dup, but insted of using get_empty_fd() to get the new fd,
 * they give it to us in 'nfd'.  If nfd is in use (and not the same as ofd)
 * do_close() it first.  Then return the new file descriptor.
 *
 * Error cases you must handle for this function at the VFS level:
 *      o EBADF
 *        ofd isn't an open file descriptor, or nfd is out of the allowed
 *        range for file descriptors.
 */
int do_dup2(int ofd, int nfd) {
  dbg(DBG_VFS, "\n");
  if (ofd == -1) return -EBADF;
  file_t *f = fget(ofd);
  if (!f || nfd < 0 || nfd >= NFILES) {
    if (f) fput(f);
    return -EBADF;
  }
  if (ofd == nfd) {
    fput(f);  
    return nfd;
  }
  if (curproc->p_files[nfd])
    do_close(nfd);
  curproc->p_files[nfd] = f;
  return nfd;
}

/*
 * This routine creates a special file of the type specified by 'mode' at
 * the location specified by 'path'. 'mode' should be one of S_IFCHR or
 * S_IFBLK (you might note that mknod(2) normally allows one to create
 * regular files as well-- for simplicity this is not the case in Weenix).
 * 'devid', as you might expect, is the device identifier of the device
 * that the new special file should represent.
 *
 * You might use a combination of dir_namev, lookup, and the fs-specific
 * mknod (that is, the containing directory's 'mknod' vnode operation).
 * Return the result of the fs-specific mknod, or an error.
 *
 * Error cases you must handle for this function at the VFS level:
 *      o EINVAL
 *        mode requested creation of something other than a device special
 *        file.
 *      o EEXIST
 *        path already exists.
 *      o ENOENT
 *        A directory component in path does not exist.
 *      o ENOTDIR
 *        A component used as a directory in path is not, in fact, a directory.
 *      o ENAMETOOLONG
 *        A component of path was too long.
 */
int do_mknod(const char *path, int mode, unsigned devid) {
  dbg(DBG_VFS, "%s, mode: %d, devid: %d\n", path, mode, devid);
  if (mode != S_IFCHR && mode != S_IFBLK)
    return -EINVAL;
  size_t namelen;
  const char *name;
  vnode_t *dir = NULL;
  int status = dir_namev(path, &namelen, &name, NULL, &dir);
  if (status) return status;

  // Check if already exists
  vnode_t *new_vnode = NULL;
  status = lookup(dir, name, namelen, &new_vnode);
  if (status && status !=-ENOENT) {
    dbg(DBG_VFS, "lookup error: %d\n", status);
    vput(dir);
    return status;
  }
  if (new_vnode) {
    dbg(DBG_VFS, "already exists!\n");
    vput(dir);
    vput(new_vnode);
    return -EEXIST;
  }
  dbg(DBG_VFS, "making node: %s\n", path);
  status = dir->vn_ops->mknod(dir, name, namelen, mode, devid);
  vput(dir);
  return status;
}

/* Use dir_namev() to find the vnode of the dir we want to make the new
 * directory in.  Then use lookup() to make sure it doesn't already exist.
 * Finally call the dir's mkdir vn_ops. Return what it returns.
 *
 * Error cases you must handle for this function at the VFS level:
 *      o EEXIST
 *        path already exists.
 *      o ENOENT
 *        A directory component in path does not exist.
 *      o ENOTDIR
 *        A component used as a directory in path is not, in fact, a directory.
 *      o ENAMETOOLONG
 *        A component of path was too long.
 */
int do_mkdir(const char *path) {
  dbg(DBG_VFS, "%s\n", path);
  vnode_t *dir;
  size_t namelen;
  const char *name;
  int ret_code = dir_namev(path, &namelen, &name, NULL, &dir);
  if (ret_code) {
    dbg(DBG_VFS, "dir_namev returned %d\n", ret_code);
    return ret_code;
  }

  // Check if already exists
  vnode_t *result = NULL;
  ret_code = lookup(dir, name, namelen, &result);
  if (result) {
    dbg(DBG_VFS, "already exists!\n");
    vput(dir);
    vput(result);
    return -EEXIST;
  }

  ret_code = dir->vn_ops->mkdir(dir, name, namelen);
  dbg(DBG_VFS, "vnode mkdir returned %d\n", ret_code);
  vput(dir);
  return ret_code;
}

/* Use dir_namev() to find the vnode of the directory containing the dir to be
 * removed. Then call the containing dir's rmdir v_op.  The rmdir v_op will
 * return an error if the dir to be removed does not exist or is not empty, so
 * you don't need to worry about that here. Return the value of the v_op,
 * or an error.
 *
 * Error cases you must handle for this function at the VFS level:
 *      o EINVAL
 *        path has "." as its final component.
 *      o ENOTEMPTY
 *        path has ".." as its final component.
 *      o ENOENT
 *        A directory component in path does not exist.
 *      o ENOTDIR
 *        A component used as a directory in path is not, in fact, a directory.
 *      o ENAMETOOLONG
 *        A component of path was too long.
 */
int do_rmdir(const char *path) {
  dbg(DBG_VFS, "%s\n", path);
  size_t namelen;
  const char *name;
  vnode_t *dir;
  int status = dir_namev(path, &namelen, &name, NULL, &dir);
  if (status) return status;
  if (namelen == 1 && *name == '.') {
    vput(dir);
    return -EINVAL;
  }
  if (namelen == 2 && !strncmp(name, "..", 2)) {
    vput(dir);
    return -ENOTEMPTY;
  }
  status = dir->vn_ops->rmdir(dir, name, namelen);
  dbg(DBG_VFS, "status: %d\n", status);
  vput(dir);
  return status;
}

/*
 * Same as do_rmdir, but for files.
 *
 * Error cases you must handle for this function at the VFS level:
 *      o EISDIR
 *        path refers to a directory.
 *      o ENOENT
 *        A component in path does not exist.
 *      o ENOTDIR
 *        A component used as a directory in path is not, in fact, a directory.
 *      o ENAMETOOLONG
 *        A component of path was too long.
 */
int do_unlink(const char *path) {
  dbg(DBG_VFS, "%s\n", path);
  size_t namelen;
  const char *name;
  vnode_t *dir;
  int status = dir_namev(path, &namelen, &name, NULL, &dir);
  if (status) return status;
  vnode_t *res;
  status = lookup(dir, name, namelen, &res);
  if (status) {
    vput(dir);
    return status;
  }
  if (S_ISDIR(res->vn_mode)) {
    vput(dir);
    vput(res);
    return -EPERM;
  }
  vput(res);
  status = dir->vn_ops->unlink(dir, name, namelen);
  vput(dir);
  return status;
}

/* To link:
 *      o open_namev(from)
 *      o dir_namev(to)
 *      o call the destination dir's (to) link vn_ops.
 *      o return the result of link, or an error
 *
 * Remember to vput the vnodes returned from open_namev and dir_namev.
 *
 * Error cases you must handle for this function at the VFS level:
 *      o EEXIST
 *        to already exists.
 *      o ENOENT
 *        A directory component in from or to does not exist.
 *      o ENOTDIR
 *        A component used as a directory in from or to is not, in fact, a
 *        directory.
 *      o ENAMETOOLONG
 *        A component of from or to was too long.
 *      o EPERM
 *        from is a directory.
 */
int do_link(const char *from, const char *to) {
  dbg(DBG_VFS, "Finding %s\n", from);
  vnode_t *res_from;
  int status = open_namev(from, O_RDWR, &res_from, NULL);
  if (status) {
    dbg(DBG_VFS, "open_namev returned %d\n", status);  
    return status;
  }
  if (S_ISDIR(res_from->vn_mode)) {
    vput(res_from);
    return -EPERM;
  }

  dbg(DBG_VFS, "Finding %s\n", to);
  status = open_namev(to, O_RDWR, NULL, NULL);
  if (status != -ENOENT) {
    vput(res_from);
    if (status) return status;
    return -EEXIST;
  }
  size_t namelen;
  const char *name;
  vnode_t *res_to_dir = NULL;
  status = dir_namev(to, &namelen, &name, NULL, &res_to_dir);
  if (status) {
    vput(res_from);
    return status;
  }
  status = res_to_dir->vn_ops->link(res_from, res_to_dir, name, namelen);
  vput(res_from);
  vput(res_to_dir);
  return status;
}

/*      o link newname to oldname
 *      o unlink oldname
 *      o return the value of unlink, or an error
 *
 * Note that this does not provide the same behavior as the
 * Linux system call (if unlink fails then two links to the
 * file could exist).
 */
int do_rename(const char *oldname, const char *newname) {
  dbg(DBG_VFS, "\n");
  int status = do_link(oldname, newname);
  if (status) {
    dbg(DBG_VFS, "do_link returned %d\n", status); 
    return status;
  }
  status = do_unlink(oldname);
  if (status) dbg(DBG_VFS, "do_unlink returned %d\n", status); 
  return status;
}

/* Make the named directory the current process's cwd (current working
 * directory).  Don't forget to down the refcount to the old cwd (vput()) and
 * up the refcount to the new cwd (open_namev() or vget()). Return 0 on
 * success.
 *
 * Error cases you must handle for this function at the VFS level:
 *      o ENOENT
 *        path does not exist.
 *      o ENAMETOOLONG
 *        A component of path was too long.
 *      o ENOTDIR
 *        A component of path is not a directory.
 */
int do_chdir(const char *path) {
  dbg(DBG_VFS, "%s\n", path);
  vnode_t *dir; 
  int result = open_namev(path, O_RDWR, &dir, NULL);
  if (result) return result;
  if (!S_ISDIR(dir->vn_mode)) {
    vput(dir);
    return -ENOTDIR;
  }
  vput(curproc->p_cwd);
  curproc->p_cwd = dir;
  return 0;
}

/* Call the readdir f_op on the given fd, filling in the given dirent_t*.
 * If the readdir f_op is successful, it will return a positive value which
 * is the number of bytes copied to the dirent_t.  You need to increment the
 * file_t's f_pos by this amount.  As always, be aware of refcounts, check
 * the return value of the fget and the virtual function, and be sure the
 * virtual function exists (is not null) before calling it.
 *
 * Return either 0 or sizeof(dirent_t), or -errno.
 *
 * Error cases you must handle for this function at the VFS level:
 *      o EBADF
 *        Invalid file descriptor fd.
 *      o ENOTDIR
 *        File descriptor does not refer to a directory.
 */
int do_getdent(int fd, struct dirent *dirp) {
  //dbg(DBG_VFS, "\n");
  if (fd == -1) return -EBADF;
  file_t *f = fget(fd);
  if (!f) return -EBADF;
  if (!S_ISDIR(f->f_vnode->vn_mode)) {
    fput(f); 
    return -ENOTDIR;
  }
  int result = f->f_vnode->vn_ops->readdir(f->f_vnode, f->f_pos, dirp);
  if (result > 0) {
    f->f_pos += result;
    result = sizeof(dirent_t);
    dbg(DBG_VFS, "%s\n", dirp->d_name);
  }
  fput(f);
  return result;
}

/*
 * Modify f_pos according to offset and whence.
 *
 * Error cases you must handle for this function at the VFS level:
 *      o EBADF
 *        fd is not an open file descriptor.
 *      o EINVAL
 *        whence is not one of SEEK_SET, SEEK_CUR, SEEK_END; or the resulting
 *        file offset would be negative.
 */
int do_lseek(int fd, int offset, int whence) {
  dbg(DBG_VFS, "\n");
  // Input validation
  if (fd == -1) return -EBADF;
  file_t *f = fget(fd);
  if (!f) return -EBADF;
  if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) {
    fput(f);
    return -EINVAL;
  }
  int new_pos;
  if (whence == SEEK_SET) {
      new_pos = offset;
  } else if (whence == SEEK_CUR) {
      new_pos = f->f_pos + offset;
  } else { // SEEK_END
    new_pos = f->f_vnode->vn_len + offset;
  }
  if (new_pos < 0) {
    fput(f);
    return -EINVAL;
  }
  f->f_pos = new_pos;
  int ret = f->f_pos;
  fput(f);
  return ret;
}

/*
 * Find the vnode associated with the path, and call the stat() vnode operation.
 *
 * Error cases you must handle for this function at the VFS level:
 *      o ENOENT
 *        A component of path does not exist.
 *      o ENOTDIR
 *        A component of the path prefix of path is not a directory.
 *      o ENAMETOOLONG
 *        A component of path was too long.
 */
int do_stat(const char *path, struct stat *buf) {
  dbg(DBG_VFS, "\n");
  struct vnode *res;
  int status = open_namev(path, O_RDONLY, &res, NULL);
  if (status) return status;
  status = res->vn_ops->stat(res, buf);
  vput(res);
  return status;
}

#ifdef __MOUNTING__
/*
 * Implementing this function is not required and strongly discouraged unless
 * you are absolutely sure your Weenix is perfect.
 *
 * This is the syscall entry point into vfs for mounting. You will need to
 * create the fs_t struct and populate its fs_dev and fs_type fields before
 * calling vfs's mountfunc(). mountfunc() will use the fields you populated
 * in order to determine which underlying filesystem's mount function should
 * be run, then it will finish setting up the fs_t struct. At this point you
 * have a fully functioning file system, however it is not mounted on the
 * virtual file system, you will need to call vfs_mount to do this.
 *
 * There are lots of things which can go wrong here. Make sure you have good
 * error handling. Remember the fs_dev and fs_type buffers have limited size
 * so you should not write arbitrary length strings to them.
 */
int do_mount(const char *source, const char *target, const char *type) {
  NOT_YET_IMPLEMENTED("MOUNTING: do_mount");
  return -EINVAL;
}

/*
 * Implementing this function is not required and strongly discouraged unless
 * you are absolutley sure your Weenix is perfect.
 *
 * This function delegates all of the real work to vfs_umount. You should not
 *worry
 * about freeing the fs_t struct here, that is done in vfs_umount. All this
 *function
 * does is figure out which file system to pass to vfs_umount and do good error
 * checking.
 */
int do_umount(const char *target) {
  NOT_YET_IMPLEMENTED("MOUNTING: do_umount");
  return -EINVAL;
}
#endif
