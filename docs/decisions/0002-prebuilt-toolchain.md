# 0002: 自洽构建工具集 `prebuilts/toolchain/`(锁版本 + 校验 + 一条命令重建)

> 状态: **已接受** | 影响面: **prototype 分支的构建**(顶层 `Makefile` 的工具链探测) + **CI 环境**; **不影响**设计侧的产物契约与符号
> 格式依据: `1-02` §2.2(动机 / 设计 / 替代方案 / 兼容性影响)
> 相关: `2-02` §3 **BR-D3**(语言与依赖锁)、**BR-D4**(构建后端边界); 欠债 `br-wa-toolchain-001`
> 兄弟 ADR(**务必区分目录名**): [`0004-brickie-prebuilts-bootstrap.md`](0004-brickie-prebuilts-bootstrap.md) = brickie **自举种子** `prebuilts/`(**进库侧**, 进版本库); 本 ADR 的 `prebuilts/toolchain/`(**派生侧**)= 外部工具链**下载缓存**(派生、不进库)。两侧现由**子目录**分开(`seed/` 进库、`toolchain/` 派生), 见 ADR [`0005`](0005-prebuilts-single-root.md)。

## 1. 动机

1. **构建当前不可复现**: 顶层 `Makefile` 探测的是**宿主机上恰好装了什么** ——
   `aarch64-linux-gnu-gcc`(本机 15.2.0)、`binutils-aarch64-linux-gnu`、`qemu-system-aarch64`。
   换一台机器、或发行版把 15.2 换成 16, 构建输入就变了, 而**没有任何地方记录过这件事**。
   这与 BR-D3 的修订口径直接冲突: **"每语言最小依赖集 + 锁版本 + 可离线复现"**。
2. **`br-wa-toolchain-001` 需要一个还债口**。该 WORKAROUND 欠的是"内部工具链未就绪, 借用宿主
   交叉编译器", 退出条件写的是"只改 `CROSS_COMPILE`"。但"内部工具链"此前**没有任何落点** ——
   本 ADR 给出落点: **`prebuilts/toolchain/`**(不是"自己从源码构建一整套工具链", 那在 v0 阶段投入产出比不成立)。
3. **减少"未声明依赖"这一类欠债**。原型把"外部 gcc"登记成了 WORKAROUND, 但
   `make`/`ninja`/`binutils`/`qemu` 的依赖连登记都没有 —— 欠债最怕的不是欠着, 是没人知道欠着。

## 2. 决策

1. **`prebuilts/toolchain/` = 派生目录, 不进版本库**(已进 `.gitignore`)。它按需重建、可随时删。
   > **目录名已被 ADR [`0005`](0005-prebuilts-single-root.md) 取代**: 本条原先的名字是 `prebuilt/`(**单数**);
   > 现收敛到单一顶层 `prebuilts/` 下, 用子目录分侧(本 ADR 的派生侧 = `prebuilts/toolchain/`)。**其余条文不变。**
2. **锁文件 `prebuilts/toolchain.lock.toml` 进版本库**, 是工具集的**唯一真值**: 每件记 `id` / `version` /
   `url` / **`sha256`** / `kind` / 解压落点。**没有 sha256 的条目不许存在**(脚本会拒)。
3. **取件脚本 `tools/fetch-prebuilt.py` 只依赖 Python 3 标准库**
   (`urllib` + `hashlib` + `tarfile`/`lzma`/`compression.zstd` + `zipfile` + 自带 ar 解析)。
   **刻意不依赖** `curl`/`wget`/`tar`/`xz`/`zstd`/`ar`/`dpkg-deb`/`unzip` —— 取件这一步本身
   不该再引入一串宿主工具。Python 3 不是新增依赖: brickie 的 L5 前端本来就要求它。
4. **校验不过就失败, 不"凑合用一个差不多的"**。上游换了内容 ⇒ 报错退出并保留下载件供比对,
   **必须显式**更新锁文件才能继续。
5. **分片是「未完成的下载」, 不是「坏缓存」**。取件脚本必须**断点续传**(HTTP Range), 且只在整份下完之后才判 SHA256; 校验不过也**不删**文件, 留供比对并要求 `--force` 才重下。
   这条是**踩出来的**: 早期实现把「校验不过的缓存文件」直接删掉重下, 结果把另一路正在续传的 14 MB 分片删了, 两路下载互相破坏。
6. **v0.1 阶段 prebuilt 优先、宿主兜底**: `Makefile` 用 `-include prebuilts/toolchain/toolchain.mk` 读实际路径,
   文件不存在就退回宿主交叉工具链(保留 `br-wa-toolchain-001` 的过渡形态)。

**v0.1 纳入的三件**(按体积/收益排):

| id | 件 | 体积(压缩) | 用途 |
|---|---|---|---|
| `arm-gnu-toolchain` | Arm GNU Toolchain **aarch64-none-elf** 15.2.rel1 | ≈ 92 MB | **目标编译器 + binutils**: 裸机 aarch64 专用, 自带 sysroot |
| `make` | Ubuntu `make` 4.4.1-3(`.deb` 内 `/usr/bin/make`) | ≈ 190 KB | BR-D4 **倾向 A(生成物 + make)** 的后端; 也是 `Makefile` 递归用的 `$(MAKE)` |
| `ninja` | Ubuntu `ninja-build` 1.13.2-1 | ≈ 170 KB | BR-D4 **备选 B(直出 ninja)** 预留 —— 只就位, 暂不接线 |

**选型取舍留痕**: clang 21(LLVM-21.1.8)本可**一份顶两用** —— 它既是目标编译器
(`--target=aarch64-none-elf`, 配 `ld.lld` + `llvm-objcopy`/`llvm-size`), 又顺带提供
`libclang` + 头文件, 而 `brickie-v0.1.md:272` 写明 brickie **v0.2 的"接口依赖扫描检查"注定要接
`libclang` 类解析器**。**否决理由只有一条: 体积 1925 MB vs 92 MB(20×)**, 而 v0.1 还用不到 libclang。
⇒ 记为**待复议项**(见 §5), 不是永久否决。

## 3. 替代方案与否决

| 方案 | 评价 |
|---|---|
| ❌ **把工具链二进制提交进版本库** | 92 MB~2 GB 级; GitHub 单文件 100 MB 硬限; 且二进制进版本库后"哪来的、什么时候换的"只能靠 commit 考古, 比锁文件差 |
| ❌ **不锁版本, 沿用宿主工具链** | 就是现状; 不可复现, 且漂移**静默**发生(编译通过、产物不同) |
| ❌ **只维护一个容器/CI 镜像** | 解决 CI, 不解决本地开发; 且把"构建输入是什么"藏进 Dockerfile, 比锁文件更难审计。**可与本方案并存**, 不互斥 |
| ◐ **自己从源码构建整套工具链** | "内部工具链"的字面读法, 但 v0 阶段投入产出比不成立(交叉 gcc 构建链是数小时级 + 易碎)。**留作长期方向**: `prebuilts/toolchain/` 是它的**过渡落点**, 不是替代品 |
| ◐ **vendored clang 21(1.9 GB)覆盖宿主+目标两用** | 见 §2 的取舍留痕: 唯一缺点是体积; 若日后需要 libclang 或要求宿主编译器也自洽, 这是自然升级路径 |

## 4. 兼容性影响

| 面 | 影响 |
|---|---|
| `Makefile` 工具链探测 | 新增 `-include prebuilts/toolchain/toolchain.mk`; 新增目标 `prebuilt`(取件)/ `prebuilt-check`(就位与校验)。**缺省行为不变**(没取件时仍走宿主工具链) |
| `br-wa-toolchain-001` | **退出条件细化**: 从"改 `CROSS_COMPILE`"变为"`prebuilts/toolchain/` 就位且 `make prebuilt-check` 全绿"。**该 WORKAROUND 本 ADR 不注销** —— 注销要等 CI 实际改用 prebuilt 并跑通 |
| CI | 新增一步"取件"(可缓存 `prebuilts/toolchain/.cache/`); 离线复现成为可验收项 |
| 版本库 | 新增 `prebuilts/toolchain.lock.toml`(小) + `tools/fetch-prebuilt.py`; `.gitignore` 增 `prebuilts/toolchain/` |
| 设计侧 | 无契约改动; 但 BR-D3/BR-D4 的落地形态有了具体实例, 回灌时可直接引用 |

## 5. 待拍 / 已知限制(不许含糊过去)

0. **`qemu-system-aarch64` 未纳入 prebuilt**。它只有 `run`/`smoke` 用得到, 但体积不小, 所以 v0.1 的 prebuilt **不等于**「构建与验证都不依赖外部工具」—— `make smoke` 仍要求宿主装了 QEMU。**待定**: 是否纳入(以及是否随 `prebuilts/` 走种子路线)。

1. **宿主编译器仍是外部的**。Arm GNU Toolchain **只产目标代码**, 它编不了宿主程序 ——
   而 `tools/brickie/cxx/`(L2 生成器)是**宿主** C++。所以"编译不依赖外部工具"目前
   只对**目标构建**成立, **工具自身的构建仍需要宿主 g++/clang**。
   这与 ADR-0003 的终局形态直接相关(那时 Makefile 只剩"编 tool"一件事, 而它恰恰需要宿主编译器)
   ⇒ **若要求全链路自洽, 必须再纳一件宿主编译器**(clang 21 那条路正是为此存在)。**待拍**。
2. **`prebuilts/toolchain/make` 动态链接宿主 glibc**。它由发行版包解出, 绑定宿主 libc 家族
   (本机 Ubuntu 26.04 / glibc 2.43)。因此 prebuilt **不是**跨发行版可移植的,
   它是"**钉住版本**"而不是"**与环境无关**"。要真正与环境无关, 需静态链接的 make ⇒ 回到自举问题。
3. **`compression.zstd` 需要 Python ≥ 3.14**(3.14 才进标准库)。更低版本碰到 zstd 载荷
   (新发行版的 `.deb` 都是)会退回调用 `zstd`/`tar` 命令 —— 此时引导依赖多一件。**待观察**。
4. **完整检索未做**: 三个 URL 的长期可用性(尤其 Arm 的 `developer.arm.com` 媒体路径)未做
   上游承诺核对; 若上游删除旧版本, 取件会失败(**不会**静默取到别的东西 —— SHA256 会拦住)。

## 6. 落地证据(实现侧)

- `prototype-v0.1.0/prebuilts/toolchain.lock.toml` —— 锁文件(URL + SHA256)
- `prototype-v0.1.0/tools/fetch-prebuilt.py` —— 取件(`--list` / `--check` / `--verify` / `--force` / `--print-hash`)
- `prototype-v0.1.0/Makefile` —— `prebuilt` / `prebuilt-check` / `prebuilt-clean` 目标 + `-include prebuilts/toolchain/toolchain.mk`
- `prototype-v0.1.0/tools/check-build.sh` —— 构建接线门禁(含"工具段零交叉依赖")

## 7. 交接记录(实现期踩到的四个坑 + 遗留)

> 本节写给**接手这条线的人**。四个坑的共同点是: 全部为**静默**故障 —— 要么不报错, 要么报错指错方向。

### 7.1 已验证的完成态

| 项 | 结果 |
|---|---|
| `prebuilts/toolchain/` 三件 | 全部就位: Arm GNU Toolchain **15.2.Rel1**(解压 465 MB)、make **4.4.1-3**、ninja **1.13.2-1** |
| 完整性 | Arm 包的 SHA256 与**上游官方 `.sha256` 逐位一致**(`66f7ce7c…`), 大小 95935004 —— 不是本地首用信任 |
| 零外部交叉工具链 | `make -n -B` 里 `aarch64-linux-gnu` 出现 **0 次**; 编译器 = `prebuilts/toolchain/arm-gnu-toolchain-15.2.rel1/bin/aarch64-none-elf-gcc` |
| 产物等价性 | 换工具链后 `build/brick.bin` 与宿主 `aarch64-linux-gnu-gcc 15.2` 的产出**逐字节相同**(12304 B) |
| 端到端 | `make` / `make smoke`(3/3) / `make check-build` / `make prebuilt-check` / `make tools-test` 全绿 |

### 7.2 四个静默坑(都已修, 记下来免得重踩)

1. **目标名与目录撞名 ⇒ `make prebuilt` 静默空操作**。`prebuilt` 与目录 `prebuilts/toolchain/` 同名, 又漏了 `.PHONY`, 于是 make 认定「目录已存在 ⇒ 无需做任何事」, 打印一句就 `exit 0`, **一件都没下**。⇒ 已进 `.PHONY`。**同理**: 任何与产出目录同名的新目标都必须显式 `.PHONY`。
2. **把「校验不过的缓存」删掉重下 ⇒ 删了另一路正在续传的分片**。分片的正确语义是「**尚未完成的下载**」, 不是「坏缓存」。⇒ 取件脚本自带 **HTTP Range 断点续传 + 重试**, 只在整份下完后判 SHA256, 校验不过也不删(留供比对, 要 `--force` 才重下)。
3. **`resp.read(1 MB)` 在慢链路上要凑满 1 MB 才落盘**。16 KB/s 下 70 秒才写一次 ⇒ 进度不可见、断线白收最多 1 MB。⇒ 改 `read1(64 KB)` + `flush`。
4. **`tarfile.open(mode="r|")` 里的 `r|` 是「未压缩的 tar 流」**! 透明压缩必须写 **`r|*`(带星号)**。写成 `r|` 就会拿 xz 字节当 tar 头读 ⇒ `ReadError: invalid header`。这个星号**值 1.7 小时**: 91.5 MB 下完之后卡在解压, 现象酷似「文件坏了」, 而其实 **SHA256 与上游完全一致**。⇒ **遇到「下载完但解压失败」先核对 SHA256, 别急着重下。**

### 7.3 给兄弟线的三条约定(避免各自重算)

1. **`[toolchain]` 的取值有两类**: **路径**(如 `make/bin/make`)与**前缀**(以 `-` 结尾, 如 `…/bin/aarch64-none-elf-`, 即 `CROSS_COMPILE` 前缀)。前缀**不是文件**, 探针 = `<前缀>gcc`。唯一判定点是 `fetch-prebuilt.py::toolchain_probe()` —— **别在别处重算**(否则一切正常却报 MISS)。
2. **门禁里的交叉编译器名必须从项目真值取**(`make -s print-cross-compile`), 不许硬编码。反例留痕: `check-build.sh` 曾写死 `aarch64-linux-gnu-gcc`, 切到 prebuilt 的 `aarch64-none-elf-gcc` 后门禁**自己**误报「镜像段没被触发」—— 门禁成了工具链选型的耦合点, 正是它该避免的。
3. **分侧, 不是两个近名顶层目录**: `prebuilts/toolchain/` 是外部工具链下载缓存(`make prebuilt` 取 / `make prebuilt-clean` 删, **不进库**); `prebuilts/seed/` 是 brickie 自举种子(`make tools-prebuilt` 发布, **进库**, `make clean` **不**动它)。见 ADR-0005 与 ADR-0004。

### 7.4 遗留(明确未做, 按需接手)

1. **QEMU 未纳入**(§5-0): `make run` / `make smoke` 仍要求宿主装了 `qemu-system-aarch64`。
2. **宿主编译器未纳入**(§5-1): Arm GNU Toolchain **只产目标代码** ⇒ 编 `tools/brickie/cxx/` 仍需宿主 C++。注意与 ADR-0004 **G-8** 的关系: launcher ELF 让 `brickie` 成了真 ELF, 但它**运行**仍需系统 `python3`, 且 `brickie-gen` 仍需宿主编译 ⇒ 「自包含」的**边界**要写清, 别把两件事混成一件。
3. **`compression.zstd` 需 Python ≥ 3.14**(§5-3); 更低版本退回 `zstd`/`tar` 命令, 引导依赖 +1。
4. **三个 URL 的长期可用性未核对**(§5-4); 上游删旧版时取件会失败(不会静默取错 —— SHA256 拦得住)。
5. **`prebuilts/toolchain/` 未纳入 `check-build` 门禁**: `prebuilt-check` 缺省「不算失败」(退回宿主工具链是合法过渡形态); 要求硬性自洽的 CI 作业需显式 `PREBUILT_STRICT=1`。**是否升为缺省硬门禁, 待定。**

