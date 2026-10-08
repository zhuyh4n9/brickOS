# `brickie` Python 前端(L5)—— 模块地图与命令状态

> 本文件是 **L5(Python3 前端)** 的说明书: 模块划分、命令状态、与 `brickie-core` /
> `brickie-gen` 的调用点、零依赖说明、剩余 TODO 与契约缺口。
> 进程边界契约: [`docs/contract.md`](../../docs/contract.md); 设计权威:
> `Design/docs/2-toolchain/brickie/brickie-v0.1.md`; 命令面基线:
> `Design/docs/2-toolchain/brickie/checklist.md` §5.1。
> 整个工具(含 C++/Rust/构建)的总览在 [`../../README.md`](../../README.md)。

---

## 1. 模块地图

```
python/brickie/
├── __init__.py   版本号与 PROTOCOL(信封版本 = 1)
├── __main__.py   `python -m brickie` 入口
├── cli.py        23 个叶子命令 + 2 个全局开关的 argparse 与**编排流水线**(§3)
├── native.py     定位并调用 brickie-core / brickie-gen; 信封组装与响应校验(§2)
├── schema.py     自带最小 JSON-Schema 子集 → BRV-MF-0001 / 退出码 2(§4)
├── writer.py     幂等落盘 / `--check` 逐字节比对 / 声明面扫描(§4)
├── present.py    把 data 渲染成人读文本; `--json` 与文本**同源**(§5)
└── hostinfo.py   宿主三元组(build/host/<arch>/<os> 的坐标; 与 tools/host-detect.sh 同口径)

> schema(`../schema/*.schema.json`)不再做包内镜像 —— 它已由 `freeze.py` 的
> `PAYLOAD_ROOTS` 直接收进入口 ELF 载荷, 见 §4.3。
```

**硬纪律**(contract §8 / V-18c): 本包**没有业务规则**。闭包、版本区间、拓扑、环、
相位单调、分类学、特权、预算、四段版本推进、IFACE-IR 规范化与 hash、任何"机器文件"
的内容拼接, 一律在 `brickie-core`。本包只做允许的六件事: 参数解析 / 找二进制 /
调子进程 / 读写文件 / JSON 序列化 / 按 schema 做**单条记录形状**校验。

### 编排流水线(每条命令同一条)

```
可选 schema 形状校验(BRV-MF-0001 ⇒ 退出码 2)
  → brickie-core <命令>            (判定 + data + files; exit_code)
  → 若 data.artifacts: brickie-gen render   (只渲染, 不判定)
  → 落盘(幂等; --check 只比对 ⇒ 不一致退出码 1; 人写文件永不覆盖)
  → 呈现(--json 与文本是同一 data 的两个序列化)
退出码汇总: 2(用法/环境) > 1(校验红) > 0
```

---

## 2. 与 `brickie-core` / `brickie-gen` 的调用点

统一信封(contract §2): 一行 JSON 从 **stdin** 进, 一行 JSON 从 **stdout** 出。
进程退出码恒为 0; 业务退出码在响应的 `exit_code` 里; 非 0 或协议不符 ⇒
`NativeError`(L5 退出码 2, 协议不符带码 `BRV-PROTO-0001`)。

### 2.1 二进制定位顺序(`native._find_tool`)

1. `$BRICKIE_CORE` / `$BRICKIE_GEN`(显式覆盖; 入口 ELF 解包件走这里)
2. `build/host/<host-arch>/<host-os>/bin/<name>`(本机构建落点; **优先于种子**)
3. `build/host/*/*/bin/<name>`(宿主三元组口径万一不一致时的兜底)
4. `prebuilts/seed/brickie/<host-arch>/<host-os>/bin/<name>`(自举种子)
5. `prebuilts/seed/brickie/*/*/bin/<name>`
6. `PATH`

### 2.2 命令 → core 命令 → 关键 `data` 字段

| CLI | `brickie-core` 命令 | 请求 `args` | 用到的 `data` / `files` |
|---|---|---|---|
| `new <ptype> <name>` | `plan-new`(调两次, 见 §2.3) | `plugin_type,name,api_type,lang,subkind,force` + `context.existing` | `data.artifacts[]` → gen render; 落盘 |
| `init <product> [--app]` | `plan-init` | `name,app` + `context.existing` | `data.artifacts[]` → gen render; `data.lock_content` → `brickie.lock`(§2.4); 落盘 |
| `gen [--check]` | `gen-plan` | `check`(透传) | `data.artifacts[]` → gen render; 落盘/比对 |
| `dep add <p> <d>[@r]` | `dep-add` | `plugin,dep,range?,kind?,phase?` | `data.snippet`(**不写盘**, 裁定 R-2) |
| `dep rm <p> <d>` | `dep-rm` | `plugin,dep,kind?` | `data.snippet`, `data.locator` |
| `dep tree [--kind]` | `dep-tree` | `kind?` | `{roots,nodes,kind}` |
| `dep graph --format` | `dep-graph` | `format` | `{format,nodes,edges,text}` |
| `dep why <a> <b>` | `dep-why` | `from,to` | `{found,path,edges}` |
| `dep index` | `dep-index` | — | `data.dependents` + `files[build/index/dependents.json]` |
| `dep closure` | `closure` | — | `{plugins,topo_order,totals,cycles,profile,…}` |
| `check` | `check` | `profile,scopes` | `{summary,scope_results}` |
| `ver show <id>` | `ver-show` | `id` | `{segments,compat_gen_source,freeze_state,…}` |
| `ver bump <id> --rule` | `ver-bump` | `id,rule` | 单元快照 `files[]` + 版本四段 |
| `iface list` | `iface-list` | — | `{units[]}` |
| `iface show <id>` | `iface-show` | `id` | `{unit,entries,hash_scope,truth,not_abi,…}` |
| `iface diff <id>` | `iface-diff` | `id` | `{changes,verdict,version,dependents}` |
| `iface status <id> [--check]` | `iface-status` | `id,check` | `{consistent,recomputed_hash,snapshot_hash}`(+可能回写) |
| `iface publish <id> …` | `iface-publish` | `id,check,note?,set?,profile?` | 快照 + `CHANGELOG.md` + lock; 面未变 ⇒ 空操作 |
| `iface freeze <id>[#e] --note` | `iface-freeze` | `id,entry?,note` | 待升格提案 `build/gen/proposals/<unit>.toml`(A-26 例外) |
| `iface deprecate/undeprecate` | `iface-deprecate` / `iface-undeprecate` | `id,entry?,note` | 快照 |
| `iface unfreeze <id> --note` | `iface-unfreeze` | `id,note` | 快照(记 baseline) |
| `iface refreeze <id> [--note]` | `iface-refreeze` | `id,note?` | 快照(可能 bump COMPAT_GEN) |
| `--version [--deps]` | `version` | — | `{version,protocol,languages[],deps[]}` |

`--check` 出现在三处, 语义分两层: **`gen --check`** 由 L5 逐字节比对(不写盘);
**`iface status|publish --check`** 透传给 core(core 做独立重算), L5 只保证不落盘。

### 2.3 `new` / `init` 的两段式

`plan-new` 的 `context.existing` 需要 L5 先知道"会写哪些路径", 所以:

1. 用空 `context` 探一次 `plan-new` → 拿到 `data.artifacts[].path`;
2. `writer.scan_existing` 读这些路径的**首行**(生成物标记在首行) → `context.existing`;
3. 用 `context.existing` 再调一次 `plan-new`(这一次的诊断/退出码是权威的);
4. `brickie-gen render` 渲染 → 落盘。

`init` 同形(`plan-init`); contract §5.2 没给 `plan-init` 的 `context`, 这里按 `new`
的同一纪律补上(多余字段被忽略, 但"已存在的人写文件不覆盖"因此在 core 侧也有输入)。

`gen render` 的入参按 contract §6: `args = {templates_root, artifacts:[{path,template,vars}]}`;
请求顶层另带一份首刀遗留的 `templates_root`(供尚未迁到新契约的实现读取)。

### 2.4 core 直传"机器文件"正文(lock)

`plan-init` 的计划里有 `brickie.lock`, 但它是 **`kind = "machine"` 且 `template = ""`** ——
机器文件的正文归 core(contract §4), L5 不能自己拼。当前 core 的实现把正文放在
`data.lock_content`, 于是 `cli._collect_files` 按路径→键的映射
(`_CORE_CONTENT_KEYS = {"brickie.lock": "lock_content"}`)取正文落到计划里的路径上:

* 有非空 `template` 的产物 → `brickie-gen render`;
* 没有 `template` 的产物 → 只认 `artifact.content` 或 `data.<映射键>` 的**直传正文**;
* 两者都没有 ⇒ 一条 **info** 诊断(不静默吞掉, 也不让 L5 变成内容生产者)。

这条映射是 rust 侧 README「与 contract 的偏差」里记的通道(contract §5.2 只写了"计划"),
core 定型后若改成模板或写进 `files`, 删掉这一个映射即可(单点切换)。

---

## 3. 命令状态

**已交付(前端全量)**: 23 个叶子命令 + 2 个全局开关, 全部 `--help` 可见、可跑通
编排/落盘/呈现/退出码。`checklist` §5.1 的 C-01…C-23 与 G-01/G-02 在 **L5 侧**逐条落地。

| 组 | 命令 | L5 前端 | 依赖的原生侧 |
|---|---|---|---|
| 骨架 | `new` | ✅ | `core plan-new` + `gen render` |
| 骨架 | `init` | ✅ | `core plan-init` + `gen render` |
| 骨架 | `gen [--check]` | ✅ | `core gen-plan` + `gen render` |
| 依赖 | `dep add` / `dep rm` | ✅(只打印片段) | `core dep-add` / `dep-rm` |
| 依赖 | `dep tree` / `dep graph` / `dep why` | ✅ | `core dep-tree` / `dep-graph` / `dep-why` |
| 依赖 | `dep index` | ✅ | `core dep-index` |
| 依赖 | `dep closure` | ✅ | `core closure` |
| 校验 | `check [--deps\|--iface\|--tax\|--priv\|--all] [--profile]` | ✅ | `core check` |
| 版本 | `ver show` / `ver bump` | ✅ | `core ver-show` / `ver-bump` |
| 接口 | `iface list/show/diff/status` | ✅ | `core iface-list/show/diff/status` |
| 接口 | `iface publish` | ✅ | `core iface-publish` |
| 接口 | `iface freeze/deprecate/undeprecate/unfreeze/refreeze` | ✅ | 同名 core 命令 |
| 全局 | `--version [--deps]` / `--json` / `--root` | ✅ | `core version`(`--version` 在 core 缺席时降级) |

**明确不做**(checklist §5.4; 一条都不出现在 `--help` 里): `build` / `test` / `run` /
`api-dump` / `verify` / `iface check` / `dbg` / `pack` / `add` / `show` / `env`。

**退出码**(BRV-D9): `0` 成功 / `1` 校验红(含 `gen --check` 不一致) / `2` 用法或环境错
(参数非法、schema 形状不符、`--root` 不存在、原生件缺失或协议不符)。

---

## 4. schema 形状校验(BRV-D2)

### 4.1 允许集与零依赖

`schema.py` **自带最小 JSON-Schema 子集**, 只用标准库(**不引入 `jsonschema`**, 守 §9.3
的"零第三方依赖"纪律)。支持的关键字(够用即止, 见模块 docstring):

```
type / properties / required / items / enum / oneOf /
additionalProperties / minimum / pattern
```

这是一份**更小的允许集** —— 不是"JSON-Schema 的完整实现", 也**不打算**成为: 它只需
覆盖 `schema/` 下的四份文件。若将来要支持 `$ref`/`if-then`/`format` 等, 也应先在
contract §9 登记, 而不是悄悄扩大 L5 的判定面。

**故意不写进 schema 的东西**: 版本串段数(VER-0005)、`range` 语义(VER-0006)、
`api_iface == api_type`(TAX-0016)、`compat_gen` 必填(VER-0003)、闭包与相位。
这些是**跨字段/跨文件不变量**, 由 core 报码; 写进 schema 会把 core 的码盖成 MF-0001。

### 4.2 记录类型与文件

| 记录 | 文件 | 格式 | 解析 |
|---|---|---|---|
| `plugin` | `<plugin>/plugin.toml` | TOML | `tomllib` |
| `product` | `<root>/product.toml` | TOML | `tomllib` |
| `lock` | `<root>/brickie.lock` | **未钉死** | 先试 TOML 再退 JSON(宽容) |
| `index` | `build/index/dependents.json` | JSON | `json` |

形状失败 ⇒ 诊断 `BRV-MF-0001`(error)、退出码 **2**; 解析失败(TOML/JSON 语法)同码。
span 用 TOML 路径(如 `export[0].entries[2].sig`), `file` 相对 `root`(contract §3)。

**逐命令的实际门禁范围**: `plugin` + `product`。`lock` / `index` 是 **core 产出的机器
文件**, 其字段集在 v0.1 未定型(`W-17` "lock 是否含冻结代"未裁定), 过早门禁会把 core
的产物误判成形状错 ⇒ 这两份 schema 只通过 `schema.load_schema()` /
`schema.validate_record()` / `schema.check_declaration()` 对本模块与用例开放。
core 定型后可把 `("lock","index")` 加进 `cli._shape_gate` 的 `kinds`(一处开关)。

### 4.3 schema 的定位(载荷里带的就是规范那一份)

`freeze.py` 的 `PAYLOAD_ROOTS = ("python", "templates", "schema")` —— `schema/**` 会跟着
入口 ELF 的载荷一起走, 解包后落在 `<tool_root>/schema/`。所以单文件形态下形状门不再依赖
源码树; 曾经的"包内符号链接镜像"(`python/brickie/schema/`)已经删掉, 不必再维护两份。

`schema.schema_search_roots()` 仍按四档查找(第一个命中的目录为准):

1. `$BRICKIE_SCHEMA_ROOT` —— 用例 / 安装形态覆盖;
2. `python/brickie/schema/` —— 旧镜像的遗留槽位(目录已不存在, 命中不了);
3. `<tool_root>/schema/` —— 源码树 / 载荷里的规范位置(**实际生效的那一档**);
4. `<repo_root>/tools/brickie/schema/` —— 从工作目录之外的兜底。

四处都找不到时**降级为跳过形状校验**并给一条 info 诊断(不误杀、不静默)。

> 注: `schema.py` 的 docstring 与上面这份清单仍把第 2 档写成"包内镜像(入口 ELF 载荷里
> 只有这一份会进去)", 与现状不符 —— 该目录已不存在, 它只是排在最前的无害回退, 代码
> 行为不受影响。改 `schema.py` 不在本次文档同步范围, 记在这里待收口。

---

## 5. 呈现纪律

* 文本与 `--json` 是**同一 `data`** 的两个序列化(contract §8 / BRV-D2 纪律 2);
  `--json` 的信封是 `{protocol,status,command,exit_code,diagnostics,data,written}`
  (`files` 的内容属传输细节, 不进 JSON 输出; 落盘结果以 `written` 呈现)。
* 诊断在 `--json` 下按 `(code, file, span, target)` **稳定排序**(contract §3);
  文本诊断走 stderr, 数据走 stdout。
* `new` / `init` / `gen` 的产物清单来自 `data.artifacts`; 每个产物的状态
  (已写入 / 已是最新 / 跳过)是同一份落盘结果。
* contract 未写死 `data` 形状的命令(`ver bump` / `iface publish` / `iface freeze` 族)
  用**宽容渲染器**: 认得的字段逐行打印, 不认得的复合结构缩进 JSON —— 集成后不必返工,
  也不会因为缺字段抛异常。

---

## 6. 剩余 TODO 与发现的契约缺口

| # | 事项 | 现状 / 处置 |
|---|---|---|
| **T-1** | ~~`freeze.py` 的 `PAYLOAD_ROOTS` 不含 `schema`~~ | **已解决**: `PAYLOAD_ROOTS` 已含 `"schema"`, 包内符号链接镜像已删(§4.3); 只剩 `schema.py` 的 docstring/查找清单措辞未同步(见 §4.3 注) |
| **T-2** | ~~core 只落地了一部分~~ | **已解决**: `brickie-core` 的 15 条命令族(`check`/`closure`/`dep-*`/`ver-*`/`iface-*`)已全部落地(Rust 侧 `--selftest` = 432 cases); L5 的编排/呈现/落盘/退出码按 contract §5 的字段名写死, 与 core 的 `data` 逐字段核对一致(见 `rust/README.md` §12)。权威端到端回归在 `tools/brickie/tests/run.sh` |
| **T-3** | `brickie.lock` 的格式与字段集 | 实测 core 产出 TOML `{schema, [lock]{profile, [lock.product]{name,version,stage}}}`; `schema/lock.schema.json` 已按此 + `plugins[]`/`units[]` 描述, 且**不设 required**(弱校验; `_parse_record` 先试 TOML 再退 JSON) |
| **T-4** | `--note <path>` 传**路径**还是**正文**未裁定 | L5 传路径原样(与 CLI 一致); 若 core 需要正文, 在 `cli._cmd_iface` 的 args 里补 `note_text`(单点改动) |
| **T-5** | `plan-init` 无 `context.existing` 条款 | L5 按 `new` 的两段式补上(§2.3); core 忽略也无害 |
| **T-6** | `plan-init` 的 `brickie.lock` 正文走 `data.lock_content`(rust 侧偏差) | L5 已按 `_CORE_CONTENT_KEYS` 消费(§2.4); 建议 contract §5.2 补一句"机器文件正文的直传通道" |
| **G-9** | **`gen --check` 不一致没有专属 BRV 码** | 诊断用 `code: null`(contract §2 允许无码诊断)+ 精确到文件的 message; *建议*补一个新码(如 `BRV-GEN-0003`"生成物与重算不一致") |
| **G-10** | `--check` 只比对**计划内**路径 | 磁盘上多出来的旧生成物不会被报为"过期"(需要 core 的 `gen-plan` 给出全量清单或另一条命令) |
| **G-11** | `ver bump` / `iface publish` / `iface freeze` 族的 `data` 形状未在 contract §5 写死 | 呈现用宽容渲染器(§5); 建议把这几条的 `data` 关键字段补进 contract §5 |
| **G-12** | `dep index` 的索引 schema 未在 §9.2 列出(checklist C-09 已登记) | L5 已交付 `schema/index.schema.json`(形状对齐 `dep-index` 的 data) |
| **G-13** | `--version` 在 core 缺席时降级打印 L5 版本 | 保 `--version`(shell/CI 自检)不被"原生件还没编出来"卡死; `--version --deps` 仍按环境错报 2 |

---

## 7. 自证(桩 + 真原生件)

### 7.1 桩(从头到尾可复现 L5 全命令面)

`brickie-core` 未就绪时, 用两个**临时桩**(只回固定 JSON, **不进仓库**)即可验证 L5:
把 `$BRICKIE_CORE` / `$BRICKIE_GEN` 指向桩脚本, 然后逐条跑命令。桩覆盖:
四类骨架的 6 件产物、重复 `gen` 逐字节幂等、`gen --check` 一致/不一致两态、
`check` 的 0/1 两态、形状不符(exit 2)、协议不符(`BRV-PROTO-0001`)、原生件缺失、
`--json` 全局/逐命令等价与快照稳定、诊断稳定排序、`--help` 反向验收、`init` 落盘、
`dep *` / `ver *` / `iface *` 的编排与呈现。交付时该套 **129 项全绿**。

### 7.2 真原生件(`build/host/<triple>/bin/{brickie-core,brickie-gen}`)

交付时已用真 core/gen 跑过一轮集成, **34 项全绿**: 四类 × `c` 的 `new`(各 6 件产物、
描述符首行标记)、重复 `new` 冲突红(`BRV-GEN-0002`)、`--force` 只对生成物目录生效、
真 core 产物通过 L5 形状门、`gen` 幂等与 `--check`、`init` 落盘 8 件(含 `brickie.lock`)、
`dep add/rm` 不写盘、`--version --deps` 指纹、`--json` 等价与信封形状。core 全量命令面
落地后, 端到端回归改由 `tools/brickie/tests/run.sh` 承担(见 `tests/ACCEPTANCE.md` 的 V-1…V-19 矩阵)。

## 8. 本地运行

```sh
cd prototype/tools/brickie
PYTHONPATH=python python3 -m brickie --help
PYTHONPATH=python python3 -m brickie new ability service/crypto --subkind service --root /tmp/repo
PYTHONPATH=python python3 -m brickie check --root /tmp/repo
```

依赖: **Python ≥ 3.11**(`tomllib`)与两个预编译原生工具(core / gen)。**零第三方 Python 包**。
