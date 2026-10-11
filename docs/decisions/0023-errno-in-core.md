# 0023 — POSIX 错误码的名字归 core(与 POSIX 运行时解耦)+ 三方件最小改动

> 状态: **已落地**。`br/core/br_errno.h` 新增(50 个 POSIX 名字, 与 `br_error.h` 的 50 个
> `BR_E*` 一一对应); `runtime/posix/include/errno.h` 由 89 行缩到 47 行(只留它独有的
> `errno` 宏与 `strerror` 上界); 两件三方移植件的 compat 层各删掉一张自带的 errno 表;
> `BR_EUCLEAN = 117` 从内核表补入, EROFS 的 19 处 `-EFSCORRUPTED` 逐处对照上游改回;
> 新门禁 `check-errno`(三条机械判据)进 `[build].post`。
> 前置: `docs/decisions/0012-file-table-and-errno.md`(数值真值与逐码对拍)、
> `docs/decisions/0014-svc-posix-posix-runtime.md`(POSIX 面的归属)、
> `docs/decisions/0022-erofs-fat.md`(引出了"三方件最小改动"这条要求)。
> 设计依据: `3-01` §11「错误码」(负 errno + 各域子集)、`11-01` §2(`errno = -ret` 零转换)、
> `11-02` P-4(core 没有通用 TLS 槽位 —— 这条决定了 `errno` **变量**只能留在 POSIX 运行时)。

## 1. 背景

ADR-0012 已经把**数值**钉死了: `br_error.h` 逐字取自 Linux 内核, 宿主 `ft-test` 拿宿主
`<errno.h>` 逐码对拍。但**名字**一直挂在 POSIX 运行时上 ——
`runtime/posix/include/errno.h` 里那 50 行 `#define EIO BR_EIO`。

于是有两个后果, 本刀(ADR-0022)把它们同时踩到了:

1. **产品不选 `runtime/posix` 就一个 POSIX 名字都没有。** "内部错误码沿用 POSIX 编号"
   这条纪律不该依赖一个**可选插件**在不在场。数值在 core、名字在插件 —— 这个切法本身
   就自相矛盾: 名字是数值的另一种拼写, 不是另一个域的能力。
2. **三方移植件只好各自抄一份映射表。** u-boot 的 `fs/fat` 与 `fs/erofs` 里到处是裸的
   `-EIO`/`-ENOENT`/`-EFSCORRUPTED`, 而移植件的硬约束是"**尽量不改上游代码**"
   ⇒ 每个插件的 compat 层都写了一串 `#define EIO BR_EIO`(fat 12 条, erofs 2 条)。
   那既是**第三处真值**(谁都能改错一个), 又是**对上游代码的多余改动**。

还有第三条更隐蔽的:

3. **表里没有的码被"就近映射"到别的码上。** EROFS 的 `EFSCORRUPTED` 在内核里是
   `EUCLEAN = 117`, 而 `br_error.h` 当时没有 117 ⇒ 移植件写成 `#define EFSCORRUPTED BR_EIO`。
   值仍然是 POSIX 码(EIO = 5), 但它**不再是上游语义**(结构损坏 ≠ I/O 错误), 而且
   "上游那一支 `#ifdef EUCLEAN` 的构造被改掉了"正是"改动上游代码"的典型样子。

## 2. 决策

### D-1 POSIX 名字表归 **core**: `br/core/br_errno.h`

* 逐条写成 `BR_E*` 的**别名**(`#define EIO BR_EIO`), **不是第二张表** —— 值只有一处真值
  (`br_error.h`)。于是"POSIX 名字 = POSIX 编号"由构造保证。
* 归属 core 而不是任何插件: `#include <br/core/br_errno.h>` 在任何组合里都成立
  (core 是无条件在镜像里的; `runtime/posix` 是可选的)。
* 它与 `br_error.h` 是**同一批数值的两种拼写**, 所以两处的关系必须是"一一对应"而不是
  "各自维护" —— 判据见 D-5。

### D-2 POSIX 运行时只留它**独有**的两件

`runtime/posix/include/errno.h` 现在只做:

1. 再导出 `<br/core/br_errno.h>`;
2. 给 `errno` 这个**宏**(每线程槽位 `br_posix_errno()`)与 `BR_POSIX_STRERROR_MAX`。

★ 为什么 `errno` **变量**不能也搬去 core: 它要"每线程槽位", 而 core 没有通用 TLS
(`11-02` P-4; pthread 层是有"每线程记录"表的, ADR-0019)。所以分工是:
**码归 core(与运行时无关), 槽位归 POSIX 运行时(它就是运行时的状态)**。
这恰好是"内部错误码沿用 POSIX 编号、且不依赖 POSIX 运行时"的落点。

### D-3 三方插件**不自带** errno 表

两件移植件的 compat 层各只多一行 `#include <br/core/br_errno.h>`, 上游的裸名字
(`EIO`/`ENOENT`/`EOPNOTSUPP`/…) 原样成立。**全树只许有一处 errno 表**(D-5 的判据 ③ 执法)。

### D-4 缺的 POSIX 码**从内核表里补**, 不许映射到别的码

* 本次补 `BR_EUCLEAN = 117`(`asm-generic/errno.h`, 紧邻 `EDQUOT = 122` 之前),
  POSIX 名字 `EUCLEAN`, 并把对拍加进宿主 `ft-test`(ADR-0012 的原话:
  "新增值只能从内核表里抄, 不许自造编号; 抄完把对照断言加进该门禁")。
* EROFS 件因此把 19 处返回码改回上游的 `-EFSCORRUPTED`, 且**逐处与上游对照**过:
  上游写 `-EIO` 的点(data.c 的 internal error / blk_read 失败, namei.c 的 dev_read 失败,
  decompress.c 的 LZ4 失败, zmap.c 的 blk_read 失败)保持 `-EIO`;
  upstream 写 `-EFSCORRUPTED` 的 20 处里, 有 1 处在**未移植**的 deflate 函数内
  (该函数按 D-7 of ADR-0022 整段不移植), 其余 19 处全部回归。
* `EFSCORRUPTED` 的定义回到上游 `internal.h` 的形状(`EUCLEAN` 那一支) ——
  移植件在这件事上**一个字都没改**。

### D-5 机械判据: `tools/check-errno.sh`(新门禁, 进 `[build].post`)

| 判据 | 内容 | 为什么需要 |
|---|---|---|
| ① 名值配对 | `br_errno.h` 的每条必须是 `#define E<名> BR_E<名>`(**同名**绑定) | 值由构造等于 `br_error.h` |
| ② 全集覆盖 | `br_error.h` 的每个 `BR_E*` 都在 `br_errno.h` 里有一个 `E*` 名字 | 堵掉"这个码没名字"的角落 |
| ③ 一处真值 | 全树(排除 `build/` `.git/` `tools/`)除 `br_errno.h` 外没有第二处 `#define E<名>` | 三方插件不许再自带 errno 表 |

①+② 与 ADR-0012 的宿主对拍(**值**)合起来 ⇒ 每个 POSIX 名字都等于宿主 POSIX 的值 ——
不需要编译器、也不需要跑起来就能查。

### D-6 "三方件最小改动"成文

三方移植件对上游代码的改动**只允许三类**, 且都要留下痕迹:

1. **编译/运行时环境的兼容层** —— 全部收在 `src/<plugin>_compat.h`(类型、字节序、
   `str*`/`mem*`、日志、`errno` 名字)。这一层是"换了个环境", **不是改上游逻辑**;
2. **上游的 bug 修复或安全加固** —— 逐条记进 `NOTICE` 的"改写要点", 并说明为什么
   (u-boot 的 FAT 侧有好几条: `set_contents` 的顺序追加越界、`get_cluster` 把整扇区读进
   调用方缓冲、只读副本带脏标志把陈旧 FAT 窗口写回盘);
3. **与本项目契约的对接** —— 四层 ops / 生命周期 / 挂载, 收在 `src/<plugin>_vfs.c`。

除此之外**不改上游函数体**; 新增的偏离必须先问"能不能放进兼容层"。本次修订就是这条的
一次实践: 上游那 50 处裸 errno 名字, 从"插件各自映射"改成"core 提供", 于是 compat 层
少了一张表、上游代码少了一处语义改动。

## 3. 与设计的偏离(逐条)

| 设计原文 | 本刀的做法 | 为什么 |
|---|---|---|
| `11-01` §2 / ADR-0014: `errno` 面归 `svc-posix`(本树 `runtime/posix`) | 数值与**名字**归 core; `errno` **变量**与 `strerror` 仍归 `runtime/posix` | 设计与 ADR-0014 说的是"POSIX **行为面**"归属, 而"码的拼写"是 core 的 `br_error.h` 的投影; 分开之后"产品不选 POSIX 运行时就一个名字都没有"这件事消失 |
| `3-01` §11: 负 errno + 各域子集 | 不动(值表与域子集仍是 ADR-0012 / ADR-0009 的裁定) | 本刀只动"名字在哪" |
| ADR-0009 §3 裁定 4: 按内核编号补入 SD-10 之外的码 | 同一条纪律的延续: 本次按它补 `EUCLEAN` | 已有先例, 不新立规则 |

## 4. 风险与遗留

| 风险 | 现状 | 退出条件 |
|---|---|---|
| 裸 `E*` 宏与宿主 libc 的 `<errno.h>` 同名 | 目标镜像无 libc(`-ffreestanding -nostdlib`), 三方插件也不包含宿主 errno; 宿主侧只有 `tests/host/ft_test.c`(它**只**包含宿主 `<errno.h>`, 不包含 `br_errno.h`)与 `tools/`(不编译)⇒ 当前没有"两者同单元"的编译单元 | 若将来出现宿主代码同时要两者: 只经 `br_errno.h` 或显式 `#undef`; 判据 = 该单元能过 `-Werror`(宏重定义在不同替换表下会报错) |
| `E*` 名字与上游某个非 errno 的 `E*` 宏撞名 | 脚本门禁 ③ 已经把"第二处 `#define E<名字>`"堵死(名字集合取自 `br_errno.h`, 精确匹配); 目前 50 个名字与三方件里的 `EROFS_*`/`EXT_CSD_*` 无交集 | 同左(门禁即判据) |
| `runtime/posix/include/errno.h` 删掉了 `BR_POSIX_OK`? | **没有删** —— 它作为本插件对外面的既有符号保留(虽然树内没有消费者) | — |

无新增 `WORKAROUND` 条目: 本刀**减少**欠债(去掉了三处 errno 真值中的两处), 没有新增。

## 5. 落地记录与实测发现

* `br_errno.h` = 50 个名字; `br_error.h` = 50 个 `BR_E*`(含两个同值别名
  `EOPNOTSUPP`/`EWOULDBLOCK`)⇒ 门禁 `check-errno` 的 ① ② 直接对上(输出
  `ok   errno 表: 50 个 POSIX 名字 / 50 个 BR_E*(一处真值)`)。
* EROFS 侧改了 19 处返回码(`erofs_data.c` 1 / `erofs_namei.c` 3 / `erofs_decompress.c` 4 /
  `erofs_zmap.c` 11), 每一处都先打印上游对应的那一行再改 —— 这一步是用
  `.scratch/uboot-erofs/*.c` 的 `grep -n EFSCORRUPTED` 与移植件的上下文**逐个**核对的,
  不是按"凡 EIO 皆改"批量替换(那样会把上游本来就是 `-EIO` 的点改错)。
* `runtime/posix/include/errno.h` 从 89 行降到 47 行; `runtime/posix/src/posix.c` 与
  `posix_selftest.c` 一字未改(它们只用名字, 不问名字从哪来)。
* 两件三方件的 compat 层: fat 少 12 行映射表、erofs 少 2 行映射 + 1 行偏离注释,
  各多 1 行 include; 上游的 `-EFSCORRUPTED` 回归上游语义。

## 6. 一句话

**码是 core 的, 槽位是 POSIX 运行时的, 名字不再是任何人的私产。** 三方件因此少改上游
一行 —— 这正是"三方件最小改动"原则能拿出的第一份可核对的成绩。
