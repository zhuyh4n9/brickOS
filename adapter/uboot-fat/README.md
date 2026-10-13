# adapter/uboot-fat — u-boot FAT(上游**原样编译**; ability / fs / third_party)

> ⚠ **本插件是 GPL-2.0-or-later**: 参与编译的上游文件与我们的适配层都在这个目录里, 来源与逐条偏离见 [NOTICE](NOTICE), 全文见 [LICENSE.GPL-2.0](LICENSE.GPL-2.0)。它与 [uboot-mmc](../uboot-mmc/README.md)、[uboot-erofs](../uboot-erofs/README.md) 一起构成本仓库的 GPL 代码区。

## 1. 它是什么

把 u-boot 的 **FAT12/16/32 读写实现**接进 brickOS, 出口是 **VFS 契约**(`framework/vfs-core/include/br/vfs/br_vfs.h`), 挂三个卷。上游原件直接参与编译, 插件里没有一份副本:

```
framework/vfs-core(br_vfs.h)  br_open / br_stat / br_opendir / …(路径走查在 vfs-core)
        │  br_fs_ops / br_inode_ops / br_file_ops
glue/fat_vfs.c   瞬态路径包装 inode; 目录 read br_dirent_t
        │  上游的**路径风格 API**(fat_read_file / …)
third_party/uboot-imx/fs/fat/fat_write.c   ★ 原件, 逐字节上游(它第 23 行 #include "fat.c" ⇒ 读+写一个 TU)
        │
glue/fat_blk.c   blk_dread/blk_dwrite(切块 + 越界拒绝)+ 卷表 + BPB 观测(挂载判据)+ 环境的那几个实现
        │
framework/bdev-core  br_bdev_ops_t(read/write/flush/geometry)
        │  emmc0 / emmc1 / emmc2
adapter/uboot-mmc(它在自己的 start 相注册三块块设备)
```

上游 .c 拿到的是**环境**: `compat/` 里每个 u-boot 头名一个薄壳(全部接到 `compat/fat_env.h`), 盘上结构则**转发**到 vendor 原件(`<fat.h>`/`<fs.h>`/`<rtc_def.h>`)。判据(全机械):

```bash
bash tools/check-third-party-pristine.sh     # vendor 逐字节 = 上游(28 个文件的 sha256 锁)
python3 tools/check-adapter-copies.py        # adapter/ 里没有三方件源码副本
cc -fsyntax-only <vendor>/fs/fat/fat_write.c -Iadapter/uboot-fat/compat …   # 上游原样编过
```

### 三个卷

| bdev | 容量 | FAT 类型 | 挂载点 | 卷 id |
|---|---|---|---|---|
| `emmc0` | 2 MiB | FAT12 | `/fat12` | `UB_FAT_VOL12` |
| `emmc1` | 16 MiB | FAT16 | `/fat16` | `UB_FAT_VOL16` |
| `emmc2` | 32 MiB | FAT32 | `/fat32` | `UB_FAT_VOL32` |

设备名/挂载点在这张表里是**自检的期望值**(`UB_FAT_DEV*`/`UB_FAT_MNT*`, 见 [glue/fat_glue.h](glue/fat_glue.h)); **运行期的对应关系由 APP 的 `[[mount]]` 挂载计划给出**: APP 调 `mount("emmc0", "/fat12", "fat", 0, NULL)`, 本件用 `uboot_fat_vol_claim(dev, mnt)` 认领一个卷槽, 于是"哪个挂载点用哪块设备"只有**一处真值**, 从前那处 `br-wa-fs-001` 的"两处真值"欠债在 fat 这一侧消掉了。自检仍按这张期望表逐卷对账(名不副实 ⇒ TC-FAT-001 红)。

### 多卷: 上游只有"当前卷", 切换是 glue 的责任

上游 `fs/fat/fat.c` 里是 `static struct blk_desc *cur_dev; static struct disk_partition cur_part_info;` —— **只认一个当前卷**。本件挂三个卷, 所以 **glue 在每个入口都先 `uboot_fat_vol_bind(vol)`**: 把卷切成"当前"(`s_cur_vol`)、按几何刷新 shim 的 `struct blk_desc`/`disk_partition`(整设备一个分区: 起始扇区 0)、调**上游自己的** `fat_set_blk_dev()` 做引导扇区校验; 已绑定过的卷再 bind 只切指针, 不重读 BPB。`fat_vfs.c` 的每一处上游 API 调用之前都有这一次 bind(共 12 处), 忘一处就会拿上一个卷的 FAT 参数读这一个卷。

## 2. 生命周期

| 相 | 做什么 | 为什么 |
|---|---|---|
| `early_init` | 空手返回 | 堆还没建池(`br_mem_init` 在 core.init) |
| `init` | 只填挂载表(`s_slots`) | 真正要分配的动作都在 start |
| `start` | `br_fs_driver_register("fat", …)`(★ ADR-0030 起: **只登记驱动, 不挂载**); 真正的挂载由 APP 的挂载计划 `mount("emmcN", "/fatNN", "fat", …)` 触发 → ops.mount 里 `uboot_fat_vol_claim(dev, mnt)` 认领卷 → `uboot_fat_vol_bind` → BPB 观测 + 几何校验 → 造根 inode | 块设备由 `adapter/uboot-mmc` 在**它的** start 相注册; `[[dep]] kind = "init"` 保证相位顺序 |

**挂载失败 ⇒ 立刻返回负 errno(首败即停机是刻意的)**: FAT 卷是产品的数据面, 缺一个卷还继续跑只会让调用方在后面某个不相干的地方拿到 -ENOENT。

> 挂载权已从本插件移到 **APP 的挂载计划**(插件只登记 `fat` 驱动并把 `(源设备, 挂载点)` 认领成卷), 所以 `fat-test` 门禁里的 `vfs: mount /fat12|16|32` / `uboot-fat: /fatNN 就绪` 两条判据**依赖 APP 侧挂载计划落地**; 在它落地之前, 本插件的自检会报 `mount_count got=0 want=3`(判据本身**没有**放宽)。

## 3. inode = 瞬态路径包装(设计 SD-3, 与 fs/tmpfs 相反的一侧)

```c
struct uboot_fat_ino {          /* 每次 lookup/create 造一个, free_inode 释放 */
    br_inode_t head;            /* type / iops / fops / fpriv(CA-2 首字段) */
    br_u8      vol;             /* 卷 id */
    br_u8      type;            /* BR_INODE_DIR / BR_INODE_FILE */
    br_u32     size;            /* 大小的**快照**; 真值在介质上的目录项里 */
    char       path[256];       /* **相对挂载点**的路径; 根 = "" */
};
```

- "节点"没有持久身份 —— 每次走查都重建, 胶水层直接把路径交给上游的路径风格 API。
- **所有权**: `lookup`/`create` 交出的节点由调用方经 `free_inode`(= `br_free`)释放; 唯一的例外是**挂载根** —— mount 用 `br_malloc` 造好、挂在 `uboot_fat_fs_t` 上长期持有。`size` 只是快照: `getattr`/`lseek(SEEK_END)`/`read` 每次都**问介质**。
- 目录句柄的**会话状态**(合成 `.`/`..` 的游标)由 glue 自己持有(`uboot_fat_dirstream_t`): 上游 `fat_opendir()` 交回的 `struct fs_dir_stream *` 是不透明的。

## 4. 与 VFS 契约的对接口径

| 契约项 | 本插件的答复 |
|---|---|
| `iops->lookup` | 新建瞬态节点; 未命中 `BR_NULL`; "." / ".." 就地解析(根的父亲 = 根) |
| `iops->create` | 先 `file_fat_write_at(…, size = 0)` 把目录项建出来; `O_EXCL` 且已存在 ⇒ -EEXIST |
| `iops->unlink` | 目录 ⇒ -EISDIR(FAT 的 `fat_unlink` 只删空目录, 但那是 **rmdir** 的语义) |
| `iops->rmdir` | 非目录 ⇒ -ENOTDIR; 非空目录由上游返回 -EINVAL(见"欠债") |
| `iops->rename` | 跨卷 ⇒ -EXDEV; "目录搬进自己的子树"由上游 `fat_rename` 拒 |
| `iops->getattr` | 只置 `BR_STAT_SIZE`; `nlink` **不置位**(FAT 没有硬链接) |
| `iops->setattr` | 只认 `BR_STAT_SIZE` ⇒ `uboot_fat_truncate()`; 其他位 ⇒ -ENOTSUP |
| `iops->symlink/readlink/link` | 三个槽位显式 `BR_NULL` ⇒ -ENOTSUP(FAT 没有链接) |
| 文件 `read/write/lseek` | 带偏移语义(读: 越过 EOF ⇒ 0; 写: `pos > 现长` ⇒ -EINVAL, 上游"不许空洞") |
| 文件 `fsync` | **已实现**: `bdev.flush()`(与 `fs_ops.sync` **共用** `uboot_fat_flush_volume()`) |
| 文件 `ioctl/poll/poll_attach` | `BR_NULL`(契约: 缺失槽位 ⇒ -ENOTSUP) |
| 目录 `read` | 每次填**恰好一条** `br_dirent_t` 并返回 `sizeof(br_dirent_t)`; 穷尽返回 0; 头两条是 `.`/`..` |
| `fs_ops.sync` | `bdev.flush()`(契约: flush **必须**落介质)。**为什么不需要回写 FAT 缓存**: 见 §5 |
| `fs_ops.unmount` | `BR_NULL`(v1 的签名先行槽位) |

**名字上界**: 本仓库的 `br_dirent_t.name[]` 只有 `BR_NAME_MAX`(32)字节, 而 FAT 的长名可到 255 字节 ⇒ 超过 31 字节的名字在 `readdir` 里返回 `BR_ERR(ENOSPC)`(与 fs/tmpfs 同口径), `create`/`mkdir` 里也拒绝。**代价**: 宿主预置的 `long name file.txt`(17 字节)可以正常列出, 但 > 31 字节的文件名在本 VFS 面上不可见 —— 见"欠债"。

## 5. 截断: 只用上游公开 API(上游没有 truncate)

用上游自己的写语义拼三种情形:

| 情形 | 实现 | 为什么成立(对着上游源码核过) |
|---|---|---|
| **变短** | `file_fat_write_at(path, newsize, NULL, 0, &act)` | 上游先把目录项 `size` 改成 `pos+size`; `set_contents()` 在 `filesize <= cur_pos` 时走 "truncate the rest": `clear_fatent()` 还掉尾部簇链 + 给末簇打 EOC。`get_set_cluster(size == 0)` 立刻返回 ⇒ **NULL buffer 不会被解引用** |
| **变 0** | 同上(`newsize == 0`) | 上游 `filesize == 0` 的专门分支: 清整条链 + 首簇置 0 + size = 0 |
| **变长** | 从现长起**追加显式零字节**(512 B/次分块) | 上游**没有"扩洞"语义**: 它的 `set_contents` 在 `pos` 落在链尾之外时会走 `assert(!pos)`/`assert(pos == cur_pos)` 那两个死断言, 然后报 "something wrong" 返回 -1(实测)。TC-FAT-008 要求的正是"新区域读回 0"(不是上一个文件的残留), 所以"追加零"与判据同义 —— **判据没有放宽**, 只是把"扩展"写成上游真能接受的形式 |

判据在 `TC-FAT-008`: 1000 B → 300 B 缩短后**读回必须恰好 300 字节**(多一个字节就说明链没释放); 再 300 B → 600 B 扩大后 `[300, 600)` 必须全是 0。

### sync 只剩 `bdev.flush()` 的原因

上游每个写入口(`file_fat_write_at`/`fat_unlink`/`fat_mkdir`/`fat_rename`)在返回前就把 dirty 的 FAT 窗口(`flush_dirty_fat_buffer`)与目录项(`flush_dir`)落盘了, 而它的 `fsdata` 是**每次调用自带、返回前 free** 的局部量。两次 API 调用之间不存在未落盘的插件内状态, `sync` 的语义就退化成"让设备把已写扇区真正落介质"。

## 6. 一致性用例(`TC-FAT-*`, [glue/fat_selftest.c](glue/fat_selftest.c))

| id | 判据 |
|---|---|
| TC-FAT-001 | 三个挂载点在挂载表里; 每卷 BPB 判出的 FAT 类型(12/16/32)与**簇数区间**自洽, 设备名对得上 |
| TC-FAT-002 | 每卷读**宿主预置**的 `hello.txt` 并逐字节比对 |
| TC-FAT-003 | 每卷 `readdir` 根: 头两条是 `.`/`..`; `hello.txt`(type/长度)与 `dir/`(目录)可见; `dir/a.bin` 的 size 与宿主一致 |
| TC-FAT-004 | `/fat16` 新建 → 三段各 512 B 写(恰好每段一簇)→ 关 → 重开读回 1536 B 逐字节一致 |
| TC-FAT-005 | `SEEK_END` 落在文件尾; EOF 读返回 0; `SEEK_SET(3)` 后读到第 4 字节(1 字节缓冲!); 负偏移 ⇒ 负 errno |
| TC-FAT-006 | `/fat32` `mkdir` + 在其中建 100 B 文件 + `stat` 的 type/size 与内容都对 |
| TC-FAT-007 | 改名后旧名 `stat` ⇒ -ENOENT, 新名内容逐字节一致 |
| TC-FAT-008 | `truncate` 缩短(读回恰好 300 B)与扩大(新区域全 0); `unlink` 后 ⇒ -ENOENT; 卷级 `sync == 0` **且文件级 `br_file_fsync` == 0** |

**诊断**: 每个 tag 只有一行 PASS/FAIL 时"红在哪一步"看不出来, 所以每个子判据在**失败分支**上补一行 `[FATCONF]   <tag> <step> got=<> want=<>`(`fat_diag`/`fat_diag_str`)—— 绿的路径日志不变长, 计数也不受影响。

**TC-FAT-001 的类型/簇数从哪来**: 上游的 `get_fs_info()`/`determine_legacy_fat_bits()` 是 **static**, 没有对外观测点 ⇒ glue 在挂载期用 **vendored `struct boot_sector` 布局**只读地解析一遍 BPB(`glue/fat_blk.c` 的 `uboot_fat_vol_probe`, 判定算法与上游同形)。它**只喂判据与日志, 不参与 I/O 决策**。若哪天观测与上游口径分叉, 表现是**挂载判据红**, 不会变成"读写错了"; 类型判定另有**间接**判据: TC-FAT-002..008 的读/写闭环在 12/16/32 三种表宽上都跑过, 表宽判错则读回必然不符。

自检**不删不改**宿主预置文件(tag 2/3 依赖它们); 自己的临时文件用 `tcN_` 前缀并在用例开头先 `unlink`(门禁镜像跨次运行是持久的)。

## 7. 前置工件(宿主必须在 QEMU 之前造好)

三块块设备上的 FAT 卷**不是**本插件格式化的(本插件只读写既有卷), 由构建后门禁 `fs-images` 用外部权威工具造(`mkfs.fat` + `mcopy`): `emmc0.img`(2 MiB FAT12)、`emmc1.img`(16 MiB FAT16)、`emmc2.img`(32 MiB FAT32), 内容:

```
/fat12: hello.txt = "fat12 hello\n";          dir/a.bin = 16 个 'A'
/fat16: hello.txt = "fat16 hello\n";          dir/a.bin = 256 个 'B'
/fat32: hello.txt = "fat32 hello\n";          dir/a.bin = 4096 个 'C';
        long name file.txt = "lfn\n"
```

这些形状作为**常量**写在 [glue/fat_glue.h](glue/fat_glue.h) 的 `UB_FAT_C*_*` 里(自检与 APP 共用一份真值); 宿主造出来的形状与它们不一致 ⇒ TC-FAT-002/003 红。

## 8. 依赖、构建与目录分工

| 边 | kind | 理由 |
|---|---|---|
| `framework/vfs-core` | init | 实现并注册 `br_fs_ops`/`br_inode_ops`/`br_file_ops` |
| `framework/bdev-core` | init | `br_bdev_get` 取三块块设备 |
| `adapter/uboot-mmc` | init | 三块块设备由它在自己的 start 相注册 |

```
adapter/uboot-fat/
├── brick.toml      sources = ../../third_party/uboot-imx/fs/fat/fat_write.c + glue/*.c
├── compat/          u-boot 运行环境(我们写的):
│     fat_env.h        环境实现头(类型/宏/字符串/日志/堆/块设备 shim/环境桩)
│     <头名>.h         薄壳: blk.h config.h exports.h log.h part.h malloc.h memalign.h
│                      rtc.h command.h div64.h rand.h dm/uclass.h linux/{ctype,log2,math64,
│                      kernel,compiler}.h asm/{byteorder,unaligned,cache}.h
│     fat.h fs.h rtc_def.h   **转发壳** → vendor 原件(盘上格式只有一处真值)
├── glue/            本项目对接: fat_blk.c(块层/卷表/BPB/环境实现)
│                     fat_vfs.c(VFS 四层 ops + 钩子)、fat_selftest.c(TC-FAT-*)
└── (src/ 这一层**已删除**)
```

`includes = ["compat", "glue"]`; **不导出 API**(没有 `[[export]]`), 消费面就是 VFS 契约本身。**不要**把 `third_party/uboot-imx/include` 加进 `includes`: brickie 的 `-I` 按字典序排, 那条会盖住 `compat/` 里的同名壳(`<fs.h>`/`<part.h>` 会被解析到 u-boot 的真头, 而那份还要 `<asm/byteorder.h>`/`<rtc_def.h>` 之类)。转发壳用**相对路径**, 没有这个歧义。

## 9. 欠债(如实登记)
> **WORKAROUND(br-wa-emmc-002)**: 本件的**写面端到端用例只在 FAT16 卷上做全**(FAT12/FAT32 只跑了读路径), 且 FAT32 的 FSInfo 空闲簇计数不维护 —— 逐条欠债与退出条件见 `WORKAROUNDS.md`。

1. **名字长度**: 长名 > 31 字节在 VFS 面上不可见(`readdir` ⇒ -ENOSPC)。退出条件 = `br_dirent_t.name[]` 加宽(或引入 LFN 影子索引), 属 VFS 契约变更。
2. **非空目录的 rmdir 返回 -EINVAL**(上游 `fat_unlink` 的口径)而不是 -ENOTEMPTY: 改成 -ENOTEMPTY 要动上游的返回码 ⇒ 与"不改上游"的纪律冲突; 由调用方决定要不要包一层。
3. **`assert()` 在本次编译里是空语句**: 两个 fat .c 都没有 `#define DEBUG`, 上游 `include/log.h` 的 `assert` 展开在 `_DEBUG == 0` 时本来就不做运行期断言(上游那几处 `assert(pos == cur_pos)`/`assert(!pos)` 是**上游自己的**死代码)。本件**没有**把它们改成"打印 + 返回错误" —— 那正是"改上游"。风险如实登记: 上游写路径在畸形盘上可能带着不满足的断言继续跑(它的错误返回路径仍然在)。
4. **写路径的簇缓冲**: 上游 `get_set_cluster()` 用**永不释放的静态缓冲**(`static u8 *tmpbuf_cluster`, 首次 `memalign(ARCH_DMA_MINALIGN, MAX_CLUSTSIZE)`)⇒ 最多 32 KiB 首次分配后常驻。`fsdata` 的 FAT 缓存(3 KiB)是**每次 API 调用**分配/释放, 于是 3 KiB 的 malloc/free 会随文件操作次数发生。
5. **`used_kib = 80`**: 峰值约 32 KiB(写路径簇缓冲)+ 3 KiB(FAT 缓存)+ 512 B(引导扇区, 栈上)+ 目录流/节点。它不是 manifest 预算生成的(与 tmpfs 的配额同源欠债 `br-wa-mem-001`)。
6. **单卷单线程假设**: `s_cur_vol`("当前卷")是**插件静态**, 与上游的 `cur_dev` 同型; 每个入口重新 bind。多线程消费者(v2)必须加锁或改成"每卷一份上下文" —— 与 `WORKAROUNDS.md` 的 `br-wa-boot-002`(自检 pass 的抢占窗口)同族。
7. **`fsync` 的粒度是"卷"**: 数据与目录项在每次写时已落盘, 所以文件级 `fsync` 与卷级 `sync` 都是 `bdev.flush()`; 一次 `fsync` 会把该卷上所有已写扇区一起落 —— 语义足够, 但不是 per-file 隔离。
8. **目录项顺序无契约**: `readdir` 的顺序 = FAT 目录槽的物理顺序, 不保证与创建顺序一致。
