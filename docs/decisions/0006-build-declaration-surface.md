# 0006: 构建声明面 —— 一处事实一个可写处, 以及 `Makefile` 退役的实际口径

> 状态: **已接受(原型已落地)** | 影响面: **构建声明面**(`plugin.toml` `[build]`/`[build.target]`、`product.toml` `[build]`、`tests/gates.toml`)+ `2-01`/`2-02` 的构建编排条文 + `brickie-v0.1.md` §0/§8/§14 + `BRV-D8` 诊断码表; **不影响** OS 契约(`br_*`/`BR_*`/`.br_*`)与插件树布局
> 格式依据: `1-02` §2.2(决策记录格式与阈值表: 动机 / 设计 / 替代方案 / 兼容性影响)
> 落点: 原型 `brickOS-prototype-v0.x.0` @ `1d60a15`(ADR-0003 的 S1–S4); 设计依据 = `docs/decisions/0003-build-ownership-makefile-retirement.md`
> 相关: `2-01` / `2-02` §1 职责表 + §3 BR-D4 + §7 / `brickie-v0.1.md` §0、§7.1、§8.4、§9.1、§14
> 证据(原型侧, 只读引用): `docs/decisions/0004-brickie-build-and-make-retirement.md`(§5 门禁抓到的真问题 / §6 实测)、`tools/brickie/docs/contract.md` §5.4/§7/§9、`tools/brickie/rust/README.md` §10(S-B1…S-B6)

## 1. 动机

1. **ADR-0003 定了方向与台阶, 没定声明面**。它给的是 S0–S4(§3, 47–53 行)与三条待拍项(§5, 71–83 行); 而设计现状是: `brickie-v0.1.md` §8.1 的 `[build]` 只有 `sources`/`includes`(1037–1039 行, 注"v0.1 只记录; v0.3 起被消费"), §8.2 的 `product.toml` **没有** `[build]`(1081–1112 行), `[build.target]` 与 `tests/gates.toml` 在设计里根本不存在。**"构建编排划给 `brickie`"(`2-02` §1, 50 行)已成定论, 但"往哪里写构建事实"当时无解**。
2. **原型 v0.x.0 已把 S1–S4 走完**(prototype ADR-0004 §2.8, 125–137 行)⇒ 那条路径上实际长出来的声明面必须先留痕, 否则同一契约两套口径(原型一套、设计一套), 正是 `comment/README.md` 记的"元契约漂移"病根。
3. **达成口径必须不夸大**。S4 的出口判据(ADR-0003 §3, 53 行)是"`make` 的唯一产物是宿主工具", 不是"`Makefile` 被删除"; ADR-0003 §6(89 行)明确"缩减期两条路径必须并存"。本 ADR 一并把"达成了什么/没达成什么"写死(D11)。

## 2. 决策(D1…D11)

**D1 声明面四处 + "一处事实一个可写处"**

| 事实 | 唯一可写处 | 证据 |
|---|---|---|
| 每个插件怎么编(源 / 包含 / 宏) | 各插件 `plugin.toml [build].{sources,includes,defines}` | `platform/qemu-aarch64/plugin.toml:223`; contract §7.1:349–352 |
| **目标事实**(arch / 交叉前缀 / arch 标志 / 链接脚本 / QEMU 型号) | platform 插件的 `[build.target]`(+`[build.target.qemu]`) | 同文件 230–252 行; contract §7.1:354–365 |
| **产品策略**(编译/汇编/链接标志、产物落点、核心本体源集合、profile 追加标志) | `product.toml [build]` / `[build.release]` | `product.toml:46–105`; contract §7.2:388–399 |
| **门禁判据**(宿主用例 / 日志正则 / 脚本) | `tests/gates.toml` | `tests/gates.toml:18–252`; contract §7.4:410–466 |
| 生成物 | `brickie gen` 产出 `build/gen/**`, 是否进镜像由 `[build].gen_sources` 决定 | contract §5.2:137 + §7.3:406–408 |

`core/` **不是插件** ⇒ 核心本体的源集合只能由产品声明(`product.toml:62` 的 `[build].core_sources`/`core_includes`; contract §7.2:389 的注)。目标事实放 platform 而非 product 的理由: 产品 manifest 只做"选择与预算"(`product.toml:3–4`), 而 arch / 交叉前缀 / 链接脚本 / QEMU 型号是"这个平台是什么"(`brickie-v0.1.md` §3.2:157 的"`platform` 每镜像恰 1"), 故归 platform 插件 —— contract §7.1:354 也把 `[build.target]` 限定为 `plugin_type = "platform"` 声明。

**D2 构建命令族 = 6 条, 命令面 = 29 叶子 + 2 全局开关**

- 6 条: `build` / `clean` / `run` / `size` / `disasm` / `test`(contract §5.4:162–170)。构建族的语义 = "组合期 check → 生成物 → 编译/链接/objcopy → 构建后门禁"(`build`), 其余按同一计划派生(contract §5.4:164–170)。
- 总数: **29 个叶子 = 23 组合期 + 6 构建族**, 外加 2 个全局开关(`--version [--deps]` / `--json`)。证据: `tools/brickie/python/brickie/cli.py:19–25`、`__init__.py:17`。23 条组合期命令的原文出处 = `2-02` §5(172 行:"23 条叶子命令 + 2 条全局开关")。

**D3 边界纪律的准确表述(裁定 R-13)**

`brickie-v0.1.md` §0(89 行)写的"v0.1 的任何命令**不得**要求 `cc`/`cargo`/`nm` 在场", **只约束组合期命令**(`check`/`dep`/`iface`/`ver`/`gen`/`new`/`init`)。构建族的**职责就是**驱动交叉工具链 —— "要求编译器在场"是它的定义, 不是纪律的例外条款。重述的原文位置: contract §9 **R-13**(497 行); 实现侧同口径写在 `tools/brickie/python/brickie/cli.py` 的 `_EPILOG`(48–55 行)。纪律的**目的**(CI 的组合期作业零编译依赖)不受影响。

**D4 判定在 core, 执行在 L5**

| 判定 | 归属 | 证据 |
|---|---|---|
| 工具候选序 + **解析结果** | core | contract §5.4:222–233 + §9 **R-14**(498 行) / S-B1(`rust/README.md` §10) |
| 门禁红绿(`require`/`forbid`/`PASS <tag>`) | core 的 `judge` 命令按 `tests/gates.toml` 判日志 | contract §5.4:170 + §9 **R-16**(500 行) / S-B5 |
| 生成物正文(`build/gen/build.mk`/`build.ninja`/`build-state.json`) | core 产出 `files[]`, L5 只落盘 | contract §5.4:164 + §4:97–101 + S-B6 |

L5 只做四件事: 校验 `argv[0]` 可执行、并发编排、超时/收日志、呈现(`runner.py:1–20`)。**本轮亲验**: `brickie build --dry-run --json` 报 `tools.cc.resolved = aarch64-linux-gnu-gcc-16`, 且该解析结果确实被**烧进生成物** —— `build/gen/build.ninja:5` 是 `cc = aarch64-linux-gnu-gcc-16`, `build/gen/build.mk:21` 的命令行也以该绝对名为首。make/ninja 不会去猜 `aarch64-linux-gnu-gcc-16`, 所以解析只能在 core。

**D5 增量口径 = argv 指纹 + 输入 mtime + 上游 `dirty` 传播**

两条判据(与 make/ninja 同族): ① `sha256(kind, argv)` 变了 ⇒ 重跑; ② 目标不存在, 或任一输入(源 + `.d` 列的头 + 被消费的对象/链接脚本)比目标新, 或输入不见了 ⇒ 重跑; 外加"同一计划里上游要重建 ⇒ 下游必须标脏"(计划在构建**之前**算, 那时对象还是旧的)。证据: contract §9 **R-15**(499 行) / S-B2; prototype ADR-0004 §2.4:78–91。**本轮亲验**: 稳态再跑 `brickie build --dry-run --json` 得 `steps_total = 46 / steps_todo = 0 / up_to_date = 46`。

**否决的方案: "输入集合 + 内容戳"判增量**。`.d` 是**构建的产物**(首次编译后才存在)⇒ 按输入集合算戳自指, **头依赖永远多编一轮**(原型实测: 干净构建后第二次仍重编 30 个文件)。时间戳口径没有这个自指(prototype ADR-0004 §2.4:89–91、§5.3:204–207)。

**D6 三后端等价性(S2 出口)**

`direct`(缺省, L5 按 core 的 steps 直接编排)/ `make`(BR-D4 后端 A)/ `ninja`(BR-D4 后端 B)跑的是**同一批 argv**, 只是由谁调度不同(contract §5.4:235–245)。**S2 的判据必须在同一棵干净树上、对同一份输入度量**: 原型在增量树上跨后端比对时观察到一次 **16 字节差异**(`.debug_macro`/`.debug_str` 的顺序), 追查结论是**编译输入的差异, 不是后端差异**; 于是复验条件钉为"干净树 / 先 `clean`"(prototype ADR-0004 §6:283 的"口径修正"行; 该次观察的字节级明细未落进设计侧或原型 ADR 正文, 见 §5-d)。

**本轮亲验**(对既有产物做只读比对, 不改树): `build/brick.elf` / `brick-mk.elf` / `brick-ninja.elf` 的 sha256 全部为 `c8de05f2…128d6c88`, 三个 `.bin` 全部为 `c0304e60…2d29c122` ⇒ ELF 与 BIN 逐字节一致。原型侧汇总证据见 prototype ADR-0004 §2.5:93–104 与 §6:282。

**D7 门禁正则子集: 宁可报错, 绝不静默不匹配**

core 不引正则库(§9.3 最小依赖集), 只支持 `.` `*` `+` `?` `[...]`(`^` 取反、`a-z` 区间)与 `\x` 转义; 写了分组/交替/锚点 `( ) | ^ $ { }` ⇒ **`BRV-BLD-0012`**(退出码 2)。码表位置: contract §3 **91 行**(表 75–91); 声明面处: contract §7.4:463–466 与 `tests/gates.toml:14–17`。理由是一条"永不匹配"的 `require` 会把门禁变成**永远绿的空判据**, 比报错危险得多(所以"三种 CONF 的 FAIL"写成三条 `forbid`, 见 `gates.toml:107–109`)。

**D8 诊断码族 `BRV-BLD-*` 与写路径纪律**

`BRV-D8` 编码表**没有 BUILD 族** ⇒ 本刀新开一族(contract §3:75–76 登记了缺口)。码表主条(contract §3:80–91):

| 码 | 含义 |
|---|---|
| `BRV-BLD-0001` | `product.toml` 缺席或它缺 `[build]` |
| `BRV-BLD-0003` | 没有 platform 声明 `[build.target]` |
| `BRV-BLD-0005` | 工具解析失败(交叉编译器 / objcopy / QEMU 不在场) —— 环境错, 退出码 2 |
| `BRV-BLD-0006` | 闭包内的插件没有 `[build].sources` |
| `BRV-BLD-0007` | `[build].sources` 里的**字面**路径不存在(通配不匹配不算错) |
| `BRV-BLD-0008` | `tests/gates.toml` 缺席, 或门禁名不在声明面里 |
| `BRV-BLD-0009` | 门禁判红(缺 `require` / 命中 `forbid` / 缺 `PASS <tag>`) |
| `BRV-BLD-0011` | 执行的步骤失败(编译 / 链接 / 脚本非零退出) |
| `BRV-BLD-0012` | 门禁正则超出子集(拒绝而非静默) |
| `BRV-BLD-0013` | 非闭包插件的生成物被跳过 —— **info**, 不是缺陷但必须看得见 |

注: contract §3 的码表实收 **0001–0012**; `0013` 只在代码与原型 ADR 登记(`tools/brickie/rust/src/build.rs:78` 的 `CODE_GEN_NOT_SELECTED`、`selftest.rs:2375`、prototype ADR-0004 §5.5:223)。回灌要求: `BRV-D8` 编码表补 BUILD 族 0001–0013(见 §4)。

写路径纪律: 机器**只写** `api/iface/**`、`brickie.lock`、`build/**`; **`plugin.toml` / `product.toml` 是人的财产, 工具不得重写**(`new`/`init` 仅在文件不存在时创建)。设计口径: `brickie-v0.1.md` §9.1.1(1194–1209, 尤其 1200 与 1205 行); 实现口径: contract §4:93–106(kind 表 + "永不被改写")。构建族不破这条: 计划/命令/生成物都写在 `build/**`, 不改产品与插件的 manifest。

**D9 对 ADR-0003 §5-1「版本号待拍」的裁定**

原型取的是 **(甲) 读法**:"v0.2 之后"指**原型 v0.2.0**。后果: 原型在 **v0.2.0** 就拿到了设计排在 **brickie v0.3** 的"编译(构建编排 + 生成物)"能力 —— `brickie-v0.1.md` §14 把 v0.3 定为"编译(构建编排 + 描述符/头文件/链接脚本生成物)"(1371 行), 而该批能力随构建族在 `1d60a15` 落地。**同批一起前移的还有两阶**: v0.4 的 **`test`**(conformance 运行器)与 v0.5 的 **`run`**(1372–1373 行)——它们随 `brickie test` / `brickie run` 与 `tests/gates.toml` 一起落地; 但 v0.4 的**入口条件**(host 平台插件)与 v0.5 的 **host-native 后端**都没有满足, 现只有 QEMU 后端。**代价**: §14 里 **v0.2「接口依赖扫描检查(符号级; `truth="header"`, `hash_scope="sym"`)」被跳过且推迟** —— 原型快照的 `truth`/`hash_scope` 仍只允许 `decl`(contract §7.1:295–296; V-11 约束见 `brickie-v0.1.md` §10:1259); v0.6 的**兼容性检查**(golden/api-dump/abidiff/版本矩阵)也未开始。证据: 原型批次版本 = v0.2.0(`prototype/docs/decisions/0005-plugin-manager.md:3`); 本批 = 分支 `brickOS-prototype-v0.x.0` @ `1d60a15`, 前一版 = `brickOS-prototype-v0.1.0` = `9471fc6`。**要求**: 设计侧 `brickie-v0.1.md` §14 需回灌/标注这条重排(见 §4), 本 ADR 不改那份文档。

**D10 ADR-0003 §5-3 已解除**

§5-3(81–83 行)写: `br-wa-boot-001` 的前置是 `3-05` 的 plugin_manager, 而 plugin_manager 的输入是 `brickie` 的生成物 ⇒ **S1 是它的前置**; 该依赖关系"目前没写在任何地方, 应补进 `WORKAROUNDS.md` 或 `1-03` 路线图"。现在:**S1 已落地**(`build/gen/**` 成为唯一真值: `build/gen/build.mk`、`build.ninja`、`build-state.json` 与各插件 `plugin_desc.c`), 且原型 0.x.0 已用 plugin_manager 接管启动链(`prototype/docs/decisions/0005-plugin-manager.md`)。⇒ §5-3 不再是开放项, 回灌为"前置已满足"。

**D11 S4 的实际达成口径(不许夸大)**

| 项 | 实情 | 证据 |
|---|---|---|
| 缺省目标已变成"编工具" | ✅ `.DEFAULT_GOAL := tools` | `Makefile:94`; `tools/check-build.sh` 不变量① 49–65 行 |
| 镜像侧目标是薄委派别名 | ✅ **12 条**(all/run/smoke/irq-test/mem-test/string-test/dbg-test/check-string/check-headers/size/disasm/clean-brickos) | `Makefile:198–288`; 名单见 `tools/check-build.sh:86` |
| 顶层 `Makefile` 内无镜像源码/标志字面量 | ✅ 机械判据在执法 | `check-build.sh` 不变量② 67–82 行 |
| **`Makefile` 被删除** | ❌ **没有删** —— 与 ADR-0003 §6(89 行)"缩减期两条路径必须并存"一致 | `Makefile` 仍在, 305 行 |

六条不变量本轮全部通过(`bash tools/check-build.sh` → `PASS: 构建接线一致…`, 退出码 0)。**准确表述**: S4 的"入口形态"达成(缺省只编工具、镜像全走委派), "`Makefile` 消失"从未是 S4 的判据; `make` 的 `all`/`run`/… 仍是可用的过渡别名。

## 3. 替代方案与否决

| 方案 | 评价 |
|---|---|
| ◐→否决 把构建事实全放 `product.toml [build]` 一处(不引入 `[build.target]`) | 原型判定它是**合法停靠点**但最终不取: 目标事实与产品策略混在一张表里, 而"平台是什么"本就包含 arch/链接脚本/QEMU 型号, platform 插件才是它的主人(prototype ADR-0004 §3:167; 字段缺口见 contract §9 **R-17**:501) |
| ❌ 让 L5(Python)自己拼编译命令 / 自己解析工具 | 违反 §9.1.1 的"机器文件正文归 core", 且"生成物里烧的是哪条命令"会失去唯一真值(contract §9 R-14; prototype ADR-0004 §3:166) |
| ❌ 增量用"输入内容 hash" | 自指 ⇒ 头依赖永远多编一轮(见 D5) |
| ❌ 引一个正则库(PCRE 等)做门禁判据 | 与 §9.3"每语言最小依赖集"冲突; 判据模式很短, 自定义子集 + **如实拒绝**足够(contract §7.4:463) |
| ❌ 直接删掉 `Makefile`(跳过缩减期) | 违反 ADR-0003 §6(89 行)的"两条路径必须并存"; 开发会中断 |
| ❌ 只让 `brickie` 生成 build 文件、自己不执行(纯 BR-D4 A) | 少了"后端可换"就无法证明生成的构建输入是对的; 且用户可见入口仍是 make, 达不到"替代 make"(prototype ADR-0004 §3:163) |

## 4. 兼容性影响(含"待回灌")

| 面 | 影响 | 待回灌对象(设计侧不改动, 只登记要求) |
|---|---|---|
| 声明面 schema | 新增 `[build].defines`(插件级)、platform 的整张 `[build.target]`/`[build.target.qemu]`、`product.toml [build]`/`[build.release]`、新文件 `tests/gates.toml` | `brickie-v0.1.md` §8.1:1037–1039 / §8.2:1081–1112; 字段口径原文 = contract §7.1:349–366、§7.2:388–399、§7.4:410–466 |
| 命令面 | 构建族 6 条落地, 设计曾标"不在 v0.1" | `2-02` §5:172/186–188(`build`/`run`/`test` 的"不在 v0.1"需改为"原型 v0.2.0 提前交付"; 命令面计数由 23 变 29) |
| 边界纪律 | 需要限定为"组合期命令" | `brickie-v0.1.md` §0:89 加限定语(裁定 R-13), 否则一句话管两族会自相矛盾 |
| 诊断码表 | 新开 BUILD 族 | `BRV-D8` 编码表补 `BRV-BLD-0001..0013`(contract §3:75–91 已登记缺口; `rust/README.md` §10) |
| 三层门禁(`1-02` §2.3) | 编排点从手写 `Makefile` recipe 换成 `brickie test` + `tests/gates.toml`; **三层的内容与判据不变** | `1-02` §2.3 的"由 `brickie` 编排"(ADR-0003 §6:90 已预告)可在该节落地为指针 |
| 演进序 | v0.3(编译)/ v0.4(test)/ v0.5(run)三阶被前移到原型 v0.2.0(**v0.4/v0.5 的入口条件未满足**: host 平台插件与 host-native 后端都还没有); §14 的 v0.2"接口依赖扫描检查"被跳过且推迟; v0.6(兼容性检查)未开始 | `brickie-v0.1.md` §14:1365–1374 需标注这条重排(见 D9) |
| WORKAROUND | `br-wa-entry-001` 的"Makefile 直编"半条注销; `br-wa-toolchain-001` 的位置从 `Makefile` 迁到 platform `[build.target].cross` | `WORKAROUNDS.md`(原型侧已改; 设计侧若引用需同步) |
| 现有产物 | 无: `build/` 是派生目录 | — |

## 5. 待办与遗留

- **(a) ADR-0003 §5-2「受治理的宿主编译器」仍未解决**: 现在只有自举种子 `prebuilts/seed/`(设计 ADR-0004); `make`(含 `tools` 目标)仍需宿主 `g++`/`cargo` 编工具 ⇒ 它仍是 S4 之后**唯一的外部宿主依赖**(prototype ADR-0004 §4:176–177)。
- **(b) S3 的 CI 层切换在原型仓库无可验证物**: `git ls-files` 里没有任何 CI 配置文件(`.github/**`、`.gitlab-ci.yml`、`Jenkinsfile`、`.woodpecker*`、`.drone*` 一概没有)⇒ "CI 入口 = `brickie build`"目前**只在本地门禁层面成立**(`brickie test` 走 `tests/gates.toml`, 全绿证据见 prototype ADR-0004 §6:284), 不能声称"CI 已切换"。
- **(c) `brickie-v0.1.md` §14 的演进序重排未回灌**: v0.2 的符号级接口依赖扫描未交付, 而 v0.3 的编译 / v0.4 的 test / v0.5 的 run 三阶已落地(其中 v0.4/v0.5 的入口条件——host 平台插件与 host-native 后端——尚未满足), v0.6 的兼容性检查未开始(见 D9/§4)。
- **(d) D6 的 16 字节差异缺字节级存档**: 该次观察只见于原型侧讨论, 未落进 `docs/decisions/0004` 正文; 本 ADR 记的是它的**结论与口径**(同输入才可比), 字节级明细待补。
- **(e) 设计 §8.1/§8.2 的 schema 回灌未做**: 本 ADR 只给"往哪里回灌"的指针; 除本文件外, 设计侧本轮无其他改动(遵守"只读现有文件"的取证纪律)。
