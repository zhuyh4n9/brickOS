#ifndef BR_POSIX_SYS_MOUNT_H
#define BR_POSIX_SYS_MOUNT_H

/*
 * `mount()` / `umount()` — **the APP decides what is mounted and where** (ADR-0029).
 *
 * ★ Ownership: in this repo mounting is initiated by the **APP** (not by an FS plugin
 *   auto-mounting in its own start phase). An FS plugin only registers its capability as a
 *   **driver** (fstype name), e.g. `"tmpfs"`/`"devfs"`/`"fat"`/`"erofs"`; the APP uses this
 *   header's `mount()` to attach them by name at a mount point.
 *
 * Parameters match POSIX (and Linux):
 *   `source`         device name for media-backed FSes (`"emmc0"`); NULL for media-less ones (tmpfs/devfs).
 *   `target`         mount point (absolute path, e.g. `"/"`, `"/fat12"`).
 *   `filesystemtype` driver name, e.g. `"tmpfs"`.
 *   `mountflags`     POSIX flags. **Reserved for v1**: always pass 0 this generation; non-0 ⇒ -EINVAL
 *                    (better to reject explicitly than accept-and-ignore and let the APP think it worked).
 *   `data`           FS-specific options. **Reserved for v1**: always pass NULL this generation; non-NULL ⇒ -EINVAL.
 *
 * Errors: `-EINVAL` (target/fstype NULL or flags/data non-empty) / `-ENODEV` (no driver for that
 *       fstype — usually that FS plugin was not selected into the composition) / `-EEXIST` (mount
 *       point already mounted) / `-ENOSPC` (table full) / `-ENODEV` (parent mount missing) /
 *       otherwise passed through from the FS's mount.
 */
int mount(const char *source, const char *target, const char *filesystemtype,
          unsigned long mountflags, const void *data);

/*
 * Unmount. **Reserved for v1**: the mount table is a static composition-time artifact and holds the
 * root inode, so unmounting would first have to reclaim every open file handle and transient inode
 * under that mount point — that is "runtime mounting" (design O-S3, no earlier than v3).
 * This generation always returns -ENOTSUP (**explicitly unimplemented**, not faking success).
 */
int umount(const char *target);

#endif /* BR_POSIX_SYS_MOUNT_H */
