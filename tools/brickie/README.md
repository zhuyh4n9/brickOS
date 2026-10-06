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

---

## 1. 现状(v0.1 命令面)

**已交付**:

| 命令 | 状态 | 规格出处 | 验收 |
|---|---|---|---|
| `brickie new <plugin_type> <name> [--api native] [--lang c] [--subkind S] [--force]` | ✅ 可用 | §8.4:936–951 | V-1 一半(生成)、V-9 局部(零编译依赖) |
| `brickie --version` | ✅ 可用 | §12 RV-5(部分) | — |

**未交付**(按 checklist §5.2 的批次排;未交付的命令**不会**出现在 `--help` 里):

| 批次 | 命令 | 阻塞 |
|---|---|---|
| P0 余项 | `gen`(生成物重建 + `--check` 幂等) | V-2 |
| P1 | `init` / `check`(含环检测) | **W-5**(相位双完成点) · **W-6**(`sched_class`/`[[res]]` 字段缺失) · **X-3**(`init` 三处口径不一) |
| P2–P7 | `dep *` / `ver *` / `iface *` | 见 checklist §5.1 逐行 |

> **L0/L1(Rust 核心)尚未落地**: 本机没有 `cargo`。`new` 只需要 L5(前端)+ L2(生成器),
> 所以首刀不带 Rust;`check` / `dep` / `ver` / `iface` 的求解与版本引擎按 §9.1 归 Rust,
> 到 P1 批次再引入(见 checklist §5.2)。

## 2. 目录结构(对照 §9.2)

```
tools/brickie/                 # 工具自身(本分支 = brickOS 原型树; 作为其 tools/)
├── Makefile                   # 构建入口: make / make selftest / make launcher-smoke / make test
├── pyproject.toml             # Python 包(brickie)与入口点
├── freeze.py                  # 打包器: python/ + templates/ → 未压缩 tar → 生成的 C 数组
├── python/brickie/            # L5 前端
│   ├── cli.py                 #   子命令 / 参数 / 呈现 / 退出码
│   ├── native.py              #   调 brickie-gen(JSON over stdio)+ 定位宿主产物
│   ├── hostinfo.py            #   宿主三元组(build/host/<arch>/<os> 的坐标)
│   └── writer.py              #   文件编排(落盘 / 扫已存在路径)
├── cxx/                       # L2 生成器 + L5 入口 ELF 的**源码**
│   ├── json.*                 #   自研极小 JSON(无第三方)
│   ├── text.*                 #   模板渲染 + 文件读取
│   ├── diag.*                 #   诊断模型(BRV-D8)与退出码(BRV-D9)
│   ├── rules.*                #   名字契约 / 分类学推导 / 相位 / 路径布局
│   ├── gen_new.*              #   `new` 的生成规则(§8.4)
│   ├── main.cpp               #   brickie-gen 入口 + 协议分派 + --selftest
│   └── launcher.cpp           #   入口 ELF `brickie`: 解包嵌入载荷 + 起 python3
├── templates/                 # 骨架模板(数据, 不是代码)
│   ├── native/<plugin_type>/c/   # plugin.toml / src.c / include.h / smoke.toml / README.md
│   └── descriptor/native/c/      # plugin_desc.c(按 api_type × lang, 不含 plugin_type)
└── tests/run.sh               # 端到端用例(make test)
```

**产物出树**(参考 Android 的 `out/host/...`; 由仓库根 `mk/host.mk` + `tools/host-detect.sh`
决定)。`cxx/` 只放源码, 编出来的东西一律在仓库级 `build/` 下:

```
build/host/<host-arch>/<host-os>/bin/brickie       ← L5 入口 ELF(嵌入 Python 载荷)
                              …/bin/brickie-gen    ← L2 生成器
                              …/lib/libbrickie-gen.a
                              …/obj/cxx/*.o
                              …/share/brickie/payload.tar   ← 嵌进 brickie 的载荷(供检查)
```

`<host-arch>` = `x86-64` / `aarch64` / …, `<host-os>` = `linux` / `darwin` / `win`。
前端 `native.py` 按同一口径找生成器(`BRICKIE_GEN` / `BRICKIE_HOST_ARCH` /
`BRICKIE_HOST_OS` 可覆盖), 不再依赖源码树内的旧落点。

**Python 前端 + 原生工具都编译进一个 ELF**(需求: "brickie 的 python 代码也需要编译为
elf" + "brickie-gen 等工具均需要编译到 python 的 brickie elf 中")。
形态是**单文件自包含入口 ELF**: `cxx/launcher.cpp` 把

- `python/brickie/**`(L5 前端)与 `templates/**`(骨架模板),
- **原生工具**(`cxx/Makefile` 的 `PAYLOAD_EMBEDS`, 现在 `brickie-gen` → 载荷 `bin/brickie-gen`),

由 `freeze.py` 打成未压缩 ustar tar 后作为数据嵌入; 运行时解包到临时目录, 用系统
`python3` 解释嵌入代码, 并把 `BRICKIE_GEN` 指到**解包出来的**原生工具。于是:

- 产物是真 ELF(魔数 `\x7fELF`), 与独立副本 `brickie-gen` 一起进 `prebuilts/`;
- **只拷 `brickie` 一个文件**即可运行 —— 不需要 `PYTHONPATH`、不需要源码树、
  不需要同目录的 `brickie-gen`、不需要 `g++`;
- **零新增第三方依赖**(不用 PyInstaller/Nuitka), 载荷逐字节可复现。
- 将来加 `brickie-core` 等: 在 `PAYLOAD_EMBEDS` 追加一行(`<产物>=bin/<名>`)即可,
  启动器按 `bin/<名>` 解包并给可执行位。

**自举种子**(`make tools-prebuilt` 发布; 与 `build/host/**` 同构但**进版本库**):

```
prebuilts/seed/brickie/<host-arch>/<host-os>/bin/brickie       ← ★ 单文件自包含(内含 brickie-gen)
                                           /brickie-gen   ← L2 生成器(冗余副本, 供开发态直用)
```

`brickie` 是"没有 `g++` 的全新 checkout 也能跑"的那一份(自带 Python 代码、模板与
原生工具), 也是将来 brickie 自举管理自身编译的起点。开发态前端查找顺序:
`$BRICKIE_GEN` → `build/host/...`(本机刚编的) → `prebuilts/...`(种子) → 旧落点 → `PATH`。
详见 [prebuilts/README.md](../../prebuilts/README.md)。

## 3. 构建与测试

```sh
make            # 构建两个宿主 ELF: brickie-gen(L2)+ brickie(L5 入口; 需要 g++/make/python3)
make selftest   # 生成器内置自检: JSON 往返 / 名字契约 / 分类学 / 渲染 / 路径布局
make launcher-smoke   # 入口 ELF 自检: 嵌入载荷能否起来(brickie --version)
make test       # 端到端: 四类骨架 / 相位推导 / 冲突与 --force / 用法错 / --json / 零编译依赖
                #           + 入口 ELF(真 ELF / 独立分发 / 种子一致)
make prebuilt   # 把两个 ELF 发布为自举种子 prebuilts/seed/brickie/<arch>/<os>/bin/
make prebuilt-check   # 种子是否落后于源码(不改盘)
```

产物在 `build/host/<host-arch>/<host-os>/bin/{brickie,brickie-gen}`(仓库根; 见 §2 与
`mk/host.mk`)。两个用法都行:

```sh
# (a) 入口 ELF: 自带 Python 代码与模板, 不需要 PYTHONPATH/源码树
build/host/<host-arch>/<host-os>/bin/brickie new ability service/crypto --subkind service
# (b) 开发态: Python 源码直跑, 前端自己去 build/host/ 找 brickie-gen
python3 -m brickie new ability service/crypto --subkind service
# 也可显式指定生成器
BRICKIE_GEN=/path/to/brickie-gen python3 -m brickie new ability service/crypto --subkind service
```

## 4. 用法

```sh
# 在仓库根(插件树的顶层即 namespace, §8.3)执行
brickie new ability service/crypto --subkind service
brickie new platform platform/qemu-aarch64
brickie new app app/hsm
brickie new interface iface/posix
brickie new --help
```

产物(6 件; §8.4):

```
<name>/plugin.toml                        人写  ← 插件级唯一真值(BRV-D4)
<name>/src/<short>.c                      人写
<name>/include/<short>/<short>.h          人写
<name>/tests/smoke.toml                   人写  ← v0.4 才被消费
<name>/README.md                          人写
build/gen/<name>/plugin_desc.c            生成物 ← 落在**仓库级** build/, 不进插件目录
```

**人写文件永不覆盖**(`--force` 也不例外);`--force` 只对**生成物目录**生效, 且只在
"该路径上放着的不是本工具的生成物"时才需要(生成物靠首行标记 `brickie:generated` 识别)。

## 5. L5 ↔ L2 协议(`brickie` ↔ `brickie-gen`)

JSON over stdio(§9.1 / C9)。**这条边界是契约**: Python 侧只做编排 / IO / 呈现,
所有判定与错误码都在原生侧。

请求:

```json
{
  "protocol": 1,
  "command": "plan-new",
  "templates_root": "/abs/path/tools/brickie/templates",
  "args": { "plugin_type": "ability", "name": "service/crypto",
            "api_type": "native", "lang": "c", "subkind": "service", "force": false },
  "context": { "existing": [ { "path": "service/crypto/plugin.toml", "first_line": "…" } ] }
}
```

响应:

```json
{
  "protocol": 1,
  "status": "ok",
  "exit_code": 0,
  "diagnostics": [ { "code": "BRV-TAX-0013", "severity": "warning", "target": "…",
                     "file": "", "span": "", "message": "…", "hint": "…" } ],
  "planned":   [ { "path": "…", "kind": "human" } ],
  "artifacts": [ { "path": "…", "kind": "human", "content": "…" } ]
}
```

命令:

| `command` | 作用 | 说明 |
|---|---|---|
| `plan-new` | 只做入参校验 + 探模板, 返回 `planned` | 让 L5 能先扫出"哪些路径已存在"再回传 `existing`;两步各一次进程调用 |
| `new` | 正式生成, 返回 `artifacts` | **只有零 error 时才产出 artifacts**(半生成状态比不生成更糟) |

`brickie-gen` 的**进程**退出码只有两种: `0` = 协议处理成功(诊断与业务退出码在响应里)、
非 `0` = 它自己故障。业务退出码只在响应的 `exit_code` 里 —— 这样"工具故障"与"校验红"
不会混成一件事。

## 6. 诊断与退出码

退出码继承 `2-02` BR-D7 / 本篇 BRV-D9:

| 码 | 含义 | v0.1 |
|---|---|---|
| 0 | 成功 | ✅ |
| 1 | 校验红(环 / 冲突 / 预算 / 版本 / 分类学 / 特权声明) | ✅ |
| 2 | 用法或环境错(参数非法 / schema 不符 / 路径不存在 / 生成的模板缺失) | ✅ |
| 3 | 编译或运行失败 | ✗(v0.3+) |

`new` 会发出的诊断(全部来自 BRV-D8 的**已分配编码表**, 未自造码):

| 码 | 严重度 | 触发 |
|---|---|---|
| `BRV-TAX-0013` | warning | 插件名不符推荐形态 `<namespace>/<short>`(§8.3;只对新增插件) |
| `BRV-TAX-0014` | info | 所选 `api_type` 模板 v0.1 未交付(`runtime_adapter` / `third_party`) |
| `BRV-TAX-0015` | error | `--subkind` 与按 namespace 的推导冲突(§8.4) |
| `BRV-GEN-0002` | error | 生成目标已存在且非生成物目录;或目标是**人写文件**(§8.4) |

**两条实现期裁定(规格有缺口, 已登记, 见 checklist §5.6)**:

1. **`BRV-TAX-0014` 的严重度是 `info`, 但退出码取 2。** 理由: 严重度描述"这条信息有多重",
   退出码描述"命令有没有完成";模板未交付 ⇒ **一件文件都没生成**, shell 必须看得见。
   实现上由原生侧显式置退出码(`setExitOverride`)。
2. **用法/环境错不带 `BRV-` 码**(`code: null`)。名字契约违例、未知 `plugin_type`/`api_type`/
   `lang`、缺少 `--subkind` 等属于"用法错"这一档(退出码 2), 不是声明面诊断。**编码表缺口**:
   现行 BRV-D8 表里**没有**"插件名不符名字契约"的码(`MF` 域只分配了 `0007`, 且语义是
   "product.toml 重复声明插件自述字段")—— 已作为待补项登记。

## 7. 与设计文档的已知偏差(需要回灌)

| # | 设计文档说 | 本实现 | 为什么 |
|---|---|---|---|
| D-1 | §9.2: `cxx/ # CMake: libbrickie-gen + brickie-gen` | 用等价的最小 **Makefile**, 产物划分不变 | 本机无 cmake;首刀不引入构建系统依赖(与 §9.3 的"最小依赖集"同向) |
| D-2 | §9.2: `templates/ # 骨架模板: <api_type>/<plugin_type>/<lang>/` | 骨架模板照此;描述符另放 `templates/descriptor/<api_type>/<lang>/` | 描述符与 `plugin_type` **正交**(只随 `api_type` 与 `lang` 变),§9.2 的路径没覆盖这一维 |
| D-3 | §13.2 的 A-3 / A-14 / BRV-Q7(未回灌) | 生成物**按目标形态**写 `ver[4]` | 现行 `1-01` §6.1 仍写 `ver[3]`, 但 `plugin.toml` 的 `version` 已是四段(`0.1.0.0`);按三段的描述符**表达不了**声明面。生成物头部已把这条假定写明 |
| D-4 | `4-02:29` 的宏形态矛盾(static vs 非 static)未定稿 | 生成物取 **`static const` + `.br_plugins`**(`3-01` §13.3 / CA-10), 并在头部注明备选 | 段收集只需 `used`+`section`, 不需外部链接;CA-10 是**已采纳的决策记录**且明写"插件导出面近零" |
| D-5 | `3-05` §2 未定稿描述符头文件名 | 生成物 `#include "br_plugin.h"`, 并在头部注明未定稿 | 任何文档都没有给出文件名;生成物**不被编译**(V-9), 它现在是声明面产物 |
| D-6 | §9.2 / BRV-D5 **未规定工具自身原生件的落点**(只给了源码树) | 产物**出树**: `build/host/<host-arch>/<host-os>/{bin,lib,obj}`(参考 Android 的 `out/host/...`) | 需求方硬约束(工具 bin 不落源码树); 已回灌 BRV-D5 表 + §9.2 注 + `2-02` BR-D5, 并在 checklist §5.6 登记为 **G-6** |
| D-7 | §9.2 / BRV-D5 **未规定"随源码提交的预编译件"**(只说了"生成物不进库") | **自举种子进库**: `prebuilts/seed/brickie/<host-arch>/<host-os>/bin/{brickie,brickie-gen}`(`make prebuilt` 发布) | 需求方硬约束(为"没有编译器也能起步"与将来 brickie 自举管理自身编译留种); 已立 ADR **0004** 并回灌 BRV-D5 表 + §9.2 + `2-02` BR-D5, checklist §5.6 登记为 **G-7** |
| D-8 | §9.1/§9.2 把 L5 定为 **Python3** 进程(隐含"要解释器 + `PYTHONPATH`"), 且把 L2 生成器当作**独立可执行** | **Python 前端与原生工具编译进同一个 ELF**: 自写 `cxx/launcher.cpp` + `freeze.py`, 把 `python/brickie/**`、`templates/**` 与 `brickie-gen`(载荷 `bin/brickie-gen`)全部嵌进 `brickie`(单文件自包含; 运行时仍用系统 `python3` 解释嵌入代码, `BRICKIE_GEN` 指向解包出来的嵌入件) | 需求方硬约束("brickie 的 python 代码也需要编译为 elf" + "brickie-gen 等工具均需要编译到 python 的 brickie elf 中"); 选零依赖入口而非 PyInstaller/Nuitka, 是为守住 §9.3 的"Python 零第三方依赖"纪律。已回灌 ADR **0004 §7** 与 checklist §5.6 **G-8** |

> 以上 8 条**都不是**"顺手多做一点", 而是 `new` 无法回避的落笔处;每一条都在
> 生成物的文件头里对读者可见, 并已登记进 checklist, 等规格拍板后改模板即可(不动代码)。

## 8. 修改纪律

- **别在 Python 里加业务规则。** 判定与错误码一律在 `cxx/`;`python/brickie/` 只做
  子命令 / IO / 呈现。这是 C9 与 §9.1 的硬约束, 由"粘合层纯度检查"保证。
- **生成物不手改。** 改 `plugin.toml` 后重建;生成物与 golden 同纪律(`2-02` §4 建议 3)。
- **模板是数据。** 想让四类骨架分化, 改 `templates/`, 不要给渲染器加条件语法 ——
  §9.2 用"按 `api_type`/`plugin_type`/`lang` 分目录"取代了模板语言。
- **改了 `python/` 或 `templates/` 记得重发种子。** 二者都被 `freeze.py` 嵌进入口 ELF,
  所以 `make prebuilt` 不只是"重编 C++"—— 载荷变了 `brickie` 就变了;
  `make prebuilt-check` 与用例的"种子逐字节一致"会抓漏。
- **启动器不碰业务。** `cxx/launcher.cpp` 只做"定位/解包/布环境/转发", 一行 Python 语义都
  不许加;载荷里的代码仍是同一个 `python/brickie/`(纪律同上一条)。
- **改动先跑 `make selftest && make test`**(要连入口 ELF 一起验, 再跑 `make prebuilt`)。
