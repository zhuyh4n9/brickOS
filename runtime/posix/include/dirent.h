/*
 * POSIX directory traversal over vfs's generic dir surface (br_opendir/br_readdir/br_closedir);
 * converts br_dirent_t{type,name_len,name} to struct dirent{d_type,d_name}. d_name is 256 B
 * for contract stability though vfs's BR_NAME_MAX is 32. d_off is always 0;
 * seekdir/telldir are not declared this generation.
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
