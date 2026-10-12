/*
 * third_party/uboot-erofs — VFS 对接 + 生命周期钩子
 *
 * ============================== 本文件是什么 ==============================
 * 上游 EROFS 是**路径风格**的(u-boot 的 fs 层给它一个路径字符串, 它自己走查),
 * 而 brickOS 的 VFS(`framework/vfs-core/include/br/vfs/br_vfs.h`)是**两段式**的
 * (父目录 inode + 一个名字), 且 inode 是**瞬态**的(SD-3)。于是本文件是那层翻译:
 *
 * 本文件 = 上游 `fs/erofs/fs.c` 的对应物, 分成两节:
 *   ① **块层 shim**(`uboot_erofs_bdev_bind/dev_read/blk_read`): 把 u-boot 的
 *      `blk_desc`/`fs_devread` 换成 `br_bdev_ops_t`, 并把"字节偏移"折成"扇区 + 块内偏移";
 *   ② **VFS 对接**: 四层 ops 表 + 生命周期钩子。
 *
 *   VFS 契约                              本文件                  上游 EROFS
 *   ---------------------------------------------------------------------------
 *   br_fs_ops.mount                       挂载 + 造根 inode       erofs_read_superblock()
 *                                                                 + erofs_ilookup("")
 *   br_inode_ops.lookup(dir, name)        路径片段 → 相对路径     erofs_ilookup(path)
 *   br_inode_ops.getattr(ino)             size/type               (inode 里的字段)
 *   br_file_ops.read(f, buf, n)           offset + pread          erofs_pread()
 *   br_file_ops(目录).read                (块号, 序号) → dirent   erofs_readdir 的块内解析
 *   create/unlink/mkdir/rmdir/rename/…    **一律 -EROFS**(只读 FS)
 *
 * ## inode = 瞬态路径包装(设计 SD-3)
 *   每次 `lookup` 用 `br_malloc` 造一个节点, 里面放:
 *     - `path`: **相对挂载点**的路径(根 = 空串)。为什么不放 nid 就够? —— 上游的入口
 *       `erofs_ilookup(path, &vi)` 只认路径, 而 vfs 只会告诉我们"父 inode + 名字"
 *       (父 inode 自己也只有路径) ⇒ 拼前缀是唯一不需要再实现一份走查的做法。
 *       代价: 每次 lookup 都要从根重走一遍(EROFS 的目录块是排序的, 但本件照上游
 *       线性扫描)。在 v1 的单 APP 规模下可接受; 登记为 README 的欠债。
 *     - `vi`: **当次**解析出来的 `struct erofs_inode`(EROFS 是只读的, inode 内容不会
 *       在两次读之间改变, 所以把它随包装一起存下来是安全的 —— 省掉每次 read 的重复解析)。
 *   所有权规则(照 tmpfs 的读法, 但结论相反):
 *     - `mount` 交出的**根 inode**由挂载表长期持有, **永不**交给 `free_inode`
 *       (它同样是 `br_malloc` 出来的, 只是没人还);
 *     - 其余每一个都是瞬态 ⇒ `free_inode` 必须 `br_free`(不然走查一次泄漏一个)。
 *
 * ## 只读执法
 *   任务书的硬性口径: 名字空间变更槽位**也要填**, 但一律返回 `BR_ERR(BR_EROFS)` ——
 *   这比 vfs 对空槽位给的 -ENOTSUP 更准(EROFS 不是"不支持", 是"只读")。
 *   同理 `br_file_ops.write` 与"写权限打开"(`br_open(path, BR_O_WRONLY)`)都报 -EROFS:
 *   vfs 的 `open` 不替 FS 判访问模式(只读 fd 能不能写是 fd 层的事), 所以
 *   `TC-EROFS-007` 要的"写打开必须失败"这条判据落在本文件的 `uboot_erofs_file_open` 里。
 *
 * ## 名字上界(本仓库 BR_NAME_MAX = 32, EROFS 是 255)
 *   `br_dirent_t.name[32]` 装不下 32 字节及以上的名字 ⇒ 目录面上**跳过并计数**
 *   (日志打一次总数), `lookup` 上超长返回 `-ENOSPC`(vfs 的 `resolve_norm` 对超长分量
 *   给的也是 -ENOSPC, 口径一致; 见 README 的取舍说明)。
 * ==========================================================================
 */
#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/core/br_mem.h>
#include <br/dev/br_bdev.h>
#include <br/vfs/br_vfs.h>

#include "erofs_internal.h"

/* 生命周期钩子原型(名 = symbol_prefix + 相; 生成物 plugin_desc.c 引用)。
 * 本插件**不导出任何 API** ⇒ 除这三个 + 自检钩子外, 本文件其余函数全是 static。 */
int uboot_erofs_early_init(void);
int uboot_erofs_init(void);
int uboot_erofs_start(void);


/* =====================================================================
 * 块层 shim(移植自上游 fs.c 的 `erofs_dev_read()` / `erofs_blk_read()`)
 *
 * ============================== 这一节解决什么 ==============================
 * EROFS 内部**全部按字节偏移寻址**(`off`), 而 brickOS 的块设备契约
 * (`framework/bdev-core/include/br/dev/br_bdev.h`)按**扇区**寻址:
 *
 *     sector     = off / sector_size
 *     sector_off = off % sector_size
 *
 * 上游把这件事写成 `fs/erofs/fs.c` 的 `erofs_dev_read()`(它认识 u-boot 的
 * `struct blk_desc` 与 `disk_partition`, 用 `fs_devread()` 做"偏移 + 长度"的读)。
 * 本件保留**同名同义的函数**, 只把介质接口换成 `br_bdev_ops_t`:
 *
 *   u-boot 侧                                  brickOS 侧
 *   ---------------------------------------------------------------------------
 *   ctxt.cur_dev + cur_part_info               br_bdev_get("emmc3", &priv) 一次
 *   fs_devread(dev, part, sect, off, len, buf) 逐扇区 ops->read(priv, lba, buf, 1)
 *   log2blksz / blksz                          geometry.sector_size(512)
 *
 * ★ 为什么在 `erofs_vfs.c` 里而不是单独一件: 上游的 `fs.c` 一半是块层、一半是
 *   u-boot 的 fs 层粘合; 两半在本项目里都归"与外界对接", 合在一件里读起来仍是
 *   `fs.c` 的对应物(`super.c`/`namei.c`/`data.c`/`zmap.c`/`decompress.c` 才是纯 EROFS)。
 *
 * ## 为什么逐扇区下发(而不是"一次多扇区")
 *   任务书的硬约束是"单次 I/O 不超过 `geometry.max_rw`"(本平台 = 64 扇区 = 32 KiB)。
 *   逐扇区是**最强**的满足方式(每次 1 扇区, 恒 ≤ max_rw), 而且顺带把"缓冲区对齐"
 *   这件事也解决了: 每次只落到调用方缓冲里的一个扇区位置, 只要调用方的缓冲至少按
 *   扇区对齐即可(`-mstrict-align` 下未对齐的多字节访问会取 Alignment fault)。
 *   代价是命令变多 —— 本插件读的是只读镜像, 每条路径都按 4 KiB 块走(见 erofs_data.c),
 *   8 次 512 B 的读换一次 4 KiB 块的语义, 在原型阶段是可接受的取舍(记在 README 的欠债里)。
 *
 * ## 有意偏离(相对上游)
 *   1. `device_id` 参数**被忽略**: 多设备卷在挂载期就被拒(见 erofs_super.c 的
 *      `erofs_init_devices`), 所以到得了这里的一定是设备 0。保留形参是为了让 data.c 的
 *      `erofs_dev_read(mdev.m_deviceid, ...)` 调用点一字不改。
 *   2. 越界(offset/len 超出介质)⇒ `-EIO`。多设备被拒之后, "盘上块地址指向本设备之外"
 *      就一定是坏镜像 —— 这一条是最后的防线(否则坏 nid 会读出一块垃圾当 inode)。
 *   3. **整扇区中转**(实测踩到): bdev 的 `read(lba, buf, 1)` 会把**512 字节**写进 `buf`,
 *      而 EROFS 要的是"偏移 + 长度"(可能是 32 字节的 inode、8 字节的映射头)。直接把
 *      调用方的缓冲交给设备 = 越界写 480 字节 ⇒ 必须先读进中转缓冲再按长度拷出。
 *      中转缓冲用共享 scratch 的**尾部**(最后一个扇区): EROFS 自己的解析全部发生在
 *      4 KiB 块内且满足 `offset % 4096 + len <= 4096`, 永远碰不到最后一个扇区。
 * ===================================================================== */

/* 挂载期绑定一次的设备事实(见 erofs_internal.h 的 br_erofs_dev_t)。
 * 非 static: 自检要经它读盘上字节做判据(只读地看几何, 不写任何字段)。 */
br_erofs_dev_t uboot_erofs_dev;

int uboot_erofs_bdev_bind(void)
{
    void *priv = BR_NULL;

    const br_bdev_ops_t *ops = br_bdev_get(BR_EROFS_DEV_NAME, &priv);
    if ((ops == BR_NULL) || (priv == BR_NULL)) {
        br_log_error("uboot-erofs: 找不到块设备 %s", BR_EROFS_DEV_NAME);
        return BR_ERR(BR_ENODEV);
    }
    if ((ops->read == BR_NULL) || (ops->geometry == BR_NULL)) {
        br_log_error("uboot-erofs: %s 的 bdev ops 缺 read/geometry 槽位", BR_EROFS_DEV_NAME);
        return BR_ERR(BR_EINVAL);
    }

    br_bdev_geom_t g;
    const int grc = ops->geometry(priv, &g);
    if (grc != 0) {
        br_log_error("uboot-erofs: %s geometry() 失败 rc=%d", BR_EROFS_DEV_NAME, grc);
        return grc;
    }
    if ((g.sector_size == 0u) || (g.n_sectors == 0u) || (g.max_rw == 0u)) {
        br_log_error("uboot-erofs: %s 几何非法(sector=%u n=%llu max_rw=%u)",
                     BR_EROFS_DEV_NAME, (unsigned)g.sector_size,
                     (unsigned long long)g.n_sectors, (unsigned)g.max_rw);
        return BR_ERR(BR_EINVAL);
    }

    uboot_erofs_dev.ops         = ops;
    uboot_erofs_dev.priv        = priv;
    uboot_erofs_dev.n_sectors   = g.n_sectors;
    uboot_erofs_dev.sector_size = g.sector_size;
    uboot_erofs_dev.max_rw      = g.max_rw;
    uboot_erofs_dev.align       = g.align;
    return 0;
}

int uboot_erofs_dev_read(int device_id, void *buf, u64 offset, size_t len)
{
    br_u8 *p = (br_u8 *)buf;
    u64    off;

    /* 偏离 1: 见文件头。到得了这里的一定是设备 0。 */
    (void)device_id;

    if (uboot_erofs_dev.ops == BR_NULL) {
        return BR_ERR(BR_EIO);                 /* 没绑定就读: FS 侧时序错误 */
    }
    if ((buf == BR_NULL) && (len != 0u)) {
        return BR_ERR(BR_EINVAL);
    }
    if (len == 0u) {
        return 0;
    }

    /*
     * ★ 关键的一步(实测踩到): bdev 的 `read(lba, buf, 1)` 是**整扇区**语义 ——
     *   它会把 512 字节写进 `buf`。而 EROFS 要的是"偏移 + 长度"(可能是 32 字节的
     *   inode、8 字节的映射头)。直接把调用方的缓冲交给设备 = **越界写 480 字节**。
     *   所以这里必须"整扇区读进中转缓冲, 再把要的那一段拷出去"。
     *
     * 中转缓冲用共享 scratch 的**尾部**(最后一个扇区): EROFS 自己的解析全都发生在
     * 4 KiB 块内且满足 `offset % 4096 + len <= 4096`, 永远碰不到最后一个扇区; 而
     * `erofs_blk_read()` 一次读整块时, 中转缓冲落在目标块内部, 拷回同一位置也无害
     * (`in_sec == 0` 时源与目的重合, memcpy 允许——两块都不重叠出界)。
     * 对齐用 u32 视图取得(DMA 通路要求 ≥ 4 字节对齐)。
     */
    br_u8 *stage = (br_u8 *)(void *)((br_u32 *)(void *)uboot_erofs_scratch() +
                                     (EROFS_MAX_BLOCK_SIZE / 4u) - (512u / 4u));

    off = offset;
    for (size_t left = len; left > 0u; ) {
        const br_u64 media_bytes = uboot_erofs_dev.n_sectors * (br_u64)uboot_erofs_dev.sector_size;
        const br_u64 lba         = off / (br_u64)uboot_erofs_dev.sector_size;
        const br_u32 in_sec      = (br_u32)(off % (br_u64)uboot_erofs_dev.sector_size);
        br_size_t    cnt         = (br_size_t)((br_u64)uboot_erofs_dev.sector_size - (br_u64)in_sec);

        if (lba >= uboot_erofs_dev.n_sectors) {
            br_log_error("uboot-erofs: 读越界 off=0x%llx len=%llu (> %llu 字节介质)",
                         (unsigned long long)offset, (unsigned long long)len,
                         (unsigned long long)media_bytes);
            return BR_ERR(BR_EIO);
        }
        if (cnt > left) {
            cnt = left;
        }

        const int rc = uboot_erofs_dev.ops->read(uboot_erofs_dev.priv, lba, stage, 1u);
        if (rc < 0) {
            /* ★ 有意偏离: EROFS 的块层错误在 errno 表里只有 -EIO(上游同样把一切读失败
             *   收成 -EIO)。保留上游的码, 但把设备的真实返回值打进日志 —— 排障要它。 */
            br_log_error("uboot-erofs: bdev read 失败 lba=%llu rc=%d",
                         (unsigned long long)lba, rc);
            return BR_ERR(BR_EIO);
        }
        if (rc != 1) {
            br_log_error("uboot-erofs: bdev 短读 lba=%llu rc=%d(短读是坏介质)",
                         (unsigned long long)lba, rc);
            return BR_ERR(BR_EIO);
        }
        (void)__builtin_memcpy(p, stage + in_sec, cnt);

        p    += cnt;
        off  += (u64)cnt;
        left -= cnt;
    }
    return 0;
}

int uboot_erofs_blk_read(void *buf, erofs_blk_t start, u32 nblocks)
{
    return erofs_dev_read(0, buf, erofs_pos(start), (size_t)erofs_pos(nblocks));
}

/*
 * 共享的 4 KiB scratch(见 erofs_internal.h 的"共享静态缓冲")。
 * ★ 为什么是 static 而不是栈: 内核给 APP 线程的栈是 4 KiB 量级, 一个 4 KiB 数组放栈上
 *   就把栈用光了(上游在 namei.c 的 `erofs_namei()` 里正是这么写的)。
 * `s_scratch` 只按 `u32` 对齐即可(我们从不把它当 64 位对象用; 所有多字节访问都走
 * `__builtin_memcpy` 或 `__le32*`)。
 */
static br_u32 s_scratch_words[EROFS_MAX_BLOCK_SIZE / 4u];

u8 *uboot_erofs_scratch(void)
{
    return (u8 *)(void *)s_scratch_words;
}

/* =====================================================================
 * 瞬态 inode(公共头 + 私有尾; CA-2)
 * ===================================================================== */

/*
 * ★ 为什么 `path` 是"相对挂载点的路径"而不是绝对路径: 挂载点是**两处真值**
 *   (plugin.toml 的 `[[mount]].path` 与 erofs_internal.h 的 BR_EROFS_MOUNT_PATH),
 *   而 `erofs_ilookup()` 的走查从卷根开始 —— 卷内的名字空间与"挂在哪个绝对路径下"
 *   无关。相对路径让"换个挂载点"这件事不需要改任何一行拼路径的代码。
 */
typedef struct uboot_erofs_ino {
    br_inode_t         head;                          /* 公共头(必须首字段) */
    struct erofs_inode vi;                            /* 当次解析出的 EROFS inode */
    char               path[UBOOT_EROFS_REL_PATH_MAX]; /* 相对挂载点; 根 = "" */
} uboot_erofs_ino_t;

/* 节点身份(与 fs/tmpfs 同款): 用 head.fpriv, 不引入额外的跨插件符号 */
static uboot_erofs_ino_t *uboot_erofs_wrap(const br_inode_t *ino)
{
    return (uboot_erofs_ino_t *)(void *)ino->fpriv;
}

static uboot_erofs_ino_t *uboot_erofs_session(br_file_t *f)
{
    return (uboot_erofs_ino_t *)br_file_fpriv(f);
}

/*
 * 目录迭代器(挂在**文件句柄**的 fpriv 上, 不动 inode 的 fpriv —— 同一个目录可以
 * 被同时打开多次迭代)。生命周期: dir_open 分配 / dir_close 释放。
 *
 * 块级缓存: EROFS 的目录数据是"每 4 KiB 一块, 块头是 dirent 数组, 块尾是名字区"。
 *   `blk`  = 当前已载入 `s_dir_blk` 的块号(相对目录文件开头, 以 4 KiB 为单位)
 *   `de_end` = 该块里 dirent 数组的结束偏移(块首 dirent 的 nameoff)
 *   `max`  = 该块的有效字节数(最后一块可能更短)
 * `pos` 是**目录文件内**的字节偏移, 也就是"下一条 dirent 在哪儿"。
 */
#define UBOOT_EROFS_DIR_NO_BLK  ((br_u32)-1)

typedef struct uboot_erofs_dir_iter {
    uboot_erofs_ino_t *ino;         /* 被枚举的目录(借用; vfs 持有它) */
    erofs_off_t        pos;         /* 目录文件内偏移(下一条 dirent) */
    br_u32             cursor;      /* 0 = ".", 1 = "..", ≥2 = 目录块里的第 (cursor-2) 条 */
    br_u32             blk;         /* 已载入 s_dir_blk 的块号; UBOOT_EROFS_DIR_NO_BLK = 无 */
    br_u32             de_end;      /* 已载入块的 dirent 数组结束偏移(= 名字区起点) */
    br_u32             max;         /* 已载入块的有效字节数 */
    br_u32             skipped;     /* 本目录里因名字超 BR_NAME_MAX 被跳过的条数 */
} uboot_erofs_dir_iter_t;

/*
 * ★ 目录块缓冲: 与"共享 scratch"**分开**的一份静态 4 KiB。
 *   为什么不借共享 scratch: 目录读的流程是"读一块目录数据 → 在块内解析若干条 dirent",
 *   而"读一块目录数据"本身走 `erofs_pread()` —— 它内部会用共享 scratch 做映射/索引解析。
 *   一旦共用, 目录块的内容会被自己的映射读覆盖掉。分开之后两条路径互不干扰,
 *   代价是 4 KiB 静态(见 erofs_internal.h 的"栈纪律": 静态换栈是这里唯一的出路)。
 */
static br_u32 s_dir_blk_words[EROFS_MAX_BLOCK_SIZE / 4u];

static u8 *uboot_erofs_dir_blk(void)
{
    return (u8 *)(void *)s_dir_blk_words;
}

/* =====================================================================
 * 类型翻译(EROFS 的 dirent 文件类型 ↔ brickOS 的 inode 类型)
 * ===================================================================== */

static br_u32 uboot_erofs_type_from_ftype(u8 ftype)
{
    switch (ftype) {
    case EROFS_FT_DIR:     return BR_INODE_DIR;
    case EROFS_FT_SYMLINK: return BR_INODE_SYMLINK;
    case EROFS_FT_REG_FILE:return BR_INODE_FILE;
    default:               return BR_INODE_NONE;
    }
}

static br_u32 uboot_erofs_type_from_mode(umode_t mode)
{
    return uboot_erofs_type_from_ftype(uboot_erofs_ftype_from_mode(mode));
}

/* 私有尾入口(不用 br_inode_priv(): head 首字段的布局下两者同址, 但 fpriv 是 vfs 交给
 * 文件面的契约载体, 用它少一个跨插件符号 —— 与 fs/tmpfs 的取舍一致)。 */
static uboot_erofs_ino_t *uboot_erofs_new(const struct erofs_inode *vi, const char *rel_path,
                                          const br_file_ops_t *fops)
{
    uboot_erofs_ino_t *n = (uboot_erofs_ino_t *)br_malloc(sizeof *n);

    if (n == BR_NULL) {
        return BR_NULL;
    }
    n->head.type  = uboot_erofs_type_from_mode(vi->i_mode);
    n->head.iops  = BR_NULL;              /* 由调用方决定(只有目录需要名字空间面) */
    n->head.fops  = fops;
    n->head.fpriv = n;                    /* 节点身份: vfs 在 open 时把它交给 br_file_t */
    n->vi         = *vi;
    /* rel_path 是我们自己拼出来的 C 串; 按长度复制到这里(与盘上名字的纪律一致)。 */
    uboot_erofs_copy_n(n->path, (br_size_t)sizeof(n->path), rel_path,
                       uboot_erofs_strnlen(rel_path, (br_size_t)sizeof(n->path)));
    return n;
}

/* 相对路径拼接: 父 + '/' + 名字(父为空 ⇒ 就是名字本身)。返回 -ENOSPC 表示放不下。
 * 名字长度在上界内(调用方先查), 所以这里只需查**总长**。 */
static int uboot_erofs_join(char *dst, br_size_t cap, const char *parent, const char *name)
{
    const br_size_t plen = uboot_erofs_strlen(parent);
    const br_size_t nlen = uboot_erofs_strnlen(name, (br_size_t)BR_NAME_MAX);
    const br_size_t need = plen + ((plen != 0u) ? 1u : 0u) + nlen + 1u;

    if ((nlen == 0u) || (nlen >= (br_size_t)BR_NAME_MAX)) {
        return BR_ERR(BR_ENOSPC);
    }
    if (need > cap) {
        return BR_ERR(BR_ENOSPC);
    }
    /* 按**长度**拼, 不靠 TPL 的 "\0"(名字来自 vfs 的分量缓冲, 一定是 C 串; 但这里
     * 统一用长度版, 免得将来有非 C 串的调用者踩到同一个坑)。 */
    if (plen != 0u) {
        (void)__builtin_memcpy(dst, parent, plen);
        dst[plen] = '/';
        (void)__builtin_memcpy(dst + plen + 1u, name, nlen);
        dst[plen + 1u + nlen] = '\0';
    } else {
        (void)__builtin_memcpy(dst, name, nlen);
        dst[nlen] = '\0';
    }
    return 0;
}

/* =====================================================================
 * 层 3: 文件面
 * ===================================================================== */

static int uboot_erofs_file_open(br_file_t *f, br_u32 flags)
{
    const uboot_erofs_ino_t *n = uboot_erofs_session(f);

    if ((n == BR_NULL) || (f == BR_NULL)) {
        return BR_ERR(BR_EINVAL);
    }
    if (n->head.type == BR_INODE_DIR) {
        return BR_ERR(BR_EISDIR);
    }
    /* ★ 只读执法(见文件头): 写权限打开在**这里**就失败, 而不是等到第一次 write。
     *   这样"打开一个只读卷上的文件要写"的调用方立刻拿到正确的 -EROFS。 */
    if (BR_O_WRITABLE(flags) || ((flags & BR_O_TRUNC) != 0u) || ((flags & BR_O_CREAT) != 0u)) {
        return BR_ERR(BR_EROFS);
    }
    return 0;
}

static br_s64 uboot_erofs_file_read(br_file_t *f, void *buf, br_size_t n)
{
    /* 名字取 `node` 而不是 `n`: 本函数的 `n` 是**要读多少字节**(契约签名如此)。 */
    const uboot_erofs_ino_t *node = uboot_erofs_session(f);
    br_s64 off;

    if ((node == BR_NULL) || ((buf == BR_NULL) && (n != 0u))) {
        return BR_ERR(BR_EINVAL);
    }
    if (node->head.type == BR_INODE_DIR) {
        return BR_ERR(BR_EISDIR);
    }
    if (node->head.type != BR_INODE_FILE) {
        return BR_ERR(BR_EINVAL);
    }
    if (n == 0u) {
        return 0;
    }

    off = br_file_offset(f);
    if (off < 0) {
        return BR_ERR(BR_EINVAL);
    }
    if ((erofs_off_t)off >= node->vi.i_size) {
        return 0;                          /* 读到/越过 EOF: 0 字节是**成功** */
    }

    erofs_off_t want = node->vi.i_size - (erofs_off_t)off;
    if ((erofs_off_t)n < want) {
        want = (erofs_off_t)n;
    }

    const int rc = erofs_pread((struct erofs_inode *)&node->vi, (char *)buf, want,
                               (erofs_off_t)off);
    if (rc < 0) {
        return rc;
    }
    br_file_set_offset(f, off + (br_s64)want);
    return (br_s64)want;
}

/* 只读: 写槽位填上(比空槽位更准), 但一律 -EROFS */
static br_s64 uboot_erofs_file_write(br_file_t *f, const void *buf, br_size_t n)
{
    (void)f;
    (void)buf;
    (void)n;
    return BR_ERR(BR_EROFS);
}

static br_s64 uboot_erofs_file_lseek(br_file_t *f, br_s64 off, int whence)
{
    const uboot_erofs_ino_t *n = uboot_erofs_session(f);
    br_s64 base;
    br_s64 pos;

    if ((n == BR_NULL) || (n->head.type != BR_INODE_FILE)) {
        return BR_ERR(BR_EINVAL);
    }
    switch (whence) {
    case BR_SEEK_SET: base = 0; break;
    case BR_SEEK_CUR: base = br_file_offset(f); break;
    case BR_SEEK_END: base = (br_s64)n->vi.i_size; break;
    default:          return BR_ERR(BR_EINVAL);
    }
    if (base < 0) {
        return BR_ERR(BR_EINVAL);
    }
    /* 允许越过 EOF(POSIX): 之后读得 0。用内建判溢出, 免得有符号加溢出(UB)。 */
    if (__builtin_add_overflow(base, off, &pos) || (pos < 0)) {
        return BR_ERR(BR_EINVAL);
    }
    br_file_set_offset(f, pos);
    return pos;
}

static int uboot_erofs_file_close(br_file_t *f)
{
    /* 文件会话没有要归还的东西: fpriv 仍是节点(瞬态节点由 vfs 经 free_inode 释放)。
     * 与目录面成对 —— 那里必须 br_free 迭代器。 */
    (void)f;
    return 0;
}

/* =====================================================================
 * 层 3: 目录面(read = 恰好一条 br_dirent_t)
 * ===================================================================== */

/*
 * 填一条定长目录项。
 * ★ 必须按 `name_len` **按长度**复制, 不能用"扫到 NUL 为止"的复制:
 *   EROFS 目录块里的名字是紧挨着拼起来的, **没有** NUL 结尾 —— 实测症状是第一条
 *   "comp" 会把后面所有名字一起吞进来("compdirhello.txtraw.bin")。
 *   整条先清零, 于是 `name[name_len..]` 全是 '\0'(不把上轮残留漏给调用方)。
 */
static void uboot_erofs_dirent_fill(br_dirent_t *de, br_u32 type, const char *name,
                                    br_u32 name_len)
{
    (void)__builtin_memset(de, 0, sizeof *de);
    de->type     = type;
    de->name_len = name_len;
    uboot_erofs_copy_n(de->name, (br_size_t)BR_NAME_MAX, name, (br_size_t)name_len);
}

static int uboot_erofs_dir_open(br_file_t *f, br_u32 flags)
{
    uboot_erofs_ino_t *n = uboot_erofs_session(f);
    uboot_erofs_dir_iter_t *it;

    (void)flags;    /* vfs 已在 br_open 里挡下"目录写打开 ⇒ -EISDIR" */
    if ((n == BR_NULL) || (n->head.type != BR_INODE_DIR)) {
        return BR_ERR(BR_ENOTSUP);
    }

    it = (uboot_erofs_dir_iter_t *)br_malloc(sizeof *it);
    if (it == BR_NULL) {
        return BR_ERR(BR_ENOMEM);
    }
    it->ino     = n;
    it->pos     = 0;
    it->cursor  = 0u;
    it->blk      = UBOOT_EROFS_DIR_NO_BLK;
    it->de_end   = 0u;
    it->max     = 0u;
    it->skipped = 0u;
    br_file_set_fpriv(f, it);
    return 0;
}

/*
 * 载入 `blk` 号目录块(若它还没在缓冲里)。返回该块的有效字节数, 失败返回负 errno。
 *
 * ★ 目录块的布局(实测确认, 与上游 `fs.c` 的 `erofs_readdir()` 同一口径):
 *     [0, de_end)        : **连续的 dirent 数组**(12 字节一条, 按名字字节序排)
 *     [de_end, max)      : 名字区(顺序拼接; 每条 dirent 的 `nameoff` 指这里)
 *   `de_end` 就是**块首第一条 dirent 的 `nameoff`** —— 它不是"名字区起点"这一个含义,
 *   EROFS 用它表达"本块的 dirent 数组有多长"。于是块内解析非常直接:
 *     idx   = 块内偏移 / 12
 *     namelen = (下一条的 nameoff) - (本条 nameoff); 最后一条则扫到 NUL。
 *   (本件第一版把它误当成"数组起点偏移"而多减了一次, 症状是四条名字被拼成一条 ——
 *    这一条留在注释里, 因为它是"看起来对但差一个常数"的典型。)
 */
static int uboot_erofs_dir_load_block(uboot_erofs_dir_iter_t *it, br_u32 blk)
{
    const uboot_erofs_ino_t *n = it->ino;
    const erofs_off_t blk_off = (erofs_off_t)blk * (erofs_off_t)erofs_blksiz();
    erofs_off_t want;
    const struct erofs_dirent *de;
    br_u32 de_end;
    int rc;

    if ((it->blk == blk) && (it->max != 0u)) {
        return (int)it->max;               /* 已在缓冲里 */
    }

    want = n->vi.i_size - blk_off;
    if (want > (erofs_off_t)erofs_blksiz()) {
        want = (erofs_off_t)erofs_blksiz();
    }
    if (want <= 0) {
        it->blk = blk;
        it->max = 0u;
        return 0;
    }

    rc = erofs_pread((struct erofs_inode *)&n->vi, (char *)uboot_erofs_dir_blk(),
                     want, blk_off);
    if (rc < 0) {
        return rc;
    }

    de = (const struct erofs_dirent *)(const void *)uboot_erofs_dir_blk();
    de_end = (br_u32)le16_to_cpu(de->nameoff);
    if ((de_end < sizeof(struct erofs_dirent)) || (de_end > erofs_blksiz())) {
        erofs_err("invalid de[0].nameoff %u @ nid %llu", de_end,
                  (unsigned long long)n->vi.nid);
        return BR_ERR(BR_EIO);
    }

    it->blk    = blk;
    it->de_end = de_end;
    it->max    = (br_u32)want;
    return (int)it->max;
}

/* 第 `idx` 条(0 基, 相对本块)在块内的字节偏移; 越过数组尾返回 -1。 */
static int uboot_erofs_dir_de_off(const uboot_erofs_dir_iter_t *it, br_u32 idx)
{
    const br_u32 off = idx * (br_u32)sizeof(struct erofs_dirent);

    if ((off + (br_u32)sizeof(struct erofs_dirent)) > it->de_end) {
        return -1;
    }
    return (int)off;
}

static br_s64 uboot_erofs_dir_read(br_file_t *f, void *buf, br_size_t n)
{
    uboot_erofs_dir_iter_t *it = (uboot_erofs_dir_iter_t *)br_file_fpriv(f);
    br_dirent_t *de = (br_dirent_t *)buf;

    if ((it == BR_NULL) || (it->ino == BR_NULL)) {
        return BR_ERR(BR_EINVAL);
    }
    /* 定长记录不可截断: 装不下一条就 -EINVAL(返回 0 会被 br_readdir 读成"目录已穷尽",
     * 返回半条会被判 -EIO 的 FS 违约)。vfs 恒传 sizeof(br_dirent_t)。 */
    if ((buf == BR_NULL) || (n < (br_size_t)sizeof(br_dirent_t))) {
        return BR_ERR(BR_EINVAL);
    }

    /* "." 与 ".." 就地合成(EROFS 的目录块里**不含**这两条 —— 上游同样如此) */
    if (it->cursor == 0u) {
        it->cursor = 1u;
        uboot_erofs_dirent_fill(de, BR_INODE_DIR, ".", 1u);
        return (br_s64)sizeof(br_dirent_t);
    }
    if (it->cursor == 1u) {
        it->cursor = 2u;
        uboot_erofs_dirent_fill(de, BR_INODE_DIR, "..", 2u);
        return (br_s64)sizeof(br_dirent_t);
    }

    for (;;) {
        if (it->pos >= (erofs_off_t)it->ino->vi.i_size) {
            break;                          /* 文件读完: 穷尽 */
        }

        const br_u32 blk = (br_u32)((erofs_off_t)it->pos >> uboot_erofs_sbi.blkszbits);
        if (blk != it->blk) {
            const int m = uboot_erofs_dir_load_block(it, blk);
            if (m < 0) {
                return m;
            }
            if (m == 0) {
                it->pos = it->ino->vi.i_size;   /* 越过末块: 收敛 */
                break;
            }
        }

        const br_u32 in_blk = (br_u32)erofs_blkoff((erofs_off_t)it->pos);

        /* ★ 块的 dirent 数组以**块首第一条的 nameoff** 为界(`de_end`):
         *   - 本块里 `in_blk >= de_end` 的位置已经属于名字区(读下去是垃圾) ⇒ 跳下一块;
         *   - 否则 `in_blk` 必然落在数组里(数组是 12 字节步进的)。 */
        if ((in_blk + (br_u32)sizeof(struct erofs_dirent)) > it->de_end) {
            it->pos = (erofs_off_t)(blk + 1u) * (erofs_off_t)erofs_blksiz();
            it->blk = UBOOT_EROFS_DIR_NO_BLK;
            continue;
        }

        const struct erofs_dirent *dent =
            (const struct erofs_dirent *)(const void *)(uboot_erofs_dir_blk() + in_blk);
        const br_u32 nameoff  = (br_u32)le16_to_cpu(dent->nameoff);
        const char  *de_name  = (const char *)(uboot_erofs_dir_blk() + nameoff);
        const br_u32 de_index = in_blk / (br_u32)sizeof(struct erofs_dirent);
        const int    next_off = uboot_erofs_dir_de_off(it, de_index + 1u);
        const br_u32 type     = uboot_erofs_type_from_ftype(dent->file_type);
        br_u32       de_namelen;

        /* 推进游标(无论这条是否被采纳) */
        it->pos = (erofs_off_t)blk * (erofs_off_t)erofs_blksiz() +
                  (erofs_off_t)(in_blk + (br_u32)sizeof(struct erofs_dirent));

        if (nameoff >= it->max) {
            erofs_err("bogus dirent nameoff %u @ nid %llu", nameoff,
                      (unsigned long long)it->ino->vi.nid);
            return BR_ERR(BR_EIO);
        }

        if (next_off < 0) {
            /* 本块最后一条: 名字扫到 NUL, 上界 = 该块有效字节数 */
            de_namelen = (br_u32)uboot_erofs_strnlen(de_name,
                                                     (br_size_t)(it->max - nameoff));
        } else {
            const struct erofs_dirent *next =
                (const struct erofs_dirent *)(const void *)
                (uboot_erofs_dir_blk() + (br_u32)next_off);
            const br_u32 next_nameoff = (br_u32)le16_to_cpu(next->nameoff);

            de_namelen = (next_nameoff > nameoff) ? (next_nameoff - nameoff) : 0u;
        }

        /* 损坏的条目: 与上游同款判据 */
        if ((nameoff + de_namelen > it->max) || (de_namelen > EROFS_DIRENT_MAX_NAME)) {
            erofs_err("bogus dirent @ nid %llu", (unsigned long long)it->ino->vi.nid);
            return BR_ERR(BR_EIO);
        }
        if (de_namelen == 0u) {
            continue;                       /* 空名字不是合法目录项: 跳过 */
        }
        if (type == BR_INODE_NONE) {
            continue;                       /* 设备文件/socket 等: 本件的 VFS 面没有它们 */
        }
        if (de_namelen >= (br_u32)BR_NAME_MAX) {
            /* ★ 名字上界取舍(见文件头): 跳过并计数, 不让一条超长名字把整个 readdir
             *   变成 -ENOSPC(vfs 的分量缓冲也装不下它)。 */
            it->skipped++;
            continue;
        }

        uboot_erofs_dirent_fill(de, type, de_name, de_namelen);
        return (br_s64)sizeof(br_dirent_t);
    }

    if (it->skipped != 0u) {
        /* 只报一次(穷尽那一刻), 免得目录被反复读时刷屏 */
        br_log_warn("uboot-erofs: %s 下有 %u 个名字超过 %u 字节, 已在本仓库的目录面上跳过",
                    (it->ino->path[0] != '\0') ? it->ino->path : "/",
                    (unsigned)it->skipped, (unsigned)BR_NAME_MAX - 1u);
        it->skipped = 0u;
    }
    return 0;                               /* 穷尽 */
}

static int uboot_erofs_dir_close(br_file_t *f)
{
    uboot_erofs_dir_iter_t *it = (uboot_erofs_dir_iter_t *)br_file_fpriv(f);

    if (it != BR_NULL) {
        br_free(it);
        br_file_set_fpriv(f, BR_NULL);      /* 让二次 close 成为空操作 */
    }
    return 0;
}

/* =====================================================================
 * ops 表
 * ===================================================================== */

static const br_file_ops_t s_file_fops = {
    .open        = uboot_erofs_file_open,
    .read        = uboot_erofs_file_read,
    .write       = uboot_erofs_file_write,   /* 填上但恒 -EROFS(只读 FS 的正确码) */
    .lseek       = uboot_erofs_file_lseek,
    .ioctl       = BR_NULL,
    .fsync       = BR_NULL,                  /* 只读卷没有"落介质"这件事 */
    .poll        = BR_NULL,
    .poll_attach = BR_NULL,                  /* v2 预留(wait-queue), v1 恒 NULL */
    .close       = uboot_erofs_file_close,
};

static const br_file_ops_t s_dir_fops = {
    .open        = uboot_erofs_dir_open,
    .read        = uboot_erofs_dir_read,
    .write       = BR_NULL,
    .lseek       = BR_NULL,
    .ioctl       = BR_NULL,
    .fsync       = BR_NULL,
    .poll        = BR_NULL,
    .poll_attach = BR_NULL,
    .close       = uboot_erofs_dir_close,
};

static br_inode_t *uboot_erofs_lookup(br_inode_t *dir, const char *name);

/* 层 2。★ 名字空间变更的 8 个槽位**全部填上**且一律 -EROFS(只读 FS 的正确错误码,
 * 比 vfs 对空槽位给的 -ENOTSUP 更准 —— 调用方据此能区分"这个 FS 不支持 create"与
 * "这个卷是只读的")。`readlink` 是**只读**操作, 支持(EROFS 有 symlink)。
 *
 * ★ 为什么是 6 个不同签名的小函数而不是一个 void 变体: `br_inode_ops_t` 的每个槽位
 *   签名不同, C 不允许把 `int f(void)` 赋给 `int (*)(br_inode_t*, const char*)`
 *   (参数个数不同就是不同类型)⇒ 每个签名各写一个壳, 函数体一样(返回 -EROFS)。 */
static int uboot_erofs_ro_create(br_inode_t *dir, const char *name, br_u32 flags,
                                 br_inode_t **out);
static int uboot_erofs_ro_name  (br_inode_t *dir, const char *name);
static int uboot_erofs_ro_rename(br_inode_t *dir, const char *name, br_inode_t *ndir,
                                 const char *nname);
static int uboot_erofs_ro_symlink(br_inode_t *dir, const char *name, const char *target);
static int uboot_erofs_ro_link   (br_inode_t *dir, const char *name, br_inode_t *target);
static int uboot_erofs_setattr  (br_inode_t *ino, const br_stat_t *st);
static int uboot_erofs_getattr  (br_inode_t *ino, br_stat_t *st);
static int uboot_erofs_readlink (br_inode_t *ino, char *buf, br_size_t cap);

static const br_inode_ops_t s_iops = {
    .lookup   = uboot_erofs_lookup,
    .create   = uboot_erofs_ro_create,
    .unlink   = uboot_erofs_ro_name,
    .mkdir    = uboot_erofs_ro_name,
    .rmdir    = uboot_erofs_ro_name,
    .rename   = uboot_erofs_ro_rename,
    .getattr  = uboot_erofs_getattr,
    .setattr  = uboot_erofs_setattr,
    .symlink  = uboot_erofs_ro_symlink,
    .readlink = uboot_erofs_readlink,
    .link     = uboot_erofs_ro_link,
};

/*
 * ★ 层 2 的第二张/第三张表: **非目录**节点也必须有一张 iops。
 *   契约头写得很清楚(br_vfs.h 的 br_inode 注释): "`getattr`/`setattr` 属**本层**
 *   (inode 级) —— 于是每个支持 `br_stat` 的 inode 都必须带一张 iops(哪怕只填 getattr)"。
 *   只给目录 iops 的后果是 `br_stat("/assets/hello.txt")` 得到 -ENOTSUP(实测),
 *   而不是文件属性 —— 这是本件第一版**真踩到**的一个契约细节。
 *
 *   于是本件用三张表:
 *     s_iops       只给目录: 名字空间面(变更槽位一律 -EROFS) + getattr
 *     s_file_iops  只给普通文件: 只有 getattr(名字空间面为空 ⇒ BR_NULL = 不支持)
 *     s_link_iops  只给符号链接: getattr + readlink(链接**自己**的属性与目标串)
 */
static const br_inode_ops_t s_file_iops = {
    .getattr  = uboot_erofs_getattr,
    /* ★ `.setattr` 也必须填: `br_truncate()` 走的是**文件自己**的 iops->setattr
     *   (vfs 的 br_truncate 先 resolve_leaf 再调 ino->iops->setattr)。第一版这里留了
     *   空槽位 ⇒ `br_truncate("/assets/hello.txt", 0)` 得到的是 vfs 对空槽位的 -ENOTSUP,
     *   而不是只读卷该给的 -EROFS(实测: TC-EROFS-007 因此判红)。 */
    .setattr  = uboot_erofs_setattr,
    .readlink = BR_NULL,
};

static const br_inode_ops_t s_link_iops = {
    .getattr  = uboot_erofs_getattr,
    .setattr  = uboot_erofs_setattr,
    .readlink = uboot_erofs_readlink,
};

/* 层 1: super。`unmount` 是 v1 的签名先行槽位(运行时卸载不早于 v3)⇒ BR_NULL;
 * `sync` = 0: 只读卷没有脏数据。 */
static int   uboot_erofs_mount(void *fs_priv, const br_fs_cfg_t *cfg, br_inode_t **root);
static void  uboot_erofs_free_inode(br_inode_t *ino);
static int   uboot_erofs_sync(void *fs_priv);

static const br_fs_ops_t s_fs_ops = {
    .mount      = uboot_erofs_mount,
    .free_inode = uboot_erofs_free_inode,
    .unmount    = BR_NULL,
    .sync       = uboot_erofs_sync,
};

/* =====================================================================
 * 层 2 实现
 * ===================================================================== */

/*
 * lookup: 把"父 inode + 一个名字"翻成"相对挂载点的路径", 再交给上游的路径走查。
 * 未命中/名字超长/类型不认识都收敛为 BR_NULL(vfs 的 `resolve_norm` 靠 BR_NULL 区分
 * "不存在"与"失败" ⇒ 得到 -ENOENT)。超长名字本件在任务书里约定返回 -ENOSPC, 但
 * lookup 的签名**没有错误码通道**(返回 inode 指针), 所以这里只能 BR_NULL;
 * -ENOSPC 的口径落在 vfs 自己那里(`resolve_norm` 对超长分量给 -ENOSPC, 一致)。
 */
static br_inode_t *uboot_erofs_lookup(br_inode_t *dir, const char *name)
{
    uboot_erofs_ino_t *d;
    uboot_erofs_ino_t *child;
    char               rel[UBOOT_EROFS_REL_PATH_MAX];
    struct erofs_inode vi;
    const br_file_ops_t *fops;
    int rc;

    if ((dir == BR_NULL) || (dir->type != BR_INODE_DIR) || (name == BR_NULL)) {
        return BR_NULL;
    }
    rel[0] = '\0';
    d = uboot_erofs_wrap(dir);
    if (d == BR_NULL) {
        return BR_NULL;
    }

    if (uboot_erofs_strcmp(name, ".") == 0) {
        return dir;                        /* 自己是瞬态节点, 直接回它(vfs 会 ino_put 一次) */
    }
    rc = uboot_erofs_join(rel, (br_size_t)sizeof(rel), d->path, name);
    if (rc != 0) {
        /* 名字超长(≥ BR_NAME_MAX): 本件的口径是"跳过/不存在"。 */
        return BR_NULL;
    }

    vi.nid = 0;
    rc = erofs_ilookup(rel, &vi);
    if (rc != 0) {
        return BR_NULL;
    }

    const br_u32 type = uboot_erofs_type_from_mode(vi.i_mode);
    if (type == BR_INODE_DIR) {
        fops = &s_dir_fops;
    } else if (type == BR_INODE_FILE) {
        fops = &s_file_fops;
    } else if (type == BR_INODE_SYMLINK) {
        fops = BR_NULL;                    /* 链接"看得见但打不开"(vfs 会展开或 -ELOOP) */
    } else {
        return BR_NULL;                    /* 设备文件/socket: 本件的 VFS 面没有它们 */
    }

    child = uboot_erofs_new(&vi, rel, fops);
    if (child == BR_NULL) {
        return BR_NULL;
    }
    /* 每个节点都要有 getattr(见上面的说明) */
    if (type == BR_INODE_DIR) {
        child->head.iops = &s_iops;
    } else if (type == BR_INODE_SYMLINK) {
        child->head.iops = &s_link_iops;
    } else {
        child->head.iops = &s_file_iops;
    }
    return &child->head;
}

static int uboot_erofs_getattr(br_inode_t *ino, br_stat_t *st)
{
    const uboot_erofs_ino_t *n;

    if ((ino == BR_NULL) || (st == BR_NULL)) {
        return BR_ERR(BR_EINVAL);
    }
    n = uboot_erofs_wrap(ino);
    if (n == BR_NULL) {
        return BR_ERR(BR_EINVAL);
    }

    /* 目录的 size 按本仓库的口径报 0(tmpfs 同款: `br_stat_t.size` 对目录没有意义);
     * 链接的 size = 目标串长度(契约头如此), 但本件不展开目标、也不把目标串读出来
     * 就能知道长度 —— `i_size` 就是它。 */
    st->valid = BR_STAT_SIZE;
    st->type  = ino->type;
    st->size  = (ino->type == BR_INODE_DIR) ? 0u : n->vi.i_size;
    st->nlink = 0u;                        /* 不置 BR_STAT_NLINK 位: 只读卷上它没有意义 */
    return 0;
}

/* ---- 只读执法: 名字空间变更(6 个签名, 一个答复: -EROFS) ---- */

static int uboot_erofs_ro_create(br_inode_t *dir, const char *name, br_u32 flags,
                                 br_inode_t **out)
{
    (void)dir;
    (void)name;
    (void)flags;
    if (out != BR_NULL) {
        *out = BR_NULL;
    }
    return BR_ERR(BR_EROFS);
}

static int uboot_erofs_ro_name(br_inode_t *dir, const char *name)
{
    (void)dir;
    (void)name;
    return BR_ERR(BR_EROFS);
}

static int uboot_erofs_ro_rename(br_inode_t *dir, const char *name, br_inode_t *ndir,
                                 const char *nname)
{
    (void)dir;
    (void)name;
    (void)ndir;
    (void)nname;
    return BR_ERR(BR_EROFS);
}

static int uboot_erofs_ro_symlink(br_inode_t *dir, const char *name, const char *target)
{
    (void)dir;
    (void)name;
    (void)target;
    return BR_ERR(BR_EROFS);
}

static int uboot_erofs_ro_link(br_inode_t *dir, const char *name, br_inode_t *target)
{
    (void)dir;
    (void)name;
    (void)target;
    return BR_ERR(BR_EROFS);
}

static int uboot_erofs_setattr(br_inode_t *ino, const br_stat_t *st)
{
    (void)ino;
    (void)st;
    return BR_ERR(BR_EROFS);
}

/*
 * readlink: EROFS 支持符号链接(目标串就存在 inode 的数据面里, 用 `erofs_pread` 读)。
 * 契约: 返回**全长**(截断不报错), `cap == 0` 或 `buf == NULL` 时只回报长度;
 * 非链接 ⇒ -EINVAL。
 */
static int uboot_erofs_readlink(br_inode_t *ino, char *buf, br_size_t cap)
{
    const uboot_erofs_ino_t *n;
    erofs_off_t len;
    int rc;

    if (ino == BR_NULL) {
        return BR_ERR(BR_EINVAL);
    }
    if (ino->type != BR_INODE_SYMLINK) {
        return BR_ERR(BR_EINVAL);
    }
    n = uboot_erofs_wrap(ino);
    if (n == BR_NULL) {
        return BR_ERR(BR_EINVAL);
    }
    len = n->vi.i_size;
    if (len == 0u) {
        return 0;
    }
    if ((buf == BR_NULL) || (cap == 0u)) {
        return (int)len;
    }

    const br_size_t cnt = ((br_size_t)len < cap) ? (br_size_t)len : (cap - 1u);

    /* 只读 cnt 个字节(不读全长): 目标串可能比 cap 长, 读多了会越过调用方的缓冲。 */
    rc = erofs_pread((struct erofs_inode *)&n->vi, buf, (erofs_off_t)cnt, 0);
    if (rc < 0) {
        return rc;
    }
    buf[cnt] = '\0';
    return (int)len;                        /* 契约: 返回全长 */
}

/* =====================================================================
 * 层 1 实现: mount / free_inode / sync
 * ===================================================================== */

static void uboot_erofs_free_inode(br_inode_t *ino)
{
    /* ★ 只读 FS 的瞬态节点就是一块 br_malloc 出来的包装(没有子对象)。
     *   挂载表的根 inode **不会**被传进来(vfs 的 ino_put 按 is_root 分流)。 */
    if (ino != BR_NULL) {
        br_free(ino);
    }
}

static int uboot_erofs_sync(void *fs_priv)
{
    (void)fs_priv;
    return 0;                              /* 只读卷: 没有脏数据要落 */
}

static int uboot_erofs_mount(void *fs_priv, const br_fs_cfg_t *cfg, br_inode_t **root)
{
    struct erofs_inode vi;
    uboot_erofs_ino_t *r;
    int rc;

    (void)fs_priv;                         /* 本件没有 FS 级记账体: 卷状态就是 sbi */

    if ((cfg == BR_NULL) || (root == BR_NULL)) {
        return BR_ERR(BR_EINVAL);
    }
    *root = BR_NULL;

    /* 挂载点校验: 与 fs/tmpfs 刻意不同 —— 本件的**卷**与挂载点无关, 但"挂在哪"是
     * manifest 的事实。这里只做自洽检查(不匹配就报 -EINVAL, 而不是默默挂错地方)。 */
    if ((cfg->path == BR_NULL) || (uboot_erofs_strcmp(cfg->path, BR_EROFS_MOUNT_PATH) != 0)) {
        br_log_error("uboot-erofs: 挂载点必须是 %s(实得 %s)", BR_EROFS_MOUNT_PATH,
                     (cfg->path != BR_NULL) ? cfg->path : "(null)");
        return BR_ERR(BR_EINVAL);
    }

    /* 1) 块设备: 取 emmc3 并记住几何(扇区大小/介质字节数) */
    rc = uboot_erofs_bdev_bind();
    if (rc != 0) {
        return rc;                          /* 首败即停机(见 plugin.toml 的口径) */
    }

    /* 2) 超级块: magic 必须在**偏移 1024**(EROFS_SUPER_OFFSET)处 */
    rc = erofs_read_superblock();
    if (rc != 0) {
        return rc;
    }
    if (erofs_blksiz() != EROFS_MAX_BLOCK_SIZE) {
        /* 上游的压缩路径只支持 PAGE_SIZE 块; 未压缩面虽然支持 512/1024/2048, 但本件的
         * 静态缓冲按 4 KiB 设计 ⇒ 统一要求 4 KiB(README 的裁剪口径写明了)。 */
        br_log_error("uboot-erofs: 只支持 %u 字节块(镜像 blkszbits=%u)",
                     (unsigned)EROFS_MAX_BLOCK_SIZE, (unsigned)uboot_erofs_sbi.blkszbits);
        return BR_ERR(BR_ENOTSUP);
    }

    /* 3) 根 inode(路径 = 空串 = 卷根) */
    vi.nid = 0;
    rc = erofs_ilookup("", &vi);
    if (rc != 0) {
        br_log_error("uboot-erofs: 解析根 inode 失败 rc=%d", rc);
        return rc;
    }
    if (!S_ISDIR(vi.i_mode)) {
        br_log_error("uboot-erofs: 根 inode 不是目录(mode=%o)", (unsigned)vi.i_mode);
        return BR_ERR(BR_EIO);
    }

    /* 4) 造根 inode(由挂载表长期持有; 同样是 br_malloc, 只是永不 free) */
    r = uboot_erofs_new(&vi, "", &s_dir_fops);
    if (r == BR_NULL) {
        return BR_ERR(BR_ENOMEM);
    }
    r->head.iops = &s_iops;
    *root = &r->head;

    /* ★ 这一行的措辞是**门禁契约的一部分**(tests/gates.toml 的 `erofs-test` 逐字要求
     *   `uboot-erofs: /assets 就绪` 这个子串): 换措辞之前先去看那条 gate。 */
    br_log_info("uboot-erofs: %s 就绪(%s; %s; %llu 块 × %u B; root_nid=%llu; %s)",
                cfg->path, BR_EROFS_TYPE_NAME, BR_EROFS_DEV_NAME,
                (unsigned long long)uboot_erofs_sbi.total_blocks,
                (unsigned)erofs_blksiz(),
                (unsigned long long)uboot_erofs_sbi.root_nid,
                (uboot_erofs_sbi.feature_incompat & EROFS_FEATURE_INCOMPAT_ZERO_PADDING)
                    ? "LZ4/0padding" : "LZ4");
    return 0;
}

/* =====================================================================
 * 生命周期钩子(symbol_prefix = uboot_erofs)
 * ===================================================================== */

int uboot_erofs_early_init(void)
{
    return 0;   /* 此刻无堆、无块设备: 真正的活都在 start */
}

int uboot_erofs_init(void)
{
    return 0;   /* 见 plugin.toml: 挂载落在 start(块设备由 uboot-mmc 的 start 注册,
                 * 本插件在 [[dep]] 里声明了对它的 init 边 ⇒ 相位顺序由插件管理器保证) */
}

int uboot_erofs_start(void)
{
    const int rc = br_mount_register(BR_EROFS_MOUNT_PATH, &s_fs_ops, BR_NULL);

    if (rc != 0) {
        /* ★ 首败即停机是**刻意的**(任务书): 卷挂不上就没有 /assets, 与其让
         *   APP 在半残的命名空间里跑, 不如在这里停住并把原因打出来。 */
        br_log_error("uboot-erofs: 挂载 %s 失败 rc=%d(首败即停机)", BR_EROFS_MOUNT_PATH, rc);
        return rc;
    }
    return 0;
}
