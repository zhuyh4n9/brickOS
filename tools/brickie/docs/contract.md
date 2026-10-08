# brickie v0.1 — 进程边界契约(L5 ↔ L0/L1/L2)

> **性质**: 实现契约(implementation contract), 不是设计文档。设计的唯一权威是
> `Design/docs/2-toolchain/brickie/brickie-v0.1.md`(下文 `§x`); 本文件只固定
> **三个进程之间的 JSON 形状、文件归属与裁定**, 使 L5(Python)、L0/L1(Rust)、
> L2(C++) 可以分别实现而不会各自长出第二种真值。
> 凡本文件与设计冲突处, 在 §9「实现期裁定」登记, 并在 `tools/brickie/README.md`
> 的偏差表里可见。

## 1. 三个宿主可执行

| 产物 | 语言 | 层 | 职责 | 环境变量 |
|---|---|---|---|---|
| `brickie` | C++ 启动器 + 嵌入载荷 | L5 入口 | 解包载荷(内含 Python 前端、模板、`brickie-core`、`brickie-gen`)→ 用系统 `python3` 跑前端 | 设 `BRICKIE_GEN` / `BRICKIE_CORE` 指向解包出来的原生工具 |
| `brickie-core` | Rust | L0 模型 + L1 求解/版本/接口引擎 | 全部**判定**与错误码; 一切"机器拥有的文件"的内容 | — |
| `brickie-gen` | C++ | L2 生成器 | 只做**模板渲染**(骨架 / 描述符 / 头文件), 不判定 | — |

**硬纪律**(§9.1 / V-18c): `python/brickie/**` 里**没有**业务规则。允许的参数解析 /
文件编排 / schema 形状校验 / JSON 序列化 / 文本呈现之外的判定, 一律在 `brickie-core`。

## 2. 信封与协议版本

两个原生工具都用**同一个信封**: 一行 JSON 从 stdin 读入, 一行 JSON 从 stdout 写出。

请求:

```json
{
  "protocol": 1,
  "command": "check",
  "root": "/abs/path/to/repo",
  "args": { },
  "context": { }
}
```

响应:

```json
{
  "protocol": 1,
  "status": "ok",
  "exit_code": 0,
  "diagnostics": [ ],
  "files": [ ],
  "data": { }
}
```

| 字段 | 规则 |
|---|---|
| `protocol` | 常量 `1`。请求或对方的版本不等于自己 ⇒ 回 **`BRV-PROTO-0001`**、`exit_code = 2`、`status = "ok"`(协议处理成功, 业务退出码在响应里) |
| `status` | `ok` = 协议处理成功(即使 `exit_code != 0`); `internal_error` = 工具自身故障(L5 抛环境错) |
| `exit_code` | BRV-D9: `0` 成功 / `1` 校验红 / `2` 用法或环境错。**进程退出码恒为 0**(除 `internal_error` 与崩溃) —— "工具故障"与"校验红"必须可分辨 |
| `diagnostics` | 见 §3; 无码诊断用 `"code": null` |
| `files` | 见 §4; 只有 `brickie-core` / `brickie-gen` 产出内容, Python 只落盘 |
| `data` | 命令专有载荷(呈现与 `--json` 用); 无内容时是 `{}` |

**协议握手**: L5 每次调用都带 `protocol`; 原生侧启动后校验。快照/用例里的
`protocol_version` 与工具不一致 ⇒ `BRV-PROTO-0002`(不自动迁移)。

## 3. 诊断

```json
{"code": "BRV-DEP-0009", "severity": "error", "target": "service/crypto",
 "file": "service/crypto/plugin.toml", "span": "dep[0].phase",
 "message": "…", "hint": "…"}
```

* `severity ∈ {error, warning, info}`; **严重度不决定退出码**, 退出码由"命令有没有完成"决定(§4 BRV-D8 注 / G-1)。
* 编号全集 = 设计 BRV-D8 的编码表; **未分配码的新缺陷**按 §9 登记, 不得自造。
* 位置化: `file` 相对 `root`; `span` 用 TOML 路径(如 `export[0].entries[2].sig`)。
* `--json` 下诊断数组按 `(code, file, span, target)` 稳定排序 ⇒ 快照测试可用。

## 4. 文件归属(§9.1.1 写路径纪律)

`files[i] = {"path": "<相对 root>", "kind": "machine"|"rendered"|"human", "content": "<UTF-8>", "mode": "0644"}`

| kind | 谁产出 | 谁落盘 | 例子 |
|---|---|---|---|
| `machine` | `brickie-core` | L5 | `api/iface/<provider>/<unit>.toml`、`api/iface/CHANGELOG.md`、`brickie.lock`、`build/index/dependents.json`、`build/gen/proposals/<unit>.toml` |
| `rendered` | `brickie-gen` | L5 | `build/gen/<plugin>/plugin_desc.c`、`brickie new/init` 的全部骨架文件 |
| `human` | **不产出** | — | `plugin.toml` / `product.toml` 是**只读输入**(§9.1.1 规则 1) |

* **`plugin.toml` / `product.toml` 永不被改写**; 只有 `new` / `init` 在文件**不存在**时创建。
* 落盘由 L5 做: 只在内容与现有文件**逐字节不同**时写(幂等); 写前 `mkdir -p`。
* **可复现**: 同一输入两次运行 ⇒ `files` 逐字节相同。机器文件里**不得出现时间戳 / 随机数 / 主机相关信息**。`CHANGELOG.md` 用"版本段"而不是日期分段。
* `new` 的产物走"两段式": `brickie-core plan-new` 给出**计划**(路径 + 模板 id + 变量), `brickie-gen render` 给出内容, L5 落盘。已存在的人写文件不覆盖 ⇒ `BRV-GEN-0002`。

## 5. `brickie-core` 命令

请求 `args` 除特别说明外都为可选默认。`id` 一律指**接口单元** `<provider>#<unit>`;
`plugin` 指插件名。

### 5.1 只读

| `command` | `args` | `data` 关键字段 |
|---|---|---|
| `version` | `{}` | `{version, protocol, languages:[{name,version}], deps:[]}`(G-01 `--version --deps` 的指纹) |
| `model` | `{plugin?}` | `{plugins:[Plugin], count}` — 规范化模型 |
| `check` | `{profile:"dev"\|"release", scopes:["deps","iface","tax","priv"], plugin?}` | `{summary:{errors,warnings,infos,profile,closure:{plugins,ram_kib,stack_kib}}, scope_results}` |
| `closure` | `{profile}` | `{plugins:[{name,version,compat_gen,api_type,plugin_type,phase,ram_kib,stack_kib,selected}], init_edges:[{from,to}], runtime_edges, type_edges, topo_order:[], totals:{ram_kib,stack_kib,platform_capacity:{ram_kib,stack_kib},budget:{ram_kib,stack_kib}}, profile, cycles:[]}` |
| `dep-tree` | `{kind:"init"\|"runtime"\|"type"\|"all", plugin?}` | `{roots:[], nodes:{name:{deps:[{name,kind,range,phase}]}}, kind}` |
| `dep-graph` | `{format:"dot"\|"mermaid"\|"json"}` | `{format, nodes:[], edges:[], text}`(`json` 时 `text` = 规范化 JSON 字符串) |
| `dep-why` | `{from, to}` | `{found, path:[name], edges:[{from,to,kind}]}` |
| `dep-index` | `{}` | `{dependents:{<plugin>:[{from,kind}]}}` + `files:[build/index/dependents.json]` |
| `ver-show` | `{id}` | `{id, plugin, version, compat_gen, segments:{compat_gen,major,minor,revise}, freeze_state, status, source:"snapshot"\|"declaration", compat_gen_source:{kind,note?}, published:bool}` |
| `iface-list` | `{}` | `{units:[{id,provider,unit,api_iface,form,version,compat_gen,freeze_state,status,published,hash}]}` |
| `iface-show` | `{id}` | `{unit:UnitSnapshot, entries:[Entry], source:"declaration"\|"snapshot", not_abi:true, truth, hash_scope, declaration_hash, snapshot_hash, published}` |
| `iface-diff` | `{id}` | `{id, changes:[Change], verdict, compat_gen_changed, version:{from,to,reasons:[]}, dependents:{direct:[],transitive:[],unsatisfied:[]}}` |
| `iface-status` | `{id, check:bool}` | `{id, recomputed_hash, snapshot_hash, declaration_hash, consistent:bool, files:[若需回写]}` |

### 5.2 写入面(产出 `files`)

| `command` | `args` | 落盘文件 | 关键规则 |
|---|---|---|---|
| `plan-new` | `{plugin_type,name,api_type,lang,subkind,force}` + `context.existing:[{path,first_line}]` | 无(计划) | 名字契约/subkind 推导/相位推导/模板可得性; 返回 `data.artifacts:[{path,kind:"human"\|"generated",template,vars}]` |
| `plan-init` | `{name, app?}` | 无(计划) | `product.toml` + `app/<name>/` + `brickie.lock` 初版 |
| `gen-plan` | `{plugin?}` | 无(计划) | 重建 `build/gen/**` 的计划(描述符 + 头文件) |
| `dep-add` | `{plugin, dep, range?, kind:"init"\|"runtime"\|"type", phase?}` | **无**(裁定 R-2: 打印 TOML 片段, 不改 `plugin.toml`) | `data.snippet` = 可直接粘进 `plugin.toml` 的 `[[dep]]` 块 |
| `dep-rm` | `{plugin, dep, kind?}` | **无** | `data.snippet` = 待删除块的定位 |
| `ver-bump` | `{id, rule:"major"\|"minor"\|"revise"}` | 单元快照 | §5.2 推进表; `compat_gen` **不在** rule 里; 单调不回退(`BRV-VER-0007`) |
| `iface-publish` | `{id, check:bool, note?, set?, profile}` | 快照 + `CHANGELOG.md` + `brickie.lock` | 面未变 ⇒ **空操作**(无 files、`exit_code` 0); `--profile release` 跑发布前门钩 |
| `iface-freeze` | `{id, entry?, note}` | `build/gen/proposals/<unit>.toml` | **A-26 例外**: v0.1 只出"待升格提案", 不落 `frozen` 快照、不 bump `compat_gen` |
| `iface-deprecate` | `{id, entry?, note}` | 快照 | 状态机 `BRV-IFACE-*`; 须 `--note` |
| `iface-undeprecate` | `{id, entry?, note}` | 快照 | 同上 |
| `iface-unfreeze` | `{id, note}` | 快照(记 baseline) | 无 `--note` ⇒ 红; 非 `frozen` ⇒ 红 |
| `iface-refreeze` | `{id, note?}` | 快照 | 有"改/删已冻结条目" ⇒ `COMPAT_GEN+1`; 空解冻 ⇒ 四段全不动 |

### 5.3 `selftest`

`brickie-core --selftest`(不经 stdin 信封)在**不加载 Python** 的前提下跑全部领域用例
(V-18a): 模型 / 版本矩阵 / IFACE-IR / 求解 / 分类学 / 特权 / 预算 / 解冻窗口。
退出码 0 全绿 / 1 有失败; 失败明细打到 stdout。

## 6. `brickie-gen` 命令

| `command` | `args` | `files` |
|---|---|---|
| `render` | `{templates_root, artifacts:[{path, template, vars}]}` | `[{path, kind:"rendered", content, mode}]` |
| `--selftest`(不经信封) | — | JSON 往返 / 名字契约 / 渲染 / 路径布局的自检 |

* 模板目录布局(§9.2 + 裁定 #5): `templates/<api_type>/<plugin_type>/<lang>/`(骨架)、
  `templates/descriptor/<api_type>/<lang>/`(描述符)、`templates/product/<lang>/`(product 骨架)。
* 渲染器**没有条件语法**(§9.2): 该分化的地方用目录分化; `vars` 只做占位符替换。
* 占位符语法: `{{name}}`; 缺失变量 ⇒ 渲染失败(`BRV-GEN-0002` 同族用法错)。
* 生成物首行标记 `brickie:generated`(识别"是不是本工具的生成物")。

## 7. 声明面 schema(v0.1 权威字段表)

字段形状由 `schema/*.schema.json` 描述(BRV-D2: 只表达**单条记录形状**);
**跨字段 / 跨文件不变量只由 `brickie-core` 报码**。TOML 是唯一表达格式。

### 7.1 `plugin.toml`

```toml
schema = 1
[plugin]
name        = "service/crypto"    # ^[a-z][a-z0-9]*(?:[-_/][a-z0-9]+)*$  全局唯一
plugin_type = "ability"           # app | interface | ability | platform
api_type    = "native"            # native | runtime_adapter | third_party
subkind     = "service"           # scheduler | framework | io | fs | service(ability 专用)
lang        = "c"                 # c | cxx | rust
phase       = "late"              # early | core | late | app(第 ② 个完成点, §7.2)
sched_class = "SAFE_PREEMPT"      # SAFE_PREEMPT | COOP_ONLY | TT_SAFE(缺省 SAFE_PREEMPT)
sched_kind  = "coop"              # 仅 subkind=scheduler: coop | preempt | tt(裁定 R-5)
version     = "0.1.0.0"           # COMPAT_GEN.MAJOR.MINOR.REVISE
summary     = "…"                 # 可选
license     = "WTFPL"             # 可选

[[res]]                            # 可多条
kind = "ram"                       # ram | stack
used_kib = 48
notes = "…"                        # 可选

[sched.tt]                         # 仅 sched_class = "TT_SAFE"
period_us = 1000
deadline_us = 800
wcet_us = 200

[compat]
core       = ">=1.0.0"             # range(3 段)
api_rev    = 1                     # 可选
hash_scope = "decl"                # v0.1 只允许 decl
truth      = "decl"                # v0.1 只允许 decl(与 hash_scope 成对)
abi_id     = ""                    # 可选占位

[[compat.requires_iface]]          # v0.1 只校验 schema, 不参与求解(V-10)
id = "framework/vfs-core#file"
api_iface = "native"
compat_gen = 3
range = ">=1.2.0"
mode = "decl"

[iface.typedefs]                   # 别名 → 规范名(IFACE-IR 规则 4)
u32 = "uint32_t"

[[dep]]
name  = "platform/qemu-aarch64"
range = ">=0.1.0"                  # init/runtime/type 都**不带 compat_gen**(F3)
kind  = "init"                     # init | runtime | type
phase = "early"                    # 可选断言(仅 kind=init)
symbol = "br_open"                 # 可选(仅 kind=runtime)

[[export]]                         # 接口单元 = 冻结与版本的基本粒度
name         = "crypto"            # 单元名 / 注册表名
api_iface    = "native"            # 必须 == [plugin].api_type(TAX-0016)
form         = "api"               # api | skin | service
version      = "0.1.0.0"           # 人维护的声明意图; 快照是发布记录
compat_gen   = 0
freeze_state = "unfrozen"          # unfrozen | frozen | unfreezing
hash         = ""                  # 发布后由工具写; 与快照不等 ⇒ IFACE-0012
status       = "experimental"
reexport_of  = []                  # 仅 form=skin: ["<provider>#<unit>", …]
symbols      = []                  # 仅 form=skin

[[export.entries]]                 # 面(surface)的成员
kind   = "func"                    # func | var | macro | type | enum | service | symbol-family
name   = "br_crypto_hash"
sig    = "int(const uint8_t*, size_t, uint8_t*)"   # func: 类型与顺序, **不含参数名**
layout = "# opaque"                # type
value  = "16"                      # macro/var/enum(enum 用成员表 ["A","B"])
ops    = ["open","close"]          # service
status = "experimental"

[privileged]
level = "P2"
[[privileged.memory]]
granularity = "pool"
ops = ["alloc","free"]
regions = ["heap","contig","page"]
[[privileged.resources]]
irq = [32]
dma_channels = [3]
pins = []
device_names = ["hsm0"]

[build]
sources = ["src/*.c"]
includes = ["include"]
```

### 7.2 `product.toml`

```toml
schema = 1
[product]
name = "hsm"; version = "0.1.0.0"; app = "app/hsm"; core = ">=1.0.0"
stage = "dev"                      # dev | release(profile 默认值, CLI 覆盖)
[select]
plugins = ["platform/qemu-aarch64", "service/crypto"]
[budget]
ram_kib = 512
stack_kib = 16
[lint]
frozen_deps = "inherit"            # inherit | allow | deny
allow_edges = [["framework/cdev-core","framework/dev-core"]]
```

### 7.3 机器文件

* `api/iface/<provider-short?>/…`: 目录用**插件名的路径段**(`service/crypto` ⇒ `api/iface/service/crypto/<unit>.toml`)。
* 快照头必须带 `hash_scope` / `truth` 与 `NOT_ABI` 提示(RV-3/V-11)。
* `brickie.lock` / `build/index/dependents.json` 的形状见 `schema/lock.schema.json` / `schema/index.schema.json`。

## 8. L5(Python)的边界

**允许**: argparse / 找二进制 / 调子进程 / 读写文件 / JSON 序列化 / 文本呈现 /
按 `schema/*.json` 做**单条记录形状**校验(BRV-MF-0001, 退出码 2) / 退出码汇总。

**禁止**(V-18c 的静态检查会抓):
* 闭包 / 区间交集 / 拓扑 / 环 / 相位单调 / 分类学 / 特权 / 预算的任何判定;
* 四段版本推进与 `range` 解析;
* IFACE-IR 规范化 / hash / 变更集;
* 任何"机器文件"的**内容**拼接(含 lock、快照、CHANGELOG、index)。

**呈现纪律**: 文本输出与 `--json` 是**同一 `data`** 的两个序列化(§BRV-D2 纪律 2)。

## 9. 实现期裁定(R-*: 规格有缺口或两种读法, 实现先选一个并留切换点)

| # | 缺口 / 歧义 | 采纳 | 理由 / 切换成本 |
|---|---|---|---|
| R-1 | `brickie check` 没有 `product.toml` 时怎么办 | **校验整棵插件树**(逐插件 + 树内边); 产品级数量约束跳过 | V-1 要求"生成的插件立刻通过 check"; 有 product 时按闭包全量判 |
| R-2 | §7.7 说 `dep add/rm` "写 `plugin.toml`", §9.1.1 规则 1 说**永不改写**人写文件 | 取 **§9.1.1**(后出的显式决策): 只**打印**片段, 不写盘 | 写库会抹掉注释且违反唯一真值; 切换成本 = 加 `--write` |
| R-3 | `ver bump <plugin>`(§7.7)vs 版本载体是**单元快照**(§9.1.1 规则 2) | 参数接受**插件名或 `<provider>#<unit>`**; 插件名要求恰一个 `[[export]]`; 写入快照, 并把"`plugin.toml` 里应改成什么"作为 info 报出 | 人写 vs 发布记录的语义不混 |
| R-4 | §6.1 `value`/`ops` 对 `macro`/`var`/`enum`/`service` 的具体形状 | `value` = 字符串或字符串数组; `ops` = 字符串数组 | 二者必须进 hash(规则 9) |
| R-5 | 调度器策略类别 `sched_kind` 无字段, 而 §2 第 3 项要判 `COOP_ONLY` 冲突 | 新增可选 `[plugin].sched_kind`(仅 `subkind=scheduler`); 缺省按名字推断(`sched-coop`/`sched-preempt`/`sched-tt`) | 否则该项无法执法; 缺口登记 |
| R-6 | `platform 提供量`(§8.1 注)无字段 | `plugin_type="platform"` 的插件, 其 `[[res]]` 视为**capacity** | 不新增字段; 缺口登记 |
| R-7 | 解冻窗口内"是否改/删了已冻结条目"没有基线 | `unfreeze` 在快照里写 `baseline`(面 hash + 条目表); `refreeze` 与它比 | 没有基线则该判定无输入(缺口登记) |
| R-8 | `publish` 的 `--set` 语义(`§6.5` 只见于 `ver`?) | `iface-publish --set <4 段>` 覆盖**本次要写入的单元版本**(仍受单调约束) | — |
| R-9 | 资源独占冲突 / 预算超限 / 调度冲突没有专属码 | 用 `BRV-MF-0001`(声明面错误, 消息里点名**冲突双方/差值**) | BRV-D8 未分配码; 缺口登记, 待补码 |
| R-10 | `dep-add --range` 缺省 | `*`(仍受 `compat_gen` 约束不适用: 结构依赖不钉代) | — |
| R-11 | 快照目录: `<provider>` 是插件全名还是短名 | 用**插件全名的路径段**(`service/crypto` ⇒ `api/iface/service/crypto/`), 单元文件名 = `<unit>.toml` | 与 §5.6 的 `<provider>/<unit>` 一致 |
| R-12 | 无时间戳的 `CHANGELOG.md` | 按"版本段"追加, 段头 = `<unit> v<4 段> (compat_gen=<N>)`, 无日期 | 逐字节可复现(§4) |

> **Rust 侧的补充裁定**: 求解/校验/接口引擎在实现中还被迫落笔了一批更细的口径
> (S-1…S-19: 首次 publish = 建档不推进段、`closure` 不扫描 `requires_iface` 而 `check` 扫描、
> platform 的 `[[res]]` 作容量不进消费者 Σ、`DEP-0011` 的判据、R2 的取值口径、
> `app → third_party ability` 的例外、窗口内 publish 消费基线以免双 bump 等)。
> 它们是**引擎内部口径**, 逐条登记在 `rust/README.md` §10; 本文件的 R-* 是其上位清单。
