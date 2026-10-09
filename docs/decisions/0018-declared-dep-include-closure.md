# 0018 — 声明依赖闭包 = 编译期包含面(未声明的 `#include` 在 check/build 两侧都报红)

> ★ **后续(ADR-0020)**: 本 ADR 引入的皮肤当时叫 `iface-posix`(目录同名); 后续按命名空间
> 形态改为 **`iface/posix`**(头 `include/iface/posix/posix.h`, 并因与 `runtime/posix` 共享
> short 而显式覆盖 `symbol_prefix = "iface_posix_"`)。下文旧名是**当时的记录**, 不再逐一改写。
>
> 状态: **已落地**(`brickie check`/`brickie build` 对未声明依赖报红; 全门禁绿 —— 见 §4)。
> 影响面:
> `tools/brickie/rust/src/build.rs`(**每单元 include 面** = core + 自己 + 声明依赖闭包;
> 去掉全局 `all_includes`; `load_ctx` 增加"声明 ↔ 使用"校验)、
> `tools/brickie/rust/src/model.rs`(`dep_closure()` + `cross_plugin_includes()` 扫描器)、
> `tools/brickie/rust/src/check.rs`(deps 域增加同口径校验)、
> **新增** `iface-posix/`(Interface 薄皮肤, `form = "skin"`, 再导出 `runtime/posix#posix`)、
> `app/hello/plugin.toml`(+`[[dep]] iface-posix`)、`app/hello/src/main.c`(只 include 皮肤头)、
> `app/hello/README.md`、`product.toml`(注释)。
> 设计依据: `1-01` §7.3(依赖方向表; APP 仅经 Interface)/§7.4(skin 再导出)/D18(POSIX 双角色)、
> `4-02` §2(插件布局与 include 面)、`2-01`/`brickie-v0.1` §7(声明面是唯一真值)。
> 相关: ADR-0005(插件管理器; 依赖边与方向表)、ADR-0015(`runtime/posix` 改名)、
> ADR-0016(APP 与 platform 解耦; 那次把 `allow_edges` 清空)、ADR-0017(`[kernel].hz`)。
> 触发: 评审发现 `app/hello` **没有**声明依赖 `runtime/posix`, 却因为用了 `usleep` 而**编过也链过**。

## 1. 背景: 声明面为什么形同虚设

`brickie check` 的 deps 域只校验**已声明**的边(方向表 / api_type 禁则 / allow_edges)。
它不看源码实际用了谁, 于是三个事实叠出漏洞:

1. **包含面是全局并集**: `build.rs` 的 `all_includes()` 把闭包内**所有插件**的 `-I`
   发给**每一次**编译 ⇒ `app/hello` 能 `#include <unistd.h>`(属于 `runtime/posix`), 无需声明;
2. **链接是符号级全局**: 镜像把闭包内所有插件链在一起 ⇒ `usleep()` 也能解析到
   `runtime/posix` 的定义;
3. **`brickie build` 不跑方向表**: 连"声明了但方向非法"这种情况, 也要等 `brickie check`
   才报。

⇒ `app/hello` 用着 `runtime/posix` 的 `usleep`/`pthread_*`/`stdio`, 声明面却写着"零依赖",
两侧都不报错。这违反"声明面是唯一真值"(BRV-D4)的基本前提。

### 1.1 一个更危险的连带发现: glibc 头会兜住常见名

收窄 `-I` 之后**仍不能**指望编译器报"找不到头": 目标工具链自带 glibc,
`#include <unistd.h>` 会被 **glibc 的** `unistd.h` 命中(`-std=c11` 下 `usleep` 被特性宏
隐藏, 表现为 "implicit declaration")。也就是说, 裸机镜像可能**悄悄**编在宿主 libc 的
声明上 —— 比"编不过"更糟。⇒ 必须有**与编译器搜索路径无关**的判据(见 §2.2)。

## 2. 决策

### 2.1 每单元的包含面 = core + 自己 + **声明依赖闭包**

`build.rs` 不再给所有单元发同一份 `-I` 并集, 而是按单元算:

| 单元 | include 面 |
|---|---|
| 插件 `P` | `product.toml [build].core_includes` + `P` 自己的 `[build].includes` + **P 的声明依赖传递闭包内各插件的 includes** |
| `core`(非插件) | `core_includes` + platform 插件的 includes(core.init 阶段③依赖 platform 数据; `br_plat.h`/`br_mmu.h` 是设计内的 core→platform 方向) |
| `build/gen` | `core_includes` + 全部被选插件的 includes(生成物要包含各插件的锚头) |

闭包按 `[[dep]]` 的**声明边**算(所有 kind), 所以"声明依赖"第一次成为**编译期**事实:
唯一命名的头(如 `<br/platform/br_plat.h>`)没声明就是 `fatal error: No such file`。

### 2.2 声明 ↔ 使用交叉校验(check 的 deps 域 + build)

对每个被选插件扫描其 `src/`+`include/` 的 `#include`, 用**文件系统**把头名映射到
**全树**里提供该头的插件(与编译器搜索路径无关; 提供方映射**不看闭包** —— 否则
"include 了闭包外插件的头"这种最严重的情形反而漏报); 落在别人头上而该提供方不在自己的
声明闭包里 ⇒ 报错:

* `brickie check`: deps 域 `BRV-MF-0001`(缺口代用 R-9: 无专属码);
* `brickie build`: 同一份判据在 `load_ctx` 里再兜一次(构建前即失败, 不给"编过了再说"的机会);
* 两处调用**同一个** `model::cross_plugin_includes()`, 口径不分叉。
  (实测: `brickie build` 会先经组合期校验, 报出的就是上一条 `BRV-MF-0001`。) 

局限(如实登记): 逐行识别 `#include`, **不解析注释**(注释里的 `#include` 也算);
不做符号级分析(自己写原型再链接仍拦不住, 见 §5)。

### 2.3 正解边: 新增 `iface-posix`(Interface 薄皮肤)

`runtime/posix` 是 `ability`, 而 `app ✗ ability`(设计 §7.3)⇒ APP 需要的是
**Interface 皮肤**。本刀落地设计 D18 的 `iface-posix`:

```toml
# iface-posix/plugin.toml
[plugin]
plugin_type = "interface"
api_type    = "runtime_adapter"   # 不变量 1 + 3(被再导出单元 runtime/posix#posix 的分类)
phase       = "late"              # interface ⇒ service 形(1-01 §6.2)
[[dep]]
name  = "runtime/posix"
kind  = "init"
phase = "late"
[[export]]
name        = "posix-skin"        # 单元 id 必须唯一: runtime/posix 已占用 "posix"
api_iface   = "runtime_adapter"
form        = "skin"
reexport_of = ["runtime/posix#posix"]
symbols     = ["usleep", "pthread_attr_init", "pthread_create"]
```

APP 侧改成:

```toml
# app/hello/plugin.toml
[[dep]] name = "iface-posix" kind = "runtime" symbol = "usleep"
[[dep]] name = "iface-posix" kind = "init"    phase  = "late"
```

`app/hello/src/main.c` 只 `#include <iface-posix/iface_posix.h>`(皮肤把 `runtime/posix`
的 POSIX 头原样转出; 皮肤里**没有一行函数体**)。于是:

* **合法**: `app → interface` 是方向表里的正解边, `allow_edges` 保持空表;
* **单一真值**: POSIX 实现仍只有 `runtime/posix` 一份; 皮肤只是声明级再导出;
* **可裁剪**: 不选 `iface-posix`/`runtime/posix` 的产品里, APP 若不用 POSIX 就不受影响。

## 3. 与既有机制的边界

| 机制 | 本刀的关系 |
|---|---|
| `[[dep]].symbol` | 仍是**声明面注释性字段**(模型解析, 不做符号级校验); 真正的执法是 §2.2 的 include 扫描 + §2.1 的包含面 |
| `allow_edges` | **不动**: 本刀解决的是"没声明", 不是"方向非法"。方向非法仍由 deps 域报 |
| `[[compat.requires_iface]]` | 未启用(v0.1 只校验 schema); 皮肤与消费者的**版本**协商属 M2 |
| `-nostdinc` | **未加**(§5): 需要 GCC 内部 include 目录(`stdarg.h`), 先不做; glibc 兜底由 §2.2 覆盖 |

## 4. 验证记录

| 判据 | 命令 | 结果 |
|---|---|---|
| **反证**(修复前) | 去掉 app 的 `[[dep]]` 后 `brickie check` | deps 域 2 错: `app/hello include 了 runtime/posix 的头 unistd.h/pthread.h, 但没声明依赖` |
| **反证(编译期)** | 同上 + `brickie build`(`-I` 已收窄) | `FAIL cc app/hello/src/main.c`: `usleep`/`pthread_*` 无声明 —— 声明面成了编译前提 |
| 正证 | `brickie check` | 0 错(加回 `[[dep]] iface-posix` 后) |
| 正证 | `brickie build` | 通过(`build/gen/iface-posix` 进镜像) |
| 门禁 | `brickie test -j4` | 全绿 |

## 5. 遗留(如实登记)

1. **符号级未做**: 若插件自己写函数原型(不 include 提供方的头), 仍可链接到未声明的实现。
   设计侧的"接口依赖扫描(符号级, `truth = "header"`, `hash_scope = "sym"`)"在 `1-04` §1.9
   里本就列为**未做**; 本刀不假装完成它。
2. **`-nostdinc` 未加**: 真正做到"镜像永远看不到宿主 libc 头"应加 `-nostdinc`(并解决
   `stdarg.h` 的来源)。本刀用 §2.2 的扫描覆盖已知面; 加了 `-nostdinc` 之后编译器本身就是
   第二道判据, 那时 §2.2 可退化。
3. **注释里的 `#include`**: 扫描器不解析注释(§2.2 的局限)。当前全树无误报。
4. `iface-posix` 的接口面快照(`api/iface/iface-posix/posix-skin.toml`)由
   `brickie iface publish` 产出; 本刀登记了首次发布。
