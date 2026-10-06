#!/usr/bin/env bash
# brickOS — 开发环境配置(**source 本文件**)
#
#   source setup.sh                # 配置 PATH + 环境变量
#   source setup.sh --quiet        # 静默(只出错时说话)
#   source setup.sh --with-make    # 额外把 prebuilts 的 make 放到 PATH 最前(会遮蔽宿主 make)
#   source setup.sh --unset        # 撤销, 恢复到 source 之前
#   bash   setup.sh --print        # 不 source, 只打印可 eval 的片段(CI 用)
#
# ## 为什么必须 source
# 子进程改不了父 shell 的环境 —— `./setup.sh` 只能是"假装成功"。本脚本检测到
# 被直接执行时会明确拒绝并给出正确写法, 而不是静默无效。
#
# ## 配置了什么(每一项都有真实消费者, 不设空转变量)
#
#   PATH                     ← 见下「PATH 追加顺序」
#   BRICKOS_ROOT             = 本仓根
#   BRICKOS_HOST_TRIPLE      = <host-arch>/<host-os>(来自 tools/host-detect.sh, 唯一真值)
#   BRICKIE_REPO_ROOT        → tools/brickie/python/brickie/native.py 读
#   BRICKIE_TOOL_ROOT        → tools/brickie/python/brickie/native.py 读
#   CROSS_COMPILE            → 顶层 Makefile 读(交叉编译器前缀)
#
# ## PATH 追加顺序(左 = 优先, 与砖具前端的查找顺序一致)
#
#   build/host/<triple>/bin            本机新编的 brickie / brickie-gen(**优先于种子**)
#   prebuilts/seed/brickie/<triple>/bin 自举种子(没有 build/ 时兜底)
#   prebuilts/toolchain/<arm…>/bin      交叉工具链 aarch64-none-elf-*(锁版本)
#   prebuilts/toolchain/ninja/bin       构建后端 ninja(BR-D4 备选 B)
#   prebuilts/toolchain/make/bin        **仅 --with-make** —— 它会遮蔽宿主 make, 故不默认加
#
# 刻意**不设** BRICKIE_GEN: 它的语义是"钉死生成器", 设了就会盖掉"本机新编优先"
# 那条查找规则(native.py 的顺序)。需要钉死时请自己显式 export。
#
# ## 幂等
# 重复 source 安全: 每次先剔除 PATH 里**属于本仓**的全部条目再重排, 不会重复堆积。

# ------------------------------------------------------------------ 参数
_brickos_mode="apply"
_brickos_quiet=0
_brickos_with_make=0
for _brickos_arg in "$@"; do
    case "$_brickos_arg" in
        --unset|-u)    _brickos_mode="unset" ;;
        --print)       _brickos_mode="print" ;;
        --quiet|-q)    _brickos_quiet=1 ;;
        --with-make)   _brickos_with_make=1 ;;
        -h|--help)
            sed -n '2,40p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
            return 0 2>/dev/null || exit 0 ;;
        *) echo "setup.sh: 未知参数 '$_brickos_arg'(试 --help)" >&2; return 2 2>/dev/null || exit 2 ;;
    esac
done
unset _brickos_arg

# ------------------------------------------------------------------ 定位与自检
_brickos_self="${BASH_SOURCE[0]}"
_brickos_root="$(cd "$(dirname "$_brickos_self")" && pwd -P)"
unset _brickos_self

if [ "$_brickos_mode" != "print" ] && [ "${BASH_SOURCE[0]}" = "${0}" ]; then
    cat >&2 <<EOF
setup.sh: **必须 source**, 不能直接执行 —— 子进程改不了父 shell 的环境。

    source setup.sh            # 或:  . setup.sh
    bash   setup.sh --print    # CI 里要片段时用这个

EOF
    unset _brickos_mode _brickos_quiet _brickos_with_make _brickos_root
    exit 2
fi

_brickos_banner() { [ "$_brickos_quiet" = "1" ] || echo "$@"; }
_brickos_warn()   { echo "setup.sh: $*" >&2; }

# ------------------------------------------------------------------ 变量清单
# 我们管理的变量(apply 时保存原值, unset 时还原)
_brickos_vars="BRICKOS_ROOT BRICKOS_HOST_TRIPLE BRICKIE_REPO_ROOT BRICKIE_TOOL_ROOT CROSS_COMPILE"

# ------------------------------------------------------------------ --unset
if [ "$_brickos_mode" = "unset" ]; then
    # 剔掉 PATH 里属于本仓的条目
    _brickos_newpath=""
    _brickos_oldifs="$IFS"; IFS=:
    for _brickos_p in $PATH; do
        case "$_brickos_p" in
            "$_brickos_root"|"$_brickos_root"/*) continue ;;
        esac
        _brickos_newpath="${_brickos_newpath:+$_brickos_newpath:}$_brickos_p"
    done
    IFS="$_brickos_oldifs"
    export PATH="$_brickos_newpath"

    for _brickos_v in $_brickos_vars; do
        _brickos_saved="_BRICKOS_SAVED_$_brickos_v"
        if [ -n "${!_brickos_saved+x}" ]; then
            eval "export $_brickos_v=\"\${$_brickos_saved}\""
        else
            unset "$_brickos_v"
        fi
        unset "$_brickos_saved"
    done
    unset _brickos_v _brickos_newpath _brickos_oldifs _brickos_p _brickos_saved

    _brickos_banner "brickOS: 环境已撤销(本仓条目已从 PATH 移除, 变量已还原)"
    unset _brickos_mode _brickos_quiet _brickos_with_make _brickos_root _brickos_vars
    unset -f _brickos_banner _brickos_warn 2>/dev/null
    return 0 2>/dev/null || exit 0
fi

# ------------------------------------------------------------------ 宿主三元组
# 唯一真值 = tools/host-detect.sh(与 mk/host.mk / check-build.sh / 用例同源)。
_brickos_triple_raw="$("$_brickos_root/tools/host-detect.sh" 2>/dev/null)"
_brickos_arch="$(printf '%s' "$_brickos_triple_raw" | cut -d' ' -f1)"
_brickos_os="$(printf '%s' "$_brickos_triple_raw" | cut -d' ' -f2)"
if [ -z "$_brickos_arch" ] || [ -z "$_brickos_os" ]; then
    _brickos_warn "tools/host-detect.sh 没给出宿主三元组; 退回 uname"
    _brickos_arch="$(uname -m)"; _brickos_os="$(uname -s | tr 'A-Z' 'a-z')"
fi
_brickos_triple="$_brickos_arch/$_brickos_os"

# ------------------------------------------------------------------ 工具链路径
# 路径真值 = 锁文件(经 fetch-prebuilt.py --print-env 导出), **不在 shell 里重算**。
_brickos_cross=""
_brickos_make_dir=""
_brickos_bin_dirs=""
if command -v python3 >/dev/null 2>&1; then
    # shellcheck disable=SC1090
    eval "$(python3 "$_brickos_root/tools/fetch-prebuilt.py" --print-env 2>/dev/null)"
    _brickos_cross="$PREBUILT_CROSS"
    _brickos_make_dir="$PREBUILT_MAKE_BIN_DIR"
    _brickos_bin_dirs="$PREBUILT_BIN_DIRS"
else
    _brickos_warn "找不到 python3 —— 无法从锁文件解析工具链路径(brickie 本身也要求 python3)"
fi

# 组装要前置的目录(左 → 右 = 优先级由高到低)
_brickos_prepend="$_brickos_root/build/host/$_brickos_triple/bin"
_brickos_prepend="$_brickos_prepend:$_brickos_root/prebuilts/seed/brickie/$_brickos_triple/bin"
[ "$_brickos_with_make" = "1" ] && [ -n "$_brickos_make_dir" ] && \
    _brickos_prepend="$_brickos_prepend:$_brickos_root/$_brickos_make_dir"
for _brickos_d in $_brickos_bin_dirs; do
    # 默认跳过 make 目录(遮蔽宿主 make); 只有 --with-make 才收
    if [ "$_brickos_d" = "$_brickos_make_dir" ] && [ "$_brickos_with_make" != "1" ]; then
        continue
    fi
    _brickos_prepend="$_brickos_prepend:$_brickos_root/$_brickos_d"
done
unset _brickos_d

# 缺失提示(只提示, 不阻断 —— PATH 里放一个不存在的目录无害)
_brickos_missing=""
for _brickos_d in $(printf '%s' "$_brickos_prepend" | tr ':' ' '); do
    [ -d "$_brickos_d" ] || _brickos_missing="$_brickos_missing $_brickos_d"
done
unset _brickos_d

# ------------------------------------------------------------------ --print
if [ "$_brickos_mode" = "print" ]; then
    echo "# brickOS 开发环境 —— 用法: eval \"\$(bash setup.sh --print)\""
    echo "export BRICKOS_ROOT='$_brickos_root'"
    echo "export BRICKOS_HOST_TRIPLE='$_brickos_triple'"
    echo "export BRICKIE_REPO_ROOT='$_brickos_root'"
    echo "export BRICKIE_TOOL_ROOT='$_brickos_root/tools/brickie'"
    [ -n "$_brickos_cross" ] && echo "export CROSS_COMPILE='$_brickos_root/$_brickos_cross'"
    echo "export PATH='$_brickos_prepend:\$PATH'"
    unset _brickos_mode _brickos_quiet _brickos_with_make _brickos_root _brickos_triple
    unset _brickos_arch _brickos_os _brickos_triple_raw _brickos_cross _brickos_make_dir
    unset _brickos_bin_dirs _brickos_prepend _brickos_missing
    unset -f _brickos_banner _brickos_warn 2>/dev/null
    return 0 2>/dev/null || exit 0
fi

# ------------------------------------------------------------------ 应用
# 先剔除本仓旧条目(幂等), 再整体前置
_brickos_newpath=""
_brickos_oldifs="$IFS"; IFS=:
for _brickos_p in $PATH; do
    case "$_brickos_p" in
        "$_brickos_root"|"$_brickos_root"/*) continue ;;
    esac
    _brickos_newpath="${_brickos_newpath:+$_brickos_newpath:}$_brickos_p"
done
IFS="$_brickos_oldifs"
export PATH="$_brickos_prepend${_brickos_newpath:+:$_brickos_newpath}"

# 保存原值(仅首次; 避免二次 source 把"我们设的"当原值存下来)
for _brickos_v in $_brickos_vars; do
    _brickos_saved="_BRICKOS_SAVED_$_brickos_v"
    [ -n "${!_brickos_saved+x}" ] || eval "_BRICKOS_SAVED_$_brickos_v=\"\${$_brickos_v-}\""
done
unset _brickos_v _brickos_saved

export BRICKOS_ROOT="$_brickos_root"
export BRICKOS_HOST_TRIPLE="$_brickos_triple"
export BRICKIE_REPO_ROOT="$_brickos_root"
export BRICKIE_TOOL_ROOT="$_brickos_root/tools/brickie"
[ -n "$_brickos_cross" ] && export CROSS_COMPILE="$_brickos_root/$_brickos_cross"

# ------------------------------------------------------------------ 汇报
if [ "$_brickos_quiet" != "1" ]; then
    echo "brickOS 开发环境已配置  [$_brickos_triple]"
    echo "  PATH 前置(左优先):"
    printf '%s' "$_brickos_prepend" | tr ':' '\n' | sed "s|^$_brickos_root/||" | sed 's/^/    /'
    echo
    echo "  环境变量:"
    printf '    %-22s %s\n' CROSS_COMPILE "${CROSS_COMPILE:-(未设)}"
    printf '    %-22s %s\n' BRICKOS_ROOT "$BRICKOS_ROOT"
    printf '    %-22s %s\n' BRICKIE_REPO_ROOT "$BRICKIE_REPO_ROOT"
    printf '    %-22s %s\n' BRICKIE_TOOL_ROOT "$BRICKIE_TOOL_ROOT"
    if [ -n "$_brickos_missing" ]; then
        echo "  ⚠ 以下目录还不存在(未取件/未构建, 无害):"
        printf '%s' "$_brickos_missing" | tr ' ' '\n' | sed '/^$/d' | sed "s|^$_brickos_root/||" | sed 's/^/      /'
        echo "    → 取外部工具链:  make prebuilt"
        echo "    → 编本机工具:    make tools"
    fi
fi
[ "$_brickos_with_make" = "1" ] && [ -n "$_brickos_make_dir" ] && \
    _brickos_banner "  注意: prebuilts 的 make 已在 PATH 最前, 它会遮蔽宿主 make"

# 清理临时变量, 只留下导出项(避免污染调用者的 shell 命名空间)
unset _brickos_mode _brickos_quiet _brickos_with_make _brickos_root _brickos_triple
unset _brickos_arch _brickos_os _brickos_triple_raw _brickos_cross _brickos_make_dir
unset _brickos_bin_dirs _brickos_prepend _brickos_missing _brickos_newpath _brickos_oldifs
unset _brickos_p _brickos_vars
unset -f _brickos_banner _brickos_warn 2>/dev/null

return 0 2>/dev/null || exit 0
