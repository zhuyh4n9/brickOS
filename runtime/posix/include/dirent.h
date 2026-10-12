/*
 * brickOS prototype v0.2.0 — directory traversal (dirent.h; provided by the runtime/posix plugin)
 *
 * ★ The base is vfs's **purely generic** directory surface: `br_opendir`/`br_readdir`/
 *   `br_closedir` (a directory = an ordinary `file_ops`, read produces a fixed-size
 *   `br_dirent_t`). This service only converts shapes —
 *   `br_dirent_t{type,name_len,name}` → `struct dirent{d_type,d_name}`.
 *
 * ★ `d_name` keeps 256 bytes (POSIX habit), while vfs's name bound is `BR_NAME_MAX = 32`:
 *   a buffer larger than actually needed is the price of a **stable contract**, not waste —
 *   when a future FS relaxes the name length, consumers need not recompile.
 *
 * ★ `d_off` / `seekdir` / `telldir` belong to TR-C (vfs's `br_dirent_t` has no `d_off`,
 *   see `11-02` §2.2) ⇒ this generation **does not declare** `seekdir`/`telldir`, and
 *   `d_off` is always 0.
 */
#ifndef BR_POSIX_DIRENT_H
#define BR_POSIX_DIRENT_H

#include <sys/types.h>

#define BR_POSIX_NAME_MAX  256

struct dirent {
    ino_t          d_ino;
    off_t          d_off;
    unsigned short d_reclen;
    unsigned char  d_type;
    char           d_name[BR_POSIX_NAME_MAX];
};

/* `d_type` values (same as Linux) */
#define DT_UNKNOWN  0
#define DT_FIFO     1
#define DT_CHR      2
#define DT_DIR      4
#define DT_BLK      6
#define DT_REG      8
#define DT_LNK      10
#define DT_SOCK     12

typedef struct br_posix_dir DIR;

DIR           *opendir(const char *path);
struct dirent *readdir(DIR *d);
int            closedir(DIR *d);
void           rewinddir(DIR *d);

#endif /* BR_POSIX_DIRENT_H */
