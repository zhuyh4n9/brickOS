# adapter/uboot-fat — u-boot FAT 移植(ability / fs / third_party)

> ⚠ **本插件是 GPL-2.0-or-later**, 来源与逐文件对照见 [NOTICE](NOTICE), 全文见
> [LICENSE.GPL-2.0](LICENSE.GPL-2.0)。它与 [uboot-mmc](../uboot-mmc/README.md)、
> [uboot-erofs](../uboot-erofs/README.md) 一起构成本仓库的 GPL 代码区(ADR-0021 D-3 /
> ADR-0022)。

## 1. 它是什么

把 u-boot 的 **FAT12/16/32 读写实现**移植进 brickOS, 出口是 **VFS 契约**
(`framework/vfs-core/include/br/vfs/br_vfs.h`), 挂三个卷:

```
                      ┌──────────────── framework/vfs-core(br_vfs.h)────────────────┐
                      │  br_open / br_stat / br_opendir / …(路径走查在 vfs-core)     │
                      └───────────────────────────┬────────────────────────────────┘
                                                  │ br_fs_ops / br_inode_ops / br_file_ops
                      ┌───────────────────────────▼────────────────────────────────┐
                      │ adapter/fat_vfs.c  瞬态路径包装 inode;目录 read br_dirent_t │
                      └───────────────────────────┬────────────────────────────────┘
                                                  │ 路径风格的上游 API(fat_read_file / …)
              ┌───────────────────────────────────┴─────────────────────────────────┐
              │ src/fat_core.c(读)                 src/fat_write.c(写 + truncate)  │
              │  BPB/簇链/目录迭代/只读文件面        长名/目录槽/簇分配/set_contents      │
              └───────────────────────────────────┬─────────────────────────────────┘
                                                  │ disk_read/disk_write(shim)
                      ┌───────────────────────────▼────────────────────────────────┐
                      │ framework/bdev-core  br_bdev_ops_t(read/write/flush/geometry)│
                      └───────────────────────────┬────────────────────────────────┘
                                                  │ emmc0 / emmc1 / emmc2
                      ┌───────────────────────────▼────────────────────────────────┐
                      │ adapter/uboot-mmc(它在自己的 start 相注册三块块设备)     │
                      └────────────────────────────────────────────────────────────┘
```

分层只隔两个接口: **VFS 契约**(往上)与 **br_bdev_ops_t**(往下)。上游戏移植代码认得出是
移植(函数名与函数体结构保留), 有意偏离处一律写 `/* 偏离: … */` 并在 NOTICE 登记。

### 三个卷

| bdev | 容量 | FAT 类型 | 挂载点 | 对应 (设计) |
|---|---|---|---|---|
| `emmc0` | 2 MiB | FAT12 | `/fat12` | `UB_FAT_VOL12` |
| `emmc1` | 16 MiB | FAT16 | `/fat16` | `UB_FAT_VOL16` |
| `emmc2` | 64 MiB | FAT32 | `/fat32` | `UB_FAT_VOL32` |

设备名/挂载点在 `src/fat_internal.h` 里是**编译期常量**, 且与 [plugin.toml](plugin.toml) 的
`[[mount]]` 逐字一致(两处真值由 `uboot_fat_mount_to_vol()` 在运行时对账: 挂载点对不上就拒挂)。

## 2. 生命周期

| 相 | 做什么 | 为什么 |
|---|---|---|
| `early_init` | 空手返回 | 堆还没建池(`br_mem_init` 在 core.init); early 相只允许"声明" |
| `init` | 只填挂载点表(s_slots); 空手返回 | 同上: 真正要分配的动作都在 start |
| `start` | 逐卷 `br_bdev_get` → 读 BPB → 校验卷几何 → `br_mount_register` | 块设备由 `adapter/uboot-mmc` 在**它的** start 相注册; `[[dep]] kind = "init"` 保证相位顺序 |

**挂载失败 ⇒ 立刻返回负 errno(首败即停机是刻意的)**: FAT 卷是产品的数据面, 缺一个卷还
继续跑只会让调用方在后面某个不相干的地方拿到 -ENOENT。

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

* "节点"没有任何持久身份 —— 每次走查都重建。于是胶水层直接把路径交给上游的路径风格 API
  (`fat_exists`/`fat_size`/`fat_read_file`/`file_fat_write_at`/`fat_unlink`/`fat_mkdir`/
  `fat_rename`), 不需要在介质上做 inode 编号 → 目录项的索引。
* **所有权**: `lookup`/`create` 交出的节点由调用方经 `free_inode` 释放(= `br_free`);
  唯一的例外是**挂载根** —— mount 用 `br_malloc` 造好、挂在 `uboot_fat_fs_t` 上长期持有,
  vfs 永不把它交给 `free_inode`(契约如此)。
* `size` 只是快照: `getattr`/`lseek(SEEK_END)`/`read` 每次都**问介质**(目录项的 size 是
  唯一真值), 免得"同一个文件被两个句柄打开"时读到脏长度。

## 4. 与 VFS 契约的对接口径

| 契约项 | 本插件的答复 |
|---|---|
| `iops->lookup` | 新建瞬态节点; 未命中 `BR_NULL`; "." / ".." 就地解析(根的父亲 = 根) |
| `iops->create` | 先 `file_fat_write_at(…, size = 0)` 把目录项建出来(之后每次写走"已存在"分支); `O_EXCL` 且已存在 ⇒ -EEXIST |
| `iops->unlink` | 目录 ⇒ -EISDIR(FAT 的 `fat_unlink` 只删空目录, 但那是 **rmdir** 的语义) |
| `iops->rmdir` | 非目录 ⇒ -ENOTDIR; 非空目录由上游返回 -EINVAL(见"欠债") |
| `iops->rename` | 跨卷 ⇒ -EXDEV; "目录搬进自己的子树"由上游的 `check_path_prefix` 拒(-EINVAL) |
| `iops->getattr` | 只置 `BR_STAT_SIZE`; `nlink` **不置位**(FAT 没有硬链接) |
| `iops->setattr` | 只认 `BR_STAT_SIZE` ⇒ `uboot_fat_truncate()`; 其他位 ⇒ -ENOTSUP |
| `iops->symlink/readlink/link` | 三个槽位显式 `BR_NULL` ⇒ -ENOTSUP(FAT 没有链接) |
| 文件 `read/write/lseek` | 带偏移语义(读: 越过 EOF ⇒ 0; 写: `pos > 现长` ⇒ -EINVAL, 上游"不许空洞") |
| 文件 `fsync` | **已实现**: 回写该卷的 dirty FAT 窗口 + `bdev.flush()`(与 `fs_ops.sync` **共用** `uboot_fat_flush_volume()`, 不是两份真值) |
| 文件 `ioctl/poll/poll_attach` | `BR_NULL`(契约: 缺失槽位 ⇒ -ENOTSUP) |
| 目录 `read` | 每次填**恰好一条** `br_dirent_t` 并返回 `sizeof(br_dirent_t)`; 穷尽返回 0; 头两条是 `.` / `..` |
| `fs_ops.sync` | 先 `flush_dirty_fat_buffer()`, 再 `bdev.flush()`(契约: flush **必须**落介质) |
| `fs_ops.unmount` | `BR_NULL`(v1 的签名先行槽位) |

**名字上界**: 本仓库的 `br_dirent_t.name[]` 只有 `BR_NAME_MAX`(32)字节, 而 FAT 的长名可到
255 字节 ⇒ 超过 31 字节的名字在 `readdir` 里返回 `BR_ERR(BR_ENOSPC)`(与 fs/tmpfs 同口径),
`create`/`mkdir` 里也拒绝。**代价**: 宿主预置的 `long name file.txt`(17 字节)可以正常列出,
但 > 31 字节的文件名在本 VFS 面上不可见 —— 见"欠债"。

## 5. 截断(本件新增, 上游没有)

上游 u-boot 只有"从 pos 写 size 字节", 没有 `fat_truncate`。本件的 `uboot_fat_truncate(path, size)`
**必须**用上游的同一套内部原语拼(`set_fatent_value`/`clear_fatent`/`flush_dirty_fat_buffer`/
`flush_dir`/`set_contents`), 否则"写"与"截断"就有了两套簇管理实现, 迟早互相踩:

* **缩小**: 走链到第 `size` 字节所在簇 → `clear_fatent()` 释放它之后的整条链 →
  把新的末尾簇标成 EOC → 目录项 `size = size` → 回写 FAT 与目录项;
  `size == 0` ⇒ 整条链释放 + 首簇清零。
* **扩大**: 分块(每次 ≤ 一个簇)调 `set_contents(dent, pos = 老长, buffer = 全零, len = 差值)`
  把新区域**显式写成 0**(不读回 0 就是信息泄漏 —— 新簇里可能是上一个文件的残留),
  再补写目录项 `size`(上游的 `set_contents` 不改目录项里的长度)。
* `size == 现长` ⇒ 空操作(照 POSIX); 目录 ⇒ -EISDIR。

判据在 `TC-FAT-008`: 1000 B → 300 B 缩短后**读回必须恰好 300 字节**(多一个字节就说明链没释放);
再 300 B → 600 B 扩大后 `[300, 600)` 必须全是 0。

## 5.1 跨操作一致性(实测抓到的三类坑, 全部已修)

这一节记录的是**只有跑起来才会暴露**的问题, 也是本插件对上游做的最重要的三处加固:

1. **上游 `set_contents()` 不覆盖"顺序追加"**: 它的"覆盖/追加"二分假设
   `pos < 链覆盖量`; 而消费者的**分段写**(写 8 B → 写 4 B → 写 9 B)在写满一个整簇后
   会走到 `pos == 链覆盖量`(长度恰好落在簇边界上)⇒ 上游报越界。
   本件改成"`ub_fat_at()` 定位 → 三分支: `cov > pos` 覆盖 / `cov == pos` 追加 /
   `cov < pos` 有洞(拒)", 并且每轮**维护不变量**"`cur` 是要写的簇、`start_off` 是它的起始
   偏移"。另外上游写完最后一簇后不标 EOC(文件尾仍指向已释放的簇) —— 一起修掉。
2. **上游 `get_cluster()` 的"尾部不足一扇区"路径会把整扇区读进调用方缓冲**:
   隐含前提是"调用方缓冲 ≥ 512 B"。消费者用 64 B 栈缓冲读 12 B 文件时就越界
   (实测 = 栈金丝雀被打爆)。本件改成"读进本函数的扇区缓冲再拷贝 `size` 字节"。
3. **卷级脏状态不许被静默丢掉**(本插件自己的结构):
   `fsdata` 挂在卷上长期有效, 于是把"重新识别"做成
   *①先 `flush_dirty_fat_buffer()` 再换缓存 ②已经有缓存就不再重读 BPB*;
   并且 `fat_dir_entries()`/`check_path_prefix()` 造的**只读局部副本**显式清掉
   `fat_dirty`(否则 `get_fatent()` 会把那份"刚读进来、没人改过"的 6 扇区窗口写回盘,
   覆盖真正的 FAT)。这三条合起来保证"APP 跑完一轮建/写/改名/删之后, 本插件的用例"
   与"同一序列里每一步"看到的是同一份盘上事实。

## 6. 一致性用例(`TC-FAT-*`, [adapter/fat_selftest.c](adapter/fat_selftest.c))

| id | 判据 |
|---|---|
| TC-FAT-001 | 三个挂载点在挂载表里; 每卷 BPB 判出的 FAT 类型(12/16/32)与**簇数区间**自洽, 设备名对得上 |
| TC-FAT-002 | 每卷读**宿主预置**的 `hello.txt` 并逐字节比对(`fat12 hello\n` / `fat16 hello\n` / `fat32 hello\n`) |
| TC-FAT-003 | 每卷 `readdir` 根: 头两条是 `.`/`..`; `hello.txt`(type/长度)与 `dir/`(目录)可见; `dir/a.bin` 的 size 与宿主一致 |
| TC-FAT-004 | `/fat16` 新建 → 三段各 512 B 写 → 关 → 重开读回 1536 B 逐字节一致 |
| TC-FAT-005 | `SEEK_END` 落在文件尾; EOF 读返回 0; `SEEK_SET(3)` 后读到第 4 字节; 负偏移 ⇒ 负 errno |
| TC-FAT-006 | `/fat32` `mkdir` + 在其中建 100 B 文件 + `stat` 的 type/size 与内容都对 |
| TC-FAT-007 | 改名后旧名 `stat` ⇒ -ENOENT, 新名内容逐字节一致 |
| TC-FAT-008 | `truncate` 缩短(读回恰好 300 B)与扩大(新区域全 0); `unlink` 后 ⇒ -ENOENT; 卷级 `sync == 0` **且文件级 `br_file_fsync` == 0**(两条入口同源) |

**诊断**: 每个 tag 只有一行 PASS/FAIL 时"红在哪一步"看不出来(实测踩过)。所以每个子判据在
**失败分支**上补一行 `[FATCONF]   <tag> <step> got=<> want=<>`(`fat_diag`/`fat_diag_str`)——
绿的路径日志不变长, 计数也不受影响。

自检**不删不改**宿主预置文件(tag 2/3 依赖它们); 自己的临时文件用 `tcN_` 前缀并在用例开头
先 `unlink`(门禁镜像跨次运行是持久的)。

## 7. 前置工件(宿主必须在 QEMU 之前造好)

三块块设备上的 FAT 卷**不是**本插件格式化的(本插件只读写既有卷), 由构建后门禁造:
`emmc0.img`(2 MiB, FAT12)、`emmc1.img`(16 MiB, FAT16)、`emmc2.img`(64 MiB, FAT32), 内容:

```
/fat12: hello.txt = "fat12 hello\n";          dir/a.bin = 16 个 'A'
/fat16: hello.txt = "fat16 hello\n";          dir/a.bin = 256 个 'B'
/fat32: hello.txt = "fat32 hello\n";          dir/a.bin = 4096 个 'C';
        long name file.txt = "lfn\n"
```

这些形状作为**常量**写在 [src/fat_internal.h](src/fat_internal.h) 的 `UB_FAT_C*_*` 里
(自检与 APP 共用一份真值)。宿主造出来的形状与它们不一致 ⇒ TC-FAT-002/003 红 —— 这是判据,
不是耦合。**未完成事项**: 本刀只交付插件目录, 造镜像脚本与 `tests/gates.toml` 的
`fat-test` 门禁条目由调用方接管(与 uboot-mmc/uboot-erofs 的分工一致)。

## 8. 依赖与构建

| 边 | kind | 理由 |
|---|---|---|
| `framework/vfs-core` | init | 实现并注册 `br_fs_ops`/`br_inode_ops`/`br_file_ops` |
| `framework/bdev-core` | init | `br_bdev_get` 取三块块设备 |
| `adapter/uboot-mmc` | init | 三块块设备(emmc0/1/2)由它在自己的 start 相注册 |

`src/*.c` 两件(上游派生): `fat_core.c` / `fat_write.c`; `adapter/*.c` 两件(本项目):
`fat_vfs.c` / `fat_selftest.c`; 私有头两件: `adapter/fat_compat.h`(u-boot/Linux 式类型与
工具宏 + 字符串小工具)、`src/fat_internal.h`(**唯一**的跨文件声明处)。**不导出 API**
(没有 `[[export]]`; `[build].includes = ["src", "adapter"]` 只给**同一插件内**两个目录
互相找头用, 不抛对外头)—— 它的消费面就是 VFS 契约本身。

## 8.1 `src/` 与 `adapter/` 的拆分(单向依赖)

GPL 边界靠**依赖方向**而非目录名维持:

- `src/`(**上游派生**, GPL-2.0+): `fat_core.c` / `fat_write.c` / `fat_internal.h`
  —— 逐字对应 u-boot `fs/fat/**`, 不含任何 `#include <br/...>`;
- `adapter/`(**本项目**): `fat_compat.h`(u-boot/Linux 式运行环境的**唯一**入口)、
  `fat_vfs.c`(四层 VFS ops 对接)、`fat_selftest.c`(TC-FAT-*)。

依赖**单向** `src/` → `adapter/`: `src/` 只经 `fat_compat.h` 取运行环境; `adapter/` 反过来
include `src/fat_internal.h` 是允许的(适配层当然要认识被适配的代码)。
[plugin.toml](plugin.toml) 据此收 `sources = ["src/*.c", "adapter/*.c"]` /
`includes = ["src", "adapter"]`, 门禁 `tools/check-third-party-isolation.sh`
(见 `tests/gates.toml`)机械地守住这条边界。完整理由与逐文件归属见
[`adapter/README.md`](adapter/README.md)。

## 9. 欠债(如实登记)
> **WORKAROUND(br-wa-emmc-002)**: 本件的**写面端到端用例只在 FAT16 卷上做全**(FAT12/FAT32 只跑了读路径), 且 FAT32 的 FSInfo 空闲簇计数不维护 —— 逐条欠债与退出条件见 `WORKAROUNDS.md`。


1. **名字长度**: 长名 > 31 字节在 VFS 面上不可见(`readdir` ⇒ -ENOSPC)。退出条件 =
   `br_dirent_t.name[]` 加宽(或引入 LFN 影子索引), 属 VFS 契约变更。
2. **非空目录的 rmdir 返回 -EINVAL**(上游 `fat_unlink` 的口径)而不是 -ENOTEMPTY:
   上游把它当成"参数错"。本刀**没有**改成 -ENOTEMPTY(那要动上游的返回码, 会与
   "保留上游函数体结构"的纪律冲突); 登记在此, 由调用方决定要不要包一层翻译。
3. **栈用量**: 写路径的 `get_set_cluster()` 在**栈上**放一个簇大小的缓冲(≤ 32 KiB),
   `set_sectors()` 的尾段用 512 B。上游用的是"永不释放的静态缓冲", 本件换成栈是为了让
   静态占用可算。⇒ **本插件的 ops 必须在栈足够大的线程上跑**(v1 的 APP 线程栈量级是 4 KiB,
   所以这是一条真实约束: 若将来把 FAT 写路径挂到小栈线程上, 必须改成堆缓冲)。
4. **`used_kib = 80`**: 运行期峰值约 33 KiB(迭代器的簇缓冲)+ 32 KiB(写路径的簇中转)
   + 3 KiB(FAT 缓存)+ 若干目录流/节点。它不是 manifest 预算生成的(与 tmpfs 的配额
   同源欠债: `br-wa-mem-001`)。
5. **单卷单线程假设**: `s_cur_vol`("当前卷")是**插件静态**, 与上游的 `cur_dev` 同型。
   本件在每次 API 调用前重新 `uboot_fat_vol_bind()`(它只写指针 + 读引导扇区), 但
   "同时操作两个卷"在 v1 的单 APP 线程模型下不会发生; 多线程消费者(v2)必须加锁或改成
   "每卷一份上下文"。
6. **`fsync` 的粒度是"卷"**: FAT 没有 per-file 脏账(数据与目录项在每次写时已落盘, 留在
   内存里的只有 6 扇区 FAT 窗口)⇒ 文件级 `fsync` 与卷级 `sync` 是同一件事。这在语义上
   足够(fsync 的承诺是"我写的数据在介质上"), 但**不是** per-file 隔离: 一次 `fsync` 会把
   该卷上所有挂起的 FAT 更新一起落盘。登记为如实口径。
7. **目录项顺序无契约**: `readdir` 的顺序 = FAT 目录槽的物理顺序, 不保证与创建顺序一致
   (FAT 用"第一个够长的空闲槽段", 于是删除过的目录项会被复用)。
