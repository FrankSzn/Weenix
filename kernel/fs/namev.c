#include "kernel.h"
#include "globals.h"
#include "types.h"
#include "errno.h"

#include "util/string.h"
#include "util/printf.h"
#include "util/debug.h"

#include "fs/dirent.h"
#include "fs/fcntl.h"
#include "fs/stat.h"
#include "fs/vfs.h"
#include "fs/vnode.h"

/* This takes a base 'dir', a 'name', its 'len', and a result vnode.
 * Most of the work should be done by the vnode's implementation
 * specific lookup() function.
 *
 * If dir has no lookup(), return -ENOTDIR.
 *
 * Note: returns with the vnode refcount on *result incremented.
 */
int lookup(vnode_t *dir, const char *name, size_t len, vnode_t **result) {
  //dbg(DBG_VFS, "Looking up: %s, length: %d\n", name, len);
  if (!S_ISDIR(dir->vn_mode)) {
    dbg(DBG_VFS, "not a directory\n");
    return -ENOTDIR;
  }
  int status = dir->vn_ops->lookup(dir, name, len, result);
  return status;
}

/* When successful this function returns data in the following "out"-arguments:
 *  o res_vnode: the vnode of the parent directory of "name"
 *  o name: the `basename' (the element of the pathname)
 *  o namelen: the length of the basename
 *
 * For example: dir_namev("/s5fs/bin/ls", &namelen, &name, NULL,
 * &res_vnode) would put 2 in namelen, "ls" in name, and a pointer to the
 * vnode corresponding to "/s5fs/bin" in res_vnode.
 *
 * The "base" argument defines where we start resolving the path from:
 * A base value of NULL means to use the process's current working directory,
 * curproc->p_cwd.  If pathname[0] == '/', ignore base and start with
 * vfs_root_vn.  dir_namev() should call lookup() to take care of resolving each
 * piece of the pathname.
 *
 * Note: A successful call to this causes vnode refcount on *res_vnode to
 * be incremented.
 */
int dir_namev(const char *pathname, size_t *namelen, const char **name,
              vnode_t *base, vnode_t **res_vnode) {
  dbg(DBG_VFS, "path: %s, %d\n", pathname, strlen(pathname));
  if (!pathname)
    return -ENOENT;
  if (!strlen(pathname))
    return -EINVAL;
  if (strlen(pathname) > MAXPATHLEN)
    return -ENAMETOOLONG;

  // Set base
  if (!base)
    base = curproc->p_cwd;
  size_t i = 0;
  if (pathname[0] == '/'){
    base = vfs_root_vn;
    for (; pathname[i] == '/'; ++i); // First not slash
  }
  vref(base);
  size_t j = 0;
  size_t k = 0;
  for (;; i += j+k) {
    // Advance to next '/' or end
    for (j = 0; pathname[i+j] && (pathname[i+j] != '/'); ++j);
    if (j > NAME_LEN) { // Check file name length
      vput(base);
      return -ENAMETOOLONG;
    }
    // Skip consecutive '/'
    for (k = 0; pathname[i+j+k] == '/'; ++k); 
    // Stop if at the end or at the trailing /
    if (!pathname[i+j+k]) break;
    vnode_t *result;
    int status = lookup(base, pathname+i, j, &result);
    vput(base);
    if (status) {
      dbg(DBG_VFS, "lookup returned %d\n", status);
      return status;
    }
    base = result;
  }
  // Set namelen, name, res_vnode
  if (!S_ISDIR(base->vn_mode)) {
    dbg(DBG_VFS, "inode %d is not a directory\n", base->vn_vno);
    vput(base);
    return -ENOTDIR;
  }
  if (namelen) *namelen = j;
  if (name) *name = pathname + i;
  dbg(DBG_VFS, "%.*s, %d\n", *namelen, *name, *namelen);
  if (res_vnode) *res_vnode = base; // Set res_vnode if possible
  else vput(base);
  return 0;
}

/* This returns in res_vnode the vnode requested by the other parameters.
 * It makes use of dir_namev and lookup to find the specified vnode (if it
 * exists).  flag is right out of the parameters to open(2); see
 * <weenix/fcntl.h>.  If the O_CREAT flag is specified, and the file does
 * not exist call create() in the parent directory vnode.
 *
 * Note: Increments vnode refcount on *res_vnode.
 */
int open_namev(const char *pathname, int flag, vnode_t **res_vnode,
               vnode_t *base) {
  dbg(DBG_VFS, "opening %s with flag 0x%x\n", pathname, flag);
  // Find directory
  vnode_t *dir = NULL;
  size_t namelen;
  const char *name;
  int status = dir_namev(pathname, &namelen, &name, base, &dir);
  if (status) {
    dbg(DBG_VFS, "dir_namev error: %d\n", status);
    return status;
  }

  // Perform lookup
  vnode_t *result = NULL;
  dbg(DBG_VFS, "looking for '%.*s' in directory 0x%p\n", namelen, name, dir);
  status = lookup(dir, name, namelen, &result);
  if (status && status != -ENOENT) {
    dbg(DBG_VFS, "lookup error: %d\n", status);
    vput(dir);
    return status;
  }
  if (status == -ENOENT) { // Not found
    dbg(DBG_VFS, "Node not found\n");
    if (!(flag & O_CREAT)) { // Check if create flag set
      vput(dir);
      return -ENOENT;
    }
    status = dir->vn_ops->create(dir, name, namelen, &result);
    if (status) {
      dbg(DBG_VFS, "create error: %d\n", status);
      vput(dir);
      return status;
    }
    dbg(DBG_VFS, "created node 0x%p in dir 0x%p\n", result, dir);
  } 
  if (res_vnode)
    *res_vnode = result;
  else
    vput(result);
  vput(dir);
  return 0;
}

#ifdef __GETCWD__
/* Finds the name of 'entry' in the directory 'dir'. The name is writen
 * to the given buffer. On success 0 is returned. If 'dir' does not
 * contain 'entry' then -ENOENT is returned. If the given buffer cannot
 * hold the result then it is filled with as many characters as possible
 * and a null terminator, -ERANGE is returned.
 *
 * Files can be uniquely identified within a file system by their
 * inode numbers. */
int lookup_name(vnode_t *dir, vnode_t *entry, char *buf, size_t size) {
  NOT_YET_IMPLEMENTED("GETCWD: lookup_name");
  return -ENOENT;
}

/* Used to find the absolute path of the directory 'dir'. Since
 * directories cannot have more than one link there is always
 * a unique solution. The path is writen to the given buffer.
 * On success 0 is returned. On error this function returns a
 * negative error code. See the man page for getcwd(3) for
 * possible errors. Even if an error code is returned the buffer
 * will be filled with a valid string which has some partial
 * information about the wanted path. */
ssize_t lookup_dirpath(vnode_t *dir, char *buf, size_t osize) {
  NOT_YET_IMPLEMENTED("GETCWD: lookup_dirpath");

  return -ENOENT;
}
#endif /* __GETCWD__ */
