/*
 * brickOS prototype v0.2.0 — 目录遍历(dirent.h; runtime/posix 插件提供)
 *
 * ★ 底座是 vfs 的**纯泛型**目录面: `br_opendir`/`br_readdir`/`br_closedir`
 *   (目录 = 普通 `file_ops`, read 产出定长 `br_dirent_t`)。本服务只做形状转换 ——
 *   `br_dirent_t{type,name_len,name}` → `struct dirent{d_type,d_name}`。
 *
 * ★ `d_name` 留 256 字节(POSIX 习惯), 而 vfs 的名字上界是 `BR_NAME_MAX = 32`:
 *   缓冲比真实需要大是**契约稳定**的代价, 不是浪费 —— 将来 FS 放宽名字长度时,
 *   消费者不必重新编译。
 *
 * ★ `d_off` / `seekdir` / `telldir` 属 TR-C(vfs 的 `br_dirent_t` 没有 `d_off`,
 *   见 `11-02` §2.2)⇒ 本代**不声明** `seekdir`/`telldir`, `d_off` 恒 0。
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

/* `d_type` 取值(与 Linux 同值) */
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
