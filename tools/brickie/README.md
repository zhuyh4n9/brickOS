# brickie — brickOS 组合期工具

> **`brickie check` 是"声明面完备性检查器"**: 它能保证"这个组合在**逻辑上**自洽",
> **不**保证"这个组合**编得过 / 跑得对**"(`brickie-v0.1.md` §2)。
>
> 这句话是设计文档点名要写进 CLI 帮助与 README 的 —— 目的是避免 v0.x 用户误信。

设计文档(**权威**, 在 `brickOS-Design` 分支, 本分支没有 `docs/`): 按章节号读
`docs/2-toolchain/brickie/brickie-v0.1.md`(下文写作 `§x.y`)。
进度与工单: `docs/2-toolchain/brickie/checklist.md`。
命名决策: `docs/decisions/0001-brickie-tool-naming.md`(**本工具叫 `brickie`, 不叫 `br`**;
但 `br_*` / `BR_*` / `.br_plugins` 等**符号前缀是已分配标识符, 不随工具改名**)。

**跨语言接口的唯一权威是本目录的 [`docs/contract.md`](docs/contract.md)**: 三个进程的
JSON 信封、诊断模型、文件归属、逐命令的 `args` 与 `data`/`schema` 字段表, 以及实现期
裁定 **R-1…R-12**, 都在那里。本文件只描述**本仓库的现状与分工**, 协议本身不在此复制
一份(复制出来的第二份真值正是契约要消灭的东西)。

---

## 1. 现状(v0.1 命令面)

**L5(Python 前端)已全量交付**: `python/brickie/` 有 **29 条叶子命令 + 2 条全局开关**
的完整 argparse 命令面, 编排 / 落盘 / 呈现 / 形状校验 / 退出码汇总逐条落地。按用途分
**两条族 + 2 条全局开关**(构建族是 ADR-0004 长出来的第二族):

| 族 | 命令 | 作用 |
|---|---|---|
| **骨架** | `new <ptype> <name>` · `init <product>` · `gen [--check]` | 生成插件/产品骨架; 重建 `build/gen/**` 生成物 |
| **依赖与校验** | `dep {add,rm,tree,graph,why,index,closure}` · `check [--deps\|--iface\|--tax\|--priv\|--all] [--profile dev\|release]` | 闭包/树/图/路径/反向索引 + 四域完备性 |
| **版本与接口** | `ver {show,bump}` · `iface {list,show,diff,status,publish,freeze,deprecate,undeprecate,unfreeze,refreeze}` | 四段版本 + 单元快照 / 冻结窗口治理 |
| **构建族(第二族)** | `build [--profile\|--jobs\|--backend\|--tag\|--dry-run\|--force\|--emit-backends\|--no-post]` · `clean [--all]` · `run` · `size` · `disasm` · `test [<name>\|--list]` | 声明面驱动的构建编排 + 执行 + 门禁(build/clean/run/size/disasm/test; core 侧另有 `judge` 只服 L5 内部调用) |
| **全局开关** | `--version [--deps]` · `--json`(另有 `--root DIR`) | 指纹 / 机器可读输出 / 换根 |

> **v0.1「边界纪律」的适用面(裁定 R-13, contract §9)**: `brickie-v0.1.md` §0 写
> "v0.1 任何命令**不得要求 cc/cargo/nm 在场**" —— 这句只约束**组合期**命令
> (骨架 / 依赖与校验 / 版本与接口三族), 也就是"声明面完备性检查器"那一半。
> **构建族是显式第二族**: 它按同一个声明面组合镜像并驱动交叉工具链,
> "要求工具链在场"是它的**职责**, 不是纪律的例外条款。CI 的组合期作业仍然零编译依赖
> (`make tools` / `brickie check` 不碰交叉工具链)。

**实现状态**(前端 = L5 的参数解析/编排/呈现; 原生侧 = 真正做判定的进程):

| 命令 | L5 前端 | 原生侧 | 说明 |
|---|---|---|---|
| `new` / `init` / `gen` | ✅ | `core plan-new` / `plan-init` / `gen-plan` ✅ + `gen render` ✅ | 两段式: core 给计划(路径+模板+变量), gen 出内容, L5 落盘 |
| `dep add` / `dep rm` | ✅ | `core dep-add` / `dep-rm` ✅ | **只打印片段, 不写盘**(裁定 R-2) |
| `check` | ✅ | `core check` ✅ | 四域 `deps/iface/tax/priv` + `summary`/`scope_results`; `--profile` 门禁 |
| `dep tree/graph/why/index/closure` | ✅ | `core dep-*` / `closure` ✅ | 闭包是**纯函数**(环报完整结构化路径; 预算合计带 used/容量/预算三个数) |
| `ver show` / `ver bump` | ✅ | `core ver-show` / `ver-bump` ✅ | 版本载体 = **单元快照**; 人写的 `plugin.toml` 只读(裁定 R-3) |
| `iface *`(10 条) | ✅ | `core iface-*` ✅ | IFACE-IR + sha256 + 变更集 + 影响报告 + 冻结窗口(含 `unfreezing` 瞬态) |
| `--version [--deps]` | ✅ | `core version` ✅ | core 缺席时 `--version` 降级打印前端版本 |
| `build` / `clean` / `run` / `size` / `disasm` / `test` | ✅ | `core build/clean/run/size/disasm/test/judge` ✅ | **第二族**(ADR-0004 / 设计 ADR-0003 的 S1–S4): 声明面驱动构建 + L5 执行器(`runner.py`, 并发/超时/日志/进度); 工具解析 / 增量 / 门禁判据全在 core(裁定 R-14/R-15/R-16) |

> **判定在哪一侧**: 全部**判定**与 BRV 码都在 `rust/`(L0/L1); L5 只做参数解析、编排、
> 落盘、形状校验、呈现与退出码汇总(V-18c 的静态检查会抓越界)。原生侧的领域自检:
> `brickie-core --selftest` —— **485 cases**(`make core` 后跑 `build/host/<triple>/bin/brickie-core --selftest`),
> 它**不需要 Python 解释器参与**(V-18a)。端到端回归见 `tests/run.sh`, 施工图 =
> `tests/ACCEPTANCE.md` 的 V-1…V-19 矩阵 + 构建族 §V-B。

**明确不做**(设计 checklist §5.4; 这些名字**一条都不在** `--help` 里, 反向验收会抓):
`api-dump` / `verify` / `iface check` / `dbg` / `pack`, 以及旧名 `add` / `show` / `env`。

> `build` / `test` / `run` 原在 §5.4 的"明确不做"名单里 —— 它们是 ADR-0004(设计
> ADR-0003 的 S1–S4)**后来交付**的第二族, 所以从禁名名单里移除; 反向验收(`N-1`/`N-2`)
> 已按新名单更新。

## 2. 目录结构(对照 §9.2)

```
tools/brickie/
├── Makefile              工具自身的构建入口(委派 cxx/ 与 cargo; 不重复编译规则)
├── pyproject.toml        Python 包(brickie)与入口点
├── freeze.py             打包器: python/** + templates/** + schema/** → 未压缩 tar → C 数组
├── python/brickie/       L5 前端(命令面 / 编排 / 落盘 / 呈现 / 形状校验)+ runner.py(构建族执行器)
├── rust/                 brickie-core: L0 模型 + L1 求解/版本/接口引擎 + 构建规划器(唯一判定处)
├── cxx/                  L2 渲染器(main/render)+ L5 入口 ELF(launcher)
├── templates/            骨架/描述符/产品模板(数据, 不是代码)
│   ├── native/<plugin_type>/<lang>/     骨架: plugin.toml / src.c / include.h / smoke.toml / README.md
│   ├── descriptor/<api_type>/<lang>/    描述符(与 plugin_type 正交)
│   └── product/<lang>/                  产品骨架
├── schema/*.schema.json  声明面**单条记录形状**的机器可读描述(plugin/product/lock/index/gates, 5 份)
├── docs/contract.md      ★ 跨语言接口契约(唯一权威; 见 §3)
└── tests/                run.sh(端到端)+ ACCEPTANCE.md(V-1…V-19 验收矩阵 + §V-B 构建族)
```

**产物出树**(参考 Android 的 `out/host/...`; 由仓库根 `mk/host.mk` + `tools/host-detect.sh`
决定)。源码树里只放源码, 编出来的东西一律在仓库级 `build/` 下:

```
build/host/<host-arch>/<host-os>/bin/brickie       ← L5 入口 ELF(嵌入载荷)
                              …/bin/brickie-gen    ← L2 渲染器
                              …/bin/brickie-core   ← L0/L1 Rust 核心
                              …/lib/libbrickie-gen.a
                              …/obj/cxx/*.o
                              …/share/brickie/payload.tar   ← 嵌进 brickie 的载荷(供检查)
```

`<host-arch>` = `x86-64` / `aarch64` / …, `<host-os>` = `linux` / `darwin` / `win`。
前端 `native.py` 按同一口径找两个原生工具(`BRICKIE_CORE` / `BRICKIE_GEN` /
`BRICKIE_HOST_ARCH` / `BRICKIE_HOST_OS` 可覆盖), 不依赖源码树内的旧落点。

**三个可执行的分工**(contract §1): `brickie`(C++ 启动器 + 嵌入载荷)、`brickie-core`
(Rust)、`brickie-gen`(C++ 渲染器)。入口 ELF 是**单文件自包含**形态: `freeze.py` 把

- `python/brickie/**`(L5 前端)、`templates/**`(骨架模板)、`schema/**`(形状门输入),
- **原生工具**(`cxx/Makefile` 的 `PAYLOAD_EMBEDS`: `brickie-gen` 必嵌, `brickie-core`
  就位时条件嵌),

打成未压缩 ustar tar 后作为数据嵌入; 运行时解包到临时目录, 用系统 `python3` 解释嵌入
代码, 并把 `BRICKIE_GEN` / `BRICKIE_CORE` 指到**解包出来的**原生工具。于是:

- 产物是真 ELF(魔数 `\x7fELF`), 与独立副本 `brickie-gen` / `brickie-core` 一起进种子;
- **只拷 `brickie` 一个文件**即可运行 —— 不需要 `PYTHONPATH`、不需要源码树、
  不需要同目录的原生工具、不需要 `g++`/`cargo`;
- **零新增第三方依赖**(不用 PyInstaller/Nuitka), 载荷逐字节可复现。

**自举种子**(`make prebuilt` 发布; 与 `build/host/**` 同构但**进版本库**):

```
prebuilts/seed/brickie/<host-arch>/<host-os>/bin/brickie        ← ★ 单文件自包含
                                           /brickie-gen    ← L2 渲染器(冗余副本, 供开发态直用)
                                           /brickie-core   ← L0/L1 核心(冗余副本)
```

`brickie` 是"没有 `g++`/`cargo` 的全新 checkout 也能跑"的那一份, 也是将来 brickie
自举管理自身编译的起点。开发态前端查找顺序(`native.py` 的 `_find_tool`):
`$BRICKIE_GEN`/`$BRICKIE_CORE` → `build/host/...`(本机刚编的) → `prebuilts/...`(种子)
→ `PATH`(每档都带 `*/*` 兜底)。详见 [prebuilts/README.md](../../prebuilts/README.md)。

## 3. 三语言分工与协议指针

| 层 | 语言 / 位置 | 职责 | 不许做 |
|---|---|---|---|
| **L0 + L1** | Rust · `rust/`(`brickie-core`) | 模型 / 求解 / 版本 / IFACE-IR / 分类学 / 特权 / 预算 —— **一切判定与错误码**, 以及"机器拥有的文件"的**内容** | 写盘(内容交给 L5 落盘) |
| **L5** | Python3 · `python/brickie/`(`brickie`) | argparse / 找二进制 / 调子进程 / 读写文件 / JSON 序列化 / 按 `schema/*.json` 做**单条记录形状**校验 / 文本呈现 / 退出码汇总 | 任何业务判定; 任何机器文件的内容拼接(contract §8) |
| **L2** | C++ · `cxx/`(`brickie-gen`) | **只做模板渲染**: `templates_root` + `artifacts[{path,template,vars}]` → `files[{path,kind:"rendered",content,mode}]` | 一切判定(名字契约/相位/路径布局/版本全在 Rust) |

**协议与裁定一律查 [`docs/contract.md`](docs/contract.md)**, 不在别处另立一份:

- §1 三个宿主可执行 · §2 信封与协议版本(`BRV-PROTO-0001/0002`) · §3 诊断(含构建族码
  `BRV-BLD-0001..0012`) · §4 文件归属(§9.1.1 写路径纪律) · §5 `brickie-core` 命令与
  `args`/`data` 字段表(**§5.4 = 构建族七条** `data.steps[i]` / `data.tools[i]`) ·
  §6 `brickie-gen` 命令 · §7 声明面 schema(**§7.4 = `tests/gates.toml` 形状**) ·
  §8 L5 边界 · §9 实现期裁定 R-1…R-16。

**三条最容易被写错、所以单独点名的纪律**:

1. **写路径**: `plugin.toml` / `product.toml` 是**只读输入**, 永不被改写; 机器只写
   `api/iface/**`、`brickie.lock`、`build/index/**`、`build/gen/**`(contract §4)。
2. **`dep add` / `dep rm` 不落盘**(裁定 **R-2**): 设计 §7.7 说"写 `plugin.toml`", 但
   §9.1.1 规则 1 说永不改写人写文件 —— 取后者。命令只**打印**可直接粘贴的 TOML 片段
   (`data.snippet`)。
3. **构建族里 L5 只执行、不判定**(裁定 **R-14/R-15/R-16**): 用哪个编译器、要不要重编、
   日志算红还是绿, 全部由 core 给; `runner.py` 只做"校验可执行 / 并发 / 超时 / 收日志 /
   呈现进度"。连编译失败用的码都是 core 随步骤给的(`steps[i].fail_code`)。

## 4. 构建与测试

```sh
make                 # = make cxx: 两个宿主 ELF(brickie-gen L2 + brickie L5 入口);
                     #   brickie-core 若已在出树落点则条件嵌入, 否则提示后继续
make core            # cargo 编 L0/L1 的 brickie-core → build/host/<triple>/bin/brickie-core
make selftest        # brickie-gen --selftest: JSON 往返 / 渲染 / 路径与模式 / 载荷确定性
make launcher-smoke  # 入口 ELF 自检: 嵌入载荷能不能起来(brickie --version)
make test            # 渲染器 selftest + tests/run.sh 端到端(V-1…V-19 + §V-B 构建族 / 命令面全覆盖 / 纯度 / 快照)
make prebuilt        # 发布**三件**种子 {brickie,brickie-gen,brickie-core}(cmp 相同则不写盘)
make prebuilt-check  # 三件是否落后于源码(不改盘)
```

工具自身的构建入口**只编工具**; 镜像由 brickie 编(ADR-0004 / 设计 ADR-0003 的 S1–S4):

```sh
build/host/<triple>/bin/brickie build            # 镜像入口: 组合期 check → gen → 编译 → 链接 → [build].post
build/host/<triple>/bin/brickie build --dry-run  # 只打印计划(不需要编译器; 用例 V-B 用它)
build/host/<triple>/bin/brickie test             # 跑 tests/gates.toml 的全部门禁
build/host/<triple>/bin/brickie clean --all      # 清镜像侧派生物(不动 build/host 的工具)
```

- **cargo 出树**: `CARGO_HOME` / `CARGO_TARGET_DIR` 缺省指向
  `prototype/build/{cargo-home,cargo-target}`(受管环境下 `$HOME/.cargo` 可能只读),
  两个都可用环境变量覆盖; Rust 源码在 `rust/`, 产物拷到宿主 bin 落点。
- **core 降级、门禁不降级**: `make` / `make cxx` / `make selftest` 在 Rust 未产出时
  **照常成功**(载荷暂不含 core, 只打印提示); 而 `make prebuilt` 把 `brickie-core`
  当**必需件** —— 缺 core ⇒ FAIL(自举种子必须三件齐), `prebuilt-check` 同样查三件。
- **仓库根委派**: `make tools`(只编 L5/L2)、`make tools-core`(只编 Rust 核心)、
  `make tools-test`(工具用例)、`make brickie-check` / `brickie-check-release` /
  `make brickie-compose`(用入口 ELF 校验 / 重建生成物)。
  **镜像侧已交给 brickie**(ADR-0004 / 设计 ADR-0003 的 S1–S4): 顶层 `make` 只编工具;
  `make all` / `run` / `smoke` / `irq-test` / `mem-test` / `string-test` / `dbg-test` /
  `check-string` / `size` / `disasm` / `clean-brickos` 都是**薄委派**(调
  `$(BRICKIE) build|run|test …|size|disasm|clean`), 真身在 `brickie build` 与
  `tests/gates.toml`。镜像的源码集合 / 标志 / 规则**不再**出现在顶层 Makefile 里 ——
  由 `product.toml [build]` + 各插件 `[build]` + platform 的 `[build.target]` 决定,
  `make check-build` 的第 ② 条不变量会抓任何回退。
  声明面红 ⇒ **`make` 在 ⓪ 止步**(退出码 1), 镜像不编 —— 这是纪律本身, 不是意外;
  只编工具用 `make tools` / `make tools-core`。

两种用法:

```sh
# (a) 入口 ELF: 自带 Python 代码、模板、schema 与两个原生工具
build/host/<host-arch>/<host-os>/bin/brickie check --root .
# (b) 开发态: Python 源码直跑, 前端自己去 build/host/ 找原生工具
python3 -m brickie check --root .   # 需 PYTHONPATH=python(或已装包)
# 也可显式指定
BRICKIE_CORE=/path/to/brickie-core BRICKIE_GEN=/path/to/brickie-gen python3 -m brickie gen
```

## 5. 诊断与退出码

退出码继承 `2-02` BR-D7 / 本篇 BRV-D9:

| 码 | 含义 | v0.1 |
|---|---|---|
| 0 | 成功 | ✅ |
| 1 | 校验红(环 / 冲突 / 预算 / 版本 / 分类学 / 特权); **构建族**里步骤失败 / 门禁判红也折进这一档 | ✅ |
| 2 | 用法或环境错(参数非法 / schema 不符 / 路径不存在 / 模板不可用 / 原生件缺失或协议不符 / 命令尚未实现 / 构建族找不到编译器或 QEMU) | ✅ |
| 3 | 编译或运行失败 | ✗ **仍不用**: 构建族把"编译失败"折进 1(`BRV-BLD-0011`), 环境错折进 2(`BRV-BLD-0005`) —— 与 BRV-D9 的"三档"一致, 不新增第四档 |

**严重度不决定退出码**: `severity ∈ {error,warning,info}` 只描述"这条信息有多重",
退出码由"命令有没有完成"决定。两个已登记的判例: `BRV-TAX-0014`(所选模板 v0.1 未交付)
是 `info`, 但**一件文件都没生成** ⇒ 退出码取 2; 用法/环境错(未知 `plugin_type`、
缺 `--subkind`、名字契约违例、schema 形状不符…)带诊断但 **`code: null`** —— 它们是
"用法错"这一档, 不是声明面诊断。

原生工具的**进程**退出码恒为 0(除 `internal_error` 与崩溃): 业务结论只在响应的
`exit_code` 里 —— 这样"工具故障"与"校验红"不会混成一件事(contract §2)。

已交付路径会发出的诊断(码全部取自设计 BRV-D8 的**已分配编码表**, 未自造):

| 码 | 严重度 | 触发 |
|---|---|---|
| `BRV-TAX-0013` | warning | 插件名不符推荐形态 `<namespace>/<short>`(§8.3; 只对新增插件) |
| `BRV-TAX-0014` | info | 所选 `api_type` 模板 v0.1 未交付(`runtime_adapter` / `third_party`) |
| `BRV-TAX-0015` | error | `--subkind` 与按 namespace 的推导冲突(§8.4) |
| `BRV-GEN-0002` | error | 生成目标已存在且非生成物目录, 或目标是**人写文件**(§8.4) |
| `BRV-MF-0001` | error | 声明面必填字段缺失 / 字段形状不符 schema(形状门, 退出码 2) |
| `BRV-BLD-0001..0012` | error(全绿判据是**无码 info**) | 构建族: 缺 `[build]` / 空 sources / 缺 `[build.target]` / 目标二义 / 工具缺失(退出码 2) / 单元缺 `[build]` / 字面源不存在 / 门禁名未知 / 门禁判红 / `[build]` 形状 / 步骤失败 / 正则超子集。逐条见 contract §3 |

## 6. 与设计文档的已知偏差 / 缺口(需要回灌)

| # | 设计文档说 | 本实现 | 为什么 |
|---|---|---|---|
| D-1 | §9.2: `cxx/ # CMake: libbrickie-gen + brickie-gen` | 用等价的最小 **Makefile**(库/可执行两个产物的划分不变) | 本机无 cmake; 不引入构建系统依赖(与 §9.3 的"最小依赖集"同向) |
| D-2 | §9.2: `templates/ # 骨架模板: <api_type>/<plugin_type>/<lang>/` | 骨架模板照此; 描述符另放 `templates/descriptor/<api_type>/<lang>/` | 描述符与 `plugin_type` **正交**(只随 `api_type` 与 `lang` 变), §9.2 的路径没覆盖这一维 |
| D-3 | §13.2 的 A-3 / A-14 / BRV-Q7(未回灌); `1-01` §6.1 仍写 `ver[3]` | `plugin.toml` 与生成物**按四段版本** `COMPAT_GEN.MAJOR.MINOR.REVISE`(如 `0.1.0.0`) | 三段的描述符**表达不了**声明面; 生成物头部已把这条假定写明 |
| D-4 | `4-02:29` 的宏形态矛盾(static vs 非 static)未定稿 | 生成物取 **`static const` + `.br_plugins`**(`3-01` §13.3 / CA-10), 并在头部注明备选 | 段收集只需 `used`+`section`, 不需外部链接; CA-10 是**已采纳的决策记录**且明写"插件导出面近零" |
| D-5 | `3-05` §2 未定稿描述符头文件名 | 生成物 `#include "br_plugin.h"`, 并在头部注明未定稿 | 任何文档都没有给出文件名; 生成物**不被编译**(V-9), 它现在是声明面产物 |
| D-6 | §9.2 / BRV-D5 **未规定工具自身原生件的落点**(只给了源码树) | 产物**出树**: `build/host/<host-arch>/<host-os>/{bin,lib,obj}`(参考 Android 的 `out/host/...`) | 需求方硬约束(工具 bin 不落源码树); 已回灌 BRV-D5 表 + §9.2 注 + `2-02` BR-D5, checklist §5.6 登记为 **G-6** |
| D-7 | §9.2 / BRV-D5 **未规定"随源码提交的预编译件"**(只说了"生成物不进库") | **自举种子进库**: `prebuilts/seed/brickie/<host-arch>/<host-os>/bin/{brickie,brickie-gen,brickie-core}`(`make prebuilt` 发布) | 需求方硬约束(为"没有编译器也能起步"与将来 brickie 自举管理自身编译留种); 已立 ADR **0004** 并回灌 BRV-D5 表 + §9.2 + `2-02` BR-D5, checklist §5.6 登记为 **G-7** |
| D-8 | §9.1/§9.2 把 L5 定为 **Python3** 进程(隐含"要解释器 + `PYTHONPATH`"), 且把 L2 生成器当作**独立可执行** | **Python 前端与原生工具编译进同一个 ELF**: 自写 `cxx/launcher.cpp` + `freeze.py`, 把 `python/brickie/**`、`templates/**`、`schema/**` 与两个原生工具(载荷 `bin/brickie-gen`、`bin/brickie-core`)全部嵌进 `brickie`(单文件自包含; 运行时仍用系统 `python3`, 环境变量指向解包件) | 需求方硬约束("brickie 的 python 代码也需要编译为 elf" + "brickie-gen 等工具均需要编译到 python 的 brickie elf 中"); 选零依赖入口而非 PyInstaller/Nuitka, 是为守住 §9.3 的"Python 零第三方依赖"纪律。已回灌 ADR **0004 §7** 与 checklist §5.6 **G-8** |
| D-9 | §7.7 说 `dep add` / `dep rm` "写 `plugin.toml`" | **只打印 TOML 片段, 不写盘**(裁定 **R-2**); 覆盖写会抹掉注释, 且违反 §9.1.1 规则 1 | 人写文件是**唯一真值**; 切换成本 = 将来加 `--write`(contract §9 R-2) |
| D-10 | 接口状态机的 `--note` **语义未裁定**(路径还是正文) | 前端按 CLI 口径把 `--note` 的值当**路径原样**传给 core(正文读取与校验归 core) | 与"参数即路径"的 CLI 一致性; 若核心改为收正文, 在 `cli.py` 的 `iface` args 一处加 `note_text`(单点切换) |
| D-11 | BRV-D8 **未给下列缺陷分配专属码**: 生成物与重算不一致 / 渲染缺变量 / 依赖方向硬禁则 / 预算与独占冲突 / 重复 `[[dep]]` … | 按"就近"兜底: 形状与声明面错误用 `BRV-MF-0001`(消息里点名双方/差值), 其余用 **`code: null` 的用法错**(`exit_code 2`); **不自造码** | 编码表是设计资产, 自造码会让"码 → 规格"的映射失真; 缺口已由 L5/Rust 两侧分别登记, 待补码后逐一收口 |
| D-12 | `[[dep]].phase` 的断言语义**自相矛盾**(§7.2 写 `rank(assertion) ≤ rank(provider)`, §8.1 写"提供方不晚于断言") | v0.1 **不执法**这一条(只做单记录形状与允许集校验), 等规格二选一后写死 | 两种读法结论相反, 先选一种会把另一半判例判错; 已登记缺口并在 `rust/README.md` 的缺口表可见 |
| D-13 | §0: "v0.1 **任何命令**不得要求 cc/cargo/nm 在场"; §7.1 注: `plugin.toml [build]` "v0.1 只记录, v0.3 起被消费" | **构建族是显式第二族**, 提前消费 `[build]`/`[build.target]`(ADR-0004 / 设计 ADR-0003 的 S1–S4); 边界纪律按"只约束**组合期**命令"读(裁定 **R-13**) | 需求方要求 `brickie build` 接管镜像入口; 两族职责不同, 一句话管两族会自相矛盾。已回灌 contract §9 R-13 + `rust/README` S-B* |
| D-14 | BRV-D8 **没有 BUILD 族** | 新开 `BRV-BLD-0001..0012`(逐条见 contract §3), 未自造形状之外的语义 | 门禁判据 / 工具解析 / 增量都需要可分辨的失败原因; 编码表待回灌(与 D-11 同向) |
| D-15 | §9.2 把工具自身的构建入口(`cxx/ Makefile`)与镜像构建混在仓库根 Makefile 的"两段式"叙事里 | 顶层 `Makefile` **只编工具**; 镜像目标降级为**薄委派别名**(`make all|run|smoke|…` → `$(BRICKIE) build|run|test|…`), 真身在 `brickie build` + `tests/gates.toml`; 源码集合 / 标志 / 规则从 Makefile 移除, 由 `make check-build` 第 ② 条门禁钉死 | ADR-0003 的 S1+S4(构建归属转移 + Makefile 退役的缩减期); 迁移期保留别名是为了不打断现有 CI/习惯 |

> 以上各条都**不是**"顺手多做一点", 而是 v0.1 命令面无法回避的落笔处; 每条都在生成物
> 的文件头或契约里对读者可见, 并已登记进 checklist, 等规格拍板后改模板/引擎即可。

## 7. 修改纪律

- **判定只归 L0/L1(`rust/`), 渲染只归 L2(`cxx/`), 编排只归 L5(`python/`)。**
  别在 Python 里加业务规则, 也别在 C++ 渲染器里加条件判定 —— 这是 C9 与 §9.1 的硬约束,
  由"粘合层纯度检查"与反向验收用例保证。
- **生成物不手改。** 改 `plugin.toml` 后重建(`brickie gen`); 生成物与 golden 同纪律。
- **模板是数据。** 想让四类骨架分化, 改 `templates/`, 不给渲染器加条件语法 ——
  §9.2 用"按 `api_type`/`plugin_type`/`lang` 分目录"取代了模板语言。
- **改了 `python/**`、`templates/**`、`schema/**` 或原生工具, 记得重发种子。**
  它们都被 `freeze.py` 嵌进入口 ELF(`PAYLOAD_ROOTS = ("python","templates","schema")`),
  所以 `make prebuilt` 不只是"重编 C++"—— 载荷变了 `brickie` 就变了;
  `make prebuilt-check` 与端到端用例的"种子逐字节一致"会抓漏。
- **启动器不碰业务。** `cxx/launcher.cpp` 只做"定位/解包/布环境/转发", 一行 Python 语义
  都不许加; 载荷里的代码仍是同一个 `python/brickie/`(纪律同上)。
- **改动先跑 `make selftest && make test`**(要连入口 ELF 一起验, 再跑 `make prebuilt`)。

## 8. L5(Python)前端的权威说明

`python/brickie/` 的模块地图、逐命令状态、与两个原生工具的调用点、schema 形状校验的
允许集、剩余 TODO 与契约缺口(T-*、G-*)**全部在
[`python/brickie/README.md`](python/brickie/README.md)** —— 本节只留指针, 不复制内容。
本文件 §1 的现状表是**跨三层**的总览; 两者冲突时, 以代码与 `docs/contract.md` 为准。

## 9. 文档同步时的"待确认"清单(现已逐条收口)

> 这一节是**文档同步与实现并行**时留下的台账。实现落地后逐条收口, 保留它是为了
> 留下"曾经不一致过"的痕迹(而不是悄悄删掉)。

| # | 写作时的状态 | 收口结果 |
|---|---|---|
| 1 | `rust/src/{solver,check,iface}.rs` 还是 `unimplemented` 空壳, `brickie check` 回 exit 2 | **已收口**: 15 条命令族落地, `brickie-core --selftest` = 432 cases; `check`/`dep *`/`ver *`/`iface *` 在原型树上实测 0 错误 |
| 2 | 种子落后于源码(`prebuilt-check` 红) | **已收口**: 三件种子按最终源码重发, `make tools-prebuilt-check` 全 ok |
| 3 | `schema.py` 的 docstring/查找清单仍写"包内镜像 `python/brickie/schema/`" | **已收口**: 查找顺序改为 `$BRICKIE_SCHEMA_ROOT` → `<tool_root>/schema`(规范位置) → 包内兼容读法(可选) → 仓库兜底 |
| 4 | `cxx/Makefile` 的 `PAYLOAD_SRCS` 比 `freeze.py` 的 `PAYLOAD_ROOTS` 窄 ⇒ 改非 `.py`/schema 不重打载荷(静默不一致) | **已收口**: `PAYLOAD_SRCS` 改为按 `PAYLOAD_ROOTS = python templates schema` 同源列举(排除 `__pycache__`/`*.pyc`) |
| 5 | 缺省 `make` 因 `brickie-check` 未实现而编不到镜像 | **已收口**: `brickie-check` 现为**真的**组合期校验(声明面红才止步); 原型树实测缺省 `make` 通过 |
