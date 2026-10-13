# 0033 — 解散 `adapter/`: 适配层按**能力**归位, `api_type` 纯化为**表面分类**, 来源另立守卫

> 状态: **已落地**(工具侧 `rules.rs`/`model.rs`/`solver.rs`/`selftest.rs`; 三个三方插件迁目录并改
> `api_type`; 两条路径门禁改造; 声明面/文档指称)。
> 判据: `brickie-core --selftest` 520+ 例、`make tools-test` 644+ 例、全部门禁(7 host + 11 QEMU +
> 10 script)全绿; 新增两条判据 `BRV-TAX-0022` / `BRV-TAX-0023` 的负例均实测红。
> 取代: **ADR-0024 §2 / ADR-0026 §2 / ADR-0029** 中"`adapter/` 是按来源分的适配层命名空间"这一
> **布局**部分; 以及 **ADR-0021 D-3 / ADR-0022 D-1** 中"两件 FS **只能**是 `third_party`"的**分类**论证。
> 上述 ADR 的**判据与机制**(vendor 逐字节只读 + sha256 锁 + 上游原样编译 + 不许夹带副本)全部保留。
> 相关: **ADR-0031**(`core` 成为第一类插件)、**ADR-0032**(`io/` → `driver/`)、ADR-0024/0026/0029。

## 1. 背景: 用户两次裁定

> "io/目录改名为 driver, native driver 将放在这个目录"
> "源码仍然放在 `third_party/` 目录下。对于适配层: fat/erofs 搬到 `fs/` 下, subkind 为 fs;
>  mmc 搬到 `driver/` 下, subkind 为 io。当前是 prototype 阶段, 可以修改设计。"
> "保留 `third_party/` 目录, 达到隔离 license、可维护、可更新的目的。对应的 adapter 放在相应类型的目录下。"
> "对于一个三方模块, 如果不抛出接口(已经完全对接到了 native), 就不定义接口类型为 `third_party`。"

第三条把问题的性质点明了: `adapter/` 是**按来源**分的命名空间, 而树的其余部分**按能力**分
(`driver/`、`fs/`、`framework/`、`service/`、`sched/`)。同一个"设备驱动"概念因此有两个住址,
取决于它是谁写的。第四条进一步指出: `api_type` 是**接口(表面)类型**, 不抛接口的模块没有表面
可分类, 把它标成 `third_party` 是**用表面字段承载来源信息**。

## 2. 决策

### 2.1 解散 `adapter/`: 适配层按能力归位

| 旧 | 新 | `subkind` | 能力 |
|---|---|---|---|
| `adapter/uboot-mmc` | **`driver/uboot-mmc`** | `io`(不变) | 块设备驱动(MMC/SDHCI, 4 卡) |
| `adapter/uboot-fat` | **`fs/uboot-fat`** | `fs`(不变) | 介质文件系统(FAT12/16/32 读写) |
| `adapter/uboot-erofs` | **`fs/uboot-erofs`** | `fs`(不变) | 介质文件系统(EROFS 只读 + LZ4) |

`third_party/uboot-imx/**` **原样不动**(逐字节上游原件 + sha256 锁), 它的只读锁从
`adapter/third-party-upstream.lock` 移到 **`third_party/upstream.lock`**(`third_party/` = vendor 区;
被哈希的子树仍是 `third_party/uboot-imx/`, 锁文件在它**之外**)。

`subkind` 一个都不改 —— 这正是 ADR-0015 §2.2 / ADR-0032 §2.2 立过的原则: **命名空间回答"住哪里 /
谁写的", subkind 回答"是什么能力"**。`uboot-mmc` 本来就是 `subkind = "io"`, 所以"作为 io subkind
放到 `driver/` 下"只差目录名。

### 2.2 `api_type` 纯化为**表面分类**(`BRV-TAX-0022`)

`api_type` 的语义由不变量 1 钉死: `export.api_iface == [plugin].api_type` —— 它分类的是插件的
**对外接口表面**。于是:

> `api_type ∈ {third_party, runtime_adapter}` **要求至少一个 `[[export]]`**。表面分类必须有表面;
> 什么都不导出却标 `third_party`, 分类的是**空气**。

三个三方插件都**不抛接口**(无 `[[export]]`, `api/iface/` 下无对应单元)⇒ 一律 **`api_type = "native"`**:
"本仓自产的表面(可以是空集)"。来源改由 **`license = "GPL-2.0-or-later"`** + `third_party/` 承载。

**实现前对这三个插件的实测**: 它们标 `third_party` 时, 全部效果都是"对不存在的边的潜在禁止" ——
`check_export_invariants` 空转(循环在 exports 上)、`dep_direction_ok` 的 `provider_is_third_party`
对 ability 消费者**冗余**(ability→ability 本就合法)、`third_party` 不在被检消费者类里。
即: 它是一个**意图声明冒充表面分类**, 而树里没有任何东西在行使它。

### 2.3 来源另立守卫: `BRV-TAX-0023`(补回被 §2.2 拿掉的那条)

§2.2 会**静默移除**旧的 `native → third_party` 禁则, 而那条禁则是唯一阻止自研代码把 vendor 拖进
自己编译闭包的判据 —— 因为适配插件的 `[build].includes` 里**含 `../../third_party/**`**, 于是
"依赖一个适配插件" = "把 `-Ithird_party/uboot-imx/drivers/mmc` 拿进自己的闭包"。实测:

```
fs/uboot-fat 的 -I 闭包(经 [[dep]] driver/uboot-mmc 继承):
  -Idriver/uboot-mmc/../../third_party/uboot-imx/drivers/mmc      ← 上游头目录
```

而另两条路径门禁**拦不住**它: `check-third-party-pristine.sh` 的判据 (5) 只禁
`#include <third_party/…>` 的**绝对路径**写法(走 `compat/` 转发 shim 的**相对**包含是**被认可的
正途**), `check-vendor-copies.py` 与依赖边无关。

⇒ 新增一条**按键在来源上**的禁则:

> **不携带 vendor 代码(`[build]` 的 `sources`/`includes` 不触及 `third_party/`)的插件, 不得依赖
> 携带 vendor 代码的插件。**

"是否携带 vendor 代码"由**声明面本身**判定(`model::carries_vendor_code`, 解析
`[build]` 路径中的 `..` 后看前缀是否落在 `third_party/`),**不读磁盘、不看目录名** —— 所以它随
插件搬到任何命名空间都成立。这条规则把 GPL 子图**闭合**: 只有另一个携带 vendor 代码的插件才
可以依赖它, 于是 `third_party/` 的 `-I` 目录永远进不了自研代码的闭包。

附带代价(诚实登记): 旧模型里 `app → third_party ability` 是被**显式放行**的(为 sqlite 型
例外准备的)。新规则下 `app → 携带 vendor 代码的插件` 会被拦; 若某产品真要这条边, 走
`product.toml [lint].allow_edges` 显式豁免(与其余方向禁则同一口子)。

### 2.4 迁移暴露的一个**潜伏缺陷**: include 顺序曾经靠"目录名的字母巧合"

搬完之后第一次 `make all` **当场红**, 报的是 u-boot 类型重定义:

```
fs/uboot-fat/compat/fat_env.h:54:  error: "false" redefined
fs/uboot-fat/compat/fat_env.h:71:  error: conflicting types for '__u64'
driver/uboot-mmc/compat/mmc_env.h:103: note: previous declaration of '__u64'
```

根因不在本次搬迁的内容, 而在 `build.rs` 计算每个单元的 `-I` 表时**末尾有一个 `sort()`**:

```rust
incs.extend(u.includes);                 // 自己的(含自己的 compat/ 转发壳)
for d in dep_closure { incs.extend(plugin_incs(d)); }   // 依赖的(含它的 compat/)
incs.sort();                             // ★ 把"自己优先"排没了
```

`-I` 是**从左到右**搜索的, 而两个移植件**各自**为同一批 u-boot 头(`<linux/types.h>`、
`<asm/io.h>`、…)**都**提供了转发壳 —— 于是"谁赢"完全取决于目录名的字典序。旧布局下
`adapter/uboot-fat/*` 与 `adapter/uboot-erofs/*` **碰巧**都排在同目录前缀的 `adapter/uboot-mmc/*`
之前(`f`/`e` < `m`), 所以它一直是绿的; ADR-0033 把 mmc 移到 `driver/` 之后, `driver/` 排在
`fs/` **之前**, 巧合消失, 冲突立刻现形。

修法不是"把目录名挪回去", 而是**让顺序表达语义**: 保留三段的构造顺序
(环境基座 core → **自己的** includes → 依赖闭包), 去掉 `sort()`, 只做**保序去重**。
判据(实测): `fs/uboot-fat` 的 `-I` 现在是
`core` → `fs/uboot-fat/{compat,glue}` → `driver/uboot-mmc/{compat,glue}` → …, 自己的壳先命中。

★ 这条值得单独记下来的理由是: 它属于**"靠巧合维持的绿"**。搬迁只是把它撞出来, 缺陷本身在
搬迁之前就存在 —— 任何一次"让消费方的目录名字典序晚于提供方"的改名都会触发它。

### 2.5 两条路径门禁随之改造

| 门禁 | 旧 | 新 |
|---|---|---|
| `check-third-party-pristine.sh` | 锁在 `adapter/third-party-upstream.lock`; 判据 (5) 文案指向 `adapter/*/compat/` | 锁在 **`third_party/upstream.lock`**; 文案改为 `<plugin>/compat/`; 锁文件头明说"vendor 子树之外的这一份是**我们的**清单, 上游换 commit 时要改它" |
| `check-adapter-copies.py` | 扫描 `adapter/**` | 改名 **`check-vendor-copies.py`**, 扫描 **`third_party/` 之外的全部 `.c`/`.h`** |

第二条是**必须**的, 不只是"顺手": 它原来**硬编码 `collect(adapter_root)`**。适配层一旦离开
`adapter/`, 以目录名为键的扫描会**静默地停止监视**它存在的理由所在的那批文件 —— 而判据本身
("这是不是上游的副本")与文件住在哪里毫无关系。改为全树后判据更严: 实测阈值 0.30 下全树最高
相似度 **0.26**(迁移前的 `src/fat_core.c` 是 0.58), 无新误报, 且耗时 1.2 s。

## 3. 被否决的替代

| 方案 | 为什么不取 |
|---|---|
| 保留 `adapter/`, 只把 `uboot-mmc` 搬去 `driver/` | 会把**自洽可整体移除**的三方子图劈成两半(ADR-0021 D-3/0022 依赖这条性质); 且 `uboot-mmc` 一搬走就脱离了 `check-adapter-copies.py` 的硬编码扫描范围 |
| 把三个插件**全**搬进 `driver/` | `fat`/`erofs` 是文件系统(`subkind = "fs"`), 住 `driver/` 会让命名空间与能力不符; 且 `driver/` 的含义会被稀释成"驱动 + 介质 FS" |
| 让 `adapter/` 降级为"只放 vendor 元数据"的空目录 | 一个只放锁文件的目录没有存在理由; 锁归 `third_party/` 更贴近它描述的对象 |
| 给 `api_type` 加第四个值(如 `none` / `vendor`)表达"无表面但有来源" | 把两条轴**重新揉进一个字段**, 正是本 ADR 要拆掉的东西; 而且"无表面"不是一种表面 |
| 把"来源"提升为新的必填字段(`origin = own \| ported`) | 来源**已经可以推导**——`[build]` 是否触及 `third_party/` 是声明面上的确定事实, 推导比再声明一处更难漂移(与 ADR-0003 "一处事实一个可写处"同向); 真需要显式声明时, `license` 已经在那儿 |
| 保留 `native → third_party` 禁则, 只把三个插件改成 `native` | 禁则以 `api_type` 为键, 改完之后它对这**唯一**的 GPL 子图**完全失效** —— 等于把守卫删掉而不补 |

## 4. 后果与遗留项(诚实清单)

1. **`third_party/` 里现在有一个我们自己的文件**(`upstream.lock`)。锁文件头已明说这一区分
   ("本文件自己**不在**被哈希的子树里"), 门禁判据 (2)(3) 也只走 `third_party/uboot-imx/`。
   若将来认为"我们写的文件不该出现在 vendor 区", 把它挪回仓库根即可(一处 `LOCK=` 变量)。
2. **许可残余风险本身没有变化**(`br-wa-tp-001`): 三个插件的源码仍是 GPL-2.0+, 静态链进同一镜像
   后整镜像在分发时仍构成 GPL 组合作品。"隔离"不等于"消除传染"—— 本 ADR 只让**隔离的表达**
   更诚实、更可执法。
3. **`adapter/` 作为字符串在仓库里彻底消失**, 只剩历史引文(ADR-0024/0026/0029 的标题与正文,
   `docs/analysis/0003` 对旧布局的分析)。这些 ADR 顶部已加取代声明, 正文路径就地更新为新位置。
4. **`api_type = "third_party"` 这个取值没有被废除**, 只是收紧了成立条件: 它现在**必须**伴随一个
   `api_iface = "third_party"` 的导出单元(不变量 1 仍然执法)。也就是说, "把上游 API 原样暴露出去"
   这种移植模式仍然可表达; 本刀取消的是"不暴露任何东西却自称 third_party"。
5. **`BRV-TAX-0022` 的边界**: 它只看"有没有 `[[export]]`", 不看导出单元是否真的"属于"该分类的
   来源。一个插件完全可以导出 native 表面却被本仓写出来(正常), 或导出 `third_party` 表面而实际
   是 clean-room 重写(工具无法判)。工具能执法的是**声明面自洽**; "这个表面到底谁的"是评审问题。
6. **`driver/` 现在同时住着自研与移植的设备驱动**(`uart-pl011` / `uboot-mmc`)。ADR-0032 里
   "`driver/` = 我们写的"这半句解释被本 ADR 取代为"`driver/` = 设备驱动"; `driver/` ⇒
   `api_type = native`(`BRV-TAX-0021`)这条**规则仍然有效**, 只是它现在约束的是"表面"而不是"来源" ——
   两件驱动都不抛接口, 所以都满足。

## 5. 判据(本刀怎么算绿)

```bash
brickie-core --selftest                      # 命名空间 / api_type 表面 / 来源守卫 的正负例
make tools-test                              # 端到端(含 build 族与新门禁名)
build/host/x86-64/linux/bin/brickie check     # 0 errors; 24 插件闭包
brickie test                                 # 7 host + 11 QEMU + 10 script
bash tools/check-third-party-pristine.sh     # vendor 逐字节 + 无绝对路径 include
python3 tools/check-vendor-copies.py         # 全树无上游副本(实测最高 0.26)
```

负例(必须红, 均已实测):

* `fs/uboot-fat/brick.toml` 的 `api_type` 改回 `third_party`(它不抛接口)⇒ **`BRV-TAX-0022`**;
* 给 `framework/bdev-core` 加一条 `[[dep]] driver/uboot-mmc`(它不携带 vendor 代码)⇒ **`BRV-TAX-0023`**;
* `driver/uboot-mmc` 的 `api_type` 改成 `third_party` 而留在 `driver/` ⇒ **`BRV-TAX-0021`**。
