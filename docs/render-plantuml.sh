#!/usr/bin/env bash
# ============================================================================
#  TangramOS 设计文档 —— PlantUML 图表编译脚本
#
#  作用: 把 docs/**/plantUML/*.puml 编译成同级 pics/*.png, 供 markdown 引用。
#        人类只改 plantUML/ 下的图源, 然后跑这个脚本刷新图片。
#
#  目录约定:
#      docs/<分类>/<文档>.md
#      docs/<分类>/plantUML/<文档>-NN.puml     <- 图源(人改这个)
#      docs/<分类>/pics/<文档>-NN.png          <- 编译产物(markdown 引用这个)
#
#  典型用法:
#      ./render-plantuml.sh                 # 增量编译(按内容比对, 一致就不动)
#      ./render-plantuml.sh -w              # 挂着监听, 改完存盘自动重编译
#      ./render-plantuml.sh -l              # 列出 源 → 图 映射与新鲜度
#      ./render-plantuml.sh -c              # 只查语法(CI 用, 不写文件)
#      ./render-plantuml.sh -V              # 严格校验: 与已提交图片逐字节比对(CI 门禁)
#
#  三个关键设计:
#      1) 一律"先渲染到临时目录, 再决定是否落盘" —— PlantUML 语法出错时会生成一张
#         错误图, 这样保证 pics/ 里永远不会出现错误图(宁可保留旧图)。
#      2) 是否重编译按【内容】判断, 不看 mtime —— 渲染结果与已提交图片逐字节一致
#         就跳过, 因此不会制造无意义的 git 改动, 也不会被 touch/checkout 骗到。
#         (PlantUML 输出是确定性的: PNG 内嵌图源、无时间戳, 同源同工具必同字节)
#      3) 因为 PNG 内嵌了图源, 校验不一致时可以把内嵌源码抽出来和当前 .puml 做 diff,
#         直接告诉人"哪里改了"或"只是工具链版本不同"。
#
#  退出码: 0=成功  1=有编译失败/校验不通过  2=用法或环境错误
# ============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DOCS_DIR="$SCRIPT_DIR"

MODE=render
FORMAT=png
FORCE=0
DRY=0
QUIET=0
VERBOSE=0
WATCH_SEC=2
declare -a TARGETS=()
declare -a SOURCES=()

info() { [ "$QUIET" = 1 ] || printf '%s\n' "$*"; }
warn() { printf '警告: %s\n' "$*" >&2; }
err()  { printf '错误: %s\n' "$*" >&2; }
die()  { err "$*"; exit 2; }

usage() {
    cat <<'EOF'
用法: render-plantuml.sh [选项] [路径...]

把 docs/**/plantUML/*.puml 编译成同级 pics/<同名>.<格式>。
是否重编译按内容判断: 渲染结果与已有图片逐字节一致就不动它。

选项:
  -f, --force        即使内容一致也重写图片(一般不需要)
  -c, --check        只做语法校验, 不写任何文件(提交前快查 / CI)
  -V, --verify       严格校验: 与已提交的图片逐字节比对, 一致才通过(CI 门禁)
                     不一致时会把 PNG 内嵌的图源抽出来和当前 .puml 做 diff
  -n, --dry-run      走完编译和比对, 但不落盘, 只报告会改哪些
  -l, --list         列出 源 → 图 映射、体积与新鲜度
  -w, --watch [秒]   轮询监听(默认 2 秒), 源文件一改就自动重编译; Ctrl-C 退出
  -t, --type FMT     输出格式 png(默认) 或 svg
  -v, --verbose      逐文件打印状态(包括"已一致")
  -q, --quiet        只输出错误
  -h, --help         显示本帮助

路径:
  不给路径 = 处理 docs 下所有 */plantUML/*.puml。
  可给单个 .puml 文件或目录(目录会递归找 .puml)。

环境变量:
  PLANTUML=<可执行文件>   指定 plantuml 命令(默认依次找 PATH、~/.local/bin)

示例:
  ./render-plantuml.sh                                   # 增量编译全部
  ./render-plantuml.sh 1-architecture                    # 只编译某个分类
  ./render-plantuml.sh -l 6-vfs-device                   # 只看某分类的新鲜度
  ./render-plantuml.sh -f 1-architecture/plantUML/00-architecture-01.puml
  ./render-plantuml.sh -w -t svg                         # 挂监听, 出 svg
EOF
}

# ---------------------------------------------------------------- 环境
find_plantuml() {
    if [ -n "${PLANTUML:-}" ]; then
        command -v "$PLANTUML" >/dev/null 2>&1 || [ -x "$PLANTUML" ] \
            || die "PLANTUML 指向的不是可执行文件: $PLANTUML"
        printf '%s' "$PLANTUML"; return
    fi
    if command -v plantuml >/dev/null 2>&1; then command -v plantuml; return; fi
    if [ -x "$HOME/.local/bin/plantuml" ]; then printf '%s' "$HOME/.local/bin/plantuml"; return; fi
    die "找不到 plantuml 命令。请先安装(见 ~/.local/share/plantuml/README.md),
或显式指定: PLANTUML=/path/to/plantuml $0"
}

plantuml_version() {
    "$PLANTUML_BIN" -version 2>&1 | sed -n '1s/.*version \([^ ]*\).*/\1/p'
}

dot_status() {
    if command -v dot >/dev/null 2>&1; then
        printf '系统 Graphviz %s' "$(dot -V 2>&1 | sed 's/^dot - graphviz version //')"
    elif [ -x "$HOME/.local/share/plantuml/graphviz/usr/bin/dot" ]; then
        printf '随附 Graphviz(由 plantuml 包装脚本挂载)'
    else
        printf '缺失! 类图会退回内置布局, 出图可能与已提交的不一致'
    fi
}

banner() {
    info "PlantUML $(plantuml_version)   Graphviz: $(dot_status)"
}

# ---------------------------------------------------------------- 路径
pics_dir_for() {
    local d; d="$(cd "$(dirname "$1")" && pwd)"
    if [ "$(basename "$d")" = "plantUML" ]; then
        printf '%s/pics' "$(dirname "$d")"
    else
        printf '%s/pics' "$d"
    fi
}

out_path_for() {
    local base; base="$(basename "$1")"; base="${base%.*}"
    printf '%s/%s.%s' "$(pics_dir_for "$1")" "$base" "$FORMAT"
}

# ---------------------------------------------------------------- 收集
collect_sources() {
    local t f
    SOURCES=()
    if [ "${#TARGETS[@]}" -eq 0 ]; then
        while IFS= read -r -d '' f; do SOURCES+=("$f"); done < <(
            find "$DOCS_DIR" -type f \( -name '*.puml' -o -name '*.plantuml' \) \
                 -path '*/plantUML/*' -print0 | sort -z)
        return
    fi
    for t in "${TARGETS[@]}"; do
        if [ -d "$t" ]; then
            t="$(cd "$t" && pwd)"
            while IFS= read -r -d '' f; do SOURCES+=("$f"); done < <(
                find "$t" -type f \( -name '*.puml' -o -name '*.plantuml' \) -print0 | sort -z)
        elif [ -f "$t" ]; then
            SOURCES+=("$(cd "$(dirname "$t")" && pwd)/$(basename "$t")")
        else
            err "找不到路径: $t"
        fi
    done
}

# ---------------------------------------------------------------- 渲染/比对
# 渲染到临时目录(绝不直接写 pics/); 成功 0, 失败 1
render_to() {
    "$PLANTUML_BIN" -charset UTF-8 "-t$FORMAT" -o "$2" "$1" >/dev/null 2>&1
}

# 抽出 PNG 内嵌的图源; PlantUML 会把 U+2013(en dash) 规范化成 ASCII '-', 这里抵消
# -metadata 输出结构: 分隔线/文件名/空行/条目...; 而 PNG 里存了两份图源
# (原始版 + 去掉注释的重排版), 所以只取"条目开头到第一个 @enduml"这一段
embedded_source() {
    "$PLANTUML_BIN" -metadata "$1" 2>/dev/null | tail -n +4 \
        | sed -n '1,/^@enduml/p' \
        | LC_ALL=C sed 's/\xe2\x80\x93/-/g'
}
normalized_file() { LC_ALL=C sed 's/\xe2\x80\x93/-/g' "$1"; }

# ---------------------------------------------------------------- 主处理
# 一个循环覆盖 render / list / verify / dry-run, 行为按 MODE 分派
process_all() {
    local src out tmpd tmpf base d
    local n_ok=0 n_same=0 n_fail=0 n_missing=0
    declare -a changed=()
    for src in "${SOURCES[@]}"; do
        out="$(out_path_for "$src")"
        base="$(basename "$src")"; base="${base%.*}"
        tmpd="$(mktemp -d)"

        if ! render_to "$src" "$tmpd"; then
            err "  编译失败 ${src#"$DOCS_DIR"/}"
            "$PLANTUML_BIN" -charset UTF-8 "-t$FORMAT" -o "$tmpd" "$src" 2>&1 \
                | sed 's/^/       /' >&2 || true
            rm -rf "$tmpd"
            n_fail=$((n_fail+1))
            continue
        fi
        tmpf="$tmpd/$base.$FORMAT"
        if [ ! -f "$tmpf" ]; then
            err "  未产出 $base.$FORMAT: ${src#"$DOCS_DIR"/}"
            rm -rf "$tmpd"
            n_fail=$((n_fail+1)); continue
        fi

        if [ -f "$out" ] && cmp -s "$tmpf" "$out" && [ "$FORCE" != 1 ]; then
            # 内容一致: 什么都不做(连 mtime 都不碰)
            n_same=$((n_same+1))
            [ "$VERBOSE" = 1 ] && info "  一致   ${out#"$DOCS_DIR"/}"
            [ "$MODE" = list ] && printf '  %-54s %s  %s\n' \
                "${out#"$DOCS_DIR"/}" "最新" "$(du -h "$out" | cut -f1)"
        else
            case "$MODE" in
                verify)
                    if [ ! -f "$out" ]; then
                        err "  缺图   ${out#"$DOCS_DIR"/}"
                        n_missing=$((n_missing+1))
                    else
                        err "  不一致 ${out#"$DOCS_DIR"/}"
                        d="$(diff <(normalized_file "$src") <(embedded_source "$out") \
                             | grep -c '^[<>]' || true)"
                        if [ "${d:-0}" -gt 0 ]; then
                            err "         已提交图片内嵌的图源与当前 .puml 有 $d 行不同:"
                            diff -u <(normalized_file "$src") <(embedded_source "$out") \
                                | sed -n '3,25p' | sed 's/^/         /' >&2 || true
                            err "         => 跑一次 ./render-plantuml.sh 重新编译"
                        else
                            err "         图源内容相同但字节不同 => 多半是 PlantUML/Graphviz 版本差异"
                            err "         本机: PlantUML $(plantuml_version), $(dot_status)"
                        fi
                    fi
                    ;;
                list)
                    printf '  %-54s %s  %s\n' "${out#"$DOCS_DIR"/}" "需重编译" \
                        "$([ -f "$out" ] && du -h "$out" | cut -f1 || echo '—')"
                    ;;
                render)
                    if [ "$DRY" = 1 ]; then
                        info "  [计划] ${out#"$DOCS_DIR"/}"
                    else
                        mkdir -p "$(dirname "$out")"
                        mv -f "$tmpf" "$out"
                        info "  已更新 ${out#"$DOCS_DIR"/}"
                    fi
                    changed+=("${out#"$DOCS_DIR"/}")
                    ;;
            esac
            n_ok=$((n_ok+1))
        fi
        rm -rf "$tmpd"
    done

    info ""
    case "$MODE" in
        verify)
            if [ "$n_fail" -eq 0 ] && [ "$n_ok" -eq 0 ]; then
                info "严格校验通过: ${#SOURCES[@]} 张图与图源逐字节一致"
            else
                err "严格校验不通过: 编译失败 $n_fail 张, 需重编译 $n_ok 张(其中缺图 $n_missing 张)"
                return 1
            fi ;;
        list)
            info "共 ${#SOURCES[@]} 张图: 最新 $n_same, 需重编译 $n_ok, 编译失败 $n_fail" ;;
        render)
            if [ "$DRY" = 1 ]; then
                info "dry-run: 会更新 $n_ok 张, 已一致 $n_same 张, 编译失败 $n_fail 张(未落盘)"
            else
                info "完成: 更新 $n_ok 张, 已一致 $n_same 张, 编译失败 $n_fail 张"
                if [ "${#changed[@]}" -gt 0 ]; then
                    info "本次更新的图片:"
                    printf '    %s\n' "${changed[@]}"
                fi
            fi
            [ "$n_fail" -eq 0 ] || return 1 ;;
    esac
}

# ---------------------------------------------------------------- 语法校验
do_check() {
    local src fail=0
    for src in "${SOURCES[@]}"; do
        if "$PLANTUML_BIN" -charset UTF-8 -checkonly "$src" >/dev/null 2>&1; then
            [ "$VERBOSE" = 1 ] && info "  OK   ${src#"$DOCS_DIR"/}"
        else
            err "  语法错误 ${src#"$DOCS_DIR"/}"
            "$PLANTUML_BIN" -charset UTF-8 -checkonly "$src" 2>&1 | sed 's/^/       /' >&2 || true
            fail=$((fail+1))
        fi
    done
    if [ "$fail" -eq 0 ]; then
        info "语法校验通过: ${#SOURCES[@]} 个图源"
    else
        err "语法校验失败: $fail / ${#SOURCES[@]}"
        return 1
    fi
}

# ---------------------------------------------------------------- 孤儿图片
# pics/ 里有、但同级 plantUML/ 里已经找不到图源的图片(源被删或改名)
check_orphans() {
    local p base found n=0
    while IFS= read -r -d '' p; do
        base="$(basename "${p%.$FORMAT}")"
        found="$(find "$(dirname "$(dirname "$p")")/plantUML" -maxdepth 1 -type f \
                 \( -name "$base.puml" -o -name "$base.plantuml" \) 2>/dev/null | head -1)"
        if [ -z "$found" ]; then
            warn "  孤儿图片(找不到对应图源): ${p#"$DOCS_DIR"/}"
            n=$((n+1))
        fi
    done < <(find "$DOCS_DIR" -type f -path '*/pics/*' -name "*.$FORMAT" -print0 2>/dev/null | sort -z)
    [ "$n" -eq 0 ] || warn "共 $n 张孤儿图片 —— 图源已删除或改名, 建议一并删掉这些图片"
    return 0
}

# ---------------------------------------------------------------- 监听
do_watch() {
    local src t out base tmpd tmpf
    declare -A seen=()
    local first=1
    info "监听中(每 ${WATCH_SEC}s 轮询)... 改图源存盘即自动重编译, Ctrl-C 退出"
    while true; do
        collect_sources
        for src in "${SOURCES[@]}"; do
            t="$(stat -c %Y "$src" 2>/dev/null || echo 0)"
            [ "${seen[$src]:-}" = "$t" ] && continue
            seen[$src]="$t"
            [ "$first" = 1 ] && continue           # 首轮只建立基线
            out="$(out_path_for "$src")"
            base="$(basename "$src")"; base="${base%.*}"
            tmpd="$(mktemp -d)"
            if render_to "$src" "$tmpd"; then
                tmpf="$tmpd/$base.$FORMAT"
                if [ -f "$tmpf" ] && { [ ! -f "$out" ] || ! cmp -s "$tmpf" "$out"; }; then
                    mkdir -p "$(dirname "$out")"
                    mv -f "$tmpf" "$out"
                    printf '[%s] 已更新 %s\n' "$(date +%H:%M:%S)" "${out#"$DOCS_DIR"/}"
                else
                    printf '[%s] 内容未变 %s\n' "$(date +%H:%M:%S)" "${src#"$DOCS_DIR"/}"
                fi
                rm -rf "$tmpd"
            else
                printf '[%s] 编译失败(保留旧图) %s\n' "$(date +%H:%M:%S)" "${src#"$DOCS_DIR"/}"
                rm -rf "$tmpd"
            fi
        done
        first=0
        sleep "$WATCH_SEC"
    done
}

# ---------------------------------------------------------------- 入口
while [ $# -gt 0 ]; do
    case "$1" in
        -f|--force)   FORCE=1 ;;
        -c|--check)   MODE=check ;;
        -V|--verify)  MODE=verify ;;
        -n|--dry-run) DRY=1 ;;
        -l|--list)    MODE=list ;;
        -v|--verbose) VERBOSE=1 ;;
        -q|--quiet)   QUIET=1 ;;
        -h|--help)    usage; exit 0 ;;
        -w|--watch)
            MODE=watch
            if [ $# -ge 2 ] && [[ "${2:-}" =~ ^[0-9]+$ ]]; then WATCH_SEC=$2; shift; fi ;;
        -t|--type)
            [ $# -ge 2 ] || die "--type 需要一个参数(png/svg)"
            FORMAT="$2"; shift
            case "$FORMAT" in png|svg) ;; *) die "不支持的格式: $FORMAT (只支持 png/svg)" ;; esac ;;
        --) shift; while [ $# -gt 0 ]; do TARGETS+=("$1"); shift; done; break ;;
        -*) die "未知选项: $1  (用 -h 看帮助)" ;;
        *)  TARGETS+=("$1") ;;
    esac
    shift
done

PLANTUML_BIN="$(find_plantuml)"
collect_sources
[ "${#SOURCES[@]}" -gt 0 ] || die "没找到任何 .puml 图源(在 $DOCS_DIR 下找 */plantUML/*.puml)"

case "$MODE" in
    render) banner; info "图源 ${#SOURCES[@]} 个"; info ""; process_all ;;
    check)  info "语法校验 ${#SOURCES[@]} 个图源"; do_check ;;
    verify) banner; info "严格校验 ${#SOURCES[@]} 张图"; info ""
            rc=0; process_all || rc=$?
            info ""; check_orphans; exit "$rc" ;;
    list)   banner; info ""; printf '  %-54s %s  %s\n' '图片(pics/)' '状态' '体积'
            process_all; info ""; check_orphans ;;
    watch)  banner; info ""; do_watch ;;
esac
