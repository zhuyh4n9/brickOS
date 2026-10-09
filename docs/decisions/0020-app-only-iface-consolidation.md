# 0020 — A-2 收口: APP 只依赖 Interface(新增 `iface/min` 皮肤 / APP 直连 core 头判红 / `iface/posix` 命名空间化)

> 状态: **已落地**(`brickie check` 0 错; `brickie build` 通过; 全门禁绿 —— 见 §3)。
> 影响面:
> **新增** `iface/min/`(Interface 直通皮肤: 日志 / 时钟 / tick / 版本)、
> `iface-posix/` → **`iface/posix/`**(目录与插件名命名空间化, 头改 `include/iface/posix/posix.h`)、
> `tools/brickie/schema/plugin.schema.json` + `rust/src/model.rs` + `rust/src/plan.rs`
> (**新增 `[plugin].symbol_prefix` 覆盖**)、
> `tools/brickie/rust/src/model.rs`(`cross_plugin_includes` 为 `<br/core/...>` 产出合成
> provider `"core"`)、`rust/src/check.rs` + `rust/src/build.rs`(**APP 直连 core 头判红**)、
> `app/hello/plugin.toml`(+`[[dep]] iface/min`, dep 改 `iface/posix`)、
> `app/hello/src/main.c`(**删掉 `<br/core/...>`**, 只 include 两个皮肤头)、
> `api/iface/iface/{posix,min}/`(快照; 旧 `api/iface/iface-posix/` 删除)、
> `product.toml` / `README.md` / 各 README。
> 设计依据: `1-01` §7.3(A-2: **APP 只依赖 Interface 插件**)/§7.4(皮肤分类)/
> §8.3(命名空间形态 `<namespace>/<short>`)/D18(POSIX 双角色)、`1-03` §1(iface-min:
> 极简别名层, 直通 native)、`4-02` §2。
> 相关: ADR-0018(声明依赖闭包 = 包含面; 本 ADR 把闭包规则扩到"APP 不许直连 core 头")、
> ADR-0019(pthread 面与多线程样例; 本 ADR 把它的皮肤改名/并入口径)。

## 1. 背景(两条被打破的约束)

1. **A-2 的收口不彻底**: `1-01` §7.3 的 A-2 说 "APP 只依赖 Interface 插件", 但 APP 为了
   日志/时钟/版本仍 `#include <br/core/br_log.h>` 等 —— core 是 iface 层**以下**的内核面,
   这条依赖没有任何声明边, 工具也不报。ADR-0018 的扫描只映射"插件拥有的头", core 头被
   静默忽略 ⇒ "APP 经 Interface 取用下层"在 core 这一半没有兑现。
2. **皮肤名不是命名空间形态**: 皮肤原名 `iface-posix`(单段), 按 `1-01` §8.3 应落在
   `iface/` 命名空间下(目录/名 = `iface/posix`)。改名后它和 `runtime/posix` **共享
   `name_short = posix`**, 而钩子符号前缀由 short 推导 ⇒ `posix_early_init/init/start`
   在链接期重复定义。

## 2. 决策

### 2.1 新增 `iface/min`: core native 的极简别名皮肤

* `plugin_type = "interface"`, `api_type = "native"`, `form = "api"`(与设计对 `iface-min`
  的分类一致); **无 `[[dep]]`**(直通 core; core 不是插件、没有 `provider#unit`,
  所以不能写 `reexport_of` —— 那正是它用 `form = "api"` 而不是 `form = "skin"` 的原因);
* 转出面只有 APP 需要的那一撮: `br_log_set_level/get_level/write` + 四个等级宏、
  `br_clock_now/freq_hz/ticks_per_ms/tick_hz/tick_count`、`BR_US_PER_MS`、
  `BR_PROTOTYPE_NAME` / `BR_VERSION_STRING`;
* **声明只有一份真值**: 皮肤头把 core 头原样 `#include` 进来, `src/iface_min.c` 只有三个
  空钩子 —— 皮肤里没有一行 core 实现(与 `iface/posix` 同构)。

### 2.2 APP 只 include 皮肤头

`app/hello/src/main.c` 现在:

```c
#include <iface/min/min.h>      /* core native: 日志 / 时钟 / tick / 版本 */
#include <iface/posix/posix.h>  /* POSIX: usleep / pthread_* / sem_* / errno */
```

源码里**不再出现 `<br/core/...>`**; 依赖边是 `[[dep]] iface/min` + `[[dep]] iface/posix`
(都是 Interface)。

### 2.3 执法: APP 直连 core 头 = check/build 红

`model::cross_plugin_includes()` 现在对 `<br/core/...>` 产出**合成 provider `"core"`**;
`check` 的 deps 域与 `build` 的 `load_ctx` **只在消费者是 `plugin_type = "app"` 时**判红
(其余插件本来就直接 include core 头, 不受限):

```
`app/hello`(APP)直接 include 了 core 头 `br/core/br_log.h`: APP 不许依赖 iface 层以下的接口
  → ADR-0020 / A-2: 经 Interface 皮肤取用(如 `<iface/min/min.h>`), 或为该 core 面在皮肤里追加条目
```

于是 "APP 只依赖 Interface" 从**约定**变成**编译期判据**(与 ADR-0018 的插件头规则合流)。

### 2.4 改名: `iface-posix` → `iface/posix`

* 目录/插件名: `iface-posix` → `iface/posix`; 头: `include/iface-posix/iface_posix.h` →
  `include/iface/posix/posix.h`; APP 的 include/dep 同步;
* 快照: `api/iface/iface/posix/posix-skin.toml`(旧的 `api/iface/iface-posix/` 删除);
* 依据: `1-01` §8.3 的推荐形态 `<namespace>/<short>`(`iface` 在 `NAMESPACES` 里, 且
  `namespace_plugin_type("iface") = "interface"`)。

### 2.5 工具: `[plugin].symbol_prefix` 覆盖

命名空间形态允许"不同命名空间共享 short"(`iface/posix` 与 `runtime/posix`),
但缺省钩子前缀按 short 推导会撞车。新增可选字段:

```toml
[plugin]
symbol_prefix = "iface_posix_"   # 缺省仍是 symbol_prefix(name_short); 给了就用它
```

* schema(`plugin.schema.json`)+ `model.rs`(解析 + 合法性: `[A-Za-z0-9_]+`)+
  `plan.rs`(`PluginFacts.symbol_prefix`, `plugin_vars` 优先用它);
* 本件用 `iface_posix_`, `iface/min` 用 `iface_min_`(都显式, 便于评审);
* 描述符/钩子/自检原型名全部随之(`iface_posix_early_init/init/start`)。

## 3. 验证记录

| 判据 | 命令 | 结果 |
|---|---|---|
| 声明面 | `brickie check` | 0 错/0 警(1 提示: 被再导出单元未冻结, dev 允许); 闭包 **18** 插件, RAM 42 KiB |
| 构建 | `brickie build` | 通过(`iface/posix` 与 `iface/min` 的钩子无重复符号) |
| 接口治理 | `brickie iface publish iface/posix#posix-skin` / `iface/min#min` | 各 0.1.0.0, green; 快照/CHANGELOG/lock 落盘, hash 抄回 plugin.toml |
| APP 只依赖 Interface | `grep -rn 'br/core/' app/hello/src/` | 0 命中(改由 `iface/min` 转出) |
| A-2 判据生效 | 临时在 APP 里加 `#include <br/core/br_log.h>` | `brickie check`/`build` 报 `BRV-MF-0001`(已实测) |
| 多线程样例 | `brickie test smoke` | `[APPCONF] SUMMARY pass=7 fail=0`(判据不变) |
| 全套 | `brickie test -j4` | 全绿 |

## 4. 遗留(如实登记)

1. **规则只覆盖 include 前缀 `br/core/`**: 符号级直用(不 include、靠其它头带出声明)仍
   抓不到 —— 与 ADR-0018 §4 的同一欠账(要 `truth = "header"`/符号级扫描才彻底)。
2. **`iface/min` 面按需增长**: 只转 APP 当前用到的 core 面, 不是 core 全量别名; 新增 core
   依赖时必须同步加条目 + 头。
3. **`symbol_prefix` 唯一性未由工具判**: 目前靠链接期重复符号暴露(本刀实际踩到一次)。
   将来可在 tax/deps 域加"派生前缀唯一"检查。
4. **`0.1.0.0` 版本不动**: 改名与新增皮肤属 dev 期结构演进; `runtime/posix#posix` 的
   `0.1.1.0` 不变(ADR-0019 已发布)。
5. 设计侧文档仍写 `iface-posix`(单段名)与"APP 可依赖 core"的口径 —— 待同步到 Design 树。
