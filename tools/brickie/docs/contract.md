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

**构建族码 `BRV-BLD-0001..0012`**(本刀新开一族: `BRV-D8` 的编码表**没有 BUILD 族**,
按 §9 的"未分配码的新缺陷按 §9 登记"落笔, 回灌项见 `rust/README.md` §10 S-B*):

| 码 | 严重度 | 触发 |
|---|---|---|
| `BRV-BLD-0001` | error | `product.toml` 缺席, 或它缺 `[build]`(构建需要产品级声明) |
| `BRV-BLD-0002` | error | `[build].sources` 展开后为空(声明了却一个文件都没匹配) |
| `BRV-BLD-0003` | error | 没有 platform 插件声明 `[build.target]`(无从得知 arch / 交叉前缀 / 链接脚本) |
| `BRV-BLD-0004` | error | 多个 platform 插件声明 `[build.target]`(谁是真值不明) |
| `BRV-BLD-0005` | error | 工具解析失败(交叉编译器 / objcopy / objdump / size / qemu 不在场)—— **环境错**, 退出码 2 |
| `BRV-BLD-0006` | error | 闭包内的插件没有 `[build].sources`(它会被组合, 却没说怎么编) |
| `BRV-BLD-0007` | error | `[build].sources` 里的**字面**路径不存在(通配不匹配**不算**错, 见 0002) |
| `BRV-BLD-0008` | error | `tests/gates.toml` 缺席, 或 `brickie test <name>` 的名字不在声明面里 |
| `BRV-BLD-0009` | error | 门禁判红(日志缺 `require` / 命中 `forbid` / 缺 PASS 用例 tag) |
| `BRV-BLD-0010` | error | `[build]` 的形状问题(产物落点 / 标志表非法, 如 `cflags` 为空) |
| `BRV-BLD-0011` | error | 执行的步骤失败(编译 / 链接 / 脚本非零退出)—— 由 L5 检出, 码由 core 随步骤给 |
| `BRV-BLD-0012` | error | `tests/gates.toml` 里的正则超出 core 支持的**子集**(报了, 不静默不匹配) |

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

### 5.4 `brickie-core` 的构建族命令

> **这一族是显式的第二族**(ADR-0004 / 设计 ADR-0003 的 S1–S4)。信封、诊断模型、
> 退出码纪律与 §5.1/5.2 **完全相同**; 唯一区别是它**要求 (交叉) 工具链在场** ——
> §0 的"不得要求 cc/cargo/nm 在场"只约束**组合期**命令, 见 §9 **R-13**。
> **一切判定在 core**: 工具候选序与解析(R-14)、增量口径(R-15)、门禁红绿(R-16)。
> L5 只做四件事: 校验 `argv[0]` 可执行、并按 `group` 并发/限时执行、收日志、呈现。

| `command` | `args` | `data` 关键字段 | `files`(机器文件) |
|---|---|---|---|
| `build` | `{profile?, jobs, backend, tag?, dry_run, force, emit_backends}` | 公共摘要(见下) + `dry_run` / `emit_backends` | `build/gen/build.mk`(`backend=make` 或 `emit_backends`)、`build/gen/build.ninja`(`backend=ninja` 或 `emit_backends`)、`build/gen/build-state.json`(`direct` 且非 `dry_run`) |
| `clean` | `{all}` | `{all, paths:[<相对 root>], note}` —— **paths 只是清单, 删除由 L5 做**(含"只删 root 之内"的守卫) | 无 |
| `run` | `{timeout_s?}` | 公共摘要 + `mode:"run"`; `steps[0].kind="run"`、`expect_timeout=false`(交互式, 继承 stdio) | 无 |
| `size` | `{}` | 公共摘要 + `mode:"size"`; 两条 `kind:"print"` 步骤(`size -A -x` 与 `size`) | 无 |
| `disasm` | `{head}` | 公共摘要 + `mode:"disasm"`; 一条 `kind:"print"` 步骤(`objdump -d`), `stdout_lines=head` | 无 |
| `test` | `{name?, list, jobs}` | `list=true` ⇒ `{gates:[名], hosttests:[名], scripts:[名], build_post:[名], file, present}`; 否则公共摘要 + `mode:"test"` + `gates:{file,gates,hosttests,scripts,build_post}` | 无 |
| `judge` | `{name}` + `context.log`(日志**正文**, 不是路径) | `{name, failed, checks:[{kind,pattern,ok}], log_lines, pass_lines, require, forbid, require_tags, file}`; 全绿 ⇒ 无码 **info**, `failed>0` ⇒ `BRV-BLD-0009` + 退出码 1 | 无 |

**公共摘要字段**(`build`/`run`/`size`/`disasm`/`test` 共有):

```json
{
  "profile": "dev",            "backend": "direct",   "tag": "",
  "target": {"owner": "platform/qemu-aarch64", "arch": "aarch64",
             "cross": "aarch64-linux-gnu-",
             "arch_flags": ["-march=armv8-a", "…"],
             "linker_script": "platform/qemu-aarch64/src/link.ld",
             "qemu": {"binary": "qemu-system-aarch64", "machine": "virt,gic-version=3",
                      "cpu": "cortex-a53", "memory": "128M", "extra": ["-nographic"]}},
  "outputs": {"obj_dir": "build/obj", "elf": "build/brick.elf",
              "bin": "build/brick.bin", "map": "build/brick.map"},
  "state_path": "build/gen/build-state.json", "gen_dir": "build/gen",
  "units": [{"name":"core","dir":"","plugin":null,"sources":3,"includes":["core/include"],
             "defines":[],"sources_list":["core/src/…"]}],
  "unit_count": 8, "source_count": 30,
  "tools": [ … 见下 … ],
  "steps": [ … 见下 … ], "steps_total": 32, "steps_todo": 32, "up_to_date": 0,
  "stale_outputs": [], "plan_hash": "sha256:…",
  "flags": {"cflags": […], "asflags": […], "ldflags": […]}
}
```

**`data.steps[i]` 的字段**(L5 侧只读这些键, 一条步骤的全部事实都在这里):

| 字段 | 类型 | 语义 |
|---|---|---|
| `label` | string | 人读标签(`cc <src>` / `ld <elf>` / `qemu <gate>` …) |
| `kind` | string | `compile` / `link` / `objcopy` / `run` / `gate` / `script` / `hosttest` / `hostrun` / `print` / `backend` |
| `argv` | [string] | **具体命令**, `argv[0]` 已是 core 解析好的工具(core 未解析出时是 `@<名>` 占位) |
| `cwd` | string? | 相对 root 的工作目录。L5 缺省按 `"."`; **core v0.1 不产出本键**(全部步骤在 root 下跑), 键位保留给将来 |
| `group` | u32 | 编排口径: 同组(且为 `compile`/`hosttest`)可并发, 组间串行 |
| `log` | string? | 日志落点(相对 root); `null` = 直接继承 stdio |
| `timeout_s` | u64? | 超时上限; `null` = 不限时 |
| `expect_timeout` | bool | `true` = **预期以超时结束**(QEMU 门禁: 镜像本来就跑到被掐)—— 超时不算失败 |
| `judge` | string? | 需要 core 判定的门禁名(日志正文经 `judge` 命令回判) |
| `stdout_lines` | u64? | 呈现时 stdout 的行数上限(展开 `--head`) |
| `outputs` | [string] | 本步骤写出的文件(增量状态 / `clean` 用) |
| `consumes` | [string] | 本步骤消费的、**由本计划里其它步骤产出**的文件。用途唯一: 上游失败时 L5 据此**跳过**下游 —— 否则 `ld` 会拿上一次的旧对象链接成功并打印 `ok ld`, 日志在骗人(而人看日志) |
| `fail_code` | string | **步骤失败时该用的码**: L5 不自己编码(恒 `BRV-BLD-0011`) |
| `tool_code` | string | `argv[0]` 不可执行时的码(恒 `BRV-BLD-0005`, 环境错 ⇒ 退出码 2) |

> **路径口径(判据最容易踩的一处)**: `data.*` 与 `steps[i].{argv,outputs,log,state_path,elf/bin/map,obj_dir}`
> 里的**一切路径都相对 root**(仓库根)。而声明面 `plugin.toml [build].sources/includes` 与
> `[build.target].linker_script` 里的路径**相对插件根** —— 由 core 展开/拼接成相对 root 之后
> 才出现在 `data` 里。所以"计划里的汇编源"是
> `platform/qemu-aarch64/src/start.S`, 不是 `src/start.S`; 链接脚本是
> `platform/qemu-aarch64/src/link.ld`。这两件事不一样, 写判据时按 **`data` 的形状**写。

**`data.tools[i]` 的字段与"工具解析在 core"(裁定 R-14)**:

| 字段 | 类型 | 语义 |
|---|---|---|
| `name` | string | 逻辑名: `cc` / `objcopy` / `objdump` / `size` / `qemu` / `make` / `ninja` |
| `what` | string | 它在这个角色里是什么(呈现用) |
| `candidates` | [string] | **候选序 = 策略**, 按序尝试。`cc` = `<cross>gcc` → `<cross>gcc-16…-13` → `cc`; binutils = 交叉版 → 宿主版 |
| `resolved` | string? | **解析结果**(即 `tools_resolved`): 仓库 `prebuilts/toolchain/bin/<名>` 优先, 再退 `context.path`(缺省 = 进程 `PATH`); 全落空 ⇒ `null` |

> `resolved` 之所以必须由 core 给: make / ninja 后端文件要把**解析结果烧进去**
> (生成物里写的是具体编译器路径), L5 与生成物不许各自解析一次 —— 否则"谁在编"
> 会有两个答案。所以 L5 只做**最后一道**可执行性校验(`runner.resolve_exe`), 不重选。

**三后端与 BR-D4 的 A/B**: `--backend direct`(缺省)= L5 按 core 的 `steps` 直接编排;
`make` = core 渲染 `build/gen/build.mk`(BR-D4 的**后端 A**)并执行; `ninja` = core 渲染
`build/gen/build.ninja`(**后端 B**)并执行。ninja 生成物有两条要点:

* **C 源与 `.S` 汇编各一条 rule**(`rule cc` / `rule asm`): 两类源的标志不同
  (`cflags` vs `asflags`), 用一条模板渲染两类等于把汇编当 C 编 —— 编得过是**运气**, 不是判据;
* 文件顶部钉 `builddir = build/gen/ninja`: ninja 的 `.ninja_log` / `.ninja_deps` 默认落在
  **调用目录**, 不钉就会往仓库根丢两个状态文件(踩过)。

三后端跑的是**同一批 argv**(只是由谁调度不同) ⇒ 同一输入出**逐字节一致**的 ELF/BIN
(BR-D4 的 S2 判据)。`--emit-backends` 一次产出 A/B 两份, 不执行。

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
sources  = ["src/*.c"]             # 相对插件根的通配(`*` 不跨 `/`); 字面路径不存在 ⇒ BRV-BLD-0007
includes = ["include"]             # 相对插件根的 include 目录
defines  = ["BR_FOO=1"]            # 可选: 追加 -D<name>(本刀新增字段, 裁定 R-17)

[build.target]                     # ★ 只允许 plugin_type = "platform" 声明(每镜像恰一个, §8.3 ④)
arch          = "aarch64"          # 目标架构名(呈现与产物命名用)
cross         = "aarch64-linux-gnu-"   # 交叉前缀(WORKAROUND br-wa-toolchain-001 的过渡形态)
arch_flags    = ["-march=armv8-a", "-mgeneral-regs-only", "-mstrict-align"]  # 由目标决定的标志
linker_script = "src/link.ld"      # 相对**本插件根**的链接脚本

[build.target.qemu]                # 目标机的跑法(brickie run / test 的 QEMU 门禁用)
binary  = "qemu-system-aarch64"
machine = "virt,gic-version=3"
cpu     = "cortex-a53"
memory  = "128M"
extra   = ["-nographic"]
```

> `[build]` / `[build.target]` 在 `brickie-v0.1.md` §7.1 里是"v0.1 只记录、v0.3 起被消费";
> 本实现**提前消费**它(ADR-0004 的 S1–S3), 并新增 `defines` 与整张 `[build.target]` 表 ——
> 缺口与理由登记在 §9 **R-17**。

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
[kernel]
hz = 200                           # 时钟节拍频率(Hz; 参考 Linux 的 CONFIG_HZ); 缺省(不写) = 200
                                   # ⇒ 5 ms 一拍。brickie build 转成 -DBR_CFG_TICK_HZ 下发到全部编译单元(ADR-0017)

[build]                            # ★ 产品级构建策略: 标志表 + 产物落点 + 核心本体源集合(ADR-0003 的 S1)
core_sources  = ["core/src/*.c", "core/src/*/*.c"]   # 核心本体(**不是插件**)的源集合, 相对仓库根
core_includes = ["core/include"]   # 核心本体的 include 目录, 相对仓库根
gen_sources   = ["build/gen/**/*.c"]  # 生成物源集合(由 brickie gen 产出); v0.1 可是 []
cflags        = ["-std=c11", "-O2"]   # 必填(空 ⇒ BRV-BLD-0010: 工具不自带缺省策略)
asflags       = ["-g3"]
ldflags       = ["-nostdlib", "-no-pie"]
obj_dir       = "build/obj"        # 可选; 缺省 build/obj

[build.release]                    # `--profile release` **追加**在 dev 标志之后
cflags_extra  = ["-DNDEBUG"]       # 另有 asflags_extra / ldflags_extra
```

### 7.3 机器文件

* `api/iface/<provider-short?>/…`: 目录用**插件名的路径段**(`service/crypto` ⇒ `api/iface/service/crypto/<unit>.toml`)。
* 快照头必须带 `hash_scope` / `truth` 与 `NOT_ABI` 提示(RV-3/V-11)。
* `brickie.lock` / `build/index/dependents.json` 的形状见 `schema/lock.schema.json` / `schema/index.schema.json`。
* `build/gen/build.mk` / `build/gen/build.ninja`(BR-D4 的 A/B 后端)与 `build/gen/build-state.json`
  (增量状态)是**派生物**: 首行标记 `brickie:generated`, 可删可重建, 不进版本库。
  形状由 `brickie-core` 给定(`files[]`), L5 只落盘(§4)。

### 7.4 `tests/gates.toml` 的形状

`brickie test` / `brickie build` 的门禁由这张**人写**表声明(形状门 = `schema/gates.schema.json`,
BRV-D2 口径: 只表达单条记录形状; 跨字段判定由 `brickie-core` 报 `BRV-BLD-*`)。

```toml
schema = 1

[run]
default_timeout_s = 5              # 可选: 未单列 timeout_s 的 QEMU 门禁的缺省上限

[host]                             # 宿主用例(不碰交叉工具链/QEMU)的编译事实
cc      = "cc"                     # 可选; 缺省 cc
cflags  = ["-std=c11", "-O2"]      # 可选
bin_dir = "build/hosttest"         # 可选; 缺省 build/hosttest

[[hosttest]]                       # 宿主可执行用例: 编 → 跑 → 退出码即判据
name     = "string-test"
sources  = ["core/src/string.c", "tests/host/string_test.c"]
includes = ["core/include", "tests/host"]
defines  = ["BR_HOSTTEST=1"]       # 可选
depends  = ["other-hosttest"]      # 可选: 跑本用例前先跑它(拓扑展开)
timeout_s = 30                     # 可选

[[gate]]                           # QEMU 门禁: 限时跑镜像 → 日志按正则判 + 用例 tag 点名
name         = "smoke"
timeout_s    = 3                   # 可选(缺省取 [run].default_timeout_s)
log          = "build/logs/smoke.log"  # 可选(缺省 build/logs/<name>.log)
require      = ["core MainLoop", "irq_ticks=[1-9]"]   # 每条都必须在日志里出现
forbid       = ["\\[PANIC\\]"]                        # 命中任何一条即红
require_tags = ["TC-IRQ-001"]      # 每个 tag 都要有 "PASS <tag> " 行(裁掉用例也算红)
expect       = "timeout"           # 可选; 缺省 timeout(镜像跑到被掐是预期结果)

[[script]]                         # 脚本门禁: 跑一个脚本 → 退出码 + 可选 stdout 正则
name    = "check-string"
argv    = ["bash", "tools/check-string.sh", "build/obj/core/src/string.o"]
require = ["ok   编译器支持例程无自递归"]
forbid  = []

[[script]]                         # 头文件自足性门禁: 每个头单独/全体/二次 include
name    = "check-headers"          #   `-fsyntax-only` 都要过; 判据在脚本里
argv    = ["bash", "tools/check-headers.sh"]

[build]
post = ["check-string", "check-headers"]   # `brickie build` 成功后自动跑的短名单
```

> **`check-headers` 为什么值得一条门禁**: 注释里误写 `*/` 会**提前闭合注释**,
> 而最常见的误写来源就是路径通配符 —— `core/src/sched/*.c`、`platform/*/src/link.ld`、
> `core/src/sync/**` 都含 `*/`(本仓已踩过三次)。这种错"编得过"是假象: **没有 `.c`
> 包含的头文件根本不会被编译到**, 错误要等第一个使用者出现才炸 ⇒ 必须主动对每个头做
> `-fsyntax-only`(单独 / 全体 / 二次 include 三种形态)。

**正则子集(如实声明, 免得写出"永不匹配"的假绿)**: core **不引正则库**(§9.3 最小依赖集),
只支持 `.` `*` `+` `?` `[...]`(含 `^` 取反与 `a-z` 区间)与 `\x` 转义; **不支持**
分组 / 交替 / 锚点 `( ) | ^ $ { }` —— 写了它们 ⇒ **`BRV-BLD-0012`**(退出码 2), 而不是
静默不匹配。"三种 FAIL 的 forbid"要写成三条, 不能写成一条交替正则(见 `tests/gates.toml` 的自注)。

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
| R-13 | `brickie-v0.1.md` §0 的边界纪律写"v0.1 任何命令**不得要求 cc/cargo/nm 在场**", 而构建族**必须**要交叉编译器 | 边界纪律的准确表述是: **组合期**命令(check / dep / iface / gen / new)不得要求编译器在场; 构建族(build / clean / run / size / disasm / test / judge)是**显式第二族**, 它就是"要求工具链在场"的命令 | 一句话管两族会自相矛盾; 纪律的**目的**(CI 的组合期作业零编译依赖)不受影响。见 `tools/brickie/README.md` §1 与 ADR-0004 |
| R-14 | 工具候选序(`gcc` → `gcc-16` → …)与"用哪个编译器"该由谁定 | **候选序与解析都在 core**(`data.tools[i].candidates/resolved`); L5 只做最后一道可执行性校验, 不重选 | make / ninja 后端文件是**生成物**, 必须把解析结果**烧进去** —— 若 L5 或生成物各解析一次, "谁在编"就有两个答案 |
| R-15 | 增量口径: "输入集合的内容戳"还是"argv 指纹 + 输入 mtime" | **argv 指纹 + 输入 (size, mtime)** 两条判据(与 make / ninja 同族); 上游要重建 ⇒ 下游显式传递重建 | `.d` 依赖文件是**构建的产物**(首次编译后才存在) ⇒ 按"输入集合算戳"会让头依赖永远多编一轮(实测踩到); 时间戳口径没有这个自指 |
| R-16 | 门禁日志的红绿判据(require / forbid / PASS tag)归谁 | **判据在 core**(`judge` 命令按 `tests/gates.toml` 的正则判), L5 只收日志正文并执行 | "红绿"只能有一处真值; 否则文本输出、`--json` 与 CI 退出码会各判一次 |
| R-17 | `plugin.toml [build]` / `product.toml [build]` 在 `§7.1` 里只有 `sources`/`includes` 两个字段, 表达不了完整编译事实 | 新增 `[build].defines`(插件级)与 `product.toml [build]` 的 `core_sources`/`core_includes`/`gen_sources`/`cflags`/`asflags`/`ldflags`/`obj_dir`/`[build.release].{cflags,asflags,ldflags}_extra`, 以及 platform 的整张 `[build.target]`/`[build.target.qemu]` | "怎么编"必须有**一处**声明面可写(否则又回到 Makefile 字面量); 形状已补进 `schema/*.schema.json` 与 §7.1/§7.2, 待回灌设计 §7.1/§7.2 |
| R-18 | 产品级**语义配置**(第一项 = 时钟节拍 HZ)怎么进编译 | `product.toml [kernel].hz`(整数, 缺省 200)由 `brickie build` 转成 `-DBR_CFG_TICK_HZ=<hz>` 下发到**全部**编译单元(core/插件/生成物); 该 define 与通用 `cflags` 分开, 走 `config_defines` 通道 | 不给 `cflags` 写裸 `-D`: 语义键能被 `schema/product.schema.json` 校验(`integer` + `minimum: 1`), 而且"每秒多少次时钟中断"只有一处真值(core 的 jiffies 与 platform 的装弹周期读同一个宏; ADR-0017) |
| R-19 | 声明依赖怎么变成**编译期事实**(此前 `-I` 是全局并集, 未声明的跨插件 `#include` 静默编过) | `build` 的每单元包含面 = `core_includes` + 自己 + **声明依赖传递闭包**内各插件的 includes(core 额外拿 platform 头; `build/gen` 拿全部被选插件的头); 另加**声明 ↔ 使用**交叉校验(扫源码 `#include` 映射到提供方, 不在声明闭包即报), `check` 的 deps 域与 `build` 同口径 | 只收窄 `-I` 不够: 目标工具链的 glibc 头会兜住 `unistd.h` 这类常见名(不是"找不到头"而是静默改用宿主 libc 声明)⇒ 必须有与编译器搜索路径无关的判据; 两处共用 `model::cross_plugin_includes()` 以免口径分叉(ADR-0018) |

> **Rust 侧的补充裁定**: 求解/校验/接口引擎在实现中还被迫落笔了一批更细的口径
> (S-1…S-19: 首次 publish = 建档不推进段、`closure` 不扫描 `requires_iface` 而 `check` 扫描、
> platform 的 `[[res]]` 作容量不进消费者 Σ、`DEP-0011` 的判据、R2 的取值口径、
> `app → third_party ability` 的例外、窗口内 publish 消费基线以免双 bump 等),
> 构建族另有 **S-B1…S-B6**(工具解析在 core / 增量口径 / 三后端与 BR-D4 的对应 /
> 正则子集拒绝而非静默 / 门禁判据归属 / `build/gen/**` 的生成物归属)。
> 它们是**引擎内部口径**, 逐条登记在 `rust/README.md` §10; 本文件的 R-* 是其上位清单。
>
> ⚠ **编号注意**: `rust/README.md` §10 的历史表用的是它自己的 R-1…R-16 编号
> (与本文 R-* **不是同一套**); 本文的 R-13…R-16 是构建族的上位裁定。两者待回灌设计时统一。
