# brickOS 接口 safe/unsafe 管控 — 设计汇总(评审稿)

> 分支: `brickOS-prototype-safe-unsafe`(基线 `d144e40`, ADR-0037)
> 状态: **设计收敛中, 尚未写任何代码**。本文是评审用的汇总, 通过后再落 ADR + 实现。
> 目标: 在 C(无 Rust 式安全语义)下, 让"调用危险接口"这件事**必须显式、必须写出来、忘了一定失败**。
>
> ⚠ **命名说明**: 本文写于 ADR-0039/0040 之前, 里面的 `runtime/posix`、`service/*`、`platform/*`
> 是**当时的名字**。按现行命名: `runtime/posix` → `provider/posix`, `service/*` → `provider/*`,
> `platform/*`(类型) → `runtime/*`。正文不改, 以保住"当时是怎么想的"这条留痕。
>
> **修订记录**
> - 第一轮: 五条决策(生成头 / 函数指针暂缓 / vendor 拆分 / 改名+宏管控 / `[[glue]]`)。
> - **第二轮: 用户 4 条裁定, 展开为 N1–N7(见 §3.1)** —— 引入 `br_unsafe_cast`、禁止隐式类型转换、
>   `void*` 不再判 unsafe、鸭子类型不背 token、**safe/unsafe 只约束 native 接口**。
>   这一轮把 unsafe 判据从"签名形状"改为"是否碰硬件",并让类型转换独立成另一套机制。
> - **第三轮: clang 进入 prebuilts 后的重新分析(见 §3.5.1 / §3.6 / §11.4)**。
>   结论是**反向的**: clang 让 AST 检查变得可行,但同一轮里找到的**结构体包装**方案让 AST
>   变得**不必要** —— 类型转换禁令由类型系统直接执法,不需要任何检查器。
>   同时更正了 §11.4 一处关于 clang 默认严格度的过度断言。

---

## 0. 要解决的问题

C 没有 `unsafe fn`,也拿不到 Rust 那种编译器级安全保证。brickie 是仓库外的静态检查器,
只看到文本 —— 任何"纯 brickie 检查"都是启发式的(ADR-0018 §2.2 已诚实登记: "不做符号级
分析, 自己写原型再链接仍拦不住")。

因此本设计的基本策略是:

> **凡是编译器能执法的, 一律推给编译器;brickie 只审计编译器管不到的那一小块残余。**

原始需求(用户)拆成三条主线:

1. **接口声明 safe/unsafe**, 调用 unsafe 必须经一个"制造麻烦"的宏;
2. **必须声明依赖才能调用**某个插件导出的接口;
3. **(第二轮)** **类型转换**必须显式经 `br_unsafe_cast`, 隐式转换一律禁止。

三条主线对应三套**互不重叠**的机制(见 §2.1),这是第二轮最重要的结构改进。

---

## 1. 已定的决策(第一轮五条)

### 决策 1 — 消费者头文件由 brickie 生成; safe/unsafe 只对**函数**生效

开发者**不再手写公共头**,也不再手写公共符号的定义。只写 `__br_*`。

| 产物 | 内容 | 谁写 |
|---|---|---|
| `iface_public.h` | 公共原型(unsafe 函数带 token 首参) | brickie 生成 |
| `iface_impl.h` | `__br_*` 原型 | brickie 生成 |
| `iface_glue.c` | 公共名定义(转调 `__br_*`) | brickie 生成 |
| 插件的 `.c` | **只定义 `__br_*`** | 开发者 |

**后果(必须登记)**: 早期方案里"手写头与 `sig` 不一致 ⇒ `conflicting types`"这条检查**消失**
(手写头没有了)。但不是损失 —— 它消除了第二真值: `sig` 从"被哈希的声明"升格为**唯一的声明**。

检查链改挂到**实现侧**: `iface_impl.h`(由 `sig` 生成)是唯一允许声明 `__br_*` 的地方, 配合
product cflags 里**已有**的 `-Wmissing-prototypes`, 实现与 `sig` 不符仍然报红(已实测)。

**约束**: `safe/unsafe 只对函数生效` ⇒ **unsafe 的全局量必须降级为函数**(accessor)。
`var` 类导出无法承载分类, 不能留白 —— 否则指向 MMIO 的 `var` 会从规则缝隙漏过。

### 决策 2 — 函数指针问题**暂缓**, 但要登记缺口

暂缓的落点很关键: `kind = "service"` 的 ops 表(`contract.md` §7.1)正是 vfs 的
`fstype -> ops` 注册机制, 也就是**所有 FS 与设备驱动的接入点**。

代价可控的原因: **表类型本身仍由 brickie 生成**。将来把 token 加进成员时,
改动是 (a) 生成器一处 + (b) 现有 ops 表重新生成, `sig`/hash 随之成为版本事件。
**不推翻今天任何决定, 纯增量。**

### 决策 3 — `third_party` 分类保留; vendor 与 glue **拆成两个插件**

**当前状态不符合目标**: 三个 vendor 插件今天把上游 `.c` 与 `glue/` **同编在一个插件内**
(`carries_vendor_code = true`), 即 "vendor 代码连同 adapter 一同编译"。

拆分为(三个插件同型):

```
fs/uboot-erofs/          api_type = "third_party"   ← vendor 插件
├── brick.toml           sources 直指 ../../third_party/uboot-imx/...  (无副本)
│                        [[export]] 原始面(最小集)
├── compat/              u-boot 运行环境(我们写的, 服务 vendor 编译)
└── NOTICE / ADAPTER-NOTES.md

fs/erofs/                api_type = "native"        ← glue 插件
├── brick.toml           [[dep]] fs/uboot-erofs
│                        [[glue]] provider = "fs/uboot-erofs"
└── src/erofs_vfs.c      ← 原 glue/erofs_vfs.c
```

映射: `fs/uboot-fat` / `fs/fat`、`driver/uboot-mmc` / `driver/mmc`。
`uboot-` 前缀 = vendor 来源;无前缀 = 本仓 glue。**不新造 `vendor/` 命名空间**
(那正是 ADR-0033 拆掉的 `adapter/` 的翻版)。

#### 1.1 三条硬约束(已核实)

**① `third_party/` 下不能放 `brick.toml`。**
`tools/check-third-party-pristine.sh` 判据 (4) 明令:

```
fail "brick.toml found under third_party/ — third-party code is not a brickOS plugin
      (our plugins carry it and live in the capability namespaces: driver/, fs/)"
```

⇒ 拆出的 vendor 插件其 manifest 仍住在 `fs/` `driver/` 下, `[build].sources` 反向指向
`../../third_party/...`。这是 ADR-0033 的原意, 不是绕路。

**② 拆开后 `BRV-TAX-0023` 会立刻反噬, `[[glue]]` 是必要前提。**
`model::carries_vendor_code` 见 `tools/brickie/rust/src/model.rs`。今天 glue 与 vendor 同插件
⇒ `true` ⇒ 依赖 `driver/uboot-mmc`(也 `true`)被 ADR-0033 §2.3 放行。拆开后 glue 插件
`carries_vendor_code = false`, 依赖 vendor 插件**被拦**。
⇒ `[[glue]]` 不是"优雅替代", 而是**拆分的必要条件**, 正是 ADR-0033 §2.3 亲口登记的代价的解法。

**③ `BRV-TAX-0022` 要求 `third_party` 必须有 `[[export]]`。**
三个 vendor 插件今天导出数都是 **0**(ADR-0033 §2.2 据此把它们改成了 `native`)。
⇒ 拆分**强制**"vendor 的原始面是什么"成为显式声明 —— 这是本拆分的真正工作量与意义。

#### 1.2 导出面规模(已实测)

glue 实际调用的 u-boot erofs 符号 ≈ **11 个**(部分为宏:`erofs_blkoff`/`erofs_blksiz`/`erofs_pos`/
`erofs_err`), 真正的函数入口约 **7 个**:

```
erofs_read_superblock   erofs_read_inode_from_disk   erofs_ilookup
erofs_namei             erofs_pread                  erofs_blk_read   erofs_dev_read
```

⇒ 最小面, 可控工作量。

#### 1.3 收益

| 收益 | 说明 |
|---|---|
| **`compat/` 竞争结构性消失** | ADR-0033 §2.4 的"靠目录名字典序维持的绿", 根因是三个移植件各自为**同一批 u-boot 头**提供转发壳。拆开后每个 vendor 插件只服务自己 |
| **`carries_vendor_code` 粒度变准** | 今天"碰没碰 vendor"与"是不是 glue"是同一个布尔;拆开后解耦 |
| **GPL 子图真正闭合** | 今天整块放行/拦截;拆开后边界是有名字的边, 可逐一审计 |

#### 1.4 待确认

`*_selftest.c` 归属: 走 glue 注册路径的测试留 glue, 直测 vendor 原始 API 的留 vendor。

### 决策 4 — `br_safe_call` **改名**, 且语义是"减速带", **不做现场恢复**

#### 4.1 改名(理由比"提示"更强)

现名有硬缺陷: `br_safe_call` 用来调用 **unsafe** 接口, 而 **safe** 接口反而不需要它 ——
**字面含义与用法正好相反**。

| 候选 | 评价 |
|---|---|
| **`br_unsafe_call` + `br_unsafe_t`** | **推荐**。命名**被调用方**属性, 与 manifest `safety = "unsafe"` 同词; 一词贯穿 manifest / 签名 / 调用宏 / panic 报文 |
| `br_danger_call` + `br_danger_t` | 视觉更跳跃、更难忽略(更服务"制造麻烦"), 但与 manifest 用词不一致 |
| ~~`br_safe_call`~~ | 词义与用法相反 |
| ~~`br_checked_call`~~ | **不诚实**: 没有任何检查发生 |

**改名顺手解掉一个矛盾**: 设计文档里 "safe 接口也可以通过 br_safe_call 调用" 与决策 1
"safe 直接调用" 冲突(签名层面必然 `too many arguments`)。换成 `br_unsafe_call` 后
**矛盾自动消失** —— 该宏按定义只服务 unsafe。

现在改名**零成本**: token 尚未进任何头文件、`sig`、`abi_id`。

#### 4.2 设计目的是"制造麻烦", 不是恢复

恢复语义(`__builtin_setjmp`/`longjmp`、改 fault 路径与 `vectors.S`)**整体砍掉**。
随之消失的复杂度: 不需要两条 DAIF 汇编访问器、六条硬约束作废、per-CPU vs per-thread
记录栈的难题消失。

**契约简化**: 不恢复 ⇒ 跨界失败 = **不可恢复, 必须 panic**。没有"资源已分配但不回收"这类
残留状态可讨论。

**减速带可以不要钱**: token 退化为编译期常量,

```c
#define br_unsafe_call(fn, ...)  (fn((br_unsafe_t){ .cookie = ... }, ##__VA_ARGS__))
```

`-O2` 下**零指令**(唯一残留成本是多传一个参数寄存器, 而这个成本已被 unsafe glue 的
`mov x0, x1; b` 吃掉 —— 见 §5.1)。

#### 4.3 归因: 保留, 但受 product.toml 宏管控, **dev 默认开 / release 默认关**

与既有三态开关同型(`[selftest].enabled`、`[lint].frozen_deps`):

```toml
[audit]
# Attribution for br_unsafe_call crossings. "inherit" = follow [product].stage (dev on, release off).
safe_call = "inherit"          # true | false | "inherit"
```

机制走**编译期 define**, 与 `[kernel].hz` 完全同一条路(`build.rs` 的 `config_defines`):

| stage | 取值 | `BR_CFG_SAFE_AUDIT` | 运行期 |
|---|---|---|---|
| dev | `inherit`/`true` | **1** | 2 次 store, panic 打出 `文件:行` + 发起跨界的 caller |
| release | `inherit`/未声明 | **0** | token 为常量, **零指令** |

`config_defines` 推给所有 C 单元(与 `TICK_HZ` 同), core 的 `safe.c` 自然拿到。
建议照 `br_time.h` 先例加守卫:

```c
_Static_assert(BR_CFG_SAFE_AUDIT == 0 || BR_CFG_SAFE_AUDIT == 1, "must be 0 or 1");
```

**两档语义完全一致**, release 只是少一行日志 —— 不存在"只在 dev 才成立的正确性"。

### 决策 5 — `[[glue]]` 具名边, 取代现有三处分散机制

```toml
# 消费方(glue/skin)声明
[[glue]]
provider = "runtime/posix"              # 或 "fs/uboot-erofs"
units    = ["runtime/posix#posix"]      # 精确到单元
```

统一判据:

> 消费者 C 可依赖提供者 P 的单元 U ⟺ 方向表允许 C→P **且**
> ( `U.api_iface == "native"` **或** C 声明了 `[[glue]] provider = P` 且 `U ∈ units` )

一次吃掉三处:

| 现状 | 位置 | 换成 |
|---|---|---|
| `skin_exempt` 按 `reexport_of` **前缀字符串匹配** | `tools/brickie/rust/src/solver.rs` | 具名 `[[glue]]` |
| `native → runtime_adapter` 的 `api_type` 禁则 | `tools/brickie/rust/src/rules.rs` | 同一判据, 按 glue 开口 |
| `BRV-TAX-0023` vendor 来源禁则 | ADR-0033 §2.3 | 同一判据, 按 glue 开口 |

⇒ 特例变成**具名边**, 与 `[[dep]].name`、`reexport_of = ["provider#unit"]`、
`allow_edges = (consumer, provider)` 形态一致, 不再"靠前缀猜"。

**待确认**: 是否"必须且互斥"(未声明 glue 却依赖外来面即报错;两个插件声明同一
`provider#unit` 即冲突, 与既有"符号族冲突"同型)。推荐: 是。

---

## 2. `api_type` 的明确定义(回答"runtime/posix 如何定义")

三值定义为**两条正交所有权的组合**, 不再是三个形容词:

| `api_type` | 规范是谁的 | 代码是谁的 | 例子 | safety 分类 | 访问控制 |
|---|---|---|---|---|---|
| `native` | 本仓 | 本仓 | `framework/vfs-core`、`driver/uart-pl011` | **有**(D0–D4) | 方向表 + 类型转换 |
| **`runtime_adapter`** | **外部**(POSIX/glibc/pthread) | **本仓** | **`runtime/posix`** | **无**(N6) | 方向表 + `[[glue]]` |
| **`third_party`** | **外部**(u-boot/EROFS) | **外部**(`third_party/` 逐字节) | **`fs/uboot-erofs`** | **无**(N6) | **必须 `[[glue]]`**(N7) |

⇒ **`runtime/posix` 的准确定义**: 实现的是**外部既定规范(POSIX)**的一面, **代码是自己写的**
(`carries_vendor_code = false`)。所以它**不是** `third_party`;但它的**面**与 `third_party`
一样是外来面, 访问规则相同。

这解释了为什么 `app/hello` 不能直接调 `runtime/posix`: 不是因为它"危险", 而是
**外来规范面必须经一道本仓的、可审计的收敛点**(skin/glue), 否则 POSIX 语义会从几十个
调用点各自渗进 APP。

### 2.1 第二轮新增的适用范围裁定(N6 / N7)

**N6 —— safe/unsafe 只约束 `api_type = native`。** 其后果是 unsafe 的判据可以彻底简化为
"是否触碰硬件/机器层"(§3.2), 不再需要为"外来面"设计任何 safety 规则。`runtime_adapter` 与
`third_party` 的面**没有 safety 字段**,且**不带 token** —— 它们的风险由"必须经 glue"承担。

这三套机制**互不重叠**,各管一段:

| 面 | 风险是什么 | 机制 |
|---|---|---|
| `native` | 会直接动硬件/机器状态 | safety 分类 + `br_unsafe_call` |
| `native` 内的类型擦除 | 把类型系统撕开 | `br_unsafe_cast` + `br_opaque_t` |
| `runtime_adapter` / `third_party` | 外来语义从多处渗出 | **`[[glue]]`**(唯一收敛点) |

**N7 —— native 插件只能经 glue 访问 `third_party`。** 这条把决策 5 从"一个可选属性"
升级为"**强制**":  `native → third_party` 的依赖边, 若消费者没有声明
`[[glue]] provider = <提供方>`, 则**直接报错**(不再是"方向表允许就放行")。

它同时与 ADR-0033 §2.3 的 `BRV-TAX-0023` 叠加(见 §1.1 事实②): 拆分后的 glue 插件
`carries_vendor_code = false`, 依赖 vendor 插件时会同时踩两条规则, **只有 `[[glue]]` 能同时
打开两个口子**。这正是 `[[glue]]` 作为"具名边"的价值 —— 它把"我允许这条边"这件事变成
一条可审计的声明,而不是散落在两处特例里。

**"runtime_adapter 是否升级为属性"的回答**: 不需要升。它作为 `api_type` 的值已准确表达
"外来规范、本仓实现";缺的不是分类, 而是**绑定**(只有谁能访问), 载体就是 `[[glue]]`,
与 `api_type` 正交。

#### 2.1.1 一处需要确认的边界

N7 的原文只点名 `third_party`("非 native 且非 `runtime_adapter`")。所以
**`native → runtime_adapter` 是否也强制 glue, 文本上未定**。两种读法:

| | 读法 | 后果 |
|---|---|---|
| 甲 | 只有 `third_party` 强制 glue | `app → iface/posix`(runtime_adapter)保持现状, 由方向表管;`[[glue]]` 仍是 iface/posix 的声明方式但不强制 |
| 乙 | `runtime_adapter` 也强制 glue | `app` 必须声明 glue 才能用 POSIX 面 —— 收敛点更硬, 但与 N7 字面不符 |

倾向 **甲**(与 N7 字面一致, 且 `runtime/posix` 已被方向表 `app ✗ ability` 挡住)。
但 §2 上表暂按"乙"的图景写(`runtime_adapter` 的访问控制列了 `[[glue]]`),需裁定后统一。

---

## 3. unsafe 判定规则(按第二/三轮裁定重新定义)

> ★ **本节已按第二轮五条裁定整体重写**。核心变化: unsafe 不再由"签名里有 `void*`"判定,
> 而是**只由"这个接口是否触碰硬件/机器层"判定**;类型转换独立成另一套机制(`br_unsafe_cast`)。
> 两件事分开之后,判据各自都变干净了。

### 3.1 第二轮裁定(用户 4 条, 展开为 N1–N7)

| # | 裁定 | 含义 |
|---|---|---|
| N1 | 新增 `br_unsafe_cast(from, to, size)` | 类型转换的唯一合法入口 |
| N2 | native 插件: brickie **默认禁止一切隐式类型转换** | 隐式转换由编译器执法(见 §3.5) |
| N3 | safe 接口的类型转换**必须**用 `br_unsafe_cast` | 显式转换也要走这个宏 |
| N4 | `void*` 转换**不判 unsafe**, 但转换必须用 `br_unsafe_cast` | 砍掉旧的 D5a/D5b/D5c |
| N5 | 鸭子类型**不加** unsafe token | ops 表、`priv` 句柄不背 token |
| N6 | **safe/unsafe 只约束 `api_type = native` 的接口** | `runtime_adapter`/`third_party` 面不做 safety 分类 |
| N7 | native 插件**只能经 glue** 访问 `third_party` 插件 | 第 4 条,强化决策 5 |

### 3.2 判据(按优先级, 首个命中即定)

```
适用范围: 仅 api_type = native 的接口单元。其余一律不做 safety 分类(裁定 N6)。

D0. 显式 safety = "unsafe" | "safe" 永远优先
    unsafe → 收紧, 无条件允许
    safe   → 放宽, 仅当该 unit 声明 handled = [...] 且每条都被 br_unsafe_call 包住

默认推导(未声明时), 只看"是否触碰硬件/机器层":
D1. host plugin_type == "platform"                         → unsafe   [直接硬件]
D2. host subkind ∈ {io, scheduler}                         → unsafe   [需求 2]
D3. host [privileged.resources] 非空(irq/dma/pins/device)  → unsafe   [直接硬件]
D4. host [privileged].level ≥ P1                           → unsafe   [资源/机器]
```

**原 D5(按 `void*` 形状判 unsafe)整条删除**(裁定 N4)。理由不只是"用户裁定",而是它
**判错了对象**: 有 `void*` 参数说明"这个接口收一个类型擦除的指针", 说明的是**调用方要做
转换**, 而转换的风险应该由**转换点**承担,不该由接口签名承担。把风险从接口挪到转换点,
判据从"形状"变成"行为",这是一次实质改进。

### 3.3 规则规模重算

按 D1–D4 重新统计(D5 删除后), unsafe 面收敛到:

| 来源 | 插件 | 说明 |
|---|---|---|
| D1 platform | `platform/qemu-aarch64` | 全部导出面 |
| D2 subkind=io | `driver/uart-pl011`、(拆分后 `driver/mmc` 的 glue) | 设备驱动 |
| D2 subkind=scheduler | `sched/coop`、`sched/rr` | 调度器 |
| D3/D4 privileged ≥P1 | 同 D1/D2 的插件 | 与上重叠 |

⇒ **unsafe 集合 = "直接碰硬件的那几个插件"**,与"注册/取回/句柄"这类类型擦除面**无关**。
后者改由 `br_unsafe_cast` 在转换点收费(§3.4)。

原先被 D5 判 unsafe 的 11 条(`br_*_register`、`br_*_get`、`br_*_fpriv`、`br_file_ioctl`)
**全部降为 safe** —— 这正是裁定 N4 的效果,也回答了"鸭子类型不背 token"(N5)。

### 3.4 `br_unsafe_cast` 的设计

#### 3.4.1 形态(已实测,第三轮按结构体包装定版)

```c
/* core/c_core/include/br/core/br_safe.h */
#define BR_EXPECT(cond, msg)  ((void)sizeof(struct { _Static_assert(cond, msg); int _; }))

/* 不透明句柄: 结构体包装, 与任何指针类型都不兼容(§3.5.1b) */
typedef struct br_opaque { void *__p; } br_opaque_t;

/* 生产侧: 把真实指针装进句柄 */
#define br_opaque_of(ptr)      ((br_opaque_t){ .__p = (ptr) })
static inline int br_opaque_is_null(br_opaque_t h) { return h.__p == 0; }

/* 消费侧: to = 目标对象类型(不带 *);返回 to*
 * size 必须等于 sizeof(to) —— 编译期等式断言 */
#define br_unsafe_cast(h, to, size)                          \
    (BR_EXPECT((size) == sizeof(to),                         \
               "br_unsafe_cast: size != sizeof(to)"),        \
     (to *)((h).__p))
```

`to` 取**对象类型**而非指针类型,理由: 这样 `sizeof(to)` 是**对象大小**而不是指针大小,
静态断言才有意义(若 `to` 写成 `my_t *`,`sizeof` 恒为 8,断言退化成废纸)。

实测结果(真实 product cflags,aarch64):

| 用法 | 结果 |
|---|---|
| `br_unsafe_cast(h, my_t, sizeof(my_t))` | ✅ 零警告,`-O2` 下**零指令** |
| `br_unsafe_cast(h, my_t, sizeof(int))` | ❌ `static assertion failed: (4) == sizeof(my_t)` |
| `br_opaque_of(&s)` / `br_opaque_is_null(h)` | ✅ 零警告,`cmp x0,#0` |
| `(my_t *)h` 直接写 | ❌ **hard error**(结构体→指针不是合法转换) |

#### 3.4.2 `size` 参数的意义

它不是冗余,是**仪式**: 强制开发者写出"我认为这个对象的类型和大小",从而

1. 制造一次必须停顿的思考(与 `br_unsafe_call` 同一设计哲学);
2. 让每个转换点在源码里可 grep、可审计(`grep -c br_unsafe_cast` 就是风险面计数);
3. 把"我随手抛了个指针"变成"我声明了这个对象是什么"。

#### 3.4.3 为什么不带 token

裁定 N5: 鸭子类型不加 unsafe token。`br_unsafe_cast` 是**宏**,不是带 token 的函数 ——
它的摩擦来自"必须写出来",而不是来自"必须拿凭证"。这与 `br_unsafe_call` 形成两种不同
强度的摩擦,**各有其位**:

| 机制 | 摩擦形式 | 适用于 |
|---|---|---|
| `br_unsafe_call(fn, ...)` | 需要 token 凭证(签名强制) | 调用**硬件/机器层**接口 |
| `br_unsafe_cast(from, to, size)` | 需要显式写出类型与大小(宏) | **类型转换** |

### 3.5 为什么必须同时把 `void*` 换成不透明句柄类型(本设计的关键发现)

裁定 N2 要求"禁止一切隐式类型转换"。**但 `void*` 在 C 里让这条无法被编译器执法**。实测:

```c
void f_void(void *p) { my_t *m = p; }        /* ← 零诊断, -Wall -Wextra -Werror 全过 */
```

原因: C 标准规定 `void*` 与任何对象指针**可隐式互转**,是语言内建豁免,没有任何
`-W` 开关能关掉它。

⇒ **只要接口里还是 `void*`, N2 就只能靠 brickie 扫文本,永远不是硬保证。**

#### 3.5.1 两个候选,强度差一整个量级

把 `void*` 换掉有两种写法,我实测了它们在**显式转换**(最难挡的那一格)上的差别:

| | (a) 指针别名 | (b) **结构体包装** |
|---|---|---|
| 定义 | `typedef struct br_o *br_opaque_t;` | `typedef struct { void *__p; } br_opaque_t;` |
| `my_t *m = h;`(隐式) | ⚠️ warning(需 `-Werror`) | ✅ **hard error** |
| `my_t *m = (my_t *)h;`(显式) | ❌ **编译通过** ⇒ 仍需 AST | ✅ **hard error** |
| 编译器 | gcc/clang 均可 | **gcc 与 clang 都不加任何 flag 即报错** |

实测原文(clang 18.1.3 与 gcc 13,`-std=c11`,**无 `-Werror`**):

```
(a) clang: warning: incompatible pointer types initializing 'bp' with 'ap' [-Wincompatible-pointer-types]
(b) clang: error: operand of type 'br_opaque_t' where arithmetic or pointer type is required
    clang: error: initializing 'my_t *' with an expression of incompatible type 'br_opaque_t'
    gcc  : error: cannot convert to a pointer type
    gcc  : error: incompatible types when initializing type 'my_t *' using type 'br_opaque_t'
```

**(b) 的决定性优势**: 结构体→指针在 C 里**根本不是合法的转换**,所以连"写个显式 cast
硬转"这条最常用的逃逸路径都被语言本身堵死。这不需要 clang、不需要 AST、不需要白名单。

#### 3.5.2 (b) 的代价:实测为零

```asm
<take>:                      ; 收 br_opaque_t 参数
    ldr  w0, [x0]            ; ← 句柄就在 x0, 与裸指针完全一致
    ret
<give>:                      ; 返回 br_opaque_t
    adrp x0, ...
    ret                      ; ← 返回值也在 x0
<nullchk>:
    cmp  x0, #0x0
    cset w0, eq
```

单指针结构体按 AAPCS64 **走寄存器传参与返回**,与裸指针**完全同 ABI**。全套 product cflags
(`-O2 -Wall -Wextra -Werror -Wshadow -Wpointer-arith -Wmissing-prototypes` …)下零警告。

#### 3.5.3 (b) 的人机工程代价(诚实登记)

| 代价 | 说明 | 对策 |
|---|---|---|
| `if (h)` 非法 | 结构体不能当条件 | 提供 `br_opaque_is_null(h)`(实测可用) |
| 生产侧要写复合字面量 | `(br_opaque_t){ .__p = m }` | 提供 `br_opaque_of(ptr, type)` 宏 |
| 不透明字段可被直接取用 | `h.__p` 是唯一逃逸口 | **由 §3.6 的 `__` 保留标识符规则覆盖** |
| 不能用 `%p` / 变参 | 结构体不能变参提升 | 打印前经 `br_unsafe_cast` 取出指针 |
| 不再兼容 `void*` | 与外部 C API 交互处要转一次 | 边界处显式 `br_unsafe_cast` |

**`h.__p` 这条逃逸口恰好免费**: brickie 本来就要立一条"插件源码不得使用 `__` 前导标识符"
的规则(封死手写 `extern __br_*` 绕开 glue),`__p` 自动落在同一条规则下 —— **一个规则管两个洞**。

#### 3.5.4 迁移规模(已实测)

| 范围 | `void*` 处数 | 处理 |
|---|---|---|
| 三个 native 公共头(`br_vfs.h` 20 / `br_cdev.h` 19 / `br_bdev.h` 9) | **48** | 换成 `br_opaque_t` —— 这是真正的接口边界 |
| 全树(含实现内部) | 559 | **不动** —— 其中绝大部分是 `(void *buf, br_size_t n)` 缓冲区,有长度约束,与类型擦除无关 |

⇒ **规则只作用于接口边界,不是全树重写。** 48 处,可控。

### 3.6 brickie 还需要补什么(clang 就位后的重新评估)

采纳 §3.5.1 (b) 之后,**显式转换这一格也由编译器封死**,原先设计的 C1(文本扫强制转换)
与 C3(clang AST)都不再是这条规则的必需组件。残余只剩:

| 残余 | 执法者 |
|---|---|
| `h.__p` 直接取用 | **`__` 保留标识符规则**(与 `__br_*` 同一条) |
| 手写 `extern __br_*` 绕开 glue | 同上 + `nm` 归属规则(§6.2 的 S2) |

#### 3.6.1 clang AST 现在能做什么(可选升级路径,不是必需)

clang 18.1.3 已随 prebuilt 就位(§11),AST 判据从"不可用"变为"可用"。我实测了它能做到的事:

| 能力 | 实测结果 |
|---|---|
| `-Xclang -ast-dump=json` 可用 | ✅ 目标 `aarch64-none-unknown-elf` 下正常产出 |
| 区分"宏展开的 cast"与"手写的 cast" | ✅ 宏展开的节点带 `expansionLoc`(精确到宏名 token),手写的不带 |
| 识别开发者自造宏藏匿 cast | ✅ `#define MY_CAST(p) ((my_t*)(p))` 被识别(`tokLen=7` ≠ `br_unsafe_cast` 的 14) |
| 识别头文件里的直接 cast | ✅ 同"手写",一样是 `no expansionLoc` |
| 开销 | `posix.c`(3213 行) **0.12 s / 21 MB JSON**;常规编译 0.56 s |

⇒ 若将来要做**源码定位的诊断**(例如报出"第 37 行的强制转换未经 `br_unsafe_cast`"),
AST 是可行路径。但**它现在是可选的**,因为 (b) 已经把最容易出错的那一格交给编译器了。

#### 3.6.2 一个必须修的 wrapper 缺陷(会直接卡住 AST 路径)

`tools/toolchain/aarch64-none-elf-clang` 用"参数里出现 `-c/-E/-S/-M/-MM/-MD/-MMD`"
判断是否链接,不在名单里就加 `-fuse-ld=lld` 与 `--ld-path=...`。但
**`-fsyntax-only` 与 `-Xclang -ast-dump=*` 都不在名单里**,而这两种调用根本不链接 ⇒ clang 报

```
error: argument unused during compilation: '-fuse-ld=lld' [-Werror,-Wunused-command-line-argument]
error: argument unused during compilation: '--ld-path=...'  [-Werror,-Wunused-command-line-argument]
```

在 product 的 `-Werror` 下这是**硬失败**(实测: 合法文件 `-fsyntax-only` 报 2 个 error;
加 `-Wno-unused-command-line-argument` 后 0 个)。⇒ 只要 brickie 走 AST 路径,**要么先修
wrapper**(把 `-fsyntax-only` 与 `-Xclang -ast-dump*` 并入非链接集合),要么每次调用都加
`-Wno-unused-command-line-argument`。前者是正解。

### 3.7 待确认

1. **不透明句柄取 (a) 还是 (b)**: 强烈建议 **(b) 结构体包装** —— 它是 gcc 与 clang 共同的
   硬错误,不需要 AST,不需要白名单。代价是 `if (h)` 之类的人机工程损失(§3.5.3)。
2. **`br_unsafe_cast` 的 `size`**: 取 `sizeof(to)` 等式断言(推荐,纯编译期,已实测)还是运行时
   "源对象至少 `size` 字节"检查(更强,但需要框架记录大小)?
3. **`__` 保留标识符规则的边界**: 它同时管 `__br_*` 与 `__p`,需要一份明确的白名单
   (core 公共头、`build/gen/**`)。

---

## 4. 现状核实(三条关键事实)

| # | 事实 | 出处 |
|---|---|---|
| 1 | `third_party/` 下禁止出现 `brick.toml` | `tools/check-third-party-pristine.sh` 判据 (4) |
| 2 | `carries_vendor_code` 让"vendor 与 glue 同编"今天合法;拆分后会反噬 | `tools/brickie/rust/src/model.rs`;ADR-0033 §2.3 |
| 3 | 三个 vendor 插件今天 `[[export]]` 数 **均为 0**, `api_type` 均为 `native` | 全仓 `api_type` 扫描;ADR-0033 §2.2 |

补充: `glue/` 目录**已经物理存在**(`fs/uboot-erofs/glue/` 等, 各插件下还有 `ADAPTER-NOTES.md`),
"glue" 不是新造概念。

`runtime/posix` 的**唯一**消费者是 `iface/posix`(全仓核实)。即"外来面只对 glue 开放"
这条规则**今天已经存在**, 只是被散在两处特例里(`app ✗ ability` 方向表 + `skin_exempt`)。

---

## 5. 实测数据(本机验证)

### 5.1 glue 的指令开销(aarch64, `-O2`, product flag 子集)

```asm
<br_gicv3_register>:          ; unsafe: 2 条
    mov  x0, x1               ;   丢弃 token(参数左移)
    b    __br_gicv3_register  ;   尾调用, 零栈帧

<br_gicv3_info>:              ; safe: 1 条
    b    __br_gicv3_info      ;   纯跳转
```

`safe = 4 B / 1 条`;`unsafe = 8 B / 1 mov + 1 b`。**不产生栈帧。**

### 5.1b `br_unsafe_cast` 与不透明句柄的实测(第二轮 + 第三轮修正)

**第一组: `void*` 与指针别名的强度上限**

| # | 实验 | 结果 |
|---|---|---|
| E-C1 | `void *p; my_t *m = p;` | ⚠️ **零诊断**(`-Wall -Wextra -Werror` 全过)—— `void*` 是语言内建豁免 |
| E-C2 | 指针别名 (a): `ap p; bp q = p;` | ⚠️ **仅 warning**(clang18 与 gcc13 皆然,**无 `-Werror` 不报错**) |
| E-C3 | 指针别名 (a): `bp q = (bp)p;` 显式 | ❌ **编译通过** ⇒ AST 方案也堵不住"故意写" |

**第二组: 结构体包装 (b) —— 第三轮的关键发现**

| # | 实验 | 结果 |
|---|---|---|
| E-C4 | (b) `my_t *m = h;`(隐式) | ✅ **hard error**,clang 与 gcc 都一样,**无需任何 flag** |
| E-C5 | (b) `my_t *m = (my_t *)h;`(**显式**) | ✅ **hard error** —— `operand of type 'br_opaque_t' where arithmetic or pointer type is required` |
| E-C6 | (b) 传参 ABI | ✅ **零开销**: `ldr w0,[x0]`,句柄就在 x0 |
| E-C7 | (b) 返回 ABI | ✅ **零开销**: `adrp/add x0; ret`,返回值在 x0 |
| E-C8 | (b) 全套 product cflags | ✅ 零警告 |
| E-C9 | (b) `if (h)` 判空 | ❌ 非法,需 `br_opaque_is_null(h)`(实测可用) |

**第三组: `br_unsafe_cast` 本体**

| # | 实验 | 结果 |
|---|---|---|
| E-C10 | `br_unsafe_cast(h, my_t, sizeof(my_t))` | ✅ 零警告,`-O2` 下**零指令** |
| E-C11 | `size` 与目标类型不符 | ✅ `static assertion failed: (4) == sizeof(my_t)` |
| E-C12 | 逗号表达式版本 | ❌ `-Werror=unused-value`;需 `(void)sizeof(...)` 抑制 |

**第四组: clang AST(第三轮新增)**

| # | 实验 | 结果 |
|---|---|---|
| E-C13 | `-Xclang -ast-dump=json`(目标 aarch64) | ✅ 可用;`posix.c` **0.12 s / 21 MB** |
| E-C14 | 区分宏展开的 cast 与手写的 cast | ✅ 宏展开带 `expansionLoc`(含宏名 `tokLen`),手写的不带 |
| E-C15 | 开发者自造宏藏 cast | ✅ 识别(`MY_CAST` 的 `tokLen=7` ≠ `br_unsafe_cast` 的 14) |
| E-C16 | 经 wrapper 做 AST 且带 `-Werror` | ❌ `-fuse-ld=lld` unused ⇒ **硬失败**;加 `-Wno-unused-command-line-argument` 后 0 error |

**E-C4/E-C5 是三轮里最重要的一对**: 它们说明"禁止类型转换"**不需要检查器**,只需要把
类型系统用对。E-C16 说明 AST 路径虽可行,但有前置修复(§3.6.2)。

### 5.2 glue 模型的检查强度(全部实测)

| 场景 | 结果 |
|---|---|
| 开发者自己实现公共名 | 链接期 `multiple definition` |
| 实现文件没包含生成的 impl 头 | `no previous prototype`(已有 `-Wmissing-prototypes`) |
| 消费者裸调 unsafe 面 | `too few arguments` + `incompatible type` |
| 跨 TU `__attribute__((alias))` 想省 wrapper | `aliased to undefined symbol` |
| **手写 `extern __br_*` 绕开 glue** | ⚠️ **能编过** ⇒ 必须靠规则堵(见 §6) |
| 未定义符号归属 | `nm -u` 可精确判定(见 §6) |

### 5.3 早期方案的实测否证(避免重走)

| 方案 | 结果 |
|---|---|
| `#pragma GCC poison` 保护 `br_unsafe_call(fn, ...)` | ❌ 名字作为宏实参**同样**触发 poison 报错 |
| `__extension__({ __typeof__(f(...)) _r = f(...); ... })` | ❌ void 返回值下 `variable or field '_r' declared void` |
| `##` 名字粘贴替代 token 参数 | ❌ 粘不进成员表达式,`br__u_o` undeclared(函数指针无法覆盖) |
| 跨 TU `alias` 省掉 wrapper | ❌ alias 要求同 TU |

### 5.4 `msr daif, xN` **存在**编码(与既有文档相左)

```
$ aarch64-linux-gnu-gcc-13 -c ...   # 汇编通过, objdump 自洽
msr daif, x0  -> 0xD51B4200         # PSTATE 域 MSR 编码, 形态合法
```

`README.md` 的 `P-IRQ-ASM-1/2` 写着"恢复路径不写 `msr daif`(**无此编码**)"。
⇒ **建议修正那行注释**。本设计已砍掉恢复路径, 该编码暂时用不上, 但文档说法需要更正。
(改动既有文档, 需用户同意后执行。)

---

## 6. 插件封装与"声明但未定义"的检查

### 6.1 要堵的缺口

开发者在自己的 `.c` 里手写 `extern int __br_x(const cfg_t*);` 然后直接调 ——
**已实测能编过**, 因为 `__br_*` 本身不带 token。

### 6.2 三条机制(强度递增)

| | 机制 | 判据 | 强度 |
|---|---|---|---|
| **S1** | 未定义符号必须落在**声明依赖闭包**内 | `nm -u` + 闭包内各 `.o` 的 `nm --defined-only` | 精确, 补掉 ADR-0018 §2.2 的登记缺口 |
| **S2** | `U __br_*` **只允许**出现在 `build/gen/<owner>/**` | 同上, 按键在**路径**而非闭包 | 精确, **真正堵住绕过的那条** |
| **S3** | 安全模块的 `.c` 禁止 `extern`/函数声明 | 词法扫描 | 便宜、报错早, 但可被宏规避 |

**为什么 S2 不可省**: `__br_x` 的定义在**提供方**的 glue 里, 而提供方**本来就在**你的依赖闭包
里 ⇒ S1 单独存在时该符号能解析, **不报错**。必须叠加"`__br_*` 只能出现在 owner 的生成目录"。

`__br_` 双下划线本就是 C 标准**保留给实现**的名字空间;`nm` 已在 `build.rs` 的工具表里有位置。

### 6.3 与 Rust 的兼容(前瞻)

**核心洞察: Rust 不需要 token。** Rust 有 `unsafe fn` —— 不写 `unsafe {}` 就调不动。
token 本质是"**在 C 里模拟 unsafe fn**"的手段。

| 边界 | 需要 token? | 摩擦来源 |
|---|---|---|
| Rust → Rust | ❌ | Rust 自己的 `unsafe fn` / `unsafe {}` |
| C / C++ ↔ Rust | ✅ | C 侧无法表达 unsafe, token 是唯一载体 |
| C → C(今天主路径) | ✅ | 同上 |

落地要点(每条有仓库先例):

1. **ABI 单一真值**: Rust 侧 `#[repr(C)]` + 静态断言镜像 C 侧
   (先例: `br_exc.h` 被 `vectors.S` 与 C 共享, 两侧各一个 `_Static_assert` 守布局)。
2. **`#[no_mangle]` 强制**: Rust 默认 name mangling, 而 S2 规则**依赖精确符号名**。
   Rust 2024 需写 `#[unsafe(no_mangle)]`。
3. **`sig` 保持 C ABI 语法**, 与 `lang` 无关 —— 因 surface hash 必须**语言中立**
   (同一接口无论 C/Rust 实现, 声明面事实必须一致)。代价: Rust 插件用 C 类型语法写 `sig`,
   需一张类型映射表。属既有 `2-01` O-4 邻域缺口, **非本设计引入**。
4. **Rust 同样需要 S1/S2**: 一样能写 `extern "C" { fn __br_x(...); }`。
5. **`br_unsafe_call` 的 Rust 对应物**是 `macro_rules!`(C 宏 Rust 用不了)。
6. **模板族按 `<api_type>/<lang>` 分层**, 照抄 `templates/descriptor/` 现成结构。
   今天 `TEMPLATE_LANGS = ["c"]`, 属前瞻设计, 不是当期工作量。
7. **Rust 独有的优势**: 模块可见性能真正封装(`pub(super) unsafe fn x`),
   Rust→Rust **完全不需要** `__br_` + nm 那套。**C 才需要 brickie 帮忙。**

C++(`lang = "cxx"`)与 C 同处理(无 unsafe, 同 token)。

---

## 7. 检查清单(brickie)

| # | 规则 | 落在 | 建议码 |
|---|---|---|---|
| R1 | unsafe 单元的 entry 必须是函数(`var` 不允许 unsafe) | `check` safety scope | `BRV-SAFE-0001` |
| R2 | 显式 `safety` 只能按 D0 方向覆盖;`safe` 必须列 `handled` | `check` safety scope | `BRV-SAFE-0002` |
| R3 | `__br_*` 未定义引用只允许出现在 `build/gen/<owner>/**`(S2) | `build`(需 nm) | `BRV-SAFE-0003` |
| R4 | 未定义符号必须在声明依赖闭包内(S1) | `build`(需 nm) | `BRV-SAFE-0004` |
| R5 | `native → third_party` 无 `[[glue]]` ⇒ 报错;同一 `provider#unit` 被两处 glue ⇒ 冲突(N7) | `check` tax scope | 待定 |
| R6 | `handled` 列出的每条跨界, 源码里必须有对应 `br_unsafe_call` | `check` safety scope | `BRV-SAFE-0005` |
| **R7** | **native 公共头不得出现 `void*`**(须为不透明句柄 `br_opaque_t`) | `check` safety scope | `BRV-SAFE-0006` |
| **R8** | **`__` 前导标识符只允许出现在 core 公共头与 `build/gen/**`**(同时封死 `__p` 逃逸与 `extern __br_*`) | `check` safety scope | `BRV-SAFE-0007` |
| **R9** | **`safety` 字段只允许出现在 `api_type = native` 的 entry 上**(N6) | `check` safety scope | `BRV-SAFE-0008` |

R3/R4 是**编译后**事实 ⇒ 落在 `brickie build`(或一个需先 build 的 `check --scope safety`);
其余纯静态 ⇒ 落在 `brickie check`。建议新增 `safety` scope,
与现有 `deps | iface | tax | priv` 并列(已有 `--priv` 同型先例)。

**R7 与 R8 的分工在第三轮被重新划清**:

- 原 R8("扫源码里的强制转换")**已被删除** —— §3.5.1(b) 的结构体包装让显式转换成为
  **编译错误**,不再需要检查器去扫。这省掉整张白名单及其误报。
- 新 R8 只做一件编译器做不到的事: **禁止 `__` 前导标识符**。它同时管住两个逃逸口
  (`h.__p` 与手写 `extern __br_*`),是"一个规则管两个洞"。
- R7 仍然必要: 它保证**接口上不再出现 `void*`**,否则(b)的保护范围就有缺口 —— 因为
  `void*` 依然是可自由转换的。

**第三轮之后,与"类型转换"相关的检查从 2 条降到 1 条,且剩下的那条不涉及 C 语法分析。**

---

## 8. 实现影响面

| 文件 | 改动 |
|---|---|
| `core/c_core/include/br/core/br_safe.h` | **新增**: `br_unsafe_t` + `br_unsafe_call` + **`br_unsafe_cast`** + `br_opaque_t` + 保留命名空间约定 |
| `core/c_core/src/safe.c` | **新增**: `br__unsafe_mint()`,受 `BR_CFG_SAFE_AUDIT` 管控 |
| `core/c_core/src/irq/irq_fault.c` | 归因段(与既有 `isr_nest`/`curr_virq` 归因并列);**不做恢复** |
| `core/c_core/src/irq/irq_internal.h` | `br_irq_cpu_t` 追加记录槽 |
| `core/c_core/include/br/core/br_trace.h` | 新增 trace 事件 |
| **`framework/vfs-core/include/br/vfs/br_vfs.h`** | **`void*` → `br_opaque_t`**(20 处) |
| **`framework/cdev-core/include/br/dev/br_cdev.h`** | **同上**(19 处) |
| **`framework/bdev-core/include/br/dev/br_bdev.h`** | **同上**(9 处) |
| 上述三者的实现与消费者 | 每个转换点改用 `br_unsafe_cast`(估计 30–50 处落地) |
| `tools/brickie/rust/src/rules.rs` | `derive_safety()`(D1–D4)+ `[[glue]]` 判据 + 新码 |
| `tools/brickie/rust/src/model.rs` | `ExportEntry` 增 `safety`/`handled`;`Product` 增 `[audit].safe_call`;`[[glue]]` |
| `tools/brickie/rust/src/check.rs` | 新增 `safety` scope(R1/R2/R6/R7/R8/R9) |
| `tools/brickie/rust/src/solver.rs` | 删 `skin_exempt` 前缀匹配, 换 `[[glue]]`;加 N7 强制判据 |
| `tools/brickie/rust/src/plan.rs` + `templates/` | 每插件 3 件产物(`iface_public.h`/`iface_impl.h`/`iface_glue.c`) |
| `tools/brickie/rust/src/build.rs` | `gen_sources` 纳入 glue;新增 `nm` 工具 + R3/R4 审计 Step;`BR_CFG_SAFE_AUDIT` define |
| **`tools/toolchain/aarch64-none-elf-clang`** | **修 `-fsyntax-only` / `-Xclang -ast-dump*` 未被识别为非链接的缺陷**(§3.6.2)。这是 AST 路径的前置条件;若暂不上 AST,则不阻塞 |
| `tools/brickie/rust/src/iface.rs` | `safety` 纳入 canonical surface text ⇒ 进 hash/snapshot/CHANGELOG/`compat_gen`;`br_opaque_t` 改变 `sig` ⇒ **全部相关单元重生版本** |
| `tools/brickie/schema/plugin.schema.json` | entries 增 `safety`/`handled`;新增 `[[glue]]` |
| `tools/brickie/docs/contract.md` | §7.1 同步 |
| `product.toml` | `[audit].safe_call = "inherit"` |
| **三个 vendor 插件拆分** | 见 §1, 含新增 `[[export]]` 原始面 |

**`br_unsafe_t` 进签名 ⇒ 进 `abi_id`**, 属接口面变更 ⇒ 要走 `iface publish` + snapshot +
CHANGELOG + `compat_gen`。这不是包袱, 而是正确性: 它逼着"某接口从 safe 变 unsafe"成为
**可见的版本事件**。

**`void* → br_opaque_t` 是更大的版本事件**: 它改变了 48 个 `sig` 里的形参类型,
`vfs-core`/`cdev-core`/`bdev-core` 三个单元必然 `compat_gen` 前进, 所有消费者要跟着改。
这是本次设计里**唯一一次真正的破坏性变更**,必须在 ADR 里单独立项,不能混在 safe/unsafe 里
悄悄做。

**第三轮补充: 这次变更的收益也上调了。** 若取 §3.5.1(b) 的结构体包装, 它不只是"让转换可检查",
而是**让违规转换无法编译** —— 也就是说这 48 处改动换来的是一条**不需要检查器维护的**保证。
对于一个会被长期演进、检查器自身也可能有 bug 的代码库,这个交换是划算的。

**POSIX 面因 `api_type = runtime_adapter` 而完全不参与 safety 分类(N6), 但它的 `void*`
用法(122 处)是否需要 `br_opaque_t` 化,取决于它是否暴露给 native 消费者** ——
现状是只暴露给 `iface/posix`(同为 runtime_adapter), 因此**可以不动**。

---

## 9. 待裁定

### 9.1 第三轮新增(优先级最高)

| # | 问题 | 建议 |
|---|---|---|
| **A′** | **不透明句柄取 (a) 指针别名还是 (b) 结构体包装**(§3.5.1) | **(b)**。它是 gcc/clang 共同的硬错误、零 ABI 开销,且**删掉整张转换白名单**。代价是 `if (h)` 不可用(§3.5.3) |
| **A″** | 若取 (b): `br_opaque_t` 是否需要保留一个 `void*` 的兼容入口(与外部 C API 交互) | 建议**不保留**;边界处一律 `br_unsafe_cast` |
| **E** | **是否修 `tools/toolchain/aarch64-none-elf-clang` 的 `-fsyntax-only` 缺陷**(§3.6.2) | **修**。它是 AST 路径的前置条件,且当前是一个真实存在的坑(任何 `-fsyntax-only` 调用在 `-Werror` 下必失败) |
| **F** | 是否引入 AST 判据(§3.6.1) | **暂不引入**。取 (b) 后它不再必需;等真需要"源码定位诊断"时再上 |

### 9.2 第二轮遗留

| # | 问题 | 建议 |
|---|---|---|
| **B** | `br_unsafe_cast` 的 `size`: 编译期等式断言 vs 运行时下界检查 | 编译期(零开销、已实测);运行时版需要框架记录对象大小,代价大 |
| **C** | `native → runtime_adapter` 是否也强制 `[[glue]]`(§2.1.1) | 甲:不强制(与 N7 字面一致) |
| ~~**D**~~ | ~~R8 白名单范围~~ | **作废**: 取 (b) 后不需要扫描转换,白名单随之取消 |

### 9.3 第一轮遗留

| # | 问题 | 建议 |
|---|---|---|
| 1 | **命名归属**: vendor 保留 `fs/uboot-erofs`、glue 取 `fs/erofs`? | 是 |
| ~~2~~ | ~~D5c 是否保留~~ | **作废**: D5 整条已按 N4 删除 |
| 3 | **`[[glue]]` 是否"必须且互斥"** | 是(且 N7 已把 third_party 方向变成强制) |
| 4 | **`*_selftest.c` 归属** | 按所测路径归属 |
| 5 | **token 名: `br_unsafe_t` 还是 `br_danger_t`** | `br_unsafe_t`(一词贯穿);`br_danger_t` 视觉更跳跃、更难忽略 |

另需确认: `README.md` 的 `P-IRQ-ASM-1/2` 那行"`msr daif` 无此编码"是否修正(§5.4)。

---

## 10. 落地顺序建议

1. **ADR**: 按仓库风格写 `docs/decisions/0038-*.md`(含被否决替代与诚实清单)。
   **建议拆成两篇**: 0038 = safe/unsafe + `br_unsafe_call`;0039 = 类型转换(`br_opaque_t` +
   `br_unsafe_cast`)。理由: 后者是破坏性变更(48 个 `sig` 改形参),前者是纯增量,不该共命运。
2. **core 侧骨架**: `br_safe.h` + `safe.c` + fault 归因 + trace 事件 + `BR_CFG_SAFE_AUDIT`
   —— 纯新增, 不动现有插件。
3. **brickie 声明面**: `rules.rs` 的 `derive_safety()`(D1–D4)+ `[[glue]]` 判据 + 新码;
   schema/contract 同步。
4. **check scope**: `safety`(R1/R2/R6/R7/R8/R9)。
5. **iface.rs**: `safety` 纳入 canonical text(hash/快照自动跟上)。
6. **build.rs**: `nm` 工具 + R3/R4 审计 Step。
7. **拆分三个 vendor 插件** + 新增原始面 `[[export]]`;`iface/posix` 改声明 `[[glue]]`。
8. **试点**: platform `#gicv3` + `sched/coop` 加 token, `app/hello` 改调用点, 全门禁绿。
9. **(第二篇 ADR)** `void* → br_opaque_t`: 三个公共头 + 全部转换点改 `br_unsafe_cast` +
   `compat_gen` 版本事件。

**验证方法**(照仓库惯例):

- `make dbg-test` / `make irq-test` 现有 91 项必须仍全绿(改了 fault 路径);
- **新增故意 fault 的自检用例**: 安全面里用野指针跨界, 断言 (i) dev: panic 报文含 site 字符串;
  (ii) release: `BR_CFG_SAFE_AUDIT=0` 下无归因但仍 panic;
- **B2 已砍**, 无需 SP override 用例;
- `nm` 规则的**负例**必须实测红(手写 `extern __br_*` 应被 S2 抓住);
- **不透明句柄的负例必须实测红**(两条都由**编译器**红,不是 brickie):
  `my_t *m = h;`(隐式)与 `my_t *m = (my_t *)h;`(显式)—— 第三轮实测
  `clang` 与 `gcc` 在**无任何 flag** 下都报 hard error(§5.1b E-C4/E-C5);
- **R8(`__` 标识符)的负例必须实测红**: 在插件源码里写 `h.__p` 应被 brickie 抓住。

---

---

## 11. 工具链改判: clang 成为默认目标编译器(已落地)

> 这一节记录一次**已完成**的改动,它改变了 §3.6 的可选项: AST 级判据从"不可用"变成"可用"。

### 11.1 做了什么

| 位置 | 内容 |
|---|---|
| `prebuilts/toolchain.lock.toml` | 新增 7 个 `[[artifact]]`(Ubuntu noble LLVM 18.1.3 组件包),`into = "clang"`,除首条外 `merge = true`;新增 `[toolchain].cc` |
| `tools/fetch-prebuilt.py` | 新增 `merge` 支持(多包叠进同一 `into`;首条负责清旧树) |
| `tools/toolchain/aarch64-none-elf-clang` | **进版本库**的包装器(我们的代码, 不是取件产物): 补 `--target=aarch64-none-elf` + `lld` + `LD_LIBRARY_PATH` |
| `tools/brickie/rust/src/build.rs` | `cc` 候选序: 包装器 → `<cross>gcc` → `gcc-16..13` → `cc` |
| `core/c_core/src/string.c` | `BR_NO_LOOP_PATTERN` 分编译器: clang 用 `no_builtin(...)`, GCC 仍用 `optimize(...)` |
| `tools/brickie/tests/run.sh` | V-B① 候选序断言同步 |
| `README.md` / `WORKAROUNDS.md` | 工具链要求与 `br-wa-toolchain-001` 现状 |

### 11.2 实测结果

| 项 | gcc 14.2.0(回退) | clang 18.1.3(默认) |
|---|---|---|
| 镜像 `.elf` | 2 286 488 B | **1 552 808 B**(−32%) |
| `.bin` | 369 384 B | **340 680 B**(−8%) |
| 门禁 | 全绿(基线) | **全绿**: 7 host + 11 QEMU + 10 script |
| `make tools-test` | 682 例 | **682 例全过** |

**回退已验证**: 把包装器移走后 `brickie build` 仍然成功(`.comment` 显示 GCC 14.2.0),所以
"clang 缺席就构建不了"不成立 —— `br-wa-toolchain-001` 只是**提前还了一部分**,没有变成硬前提。

### 11.3 唯一改写过的源码:`string.c` 的宏

clang 不认 `__attribute__((optimize(...)))`,而原代码用 `#if defined(__GNUC__)` 守卫 —— 但
**clang 也定义 `__GNUC__`**,于是命中错分支(`-Wunknown-attributes` + `-Werror` 直接红)。
改为 `__clang__` 优先。这不是妥协: `no_builtin` 是 clang 对同一意图的正规表达。

### 11.4 对 safe/unsafe 设计的影响(第三轮已修正)

> ⚠️ **本节初稿曾断言"`-Werror` 下 clang 对 `incompatible-pointer-types` 是默认硬错误
> (GCC 14 仍需显式开启)"。实测后这句话需要更正**:两个编译器在**都带 `-Werror`** 时
> 才会把它变成错误;不带 `-Werror` 时,**clang 18 与 gcc 13 一样只报 warning**。
> 证据:
>
> ```
> clang18 (无 -Werror): warning: incompatible pointer types initializing 'bp' with 'ap'
> gcc13   (无 -Werror): warning: initialization of 'bp' from incompatible pointer type 'ap'
> ```

所以 clang 就位**并没有**让"指针互转"变成硬保证,那条仍然依赖 product 已有的 `-Werror`。

**真正被 clang 改变的是别的东西**:

1. **AST 判据从"不可用"变为"可用"**(§3.6.1),它能区分宏展开的 cast 与手写的 cast,
   且给出源码定位。
2. **但它不再是必需的**: §3.5.1 的**结构体包装**方案让"结构体→指针"成为 **gcc 与 clang
   共同的硬错误**(不需要 `-Werror`),这比任何 AST 检查都强 —— 因为它是语言的类型系统,
   不是检查器的判断。
3. **wrapper 有一个必须修的缺陷**(§3.6.2): `-fsyntax-only` / `-ast-dump` 不在非链接集合里,
   在 `-Werror` 下会因 `-fuse-ld=lld` unused 而失败。这是走 AST 路径的前置条件。

⇒ 结论: **clang 让 AST 路径成为可能,但 §3.5.1(b) 让 AST 路径成为可选。**

### 11.5 诚实清单

- 这 7 个 `.deb` **动态链接宿主 glibc**,所以与 make/ninja 同性质: "钉住版本",不是
  "与环境无关"的自举件。真正的自举仍属内部工具链那条线。
- `libclang-rt-18-dev` 的 builtins 只有 x86_64/i386;目标侧**不需要**(镜像 `-nostdlib`
  且不依赖 libgcc),但若将来代码产生 `__udivti3` 一类的 libcall, 需要另外补 aarch64 builtins。
- 取件体积 356 MB(解包后), 下载约 70 MB。

---

## 附: 本次未决的技术争论记录

| 议题 | 结论 |
|---|---|
| 是否用 `#pragma GCC poison` | ❌ 实测在宏实参上误伤 |
| 是否用 `__typeof__` 语句表达式存临时量 | ❌ void 返回下编译失败 |
| 是否用 `##` 名字粘贴替代 token 参数 | ❌ 覆盖不了函数指针 |
| 恢复语义(A/B 档之争) | **砍掉 B 档**(用户裁定: 目的是制造麻烦, 非恢复) |
| `runtime_adapter` 是否升为属性 | 否;保持 `api_type` 值, 用 `[[glue]]` 承载绑定 |
| `api_type` 的 `third_party` 是否保留 | 保留;拆出 vendor 插件使其**第一次有实体** |
| **按 `void*` 形状判 unsafe(旧 D5)** | ❌ **删除**。风险应落在**转换点**而非接口签名(裁定 N4) |
| **用 `void*` 能禁止隐式转换吗** | ❌ **实测零诊断**;必须换不透明句柄类型 |
| **不透明句柄用指针别名(a)够吗** | ❌ 不够。隐式转换只是 warning(需 `-Werror`),**显式 cast 直接编过** |
| **不透明句柄用结构体包装(b)** | ✅ **采用**。gcc/clang 双双 hard error(显式+隐式全拦),零 ABI 开销 |
| **既然 clang 有了,是否必须上 AST** | ❌ **不必须**。(b) 已让"禁止转换"成为类型系统事实;AST 降为可选的诊断增强路径 |
| **clang 让指针互转变成硬错误了吗** | ❌ **没有**(§11.4 已更正: 无 `-Werror` 时 clang18 与 gcc13 一样只 warning) |
| **wrapper 的 `-fsyntax-only` 缺陷** | ⚠️ **真实存在**: `-fuse-ld=lld` unused ⇒ `-Werror` 下硬失败;走 AST 前必须先修 |
