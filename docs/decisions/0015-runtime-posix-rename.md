# 0015 — POSIX 运行时的名字与命名空间: `service/svc-posix` → `runtime/posix`

> 状态: **已落地**(QEMU virt aarch64; `[POSIXCONF] 20/0` 不变; `brickie test -j8` 全量门禁绿)。
> 影响面: **插件改名 + 迁目录** —— `service/svc-posix/**` → `runtime/posix/**`
> (源文件 `src/svc_posix.c` → `src/posix.c`; 钩子 `svc_posix_*` → `posix_*`);
> `tools/brickie/rust/src/rules.rs` + `selftest.rs`(**新增 `runtime` 命名空间**);
> `product.toml` / `tests/gates.toml` / `README.md` / `WORKAROUNDS.md` / 各插件与 core 的
> 注释指称; `api/iface/service/svc-posix/` → `api/iface/runtime/posix/`(旧快照删除,
> 新单元 `runtime/posix#posix` 发布); `brickie.lock`。
> 前序: **ADR-0014**(那个插件的内容本身)、ADR-0012(fd 表与 errno)。
> 设计依据: `1-01` §6.3/§7(D18 的 POSIX 双角色)、`4-02` §2(命名空间集合)、
> `brickie-v0.1` §8.3 ①(名字契约)。
> **设计侧仍用 `svc-posix` 这个名字** —— 本 ADR 只改原型树的插件身份, 两者的收口见 §4。

## 1. 背景: 用户裁定

原型的 POSIX 运行时原名 `service/svc-posix`(照设计 D18 的口径)。用户裁定:

> "svc-posix 改为 runtime-posix, 并且放在 runtime 目录下"

## 2. 决策

### 2.1 全名取 `runtime/posix`, 目录 `runtime/posix/`

**为什么不是字面上的 `runtime-posix`**: 本仓有一条**硬约束** —— `brickie` 的插件目录
**必须等于插件全名**(`build.rs` 的 `plugin_dir_of(name) = name`, 由 `[build].sources`
的相对路径解析与 `[[dep]]` 的闭包定位共用)。于是:

| 想要的目录 | 可行的全名 |
|---|---|
| `runtime/` 下 | 必须是 `runtime/<short>` |
| 名字里含 `runtime-posix` | 目录必须是 `runtime-posix/`(不在 `runtime/` 下) |

两条不能同时满足字面要求。裁定取**前者**(用户的"放在 runtime 目录下"是显式的布局要求),
`<short>` 取 `posix` ⇒ **全名 `runtime/posix`**, 落 `runtime/posix/`。
口语/文档里称它"runtime-posix"没有歧义 —— 但**机器读的名字只有一个**: `runtime/posix`。

### 2.2 `plugin_type` / `subkind` / 相位**都不变**

| 字段 | 值 | 为什么不动 |
|---|---|---|
| `plugin_type` | `ability` | 它是能力插件; `runtime` 只是**命名空间**(身份/目录约定), 不是第五个 `plugin_type` |
| `subkind` | `service` | ★ 相位推导靠它: `is_service_like(ability, service)` ⇒ **LATE**。换成新 subkind(如 `runtime`)会让 `init` 掉到 **CORE** —— 那时 core 的堆/调度/服务注册表还没就绪, POSIX 面会被提前调用 |
| `api_type` | `runtime_adapter` | D18 的分类(`iface-posix` 将来以 `form = "skin"` 再导出它的单元, 分类必须相等) |
| `phase` | `late` | 由上一条推出(类别决定, 不是自选) |

⇒ **命名空间与类别是两个维度**: `runtime/` 表达"这是一整套运行时", `subkind = service`
表达"它在相位模型里是服务"。把两者绑在一起会为了一个目录名改掉相位语义, 那是本末倒置。

### 2.3 `runtime` 进命名空间集合(brickie 侧)

`rules.rs` 的 `NAMESPACES` 追加 `"runtime"`, 并给出映射:

```rust
namespace_plugin_type("runtime") => Some("ability")
namespace_subkind("runtime")     => Some("service")
```

**影响面很小, 但是必须做**: `NAMESPACES` 只被"推荐形态 lint"(`BRV-TAX-0013`, `brickie new`
的 warning)与诊断 hint 读, **`check` 不拿它判红**。不做这一步, `brickie new runtime/posix`
会报一条"不符推荐形态"的 warning —— 一条**假的**警告比没有警告更糟(它会训练人忽略警告)。
`brickie-core --selftest` 里补了三条判据(推荐形态 / `runtime⇒ability` / `runtime⇒service`)。

### 2.4 符号前缀随之变化(生成物契约)

`short = "posix"` ⇒ `symbol_prefix = "posix_"`, 于是四个生命周期钩子变成:

| 旧 | 新 |
|---|---|
| `svc_posix_early_init` / `_init` / `_start` / `_selftest` | `posix_early_init` / `posix_init` / `posix_start` / `posix_selftest` |

**这不是"换个名字"那么轻**: 钩子名是**生成物**(`build/gen/<plugin>/plugin_desc.c`)按
`symbol_prefix` 推导并引用的, 定义侧改完必须 `brickie gen` 重生成, 否则链接期报
`undefined reference`(实测踩到)。改名清单因此是"声明面 → 生成物 → 源码"三步, 缺一步就红。

### 2.5 旧产物必须手工清(工具不清)

`build/gen/**` 与 `build/obj/**` 里**旧名的残留**不会被 `brickie gen` / `build` 回收
(`gen_sources = ["build/gen/**/plugin_desc.c"]` 是 glob ⇒ 旧文件仍会被编进镜像)。
实测症状: 链接期 `undefined reference to svc_posix_early_init` —— 镜像里**同时**编进了
新旧两份描述符。裁定: 本刀手工删 `build/gen/service/svc-posix/` 与
`build/obj/{build/gen/service/svc-posix,service/svc-posix}/`;
**"`gen` 应回收已消失插件的生成物"登记为工具侧欠账**(见 §4.4, 与 ADR-0012 §4.4 的
golden include 可见性同属"组合器要补的一课")。

### 2.6 接口单元改名: `service/svc-posix#svc-posix` → `runtime/posix#posix`

- `[[export]].name` 从 `svc-posix` 改为 `posix`(单元名 = 插件内唯一, short 口径);
- 旧快照 `api/iface/service/svc-posix/` **删除**, 新单元发布为
  `api/iface/runtime/posix/posix.toml`(hash `sha256:b8697b3b…`);
- `api/iface/CHANGELOG.md` 里旧名那条"首次发布(建档)"**删除** —— 那个单元**从未进过任何
  提交**(它是本会话未提交的中间态), 留一条指向不存在单元的记录只会误导;
- `brickie.lock` 由 `iface publish` 重新生成。

## 3. 落地清单与判据

| 面 | 落点 |
|---|---|
| 插件身份 | `runtime/posix/plugin.toml`(`name = "runtime/posix"`, 单元 `posix`) |
| 源码 | `runtime/posix/src/posix.c`(原 `svc_posix.c`)+ `src/posix_selftest.c`; 钩子 `posix_*` |
| 命名空间 | `tools/brickie/rust/src/rules.rs`(`NAMESPACES` + 两个映射)、`selftest.rs`(3 条判据) |
| 声明面 | `product.toml` 的 `[select]`; `tests/gates.toml` 的 `posix-test`(require 里的启动行) |
| 接口 | `api/iface/runtime/posix/posix.toml`(旧 `api/iface/service/svc-posix/` 删除) |
| 指称 | 39 个文件里的 `svc-posix`/`svc_posix` → `runtime/posix`/`posix`(设计侧真实文件名 `11-02-svc-posix-subset.md` 保留) |

**验证(实测)**

```
$ brickie-core --selftest        # ok 495 cases(含新增的 3 条 runtime 命名空间判据)
$ brickie check                  # 0 错 0 警; 闭包 16 个插件, RAM 40 KiB
$ brickie test -j8               # 26 步全绿(宿主 7 + QEMU 8 + 脚本 4)
  [POSIXCONF] SUMMARY pass=20 fail=0 total=20      ← 改名后一字未改
$ brickie iface status runtime/posix#posix          # 一致(hash sha256:b8697b3b…)
$ brickie iface list | grep -c svc-posix            # 0
```

## 4. 未做 / 留给下一刀

1. **设计侧的收口**(属设计仓库, 本 ADR 只登记)。设计仍写 `svc-posix`:
   - `1-01` D18 的决策行、`11-01` §1/§2 的服务清单、`1-03` §1 的插件清单、`10-01` §1/§2、
     `4-01` §6.3 的路由表、`brickie-v0.1` §8.3 的命名空间表;
   - **`br-svcposix.txt`**(`3-01` §15 第五批的 golden 名)—— 改不改要与"golden 组名规则"
     (设计 ADR-0001)一起拍;
   - `6-01` 需新增的 POSIX 用例组名(`TC-POSIX-*` 与插件新名无关, 可留)。
   ⇒ **建议**: 设计侧要么把 D18 与相关篇目的名字改为 `runtime/posix`(并给 ADR-0001 的 golden
   组名一个 `br-runtimeposix` 或 `br-posix`), 要么在 D18 上加一条"**修订**: 原型侧实现名为
   `runtime/posix`"的交叉引用 —— **两条都要做一次, 别让两棵树各说各话**。
   本刀的取舍是: 不为一个**改名**去改写一份**决策记录**(D18 是历史事实), 而是把差异显式登记。
2. **`runtime/` 命名空间的语义**只在 brickie 与 ADR 里定成"整套运行时 ⇒ ability/service";
   设计 `4-02` §2 的命名空间集合仍无它。
3. **没有第二个 `runtime/` 插件** ⇒ 这个命名空间现在只有一件住户, 是"为将来分类"还是
   "过度设计"要看后续(`service/` 与 `runtime/` 的边界判据尚无人写)。
4. **工具侧欠账(两件, 都与组合器的"生成物生命周期"有关)**:
   - `brickie gen` **不回收**已消失插件的生成物(§2.5)⇒ 改插件名/删插件必须手工清 `build/`;
   - 依赖方拿不到被依赖插件的 `include/`(`runtime/posix` 的 POSIX 头在别的插件里链不上)——
     `iface-posix` 的前置, 登记在 `11-02` 的 Q-9。
