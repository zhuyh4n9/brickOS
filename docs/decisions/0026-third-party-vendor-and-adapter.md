# 0026 — 三方件: `third_party/` 只放逐字节上游原件, 一切适配在 `adapter/`

> 状态: **部分落地**。vendor 树 + 只读锁 + 门禁已就位; `uboot-erofs` 已按新架构
> **原样编译上游**并全链路跑通; `uboot-fat` / `uboot-mmc` 仍是旧形态(改写副本),
> 待按同一手法改造(见 §7)。
> 取代: ADR-0024 的"`src/` ↔ `adapter/` 都放在插件目录里"那套结构。
> 前置: ADR-0021 D-3(三方隔离)、ADR-0022 D-1(两件 FS 为何是三方件)、
> ADR-0023(三方件最小改动)、ADR-0024(隔离的机械判据)。

## 1. 背景: 上一版结构仍然是"改写上游"

ADR-0024 把每个三方插件拆成 `src/`(上游派生)与 `adapter/`(本项目), 并加了门禁。
但复查时暴露了一个更根本的问题: **`src/` 里装的是我们改写过的上游副本, 不是上游原件**。

以 erofs 为例, `src/erofs_namei.c` 与上游 `fs/erofs/namei.c` 的差异包括: 全文件重排
(K&R→Allman、tab→4 空格)、`IS_ERR/ERR_PTR` 改成出参、栈上 4 KiB 缓冲提到文件域静态、
删掉多设备表、errno 接地……**每一处都是"改上游代码"**。于是:

- 上游升级 = 手工重做全部改写(不可自动化);
- "哪些是上游的、哪些是我们的"只能靠人读;
- 而且我们为了过自己的 `-Wall -Wextra -Werror`, 还得反过来改上游(如删掉它故意遮蔽
  全局的局部变量名) —— 这正是任务书禁止的。

## 2. 决策

### D-1 `third_party/` 是**只读 vendor 树**, 逐字节等于上游

`third_party/uboot-imx/` 只放从 u-boot 钉死 commit 引入的**原件**, 路径与上游一致
(`fs/erofs/...`、`drivers/mmc/...`、`lib/lz4.c`、`include/...`)。**永不编辑** ——
连注释、README、LICENSE 都不动。

`adapter/third-party-upstream.lock` 逐文件记 sha256, 是这条纪律的**机械判据**。

### D-2 一切适配在 `adapter/`(prototype 顶级目录)

```
prototype/
├── third_party/uboot-imx/     ← 只读 vendor: 逐字节上游原件, 不参与编译, 只作参照与被编译的源
└── adapter/
    ├── third-party-upstream.lock   ← vendor 的 sha256 只读锁
    └── uboot-{mmc,fat,erofs}/
        ├── plugin.toml      ← 插件声明面; sources **直接指向 third_party 的上游 .c**
        ├── compat/          ← u-boot 运行环境(我们写的适配层: 类型/宏/头名适配壳)
        └── glue/            ← 块层 + VFS 对接 + 自检(本项目代码)
```

**依赖方向**: `third_party/` 是叶子(没人 include 它的头以外的东西, 它自己不知道我们);
`adapter/<插件>/compat/` 兑现上游需要的**环境**; `glue/` 兑现上游需要的**行为**
(块读、`sbi` 的存储、VFS 出口)。

> ★ 为什么 `plugin.toml` 的 `sources` 直接写 `../../third_party/uboot-imx/fs/erofs/*.c`:
> 这是"上游源码原样参与编译"的落地形态 —— **没有副本**, 所以不可能有副本与原件漂移。

### D-3 三种东西, 三种归属(判据清晰)

| 是什么 | 放哪 | 例 |
|---|---|---|
| **上游原件** | `third_party/`(只读) | `fs/erofs/namei.c`、`lib/lz4.c` |
| **u-boot 运行环境**(类型/宏/头名) | `adapter/<插件>/compat/` | `erofs_env.h`、`linux/compat.h`(壳)、`u-boot/lz4.h`(壳) |
| **本项目的对接**(块层/VFS/自检) | `adapter/<插件>/glue/` | `erofs_vfs.c`、`erofs_selftest.c` |

"适配"的定义: **给上游它所依赖的环境**, 而不是**改上游让它适应我们**。
判据: `third_party/` 里一个字节都不因我们而变(哈希锁), 而上游 .c 能编过、能跑过。

### D-4 vendor 代码按**它的**告警口径编译

上游按 u-boot 的告警集合写(不含本项目的 `-Wshadow`/`-Wpointer-arith`/
`-Wmissing-prototypes`/`-Wsign-compare`/`-Wunused-parameter`)。为了不改上游,
`adapter/*/plugin.toml` 声明 `[build].cflags = ["-Wno-shadow", …]`。

为此**修了工具链**: `[build].cflags` 以前被解析但**被丢掉**(`Unit` 无该字段)⇒
单元级标志根本没到编译器。已在 `model.rs`(BuildDecl.cflags + 解析)与
`build.rs`(Unit.cflags + **接在产品级 cflags 之后**发射)补齐。顺序有意义: 后面的
`-Wno-*` 才能关掉前面 `-Wall -Wextra -Werror` 的某几条。

### D-5 门禁把纪律变成 hash

`tools/check-third-party-pristine.sh`(取代 ADR-0024 的 `check-third-party-isolation.sh`):

1. 锁元数据齐(repo/branch/commit/snapshot_sha256);
2. **vendor 逐字节等于锁**(任一字节被改 ⇒ 红);
3. 锁里每条都在磁盘上(丢了也红);
4. vendor 里没有锁外的文件(塞东西也红);
5. `third_party/` 下没有 `plugin.toml`(三方件不是 brickOS 插件);
6. 无人 include 三方件的头(vendor 只作参照, 参与编译的是被 sources 点名的 .c)。

**负例已实测**: 往 `mmc.c` 尾加一个空格 ⇒ 红; 塞一个空文件 ⇒ 红。

## 3. 落地(erofs): 上游原样编译

`adapter/uboot-erofs/plugin.toml` 的 sources 是上游的
`fs/erofs/{super,namei,data,zmap,decompress}.c` + `lib/lz4.c`(注意源码**没有副本**)。

**证据**(不是"看起来像"):
- 先 `-fsyntax-only` 探针逐个编过, 再进构建;
- `build/obj/third_party/uboot-imx/fs/erofs/*.o` 与 `lib/lz4.o` 共 6 个目标文件;
- 镜像里有 13 个 `erofs_*`/`z_erofs_*` 符号;
- `brickie build` 全绿; **`erofs-test` 门禁通过**(含 LZ4 压缩夹具)。

### 3.1 适配过程中发现的"环境事实"(都是给环境, 不是改上游)

| 上游依赖 | 我们提供 | 为什么不能"随便给一个同宽的" |
|---|---|---|
| `u64`/`s64`/`loff_t` | `unsigned long long` / `signed long long` / `long long` | u-boot 的 `u64` **不是** `uint64_t`: 在 LP64 上后者是 `unsigned long` —— 同宽**不同型**, 上游满地的 `%llu` 会与实参不符(实测踩到) |
| `PAGE_SIZE` | 4096(平台事实) | 上游 `internal.h` 有 `#if PAGE_SIZE != 4096 #error` |
| `struct list_head` | 类型本身(不给遍历) | 上游 inode/dentry **有该字段**(布局要成立), 但我们不用那些链(瞬态 inode, ADR-0022 D-3) |
| `IS_ERR`/`ERR_PTR`/`PTR_ERR` | 内核那套高地址侧约定 | 上游 `namei.c` 靠它把"找不到"与"出错"合在一个返回值里 |
| `malloc/calloc/realloc/free` | 接到 core 堆 | 裸机 unikernel 没有 libc |
| `pr_err/pr_info/pr_debug` | 接到 `br_log_write` | 上游 `internal.h` 自己把 `erofs_*` 映射到它们 ⇒ **名字由上游给, 实现由我们给** |
| `BUILD_BUG_ON` | u-boot 的机制(函数体内 `if` + `__compiletime_error`) | **不能**用 `_Static_assert`: 上游最后一条自检的条件不是整型常量表达式 |
| `get_unaligned`/`put_unaligned`、`__rcode`、`RELOC_LOADER`、`IS_ENABLED` | 宏/常量 | 上游原文用到 |
| `struct erofs_sb_info sbi` 的**存储** | 由 `glue/` 定义 | 上游把它放在 `fs.c`(u-boot 的 blk/DM 胶水), 我们不编译那份 —— 它的职责由 glue 接管 |

★ 一条反直觉的发现: 上游 `erofs_check_ondisk_layout_definitions()` 是 `static inline`
而**上游从不调用它** ⇒ 那些 `BUILD_BUG_ON` 永不诊断, **上游这份布局自检是死的**。
本件的判据(文件域 `_Static_assert`)因此不能撤 —— 见 ADR-0024 §7.2。

## 4. 与 ADR-0024 的关系

ADR-0024 的**隔离判据仍然有效**(三方代码与本项目代码必须分得开), 但它的**结构**被本
ADR 取代: 那时把"改写过的上游"放在插件目录里, 仍然混淆了"上游的"与"我们的"。现在
`third_party/` 与上游逐字节相同 ⇒ 边界从"靠人读"变成"靠哈希"。

`adapter/<插件>/src/`(改写副本)这一层**取消**。逐文件的对照报告
(`docs/analysis/0003-third-party-port-delta.md`)仍然有效, 它记录的是"改写副本相对上游
改了什么"——那正是我们现在要消灭的东西, 作为历史与对照留在那里。

## 5. 未做与欠债

| 项 | 状态 |
|---|---|
| `uboot-fat` 迁到新架构 | **待做**。探查结果: 上游 `fs/fat/fat_write.c` 是**单编译单元**(它 `#include "fat.c"`)⇒ 只需编一个文件; 依赖面 = ~21 个 u-boot 头名 + `struct blk_desc`/`disk_partition` + `blk_dread/blk_dwrite` + malloc/printf/assert/strdup/rand。首次探针只差 1 个 `command.h` ⇒ **可做**。注意本件 `compat/fat_compat.h` 目前**复制**了上游 `include/fat.h` 的盘上结构/常量 —— 那正是本 ADR 要消灭的, 应改为指向 vendored `include/fat.h` |
| `uboot-mmc` 迁到新架构 | **难**。上游 `drivers/mmc/{mmc.c,sdhci.c}` 是 u-boot DM 形态且以 `udelay()` **轮询**完成传输, 还牵连 SD/UHS/HS200/HS400/tuning/RPMB/分区/`blk_desc` 整族(未 vendor)。原样编译 = 要么把那一族都 vendor 进来, 要么放弃本项目的中断驱动 + 静态实例表(ADR-0021/0022 的设计)。**待专门决策**, 不宜顺手做 |
| `adapter/uboot-{erofs,fat,mmc}/ADAPTER-NOTES.md` 与 `README.md` | 仍描述旧的 `src/` 结构, 待重写 |
| `adapter/uboot-erofs/compat/` 里那些"壳"头的路径写法 | 现在是相对路径(`../../../../third_party/...`); 因为 brickie 的 `-I` **按字典序**排, 指向 `third_party/include` 的 `-I` 会**盖住** `compat/` 里的同名壳(`<fs.h>`/`<part.h>` 被解析到 u-boot 的真头 → 它还要 `rtc_def.h` 之类)。相对路径没有这个歧义 |

## 6. 判据

- `bash tools/check-third-party-pristine.sh`(注册于 `tests/gates.toml`);
- `bash tools/check-errno.sh`(errno 单一真值, 见 ADR-0027);
- `brickie test` 全绿 —— 其中 `erofs-test` 是"上游原样编译且真的能读盘"的判据。
