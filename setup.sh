#!/usr/bin/env bash
# brickOS dev environment setup — source this file (a child process cannot change the parent
# shell's environment; direct execution is refused).
#   source setup.sh [--quiet] [--with-make] [--unset]   |   bash setup.sh --print (eval-able, CI)
# Sets PATH, BRICKOS_ROOT, BRICKOS_HOST_TRIPLE, BRICKIE_REPO_ROOT, BRICKIE_TOOL_ROOT,
# CROSS_COMPILE. BRICKIE_GEN stays unset: setting it would override native.py's "locally built
# first" lookup order; export it yourself to pin.
# PATH prepend order (highest first): build/host/<triple>/bin, the bootstrap seed
# prebuilts/seed/brickie/<triple>/bin, the version-locked cross toolchain, ninja; make only with
# --with-make (it shadows host make). Idempotent: every run drops repo-owned PATH entries first.

# ------------------------------------------------------------------ arguments
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
        *) echo "setup.sh: unknown argument '$_brickos_arg' (try --help)" >&2; return 2 2>/dev/null || exit 2 ;;
    esac
done
unset _brickos_arg

# ------------------------------------------------------------------ locate and self-check
_brickos_self="${BASH_SOURCE[0]}"
_brickos_root="$(cd "$(dirname "$_brickos_self")" && pwd -P)"
unset _brickos_self

if [ "$_brickos_mode" != "print" ] && [ "${BASH_SOURCE[0]}" = "${0}" ]; then
    cat >&2 <<EOF
setup.sh: **must be sourced**, not executed directly — a child process cannot change the parent shell's environment.

    source setup.sh            # or:  . setup.sh
    bash   setup.sh --print    # use this when CI needs a snippet

EOF
    unset _brickos_mode _brickos_quiet _brickos_with_make _brickos_root
    exit 2
fi

_brickos_banner() { [ "$_brickos_quiet" = "1" ] || echo "$@"; }
_brickos_warn()   { echo "setup.sh: $*" >&2; }

# ------------------------------------------------------------------ variable list
_brickos_vars="BRICKOS_ROOT BRICKOS_HOST_TRIPLE BRICKIE_REPO_ROOT BRICKIE_TOOL_ROOT CROSS_COMPILE"

# ------------------------------------------------------------------ --unset
if [ "$_brickos_mode" = "unset" ]; then
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

    _brickos_banner "brickOS: environment undone (repo entries removed from PATH, variables restored)"
    unset _brickos_mode _brickos_quiet _brickos_with_make _brickos_root _brickos_vars
    unset -f _brickos_banner _brickos_warn 2>/dev/null
    return 0 2>/dev/null || exit 0
fi

# ------------------------------------------------------------------ host triple
# tools/host-detect.sh is the single source of truth for the host triple.
_brickos_triple_raw="$("$_brickos_root/tools/host-detect.sh" 2>/dev/null)"
_brickos_arch="$(printf '%s' "$_brickos_triple_raw" | cut -d' ' -f1)"
_brickos_os="$(printf '%s' "$_brickos_triple_raw" | cut -d' ' -f2)"
if [ -z "$_brickos_arch" ] || [ -z "$_brickos_os" ]; then
    _brickos_warn "tools/host-detect.sh gave no host triple; falling back to uname"
    _brickos_arch="$(uname -m)"; _brickos_os="$(uname -s | tr 'A-Z' 'a-z')"
fi
_brickos_triple="$_brickos_arch/$_brickos_os"

# ------------------------------------------------------------------ toolchain paths
# Path truth is the lock file (fetch-prebuilt.py --print-env), never recomputed here.
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
    _brickos_warn "python3 not found — cannot resolve toolchain paths from the lock file (brickie itself needs python3 too)"
fi

# Assemble the directories to prepend, left = highest priority.
_brickos_prepend="$_brickos_root/build/host/$_brickos_triple/bin"
_brickos_prepend="$_brickos_prepend:$_brickos_root/prebuilts/seed/brickie/$_brickos_triple/bin"
[ "$_brickos_with_make" = "1" ] && [ -n "$_brickos_make_dir" ] && \
    _brickos_prepend="$_brickos_prepend:$_brickos_root/$_brickos_make_dir"
for _brickos_d in $_brickos_bin_dirs; do
    # Skip make by default (it shadows host make); only --with-make takes it
    if [ "$_brickos_d" = "$_brickos_make_dir" ] && [ "$_brickos_with_make" != "1" ]; then
        continue
    fi
    _brickos_prepend="$_brickos_prepend:$_brickos_root/$_brickos_d"
done
unset _brickos_d

# Missing-directory notice only; a nonexistent dir on PATH is harmless.
_brickos_missing=""
for _brickos_d in $(printf '%s' "$_brickos_prepend" | tr ':' ' '); do
    [ -d "$_brickos_d" ] || _brickos_missing="$_brickos_missing $_brickos_d"
done
unset _brickos_d

# ------------------------------------------------------------------ --print
if [ "$_brickos_mode" = "print" ]; then
    echo "# brickOS dev environment -- usage: eval \"\$(bash setup.sh --print)\""
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

# ------------------------------------------------------------------ apply
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

# Save originals once, so a second source does not store our values as the original.
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

# ------------------------------------------------------------------ report
if [ "$_brickos_quiet" != "1" ]; then
    echo "brickOS dev environment configured  [$_brickos_triple]"
    echo "  PATH prepended (left first):"
    printf '%s' "$_brickos_prepend" | tr ':' '\n' | sed "s|^$_brickos_root/||" | sed 's/^/    /'
    echo
    echo "  environment variables:"
    printf '    %-22s %s\n' CROSS_COMPILE "${CROSS_COMPILE:-(unset)}"
    printf '    %-22s %s\n' BRICKOS_ROOT "$BRICKOS_ROOT"
    printf '    %-22s %s\n' BRICKIE_REPO_ROOT "$BRICKIE_REPO_ROOT"
    printf '    %-22s %s\n' BRICKIE_TOOL_ROOT "$BRICKIE_TOOL_ROOT"
    if [ -n "$_brickos_missing" ]; then
        echo "  ! these directories do not exist yet (not fetched/built; harmless):"
        printf '%s' "$_brickos_missing" | tr ' ' '\n' | sed '/^$/d' | sed "s|^$_brickos_root/||" | sed 's/^/      /'
        echo "    -> fetch the external toolchain:  make prebuilt"
        echo "    -> build host tools:              make tools"
        echo "    -> build images:                  brickie build   (images enter via brickie, not make; see ADR-0004)"
    fi
fi
[ "$_brickos_with_make" = "1" ] && [ -n "$_brickos_make_dir" ] && \
    _brickos_banner "  note: prebuilts make is first on PATH and will shadow host make"

# Leave only the exported variables (do not pollute the caller's namespace).
unset _brickos_mode _brickos_quiet _brickos_with_make _brickos_root _brickos_triple
unset _brickos_arch _brickos_os _brickos_triple_raw _brickos_cross _brickos_make_dir
unset _brickos_bin_dirs _brickos_prepend _brickos_missing _brickos_newpath _brickos_oldifs
unset _brickos_p _brickos_vars
unset -f _brickos_banner _brickos_warn 2>/dev/null

return 0 2>/dev/null || exit 0
