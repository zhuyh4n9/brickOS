# 0004: brickie 自举种子与 `prebuilts/` 落点

> 状态: **已接受** | 影响面: **toolchain**(工具自身的构建/分发/引导); 不影响 OS 契约(`br_*` 符号、插件树布局、`build/gen/**` 生成物)
> 格式依据: `1-02` §2.2(决策记录格式: 动机 / 设计 / 替代方案 / 兼容性影响)
> 关联: `brickie-v0.1` **BRV-D5**(状态目录) · §9.1/§9.2(分层与仓库骨架) · `2-02` **BR-D5**(状态/增量/可复现) · checklist §5.6 **G-6/G-7/G-8**
> 兄弟 ADR(务必区分): [`0002-prebuilt-toolchain.md`](0002-prebuilt-toolchain.md) = **外部工具链下载缓存** `prebuilts/toolchain/`(**派生侧**, 不进库); [`0003-build-ownership-makefile-retirement.md`](0003-build-ownership-makefile-retirement.md) = 顶层 `Makefile` 退役路径(其 §5-1/§5-2 的"宿主编译器仍需外部"正是本 ADR 要解的那半)。
> 落地: `brickOS-prototype-v0.1.0` 的 `mk/host.mk` / `tools/brickie/cxx/{Makefile,launcher.cpp}` / `tools/brickie/freeze.py` / `tools/brickie/python/brickie/{hostinfo,native}.py` / `prebuilts/README.md`

> **⚠ 两侧现由子目录分开(原「命名辨析」已由 ADR [`0005`](0005-prebuilts-single-root.md) 取代)**:
> 单一顶层 `prebuilts/` 下 —— 本 ADR 的**进库侧** = `prebuilts/seed/`(brickie 自举种子);
> ADR-0002 的**派生侧** = `prebuilts/toolchain/`(外部工具链下载缓存, 不进库)。
> 原先"两个顶层目录只差一个 `s`"的歧义已消除。make 目标面**不变**: 本 ADR 用 `tools-prebuilt`(根)
> 与 `tools/brickie/{prebuilt,prebuilt-check}`(工具内); ADR-0002 的是根级 `prebuilt`(取件)。

## 1. 动机

1. **鸡生蛋**: `brickie` 是组合期工具, 但它自己也是**编出来的**(L2 生成器 `brickie-gen` 是 C++ 可执行; 将来还有 Rust 的 `brickie-core`)。一个全新 checkout 若没有 `g++`, 连第一条命令都跑不起来 —— 而 v0.1 的边界纪律恰恰是"**CLI 不要求编译器在场**"(§0)。"不要求编译器"不能只对目标工程成立, 对**工具自身**也必须成立。
2. **真值分裂风险**: 需求方已定"宿主工具产物出树到 `build/host/<host-arch>/<host-os>/bin`"(同 Android `out/host/`)。若没有任何**进版本库**的宿主产物, 那么"工具从哪来"在每个 checkout 上都是本机私有的 —— 复现性(`BR-D5`)在工具自身上断链。
3. **自举是路线而不是偶然**: 需求方明确"**未来将用 brickie 自举管理 brickie 工具的编译**"。自举必须有一块**初始砖**(seed): 它先于"用 brickie 编 brickie"存在, 且必须随源码走。

## 2. 决策

- **`build/host/**` 与 `prebuilts/**` 是两份宿主产物, 职责不同**:

  | 落点 | 性质 | 进版本库 | 谁产生 | 消费者 |
  |---|---|---|---|---|
  | `build/host/<host-arch>/<host-os>/{bin,lib,obj}` | **本机/本次构建**产物 | **否**(派生) | `make tools`(宿主 g++/ar + python3 打包) | 本机开发、用例 |
  | `prebuilts/seed/brickie/<host-arch>/<host-os>/bin/{brickie,brickie-gen}` | **自举种子**(预编译) | **是**(资产) | `make tools-prebuilt`(从上一行拷贝) | 无编译器的全新 checkout; 将来 brickie 自举编译 |

  `brickie` = **入口 ELF**(Python 前端 + 模板嵌在二进制里, §7); `brickie-gen` = L2 生成器。
  两者**成对**发布, 且入口 ELF 自定位同目录的 `brickie-gen` ⇒ 种子目录自洽。

- **命名规范与 `build/host/**` 同构**: `prebuilts/seed/<工具族>/<host-arch>/<host-os>/bin/<可执行>`; `host-arch` ∈ {`x86-64`,`aarch64`,…}, `host-os` ∈ {`linux`,`darwin`,`win`,…}。映射真值**只有一处**(POSIX 原型为 `tools/host-detect.sh`), make / 门禁 / 前端三处共享, 禁止各自重算。
- **查找顺序(前端)**: `$BRICKIE_GEN` → `build/host/<triple>/bin`(**本机新编的优先**, 免得旧种子盖住新代码) → `prebuilts/seed/brickie/<triple>/bin` → 旧落点 → `PATH`。
- **发布与校验分离**: `make tools-prebuilt` 发布(内容相同则不写盘, 保证幂等); `make tools-prebuilt-check` 只校验"种子是否落后于源码", 不改盘。CI/用例断言"种子与本次构建逐字节一致" ⇒ 源码动了而没重新发布, 会**红**。
- **`make clean` 不动 `prebuilts/`**: 种子是资产, 不是派生品。
- **两侧由子目录分开**: `prebuilts/seed/`(自举种子, **进库**, `make clean` 不动)/ `prebuilts/toolchain/`(外部工具链下载缓存, 体积以 GB 计, **不进库**)。本 ADR 原先"两个顶层目录不合并"的条文已被 ADR [`0005`](0005-prebuilts-single-root.md) 取代 —— **"不合并"针对的是目录名, 不是职责**: 职责区分照旧。

## 3. 候选与否决

| 候选 | 评价 |
|---|---|
| **A `prebuilts/seed/<工具族>/<arch>/<os>/bin`(采纳)** | 与 `build/host/**` 同构 ⇒ 复用同一份映射真值与同一套门禁; 按宿主分目录并存 ⇒ 多宿主互不覆盖; 与 Android `prebuilts/` 同构 ⇒ 认知成本低 |
| B 把种子直接放在 `build/host/**` 并让该子目录进库 | 违反"`build/` 整体是派生目录"的既有纪律(BRV-D5 / Q5), `.gitignore` 要开洞, 且"本机新编的"与"进库的"混在一处 ⇒ 两者互相覆盖 |
| C 落 `prebuilts/toolchain/`(与外部工具链缓存同目录) | 缓存是**派生 + 大体积 + 不进库**; 种子是**资产 + 小体积 + 进库**。同目录会让 `fetch-prebuilt.py` 的清理/重建语义与种子冲突 |
| D 不落种子, 靠"README 教用户先装编译器" | 直接违背 §0 边界纪律与自举路线; 且"能不能用"退化成环境条件 |
| E 落成压缩包/二进制包(.tar.zst/.whl) | 多一层解包步骤与"解到哪"的问题; v0.1 单件约 200 KB, 裸文件即可。将来多语言多产物时再评估打包 |

## 4. 自举路线(里程碑, 与本 ADR 的落点绑定)

| 阶段 | 能力 | 与本 ADR 的关系 |
|---|---|---|
| **S0(本次)** | 种子进库; 前端无 `build/` 时可用种子跑通 `new`; "种子是否过期"可门禁 | 提供"第一块砖" |
| S1 | 种子被显式固化: 记录版本 + SHA256(台账见 `prebuilts/README.md` §5), 发布动作有据可查 | 让种子**可审计** |
| S2 | `brickie` 增加"构建自己"的命令面(生成构建输入 → 调 make/后端)，种子作为**上一代**输入 | 自举闭环开始 |
| S3 | 阶段化自举(stage0 种子 → 用 stage0 编出 stage1 → 比对/使用 stage1)，差异即回归 | 自举可信 |
| S4 | 多语言自举(L2 C++ 与 L0/L1 Rust 同栈自举), 种子按 `<arch>/<os>` 多维并存 | 覆盖全工具族 |

> S2+ 属 v0.x 之后; 本 ADR 只**钉死落点与语义**, 使 S2 不需要再改目录约定(改目录 = 改历史路径, 成本随引用数线性上升)。

## 5. 明确不改的项

| 项 | 理由 |
|---|---|
| `build/` 整体不进版本库, `build/gen/**` 仍是**生成物**(不进库) | BRV-D5 既有纪律; 种子只开 `prebuilts/` 一个新目录, 不给 `build/` 开洞 |
| `prebuilts/toolchain/`(派生侧)缓存目录名与 `fetch-prebuilt.py` 语义 | 已在 §2 用"职责不同"消歧; 改名会影响 `fetch-prebuilt.py`/`prebuilts/toolchain.lock.toml` 的既有引用, 收益为零 |
| v0.1 命令面(23 条叶子命令) | 种子是**构建/分发**机制, 不新增用户可见命令; `--help` 反向验收(§5.5)不受影响 |
| 错误码体系(BRV-D8) | "种子缺失/过期"属**环境错**(BRV-D9 退出码 2 一档), 不新造 `BRV-*` 码 |

## 6. 后续(未完成项)

1. **种子台账与校验**: 在 `prebuilts/README.md` §5 记录每份种子的版本/三元组/说明; S1 再补 SHA256 与生成命令(让"审计"可机器化)。
2. **多宿主种子**: 目前只有 `x86-64/linux` 一份。CI 若在 aarch64/darwin 宿主上跑, 需要各自发布一份 —— `check-build` 会明确报"种子缺失"并给出发布命令。
3. **回灌设计正文**: BRV-D5 / §9.1 / §9.2 / `2-02` BR-D5 / checklist §5.6(G-7/G-8)与本 ADR 同批落地(本次已完成)。
4. **自举闭环(S2–S4)排期**: 与 `checklist` §5.2 的 P8 / §14 的版本表对齐后再定, 本 ADR 只负责落点先行。

## 7. 增补决策(2026-10-07): Python 前端与原生工具编译为 ELF —— 单文件自包含入口形态

> **需求**(需求方原话, 两次):
> ① "brickie 的 python 代码也需要编译为 elf";
> ② "brickie-gen 等工具均需要编译到 python 的 brickie elf 中"。
> **裁定**(需求方二选一后): 形态取 **B 零依赖入口 ELF**, 且**原生工具也嵌入同一个 ELF**
> (单文件自包含), 与种子一起进 `prebuilts/`。

### 7.1 两种语义与选型留痕

| 备选 | 含义 | 评价 |
|---|---|---|
| A 自包含冻结(PyInstaller / Nuitka) | 运行时**连 `python3` 也不要** | 最彻底, 但要引入**构建期第三方依赖** + 联网 bootstrap + 锁定版本, 与本篇的设计侧纪律"**Python 零第三方依赖**"(`brickie-v0.1` §9.3 / `2-02` BR-D3 修订口径)正面冲突; 产物 10–20 MB。**留作后续可选形态**(§7.4) |
| **B 零依赖入口 ELF(采纳)** | **自写 C++ 启动器**, 把 Python 包、模板与**原生工具**都**作为数据嵌入**; 运行时解包再用**系统 `python3`** 解释、执行嵌入工具 | 真 ELF、零新增第三方依赖、载荷逐字节可复现、体积小; 代价 = 运行时仍需 `python3`(而 L5 本来就是 Python3, CI 已要求它, 见 §9.3) |
| C 只做一个 shell/wrapper 脚本 | 最省事 | **不满足"编译为 elf"**: 产物是脚本不是 ELF, 门禁会红 |

**选 B 的理由**: 需求要的是"**产物形态是 ELF**"+"**代码与工具都在 ELF 里**", 而纪律要的是"**不引入第三方**"。B 同时满足两者; A 只多买了"不需要解释器", 却把零依赖纪律卖了 —— 在 v0.1 不值。

### 7.2 机制(实现侧)

```
tools/brickie/python/brickie/**  ┐
tools/brickie/templates/**       ├─► freeze.py ─► 未压缩 ustar tar(归一化元数据 + 固定模式位)
<构建产物: brickie-gen 等>       ┘        └─► 生成的 C++ 数组 brickie_payload.cpp
                                                      │
cxx/launcher.cpp ──────────────────────────────────────┴─► brickie(单文件自包含 ELF)
载荷布局: python/brickie/**(0644) | templates/**(0644) | bin/<原生工具>(0755)
运行时: ① 定位自身(/proc/self/exe, 退化 argv[0])
        ② 解包嵌入载荷 → $TMPDIR/brickie-XXXXXX/{python,templates,bin}
           (按 tar 头里的模式位落盘 ⇒ 内嵌工具可执行)
        ③ 布环境: PYTHONPATH=<tmp>/python; BRICKIE_TOOL_ROOT=<tmp>(可被显式覆盖);
                  BRICKIE_GEN=<tmp>/bin/brickie-gen(嵌入件优先; 可被显式覆盖;
                  载荷里没有该工具时才退回同目录的独立 brickie-gen)
        ④ fork+exec: python3 -m brickie <原样参数>; 等子进程(含其孙进程 = 内嵌工具)
        ⑤ 清理临时目录, 原样转发退出码(信号 ⇒ 128+N; 启动器自身环境错 ⇒ 2)
```

- **为什么用未压缩 tar 而不是 zip**: 启动器要**不链 zlib**。tar 是 512 字节定长头, 解析器 ~60 行; zip 的 deflate 需要 zlib(或自带 inflate)。
- **为什么原生工具嵌进 ELF(需求 ②)**: 让产物成为**单文件**: 只要拷 `brickie` 一个文件, 就能跑完整闭环(`new` 会调到内嵌的 `brickie-gen`)。工具清单是 `cxx/Makefile` 的 `PAYLOAD_EMBEDS`(现在 `brickie-gen=bin/brickie-gen`); 将来 `brickie-core` 等在此追加一行即可 —— 启动器按 `bin/<名>` 解包并给可执行位。
- **兜底**: 载荷里没有该工具时, 才退回去找**同目录**的独立 `brickie-gen`(开发态/过渡形态)。
- **复现性(自举前提)**: `freeze.py` 把 tar 元数据归零(mtime/uid/gid/uname)+ 路径排序 + 固定模式位, 于是**同一份源码两次构建的 `brickie` 逐字节相同**(实测 sha256 相同); 门禁的"种子与本次构建一致"因此可执行。
- **退出码口径**: 子进程退出码原样转发; "解不开载荷 / 建不了临时目录 / 起不了 `python3`"属**环境错**(BRV-D9 的 **2** 档), 不新造 `BRV-*` 码。

### 7.3 兼容性影响

| 面 | 影响 |
|---|---|
| `build/host/<triple>/bin/` | 新增 `brickie`(单文件自包含 ELF); `brickie-gen` 不变(它同时是**被嵌入的输入**与独立产物) |
| `prebuilts/seed/brickie/<triple>/bin/` | `brickie` **自包含**(内含 `brickie-gen`); 另存一份独立 `brickie-gen` 供开发态直用(冗余, 非运行前提); `make tools-prebuilt{,-check}` 仍逐件比对 |
| `make tools` 的输入 | 新增"原生工具 → 载荷"一步(工具先编, 再嵌); 需要 `python3`(本来就要: L5 是 Python3); **不新增第三方** |
| 用户可见命令面 | **不变**(同一套 L5 代码 + 同一个 `brickie-gen`, 只是从载荷里起); `--help` 反向验收不受影响 |
| 门禁 | `check-build` 增"入口 ELF 出树 + `--embed …=bin/brickie-gen` + 种子含 `brickie` + 魔数 `\x7fELF`"; 用例增"载荷里嵌了可执行的原生工具"与"**仅一个 ELF** + 空环境可跑 `new`" |
| 设计侧 | 无契约改动; `brickie-v0.1` §9.1/§9.2 补"**L5 的发布形态可为自包含入口 ELF; L2 工具嵌在其中**" |

### 7.4 已知限制(不许含糊过去)

1. **仍然需要系统 `python3`**: 本形态不解决"没有解释器"。真正自包含 = A(PyInstaller/Nuitka), 属**后续可选**; 若将来要求"连解释器也不要", 在本 ADR 追加一条并同步 CI 依赖清单。
2. **每次运行解包到临时目录**: 载荷 ~255 KB(v0.1 实测 28 文件, 其中 `brickie-gen` ~190 KB), 代价可忽略; 若日后载荷变大, 可按内容 hash 落 `$XDG_CACHE_HOME` 复用(不影响语义)。
3. **临时目录必须可执行**: 启动器把内嵌工具解到 `$TMPDIR`(默认 `/tmp`)再 exec; 若该文件系统挂了 `noexec`, 需 `TMPDIR=<可执行目录>`(Linux 上也可考虑 `memfd_create` + `fexecve` 的后续优化)。
4. **Windows 未实现**: 启动器目前是 POSIX(`fork/execvp/mkdtemp/opendir`); `host-os = win` 需要一个 `_spawnvp` 变体。`host-os` 映射里已留 `win`, 但种子暂只发布 POSIX 宿主。
5. **载荷路径上限**: 用 ustar 的 100 字节路径上限(当前最长 ~44 字节); 超过需改用 `prefix` 字段或换载荷格式(freeze.py 会显式报错, 不静默截断)。
6. **`PYTHONNOUSERSITE=1`**: 启动器默认置位以隔离用户 site-packages(提高复现性); 若有人依赖它, 需显式取消(与 `BRICKIE_PYTHON` / `BRICKIE_GEN` 同理, 都是可覆盖环境变量)。

### 7.5 增补记录(同日第二次): 原生工具也嵌入

需求 ②("brickie-gen 等工具均需要编译到 python 的 brickie elf 中")在 §7 落地后追加, 相对首版的增量:

| 面 | 首版(仅 Python) | 本版(含原生工具) |
|---|---|---|
| 载荷 | `python/**` + `templates/**` | 追加 `bin/<工具>`(**0755**) |
| 打包器 | `freeze.py` 只走源码树 | 增 `--embed <构建产物>=bin/<名>`(可重复); tar 记模式位 |
| 启动器 | 解包后**自定位同目录** `brickie-gen` | 解包后把 `BRICKIE_GEN` 指到**载荷里的** `bin/brickie-gen`; 同目录自定位降级为兜底 |
| 分发 | 需 `brickie` + `brickie-gen` **两个文件** | **一个文件**(`brickie`)即可; `brickie-gen` 仅作开发态便利副本 |
| 体积 | ~104 KB | ~296 KB(含内嵌工具) |
| 门禁/用例 | 两个 ELF 的独立分发 | "载荷里嵌了可执行的原生工具" + "**仅一个 ELF** 的独立分发" |

> **尚未纳入但已在同类位置的**: `brickie-core`(Rust, 未落地)/ `brickie-verify` 等 —— 落地后按同一 `PAYLOAD_EMBEDS` 机制追加, 无需改启动器或目录约定。
