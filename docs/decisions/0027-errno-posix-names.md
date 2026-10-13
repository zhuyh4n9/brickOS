# 0027 — errno 的名字与 POSIX 完全一致(去掉 `BR_` 前缀)

> 状态: **已落地**。`br_error.h` 的 50 个 errno 名字由 `BR_E<名>` 改为 **`E<名>`**(POSIX
> 原名), 76 个文件随之改名; `br_errno.h` 退化为转发头; 门禁 `check-errno` 判据重写;
> 宿主用例 `ft-test` 的 errno 对拍改为"先捕获宿主真值再比"。
> 前置: ADR-0012(数值逐字取自内核 + 宿主逐码对拍)、ADR-0023(名字表上收 core)、
> **ADR-0026**(三方件改为原样编译上游 —— 本文是它的一半前提)。

## 1. 背景: 两条要求其实是同一件事

任务书有两条要求:

1. **不要修改任何三方代码**, 一切兼容放适配层;
2. **errno 的名称与 POSIX 完全一致, 没有 `BR_` 前缀**。

初看是两件事, 实际是同一件事的两面: 上游 u-boot 的代码里到处是**裸的** `-EIO` /
`-ENOENT` / `-EFSCORRUPTED`(它假设环境里有 `<errno.h>`)。只要我们的 errno 名字与 POSIX
**完全同名同值**, 那些代码就**一个字都不用改** —— 适配层只需要保证"这些名字在场"。

反过来, 名字带 `BR_` 前缀时, 唯一能让上游编过的办法就是**往上游文件里塞一张映射表**
(ADR-0023 之前 fat/erofs 就是这么干的: `#define EIO BR_EIO`)。那既是第二处真值,
**也是对上游代码的修改**。⇒ 去掉前缀, 映射表就没有存在的必要。

## 2. 决策

### D-1 名字就是 POSIX 原名, 且与数值同处一地

`core/c_core/include/br/core/br_error.h`:

```c
#define EPERM        1   /* Operation not permitted */
#define ENOENT       2   /* No such file or directory */
...
#define EWOULDBLOCK  EAGAIN      /* 11(内核里就是两个名字) */
#define EOPNOTSUPP   ENOTSUP     /* 95 */
```

- **单一真值**: 名字与数值在同一个文件、同一行 —— 不再有"名字表 + 数值表"两张表。
- `BR_ERR(e)`(`-(int)(e)`)保留: 它是"取负"的宏, **不是 errno 名**。
- `BR_OK` 保留(不是 errno)。
- `core/c_core/include/br/core/br_errno.h` 退化为**转发头**(只 `#include <br/core/br_error.h>`),
  既有包含点不用改; 它自己不再定义任何名字。

### D-2 只改 errno 名, 不碰同前缀的别的标识符

`BR_E` 是个**容易撞车的前缀**: 本仓还有 `BR_EC_*`(异常类)、`BR_EMMC_*`、
`BR_EROFS_*`、`BR_ESR_*`、`BR_EXC_*`、`BR_EXTABLE_*`、`BR_ERR`、`BR_ERRNO_*`。
⇒ 改名是**按 `br_error.h` 的定义表逐条做**的(50 个名), **不是**正则全量替换
"`BR_E` → `E`"。判据: 全树 grep 这 50 个名都不再带前缀, 而上面那些非 errno 名一个没动。

### D-3 门禁判据跟着改(否则会留下"没人守"的角落)

`tools/check-errno.sh` 原来守的是"名字表 ↔ 数值表**配对**"。两张表合一之后, 判据变成:

1. **裸名**: `br_error.h` 里不许再出现带 `BR_` 前缀的 errno 定义;
2. **一处真值**: 全树(排除 `build/`、`third_party/`)除 `br_error.h` 之外**无人**
   `#define E<名>` —— 三方插件不许自带 errno 表;
3. **数值对拍**: 交给宿主用例 `ft-test`(见下)。

### D-4 ★ 一个**会静默丢覆盖**的连带修改: 宿主对拍必须换写法

`tests/host/ft_test.c` 原来把"本仓名 vs 宿主名"写成宏表 `X(EPERM, EPERM)` ——
左边是我们的 `BR_EPERM`, 右边是宿主 `<errno.h>` 的 `EPERM`。

名字改成同名之后, 它变成 **`X(EPERM, EPERM)` = 自己跟自己比(恒真)** ⇒ 那个用例会
**永远绿**。这是比"编不过"更坏的一类问题: 测试还在, 覆盖没了。

改法(顺序是关键):

```c
#include <errno.h>            /* ① 先拿宿主的真值 */
#define BR_ERRNO_NAMES(X) X(EPERM) X(ENOENT) ...   /* 单一名字表 */
static const int s_host_errno[] = { BR_ERRNO_NAMES(BR_HOST_CAP) };  /* ② 捕获 */
#undef EPERM ...              /* ③ 撤掉宿主的同名宏 */
#include <br/core/br_error.h> /* ④ 让我们的定义生效 */
/* 比较: (int)(EPERM) vs s_host_errno[i] —— 比的是"我们的名字"vs"宿主的值" */
```

第 ③ 步不是可选的: glibc 也定义 `ENOTSUP`/`EOPNOTSUPP`(且拼写可能不同), 不撤就会
"重定义"冲突(-Werror 直接红)—— 实测踩到。

## 3. 影响面

| 项 | 数 |
|---|---|
| 改名的 errno 名 | 50 |
| 受影响文件 | 76(核心/框架/平台/IO/FS/调度/服务/运行时/APP/接口/测试/适配层/门禁/文档) |
| 保留的非 errno `BR_E*` | `BR_EC_*` / `BR_EMMC_*` / `BR_EROFS_*` / `BR_ESR_*` / `BR_EXC_*` / `BR_EXTABLE_*` / `BR_ERR` / `BR_ERRNO_*` |
| 改名的定义处 | `br_error.h`(定义) + `br_errno.h`(退化为转发) |

## 4. 验证

- `bash tools/check-errno.sh` ⇒ `ok errno 表: 50 个裸 POSIX 名(无 BR_ 前缀 / 一处真值)`;
- `brickie build` 全绿;
- `brickie test` ⇒ **门禁全绿(全部)**: 宿主 7(含 `ft-test` 的 50 条与宿主 `<errno.h>`
  逐码对拍)+ QEMU 11 + 脚本 7;
- `bash tools/check-third-party-pristine.sh` ⇒ PASS(vendor 树未受影响)。

## 5. 与设计的关系

设计 `3-01` §11 的口径是"负 errno + 各域子集", `11-01` §2 是"`errno = -ret` 零转换"。
本文不改这两条, 只改**名字的拼写**: 从"本仓前缀 + POSIX 名"变成"就是 POSIX 名"。
于是 "`errno` 的名字"与"本项目自己的符号前缀(`BR_`)"不再混在一起 —— 这对一个
"内部错误码沿用 POSIX 编号"的项目是更自洽的: **编号是人家的, 名字也该是人家的**;
`BR_` 前缀留给本仓自己发明的东西。

> 历史记录不改: `docs/decisions/0012-*`/`0023-*` 里出现的 `BR_E*` 是**当时**的事实,
> ADR 不重写历史。本文是它们的接续。
