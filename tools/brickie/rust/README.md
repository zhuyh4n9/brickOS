# brickie-core — L0/L1 Rust 核心(`prototype/tools/brickie/rust/`)

> `brickie check` 是**声明面完备性检查器**: 保证组合在**逻辑上**自洽, 不保证"编得过 / 跑得对"。
> 本 crate 是**唯一判定处**: 分类学 / 版本 / 区间 / 特权 / 生成计划的任何业务规则
> 都不许留在 L5(Python)。权威接口 = [`../docs/contract.md`](../docs/contract.md); 设计 = `Design/docs/2-toolchain/brickie/brickie-v0.1.md`(下称 `§x`)。

**v0.1 命令面已全通**(23 条叶子命令 + `version` / `model`): 首刀交付
`version` / `model` / `plan-new` / `plan-init` / `gen-plan` / `dep-add` / `dep-rm`;
第二刀(agent-B)补齐 `check` / `closure` / `dep-tree` / `dep-graph` / `dep-why` /
`dep-index` / `ver-show` / `ver-bump` / `iface-list` / `iface-show` / `iface-diff` /
`iface-status` / `iface-publish` / `iface-freeze` / `iface-deprecate` /
`iface-undeprecate` / `iface-unfreeze` / `iface-refreeze`。
命令面状态见 §3, 实现期裁定见 §7/§10, 与 L5 的字段对齐见 §12。

---

## 1. 构建与自证

`$HOME` 只读时 **必须**设一个可写的 `CARGO_HOME`(本机 `/home/zhuyh/.cargo` 不存在):

```sh
cd prototype/tools/brickie/rust
export CARGO_HOME=/home/zhuyh/workspace/unikernel-dev/build/cargo-home
mkdir -p "$CARGO_HOME"
cargo build --release          # 产物: rust/target/release/brickie-core
./target/release/brickie-core --selftest
```

- 依赖(§9.3 最小依赖集): `toml = "0.8"`、`serde_json = "1"`、`sha2 = "0.10"`;
  **不引入** clap / serde derive。`Cargo.lock` 已生成, **提交**。
- 记录的自证(本机跑通, 第二刀结束时复测):
  - `CARGO_HOME=… cargo build --release --locked` ⇒ `Finished release ... in 3.9s`,
    **0 error / 0 warning**(全量重编, 非缓存命中)。
  - `brickie-core --selftest` ⇒ `ok 432 cases`, 退出码 0(首刀 247 ⇒ 第二刀 +185 条断言)。
  - `echo '{"protocol":1,"command":"version","root":"."}' | brickie-core` ⇒ `exit_code:0`, `data.languages[0].version = "1.93.1"`。
  - 端到端(真实 L5 CLI, `BRICKIE_CORE` 指向本刀产物): 造环 ⇒ 完整路径 + exit 1;
    `iface publish` 两次 ⇒ 第二次 `files=[]`; `iface status --check` 一致;
    `check --profile dev` 绿 / `release` 红; `unfreeze`/`refreeze` 空解冻 ⇒ 四段不动
    (输出摘录见仓库对话记录 / `§12`)。
- **落点**: 本刀产物在 `rust/target/release/brickie-core`(`rust/.gitignore` 已忽略 `target/`)。
  按 G-6 应**出树**到 `build/host/<host-arch>/<host-os>/bin/brickie-core`(仓库根 Makefile
  由并行 agent 负责; 本目录的 Makefile 未动)。

## 2. 模块地图

| 文件 | 行数级 | 职责 |
|---|---|---|
| `main.rs` | 235 | 信封入口 / 命令分派 / `--version` / `--help`; 进程退出码纪律 |
| `proto.rs` | 182 | 请求↔响应信封、协议校验(`BRV-PROTO-0001`)、`internal_error` / `unimplemented` |
| `diag.rs` | 249 | 诊断 `{code,severity,target,file,span,message,hint}`、severity、退出码汇总、`(code,file,span,target)` 稳定排序 |
| `rules.rs` | 652 | 名字契约 / namespace 枚举 / `subkind` 推导 / 相位推导与约束 / §3.4 特权级别与 `(ops×granularity×region)` 正交表 / §7.3 依赖方向表 + `api_type` 禁则 + `allow_edges` / §3.5 导出面不变量 / 模板可得性 |
| `version.rs` | 413 | 四段版本解析/序列化/显示/比较; `range` 解析 + `^`/`~` 解析期展开 + 交集 + 可满足性; `compat_gen` 精确匹配; §5.2 事件→段推进; `BRV-VER-0007` |
| `model.rs` | 1968 | `plugin.toml` / `product.toml` 类型化模型 + 加载器(树扫描/跳过目录/名字唯一); 单记录内跨字段不变量(TAX-0015..0019 / PRIV-0001/0002 / IFACE-0011); `Snapshot`(`api/iface/**`, 含 `baseline_*`/`strict_params`)与 `Lock`(`brickie.lock`)只读; `DependentsIndex` |
| `emit.rs` | 329 | **确定性** TOML/JSON 发射原语(`TDoc`/`TTable`/`TValue`)+ `render_lock` / `render_dependents` / `render_snapshot`(含 baseline / strict_params) |
| `plan.rs` | 627 | `plan-new` / `plan-init` / `gen-plan`; 变量展开(**大小写两套占位符**); 冲突判定(`BRV-GEN-0002`) |
| `dep.rs` | 203 | `dep-add` / `dep-rm` 的**片段生成**(裁定 R-2, 不写 `plugin.toml`) |
| `solver.rs` | 1204 | ✅ `closure`/`dep-tree`/`dep-graph`/`dep-why`/`dep-index` + **纯函数闭包求解**(`solve`: 选择集 → 三类边闭包 → `compat_gen`/`range` → 拓扑 + 环(结构化路径)→ 相位单调 → 方向禁则 → 预算合计 → profile 判定) |
| `check.rs` | 518 | ✅ `check`: 四域(`deps`/`iface`/`tax`/`priv`)汇总 + `scope_results` + `summary`; 复用 `solver::solve` |
| `iface.rs` | 2331 | ✅ `ver-show`/`ver-bump`/`iface-*`: **IFACE-IR 规范化 + sha256 hash**、变更集(append vs modify)、影响报告、版本推进、幂等、解冻窗口 + baseline、机器文件发射 |
| `selftest.rs` | 2034 | `--selftest`: 领域用例(不经 Python), 421 条 |

## 3. 命令实现状态

| `command` | 状态 | `data` 关键字段 | 备注 |
|---|---|---|---|
| `version` | ✅ | `{version,protocol,languages,deps}` | `languages=[{name:"rust",version:<rustc>}]`; `deps` = 三个直接依赖 |
| `model` | ✅ | `{plugins:[Plugin],count,product,has_product,platform_capacity}` | `args.plugin?` 过滤; 顺带报形状错 |
| `plan-new` | ✅ | `{artifacts:[{path,kind,template,vars}],planned,count}` | 两段式: 无 `context.existing` = 先出计划; 有则做冲突判定 |
| `plan-init` | ✅ | 同上 + `lock_content` | 见偏差 D-3 |
| `gen-plan` | ✅ | 同上 | 每插件一条 `build/gen/<plugin>/plugin_desc.c` |
| `dep-add` | ✅ | `{snippet,path,span,found}` | 裁定 R-2: 只打印, 不写盘 |
| `dep-rm` | ✅ | `{snippet,path,span,found}` | 同上; `found=false` 表示没定位到 |
| `check` | ✅ | `{summary:{errors,warnings,infos,profile,closure:{plugins,ram_kib,stack_kib}},scope_results,scopes}` | `args.profile` / `args.scopes[]` / `args.plugin?`(按诊断 target 过滤, 见 S-7) |
| `closure` | ✅ | contract §5.1 全字段 + `cycles:[{path,edges}]` + `selected`/`seeds`/`has_platform` | **不扫描 `requires_iface`**(V-10② 逐字节可复现, 见 S-1) |
| `dep-tree` | ✅ | `{roots,nodes,kind}` | `args.kind` = init/runtime/type/all; `args.plugin?` |
| `dep-graph` | ✅ | `{format,nodes,edges,text}` | dot/mermaid/json; `json` 时 `text` 是规范化 JSON 串 |
| `dep-why` | ✅ | `{found,path,edges}` | 消费者→提供者的最短路径; 找不到 `found=false` 且 **exit 0** |
| `dep-index` | ✅ | `{dependents:{<plugin>:[{from,kind}]}}` + `files:[build/index/dependents.json]` | 确定性排序; 无时间戳 |
| `ver-show` | ✅ | `{id,plugin,version,compat_gen,segments,freeze_state,status,source,compat_gen_source,published,not_abi,hash_scope,truth}` | RV-11 的"代不参与比较"以 info 报出 |
| `ver-bump` | ✅ | `{id,plugin,unit,rule,from,to,version,segments,compat_gen,file}` | 写快照 + CHANGELOG; R-3 的"`plugin.toml` 应改成什么"以 info 报出 |
| `iface-list` | ✅ | `{units:[{id,provider,unit,api_iface,form,version,compat_gen,freeze_state,status,published,hash,hash_short,file}],count}` | 声明面 ∪ 快照 |
| `iface-show` | ✅ | `{id,unit,entries,source,not_abi:true,truth,hash_scope,declaration_hash,snapshot_hash,published}` | V-10③ / V-11①③ |
| `iface-diff` | ✅ | `{id,changes,verdict,compat_gen_changed,version:{from,to,reasons},dependents:{direct,transitive,unsatisfied},freeze_state,published}` | §6.3 |
| `iface-status` | ✅ | `{id,recomputed_hash,snapshot_hash,declaration_hash,plugin_toml_hash,consistent,check,expected_snapshot,expected_snapshot_path}` | V-11④ 的 `BRV-IFACE-0012`; 门禁不写盘(S-12) |
| `iface-publish` | ✅ | `{id,published,noop,verdict,from,to,version,compat_gen,compat_gen_changed,hash,strict_params,changes,version_change,report:{dependents},files_planned}` | 落快照 + `CHANGELOG.md` + `brickie.lock`; 面未变 ⇒ `noop:true` + `files:[]` |
| `iface-freeze` | ✅ | `{id,entry,proposal,freeze_state,status,compat_gen}` | **A-26 例外**: 只写 `build/gen/proposals/<unit>.toml` |
| `iface-deprecate` / `iface-undeprecate` | ✅ | `{id,entry,status,freeze_state,compat_gen,from,to,version,changes,files_planned}` | 状态机 + 幂等(S-9) |
| `iface-unfreeze` | ✅ | `{id,freeze_state:"unfreezing",status,compat_gen,version,baseline_hash,note,files_planned}` | 须 `--note` + `frozen`; 快照里记 baseline(R-7) |
| `iface-refreeze` | ✅ | `{id,freeze_state:"frozen",status,from,to,version,compat_gen,compat_gen_changed,empty_unfreeze,changes}` | 空解冻 ⇒ 四段全不动(V-16) |
| 未知名 | ✅ 用法错 | `{}` | `exit_code 2` + `code:null` |

## 4. 信封、诊断与退出码

- 请求 `{protocol, command, root, args, context}`; 响应 `{protocol, status, exit_code,
  diagnostics, files, data}`。stdin 读**全部**(一行或美化多行都可), stdout 写**一行** JSON。
- `protocol != 1`(含缺失)⇒ `BRV-PROTO-0001` + `exit_code:2` + `status:"ok"` + **进程退出码 0**。
- **进程退出码恒为 0**(除 `status:"internal_error"` 与崩溃, 二者为 2): 业务结论只在
  `exit_code` 里(BRV-D9)。
- 退出码汇总: 显式覆盖(2) > 有 `error`(1) > 0。`usage`/`env`/`shape` 三类诊断把覆盖置 2。
- `--json` 的诊断数组按 `(code,file,span,target)` **稳定排序**; 无码诊断排最前。
- **JSON 键序**: serde_json 默认用 `BTreeMap` ⇒ 对象键按字典序输出。**逐字节快照测试
  必须知道这一点**; 若要与 contract 示例的键序逐字节一致, 需要在 L5 侧重组。

## 5. 复用接口(首刀给第二刀的入口, 现已全部有调用点)

### 5.1 分派约定

`main.rs` 的分派(第二刀已接线完毕):

| 模块 | 命令 |
|---|---|
| `check::run` | `check` |
| `solver::run` | `closure` / `dep-tree` / `dep-graph` / `dep-why` / `dep-index` |
| `iface::run` | `ver-show` / `ver-bump` / `iface-*` |

### 5.2 已就绪、**请复用**的原语

| 需求 | 原语 |
|---|---|
| 区间匹配 / 交集 / 冲突 | `version::Range::{matches,intersect,is_satisfiable,canonical}` |
| `compat_gen` 精确匹配 | `version::compat_gen_matches` |
| §5.2 事件→段推进 | `version::{Event,advance}`(右段清零、`COMPAT_GEN` 不清零) |
| `BRV-VER-0007` | `version::check_no_regress` |
| 依赖方向 / api_type 禁则 / `allow_edges` | `rules::dep_edge_violation`(+ `dep_direction_ok` / `api_type_edge_reason`) |
| 相位单调(R1) | `rules::phase_rank` + `Plugin.phase` |
| §3.4 正交表 | `rules::memory_combo_legal` / `min_level_for_memory_op` / `check_privileged` |
| 导出面不变量(单记录) | `rules::check_export_invariants` |
| 树加载(含形状诊断) | `model::load_tree` → `TreeLoad{diags,plugins,product,has_product}` |
| **裁定 R-6 平台容量** | `model::TreeLoad::platform_capacity()` —— `platform` 的 `[[res]]` 作 `totals.platform_capacity`(见 §10 S-14: 它**不**进消费者侧 Σ) |
| **裁定 R-1 无 product 分支** | `model::TreeLoad::product_level_constraints_apply()` / `has_product` |
| §7.5 profile 严格度 | `model::Product::release_strict()`(`[lint].frozen_deps` 覆盖 `stage`) |
| 反向依赖索引 | `model::DependentsIndex::build(&tree)` + `emit::render_dependents` |
| 快照 / lock 只读 | `model::{load_snapshots,load_lock}` |
| 确定性 TOML 发射 | `emit::{TDoc,TTable,TValue,toml_string,render_lock,render_snapshot}` |
| 逐插件占位符 | `plan::{PluginFacts,plugin_vars,facts_of,plugin_paths}` |
| 段 / hash | `sha2` 已在依赖里; IFACE-IR 域分隔见 `iface::HASH_REV`(规则 10) |
| **闭包求解(纯函数)** | `solver::solve(&tree, profile, scan_iface) -> SolveResult`(带域标签的诊断) |
| **IFACE-IR / hash** | `iface::{canonical_surface_text,surface_hash,short_hash,Entry,Surface}` |
| **变更集 / 影响报告** | `iface::{change_set,classify_modify,Change,dependents_of,Dependents}` |

### 5.3 **不要重复报**的码

`model::load_tree` 已在加载期对**每条记录**报出:
`BRV-MF-0001`(形状)、`BRV-VER-0005/0006`(版本串)、`BRV-IFACE-0011`(truth/hash_scope)、
`BRV-TAX-0013`(仅 plan-new)、`BRV-TAX-0015/0016/0017/0018/0019`、`BRV-PRIV-0001/0002`。

`check.rs` **不对**同一插件/同一导出面再报一遍; 只报**跨记录/跨文件**的部分:
环与拓扑(`BRV-MF-0001` + 完整边路径)、相位单调(`BRV-DEP-0009/0010`)、
单版本政策(`BRV-DEP-0011`)、`compat_gen`/`range`(`BRV-VER-0001/0002/0004`)、
预算与独占冲突(`BRV-MF-0001` + 双方/差值)、release 门禁(`BRV-IFACE-0009`)、
导出不变量 3 的**跨文件分类相等**(`BRV-TAX-0018`)。

### 5.4 第二刀已实现的规则(首刀**故意不做**)

- 依赖方向表的**跨插件**执法(`dep_edge_violation` 的调用点)+ `skin` 再导出边的
  `api_type` 豁免(§3.5 消费方 3)。
- 环检测: init 环 ⇒ 硬错误 + **结构化完整路径**; runtime/type 环 ⇒ 只报 info。
- 相位单调 R1/R2(§7.2; R2 的口径裁定见 §10 S-6)。
- `sched_class` 组合期冲突(`TT_SAFE` 缺 `[sched.tt]`; `COOP_ONLY` × 非 coop 调度器;
  `sched_kind` 见裁定 R-5)。
- 预算合计(product `[budget]` + platform 容量)与 IRQ/DMA/引脚/设备名独占冲突。
- 导出不变量 3 的跨文件部分(`reexport_of` 指向的单元存在且分类相等)+ 单元名/符号族碰撞。
- IFACE-IR 规范化 + `sha256` + 变更集 + 影响报告 + 解冻窗口(§5.3/§5.4/§6.2/§6.3)。

## 6. 机器文件形状

> 形状**以 `schema/*.schema.json` 为权威**(并行 agent 已交付; 本 crate 已对齐并用
> `jsonschema` 实测 `plan-init` 的 lock 通过 `lock.schema.json`)。
> 生成物首行统一标记 `# brickie:generated`(与 `brickie-gen` 同字面量; 见 `emit::GENERATED_MARKER`)。

### 6.1 `brickie.lock`(TOML, `emit::render_lock`, 形状 = `schema/lock.schema.json`)

```toml
# brickie:generated
# brickie.lock — 机器拥有的文件, 请勿手改; 同一输入两次运行逐字节相同(contract §4)

schema = 1

[lock]
profile = "dev"

[lock.product]           # 本实现的扩展(顶层/ lock 均 additionalProperties:true)
name = "hsm"
version = "0.1.0.0"
stage = "dev"

[[lock.plugins]]
name = "service/crypto"
version = "0.1.0.0"
compat_gen = 0

[[lock.units]]
id = "service/crypto#crypto"
provider = "service/crypto"
unit = "crypto"
api_iface = "native"
version = "0.1.0.0"
compat_gen = 0
hash = "sha256:…"
hash_scope = "decl"
truth = "decl"
```

`[[lock.plugins]]` 按插件名、`[[lock.units]]` 按单元 id 排序(调用方保证);
无时间戳 / 随机 / 主机信息 ⇒ 逐字节可复现。`model::load_lock` 对缺字段容错
(schema 的 v0.1 弱校验: 顶层无 required)。

### 6.2 `build/index/dependents.json`(`emit::render_dependents`, 形状 = `schema/index.schema.json`)

```json
{"schema": 1, "dependents": {"service/crypto": [{"from": "app/hsm", "kind": "runtime"}]}}
```

键用 `BTreeMap` 排序; 只按**已声明的** `[[dep]]` 建边(运行期注册表取用不产生边, §3.1/BRV-Q13)。

### 6.3 快照读取 `api/iface/<provider 路径段>/<unit>.toml`

`model::load_snapshots` **容错**读取: 字段可平铺, 也可在 `[unit_meta]` 下; 条目表名
接受 `[[entry]]` / `[[entries]]` / `[[export.entries]]`; 另读 `strict_params`(规则 10)、
`baseline_hash` 与 `[[baseline_entry]]`(裁定 R-7 的解冻基线)。`provider` 由相对路径推导
(`api/iface/service/crypto/crypto.toml` ⇒ `service/crypto`), `id = <provider>#<unit>`(裁定 R-11)。
`api/iface` 不存在 ⇒ 空表 + 无诊断 = "未发布"; `brickie.lock` 不存在 ⇒ `None`。

### 6.4 第二刀新增的机器文件

| 文件 | 谁写 | 形状 |
|---|---|---|
| `api/iface/<provider 路径段>/<unit>.toml` | `iface-publish`/`ver-bump`/`iface-{un,re}freeze`/`iface-{de,un}deprecate` | `emit::render_snapshot`(含 `NOT_ABI` / `hash_scope` / `truth` / `strict_params`, 可选 baseline) |
| `api/iface/CHANGELOG.md` | 同上 | **版本段追加**(段头 `<unit> v<4 段> (compat_gen=<N>)` + hash + 变更摘要), **无时间戳**(R-12) |
| `brickie.lock` | `iface-publish`(以及 `ver-bump` 之外的写入面) | `emit::render_lock`; `units` = 全部已发布快照 + 本次更新(按 id 排序) |
| `build/gen/proposals/<unit>.toml` | `iface-freeze` | 待升格提案(A-26), **不**落 frozen 快照、**不** bump `COMPAT_GEN` |
| `build/index/dependents.json` | `dep-index` | `emit::render_dependents` |

## 7. 关键实现期裁定 / 与 cxx 首刀的偏差

| # | 事项 | 本实现 | 依据 |
|---|---|---|---|
| D-1 | `[plugin].phase` 允许集 | `interface` / `ability.service` ⇒ `{early,late}`(给 `core` ⇒ 错); `platform` / `ability(其它)` ⇒ `{early,core}`; `app` ⇒ `{early,core,app}`。**`early` 恒合法** = 无 `init` 钩子 | 父 agent 澄清 + §7.2; 判例 `platform/qemu-aarch64(phase=core)` × `sched-coop(phase=core)` 的 init 边 **不报** `BRV-DEP-0009` |
| D-2 | 相位推导默认 | `platform`/`ability(非 service)` ⇒ `core`; `ability.service`/`interface` ⇒ `late`; `app` ⇒ `app`; 无 `init` ⇒ `early` | 同上。**注意**: cxx 首刀把 `scheduler` 推成 `early`, 本实现按 §7.2 推成 `core`(与父 agent 澄清一致) |
| D-3 | `plan-init` 的 `brickie.lock` | 计划里给 `{path:"brickie.lock", kind:"machine", template:""}`, 文本另放 `data.lock_content` | contract 说 plan 不产内容, 但 lock 是 **core 拥有的机器文件**(§4); 二者并存便于 L5 直接落盘 |
| D-4 | 模板占位符 | **同时**给小写(`{{name}}`)与大写(`{{PLUGIN_NAME}}`)两套变量 | 现行 `templates/` 用小写, cxx 首刀用大写; 多给变量对渲染无副作用 |
| D-5 | `descriptor_include` | `"<short>/<short>.h"` | 描述符模板 `#include "{{descriptor_include}}"` 无规格定义; 这是唯一合理读法(插件自己的对外头) |
| D-6 | 未知枚举 / 名字契约的码 | 一律 `BRV-MF-0001` + `exit_code 2` | BRV-D8 的 `MF-0001` 明写含"插件名不符名字契约"(G-2 的收口); R-9 兜底 |
| D-7 | `lang` 未交付 | **无码用法错** + `exit_code 2`(`cxx`/`rust`) | BRV-D8 没有对应码; 登记为缺口 R-4 |
| D-8 | 重复 `[[dep]]` | **不报** | 无已分配码; 借用 `BRV-MF-0001` 会把良性提示升成退出码 2(缺口 R-14) |
| D-9 | `gen-plan` 的"头文件" | 只出**描述符** | §8.4 的 `include/<short>/<short>.h` 在 v0.1 是**人写**文件; 生成头文件排 v0.3(缺口 R-13) |
| D-10 | cxx 首刀的 `{{descriptor_include}}` 等缺失 | 本实现补齐全部现行模板占位符 | 使 `plan-new` 的计划可被 `brickie-gen render` 直接渲染 |
| D-11 | 加载器对 schema 的对齐程度 | 与 `schema/plugin.schema.json` **逐字段对齐**: `[compat].core`、`export.version/compat_gen/freeze_state/status` 在**存在时必填**; `[[privileged.resources]]` 按**数组表**读取累加; `value` 接受 `string \| string[]`(数组以 `,` 规范化, 裁定 R-4); `product.version` 按 schema 作**可选**(缺省 `0.0.0.0`) | schema 是形状权威; L5 会先拦, core 亦须独立可报(V-18a) |
| D-12 | 机器 TOML 是否带 `brickie:generated` 标记 | 带(`emit::machine_header`); JSON 索引不带(JSON 无注释) | 与 `brickie-gen` / 现有 `.scratch` 机器文件同口径, 便于识别"是不是本工具产物" |
| D-13 | `dep closure` 是否扫描 `requires_iface` | **不扫描**(`scan_iface=false`); `check` 才扫描 | §10 V-10② 要求"加/去该字段闭包逐字节相同"; V-7 的 `compat_gen` 冲突在 `check --deps` 里报(见 §10 S-1) |
| D-14 | `closure` 里 `plugins[]` 是否只含闭包成员 | 列**整棵树的插件**并带 `selected` 布尔(另给 `selected` 名字数组) | contract §5.1 有 `selected` 字段 ⇒ 它必须能表达"剔除"; 计数用 `summary.closure.plugins` |
| D-15 | `iface-status` 是否回写快照 | **不产 files**(门禁); 把应有的快照放 `data.expected_snapshot` | "status 是 CI 门禁, 不是修复命令"; 见 §10 S-12 |
| D-16 | `dep-why` 找不到路径 | `found:false` + `exit 0` | "找不到"是查询结果, 不是校验红 |
| D-17 | 窗口内 `publish` 与 `refreeze` 都会 bump `COMPAT_GEN` 吗 | **只 bump 一次**: `publish` 记录变更并**消费基线**(新面成为新承诺面), `refreeze` 只关窗口 ⇒ 空解冻 | 见 §10 S-19; 避免"publish + refreeze 双 bump" |

## 8. `--selftest` 覆盖(V-18a, 不经 Python)

`ok 432 cases`(首刀 247 + 第二刀 185), 覆盖:

1. **版本矩阵逐行**(V-4 a–f): `COMPAT_GEN+1` 只清 minor/revise、`MAJOR+1` 不动 `COMPAT_GEN`、
   新增/枚举追加/状态转移 ⇒ `MINOR+1`、修 bug ⇒ `REVISE+1`、空解冻 ⇒ 四段全不动。
2. **版本串**: 4 段解析/序列化/`v` 显示、3 段 ⇒ `0005`、`compat_gen` 精确匹配、`0007` 回退。
3. **range**: `=`,`>=`,`>`,`<=`,`<`,`~`,`^`,`*`、缺段右补 0、4 段 ⇒ `0006`、
   交集 + 可满足性(开/闭同点)。
4. **名字契约与 subkind**: 合法/非法名、namespace 提取、推荐形态、`derive_subkind`。
5. **相位**: 序号、推导默认、允许集(含 `app+app` 判例、`service+core` 反例)。
6. **特权**: 各级别上限、正交表正反例、`BRV-PRIV-0001/0002` 三组反例。
7. **依赖方向**: 三条硬禁则 + `api_type` 禁则 + `allowedges` 只豁免方向表。
8. **导出面不变量**: `TAX-0016/0017/0018/0019` + skin 多提供者正例。
9. **TOML 模型加载**: 正例(含 `[[privileged.resources]]` 数组表、`value` 字符串数组、
   R-6 平台容量)、缺 name、3 段版本、4 段 range、相位不相容、subkind 冲突、重名、
   非法 TOML、跳过 `build/` 与 `.hidden/`、`[compat]` 缺 `core`、export 缺 `freeze_state`、
   `[privileged]` 缺 `level`。
10. **快照 / lock**: 快照读取、"未发布"、lock 往返逐字节一致、dependents 幂等。
11. **`plan-new` 四类**: 6 件计划 / 相位 / 模板路径 / 大小写占位符; 裸名 + `--subkind`;
    缺 `--subkind`、`subkind` 冲突、`runtime_adapter`(TAX-0014 + exit 2)、名字契约、
    人写冲突(`GEN-0002` exit 1)、生成物冲突 + `--force`。
12. **`plan-init`**: `product.toml` + `app/<name>/` + `brickie.lock`。
13. **发射器**: TOML 转义 / 数组 / 幂等 / 可被 `toml` 回解析。
14. **环与拓扑**(V-3): init 环 ⇒ `MF-0001` + 完整路径 + `data.cycles[].path/edges` + exit 1;
    runtime 环只报 info; `topo_order` 是**初始化序**(提供方在前)。
15. **相位单调**(V-8): R1 反例(`BRV-DEP-0009`)、R2 反例/正例(`BRV-DEP-0010`)、
    `sched-coop(core) → platform(core)` **不误杀**。
16. **版本冲突**(V-7): `range` 越界 `VER-0002`、`requires_iface` 的 `compat_gen` 冲突
    `VER-0001`、互斥区间 `DEP-0011`、缺依赖 `MF-0001`、预算差值。
17. **profile 门禁**(V-14): 同一输入 dev 绿(info `VER-0004`)/ release 红(error `VER-0004`)。
18. **`requires_iface` 不参与闭包**(V-10②): 加/去该字段 `closure` 的 `data` 逐字节相同;
    `iface show` 对它只报 info(V-10③)。
19. **跨文件导出不变量**(V-12): skin 指向不存在单元 / 分类不等 ⇒ `TAX-0018`;
    单元名碰撞 / 符号族碰撞 ⇒ `MF-0001`。
20. **独占资源与调度**(V-19④): IRQ/DMA 冲突**报双方**; `P2 + map` ⇒ `PRIV-0001`;
    非法 `(ops×granularity×region)` ⇒ `PRIV-0002`; `TT_SAFE` 缺 `[sched.tt]`;
    `COOP_ONLY` × `sched-preempt` 红 / × `sched-coop` 绿。
21. **IFACE-IR / hash 输入完整性**(V-17): `BR_MAX 16→4096`、service ops 加槽、枚举追加、
    枚举重排**各产生不同 hash**; `status` 不进 hash; 声明顺序不影响 hash; 域分隔;
    形参名/排版非 strict 不进 hash、strict 进 hash; typedef 展开等价。
22. **append vs modify**(V-15): 枚举末尾 ⇒ `EXTENDED`; 枚举重排/结构体加字段/service
    ops 加槽/func 签名/macro 值 ⇒ `CHANGED`; `is_hard` 只对已 `frozen`/`deprecated` 为真。
23. **publish 幂等**(V-5): 首次 3 类机器文件(`kind=machine`)+ 快照头 `NOT_ABI`/
    `hash_scope`/`truth`/`strict_params`; 第二次 `files=[]` + `noop`; 两次运行逐字节相同;
    `status --check` 一致/不一致两个方向。
24. **解冻窗口**(V-16): 无 `--note` 红; `frozen→unfreezing` 且快照记 baseline;
    窗口内 release ⇒ `IFACE-0009`(dev 绿); 空解冻 `refreeze` ⇒ 四段全不动;
    改已冻结条目后 `refreeze` ⇒ `COMPAT_GEN+1`。
25. **dep 命令面**: graph 三格式(含 `json` 的 `text` 可解析)、why 正/反、index 幂等。
26. **版本面命令**: `ver show` 四段/来源、`ver bump` 三 rule + `compat_gen` 非法 ⇒ 用法错、
    多 export 插件名 ⇒ 用法错(R-3)、`iface freeze` 的 A-26 提案(A-26)。


## 9. TODO(明确剩余项)

### 9.1 命令面
- [x] `check.rs`: 环 + 拓扑、依赖方向跨插件执法、相位单调、单版本、预算/独占、profile 门禁。
- [x] `solver.rs`: `closure` / `dep-tree` / `dep-graph`(dot/mermaid/json) / `dep-why` / `dep-index`。
- [x] `iface.rs`: `ver-show` / `ver-bump` / `iface-list/show/diff/status/publish/freeze/deprecate/undeprecate/unfreeze/refreeze`; IFACE-IR hash。
- [ ] `iface-publish --strict-params`: core 已支持(`args.strict_params`), 但 **L5 的
      argparse 没有该旗标** ⇒ 只能从 core 直接调; 需要时在 `cli.py` 加一行(不改 python/ 的纪律下未做)。

### 9.2 未做 / 无法实现(诚实清单, 见 §10 缺口)
- [ ] `SEMANTIC` 变更类别: v0.1 的声明面**没有纯语义变更的输入**(`sig/layout/value/ops`
      一个都不动 ⇒ 从 TOML 不可观测)⇒ `iface-diff` 永不产出 `SEMANTIC`,
      `BRV-IFACE-0010` 无可报(缺口 S-11)。
- [ ] 弃用周期的"两个 minor 无使用"计数: v0.1 只做**声明面零使用**硬门
      (`reexport_of`/`requires_iface` 引用检查); 符号面使用统计需 v0.2(§6.4, 缺口 S-16)。
- [ ] `check` 的数量约束(`app` 恰 1 / `platform` 恰 1 / `scheduler` 恰 1):
      有 product 时闭包内可数, 但 §3.2 的"每镜像恰 1"在**无 product 的全树模式**下
      无法判定"哪个镜像" ⇒ 未执法(缺口 S-17)。
- [ ] 出树到 `build/host/<triple>/bin`(G-6, 由仓库 Makefile 主人做)。
- [ ] `templates/` 定型后核对占位符(尤其 `descriptor_include`; 缺口 D-5)。

## 10. 规格缺口登记(需回灌设计/BRV-D8)

> 前 16 条(R-1…R-16)是**首刀**登记; S-1…S-17 是**第二刀**(solver/check/iface)被迫落笔处。

| # | 缺口 | 本实现 | 建议 |
|---|---|---|---|
| R-1 | `[plugin].phase` 单值表达两个完成点, 且允许集在文档间不一致 | 按父 agent 2026 澄清口径(§7 D-1) | 把允许集写进 §7.2/§8.1 |
| R-2 | 导出不变量 3 的"被再导出单元分类相等"是跨文件判定 | ✅ 第二刀已执法(`check` 的 iface 域, `BRV-TAX-0018`) | 在 §3.5 指明执法点 |
| R-3 | **依赖方向禁则无已分配码**(三条硬禁则) | `BRV-MF-0001`(R-9), 消息点名双方 | 补 `BRV-DEP-0012/0013/0014` 或 `BRV-TAX-0001..` |
| R-4 | `lang` 模板未交付 / 未知 `lang` 无码 | 无码用法错 + exit 2 | 补码或明确归 `MF-0001` |
| R-5 | `[[dep]].phase` 断言语义矛盾(§7.2 写 `rank(assertion) ≤ rank(provider)`, §8.1 写"提供方不晚于断言") | ✅ 已裁定, 见 **S-6** | 二选一并写死 |
| R-6 | `sched_class = COOP_ONLY` × `sched_kind = preempt` 冲突无码/无条文 | ✅ 第二刀已执法(无码, 用 `BRV-MF-0001`) | §2 第 3 项补码 |
| R-7 | `lock` / `index` 的 schema 未落地 | ✅ 已对齐(`render_lock`/`load_lock`/`render_dependents`) | ✅ 关闭 |
| R-8 | 描述符 `{{descriptor_include}}` 无语义 | 取 `<short>/<short>.h` | `3-05` §2 定稿 |
| R-9 | `templates/` 占位符大小写两套并存 | 两套都发 | 统一模板变量命名 |
| R-10 | `plan-init` 的 lock 内容 vs "plan 不产内容" | 计划 + `data.lock_content` | contract §5.2 补一句 |
| R-11 | `gen-plan` 的"头文件"归属 | 只出描述符 | contract §5.2 澄清 |
| R-12 | 数量约束(app 恰 1 / platform 恰 1 / scheduler 恰 1)与 R-1 分支 | 未执法(缺口 S-17) | 明确"每镜像"在无 product 时的含义 |
| R-13 | 重复 `[[dep]]` 无码 | 不报(第二刀沿用) | 可选补 info 型码 |
| R-14 | "无码用法错"的码义务(G-5) | 保留无码; 形状类一律 `MF-0001` | BRV-D8 明确 |
| R-15 | response JSON 键序未规定 | serde_json 字典序 | 若要逐字节快照, 写进 contract |
| R-16 | 跨 agent fixture 与澄清口径冲突(`app` + `phase="late"`) | `model` 报 `BRV-MF-0001`(exit 2) | 该 fixture 应改成 `phase = "app"` |
| **S-1** | `requires_iface` 的版本校验放在哪: V-10② 要求"闭包逐字节相同", V-7 要求"`compat_gen` 冲突可构造" | `closure` **不扫描**(`scan_iface=false`); `check --deps` 扫描并报 `VER-0001/0002/0004` | contract §5.1 给 `closure` 补一句"不评估接口依赖" |
| **S-2** | 结构依赖(`init/runtime/type`)**不钉代**, 那 V-7 的 `range` 越界比什么 | 比**提供方插件的四段版本**的 `MAJOR.MINOR.REVISE` ⇒ `VER-0002` | §7.4 写明"结构依赖 `range` 作用于提供方插件版本" |
| **S-3** | "同插件多版本共存 ⇒ `DEP-0011`"在"一插件一声明"下不可构造 | 取**多个依赖方区间约束的交集为空**为构造形式 ⇒ `DEP-0011` | §7.4 补"区间交集为空"的判据 |
| **S-4** | §7.1 表写 `type` 边"闭包参与 ✅(但不拉入运行期依赖)" | `type` 边只把提供方拉进闭包, 不沿其 `init`/`runtime` 边扩张 | §7.1 写清闭包扩张规则 |
| **S-5** | 环结构化形状: contract 只写 `cycles:[]` | 每个元素 `{path:[...], edges:[{from,to}]}`(V-3 要"结构化边列表", present 也要 `path`) | contract §5.1 给出形状 |
| **S-6** | `[[dep]].phase`(R2)方向矛盾(§7.2 vs §8.1) | 取 §8.1 口径: **提供方自述晚于断言才红**(`rank(provider) > rank(assertion)`) | 写死一条 |
| **S-7** | `check` 的 `args.plugin?` 语义未定义 | 按**诊断的 `target`** 过滤(即"只看这个插件报的"), 不是"只算它的闭包" | contract §5.1 说明 |
| **S-8** | 首次 `publish`(无旧快照)是否推进段 | **建档**: 只记录当前声明版本与 hash, 不推进任何段; 之后才按变更集推进 | §6.5 补"首次发布"行 |
| **S-9** | 状态机命令(`deprecate`/`undeprecate`)与 `plugin.toml` 只读冲突 | 合法性按**快照状态**判, 新面按声明面构造 ⇒ 重复执行是空操作; `plugin.toml` 应改成什么以 info 报出 | §6.5 补"人写面同步"的说明 |
| **S-10** | `publish --profile release` 的"依赖方仍匹配"门钩会让**任何** `COMPAT_GEN` 跃迁在 release 下被拒 | 按 §6.5 表执行(`BRV-VER-0001`), 并在 hint 里提示先协调依赖方 | §6.5 说明"跃迁需依赖方一起改" |
| **S-11** | `SEMANTIC` 变更在 v0.1 无输入(纯行为语义不可从 TOML 观测) | 不产出 `SEMANTIC`; `BRV-IFACE-0010` 无可报 | §6.3 注明"v0.1 的 SEMANTIC 由人工声明/不可观测" |
| **S-12** | `iface status` 的 `files:[若需回写]` 语义 | 门禁**不写盘**; 不一致时把应有的快照放 `data.expected_snapshot` | contract §5.1 明确 |
| **S-13** | §7.3 app 行"ability ✗" + §10 V-12② "`app → service/sqlite` 必须通过" | 取 V-12: `app` 可依赖 `api_type = third_party` 的 ability(即"不得直调 **native**"的精确形式); `rules::dep_direction_ok` 的 app 分支加该例外 | 把例外写进 §7.3 表 |
| **S-14** | R-6 "platform 的 `[[res]]` 同时是容量"若**同时进 Σ**, 物理上限恒红(§8.1 注①失去意义) | `platform` 的 `[[res]]` 作 `platform_capacity`, **不**进消费者侧 Σ(`totals.ram_kib` = 非 platform 之和) | 明确 R-6: 平台声明的是**提供量**, 不是自身耗用 |
| **S-15** | `unfreeze` 的 baseline 取"上次发布的冻结面"还是"解冻时的声明面" | 取**上次发布的快照条目**(承诺的真值); 解冻窗口内未发布的改动会在 `refreeze` 时按"改已冻结条目"计入 | §5.3.4 补 baseline 定义 |
| **S-16** | 弃用周期的"两个 minor 无使用"在 v0.1 无符号级输入 | 只做**声明面零使用**硬门; 符号面在 release 下降级为 warning 的能力**未实现**(无符号级数据) | §6.4 明确 v0.1 只做声明面 |
| **S-17** | `app`/`platform`/`scheduler` "每镜像恰 1"的数量约束在无 `product.toml` 的全树模式下无法判"哪个镜像" | 未执法(有 product 时闭包内可数, 但为避免误杀也未启用) | §3.2/R-1 明确无 product 时的数量语义 |
| **S-19** | 解冻窗口内 `publish` 与 `refreeze` 的职责重叠: 若两者都按"相对 baseline 的硬变更"bump, 会双 bump; 若 `publish` 按声明面重建快照, 又会把 `unfreezing` 抹成 `frozen`(窗口丢失) | `publish` **保留有效冻结态**(声明 OR 快照)并把本次发布的面前推为**新基线**; `refreeze` 随后是"空解冻"(四段不动) | §5.3.3/§6.5 写清"谁记录变更、谁关窗口" |
| **S-18** | 缺口代用的信息型诊断(如"代不参与比较"/"NOT_ABI"/"应改成什么")借用 `BRV-MF-0001`(error 域)但写 `info` | 严重度与码域不一致, 但机读消费方仍能按 severity 分类 | BRV-D8 补一个 info 型通用码 |

## 11. 修改纪律

- **判定只在本 crate**: 别把闭包/区间/拓扑/分类学/特权/版本推进搬到 Python。
- **码只从 BRV-D8 取**: 未分配码的缺陷用 `BRV-MF-0001`(R-9)**并更新 §10 的缺口表**。
- **机器文件无时间戳/随机/主机信息**; 新序列化一律走 `emit.rs` 的原语(别另写第二份)。
- **`plugin.toml` / `product.toml` 只读**: 任何命令都不得改写(§9.1.1 规则 1);
  `new`/`init` 只在文件**不存在**时创建。
- 改完跑 `cargo build --release --locked && ./target/release/brickie-core --selftest`。

## 12. 与 L5(Python)的字段对齐

`python/**` 未改(纪律)。对齐情况以上了真实 CLI 的端到端跑通为准:

| 命令 | L5 读取的字段 | core 提供 | 结论 |
|---|---|---|---|
| `check` | `data.summary.{errors,warnings,infos,profile,closure{plugins,ram_kib,stack_kib}}`、`data.scope_results` | ✅ 同名字段 | 对齐 |
| `dep closure` | `data.{plugins[].{name,version,compat_gen,plugin_type,phase,ram_kib,stack_kib,selected}, topo_order, totals{...}, cycles[].path, profile}` | ✅ | 对齐 |
| `dep tree` | `data.{roots, nodes{<n>{deps[{name,kind,range,phase}]}}, kind}` | ✅ | 对齐 |
| `dep graph` | `data.{format, nodes, edges, text}` | ✅ | 对齐 |
| `dep why` | `data.{found, path, edges[{from,to,kind}]}` | ✅ | 对齐 |
| `dep index` | `data.dependents{<plugin>:[{from,kind}]}` | ✅ | 对齐 |
| `ver show` | `data.{id,version,segments,plugin,source,status,freeze_state,published,compat_gen_source{kind,note}}` | ✅ | 对齐 |
| `ver bump` | `data.{id,from,to,rule,segments,version}` | ✅ | 对齐 |
| `iface list` | `data.units[].{id,api_iface,form,version,compat_gen,freeze_state,status,published,hash}` | ✅ | 对齐 |
| `iface show` | `data.{unit{...}, entries[].{kind,name,status}, source, not_abi, truth, hash_scope}` | ✅ | 对齐 |
| `iface diff` | `data.{id,verdict,version{from,to,reasons},compat_gen_changed,changes[].{kind,name,entry,from,to},dependents{direct,transitive,unsatisfied}}` | ✅ | 对齐 |
| `iface status` | `data.{id,consistent,recomputed_hash,snapshot_hash,declaration_hash}` | ✅ | 对齐 |
| `iface publish` | `data.{id,noop,version,from,to,compat_gen,verdict,published,report.dependents}` | ✅ | 对齐 |
| `iface freeze/deprecate/undeprecate/unfreeze/refreeze` | `data.{id,entry,proposal,freeze_state,status,compat_gen}` | ✅ | 对齐 |

**已知差异(不影响 L5 编排)**:

1. L5 的 `--json` 外层信封用 `{protocol,status,command,exit_code,diagnostics,data,written}`,
   而 contract §5 的 core 响应用 `{protocol,status,exit_code,diagnostics,files,data}` ——
   L5 是**呈现层**, 把 `files` 换成"实际落盘清单 `written`"。**core 侧保持 contract 形状**,
   未改 `python/`。
2. `dep tree` / `dep graph` 的 `nodes` 里 core 额外给了 `file`/`phase`(L5 忽略) —— 纯增量。
3. `iface publish` 的 `data.changes[].from/to` 对 `ADDED`/`REMOVED` 可能是 `null`(L5 用
   `is not None` 判断, 已兼容)。
4. `check` 的 `data.scope_results` 是**对象**(键 = 域), 另含 `manifest` 伪域 —— L5 的
   `present.py` 两种形状都支持(dict / list)。
5. L5 的 `iface publish --strict-params` 旗标缺失(见 §9.1) —— core 已支持该 `args` 键。
