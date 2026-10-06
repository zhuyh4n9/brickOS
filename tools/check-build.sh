#!/usr/bin/env bash
#
# brickOS prototype — 构建接线一致性检查(工具段 ↔ 镜像段)
#
# 为什么需要这个脚本:
#   "工具先于镜像"是一条**顺序纪律**, 而顺序纪律最容易的坏法不是报错, 是**静默**:
#   把 tools/ 段写成文件里的第一条规则, `.DEFAULT_GOAL` 就变成了 tools ——
#   于是 `make` 只编工具、然后 exit 0, 镜像根本没编, 而没有任何错误输出。
#   (这个坑真踩过一次, 所以把三条不变量钉成门禁。)
#
# 六条不变量:
#   1) 缺省目标真的编镜像(不是编完工具就收工);
#   2) 工具段排在镜像段之前;
#   3) 顺序靠 order-only 依赖而非行序(-j 也不倒);
#   4) 工具段零交叉依赖(判据 = 交叉编译器前缀本身, 不是裸 'aarch64');
#   5) 宿主产物**出树**到 build/host/<host-arch>/<host-os>/bin(参考 Android):
#      brickie-gen(L2)与 brickie(自包含入口 ELF)两个都要, 且原生工具要**嵌进**
#      入口 ELF(`--embed <产物>=bin/<名>`) —— 需求: "brickie-gen 等工具均需要
#      编译到 python 的 brickie elf 中";
#   6) 自举种子**进库**在 prebuilts/seed/brickie/<host-arch>/<host-os>/bin(见 ADR-0004),
#      且入口 ELF 的魔数必须是 \x7fELF("编译为 elf" 不许退化成脚本)。
#
# 用法: bash tools/check-build.sh   (或 make check-build)
# 退出码: 0 = 一致, 1 = 不一致, 2 = 用法/环境错
#
# 实现说明:
#   * 只用 `make -n`(干跑), 且必须配 `-B`: 不加 -B 时, 已经构建过的树会让
#     `make -n` 只说一句"无需做任何事" —— 门禁就变成"只在干净树上有效",
#     而 CI 里它多半是在**构建之后**被调用的(本脚本第一版就踩了这条)。
#   * GNU make 对含 `$(MAKE)` 的递归行**即使 -n 也会真的执行**, 所以本脚本只对
#     **非破坏性**目标(all / tools)做干跑, 绝不干跑 clean。

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

# ---------------------------------------------------------------- 1) 缺省目标
# `make` 必须真的去编镜像, 不能只编工具就收工。
dry_all="$($DRY 2>&1)"
if printf '%s\n' "$dry_all" | grep -q -- '-o build/brick.elf'; then
    pass "缺省目标会构建镜像(make -n -B 含 '-o build/brick.elf')"
else
    fail "缺省目标**不**构建镜像: 'make' 很可能停在了 tools 段(检查 .DEFAULT_GOAL)"
    printf '%s\n' "$dry_all" | sed 's/^/     | /' | head -5
fi

# ---------------------------------------------------------------- 2) 工具在前
# 干跑的串行顺序里, 进 tools 的递归必须在第一条交叉编译之前。
#
# ⚠ 编译器名**必须**从项目自己的真值取(`make print-cross-compile`), 不许硬编码:
#   早先这里写死了 `aarch64-linux-gnu-gcc`, 结果切到 prebuilt 的 Arm 工具链
#   (`aarch64-none-elf-gcc`)后本项立刻**误报**"镜像段没被触发" —— 门禁自己成了
#   工具链选型的耦合点, 正是它该避免的事。
cross_prefix="$($MK -s print-cross-compile 2>/dev/null)"
cross_re="$(printf '%s' "$cross_prefix" | sed 's/[.[\*^$()+?{}|]/\\&/g')gcc.*-c "
line_tools="$(printf '%s\n' "$dry_all" | grep -n 'tools/brickie' | head -1 | cut -d: -f1)"
line_cross="$(printf '%s\n' "$dry_all" | grep -nE "$cross_re" | head -1 | cut -d: -f1)"
if [ -n "$line_tools" ] && [ -n "$line_cross" ] && [ "$line_tools" -lt "$line_cross" ]; then
    pass "工具段排在镜像段之前(行 $line_tools < 行 $line_cross; 编译器前缀 $(printf '%s' "$cross_prefix" | sed 's|.*/bin/||'))"
elif [ -z "$line_tools" ]; then
    fail "干跑里没有 tools 段 —— 工具根本没被接进构建系统"
elif [ -z "$line_cross" ]; then
    fail "干跑里没有交叉编译行(按 print-cross-compile='$cross_prefix' 找) —— 镜像段没被触发"
else
    fail "顺序反了: 交叉编译在行 $line_cross, 工具段在行 $line_tools"
fi

# ---------------------------------------------------------------- 3) 顺序靠依赖而非靠行序
# 上一条只证明"串行干跑是这个顺序"。真正让 `make -j` 也不倒过来的, 是这条
# order-only 依赖; 它一旦被重构掉, 上一条就会靠运气通过。
if grep -qE '^\$\(OBJS\)[[:space:]]*:[[:space:]]*\|[[:space:]]*tools' Makefile; then
    pass "存在 order-only 依赖 \$(OBJS): | tools(-j 也不会倒过来)"
else
    fail "Makefile 里找不到 '\$(OBJS): | tools' —— 并行构建下顺序无保证"
fi

# ---------------------------------------------------------------- 4) 工具段零交叉依赖
# brickie-v0.1 §0 的边界纪律: v0.1 的工具不得要求交叉工具链在场。
#
# 判据用**交叉编译器前缀本身**(而不是裸的 'aarch64'): 工具产物现在按宿主出树,
# 路径里带 host-arch —— 在 aarch64 宿主上 `build/host/aarch64/linux/...` 会含
# 'aarch64', 拿裸词当判据会**误报**(门禁自己把好构建判红)。
cross_prefix="$(make -s print-cross-compile 2>/dev/null)"
dry_tools="$($DRY tools 2>&1)"
if [ -n "$cross_prefix" ] && printf '%s\n' "$dry_tools" | grep -qF "$cross_prefix"; then
    fail "'make tools' 的干跑里出现了交叉工具链 '$cross_prefix' —— 工具段被污染"
    printf '%s\n' "$dry_tools" | grep -F "$cross_prefix" | sed 's/^/     | /' | head -3
else
    pass "'make tools' 不触及交叉工具链(纯宿主; 前缀 '$cross_prefix')"
fi

# ---------------------------------------------------------------- 5) 宿主产物出树
# 需求(参考 Android): tools 的 bin 编到 build/host/<host-arch>/<host-os>/bin。
# 落点由 mk/host.mk 给出; 这里只核对**实际命令**写的是那个目录。
host_bin="$(make -s print-host-bin-dir 2>/dev/null)"
host_triple="$(make -s print-host-triple 2>/dev/null)"
if printf '%s\n' "$host_bin" | grep -qE '/build/host/[^/]+/[^/]+/bin$'; then
    pass "宿主 bin 目录符合 build/host/<host-arch>/<host-os>/bin($host_triple)"
else
    fail "宿主 bin 目录不是 build/host/<host-arch>/<host-os>/bin: '$host_bin'"
fi
if [ -n "$host_bin" ] && printf '%s\n' "$dry_tools" | grep -qF -- "-o $host_bin/brickie-gen"; then
    pass "生成器产物落在宿主 bin 目录(-o $host_bin/brickie-gen)"
else
    fail "生成器产物**没有**落在宿主 bin 目录($host_bin/brickie-gen)"
    printf '%s\n' "$dry_tools" | grep -E ' -o ' | sed 's/^/     | /' | head -3
fi
# 入口 ELF(Python 前端)也必须出树 —— 注意正则要排除 brickie-gen 的前缀匹配。
if [ -n "$host_bin" ] && printf '%s\n' "$dry_tools" | grep -qE -- "-o $host_bin/brickie( |\$)"; then
    pass "入口 ELF 落在宿主 bin 目录(-o $host_bin/brickie)"
else
    fail "入口 ELF(Python 前端)**没有**落在宿主 bin 目录($host_bin/brickie)"
fi
# 原生工具必须**嵌进**入口 ELF(需求: "brickie-gen 等工具均需要编译到 brickie elf 中")。
# 判据是打包命令里带 --embed <构建产物>=bin/<名>; 否则入口 ELF 只是"壳", 不是自包含的。
if printf '%s\n' "$dry_tools" | grep -qE -- '--embed .*brickie-gen=bin/brickie-gen'; then
    pass "打包命令把原生工具嵌进入口 ELF(--embed ...=bin/brickie-gen)"
else
    fail "打包命令**没有**把 brickie-gen 嵌进入口 ELF(缺 --embed ...=bin/brickie-gen)"
    printf '%s\n' "$dry_tools" | grep -F -- 'freeze.py' | sed 's/^/     | /' | head -3
fi
if printf '%s\n' "$dry_tools" | grep -qE ' -o (main|json|diag|text|rules|gen_new)\.o'; then
    fail "工具段仍在源码树里落中间产物(-o <name>.o)"
    printf '%s\n' "$dry_tools" | grep -E ' -o [a-z_]+\.o' | sed 's/^/     | /' | head -3
else
    pass "工具段不在源码树里落中间产物(出树构建)"
fi

# ---------------------------------------------------------------- 6) 自举种子进库
# build/host/** 是派生(不进库), prebuilts/seed/brickie/<arch>/<os>/bin 是随源码提交的种子。
# 这里**不**干跑 tools-prebuilt(它的递归行会真去写盘); 只查目录约定 + 种子是否就位。
seed_bin="$(make -s print-prebuilt-bin-dir 2>/dev/null)"
if printf '%s\n' "$seed_bin" | grep -qE '/prebuilts/seed/brickie/[^/]+/[^/]+/bin$'; then
    pass "自举种子目录符合 prebuilts/seed/brickie/<host-arch>/<host-os>/bin"
else
    fail "自举种子目录不符合约定: '$seed_bin'"
fi
for name in brickie brickie-gen; do
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
    echo "PASS: 构建接线一致(缺省目标 / 顺序 / 依赖 / 工具段零交叉依赖 / 宿主产物出树 / 自举种子进库)"
else
    echo "提示: 改 Makefile 的 tools 段后请重跑本检查" >&2
fi

exit "$status"
