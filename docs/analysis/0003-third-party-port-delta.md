# 三方移植件逐文件对照(相对 u-boot 钉死 commit)

> ⚠ **本文记的是"改写副本"时代的事实(ADR-0024 的取证底稿), 不是当前结构**。
> 自 ADR-0026 起 `adapter/<插件>/src/`(上游改写副本)这一层被取消: 上游原件只在
> `third_party/uboot-imx/`(只读 + sha256 锁), 由各 `plugin.toml` 的 `sources` **直接编译**;
> 适配收进 `adapter/<插件>/{compat,glue}`。所以下表的"行级相似度 / 变更行"现在读作
> **"当年那份改写副本改了上游多少"** —— 它正是要消灭的东西, 作为历史与对照留在这里。
> - erofs 的迁移见 ADR-0026(1/3);
> - fat 与 mmc 的迁移见 [ADR-0029](../decisions/0029-fat-mmc-vendor-pristine.md)(2/3), 那里有
>   迁移**后**的对照(参与编译的 vendor 文件清单 + adapter 侧的相似度上界);
> - "adapter/ 里不许夹带副本"这条判据现在是机械的:
>   `python3 tools/check-adapter-copies.py`(注册于 `tests/gates.toml`)。
>
> 本文仍是**历史取证的唯一底稿**: 基准与复现命令、每个文件的差异性质、审计修掉的缺陷
> (R-1..R-5)、已删除的"上游没有的代码"、以及两条"判定不可行"的自我更正(§8/§9)。

> 本文是 ADR-0024 的**取证底稿**: 逐文件给出"相对上游改了什么、还差多少、为什么"。
> ADR 记的是**决策**, 本文记的是**事实与数字** —— 两者分开, 免得决策文档被 300 行表格淹没。
>
> 状态: 已落地(指**当年**这一刀)。**没有任何 `src/` 文件与上游逐字节相同, 除了 `erofs_fs.h`** ——
> 它是唯一的例外, 且只差一个 include(见 §4)。

## 1. 基准与复现

| 项 | 值 |
|---|---|
| 仓库 | `https://github.com/nxp-imx/uboot-imx` |
| 分支 | `lf_v2026.04` |
| **commit** | `bb41975aeb4cbd153b49c9fd62da1d64cd65968d` |
| 快照 | `codeload` tarball, 41 546 项, **47 019 609 B** |
| sha256(快照) | `9d060f32c84b15c1ae2e9903c300d51b3088898b3ad80b95bca74c67ef6a4a52` |

```bash
# 取基准(落点在 build/, 不进版本库) —— 与 ADR-0024 §2 同一条命令
REF=build/uboot-ref/uboot-imx-bb41975aeb4cbd153b49c9fd62da1d64cd65968d
mkdir -p build/uboot-ref && cd build/uboot-ref
curl -sL -o uboot.tar.gz \
  https://codeload.github.com/nxp-imx/uboot-imx/tar.gz/bb41975aeb4cbd153b49c9fd62da1d64cd65968d
sha256sum uboot.tar.gz     # 期望 9d060f32…4a52
tar xzf uboot.tar.gz "$(basename "$REF")"

# 逐文件看差异(示例)
diff -u "$REF/drivers/mmc/sdhci.c"  <repo>/adapter/uboot-mmc/src/sdhci_host.c | head
diff -u "$REF/fs/erofs/erofs_fs.h"  <repo>/adapter/uboot-erofs/src/erofs_fs.h
```

### 1.1 两个口径, 别混用

| 口径 | 定义 | 用途 | 陷阱 |
|---|---|---|---|
| **原始 diff** | `diff -u` 的增删行数 / `difflib` 行级相似度(**含**注释与空白) | 判断"能不能逐字" | 无 |
| **归一化** | 去注释、去空白、折叠括号后再比 | 判断"逻辑上改了多少" | **会把"重排"洗掉** ⇒ 高估逐字可行性 |

> ★ **方法论教训**(ADR-0024 §7.3 记过一次): 本刀先按归一化口径(erofs LZ4 段"实义行 ~90% 相同")
> 把该段列为"下一步可逐字化", 后来用原始口径复测 —— 545 行里 **919 个变更行**(相似度 27.9%),
> 绝大多数是重排。**结论相反**。判"能不能逐字"必须用原始口径。

## 2. 逐文件对照表

指标 = 原始口径行级相似度(`difflib.SequenceMatcher`, 未归一化)。`变更行` = `diff` 增删行数。

> ⚠ 头文件的数字要**谨慎读**: `*_internal.h` 是"本件的内部契约", 它把上游若干头的内容
> (类型/宏/原型/结构体)**重排 + 裁剪 + 补充**到一处 ⇒ 相似度天然低, 但不代表逻辑差异大。
> 同理 `erofs_internal.h` 与上游 `decompress.h` 也有交叉(单独比只有 4.0%, 但
> `z_erofs_decompress_req` 确实搬进来了)。**判断"改了什么"请看各表的差异性质, 而不是只看百分比。**

### 2.1 `adapter/uboot-mmc`

| `src/` 文件 | 行 | 上游对应物 | 上游行 | 相似度 | 变更行 | 判定 |
|---|---|---|---|---|---|---|
| `mmc_core.c` | 409 | `drivers/mmc/mmc.c` **+ `mmc_write.c`** | 3352+254 | 4.7% | 3753 | **重写** |
| `sdhci_host.c` | 560 | `drivers/mmc/sdhci.c` + `sdhci-adma.c` | 1050+87 | 10.3% | 1600 | **重写** |
| `mmc_regs.h` | 203 | `include/mmc.h` + `include/sdhci.h` | 1093+546 | 5.1% | 1288 | 摘录子集 |
| `emmc_internal.h` | 134 | `drivers/mmc/mmc_private.h` | 135 | 12.6% | 219 | 改写 |

- **为什么"重写"**: 上游的 mmc/SDHCI 是 **u-boot DM 形态**(`udevice`/`blk_desc`/`uclass`),
  且以 `udelay()` **轮询** `INT_STATUS` 完成一次传输; 本件是裸机 unikernel, 出口是
  `br_bdev_ops_t`, 传输是**中断驱动 + 信号量**(ADR-0021 D-4), 主机层是**静态实例表**
  (ADR-0022, 四块卡)。这是**结构性**差异, 不是改名能收敛的: 要逐字就得把
  DM/uclass、SD/UHS/HS200/HS400/tuning/RPMB/分区、`blk_desc` 整族搬回来。
- `mmc_regs.h` 是**摘录**(只留本件用到的常量)⇒ 结构上不可能逐字; 但它逐条对齐上游数值,
  唯一的**值错**已修(见 §3)。
- 上游来源 **`mmc_write.c`** 是审计补上的: 早先 `NOTICE` 把写路径也记在 `mmc.c` 名下, 不准。

### 2.2 `adapter/uboot-fat`

| `src/` 文件 | 行 | 上游对应物 | 上游行 | 相似度 | 变更行 | 判定 |
|---|---|---|---|---|---|---|
| `fat_core.c` | 1523 | `fs/fat/fat.c` | 1448 | 21.8% | 2691 | 派生 |
| `fat_write.c` | 2290 | `fs/fat/fat_write.c` | 2179 | 25.4% | 3853 | 派生 |
| `fat_internal.h` | 349 | `fs/fat/fat.c`(仅 `fat_itr`/`fat_dir` 两段照抄)+ 本件契约 | 1448 | 4.9% | 1777 | 混合 |

- 归一化后 fat 是**三件里最贴近上游的**(实义行 ~76%/~66% 相同); 相似度低主要是
  **重排**(4 空格替代 tab)与无 libc 的改名(`uboot_fat_strlen` 之类)。
- **本件新增**(上游没有): `uboot_fat_truncate()` 160 行 —— 上游只有"从 pos 写 size 字节",
  而本项目 VFS 契约要 `setattr(BR_STAT_SIZE)`。**已评估"搬去 `adapter/`"的代价并放弃**,
  理由是实测结论(ADR-0024 §5): 需把 4 个 static 提升为全局, 且它要写可变全局
  `total_sector`(不变量 = "每个入口从卷刷新")⇒ 跨 TU 共享会把该不变量扩散出去。

### 2.3 `adapter/uboot-erofs`

| `src/` 文件 | 行 | 上游对应物 | 上游行 | 相似度 | 变更行 | 判定 |
|---|---|---|---|---|---|---|
| **`erofs_fs.h`** | 472 | `fs/erofs/erofs_fs.h` | 466 | **98.3%** | **16** | ✅ **逐字**(仅 include, 见 §4) |
| `erofs_super.c` | 139 | `fs/erofs/super.c` | 108 | 19.8% | 202 | 派生 |
| `erofs_namei.c` | 344 | `fs/erofs/namei.c` | 266 | 17.4% | 574 | 派生 |
| `erofs_data.c` | 447 | `fs/erofs/data.c` | 359 | 17.9% | 752 | 派生 |
| `erofs_zmap.c` | 815 | `fs/erofs/zmap.c` | 698 | 17.1% | 1399 | 派生 |
| `erofs_decompress.c` | 710 | `fs/erofs/decompress.c` **+ `lib/lz4.c`** | 181+546 | 27.9%(LZ4 段) | 919(段) | 派生(两文件合并) |
| `erofs_internal.h` | 476 | `fs/erofs/internal.h` | 397 | 25.7% | 737 | 派生(大幅裁剪) |

- `erofs_decompress.c` 是**两个上游文件的合并**(`decompress.c` + `lib/lz4.c`), 且本件有意把
  `LZ4_decompress_safe*` 收成 `static`(不污染镜像符号空间)⇒ **整文件永远不可能逐字**。
- 各 `.c` 的差异性质(逐条): ① 全文件重排(K&R→Allman, tab→4 空格); ② 无 libc/u-boot 的
  接地(`malloc`→`br_malloc`, `printf`→`br_log_*`, `IS_ERR/ERR_PTR`→出参); ③ **删**多设备
  (`extra_devices`)与 deflate/zstd; ④ **栈纪律**: 上游 `char buf[4096]` 与内嵌 4 KiB
  `mpage` 的 `struct erofs_map_blocks` 提到文件域静态; ⑤ errno 名字由 core 提供(ADR-0023)。

## 3. 审计核实并修掉的缺陷(逐条留证)

| # | 位置 | 事实 | 判据 | 处置 |
|---|---|---|---|---|
| R-1 | `mmc_regs.h` | `EXT_CSD_CMD_SET_NORMAL 0u`, 上游 `include/mmc.h:249` 是 `(1 << 0)` | 直接 `grep` 两侧 | 改为 `(1u << 0)` |
| R-2 | `erofs_zmap.c` | `z_erofs_reload_indexes()` 读进 `map->mpage`, 两个解析器却读 `uboot_erofs_scratch()` —— **两个不同缓冲、中间无拷贝**; `mpage` 全文只写不读 | `grep -rn mpage` 全树 + 确认无拷贝 | 读点改 scratch |
| R-3 | 11 个 `src/` 文件 | 上游每个文件都有 `SPDX-License-Identifier`, 派生件全丢 | `grep -c SPDX` 两侧 | 按上游逐文件补回 |
| R-4 | `erofs_fs.h` | 裁剪把 `erofs_xattr_ibody_size()` 的 `sizeof(struct …)` 换成魔数 `12u`, `EROFS_XATTR_ALIGN` 换成 `4u`; 该函数**有读者**(`namei.c:74,110` → `xattr_isize` → inline 数据定位) | 恢复上游后由 `BUILD_BUG_ON(... != 12)` + 适配层 `_Static_assert` 双钉 | 恢复 `sizeof()` |
| R-5 | `erofs_super.c` | 写了 `sbi->available_compr_algs` / `lz4_max_distance` —— **上游该 commit 从头到尾没有赋过值, 且无任何读者**(全树只有字段声明); 本件抄的是 Linux 内核 `super.c` 的口径 | `grep -rn` 两侧 + 确认无读者 | **删除**(见 §5) |

> R-2 与 R-4 都是"看起来只是裁剪/改名, 实际会读错盘"的类型 —— 这类才是审计的价值所在。
> R-4/R-5 的完整论证见 ADR-0024 §7/§9。

### 3.1 常量值的**穷举**核对: `mmc_regs.h` 没有第二个错值

R-1 是"抄错一个常量值"。这类错误**最隐蔽也最危险**(值错 = 寄存器/命令写错, 而编译器
不会说话), 所以本刀对 `mmc_regs.h` 做了一次**穷举**核对, 而不是抽查:

**做法**(需要 §1 取的上游树):
1. 从两侧抽出全部 `#define`, 注意**函数式宏的判据是"宏名与 `(` 之间无空格"**(C 预处理规则),
   不是"正文里有括号" —— 判错会把 `(1u << 0)` 这类对象宏误当函数宏跳过;
2. 递归展开**同文件内**的宏引用(本件与上游各自展开);
3. 把上游的 `BIT(n)` 展成 `(1 << n)`(u-boot `include/linux/bitops.h`); 本件写 `(1u << n)`, 等价;
4. **先剥字面量后缀、再转十六进制** —— 顺序反了会失配: `0x80000000u` 里紧跟 `u` 的位置
   没有词边界, 十六进制正则匹配不到(本刀在此踩过一次, 是脚本的 bug 不是代码的);
5. 交给求值器比值。

**结果**:

| | 数量 |
|---|---|
| 同名常量**值相同** | **134** |
| 同名常量**值不同** | **0** |
| 跳过(需人工看) | 5 |

5 条跳过的**逐条人工核实**结果:

| 名 | 本件 | 上游 | 判定 |
|---|---|---|---|
| `MMC_RSP_R1b` | `(MMC_RSP_R1 \| MMC_RSP_BUSY)` | `(MMC_RSP_PRESENT\|MMC_RSP_CRC\|MMC_RSP_OPCODE\|MMC_RSP_BUSY)`, 跨两行 | **等价**: 本件的 `MMC_RSP_R1` 与上游逐位相同 ⇒ 展开后同一值 |
| `MMC_CARD_TYPE_26` / `_52` | `(1u << 0)` / `(1u << 1)` | `EXT_CSD_CARD_TYPE_26` / `_52` = `(1 << 0)` / `(1 << 1)` | **值相同, 只是改名**(上游前缀是 `EXT_CSD_`) |
| `SDHCI_INT_OURS` | 本件自有的聚合宏(汇总本驱动会打开信号的 13 个中断位) | — | 本件自有, 无上游对应物; 源码里已注明 |
| `BR_THIRD_PARTY_UBOOT_MMC_REGS_H` | 本件 include guard | — | 本件自有 |

**结论**: `mmc_regs.h` 里**没有第二个错值**; R-1 是唯一的(且已修)。这一条是"审计"而非
"重构"的产出 —— 它把"看起来像抄错"的 39 个疑似项逐个证伪, 从而把该文件的值可信度钉死。

> ★ 附带事实: `mmc_regs.h` 与上游**逐字相同的行为 0**(93 条仅拼写差异: `0u`↔`0`、
> `BIT(n)`↔`(1u << n)`、tab↔空格对齐)。**拼写不影响值**, 而该文件是**摘录子集**、
> 结构本来就是本件的 ⇒ 不做对齐(收益是 diff 好看, 代价是 90+ 行 churn 与引入 `BIT` 依赖)。
> 判定依据与 §9 的"外部事实 vs 实现内部"不同: 这里**值**才是外部事实, 拼写不是。

## 4. 唯一做到逐字的文件: `erofs_fs.h`

```
$ diff -u <upstream>/fs/erofs/erofs_fs.h <repo>/adapter/uboot-erofs/src/erofs_fs.h
-（只有这 5 行被删）
-#include <asm/unaligned.h>
-#include <fs.h>
-#include <part.h>
-#include <stdint.h>
-#include <compiler.h>
+（换成这 1 行 + 10 行说明注释）
+#include "erofs_compat.h"
```

**除这 5 行 include 外, 466 行与上游逐字节相同(含缩进)。** 收敛过程:

| | 相对上游的变更行 | 相似度 |
|---|---|---|
| 收敛前 | 529 | — |
| 收敛后 | **16** | **98.3%** |

代价与前提(两个必须**照搬上游**的机制, 否则上游原文编不过 —— 详见 ADR-0024 §7.1):

1. `BUILD_BUG_ON` **不能**是 `_Static_assert`: 上游最后一条自检是
   `BUILD_BUG_ON(__builtin_constant_p(fmh) ? fmh != cpu_to_le64(1ULL<<63) : 0)`, 而 `fmh`
   是函数内 `const` 局部量、**不是整型常量表达式** ⇒ `_Static_assert` 报
   "expression in static assertion is not constant"。适配层照搬了 u-boot 的
   "函数体内 `if` + `__compiletime_error`"机制。
2. `__packed`(上游写成**位置属性** `} __packed u1;`)、`__maybe_unused`、`BIT` 由 u-boot 的
   `<compiler.h>`/`<linux/bitops.h>` 提供, 适配层必须补齐。

### 4.1 附带发现: 上游那份布局自检**是死的**

上游 `erofs_check_ondisk_layout_definitions()` 是 `static inline` 而**上游从不调用它**
(该 commit 全树 grep 只有定义处)⇒ GCC 不实例化 ⇒ 里面的 `__compiletime_error` 永不诊断。
本插件早先改写成**文件域 `_Static_assert`** 的版本反而**更强**。

处置: **定义**留 `src/`(与上游逐字), **判据**放 `adapter/erofs_vfs.c` 的文件域
`_Static_assert` 表。负例已实测: 改错任一 `== N` ⇒ 编译失败并打出可读消息。
(先试过"取上游函数地址强制实例化"—— 断言确实被激活, 但上游那句复合字面量在本项目的
`-Wall -Wextra -Werror` 下触发 `-Wuninitialized`, 为编过一句原文得压制告警 ⇒ 放弃。)

## 5. 已删除的"上游没有的代码"

| 位置 | 是什么 | 为什么删 |
|---|---|---|
| `erofs_super.c` 的压缩字段初始化(11 行) | 抄自 Linux 内核口径, 填 `available_compr_algs`/`lz4_max_distance` | 上游**没有**这段, 且填完**无人读** ⇒ 纯粹是"额外添加的代码"(任务书明令禁止)。删除后 `erofs-test`(含 LZ4 压缩夹具)仍全绿 ⇒ 证实无用 |

**已验证但**判定为"非缺陷、保留"的两处(免得后来者误"修"):

| 位置 | 事实 | 结论 |
|---|---|---|
| `fat_core.c:43` / `fat_write.c:48` 的 `#define LOG_CATEGORY LOGC_FS`, 而 `LOGC_FS` 全树未定义 | 这一行是**上游自己的**(`fat.c:11`/`fat_write.c:8`); `LOGC_FS` 定义在 u-boot 的 `include/log.h`, 本件不引入日志分类体系 | **忠实保留, 不是 bug**: `LOG_CATEGORY` 在本件从不被展开, 无副作用。删它反而**偏离**上游 |
| `fat_compat.h` 里的 `uboot_fat_fls`/`uboot_fat_ilog2`/`uboot_fat_get_unaligned_le32` 无引用 | 属 `adapter/`(我们的代码), 不是上游派生件 | 可清理, 但与本刀(隔离/收敛)无关, 登记为小债 |

## 6. 当前布局与判据

| 插件 | `src/`(上游派生) | `adapter/`(本项目) |
|---|---|---|
| uboot-mmc | `mmc_core.c` `sdhci_host.c` `mmc_regs.h` `emmc_internal.h` | `mmc_compat.h` `emmc_bdev.c` `emmc_selftest.c` |
| uboot-fat | `fat_core.c` `fat_write.c` `fat_internal.h` | `fat_compat.h` `fat_vfs.c` `fat_selftest.c` |
| uboot-erofs | `erofs_{super,namei,data,zmap,decompress}.c` `erofs_fs.h` `erofs_internal.h` | `erofs_compat.h` `erofs_vfs.c` `erofs_selftest.c` |

- 三个 `plugin.toml` 均为 `sources = ["src/*.c", "adapter/*.c"]` + `includes = ["src","adapter"]`;
- **单向依赖**: `src/` 不出现 `#include <br/...>`(运行环境只经适配层入口头);
- 判据 = `tools/check-third-party-isolation.sh`(四条机械判据, 注册于 `tests/gates.toml`)。

## 7. 验证

| 项 | 结果 |
|---|---|
| `brickie build` | 全绿(含强制重链) |
| `brickie check` | 0 错误 / 0 警告 / 1 提示 |
| `brickie test` | **门禁全绿(全部)**: 宿主 7 + QEMU 11(含 `emmc`/`fat`/`erofs`)+ 脚本 7 |
| `make tools-prebuilt-check` | 三件全 ok |
| 布局自检负例 | 改错 `== N` ⇒ 编译失败(已实测) |
| 隔离门禁 | PASS |

> ★ 无回归的核实手法: 自检 pass 内有**既有**间歇 race(`TC-POSIX-016`/`APP-PT-006`/FAT 写闭环
> = `br-wa-boot-002`), 所以判据不能是"跑一次全绿"。用 `git stash` 隔离出**改动前**的树、
> 对同一门禁重复 25 次: **改动前同样 1/25 复现同样两条** ⇒ 与本刀无关。

## 8. 还没做(按"值不值得"排序)

| 项 | 可行性 | 为什么没做 |
|---|---|---|
| `mmc` 两件 `.c` 逐字化 | **低** | 结构性: 要搬回 DM/uclass、SD/UHS/HS200/HS400/tuning/RPMB/分区、`blk_desc` —— 与 ADR-0021/0022 的设计(bdev 形态/中断驱动/静态实例表)直接冲突 |
| erofs/fat 各 `.c` 的**重排**对齐 tab | 中(纯机械) | 高 churn、零功能收益; 且这些文件**本来就不可能**逐字(见 §2.3) ⇒ 收益只是 diff 好看 |
| `erofs_internal.h` 逐字化 | **低(已判定不可行)** | 见 §9 —— 它**不是** `erofs_fs.h` 那类「盘上格式」, 而是**实现侧内部头**; 恢复上游原文得把 dentry 缓存与 mkfs buffer-head 两套**本件根本没有的子系统**的声明一并搬进来 |
| `fat_internal.h` 里 `uboot_fat_fls` 等死助手清理 | 高(小) | 属 `adapter/`, 与本刀主题无关; 登记为小债 |

## 9. 为什么「内部头」不能像 `erofs_fs.h` 那样逐字化

本刀先前把 `erofs_internal.h` 列为「下一个真正的候选」(理由是它与 `erofs_fs.h` 一样是
「纯定义的头」)。**这个判断是错的**, 复核后撤回。区别是原则性的:

| 类别 | 例 | 能否逐字 | 为什么 |
|---|---|---|---|
| **盘上格式**(on-disk layout) | `erofs_fs.h` | ✅ **能, 且应该** | 它定义的是**介质上的字节**; 与上游不一致 = 读错盘。且它不依赖运行环境(只依赖整数类型/`round_up` 之类) |
| **实现侧内部头** | `erofs_internal.h`、`*_internal.h`、`mmc_regs.h` | ❌ **不能** | 它描述的是**上游那份实现的内部结构**; 本件的实现架构不同(无 dentry 缓存 / 无 mkfs / 无 DM), 照搬声明 = 搬进不存在的子系统 |

`erofs_internal.h` 的具体阻塞项(逐条 grep 核实; 这些符号在本件**只出现在说明注释里**):

| 上游有、本件无 | 它属于哪个子系统 | 本件为什么没有 |
|---|---|---|
| `i_hash`/`i_subdirs`/`i_xattrs`(`struct list_head`) | inode/dentry 缓存 | 本件的 inode 是**瞬态路径包装**(ADR-0022 D-3, 设计 SD-3), 不做缓存 |
| `i_count`/`i_parent`/`i_ino`/`i_srcpath` | erofsfuse / mkfs | 本件既不是 FUSE 也不是造镜像工具 |
| `struct erofs_dentry` + `is_dot_dotdot()` + `IS_ROOT()` | dentry 层 | 本件没有 dentry 对象(同上) |
| `struct erofs_buffer_head` | mkfs 的写缓冲 | 本件**只读** |
| `erofs_sb_set_*`/`erofs_sb_clear_*`(由 `EROFS_FEATURE_FUNCS` 生成) | 运行期改特性位 | 本件只读, 只用 `has_*` |
| `xattr_shared_*` 等字段 | xattr 共享区(只写者) | 本件不解析 xattr |

**结论**: 逐字恢复它 = 往 `src/` 里加进 dentry 缓存与 mkfs 的**声明**(以及为编过它们所需的
`list_head` 等类型), 而对应的**实现**在本件根本不存在。这恰是任务书禁止的「额外添加代码」。
⇒ 判定: `erofs_internal.h` **保持现状**(裁剪 + 本件契约), 差异逐条登记, 不再作为收敛目标。

> **可推广的判据**(供后续判断任何三方文件):
> 逐字化只在「**该文件描述的是外部事实**(盘上格式 / 协议线格式 / 寄存器布局)」时才既可行又正确;
> 一旦它描述的是「**上游实现自己的内部结构**」, 本件架构不同 ⇒ 逐字化等于把不存在的子系统搬进来。
