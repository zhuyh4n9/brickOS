# 0012 — 文件表(fd 表)独立成件 + errno 与 Linux 内核逐值对齐

> 状态: **已落地**(QEMU virt aarch64; `[FTCONF] 11/0`; 宿主 `ft-test` 全绿, 其中含
> **errno 与宿主 `<errno.h>` 49 码逐一对拍**; `brickie test -j8` 全量门禁绿:
> 宿主 7 + QEMU 7 + 脚本 4)。
> 影响面(**新插件 1 件**): `framework/file-table/**`(plugin.toml / include/br/ft/br_ft.h /
> src/file_table.c / src/ft_selftest.c / README.md / tests/smoke.toml);
> `core/include/br/core/br_error.h`(**只加不破**: errno-base 整表 + generic 补码);
> `tests/host/ft_test.c`(新宿主用例); `product.toml`(`[select]` 追加 1 件);
> `tests/gates.toml`(新增 `[[hosttest]] ft-test` + `plugin-test` 扩三条判据);
> `WORKAROUNDS.md`(扩写 `br-wa-test-001`)。
> 设计依据: `11-01-service.md` §1(D18: **fd 表 = 共享状态单一主人**判例)、
> `1-01-architecture.md` §7.2 规则 3 + D19(框架件 = 插件的身份)、
> `3-06-service-mgmt.md` §2(三类共享状态主人: fd 表 / socket 表 / trace 环)、
> `7-01-vfs.md` §1/§4.1(fd 表项统一持有 `br_file_t*`; `br_open` 是它的底层原语)、
> `3-01-core-api-list.md` §11(core 的 int 返回 = 负 errno; errno 编号沿用 Linux)。
> 相关: ADR-0009(vfs-core 的 `br_open_err` 与"访问模式策略归 svc-posix 的 fd 层"是
> 本件的前提; §5 遗留项 5)、ADR-0010(自检统一驱动)、ADR-0007(插件管理器相位)。

## 1. 背景与本刀范围

设计侧把 POSIX 拆成两个身份(D18): `svc-posix`(POSIX 运行时**服务**)与 `iface-posix`
(薄皮肤)。其中 **fd 表**被写成"共享状态单一主人"的**判例**(`11-01` §1、`3-06` §2),
但设计从未规定它由**哪个插件**实现 —— 只在 `1-03` §1 的 M2 行留了 `svc-posix(fd/stdio 子集)`
一句。原型侧至今 `svc-posix` **零行代码**(全树只有注释引用)。

本刀**不**做 svc-posix(那是 M2 的一大件: POSIX 子集清单、libc stub、stdio、pthread)。
本刀只做它**唯一无法绕过的地基**: **一张"小整数 → `br_file_t*`"的表**, 外加它的全部语义。

| 做 | 不做 |
|---|---|
| `framework/file-table`(表 + 语义 + 自检) | `open/read/write/stat/…` 的 POSIX 面(归 svc-posix) |
| errno 值表与内核逐值对齐 + 可执行对拍 | 域子集裁定(SD-10 仍按域成文) |
| 宿主/目标两条腿的判据 | 引用计数(见 §2.4 —— 刻意**不**做) |

## 2. 决策

### 2.1 fd 表从 svc-posix 里**抽出来**当框架件, 而不是留在服务里

设计原文是"fd 表 = svc-posix 的唯一主人"。本刀把它读成**规则 3(共享状态单一主人)**的
一个实例, 而不是"主人必须是 POSIX 服务" —— 理由是 **D19 的口径本来就是这个**:
能力框架以**插件形态**存在("框架件 = 插件的身份, core 的纪律")。抽出来之后:

1. **主人更清楚**: 全树只有 `framework/file-table` 写那张表; svc-posix 变成**消费者**
   (它自己一行表都不揣), 于是"谁拥有 fd 编号空间"这个问题只有一个答案;
2. **v2 的 socket 表可复用同一套语义**(`3-06` §2 把二者的主人问题并列), 不必再写一遍
   "最小可用 fd / dup / 最后一个引用";
3. **判决表是纯算法**, 于是"表满 / dup2 顶替 / 极值 fd"这些只有真实负载才碰得到的边界
   能在**宿主**上跑完(与 mem-test / sync-test / sched-test 同型的分工)。

**代价(诚实记录)**: 多一个插件进闭包(+1 KiB RAM), 且 svc-posix 与它之间多一条
`[[dep]] kind = "init"` 边(消费者 → 框架件, 与 vfs-core 同型, 方向合法)。本刀结束时尚无
消费者, 所以它在 `product.toml` 里是**直接选中**的 —— svc-posix 到位后那条 `[select]` 行
应当删掉, 让它经闭包自动进入(已写在该行的注释里)。

### 2.2 表**不解引用** `br_file_t` ⇒ 零依赖(与 vfs-core 同型)

`br_file_t` 只作**前向声明**: 表从头到尾只持有与比较指针。于是本件**没有 `[[dep]]`**
("deps 为空是设计结果, 不是漏写" —— 与 `framework/vfs-core` 同一条口径), 也不会把
vfs-core 拉进任何组合(设计 `8-01` 的形态 B 仍可裁剪)。

**推论: "谁关文件"必须写成回报形态。** `br_ft_release(fd, &last_file)` 只在**最后一个
引用**消失时把句柄交回调用方, 由 svc-posix 调 `br_file_close()`:

```c
int close(int fd) {
    br_file_t *f = BR_NULL;
    int rc = br_ft_release(fd, &f);
    if (rc != 0) { errno = -rc; return -1; }        /* -EBADF */
    if (f != BR_NULL) { return br_file_close(f); }  /* 最后一个引用: 才真关文件 */
    return 0;                                       /* 只是摘掉了一个 fd */
}
```

这是 POSIX `close(2)` 的**逐字翻译**, 而不是"表顺手把文件关了"。`dup2` 的 `displaced`
出参是同一形状(被顶掉的句柄若有别的引用则不回报)。

### 2.3 引用数 = 在场槽位个数(**不设 `refcount` 字段**)

POSIX 的"同一 open file description"在本件里就是**同一个 `br_file_t*`**(`dup`/`dup2`
复制的是指针)。于是"最后一个引用走了没有"可以**扫表回答**:

- **好处**: 少一个必须与槽位保持同步的字段 ⇒ 少一整类"计数漂移"缺陷
  (漏加一次 `ref++`、`release` 后忘了 `ref--` 都会留下 UAF 或泄漏);
- **代价**: `release`/`release_all`/`dup2` 里各一次 O(n) 扫描(n = `BR_FT_MAX` = 32)。

同一条思路的另一面: **在场判据 = 句柄非空**, 没有独立的 `used` 位 ⇒
"表里有几个 fd"与"表里有几个非空句柄"**永远是同一个答案**, 不可能对不上。

### 2.4 `dup` 的 flags 有两重身份, 本件只存**快照**(已知边界)

访问模式(`O_RDONLY/O_WRONLY/O_RDWR`)按 POSIX 是**每 fd** 的; 而 `O_APPEND`/`O_NONBLOCK`
属 **open file description**(被 dup 共享)。本件只有一份 `flags` 快照, 无法表达"一半共享
一半不共享"。裁定:

- 表**存全部 flags 并把解释权留给消费者**(表不解释任何位);
- `br_file_t` 侧的 `br_file_flags` 与驱动是共享位的**真值**所在(vfs-core 明写 tmpfs 用
  `br_file_flags(f) & BR_O_APPEND` 决定写前定位) ⇒ svc-posix 的 `F_SETFL` 必须把共享位
  **同时**落到文件侧, 不能只改本表;
- 这条是**已知边界**, 不是遗漏: 它写在契约头与 README 的"已知的语义边界"里, 由 svc-posix
  落地时一并收口(与 ADR-0009 §5 遗留项 5"访问模式策略归 fd 层"是同一条账)。

### 2.5 errno: **errno-base 整表收录** + generic 按需补, 全部逐字取自内核

`br_error.h` 原本只有 20 个码(core + 存储域用到的子集)。本刀改成两段:

- **第一段 = `asm-generic/errno-base.h` 全集(1–34)**。整表收录的理由不是"用得到",
  而是值表与域子集是两件事: **值表按内核抄, 域子集按语义裁**。只抄用到的会让"下一次
  需要某个码"变成一次抄写 + 一次对账; 整表收录一次到位, 且不可能"抄对一半"。
- **第二段 = `asm-generic/errno.h` 的 generic 部分, 只取用到/明确需要的**(35 起):
  `EDEADLK/ENAMETOOLONG/ENOLCK/ENOSYS/ENOTEMPTY/ELOOP/EOVERFLOW/ENOTSUP/ETIMEDOUT/`
  `EDQUOT/ECANCELED/EOWNERDEAD/ENOTRECOVERABLE`。其余 100+ 个多属网络/内核模块域,
  留给 v2 的 socket 面按需补。
- **同值别名**: `EWOULDBLOCK = EAGAIN`、`EOPNOTSUPP = ENOTSUP`(内核里就是两个名字)。

**可执行判据(这条是本刀的重点)**: 光靠"注释说与内核一致"是自我声明。宿主门禁
`tests/host/ft_test.c` 用 X-macro 表把**每一个** `BR_E*` 与宿主 `<errno.h>` 的同名宏比一次
(49 码, 一个漏抄/抄错即红): 宿主 x86-64 与目标 aarch64 共用内核 `asm-generic` 编号,
所以这条对拍**对目标同样成立**。`ft-test` 因此同时是"文件表用例"与"errno 值域用例"。

**为什么数值必须一致**(而不是"反正只在本仓内用"): POSIX 面的最终形状是
`errno = -ret`(**零转换**, `11-01` §2)⇒ 本表的编号**就是**用户态看到的编号;
抄歪一个数字, 症状会出现在任何一段按 errno 分支的三方代码里(sqlite 模式 A 即消费者)。

### 2.6 并发: 关中断临界区, 不引入锁对象

表是共享状态(会被多线程与将来的 socket 层并发调用)。每次表操作只有几条指令、**从不
在临界区里睡眠** ⇒ core 的 `br_irq_lock`(嵌套安全, 设计 `3-02` §6.4)就是正确且最省的
形态: 不需要锁对象、不需要 init、不会有"锁没初始化"这类缺陷。
`sched_class = SAFE_PREEMPT` 的依据在此。

### 2.7 错误码口径(表自己的三个码, 全部内核编号)

| 情形 | 返回 | 为什么 |
|---|---|---|
| 表满 | `-EMFILE` | 进程级 fd 用尽(`ENFILE` 是系统级的; 本件是单进程表) |
| fd 非法(越界**与**空槽) | `-EBADF` | POSIX 不区分两者, 区分了反而泄漏"这个编号曾经是什么" |
| `BR_NULL` 句柄 / 空出参 | `-EINVAL` | 空句柄不是"合法的空文件" |

`br_ft_dup` 表满时**不动 `oldfd`**, `br_ft_release_all` 装不下时**表不变** ——
失败不留副作用(POSIX 的通行纪律; 半途清空会让调用方既拿不到全部句柄也回不到原状态)。

## 3. 落地清单与判据

| 面 | 落点 |
|---|---|
| 契约 | `framework/file-table/include/br/ft/br_ft.h`(2 宏 + 11 函数; 单元 `filetable`) |
| 实现 | `framework/file-table/src/file_table.c`(静态 32 槽; 关中断临界区; 无堆分配) |
| 目标套件 | `framework/file-table/src/ft_selftest.c` → `[FTCONF] 11/0`, 由 core 在全部 `start()` 后驱动(ADR-0010) |
| 宿主套件 | `tests/host/ft_test.c` → `[FTTEST]`; 含 errno 49 码对拍 + 重跑 in-image 套件 + 极值 fd 边界 |
| 声明面 | `plugin.toml`(`[[res]] ram = 1 KiB`, 无 `[[dep]]`, `[selftest] cases = 11`) |
| 门禁 | `tests/gates.toml`: `[[hosttest]] ft-test` + `plugin-test` 增 `require/forbid/require_tags` |
| 用例骨架 | `framework/file-table/tests/smoke.toml`(TC-FT-001..011 的声明面) |
| 债 | `br-wa-test-001` 扩写(第五组自编号 `TC-FT-*`; 设计侧尚无"文件表"用例组) |

**验证(实测)**

```
$ brickie check                    # 0 错 0 警; 闭包 15 个插件, RAM 36 KiB
$ brickie test -j8                 # 宿主 7 + QEMU 7 + 脚本 4 全绿
  [FTCONF] SUMMARY pass=11 fail=0 total=11
  [SELFTEST] SUMMARY plugins=15 ran=12 skipped=4 fails=0 errors=0
  [FTTEST] errno 对拍: 49 码, 不一致 0 个
  [FTTEST] SUMMARY pass=6 fail=0 total=6
```

> ⚠ **跑门禁前先看 `product.toml [selftest].enabled`**: 它是**生成期**开关(关掉 ⇒ 描述符
> 写 `BR_PLUGIN_NO_HOOK`, 测试代码被 `--gc-sections` 裁出镜像)。而所有 QEMU 门禁的
> `require` 里都有 `\[SELFTEST\] SUMMARY … ran=[1-9]…` ⇒ `enabled = false` 时**所有 QEMU
> 门禁都会红**(不只本件)。本刀的实测是在 `enabled = true` 下跑的。

## 4. 未做 / 留给下一刀

1. **`svc-posix` 本体**: POSIX 子集清单、`errno = -ret` 的接线、libc stub、stdio、pthread。
   本件只提供它 `open/close/dup/dup2/fcntl` 四族的**表**那一半(另一半是 vfs-core)。
2. **`F_DUPFD`(指定下限的复制)**: 需要时按面预算追加, 或由 svc-posix 用 `dup2` 组合。
3. **`flags` 的共享位收口**(§2.4): 等 svc-posix 的 `F_SETFL` 落地时一并处理。
4. **socket 表**: v2 复用本件语义还是另起一件(设计 `3-06` §2 的开放问题), 到 v2 再拍。
5. **设计侧回灌**(属设计仓库, 本 ADR 只登记):
   - `11-01` §2 第 1 项把 fd 表写成"`svc-posix` 的一部分"; 本刀的实现把它拆成**框架件** ⇒
     设计侧需要一次口径收口(要么承认拆分并把它写进 `1-03` §1 的插件清单, 要么明确
     "表归服务"并说明为什么原型这样拆不成立);
   - `6-01` 需要补"文件表 / fd 表"的用例组(否则 `TC-FT-*` 永远是自编号);
   - `3-01` §11 的 errno 清单可以指向本刀的"errno-base 整表 + generic 按需"口径。
