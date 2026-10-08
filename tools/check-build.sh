#!/usr/bin/env bash
#
# brickOS prototype — 构建接线一致性检查(工具段 ↔ 镜像委派)
#
# 为什么需要这个脚本:
#   镜像的构建入口已从"顶层 Makefile 里的字面规则"搬到**声明面 + brickie**(ADR-0003 的
#   S1/S4)。这种迁移最容易的坏法不是报错, 而是**静默回潮**: 谁顺手往 Makefile 里加一条
#   `core/src/foo.c` 或一个 `-ffreestanding`, 构建就又有两处真值, 而 `make` 照样成功。
#   所以把"工具只编工具 / 镜像只委派 / 声明面是唯一真值"钉成六条**能真报红**的机械判据。
#
# 六条不变量:
#   ①) 缺省目标是**工具**(`.DEFAULT_GOAL := tools`, 不再是 `-o build/brick.elf`);
#   ②) Makefile 里**没有镜像源码 / 编译标志的字面量**(正则扫描必须为空);
#   ③) 镜像目标确实改成**委派**(all/run/smoke/… 都在调 $(BRICKIE) 的对应命令, 含 check-headers);
#   ④) 工具段零交叉依赖(判据 = 交叉编译器前缀本身, 不是裸 'aarch64');
#   ⑤) 宿主产物**出树**到 build/host/<host-arch>/<host-os>/bin:
#      三件都要 —— brickie-gen(L2)、brickie-core(L0/L1)、brickie(自包含入口 ELF),
#      且两个原生工具都要**嵌进**入口 ELF(`--embed <产物>=bin/<名>`);
#   ⑥) 自举种子**进库**在 prebuilts/seed/brickie/<host-arch>/<host-os>/bin,
#      三件齐({brickie,brickie-gen,brickie-core}), 且入口 ELF 的魔数必须是 \x7fELF。
#
# 用法: bash tools/check-build.sh   (或 make check-build)
# 退出码: 0 = 一致, 1 = 不一致, 2 = 用法/环境错
#
# 实现说明:
#   * 只用 `make -n`(干跑), 且必须配 `-B`: 不加 -B 时, 已经构建过的树会让
#     `make -n` 只说一句"无需做任何事" —— 门禁就变成"只在干净树上有效"。
#   * 递归 make 的子进程继承 `-n`, 所以干跑不会真编东西; 本脚本仍只对
#     **非破坏性**目标(default / tools)做干跑, 绝不干跑 clean。

set -uo pipefail

cd "$(dirname "$0")/.." || exit 2

MK="make"
DRY="$MK -n -B"   # -B: 无视时间戳, 已构建的树也要打印全部命令

if [ ! -f Makefile ]; then
    echo "FAIL: 找不到 Makefile" >&2
    exit 2
fi

status=0
pass() { echo "ok   $1"; }
fail() { echo "FAIL $1"; status=1; }

# ---------------------------------------------------------------- 1) 缺省目标是工具
# `make` 必须只编工具; 委派镜像的命令只许出现在显式目标(`make all`)里。
if grep -qE '^\.DEFAULT_GOAL[[:space:]]*:?=[[:space:]]*tools[[:space:]]*$' Makefile; then
    pass "缺省目标是工具(.DEFAULT_GOAL := tools)"
else
    fail "缺省目标不是 tools —— 检查 .DEFAULT_GOAL(迁移期应只编工具)"
fi
dry_default="$($DRY 2>&1)"
if printf '%s\n' "$dry_default" | grep -q -- 'tools/brickie'; then
    pass "缺省目标真的编工具(make -n -B 里进了 tools/brickie)"
else
    fail "缺省目标**不**编工具: 干跑里没有 tools/brickie"
    printf '%s\n' "$dry_default" | sed 's/^/     | /' | head -5
fi
if printf '%s\n' "$dry_default" | grep -q -- 'brickie build'; then
    fail "缺省目标顺手委派了镜像(brickie build)—— 迁移期缺省应只编工具"
else
    pass "缺省目标不委派镜像(编镜像走显式目标 make all)"
fi

# ---------------------------------------------------------------- 2) 无镜像源码/标志字面量
# 判据是**机械的**: 出现任何 `<core|platform|service>/….(c|S)` 或镜像编译标志, 即回潮。
src_hits="$(grep -nE '(core|platform|service)/[a-z0-9_/]*\.(c|S)' Makefile || true)"
if [ -z "$src_hits" ]; then
    pass "Makefile 无镜像源码字面路径(样式扫描为空)"
else
    fail "Makefile 里仍有镜像源码字面路径 —— 它们属于各插件的 [build].sources"
    printf '%s\n' "$src_hits" | sed 's/^/     | /' | head -5
fi
flag_hits="$(grep -nE '(-ffreestanding|-fno-builtin|-mstrict-align|-march=|-mgeneral-regs-only|-Wl,-T|-nostdlib|-nostartfiles|-fno-omit-frame-pointer|-fdata-sections|-ffunction-sections)' Makefile || true)"
if [ -z "$flag_hits" ]; then
    pass "Makefile 无镜像编译标志字面量(样式扫描为空)"
else
    fail "Makefile 里仍有镜像编译标志 —— 它们属于 product.toml [build] / 插件 [build]"
    printf '%s\n' "$flag_hits" | sed 's/^/     | /' | head -5
fi

# ---------------------------------------------------------------- 3) 镜像目标改成委派
# 每个镜像侧的名字都必须是薄委派(调 $(BRICKIE) 的对应命令), 且目标本身仍存在。
for t in all run smoke irq-test mem-test string-test dbg-test check-string check-headers size disasm clean-brickos; do
    if grep -qE "^$t:" Makefile; then
        pass "镜像别名存在($t)"
    else
        fail "镜像别名丢失: $t 目标不在 Makefile 里"
    fi
done
wants=(
    '$(BRICKIE) build'
    '$(BRICKIE) run'
    '$(BRICKIE) test smoke'
    '$(BRICKIE) test irq-test'
    '$(BRICKIE) test mem-test'
    '$(BRICKIE) test string-test'
    '$(BRICKIE) test dbg-test'
    '$(BRICKIE) test check-string'
    '$(BRICKIE) test check-headers'
    '$(BRICKIE) size'
    '$(BRICKIE) disasm'
    '$(BRICKIE) clean'
)
for w in "${wants[@]}"; do
    if grep -qF -- "$w" Makefile; then
        pass "委派点存在($w)"
    else
        fail "缺委派点: $w —— 镜像别名必须调 brickie, 不许自己写规则"
    fi
done

# ---------------------------------------------------------------- 4) 工具段零交叉依赖
# 边界纪律(contract §9 R-13): **组合期**命令不得要求交叉工具链在场。
#
# 判据用**交叉编译器前缀本身**(而不是裸的 'aarch64'): 工具产物现在按宿主出树,
# 路径里带 host-arch —— 在 aarch64 宿主上 `build/host/aarch64/linux/...` 会含
# 'aarch64', 拿裸词当判据会**误报**(门禁自己把好构建判红)。
cross_prefix="$($MK -s print-cross-compile 2>/dev/null)"
dry_tools="$($DRY tools 2>&1)"
if [ -n "$cross_prefix" ] && printf '%s\n' "$dry_tools" | grep -qF "$cross_prefix"; then
    fail "'make tools' 的干跑里出现了交叉工具链 '$cross_prefix' —— 工具段被污染"
    printf '%s\n' "$dry_tools" | grep -F "$cross_prefix" | sed 's/^/     | /' | head -3
else
    pass "'make tools' 不触及交叉工具链(纯宿主; 前缀 '$cross_prefix')"
fi

# ---------------------------------------------------------------- 5) 宿主产物出树
# 需求(参考 Android): tools 的 bin 编到 build/host/<host-arch>/<host-os>/bin。
host_bin="$($MK -s print-host-bin-dir 2>/dev/null)"
host_triple="$($MK -s print-host-triple 2>/dev/null)"
if printf '%s\n' "$host_bin" | grep -qE '/build/host/[^/]+/[^/]+/bin$'; then
    pass "宿主 bin 目录符合 build/host/<host-arch>/<host-os>/bin($host_triple)"
else
    fail "宿主 bin 目录不是 build/host/<host-arch>/<host-os>/bin: '$host_bin'"
fi
if [ -n "$host_bin" ] && printf '%s\n' "$dry_default" | grep -qF -- "-o $host_bin/brickie-gen"; then
    pass "生成器产物落在宿主 bin 目录(-o $host_bin/brickie-gen)"
else
    fail "生成器产物**没有**落在宿主 bin 目录($host_bin/brickie-gen)"
    printf '%s\n' "$dry_default" | grep -E ' -o ' | sed 's/^/     | /' | head -3
fi
# 入口 ELF(Python 前端)也必须出树 —— 注意正则要排除 brickie-gen 的前缀匹配。
if [ -n "$host_bin" ] && printf '%s\n' "$dry_default" | grep -qE -- "-o $host_bin/brickie( |\$|\))"; then
    pass "入口 ELF 落在宿主 bin 目录(-o $host_bin/brickie)"
else
    fail "入口 ELF(Python 前端)**没有**落在宿主 bin 目录($host_bin/brickie)"
fi
# 原生工具必须**嵌进**入口 ELF(需求: "brickie-gen 等工具均需要编译到 brickie elf 中")。
# 判据是打包命令里带 --embed <构建产物>=bin/<名>; 否则入口 ELF 只是"壳", 不是自包含的。
for tool in brickie-gen brickie-core; do
    if printf '%s\n' "$dry_default" | grep -qE -- "--embed .*${tool}=bin/${tool}"; then
        pass "打包命令把原生工具嵌进入口 ELF(--embed ...=bin/${tool})"
    else
        fail "打包命令**没有**把 ${tool} 嵌进入口 ELF(缺 --embed ...=bin/${tool})"
        printf '%s\n' "$dry_default" | grep -F -- 'freeze.py' | sed 's/^/     | /' | head -3
    fi
done
# L0/L1 的 Rust 核心: 构建命令必须把它**出树**到宿主 bin 目录。
# `core` 是 **tools/brickie/Makefile 里的目标**(不在 `tools` 的依赖里: 没有 cargo
# 的 checkout 也要能编 L5/L2), 所以这里**直接进那一层干跑**。
dry_core="$($DRY -C tools/brickie core 2>&1)"
if [ -n "$host_bin" ] && printf '%s\n' "$dry_core" | grep -qF -- "$host_bin/brickie-core"; then
    pass "Rust 核心产物落在宿主 bin 目录($host_bin/brickie-core)"
else
    fail "Rust 核心(brickie-core)**没有**落在宿主 bin 目录($host_bin/brickie-core)"
    printf '%s\n' "$dry_core" | grep -F -- 'brickie-core' | sed 's/^/     | /' | head -3
fi
if printf '%s\n' "$dry_default" | grep -qE ' -o (main|json|diag|text|rules|gen_new|render)\.o'; then
    fail "工具段仍在源码树里落中间产物(-o <name>.o)"
    printf '%s\n' "$dry_default" | grep -E ' -o [a-z_]+\.o' | sed 's/^/     | /' | head -3
else
    pass "工具段不在源码树里落中间产物(出树构建)"
fi

# ---------------------------------------------------------------- 6) 自举种子进库
# build/host/** 是派生(不进库), prebuilts/seed/brickie/<arch>/<os>/bin 是随源码提交的种子。
# 这里**不**干跑 tools-prebuilt(它的递归行会真去写盘); 只查目录约定 + 种子是否就位。
seed_bin="$($MK -s print-prebuilt-bin-dir 2>/dev/null)"
if printf '%s\n' "$seed_bin" | grep -qE '/prebuilts/seed/brickie/[^/]+/[^/]+/bin$'; then
    pass "自举种子目录符合 prebuilts/seed/brickie/<host-arch>/<host-os>/bin"
else
    fail "自举种子目录不符合约定: '$seed_bin'"
fi
for name in brickie brickie-gen brickie-core; do
    if [ -x "$seed_bin/$name" ]; then
        pass "自举种子就位($seed_bin/$name)"
    else
        fail "自举种子缺失: $seed_bin/$name —— 跑 'make tools-prebuilt' 发布一份"
    fi
done
# 入口 ELF 必须是真 ELF(魔数 7f 45 4c 46): "编译为 elf" 是硬需求, 不许退化成脚本。
if [ -f "$seed_bin/brickie" ] && [ "$(head -c 4 "$seed_bin/brickie" | od -An -tx1 | tr -d ' \n')" = "7f454c46" ]; then
    pass "自举种子里的 brickie 是真 ELF(\\x7fELF)"
else
    fail "自举种子里的 brickie 不是 ELF(魔数不符)"
fi

if [ "$status" -eq 0 ]; then
    echo "PASS: 构建接线一致(缺省目标=工具 / 无字面源码 / 镜像委派 / 工具段零交叉依赖 / 宿主产物出树 / 自举种子进库)"
else
    echo "提示: 改 Makefile 的 tools 段或镜像别名后请重跑本检查" >&2
fi

exit "$status"
