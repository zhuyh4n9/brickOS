# prebuilts/ — 进版本库的**自举种子**(host 侧预编译件)

> **一句话**: `build/host/**` 是**本机刚编出来的**产物(派生, 不进库);
> `prebuilts/**` 是**随源码提交**的种子 —— 让没有 `g++` 的全新 checkout 也能
> 直接把 `brickie` 跑起来。将来 `brickie` 自举管理自身编译时, 这里就是"第一块砖"。

## 1. 目录布局

```
prebuilts/
└── brickie/
    └── <host-arch>/<host-os>/bin/
        ├── brickie        # ★ 单文件自包含入口 ELF(Python 前端 + 模板 + 原生工具全在里面)
        └── brickie-gen    # L2 生成器(独立副本; 已被上面那份嵌入, 供开发态直接调用)
```

- `<host-arch>` = 处理器架构: `x86-64` / `aarch64` / …
- `<host-os>`   = 操作系统: `linux` / `darwin` / `win`

与 `build/host/<host-arch>/<host-os>/bin` **同构**, 只是一份进库、一份不进库。
映射真值在 [`tools/host-detect.sh`](../tools/host-detect.sh)(`mk/host.mk` 把它
变成 make 变量, `python/brickie/hostinfo.py` 是它的镜像)。

### 1.1 两个文件各自是什么

| 文件 | 是什么 | 运行时依赖 |
|---|---|---|
| `brickie` | **自包含入口 ELF**: C++ 启动器 + 嵌入的 Python 包(`python/brickie/**`)、模板(`templates/**`)与**原生工具**(`bin/brickie-gen`)。运行时: 解包嵌入载荷到临时目录 → 布环境 → `python3 -m brickie` | 系统 `python3`(仅解释器; 不需要 `PYTHONPATH`、不需要源码树、**不需要同目录的 `brickie-gen`**) |
| `brickie-gen` | **L2 生成器**: 纯 C++ 可执行。它**已被嵌进 `brickie`**; 这里单独放一份, 是给"开发态 `python3 -m brickie`"与直接调用用的 | 无(动态链 libc/libstdc++) |

> **`brickie` 一个文件就够了**: 需求是"brickie-gen 等工具均需要编译到 brickie elf 中",
> 所以 `prebuilts/.../bin/brickie` **单独拷走**即可工作(实测: 只拷它一个文件 +
> `env -i PATH=/usr/bin:/bin` 就能跑 `new`)。同目录的 `brickie-gen` 是**冗余但有用**的
> 副本, 不是运行前提。

入口 ELF 的关键行为(见 [`cxx/launcher.cpp`](../tools/brickie/cxx/launcher.cpp)):

1. **自包含代码、模板与工具**: 三者由 [`freeze.py`](../tools/brickie/freeze.py)
   打成**未压缩 ustar tar** 后逐字节嵌进二进制(原生工具带可执行位; 不用 zip/压缩,
   启动器就不必链 zlib); 启动器解包后把 `BRICKIE_GEN` 指到**解包出来的**那份
   (`<tmp>/bin/brickie-gen`), 前端因此调用的正是嵌入件;
2. **兜底自定位**: 若载荷里没有该工具, 才退回去找同目录的 `brickie-gen`。

> **这是"brickie 的 Python 代码编译为 ELF"+"原生工具编译进 ELF"的落地形态**
> (ADR-0004 §7): 零依赖入口 ELF, 不是 PyInstaller/Nuitka 冻结 —— 理由是设计侧
> v0.1 的"Python 零第三方依赖"纪律(BR-D3 修订口径), 而自写启动器只需 g++ 与标准库。

## 2. `prebuilts/` 下分两侧: `seed/` 与 `toolchain/`

**只有一个顶层目录 `prebuilts/`**, 里面按"进不进版本库"分成两个子目录:

| 子目录 | 是什么 | 进版本库? | 谁产生 |
|---|---|---|---|
| `prebuilts/seed/` | `brickie` 自己的**自举种子**(预编译件) | **是**(资产) | `make tools-prebuilt`(ADR-0004) |
| `prebuilts/toolchain/` | **外部工具链**: 下载缓存 `.cache/` + 解压结果(`arm-gnu-toolchain-*`/`make`/`ninja`)+ `toolchain.mk`(体积以 GB 计) | **否**(`.gitignore` 一条规则覆盖整个子目录) | `make prebuilt` / `python3 tools/fetch-prebuilt.py`(锁版本 + SHA256; ADR-0002) |

> **为什么不再用两个顶层目录**(`prebuilt/` 单数 vs `prebuilts/` 复数): 两个名字只差一个 `s` 而语义相反,
> 是个反复踩的脚。现按"**一个顶层目录 + 子目录分侧**"收敛, 见 ADR-0005(它取代 ADR-0002 §2-1 与 ADR-0004 §2 的"不合并"条文)。
> `prebuilts/toolchain/` 是**派生**的 —— 删掉后用 `make prebuilt` 一条命令重建(锁文件 `prebuilts/toolchain.lock.toml` 才是真值)。

## 3. 怎么发布 / 怎么用

发布(改了 `tools/brickie/**` 之后):

```bash
make tools-prebuilt          # 编两个 ELF → 拷贝到 prebuilts/seed/brickie/<arch>/<os>/bin/
make tools-prebuilt-check    # 只检查种子是否落后于源码(不改盘)
```

用法一(推荐): **直接用入口 ELF** —— 不需要 `PYTHONPATH`、不需要源码树,
也**不需要同目录的 `brickie-gen`**(它在 ELF 里):

```bash
prebuilts/seed/brickie/x86-64/linux/bin/brickie new ability service/crypto --subkind service
# 只拷这一个文件走也行:
cp prebuilts/seed/brickie/x86-64/linux/bin/brickie /tmp/ && /tmp/brickie --version
```

用法二(开发态): `python3 -m brickie`。此时前端按下面顺序找 `brickie-gen`
(见 `python/brickie/native.py`):

1. `$BRICKIE_GEN`(显式覆盖; 入口 ELF 会把它指到**解包出来的嵌入件**);
2. `build/host/<arch>/<os>/bin/brickie-gen` —— **本机刚编的**(优先, 免得旧种子盖住新代码);
3. `prebuilts/seed/brickie/<arch>/<os>/bin/brickie-gen` —— **本种子**(没有 `build/` 时用它);
4. 旧落点 `tools/brickie/cxx/brickie-gen`;
5. `PATH` 上的 `brickie-gen`。

所以"全新 checkout 没装 `g++`"时, 两条路都通:

```bash
# (a) 入口 ELF: 代码/模板/原生工具全在二进制里
prebuilts/seed/brickie/<arch>/<os>/bin/brickie --version
# (b) 开发态: 显式给 Python 指源码
PYTHONPATH=tools/brickie/python python3 -m brickie new ability service/crypto --subkind service
```

## 4. 纪律

- **种子必须与源码同步**: 动 `tools/brickie/**`(含 `python/`、`templates/`、`cxx/`)或
  动被嵌入的原生工具(如 `brickie-gen`)就得 `make tools-prebuilt`, 否则
  `make tools-prebuilt-check` 与端到端用例("种子与本次构建逐字节一致"、
  "载荷里嵌了可执行的原生工具"、单文件独立分发)会报红。这是有意的 ——
  自举链最怕"源码与种子各说各话"。
- **嵌进 ELF 的工具是"载荷的一部分"**: `cxx/Makefile` 的 `PAYLOAD_EMBEDS` 是唯一清单
  (现在 `brickie-gen=bin/brickie-gen`); 加 `brickie-core` 等只需在此追加一行 +
  让 `launcher.cpp` 认得它(按 `bin/<名>` 解包即可)。漏加的后果是"能用但没嵌",
  由门禁的 `--embed ...=bin/<名>` 检查拦住。
- **载荷必须可复现**: `freeze.py` 归一化 tar 元数据(mtime/uid/gid 全零 + 路径排序 +
  固定模式位), 所以同一份源码两次构建的 `brickie` **逐字节相同**(门禁依赖这条)。
- **`make clean` 不动 `prebuilts/`**: 它是资产, 不是派生品(`build/` 才是)。
- **二进制进库是有意的取舍**: 换来自举能力与"零编译器起步"; 体积代价可控
  (v0.1 实测: 自包含 `brickie` ~296 KB, 含内嵌的 `brickie-gen` ~190 KB;
  独立副本 `brickie-gen` 另 ~190 KB)。若将来出现多宿主多版本,
  按 `<host-arch>/<host-os>` 分目录并存, 不互相覆盖。
- **仍然需要 `python3`**: 本形态只保证"不需要编译器、不需要 PYTHONPATH、
  不需要源码树、不需要同目录的其它工具"。真正"连解释器也不要"的自包含冻结
  (PyInstaller/Nuitka)是可选的后续形态, 见 ADR-0004 §7 的取舍留痕。
- **临时目录必须可执行**: 启动器把载荷解到 `$TMPDIR`(默认 `/tmp`)再 exec 内嵌工具;
  若该文件系统挂了 `noexec`, 用 `TMPDIR=<可执行目录> brickie …` 绕过。
- **人类可读的变更说明**写在本文件 §5, 不写进二进制。

## 5. 种子台账(发布了就补一行)

| 日期 | `brickie` 版本 | host 三元组 | 说明 |
|---|---|---|---|
| 2026-10-03 | 0.1.0 | `x86-64/linux` | 首个自举种子: `new` 命令面(L5 + L2 `brickie-gen`) |
| 2026-10-07 | 0.1.0 | `x86-64/linux` | 增补 **入口 ELF `brickie`**(Python 前端 + 模板嵌入二进制; 零依赖入口形态) |
| 2026-10-07 | 0.1.0 | `x86-64/linux` | 原生工具 **`brickie-gen` 也嵌进 `brickie`** ⇒ 单文件自包含(只拷 `brickie` 即可运行) |

## 6. 与设计文档的关系

- 决策记录: `docs/decisions/0004-brickie-prebuilts-bootstrap.md`(设计分支 `brickOS-Design`)。
- 落点回灌: `brickie-v0.1.md` §BRV-D5 表 / §9.2 注; `2-02-brickie-arch.md` BR-D5;
  `checklist.md` §5.6 的 **G-6/G-7/G-8** 与 §5.2 落地记录。
- 本目录的存在本身是一条**自举承诺**: `brickie` 迟早要能编 `brickie`(见 ADR 0004 §4 的里程碑)。
