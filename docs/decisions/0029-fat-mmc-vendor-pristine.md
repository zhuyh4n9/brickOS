# 0029 — 三方件迁移(2/3): FAT 与 MMC 的**上游原样编译** + "不许夹带副本"的机械判据

> ★ **布局已被 [ADR-0033](0033-dissolve-adapter-namespace.md) 取代**: `adapter/uboot-{fat,mmc}` →
> `fs/uboot-fat` / `driver/uboot-mmc`; "不许夹带副本"的判据同时从 `adapter/**` 扩到**全树**
> (`tools/check-vendor-copies.py`)。下文已就地更新为新路径。

> 状态: **已落地**。
> 推进: [ADR-0026](0026-third-party-vendor-and-adapter.md) §5 的两条欠债
> (「`uboot-fat` 迁到新架构 = 待做」「`uboot-mmc` 迁到新架构 = 难/待专门决策」)——
> 两条都已结清: 三件三方插件(erofs/fat/mmc)现在**都**是"上游原件直接参与编译 + 适配在
> `adapter/<插件>/{compat,glue}`", `src/` 层在三件里都已删除。
> 前置: ADR-0021(三方隔离/bdev/中断)、ADR-0022(两件 FS 为何是三方件)、
> ADR-0024(隔离判据)、ADR-0026(vendor + adapter 三角)、ADR-0027(errno 名字归 core)。
> 编号说明: `0028` 已被在飞的一刀(`iface/posix` 与 POSIX 一致性门禁)占用。

## 1. 背景: 迁移只做了 1/3

ADR-0026 定了新架构并落地了 erofs(1/3): `brick.toml` 的 `sources` **直接编译**
`third_party/uboot-imx/fs/erofs/**` 的上游原件, 适配收进 `fs/uboot-erofs/{compat,glue}`。
fat 与 mmc 当时仍是旧形态 —— 插件目录里躺着 `src/fat_core.c`(与上游 `fs/fat/fat.c`
行级相似度 **0.58**)、`src/fat_write.c`(0.60)、`src/mmc_core.c` / `src/sdhci_host.c`
(改写副本)。ADR-0026 §5 把 mmc 记为**难**:

> 上游 `drivers/mmc/{mmc.c,sdhci.c}` 是 u-boot DM 形态且以 `udelay()` **轮询**完成传输,
> 还牵连 SD/UHS/HS200/HS400/tuning/RPMB/分区/`blk_desc` 整族… 原样编译 = 要么把那一族
> 都 vendor 进来, 要么放弃本项目的中断驱动 + 静态实例表(ADR-0021/0022 的设计)。

本刀把这两件做完, 并回答"mmc 到底能不能既原样编译上游、又保住中断驱动"。

## 2. `adapter/` 里"不许夹带三方件源码副本"从纪律变成判据

ADR-0026 的判据只覆盖了两侧: vendor 侧(`check-third-party-pristine.sh` 的 sha256 锁)与
"适配 = 给环境"(D-3 的文字)。但**"把上游抄一份改名再改缩进"在文件系统上与"适配"长得一样**
—— 旧 `src/fat_core.c` 就是这么来的。于是本刀补一条机械判据:

`tools/check-vendor-copies.py` —— 去空行/去纯注释行/去 `#` 预处理行后, 对
`adapter/**` 的每个 `.c/.h` 与 `third_party/**` 的每个 `.c/.h` 算
`difflib.SequenceMatcher` 行级相似度; **相似度 ≥ 阈值(0.30)且双方归一化行数都 ≥ 30 ⇒ 红**,
除非在脚本里**具名豁免**(每条要写理由)。

- **正例(迁移前实测)**: `fs/uboot-fat/src/fat_core.c ~= fs/fat/fat.c = 0.58`、
  `src/fat_write.c ~= fs/fat/fat_write.c = 0.60` ⇒ 报红 —— 判据确实抓得住"副本"。
- **负例(迁移后)**: 全树最高相似度 = 0.xx(adapter 侧只剩 `compat/` 的壳与 `glue/` 的对接)
  ⇒ 全过。

它在 `tests/gates.toml` 里注册为脚本门禁 `check-vendor-copies`。

## 3. fat 的落地

### 3.1 参与编译的上游原件

| vendor 文件 | 怎么参与 | 为什么 |
|---|---|---|
| `fs/fat/fat_write.c` | **唯一的 `.c`**(`sources` 直接点名) | 它第 23 行 `#include "fat.c"` ⇒ 读+写在**同一个编译单元**(上游的设计); 再单独编 `fat.c` 会**重复符号** |
| `fs/fat/fat.c` | 经上面那条相对 include 进同一单元 | 不单独编 |
| `include/fat.h` | 经 `compat/fat.h` 的**相对路径转发壳**引用 | 盘上格式(`boot_sector`/`dir_entry`/`fsdata`/属性位)的**唯一真值** —— 上一版把这些结构**照抄**进插件, 正是本 ADR 要消灭的 |
| `include/fs.h` | 经 `compat/fs.h` 转发 | `struct fs_dirent`/`fs_dir_stream` 的上游形状(上一版抄进来还**裁掉了** rtc 字段) |
| `include/rtc_def.h` | **本刀新引入的原件**(已补锁) | vendor `include/fs.h` 自己 include 它(`struct rtc_time` 是 `fs_dirent` 的字段类型) |

### 3.2 适配层提供了什么

- `compat/fat_env.h`: 运行环境(类型/`__leN`/小端判据/DIV_ROUND_UP/`min_t`/字符串自研/
  `malloc`→`br_malloc`/`pr_*`→`br_log_*`/errno 名字归 core), 外加 `struct blk_desc` +
  `disk_partition` + `blk_dread/blk_dwrite` 的**极小 shim**(整设备一个分区: 起 0、扇区数 =
  几何), 以及 `part_get_info`/`dm_rtc_get` 之类的空实现;
- `compat/<u-boot 头名>.h` × 18: 每个上游 include 名一个薄壳;
- `compat/{fat,fs,rtc_def}.h` × 3: **转发壳**(相对路径指向 vendor 原件, 不抄内容)。
  为什么用相对路径而不是 `-I third_party/uboot-imx/include`: brickie 的 `-I` **按字典序**排,
  `../..` 会**盖住** `compat/` 的同名壳(`<fs.h>`/`<part.h>` 被解析到 u-boot 真头, 而那份还要
  `rtc_def.h`/`asm/byteorder.h` 之类)
- `glue/fat_blk.c`: 块层 shim 的**实现**(`blk_dread/dwrite` → `br_bdev_ops_t`, 按
  `geometry.max_rw` 切块、越界拒绝)+ 卷表 + **BPB 观测**(挂载期判 FAT 类型/簇数, 供
  TC-FAT-001 用 vendor 的 `boot_sector` 布局解析);
- `glue/fat_vfs.c`: VFS 四层 ops + 生命周期 + `uboot_fat_vol_claim()`;
- `glue/fat_glue.h`: 适配层内部契约; `glue/fat_selftest.c`: TC-FAT-001..008。

### 3.3 两处"上一版有、这一版没有"的东西(都是收窄)

| 上一版(`src/`) | 这一版 | 为什么 |
|---|---|---|
| 160 行的 `uboot_fat_truncate()` 改写 | **删除**, 改为一行调上游公开 API: `file_fat_write_at(path, newsize, NULL, 0, &actwritten)` | 已读上游源码核实: `file_fat_write_at` 先改目录项 `size`, `set_contents()` 在 `filesize <= cur_pos` 时把尾部簇链 `clear_fatent()` 并给末簇打 EOC; `filesize == 0` 有专门分支清链并把首簇置 0; `get_set_cluster(size == 0)` 立即返回 ⇒ NULL buffer 不会被解引用。TC-FAT-008 的"变短/变 0/再变长"三条路径由用例真实覆盖 |
| `fsdata` **归卷**(跨调用保留 FAT 缓存) | **撤销**: 上游每个公开 API 自带一份 `fsdata`, glue 只调公开 API | 上一版那条"每个入口记得刷新 `total_sector`"的不变量因此**不存在了**(少一个静默越界判定的坑) |
| `assert()` → "打印 + 返回 -EIO" | 给成**空语句**(= 上游在未定义 `DEBUG` 时的行为) | 上游两个 `.c` 都没 `#define DEBUG` ⇒ `assert` 本来就是空; 自造一个不同语义的 `assert` 反而偏离上游 |

### 3.4 证据(fat)

| 项 | 结果 |
|---|---|
| `tools/check-third-party-pristine.sh` | **PASS** —— vendor 27 个文件逐字节 = 锁; 无多余文件; 无插件; 头只经 adapter 转发壳 |
| `tools/check-vendor-copies.py` | **PASS** —— 全树最高相似度 **0.16**(阈值 0.30); 迁移**前** `src/fat_core.c`=**0.58** / `src/fat_write.c`=**0.60** ⇒ 判据确实抓得住副本 |
| `-fsyntax-only` 直编 `third_party/uboot-imx/fs/fat/fat_write.c` | rc=0(带 `compat/` 的 `-I`; vendor 一个字没改) |
| `build/obj/third_party/uboot-imx/fs/fat/fat_write.o` | 在场 ⇒ **编的是 vendor 原件** |
| 镜像符号 | `file_fat_write_at` / `fat_set_blk_dev` / `fat_readdir` / `uboot_fat_truncate` 都在 |
| `brickie test fat-test` | **PASS**: `[FATCONF] SUMMARY pass=8 fail=0 total=8`, 8 条 tag 全点名, 无 `[FATCONF] FAIL`/`[PANIC]` |

> ★ **挂载权的变化(ADR-0030, 与本刀同期落地)**: 本刀开工时 fat 插件是**自己**
> `br_mount_register("/fat12", …)`; 落地时 vfs-core 已引入**驱动注册表 + 挂载计划** ——
> FS 插件只 `br_fs_driver_register("fat", &s_fs_ops, NULL)` 并用 `uboot_fat_vol_claim(dev, mnt)`
> 认领卷, 三卷的 `mount("emmc0", "/fat12", "fat", …)` 由 **APP 按 `[[mount]]` 计划**执行。
> 这与本刀无关但**必须记清**: `fat-test` 的 `vfs: mount /fat12 (dev=emmc0 …)` 三行现在来自
> APP 侧的计划, 而不是插件自己的 start 相。


## 4. mmc 的落地

(待回填: 配置矩阵、`mmc.h`/`sdhci.h` 转发 + `blk.h`/`part.h` 接口壳的理由、
`get_timer()/udelay()` 的中断化等待、ISR 掩 `SIGNAL_ENABLE` 的理由、实例表与 bdev 绑定、
证据与门禁结果。)

## 5. 与 ADR-0021/0022 的关系

(待回填。)

## 4. mmc 的落地

### 4.1 参与编译的上游原件(5 个 `.c`, 全部逐字节)

| vendor 文件 | 作用 |
|---|---|
| `drivers/mmc/mmc.c` | 协议核心(初始化/CMD/ACMD/CID/CSD/EXT_CSD/块读写) |
| `drivers/mmc/mmc_write.c` | 写/擦路径(`mmc_bwrite`/`mmc_berase`) |
| `drivers/mmc/mmc_legacy.c` | **非 DM 形态**的设备链与块设备挂钩(`mmc_create`/`find_mmc_device`/`mmc_get_blk_desc`/`mmc_list_*`)。本刀新增 vendor |
| `drivers/mmc/sdhci.c` | SDHCI 主机(寄存器/命令/ADMA2/PIO) |
| `drivers/mmc/sdhci-adma.c` | ADMA2 描述符表构造 |
| 头: `include/mmc.h`、`include/sdhci.h`、`include/{memalign.h,dm/uclass-id.h,linux/{kconfig,list,poison}.h}` | 经 `compat/` 的**相对路径转发壳**引用; 后 5 件是本刀新增 vendor |

### 4.2 为什么"难"变成了"可做":两个前提都被证伪

1. **不必搬 DM/uclass**: u-boot 的 MMC 栈保留着**非 DM(`!DM_MMC`)形态** ——
   `struct mmc_config` 里有 `const struct mmc_ops *ops`, `sdhci.c` 在 `!DM_MMC` 时给出
   `static const struct mmc_ops sdhci_ops` 并提供 `add_sdhci()`; 设备链在 `mmc_legacy.c`
   (非 TINY 分支用 `calloc` + `list_for_each`, `bdevnum` 依次 0/1/2/3 ⇒ 正好 `emmc0..3`)。
2. **"中断驱动"与"上游 `udelay()` 轮询"不是二选一**: 上游那两条等待循环**只**通过
   `get_timer()` / `udelay()` 让出 CPU。于是**环境层**把这两个实现成
   "**让出 CPU 到下一 ms 或当前设备的设备中断**", 上游的轮询循环就自动变成
   "睡下 → INTx → ISR → bh 唤醒 → 再读一次 `INT_STATUS`"的中断驱动等待 —— **上游零改动**。
   这是本刀最关键的适配手法(与"给上游它依赖的环境, 而不是改上游"完全同构)。

### 4.3 适配层提供了什么

- `compat/generated/autoconf.h` = **配置矩阵**(u-boot 真实构建里那个文件的角色);
  `compat/mmc_env.h` = 环境实现(类型/宏/`#ifdef` 陷阱/日志阈值/时间/MMIO/cache/DMA/list…);
  `compat/<头名>.h` × 28 薄壳 + 7 个 vendor 转发壳 + `blk.h`/`part.h`/`cyclic.h` 三个
  **接口类型壳**(真件会拖进 `<efi.h>`/`<bouncebuf.h>`/`<u-boot/uuid.h>` 整族, 而本件只要
  一个结构 + 一个宏 + 两个原型);
- `glue/emmc_bdev.c`: PCIe 绑定 + 4 实例静态表 + IRQ(ISR/bh) + bdev ops + ioctl + 等待原语;
- `glue/emmc_internal.h`(内部契约)、`glue/emmc_selftest.c`(TC-EMMC-001..008, 判据未动)。

### 4.4 三个"配置矩阵陷阱"(实测踩到, 逐条记下来)

| 陷阱 | 事实 | 处置 |
|---|---|---|
| `#ifdef` 认"定义没定义", 不认值 | 把要**关闭**的开关写成 `CONFIG_DM_MMC 0` 会让 `#ifdef CONFIG_DM_MMC` 变**真** | 关闭项**一律不定义**; `mmc_env.h` 用 `#error` 把 `DM_MMC`/`BLK` 钉死为"必须未定义" |
| `log_*` 与 `pr_*` 是两套口径 | 上游 `printk.h` 的 `pr_debug` 是**编译期**按 `CONFIG_LOGLEVEL` 裁剪的, 而 `mmc_mode_name()` 的**定义**也被同一阈值裁掉、它的调用点全在 `pr_debug` 的实参里 ⇒ 无条件转发成 `br_log_debug` 会 `undefined reference to mmc_mode_name` | 环境层按上游 printk.h **逐阈值**给出常量三元式 |
| 配置项落在**头名意外**的文件里 | `LOG2` 的唯一定义处是 `part.h`(不是 `linux/log2.h`) | 由编译器驱动补壳, 不靠猜 |

### 4.5 中断口径(与 ADR-0021 D-4 的对应)

QEMU 的 SDHCI 是**电平**中断: 断言条件 = `INT_STATUS & SIGNAL_ENABLE`。于是:

- **ISR**: 只读 `INT_STATUS`(**不清** —— 上游的等待循环必须能读到它), 把
  `SIGNAL_ENABLE` 写 0 **去断言线**, `br_work_submit()` 交下半部;
- **bh**: 只做 `br_sem_give()`(bh 里不许有任何线程面调用);
- **等待原语**(环境层): 睡下**之前**重开 `SIGNAL_ENABLE` ⇒ 每一轮都能被下一次中断唤醒。

⇒ 可判据的不变量不是"`SIGNAL_ENABLE == 0`", 而是"**没有人在等 ⇒ INTx 线未断言**";
每块卡打印 `idle-signal=flat`(`(INT_STATUS & SIGNAL_ENABLE) == 0`)。

### 4.6 四个"我自己适配层里的 bug"(上游一行未改, 逐一留证)

| # | 症状 | 根因 |
|---|---|---|
| 1 | `emmc0 mmc_init 失败 rc=-70`; `adma_err=0x1`; 描述符 `len=0x200 addr_lo=0x0` | `compat/mmc_env.h` 把 `lower_32_bits` 抄成了 `upper_32_bits` 的展开式 ⇒ 每个 ADMA2 描述符的数据地址 = 0 |
| 2 | 链接期 `undefined reference to mmc_mode_name` | `pr_*` 没按 `CONFIG_LOGLEVEL` 做编译期裁剪(见 §4.4) |
| 3 | 整轮 IRQCONF 自检期间 `[PANIC] br_task_exit(): 不许在中断下半部(bh)里结束线程` | bring-up 结束时**无条件**打开 `SIGNAL_ENABLE`, 而 `INT_STATUS` 里常压着"读到未清"的位 ⇒ 电平线在整个非传输窗口持续投递, 与 IRQCONF 在 bh 里的 thread-only 执法用例交叉 |
| 4 | 日志读起来像"等待完全靠 1 ms 兜底"(`wake irq/timeout=0/N`) | **记账 bug**: `br_sem_take` 在"令牌已在"时立即返回(ISR/bh 上一轮就 give 过), 被误归成 timeout。按**实际耗时**拆成 `irq/token/timeout` 三类后: `A+S` ≈ 98.6%, 真兜底只有 16 次 |

### 4.7 证据(mmc)

| 项 | 结果 |
|---|---|
| `tools/check-third-party-pristine.sh` | **PASS**(32 个 vendor 文件逐字节 = 锁) |
| `tools/check-vendor-copies.py` | **PASS**(uboot-mmc 无 findings; 全树最高 ≈0.15) |
| `brickie build` | 全绿(修复"两件三方插件都定义 `blk_dread/blk_dwrite`"的**重复符号**之后: 定义归**上游真正调用它的那一件** —— `fs/uboot-fat`; mmc 的上游不调用, 定义在 mmc 是死代码) |
| `build/obj/third_party/uboot-imx/drivers/mmc/` | `mmc.o` `mmc_legacy.o` `mmc_write.o` `sdhci-adma.o` `sdhci.o` ⇒ **编的是 vendor 原件** |
| 端到端门禁 | `emmc-test` / `fat-test` / `erofs-test` **三件都 PASS**(`brickie test` 全量跑) |
| ★ 一处**真实回归**(如实记账, 见 §6) | 迁移把"启动→自检结束"的墙钟推后了 ~0.6 s(基线 5.04 s / 迁移后 5.61~5.89 s), 而若干门禁的 `timeout_s = 6` 且判据是日志里要有 `[SELFTEST] SUMMARY` ⇒ **踩在边界上**: `irq-test` 单跑从"基线 2/5 红"变成"5/5 红"; `--jobs 4` 全量跑时基线红 2 关(`irq-test`/`emmc-test`)、迁移后红 3 关(多一个 `plugin-test`)。逐条数字与根因见 §6 的同名欠债行 |
| `brickie test emmc-test` | **PASS**: 8 条 TC 全 PASS、`[EMMCCONF] SUMMARY pass=8 fail=0`、四端点 `1b36:0007`、`eMMC ready`、`/dev/emmc0`+`/dev/emmc3 就绪`、`共接管 4 块 sdhci-pci 卡`、无 `[PANIC]` |
| 中断驱动的可观测证据 | 每卡 `IRQ count=… (bh=…, dropped=0, wake irq/token/timeout=…)`, `idle-signal=flat`; 用例级 `IRQ[005/ADMA2-8sec] ISR +23 … wake irq/token/timeout +16/+6/+0` 等 |
| 门禁级判据 ⑤ | `grep -rnE '^\s*#\s*include\s*[<"]third_party/'` = **0 命中**(门禁 PASS); 相对路径**转发壳**命中 11 处, 那正是本 ADR 规定的写法 |

## 5. 与 ADR-0021/0022 的关系

- **ADR-0021 D-3(三方隔离)**: 方向不变, 判据更强(哈希锁 + "无副本"判据);
- **ADR-0021 D-4(中断驱动 + ADMA2/PIO 可观测)**: 语义不变, 但**实现手法换了** ——
  不再是"改写上游的等待循环", 而是"环境层让 `get_timer()/udelay()` 可被设备中断唤醒";
  `SET_PATH` 仍只切 `host->flags` 的 `USE_ADMA` 位(那字段就是接口);
- **ADR-0021 D-5 / `br-wa-emmc-001`(flush)**: 不变, 标记已从 `src/mmc_core.c` 搬到
  `glue/emmc_bdev.c::emmc_flush()`;
- **ADR-0022 D-1(两件 FS 为何只能是 third_party)**: 不变(工具链的 `native → third_party` 禁则);
- **ADR-0022 的多卡静态实例表**: 协议核心的实例由上游 `mmc_create()` 分配(每卡一份
  `struct mmc`), 我们的 glue 只持有"每卡一份 {bdev 几何, SDHCI 注册窗口, 信号量, 中断计数,
  当前通路}"; `[[res]]` 预算仍是**编译期上界**(`BR_EMMC_MAX_DEVS = 4`, 受 4 根 INTx 约束)。

## 6. 未做与欠债

| 项 | 说明 |
|---|---|
| `U_BOOT_LEGACY_BLK(mmc)` | 降级为普通全局对象(本件没有链接段枚举); 接口面由 glue 直接兑现。**不改上游**的代价是这一处语义弱化, 记录在 `ADAPTER-NOTES.md` |
| `part_init()` / `blk_dselect_hwpart()` | 显式空实现/直通(本件不做软件分区, v1 暴露裸设备; 分区映射是 SD-9/v1.x) |
| 冗余 INTx 投递 | ISR 不能清 `INT_STATUS`(上游要读)、等待前又要重开 `SIGNAL_ENABLE` ⇒ 同一条命令内"读到未清"期间会**重复断言-投递**。这些是**真中断**, 只是冗余; 根因是这组硬约束的必然结果, 已登记 |
| 5 个 errno 名字寄在插件里 | `ECOMM(70)`/`EBADMSG(74)`/`ENOMEDIUM(123)`/`EMEDIUMTYPE(124)`/`ENOTSUPP(524)` 现定义在 `compat/mmc_env.h`(数值逐字取自 u-boot `include/linux/errno.h`)。按 ADR-0023 D-4 的口径它们**应当**上收 `core/br_error.h`(本仓库的 errno 单一真值); 未做, 登记为欠债 |
| FAT 的 `strdup`/`sprintf`/`rand`/`assert` | `assert` 给成**空语句**(与上游 `_DEBUG=0` 的展开一致 —— 上一版"打印 + 返回 -EIO"才是改上游); `strdup`/有界 `sprintf`/确定性 `rand` 是环境实现(u-boot 的 `rand` 在 vendor 树里没有原件)。前三条登记在 `fs/uboot-fat/README.md` §9 |
| **`irq-test` 的时序回归(本刀唯一一处"变差")** | 见 §4.7 的表末行: 迁移后"启动→自检结束"晚 ~0.6 s(`irq-test` 的 6 s 上限), 从基线 2/5 红变成 5/5 红。**根因**: 上游原样编译时 ISR 不能清 `INT_STATUS`(上游等待循环要读它)⇒ 只能用 `SIGNAL_ENABLE` 去断言电平线, 而"等待前重开"会让线上仍有未清位时**立刻再投递一次** ⇒ 冗余 INTx(实测 5–10 次/命令)。**已做的两次优化**: ① `udelay()` 恢复忙等语义(上游取值只有 10/100/1000/2000 µs, 而调度器的睡眠粒度是一拍 5 ms ⇒ 短自旋曾被放大 50×): 4 卡 bring-up **0.376 s → 0.204 s**; ② `udelay()` 不再顺带打开 `SIGNAL_ENABLE`(它本来就不需要被唤醒, 开了不掩会在窗口之后继续断言电平线 ⇒ 直接炸 `br_task_exit in bh`)。**未做的正解**: 降低重开频率(只在"上一次等待真的睡下去且没等到"时重开 `SIGNAL_ENABLE`), 或让等待原语把"当前要等的位"传下来、只 arm 那一组。判据 = `irq-test` 恢复到基线频率(≤2/5 红, 因为它**本来就是**间歇红) |
| `errno.h`/`time.h` 走 cflags 而非 `includes` | 两个**标准头名**的壳放在 `compat/std-names/`, 因为 brickie 的包含面判据是**名字级**的(ADR-0018), 而 `runtime/posix/include/` 下有同名文件 ⇒ 放进声明面会被判成"别人 include 了我们的头"。这是**判据的粗粒度**导致的绕行, 回归条件 = 判据能按 per-unit `-I` 解析(或排除标准 C 头名) |

## 7. 判据

- `bash tools/check-third-party-pristine.sh` —— vendor 逐字节 = 上游;
- `python3 tools/check-vendor-copies.py` —— adapter 里没有三方件源码副本(迁移前两个
  `src/fat_*.c` = 0.58/0.60 报红, 迁移后全树最高 ≈0.15);
- `brickie build` 全绿; `brickie test` 全绿(含 `fat-test` / `emmc-test` / `erofs-test` 与
  两个新脚本门禁);
- `build/obj/third_party/uboot-imx/**` 下能看到上游 `.o`(证明编的是 vendor 原件)。
