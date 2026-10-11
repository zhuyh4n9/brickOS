# 0022 — 第三方 FAT / EROFS 移植 + 多卡 eMMC(emmc0..3)+ 外部工具造镜像

> 状态: **已落地**(P0~P6 全部实现并在 QEMU virt 上端到端验证: `[FATCONF] 8/0`、
> `[EROFCONF] 8/0`、`[EMMCCONF] 8/0`(四卡)、`[APPCONF]` 两族各 7/0(FS 族在 start 相、
> pthread 族在 APP 线程), `[SELFTEST] SUMMARY plugins=23 ran=18 skipped=6 fails=0 errors=0`;
> `brickie check` 0 错 0 警、`brickie test` 全绿(7 宿主用例 + 11 QEMU 门禁 + 5 脚本门禁))。
> 前置分析: `docs/analysis/0002-erofs-fat-gap.md`(上游文件清单/依赖映射/三处真实缺口/
> 工具与格式的实测取证)。
> 前置决策: `docs/decisions/0021-bdev-pci-emmc.md`(bdev-core、pci-core、三方隔离、
> 中断 + ADMA2/PIO)。本刀**沿用**它的 D-3 与 D-4, 不重开。
> 设计依据: `docs/7-storage/7-01-vfs.md` §1~§3(四层 ops / 瞬态 inode)、`7-03-concrete-fs.md`
> §1~§2(介质 FS 与 vfs-core 的绑定)、`1-03-roadmap.md` §1/§3(EROFS 的排期与角色)。

## 1. 背景

`7-03` §1 把 FS 分两类: **管理类**(tmpfs/devfs, 无介质)与**介质 FS**(按介质绑定
cdev-core/bdev-core)。前一刀(ADR-0021)把**块设备这一侧**打通了: `framework/bdev-core`
+ `framework/pci-core` + `third_party/uboot-mmc` 让 QEMU 里出现了一块真 eMMC。本刀做另一侧:
**把文件系统架到那块 eMMC 上**, 并在 APP 里经 POSIX 面端到端验证。

选 FAT 与 EROFS 这一对, 是因为它们把两个方向都覆盖了:

| | FAT | EROFS |
|---|---|---|
| 读写 | **读写**(建/写/截断/改名/删/建目录) | **只读** |
| 结构 | FAT12/16/32 三种根目录形态与表宽 | 元数据块 + 平铺/内联/压缩三种 datalayout |
| 数据通路 | 经块设备读写(缓存一致性、写回) | 压缩块要 LZ4 解压(纯读) |
| 设计侧状态 | `7-03` §1: **未排期** | `7-03` §1: v2.0 |

也就是说: 这一刀在设计侧**没有排期**, 是被显式要求提前落地的能力。因此它必须比"照排期
干活"多交代两件事 —— **与设计偏离的逐条账**(§3)与**不假装做完的部分**(§5)。

## 2. 决策

### D-1 两件 FS 都是 `api_type = third_party` 插件(不是选择, 是工具链的推论)

`tools/brickie/rust/src/rules.rs::api_type_edge_reason` 有一条硬禁则:
**`native` 插件不得依赖 `api_type = "third_party"` 的插件**。而本刀的两件 FS 都要满足:

* 它们要 include `framework/vfs-core`/`framework/bdev-core` 的头(→ `type`/`init` 边, 合法);
* 它们的**挂载必须发生在 `third_party/uboot-mmc` 的 start 相之后** —— 块设备是那块卡
  在自己的 start 相(全局开中断之后)注册的, 在那之前 `br_bdev_get("emmcN")` 只会得
  `BR_NULL`。这条顺序只能由 `[[dep]] third_party/uboot-mmc kind = "init"` 表达
  (插件管理器的拓扑序对同一相内的执行顺序生效, 见 `plugin_manager` 的 `s_order`)。

两条合在一起 ⇒ 两件 FS **只能是 third_party**。这不是"为了省事", 而是**代码来源与
依赖形状**同时指到同一个结论: 它们的源码本来就是 GPL-2.0+ 的 u-boot 代码
(`fs/fat/**`、`fs/erofs/**`), 按 ADR-0021 D-3 的隔离政策本来就该登记为三方件。

于是闭包里出现一个**自洽的第三方子图**:

```
platform/qemu-aarch64 ──► framework/pci-core ──► third_party/uboot-mmc   (块设备 emmc0..3)
                                                        ▲     ▲
                              third_party/uboot-fat ─────┘     │
                              third_party/uboot-erofs ────────┘
                                        │
                                        ▼
                     framework/vfs-core(挂载表/走查)+ framework/bdev-core(类 API)
```

WTFPL 的框架件与平台侧**一行 GPL 代码都不含**; 但静态链进同一镜像后整镜像仍是 GPL
组合作品 —— 这条残余风险在 ADR-0021 §5 已如实登记(`br-wa-tp-001`), 本刀把它**扩大**到
两件, 没有消除它(也不假装能消除)。

### D-2 四块卡, 而"4"是 INTx 线数决定的上界

一台 `sdhci-pci` 只挂一张卡(硬件如此), 而四件介质各需一块:

| 卡 | 容量 | 内容 | 挂载点 | 由谁造 |
|---|---|---|---|---|
| `emmc0` | 2 MiB | FAT12 卷 | `/fat12` | `mkfs.fat -F 12 -s 2` |
| `emmc1` | 16 MiB | FAT16 卷 | `/fat16` | `mkfs.fat -F 16 -s 1` |
| `emmc2` | 64 MiB | FAT32 卷 | `/fat32` | `mkfs.fat -F 32 -s 1` |
| `emmc3` | 16 MiB | EROFS 镜像(扇区 0 起, ≤ 1 MiB)+ 卡尾 512 KiB 擦写区 | `/assets` | `mkfs.erofs -zlz4` |

**为什么不是"想加就加"**: QEMU virt 的 GPEX 只把 **4** 根 INTx 线接到 GIC
(SPI 3..6 = `BR_IRQ_PCIE_INTX_A..D` = virq 7..10), 而 swizzle 是 `(pin-1+slot) % 4` ——
slot 3/4/5/6 恰好用满 A/B/C/D; 再加一块(slot 7)就会与 slot 3 **撞同一根线**。而 core 的
IRQ 框架规定 **INV-A: 每根物理线恰一属主**(`irq_core.c`, 无共享线)⇒ 第 5 块卡必然
`-EBUSY`。所以:

* `BR_EMMC_MAX_DEVS = 4` 是**平台事实的推论**, 不是拍脑袋的上界;
* 想再挂一块卡, 先要有**共享线**(Level IRQ 多属主)或 **MSI** —— 前者是 core 的契约
  变更, 后者要 sdhci 侧支持。两者都**不在本刀**。

**擦写区为什么在 `emmc3` 的卡尾**: 设备级用例(写特征模式再读回、ADMA2/PIO 通路切换)要
写**裸扇区**, 而 `emmc0/1/2` 上装着**活的 FAT 卷** —— 往那里写会撕掉文件系统(而这些
用例跑在 FS 用例**之后**的自检相位, 撕掉的正是别人刚用过的数据)。EROFS 卡是只读介质,
它的镜像只占卡首(实测 16384 B), 卡尾 512 KiB 天然空闲。于是:

* 写区起点 `BR_EMMC_SCRATCH_LBA = 4096`(2 MiB);
* **"镜像与写区永不重叠"是算出来的, 不是约定**: `tools/mk-fs-images.sh` 对镜像有一条
  硬断言(**> 1 MiB 直接失败**), 1 MiB < 2 MiB 是常量关系。

### D-3 inode = 瞬态**路径包装**; 复用上游的路径风格 API

设计 SD-3(`7-01` §2)说 inode 是走查的**瞬态**产物、由 `free_inode` 即弃。上游两件 FS 的
公开面都是**路径风格**的(`fat_exists(path)` / `fat_read_file(path, off, len, …)` /
`erofs_ilookup(path, &vi)` / `erofs_opendir(path)`)。两种选择:

* **A(选它)**: 瞬态 inode 里存"卷 id + 相对挂载点的路径", 胶水把
  `lookup(dir, name)` 翻译成 `dir_path + "/" + name` 再调上游。走查每次都从挂载根重走。
* **B(没选)**: 把上游的目录迭代器与路径解析**重写**成 inode/name 模型(上游的
  `fat_itr`/`erofs_namei` 都得拆开重组)。

选 A 的理由有三条, 逐条都有判据:

1. **这正是设计给介质 FS 的形状**。`fs/tmpfs/src/tmpfs.c` 的文件头把两种形状写得最清楚:
   "设计 SD-3 说 inode 是走查的中间产物、`free_inode` 即弃 —— 那是 littlefs 的形状
   (节点 = 路径前缀包装, 走查一次就得重建一次)"。FAT/EROFS 与 littlefs 同类。
2. **移植件的价值在于"认得出是上游代码"**。B 方案会把两个上游文件的核心循环打散,
   以后对齐上游版本只能靠人肉 diff。A 方案让 `fat_core.c`/`fat_write.c`/`erofs_*.c`
   与上游**逐函数对得上**(NOTICE 里的逐文件对照因此可核)。
3. **代价是可接受的、且已被设计登记**: 每次 lookup 一次 O(depth) 重走, 属 SD-3 的
   "v1 无 inode cache / dcache"上限(R-S3); 本刀的文件规模(3 个 FAT 卷 + 1 个只读镜像,
   单 APP)下它不可观测。

### D-4 镜像由**外部权威工具**造, 锁版本、不提交二进制

判据必须是"读出来的字节 == 别人放进去的字节"。如果 mkfs 与 reader 都是本项目自研的,
这句话就成了"自研件与自研件互相印证", 证明不了与**真实世界**的兼容性。所以:

* 造镜像用 `mkfs.fat`(dosfstools)+ `mcopy`/`mmd`(mtools)+ `mkfs.erofs`(erofs-utils);
* 三件**纳进同一张锁表** `prebuilts/toolchain.lock.toml`(版本 + URL + sha256), 由
  `tools/fetch-prebuilt.py` 取件 —— 与 make/ninja 同一条纪律(ADR-0002);
* 工具候选序与交叉工具链同款: `prebuilts/toolchain/` → PATH ⇒ **没有网络也能用宿主自带的
  同名工具**, 这条是"降级可用"而不是"另一份真值"(语义相同, 只换来源);
* **不把镜像提交进仓库**: 镜像内容随用例演进(改一个期望字节就要重造), 提交进去的字节
  无法重放; 且 FAT32 的最小合法卷是 34 MiB, 而 QEMU 又要求后备文件是 2 的幂 ⇒ 64 MiB。
  于是 `tools/mk-fs-images.sh` **每次重建**
  (不像旧的 `emmc-image` 那样幂等跳过)—— 卷里有文件系统的**活状态**, 上一轮写进去的
  东西必须消失, 否则 rw 用例的判据会被历史污染。

### D-5 只读 FS 的写面**填槽位并返回 `-EROFS`**, 而不是留空

`br_vfs.h` 的约定是"空槽位 ⇒ 调用方得 `-ENOTSUP`"。对 EROFS 来说那是**错的错误码**:
"这个文件系统是只读的"与"这个文件系统不支持这项操作"是两件事, 消费者(POSIX 面的
`open(O_WRONLY)`/`unlink`/`mkdir`)要能分辨。所以 EROFS 件把 `create`/`unlink`/`mkdir`/
`rmdir`/`rename`/`setattr`/`write`/`symlink`/`link` **全部填上**, 一律
`return BR_ERR(BR_EROFS)`。用例 `TC-EROFS-007` 与 `APP-FS-006` 逐条断言 errno == `EROFS`,
**不是**"返回了负数就算过"。

### D-6 名字上界: `BR_NAME_MAX = 32` 是 v1 的契约, FS 侧按它裁

`br_dirent_t.name[BR_NAME_MAX]` 是 vfs 的静态上界。而 FAT 的长名(LFN)与 EROFS 的
`EROFS_NAME_LEN = 255` 都比它宽。本刀的口径(**与 tmpfs 同名上界, 不新开一处**):

* 目录面上, 名字长于 31 字节的条目**跳过**(不截断 —— 截断会让两个不同文件看起来同名);
* `lookup` 上, 超长名字返回 `BR_ERR(BR_ENOSPC)`(与 tmpfs 对超长名的口径一致)。

代价如实登记: 宿主夹具里刻意放了一个 LFN 文件 `long name file.txt`(19 字节, 在界内)
覆盖"VFAT 长名解析"这条路径, 但**超过 31 字节的名字在本代取不到**。

### D-7 EROFS: 未压缩 + **LZ4**; 其它算法 `-ENOTSUP`

EROFS 的 `zmap.c`(压缩映射迭代)与 `decompress.c` 是移植里最重的一块。本刀的口径:

* **未压缩**(FLAT_INLINE / FLAT_PLAIN)与 **LZ4** 两条路都实现;
* deflate / zstd 分支**直接删掉**(上游是 `IS_ENABLED(CONFIG_*)` 条件编译), 命中即
  `-ENOTSUP` —— 不是"忘了", 是明示的边界;
* LZ4 解码器从上游 `lib/lz4.c` 移植(该文件 SPDX 是 `GPL 2.0+ OR BSD-2-Clause`,
  NOTICE 里如实标注)。

## 3. 与设计的偏离(逐条)

| 设计原文 | 本刀的做法 | 为什么 |
|---|---|---|
| `7-03` §1: EROFS **v2.0**; FAT **未排期** | 两件都提前落地 | 显式要求的刀; 因此排期账要在这里交代清楚, 而不是悄悄改 roadmap |
| `1-03` §170: `fs/erofs` 的 provider 是 `vfs-core + bdev-core` | 落地为 `third_party/uboot-erofs`, 依赖 `vfs-core` + `bdev-core` + `uboot-mmc` | D-1(源码许可 + 顺序边) |
| `7-03` 未给 FAT 任何条目 | 三卷分别挂 `/fat12` `/fat16` `/fat32` | 三个卷是**验证覆盖**的需要(FAT12/16/32 的根目录形态与表宽各不相同); 名字取"如实", 不假装它们是产品形态的 `/data` |
| `1-03` §156: EROFS 挂 `/assets` | 一致 | 沿用设计给的挂载点 |
| `7-01` §2: `br_fs_cfg.opts` 是 FS 自有配置 | 未用(`BR_NULL`) | 本刀不需要选项; 不为了"看起来完整"造一个空 opts 面 |
| `7-03` §6: 挂载顺序由 **manifest 生成** | 挂载点仍是**代码常量**(`uboot_fat_*_MOUNT`), `[[mount]]` 只是人读的声明面 | 继承 `br-wa-fs-001` 的既有欠债(组合器还不消费 `[[mount]]`); 本刀**没有**扩大它 —— 新增的两件照同一口径登记 |

## 4. 风险与遗留

| 风险 | 现状 | 退出条件 |
|---|---|---|
| GPL 组合作品 | 三方件从 1 件变 3 件; 目录级隔离清晰, 传染性未消除 | `br-wa-tp-001`(与 ADR-0021 同一条, 覆盖面扩大) |
| 每次 lookup 一次路径重走 | 本刀规模下不可观测; 属 SD-3 的已知上限(R-S3) | v1 之后的 inode cache(设计已在 SD-3 排期) |
| 写面缺 `truncate`/`FSInfo` 维护的完整性 | FAT 写面是上游 `fat_write.c` 的能力面; 本刀**自补了 truncate**(上游没有) | 见 §5: 自补件需要更多用例 |
| 名字 > 31 字节不可达 | D-6 的静态上界 | `BR_NAME_MAX` 放宽(会波及 vfs 与全部 FS 件, 不能单独改) |
| EROFS 只覆盖未压缩 + LZ4 | D-7 的明示边界 | 需要 deflate/zstd 时另起(上游对应解码器不在本刀范围) |
| 4 块卡是 INTx 线数的硬上界 | D-2 的推论 | 共享线(Level IRQ 多属主)或 MSI —— 都是 core/驱动契约变更 |

## 5. 不假装做完的部分

* **FAT12/16/32 的写面**: 三卷都做了**读**的端到端(TC-FAT-002/003), 写闭环
  (建/写/截断/改名/删/建目录)在 **FAT16** 卷上做全了(TC-FAT-004~008); FAT12/FAT32 的
  **读**路径有端到端, **写**路径没有独立用例。理由: 三者的写路径共用同一套
  `set_fatent_value`/`flush_dir`/`new_dir_table` 原语, 差异只在 FAT 表项宽度与根目录
  定位 —— 而根目录定位的差异由**读**用例覆盖到了。这条登记在 `br-wa-emmc-002`。
* **FAT 的 FSInfo 维护**(FAT32)与**坏簇标记**: 上游 `fat_write.c` 不维护 FSInfo 空闲计数。
  宿主用 `fsck.fat` 校验镜像, 但**没**在 guest 写完后再校验一次(需要把一个宿主工具搬进
  镜像里, 不在本刀范围)。
* **EROFS 的 xattr / symlink 面**: `erofs_fs.h` 与 `data.c` 的 xattr 读取没有移植
  (本刀不需要); symlink 的 `readlink` 视上游 `namei.c` 的能力面实现。若没实现, 该槽位
  留空 ⇒ `-ENOTSUP`, 与"设计未排期"一致。
* **共享中断线 / MSI**: 见 D-2。第 5 块卡需要它们。

## 6. 落地顺序

| 阶段 | 内容 | 判据 |
|---|---|---|
| P0 | 差异分析(上游清单/依赖/缺口/工具取证) | `docs/analysis/0002-erofs-fat-gap.md` |
| P1 | `uboot-mmc` 单例 → 静态实例表(多卡) | 同一镜像里 4 块 `emmc0..3` 都就绪; PIO 切换只影响被切的那块(TC-EMMC-006) |
| P2 | 宿主工具链: 锁表 + 取件 + `mk-fs-images.sh` | `ok   fs images`; `fsck.fat` 回读通过; EROFS 镜像 ≤ 1 MiB 断言 |
| P3 | `third_party/uboot-fat`(读 + 写 + vfs 对接) | `[FATCONF] 8/0` |
| P4 | `third_party/uboot-erofs`(未压缩 + LZ4 + vfs 对接) | `[EROFCONF] 8/0` |
| P5 | APP 侧经 POSIX 的端到端样例 | `[APPCONF]` 14/0(含 `APP-FS-001..007`) |
| P6 | 声明面 + 门禁 + 文档 | `brickie check` 0/0; `brickie test` 全绿 |

## 7. 落地记录与实测发现

全部已实现并验证。几条**只有跑起来才知道**的事实(都写进了对应源码/脚本的注释里):

1. **`erofs-utils` 对"不超过一个块"的文件不做压缩**。实测: 2 KiB 与 4 KiB 的可压文件在
   `-zlz4` 下仍是 layout 2(FLAT_INLINE)/0(FLAT_PLAIN); 只有 **> 4096 B** 的文件才走
   layout 3(压缩)。理由也清楚: 一个块是它无论如何都要占的下界, 压了也不省。
   ⇒ 夹具里的压缩件因此取 **70000 B**(且刻意不是块对齐, 覆盖尾部 tail extent)。
   这条直接决定了 `tools/mk-fs-images.sh` 里几个文件的尺寸。
2. **FAT32 的卷容量被三条边界夹住 ⇒ 最终是 64 MiB**:
   ① FAT32 的簇数下限 65525, 而把簇数顶上去的唯一办法是 512 B 簇(4 KiB 簇连下限都够不到)
   ⇒ `mkfs.fat -F 32 -s 1` 在 32 MiB 上只给 64496 个簇(实测告警), 34 MiB 才过线;
   ② **QEMU 的 `-device emmc` 要求后备文件大小是 2 的幂** —— 34 MiB 被直接拒载
   (`Invalid eMMC size: 34 MiB / eMMC size has to be a power of 2, e.g. 64 MiB`),
   这条是**跑起来才知道**的(第一版夹具就是 34 MiB, smoke 门禁第一次启动即红);
   ③ 于是 64 MiB 是同时满足 ①② 的最小 2 的幂。
   FAT12 反过来: 2 MiB 卷必须用 **1024 B 簇**(512 B 簇会给 4096 个簇, 越过 FAT12 的
   4084 上界)。★ 这一条同时说明"卡容量不是随便挑的": 它是格式下限、QEMU 约束与
   "别造大文件"三者的交点。
3. **`erofs-utils` 1.9.4(resolute)链接 `libcrypto.so.4`**, 而常见宿主机只有
   `libcrypto.so.3` ⇒ 一个"造镜像的工具"不该再牵出一串宿主库依赖, 故锁 **1.8.10**。
   同一类事实: `.deb` 的 `./usr/...` 会被 `strip = 1` 剥掉一层 ⇒ 锁表里的落点必须是
   `<into>/sbin/...` 与 `<into>/bin/...`(没有 `usr/`)。
4. **`prebuilts/toolchain/` 在全新 checkout 里不存在**, 而取件脚本的下载落点是它下面的
   `.cache/` —— 不先 mkdir 的话, `dest.open()` 每次抛 `FileNotFoundError`, 被
   "网络中断"分支吞掉并按 60 次 × 最多 15 s 重试(表现: 卡十分钟后报下载未完成)。
   已修(`tools/fetch-prebuilt.py::download` 的第一行)。
5. **INTx 的 4 根线决定了卡数上界**(D-2)。这不是理论: `pci-core` 的 swizzle
   `(pin-1+slot)%4` 让 slot 3 与 slot 7 落到同一根线, 而 IRQ 框架的 INV-A 会让第二次
   `br_irq_register` 得 `-EBUSY`。四块卡因此是"平台能挂满"的数, 不是随手取的数。
6. **★ `br_sched_run()` 不是"开始调度"的唯一入口 —— 这件事被本刀真的踩到了。**
   原来的形状是: APP 的 `start` 里 `pthread_create` 建线程, 然后 APP 线程跑一致性样例。
   但**timer 抢占在 `br_sched_run()` 之前就已经生效**(ADR-0011 的 IRQ 出口换栈只看
   "有没有就绪任务"), 于是 APP 线程与紧随其后的 `[SELFTEST]` pass **并发**。在
   "纯内存 + 纯 POSIX 同步原语"的旧样例里这无害(所以一直没被发现), 而本刀两边都要
   **访问同一批卷/同一块卡** ⇒ 实测三种症状同时出现:
     - EROFS 侧 `bdev read 失败 lba=2 rc=-5`(读到 EIO);
     - eMMC 侧 `sdhci wait 超时[信号量] (pending=0x0)`, TC-EMMC-007/008 各等满 2 s;
     - FAT 侧写闭环红。
   **处置**: 介质类 FS 的 APP 端到端样例**搬到 `start` 相**(`hello_start` 直接调用) ——
   此刻只有一条控制流, 四个卷都已挂好、中断已开。这不是"绕开问题", 而是把该有的
   相位次序说明白: **"自检 pass 与 APP 并发"这件事本身就是相位模型的一个缺口**
   (登记为 `br-wa-boot-002`)。
7. **`br_truncate()` 走的是"文件自己"的 iops->setattr**(vfs 先 `resolve_leaf` 再
   `ino->iops->setattr`)。EROFS 件因此必须给**文件**的 iops 表也填 `.setattr` ——
   第一版只给目录表填了, 于是 `br_truncate("/assets/hello.txt", 0)` 得到的是
   vfs 对空槽位的 `-ENOTSUP`, 而只读卷该给的是 `-EROFS`(TC-EROFS-007 因此判红)。
   这条是"空槽位语义"与"只读语义"撞车的具体例子: **空槽位不是万能的默认值**。
8. **★ FAT 卷的 `fsdata` "归卷长期持有"推翻了上游的一条隐含前提。**
   上游 u-boot 的 FAT 代码每调一次 API 就重建一份 `fsdata`(含 FAT 窗口缓存), "用完即弃";
   本移植为了性能把它**归卷长期持有**。这个改动带来三处真缺陷(都在跑起来之后才暴露):
     - `get_fs_info()` 直接释放 FAT 窗口 ⇒ **上一步留下的脏窗口被静默丢弃**
       (刚分配的簇链就没了);
     - `fat_itr_root()` 每个入口都重读 BPB/重建缓存 ⇒ 放大上一条;
     - 局部只读的 `fsdata` **副本**把 `fat_dirty` 一起抄走 ⇒ `get_fatent()` 会把
       "刚读进来、没人改过"的 6 扇区窗口**写回盘覆盖真 FAT**。
   ⇒ 教训: 把"每次新建的临时对象"变成"长期持有的共享对象"时, 必须逐条检查上游对它
   的**生命周期假设**(这里就是 `dirty` 标志的归属)。
9. **`mkfs.fat` + `mcopy` 造的目录项不置 `lcase` 小写位** ⇒ `readdir` 如实报 `HELLO.TXT`。
   FAT 的名字比较本来就是**大小写不敏感**的(所以按名走查一直是绿的), 但"readdir 报出来
   的字符串"是**盘上原始形状** —— 判据必须按大小写不敏感比较, 且 `name_len` 要用"报出来
   的名字"的长度(8.3 的名是 9 字节, 不是宿主源名的 12 字节)。这是"夹具是外部工具造的"
   带来的**真实形状差异**, 正是这一刀想要的那种证据。
10. **"某次 `read()` 的返回值"不等于"读满了"。** FAT 自检里有两处用自己的 64 B 栈缓冲
   **一次** `br_file_read()` 去读 100 B / 300 B 的文件, 于是永远判红 —— 而实现是对的
   (同一批操作在 APP 侧走 POSIX 完整读回是绿的)。这条与发现 8 是一类: 判据自身的形状
   要先对, 否则会去修根本没坏的东西。⇒ 两件移植件的自检最后都改成了"**分块读到 EOF + 再读
   一次必须为 0**"的形状。
11. **FAT 的文件级 `fsync` 必须存在**(不能留空槽位)。第一版按"卷级 sync 在 fs_ops,
    文件级 fsync 属 v2"把它留空 ⇒ POSIX `fsync(fd)` 得 `-ENOTSUP`。可写介质 FS 的
    `fsync` 是**能力面**而不是 v2 增量: 现在 `fs_ops.sync` 与 `file_ops.fsync` **共用**
    同一个 `uboot_fat_flush_volume()`(刷脏 FAT 窗口 + `bdev.flush()`), 两处入口一份实现。
    ★ 与发现 7 合起来是一条通则: **`br_*_ops` 的空槽位会被 vfs 翻译成 `-ENOTSUP`, 所以
    "这个 FS 该有什么能力"必须逐槽位过一遍, 不能靠"空着也没事"**。

## 8. 遗留(见 §4/§5)

`br-wa-tp-001`(三方许可, 覆盖面扩大)、`br-wa-emmc-002`(FAT12/32 写面缺独立用例)、
`br-wa-fs-001`(挂载点两处真值, 未扩大)、`br-wa-test-001`(用例编号与 `6-01` 未对齐,
本刀新增 `TC-FAT-*`/`TC-EROFS-*`/`APP-FS-*` 三族自编号, 已并入该条)、
`br-wa-boot-002`(**自检 pass 与 APP 线程可并发** —— 见 §7 发现 6)。

另两条**不是欠债、但要说清**的:

* `brickie.lock` 停在 ADR-0021 的状态(含 `[lock.product].version = "0.1.0.0"`)。这不是
  遗漏: 它由 `iface publish` 写, 而该命令的语义是"**面未变 ⇒ 空操作(不写盘、不推进版本)**"
  (contract §6.5)。本刀没有新增/变更任何接口单元(`TC-*` 用例不是接口面, 两件 FS 也不导出
  API)⇒ `brickie iface publish iface/posix --check` 报的正是"面未变 ⇒ 空操作"。
  于是版本这一轴的真值只有两处: `product.toml [product].version` 与
  `core/include/br/core/br_version.h`(本刀都改到 0.2.0)。
* 两件 FS 的**卷级并发**没有锁: vfs-core / fs/tmpfs / 两件移植件都按 v1 的"FS 调用串行化"
  约定, 与"设备 ops 由驱动自锁"是两件事(后者是 `8-01` §4 对**设备**的要求)。本刀的
  症状由 §7 发现 6 的相位处置消掉, 但"两个 APP 线程同时读同一卷"这条路径**没有**保护 ——
  v1 的单 APP 线程模型下不可达, 登记在 §4 的"每次 lookup 一次路径重走"同一类 v1 边界里。
