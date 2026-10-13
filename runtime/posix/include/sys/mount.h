#ifndef BR_POSIX_SYS_MOUNT_H
#define BR_POSIX_SYS_MOUNT_H

/*
 * mount()/umount(): the APP decides what is mounted where; an FS plugin only registers a
 * driver name (fstype). source is the device for media-backed FSes (NULL for media-less),
 * target an absolute mount point, filesystemtype the driver name.
 * mountflags and data are reserved for v1: pass 0/NULL; non-zero returns -EINVAL.
 * Errors: -EINVAL (NULL target/fstype or non-empty flags/data), -ENODEV (no driver for the
 * fstype, or missing parent mount), -EEXIST (already mounted), -ENOSPC (table full),
 * otherwise passed through from the FS's mount.
 */
int mount(const char *source, const char *target, const char *filesystemtype,
          unsigned long mountflags, const void *data);

/*
 * Unmount. Reserved for v1: the static mount table holds the root inode, so unmounting would
 * first have to reclaim every open file handle and transient inode under the mount point.
 * Always returns -ENOTSUP (explicitly unimplemented).
 */
int umount(const char *target);

#endif /* BR_POSIX_SYS_MOUNT_H */
