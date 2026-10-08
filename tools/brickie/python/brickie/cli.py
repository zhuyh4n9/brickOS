"""brickie 的 CLI(L5 前端)。

## 分工(contract §8 / checklist V-18c)

本模块**只做**: 参数解析(用法错 ⇒ 退出码 2)、调 ``brickie-core``/``brickie-gen``、
编排(schema 形状校验 → core → gen render → 落盘 → 呈现)、退出码汇总。
**所有判定与码都来自 core** —— 这里不写闭包/版本/分类学/特权/hash 的任何规则。

## 编排形状

每条命令都是同一条流水线:

    可选 schema 形状校验(BRV-MF-0001 / 退出码 2)
      → brickie-core <命令>(判定 + data + files)
      → 若 data.artifacts: brickie-gen render(渲染内容)
      → 落盘(幂等; --check 时只比对 ⇒ 不一致退出码 1)
      → 呈现(文本与 --json 同源)

## 命令面(29 叶子 + 2 全局开关, 两条族)

**组合期族**(23 叶子): ``new`` / ``init`` / ``gen`` / ``dep {add,rm,tree,graph,why,index,closure}`` /
``check`` / ``ver {show,bump}`` / ``iface {list,show,diff,status,publish,freeze,deprecate,
undeprecate,unfreeze,refreeze}`` / ``--version [--deps]`` / ``--json``。

**构建族**(6 叶子, ADR-0004): ``build`` / ``clean`` / ``run`` / ``size`` / ``disasm`` / ``test``。

checklist §5.4 明确**不做**的命令一条都不在这里(未交付的命令不得出现在 ``--help`` 里,
§5.5 的反向验收); ``build``/``test``/``run`` 已由 ADR-0004 交付, 故从禁名名单移除。
"""

from __future__ import annotations

import argparse
import shutil
import sys
from dataclasses import dataclass, field
from pathlib import Path

from . import PROTOCOL, __version__
from . import native, present, runner, schema, writer
from .native import NativeError

# argparse 的用法错与 BRV-D9 的退出码 2 是同一档
_EXIT_OK = 0
_EXIT_RED = 1        # 校验红(环 / 冲突 / 版本 / 分类学 / 特权 / --check 不一致)
_EXIT_USAGE = 2      # 用法或环境错(参数非法 / 形状不符 / 路径不存在 / 原生件缺失)

_EPILOG = """\
v0.1 的边界: 组合期命令(check / dep / iface / gen / new)是**声明面完备性检查器** ——
保证组合在逻辑上自洽, 不保证"编得过 / 跑得对"; 这一族**不要求** cc / cargo / nm 在场
(brickie-v0.1 §0 的边界纪律, 也是 CI 零编译依赖作业的前提)。

构建族(build / run / test / size / disasm / clean)是**后来长出来的第二族**(ADR-0004):
它按**同一个声明面**组合镜像并驱动交叉工具链 —— "要求编译器在场"是它的职责, 不是
纪律的例外条款。判定与错误码一律来自 brickie-core; 本前端只做参数解析 / 编排 /
落盘 / 执行 / 呈现。
"""


class UsageError(Exception):
    """CLI 用法错(参数域不合法 / 位置参数形态不对) —— 退出码 2。"""


@dataclass
class Result:
    """一次命令执行的完整结果(呈现与退出码的唯一来源)。"""

    command: str
    exit_code: int
    diagnostics: list[dict] = field(default_factory=list)
    data: dict = field(default_factory=dict)
    written: list[str] = field(default_factory=list)
    extra: dict = field(default_factory=dict)


# ------------------------------------------------------------------ 解析器


def _add_common(parser: argparse.ArgumentParser, *, with_defaults: bool) -> None:
    """``--json`` 与 ``--root`` 同时挂在根、命令组与各叶子命令上。

    理由: checklist §5.3 的 **X-8**(``--json`` 是全局旗标还是逐命令旗标)尚未裁定 ——
    两种写法都接受。子命令上必须用 ``SUPPRESS`` 默认值, 否则"根上给了 ``--json``、
    叶子又把默认 False 写回"会把全局旗标吃掉。
    """
    if with_defaults:
        parser.add_argument("--json", action="store_true", default=False,
                            help="机器可读输出(JSON 到 stdout; 诊断在同源信封里)")
        parser.add_argument("--root", default=".", metavar="DIR",
                            help="仓库根目录(默认当前目录); 插件树与状态目录都相对它解析")
    else:
        parser.add_argument("--json", action="store_true", default=argparse.SUPPRESS,
                            help="机器可读输出(JSON 到 stdout; 诊断在同源信封里)")
        parser.add_argument("--root", default=argparse.SUPPRESS, metavar="DIR",
                            help="仓库根目录(默认当前目录); 插件树与状态目录都相对它解析")


def _leaf(sub: argparse._SubParsersAction, name: str, help_: str, description: str | None = None):
    """加一个带公共旗标的叶子命令解析器。"""
    parser = sub.add_parser(name, help=help_, description=description or help_)
    _add_common(parser, with_defaults=False)
    return parser


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="brickie",
        description="brickie — brickOS 组合期工具(host 侧, 零编译依赖)",
        epilog=_EPILOG,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    _add_common(parser, with_defaults=True)
    parser.add_argument("--version", action="store_true", help="打印版本后退出")
    parser.add_argument("--deps", dest="version_deps", action="store_true",
                        help="与 --version 连用: 打印语言/依赖指纹(G-01)")

    sub = parser.add_subparsers(dest="command", metavar="<command>")

    # ---------------------------------------------------------- 骨架生成
    new = _leaf(sub, "new", "生成插件骨架(§8.4)",
                "生成插件骨架: plugin.toml + 实现/头文件/用例/作者说明 + 描述符生成物。")
    new.add_argument("plugin_type", metavar="<plugin_type>",
                     help="app | interface | ability | platform")
    new.add_argument("name", metavar="<name>", help="插件名, 推荐 <namespace>/<short>(§8.3)")
    new.add_argument("--api", default="native", metavar="<api_type>",
                     help="native(默认) | runtime_adapter | third_party")
    new.add_argument("--lang", default="c", metavar="<lang>", help="c(v0.1 只交付 c)")
    new.add_argument("--subkind", default=None, metavar="<subkind>",
                     help="ability 细分; 裸名必须显式给")
    new.add_argument("--force", action="store_true", help="只对生成物目录生效(§8.4)")

    init = _leaf(sub, "init", "生成产品骨架(§8.4)",
                 "生成产品骨架: product.toml + app/<name>/ + brickie.lock 初版。")
    init.add_argument("product", metavar="<product>", help="产品名")
    init.add_argument("--app", default=None, metavar="<app_name>",
                      help="app 名(缺省 = 产品名)")

    gen = _leaf(sub, "gen", "重建生成物(--check 只比对)",
                "重建生成物(描述符 + 头文件); --check 逐字节比对不写盘。")
    gen.add_argument("--check", action="store_true",
                     help="只比对不写盘; 与重算不一致 ⇒ 退出码 1")

    # ---------------------------------------------------------- build 族(ADR-0004)
    # 边界纪律的一处**显式偏离**: v0.1 的"任何命令不得要求 cc/cargo/nm 在场"指的是
    # **组合期**命令(见 contract §9 的 R-13); build 族就是"要求交叉编译器在场"的那一族。
    build = _leaf(sub, "build", "构建镜像(声明面驱动; 替代 make 的组合段)",
                  "构建 aarch64 镜像: 组合期 check → 生成物 → 编译/链接 → 构建后门禁。")
    build.add_argument("--profile", default=None, metavar="<profile>",
                       help="dev | release(缺省 = product.toml 的 [product].stage)")
    build.add_argument("--jobs", "-j", type=int, default=0, metavar="N",
                       help="并发步骤数(0 = CPU 数)")
    build.add_argument("--backend", default="direct", metavar="<backend>",
                       help="direct(默认, brickie 自己编排) | make | ninja(BR-D4 的 A/B)")
    build.add_argument("--tag", default=None, metavar="<tag>",
                       help="产物后缀(后端对比门禁用: build/brick-<tag>.elf)")
    build.add_argument("--dry-run", action="store_true", help="只打印计划, 不执行")
    build.add_argument("--force", action="store_true", help="忽略增量状态, 全量重建")
    build.add_argument("--emit-backends", action="store_true",
                       help="只产出 build/gen/{build.mk,build.ninja}, 不执行")
    build.add_argument("--no-post", action="store_true",
                       help="跳过 tests/gates.toml 的 [build].post 门禁")

    clean = _leaf(sub, "clean", "清掉镜像产物(不动 build/host 的工具)")
    clean.add_argument("--all", action="store_true",
                       help="连 build/gen 与 build/index 一起清")

    run = _leaf(sub, "run", "在 QEMU 上跑镜像(Ctrl-A X 退出)")
    run.add_argument("--no-build", action="store_true", help="不先构建, 直接跑现有镜像")
    run.add_argument("--timeout-s", type=int, default=0, metavar="N",
                     help="限时跑(0 = 不限时, 交互式)")

    size = _leaf(sub, "size", "段体积(size -A -x)")
    size.add_argument("--no-build", action="store_true")

    disasm = _leaf(sub, "disasm", "反汇编前 N 行")
    disasm.add_argument("--head", type=int, default=120, metavar="N")
    disasm.add_argument("--no-build", action="store_true")

    test = _leaf(sub, "test", "跑门禁(宿主用例 + QEMU 日志判据 + 脚本门禁)",
                 "跑 tests/gates.toml 声明的门禁; 不给名字 = 全跑。")
    test.add_argument("name", nargs="?", default=None, metavar="[name]")
    test.add_argument("--list", action="store_true", help="列出全部门禁名")
    test.add_argument("--jobs", "-j", type=int, default=0, metavar="N")
    test.add_argument("--no-build", action="store_true", help="不先构建, 直接跑门禁")

    # ---------------------------------------------------------- dep 族
    dep = sub.add_parser("dep", help="依赖管理与分析(§7.7)",
                         description="依赖管理与分析: 片段 / 树 / 图 / 路径 / 索引 / 闭包。")
    _add_common(dep, with_defaults=False)
    dep_sub = dep.add_subparsers(dest="group_cmd", metavar="<subcommand>")

    dep_add = _leaf(dep_sub, "add", "打印 [[dep]] 片段(不写盘 —— 裁定 R-2)")
    dep_add.add_argument("plugin", metavar="<plugin>", help="目标插件名")
    dep_add.add_argument("dep", metavar="<dep>", help="依赖插件名, 可带 @<range> 后缀")
    dep_add.add_argument("--kind", default=None, metavar="<kind>",
                         help="init | runtime | type(缺省由 core 定)")
    dep_add.add_argument("--phase", default=None, metavar="<phase>",
                         help="early | core | late | app(仅 init 边的相位断言)")

    dep_rm = _leaf(dep_sub, "rm", "打印待删除块定位(不写盘 —— 裁定 R-2)")
    dep_rm.add_argument("plugin", metavar="<plugin>", help="目标插件名")
    dep_rm.add_argument("dep", metavar="<dep>", help="依赖插件名")
    dep_rm.add_argument("--kind", default=None, metavar="<kind>", help="init | runtime | type")

    dep_tree = _leaf(dep_sub, "tree", "依赖树(§7.7)")
    dep_tree.add_argument("--kind", default=None, metavar="<kind>", help="init | runtime | type | all")

    dep_graph = _leaf(dep_sub, "graph", "依赖图导出(§7.7)")
    dep_graph.add_argument("--format", default="dot", metavar="<format>",
                           help="dot(默认) | mermaid | json")

    dep_why = _leaf(dep_sub, "why", "最短依赖路径解释(§7.7)")
    dep_why.add_argument("a", metavar="<a>", help="起点插件")
    dep_why.add_argument("b", metavar="<b>", help="终点插件")

    _leaf(dep_sub, "index", "重建反向依赖索引(§6.3)")

    _leaf(dep_sub, "closure", "产品闭包 + 拓扑序 + 预算合计(§7.6)")

    # ---------------------------------------------------------- check
    check = _leaf(sub, "check", "组合期校验(§7.7)",
                  "组合期校验: 依赖闭包 / 接口 / 分类学 / 特权与预算, 含 profile 门禁(§7.5)。")
    check.add_argument("--deps", dest="scope_deps", action="store_true", help="只查依赖域")
    check.add_argument("--iface", dest="scope_iface", action="store_true", help="只查接口域")
    check.add_argument("--tax", dest="scope_tax", action="store_true", help="只查分类学域")
    check.add_argument("--priv", dest="scope_priv", action="store_true", help="只查特权/资源域")
    check.add_argument("--all", dest="scope_all", action="store_true", help="全部四域(缺省)")
    check.add_argument("--profile", default="dev", metavar="<profile>", help="dev(默认) | release")

    # ---------------------------------------------------------- ver 族
    ver = sub.add_parser("ver", help="版本管理(§5 / §7.7)",
                         description="版本管理: 展示四段版本与 COMPAT_GEN 来源 / 人工声明推进。")
    _add_common(ver, with_defaults=False)
    ver_sub = ver.add_subparsers(dest="group_cmd", metavar="<subcommand>")

    ver_show = _leaf(ver_sub, "show", "展示四段版本 + COMPAT_GEN 来源(§7.7)")
    ver_show.add_argument("id", metavar="<id>", help="插件名或 <provider>#<unit>")

    ver_bump = _leaf(ver_sub, "bump", "人工声明 MAJOR/MINOR/REVISE(§5.2)")
    ver_bump.add_argument("id", metavar="<id>",
                          help="插件名(恰一个 export)或 <provider>#<unit>")
    ver_bump.add_argument("--rule", required=True, metavar="<rule>",
                          help="major | minor | revise(COMPAT_GEN 不在其中)")

    # ---------------------------------------------------------- iface 族
    iface = sub.add_parser("iface", help="接口发布与冻结状态(§6.5)",
                           description="接口单元: 查看 / 变更集 / 一致性 / 发布 / 冻结与解冻窗口。")
    _add_common(iface, with_defaults=False)
    iface_sub = iface.add_subparsers(dest="group_cmd", metavar="<subcommand>")

    _leaf(iface_sub, "list", "列出接口单元与状态(§6.5)")

    iface_show = _leaf(iface_sub, "show", "展示接口面(条目 + 状态 + 版本 + hash; §6.5)")
    iface_show.add_argument("id", metavar="<id>", help="<provider>#<unit>")

    iface_diff = _leaf(iface_sub, "diff", "旧快照 vs 当前声明的变更集(§6.3)")
    iface_diff.add_argument("id", metavar="<id>", help="<provider>#<unit>")
    iface_diff.add_argument("--strict-params", action="store_true",
                            help="IFACE-IR 规则 3: 形参名也进 hash(默认只留类型与顺序)")

    iface_status = _leaf(iface_sub, "status", "独立重算 hash 与快照比对(§6.5)")
    iface_status.add_argument("id", metavar="<id>", help="<provider>#<unit>")
    iface_status.add_argument("--check", action="store_true", help="只比对不写盘")
    iface_status.add_argument("--strict-params", action="store_true",
                              help="独立重算时按严格形参名(§6.2 规则 3)")

    iface_publish = _leaf(iface_sub, "publish", "发布接口面(§6.5 / §5.4)")
    iface_publish.add_argument("id", metavar="<id>", help="<provider>#<unit>")
    iface_publish.add_argument("--check", action="store_true", help="只比对不写盘")
    iface_publish.add_argument("--note", default=None, metavar="<path>", help="决策记录路径")
    iface_publish.add_argument("--set", default=None, metavar="<ver>",
                               help="覆盖本次写入的单元版本(4 段)")
    iface_publish.add_argument("--profile", default=None, metavar="<profile>",
                               help="dev | release(release 跑发布前门钩)")
    iface_publish.add_argument("--strict-params", action="store_true",
                               help="IFACE-IR 规则 3: 形参名也进 hash(存进快照)")

    for name, summary in (
        ("freeze", "冻结承诺: 输出待升格提案(§6.4 A-26 例外)"),
        ("deprecate", "弃用单元或条目(§6.5)"),
        ("undeprecate", "取消弃用(§6.5)"),
    ):
        leaf = _leaf(iface_sub, name, summary)
        leaf.add_argument("id_or_entry", metavar="<id>[#entry]",
                          help="<provider>#<unit> 或 <provider>#<unit>#<entry>")
        leaf.add_argument("--note", required=True, metavar="<path>", help="决策记录路径(必填)")

    iface_unfreeze = _leaf(iface_sub, "unfreeze", "进入解冻窗口(§5.3.4)")
    iface_unfreeze.add_argument("id", metavar="<id>", help="<provider>#<unit>")
    iface_unfreeze.add_argument("--note", required=True, metavar="<path>", help="决策记录路径(必填)")

    iface_refreeze = _leaf(iface_sub, "refreeze", "退出解冻窗口(§5.3.4)")
    iface_refreeze.add_argument("id", metavar="<id>", help="<provider>#<unit>")
    iface_refreeze.add_argument("--note", default=None, metavar="<path>", help="决策记录路径(可选)")

    return parser


# ------------------------------------------------------------------ 通用编排


def _enum(value: str, allowed: tuple[str, ...], flag: str) -> str:
    """旗标取值域校验(CLI 自身的旗标域, 不是业务判定) —— 不合法 ⇒ 用法错 2。"""
    if value not in allowed:
        raise UsageError(f"{flag} 只能是 {' | '.join(allowed)}(实际 {value!r})")
    return value


def _split_entry(raw: str) -> tuple[str, str | None]:
    """``<provider>#<unit>[#<entry>]`` → ``(id, entry|None)``(§6.5 的 ``<id>[#entry]``)。"""
    parts = raw.split("#")
    if len(parts) == 2 and all(parts):
        return f"{parts[0]}#{parts[1]}", None
    if len(parts) == 3 and all(parts):
        return f"{parts[0]}#{parts[1]}", parts[2]
    raise UsageError(f"`{raw}` 不是 <provider>#<unit>[#<entry>] 形态")


def _command_path(ns: argparse.Namespace) -> str:
    group = getattr(ns, "group_cmd", None)
    if ns.command in ("dep", "ver", "iface") and group:
        return f"{ns.command} {group}"
    return str(ns.command)


def _root_of(ns: argparse.Namespace) -> Path:
    root = Path(str(ns.root)).expanduser()
    if not root.is_dir():
        raise UsageError(f"--root 指向的目录不存在或不是目录: {root}")
    return root.resolve()


def _call_core(command: str, root: Path, args: dict, context: dict | None = None) -> dict:
    request = native.make_request(
        command, root, args, context, templates=native.templates_root()
    )
    return native.call_core(request)


def _shape_gate(root: Path, kinds: tuple[str, ...]) -> list[dict]:
    """可选的单条记录形状校验(BRV-D2 / BRV-MF-0001)。"""
    if not kinds:
        return []
    if not schema.any_schema_available():
        return [schema.missing_schema_diagnostic()]
    diagnostics: list[dict] = []
    for rel, kind in writer.iter_declaration_files(root, kinds):
        diagnostics.extend(schema.check_declaration(kind, rel, writer.read_text(root / rel)))
    return diagnostics


def _has_error(diagnostics: list[dict]) -> bool:
    return any(d.get("severity") == "error" for d in diagnostics)


def _combine(*codes: int) -> int:
    """退出码汇总: 2(用法/环境) > 1(校验红) > 0(BRV-D9)。"""
    return max(codes) if codes else _EXIT_OK


def _plan_artifacts(response: dict) -> list[dict]:
    data = response.get("data") or {}
    artifacts = data.get("artifacts")
    if not isinstance(artifacts, list):
        return []
    return [a for a in artifacts if isinstance(a, dict) and isinstance(a.get("path"), str)]


def _kinds_of(artifacts: list[dict]) -> dict[str, str]:
    """计划里的 ``kind``(human/generated)按路径索引 —— 落盘层要靠它保护人写文件。"""
    return {
        a["path"]: str(a.get("kind") or "rendered")
        for a in artifacts
        if isinstance(a.get("path"), str)
    }


def _render(root: Path, artifacts: list[dict]) -> tuple[list[dict], list[dict], int]:
    """调 ``brickie-gen render``(§6)把计划渲染成文件内容。"""
    if not artifacts:
        return [], [], _EXIT_OK
    response = native.render(artifacts, root=root)
    return response["files"], response["diagnostics"], response["exit_code"]


def _human_skip_diagnostics(skipped: list[str]) -> list[dict]:
    return [
        {
            "code": None,
            "severity": "warning",
            "target": "",
            "file": rel,
            "span": "",
            "message": "已存在的人写文件未覆盖(contract §4: 永不改写人写文件)",
            "hint": "需要改动请手工编辑这个文件",
        }
        for rel in skipped
    ]


def _check_mismatch_diagnostic(path: str, why: str) -> dict:
    return {
        "code": None,
        "severity": "error",
        "target": "",
        "file": path,
        "span": "",
        "message": f"--check 不一致: {path} {why}",
        "hint": "重跑 `brickie gen` 落盘; --check 不写盘",
    }


#: ``plan-init`` 等"机器文件"的正文直传通道: contract §5.2 只给计划(`template` 为空),
#: 正文由 core 放在 ``data.<键>`` 里(见 rust/README 的"与 contract 的偏差"表)。
#: L5 只把它落到计划里的路径上 —— 内容仍是 core 产出的(§8 禁止 L5 拼接机器文件内容)。
_CORE_CONTENT_KEYS = {"brickie.lock": "lock_content"}


def _collect_files(
    root: Path, data: dict, artifacts: list[dict]
) -> tuple[list[dict], list[dict], int, dict[str, str]]:
    """把 core 的计划变成待落盘的 ``files``。

    * 有非空 ``template`` 的产物 → 交 ``brickie-gen render``(它只渲染, 不判定);
    * 没有 ``template`` 的产物(**机器文件**)→ 只接受 core 直传的正文
      (``artifact.content`` 或 ``data.<_CORE_CONTENT_KEYS>``);
    * 两者都没有 ⇒ 一条 info 诊断(既不静默吞掉, 也不把 L5 变成内容生产者)。
    """
    kinds = _kinds_of(artifacts)
    files: list[dict] = []
    diagnostics: list[dict] = []
    exit_code = _EXIT_OK

    templated = [a for a in artifacts if str(a.get("template") or "").strip()]
    if templated:
        rendered, gen_diagnostics, gen_exit = _render(root, templated)
        files += rendered
        diagnostics += gen_diagnostics
        exit_code = _combine(exit_code, gen_exit)

    for artifact in artifacts:
        if str(artifact.get("template") or "").strip():
            continue
        rel = artifact["path"]
        content = artifact.get("content")
        if not isinstance(content, str):
            content = data.get(_CORE_CONTENT_KEYS.get(rel, ""))
        if isinstance(content, str):
            entry = {"path": rel, "kind": artifact.get("kind") or "machine", "content": content}
            if isinstance(artifact.get("mode"), str):
                entry["mode"] = artifact["mode"]
            files.append(entry)
        else:
            diagnostics.append({
                "code": None,
                "severity": "info",
                "target": "",
                "file": rel,
                "span": "",
                "message": "core 的计划里这个产物既无 template 也无直传正文, 已跳过落盘",
                "hint": "机器文件的正文必须由 core 给(contract §4); 见 rust/README 的偏差表",
            })
    return files, diagnostics, exit_code, kinds


def _readonly(
    ns: argparse.Namespace,
    command: str,
    core_command: str,
    args: dict,
    *,
    shape: tuple[str, ...] = (),
    extra: dict | None = None,
) -> Result:
    """只读命令: 形状校验 → core → 呈现(**不落盘**, 即使 core 回了 files)。"""
    root = _root_of(ns)
    gate = _shape_gate(root, shape)
    extra = dict(extra or {})
    if _has_error(gate):
        return Result(command, _EXIT_USAGE, gate, {}, [], extra)
    response = _call_core(core_command, root, args)
    return Result(
        command,
        response["exit_code"],
        gate + response["diagnostics"],
        response["data"],
        [],
        extra,
    )


def _writing(
    ns: argparse.Namespace,
    command: str,
    core_command: str,
    args: dict,
    *,
    shape: tuple[str, ...] = (),
    write: bool = True,
    extra: dict | None = None,
) -> Result:
    """写入命令: 形状校验 → core → 落盘(幂等; ``write=False`` 时只呈现)。"""
    root = _root_of(ns)
    gate = _shape_gate(root, shape)
    extra = dict(extra or {})
    if _has_error(gate):
        return Result(command, _EXIT_USAGE, gate, {}, [], extra)
    response = _call_core(core_command, root, args)
    diagnostics = gate + response["diagnostics"]
    exit_code = response["exit_code"]
    written: list[str] = []
    if exit_code == _EXIT_OK and write and response["files"]:
        outcome = writer.write_files(root, response["files"])
        written = outcome.written
        diagnostics += _human_skip_diagnostics(outcome.skipped_human)
        extra["unchanged"] = outcome.unchanged
        extra["skipped"] = outcome.skipped_human
    extra["written"] = written
    return Result(command, exit_code, diagnostics, response["data"], written, extra)


def _finish_plan(command: str, root: Path, response: dict, extra: dict) -> Result:
    """``new`` / ``init`` 的收尾: core 的计划数据 + gen 渲染 + 落盘。"""
    exit_code = response["exit_code"]
    data = response["data"]
    diagnostics = list(response["diagnostics"])
    artifacts = _plan_artifacts(response)
    written: list[str] = []
    if exit_code == _EXIT_OK and artifacts:
        files, gen_diagnostics, gen_exit, kinds = _collect_files(root, data, artifacts)
        diagnostics += gen_diagnostics
        exit_code = _combine(exit_code, gen_exit)
        if files and exit_code == _EXIT_OK:
            outcome = writer.write_files(root, files, kinds=kinds)
            written = outcome.written
            diagnostics += _human_skip_diagnostics(outcome.skipped_human)
            extra.update(unchanged=outcome.unchanged, skipped=outcome.skipped_human)
    extra["written"] = written
    return Result(command, exit_code, diagnostics, data, written, extra)


# ------------------------------------------------------------------ 命令实现


def _cmd_new(ns: argparse.Namespace) -> Result:
    root = _root_of(ns)
    args = {
        "plugin_type": ns.plugin_type,
        "name": ns.name,
        "api_type": ns.api,
        "lang": ns.lang,
        "subkind": ns.subkind,
        "force": bool(ns.force),
    }
    extra = {"args": args}

    # 两段式(§4): 先探"会写哪些路径"(规则在 core), 据此扫已存在文件, 再正式请求。
    probe = _call_core("plan-new", root, args)
    artifacts = _plan_artifacts(probe)
    paths = [a["path"] for a in artifacts]
    if probe["exit_code"] != _EXIT_OK or not paths:
        return Result("new", probe["exit_code"], probe["diagnostics"], probe["data"], [], extra)

    existing = writer.scan_existing(root, paths)
    response = _call_core("plan-new", root, args, {"existing": existing})
    return _finish_plan("new", root, response, extra)


def _cmd_init(ns: argparse.Namespace) -> Result:
    root = _root_of(ns)
    args: dict = {"name": ns.product}
    if ns.app:
        args["app"] = ns.app
    extra = {"args": args}

    probe = _call_core("plan-init", root, args)
    artifacts = _plan_artifacts(probe)
    paths = [a["path"] for a in artifacts]
    if probe["exit_code"] != _EXIT_OK or not paths:
        return Result("init", probe["exit_code"], probe["diagnostics"], probe["data"], [], extra)

    existing = writer.scan_existing(root, paths)
    response = _call_core("plan-init", root, args, {"existing": existing})
    return _finish_plan("init", root, response, extra)


def _cmd_gen(ns: argparse.Namespace) -> Result:
    root = _root_of(ns)
    check = bool(ns.check)
    gate = _shape_gate(root, ("plugin", "product"))
    extra = {"check": check}
    if _has_error(gate):
        return Result("gen", _EXIT_USAGE, gate, {}, [], extra)

    response = _call_core("gen-plan", root, {"check": check})
    diagnostics = gate + response["diagnostics"]
    exit_code = response["exit_code"]
    artifacts = _plan_artifacts(response)
    written: list[str] = []

    if exit_code == _EXIT_OK and artifacts:
        files, gen_diagnostics, gen_exit, kinds = _collect_files(root, response["data"], artifacts)
        diagnostics += gen_diagnostics
        exit_code = _combine(exit_code, gen_exit)
        if files and exit_code == _EXIT_OK:
            outcome = writer.write_files(root, files, kinds=kinds, check=check)
            if check:
                extra["stale"] = outcome.stale
                extra["missing"] = outcome.missing
                for path in outcome.stale:
                    diagnostics.append(_check_mismatch_diagnostic(path, "内容与重算不同"))
                for path in outcome.missing:
                    diagnostics.append(_check_mismatch_diagnostic(path, "不存在"))
                if outcome.stale or outcome.missing:
                    exit_code = _combine(exit_code, _EXIT_RED)
            else:
                written = outcome.written
                diagnostics += _human_skip_diagnostics(outcome.skipped_human)
            extra["unchanged"] = outcome.unchanged
            extra["skipped"] = outcome.skipped_human

    extra["written"] = written
    return Result("gen", exit_code, diagnostics, response["data"], written, extra)



# ------------------------------------------------------------------ build 族(ADR-0004)


def _out(ns: argparse.Namespace):
    """人读进度的落点: ``--json`` 时一律走 stderr(否则 stdout 不是合法 JSON)。"""
    return sys.stderr if getattr(ns, "json", False) else sys.stdout


def _note(message: str, hint: str = "", *, severity: str = "error",
          code: str | None = None) -> dict:
    """L5 侧的编排级诊断(不是业务判定): 步骤失败 / 环境错。"""
    return {
        "code": code, "severity": severity, "target": "", "file": "", "span": "",
        "message": message, "hint": hint,
    }


def _judge_callback(root: Path):
    """把 core 的 ``judge`` 命令包成执行器要的回调(判据仍在 core)。"""
    def judge(name: str, text: str) -> dict:
        response = native.call_core(
            native.make_request("judge", root, {"name": name}, {"log": text})
        )
        return {
            "failed": (response.get("data") or {}).get("failed", 0),
            "diagnostics": response.get("diagnostics") or [],
            "exit_code": response.get("exit_code", 0),
        }
    return judge


def _compose(root: Path, profile: str | None) -> tuple[list[dict], int, dict]:
    """组合期三段: ``check`` → ``gen-plan`` → 渲染并落盘生成物。

    这是 ``brickie build`` 的**前半段**(设计 ADR-0003 的 S3 判据: "``brickie build``
    覆盖 check + 生成 + 编译")。退出码非 0 ⇒ 调用方必须停下 —— 声明面不自洽时编出来的
    镜像没有意义。
    """
    diagnostics: list[dict] = []
    check_args: dict = {}
    if profile:
        check_args["profile"] = profile
    check = _call_core("check", root, check_args)
    diagnostics += check["diagnostics"]
    if check["exit_code"] != _EXIT_OK:
        return diagnostics, check["exit_code"], check["data"]

    plan = _call_core("gen-plan", root, {})
    diagnostics += plan["diagnostics"]
    if plan["exit_code"] != _EXIT_OK:
        return diagnostics, plan["exit_code"], plan["data"]
    artifacts = _plan_artifacts(plan)
    if artifacts:
        files, gen_diagnostics, gen_exit, kinds = _collect_files(root, plan["data"], artifacts)
        diagnostics += gen_diagnostics
        if gen_exit != _EXIT_OK:
            return diagnostics, gen_exit, plan["data"]
        outcome = writer.write_files(root, files, kinds=kinds)
        diagnostics += _human_skip_diagnostics(outcome.skipped_human)
    return diagnostics, _EXIT_OK, check["data"]


def _persist(root: Path, files: list[dict]) -> tuple[list[str], list[dict]]:
    """落 core 给的机器文件(后端文件 / 增量状态), 幂等。"""
    if not files:
        return [], []
    kinds = {f["path"]: str(f.get("kind") or "machine") for f in files if isinstance(f.get("path"), str)}
    outcome = writer.write_files(root, files, kinds=kinds)
    return outcome.written, _human_skip_diagnostics(outcome.skipped_human)


def _post_gate_names(root: Path) -> list[str]:
    """``tests/gates.toml [build].post`` —— 构建成功后自动跑的门禁名。"""
    response = _call_core("test", root, {"list": True})
    return list((response.get("data") or {}).get("build_post") or [])


def _cmd_build(ns: argparse.Namespace) -> Result:
    root = _root_of(ns)
    gate = _shape_gate(root, ("plugin", "product"))
    if _has_error(gate):
        return Result("build", _EXIT_USAGE, gate, {}, [], {"args": {"build": True}})

    profile = _enum(ns.profile, ("dev", "release"), "--profile") if ns.profile else None
    diagnostics, exit_code, check_data = _compose(root, profile)
    if exit_code != _EXIT_OK:
        return Result("build", exit_code, gate + diagnostics, check_data, [])

    args: dict = {"backend": ns.backend, "jobs": int(ns.jobs or 0)}
    if profile:
        args["profile"] = profile
    if ns.tag:
        args["tag"] = ns.tag
    if ns.dry_run:
        args["dry_run"] = True
    if ns.force:
        args["force"] = True
    if ns.emit_backends:
        args["emit_backends"] = True

    response = _call_core("build", root, args)
    diagnostics += response["diagnostics"]
    data = response["data"]
    if response["exit_code"] != _EXIT_OK:
        return Result("build", response["exit_code"], gate + diagnostics, data, [])

    written, write_diagnostics = _persist(root, response["files"])
    diagnostics += write_diagnostics

    steps = list(data.get("steps") or [])
    if ns.dry_run or ns.emit_backends:
        results, ok = runner.run_steps(root, steps, jobs=ns.jobs, dry_run=True, out=_out(ns))
    else:
        results, ok = runner.run_steps(
            root, steps, jobs=ns.jobs, judge=_judge_callback(root), out=_out(ns)
        )

    # 构建后门禁(短名单; 缺省跑 `[build].post` 声明的那些)
    post: list[str] = []
    # 带 `--tag` 的是**诊断性构建**(后端对比 / 双份产物): 门禁的 argv 里写的是默认
    # 落点(`build/obj/...`), 对带 tag 的构建不成立 —— 所以 tag 构建不跑构建后门禁。
    if ok and not ns.dry_run and not ns.emit_backends and not ns.no_post and not ns.tag:
        post = _post_gate_names(root)
        for name in post:
            gate_result = _run_test(root, name, jobs=ns.jobs, diagnostics=diagnostics, out=_out(ns))
            diagnostics = gate_result["diagnostics"]
            if gate_result["exit_code"] != _EXIT_OK:
                ok = False
                break

    exit_code = _EXIT_OK if ok else _EXIT_RED
    extra = {
        "args": {"build": True, "backend": ns.backend, "profile": profile or data.get("profile")},
        "results": [_result_summary(r) for r in results],
        "post": post,
        "written": written,
    }
    if not ok:
        diagnostics.append(_note(
            "构建失败: 有步骤未通过(见上面的步骤输出)",
            "先看第一个 FAIL 的步骤输出; 退出码 1 = 校验/构建红, 2 = 用法或环境错",
        ))
    return Result("build", exit_code, gate + diagnostics, data, written, extra)


def _result_summary(result) -> dict:
    return {
        "label": result.label,
        "kind": result.kind,
        "ok": result.ok,
        "rc": result.rc,
        "timed_out": result.timed_out,
        "judged": result.judged,
        "judge_failed": result.judge_failed,
        "log": result.log,
    }


def _run_test(root: Path, name: str | None, *, jobs: int, diagnostics: list[dict],
              out=None) -> dict:
    """跑一组(或一个)门禁; 返回 ``{diagnostics, exit_code, data, results}``。"""
    args: dict = {"jobs": int(jobs or 0)}
    if name:
        args["name"] = name
    response = _call_core("test", root, args)
    diagnostics = diagnostics + response["diagnostics"]
    data = response["data"]
    if response["exit_code"] != _EXIT_OK:
        return {"diagnostics": diagnostics, "exit_code": response["exit_code"], "data": data, "results": []}
    steps = list(data.get("steps") or [])
    if not steps:
        return {"diagnostics": diagnostics, "exit_code": _EXIT_OK, "data": data, "results": []}
    results, ok = runner.run_steps(root, steps, jobs=jobs, judge=_judge_callback(root), out=out)
    for step, result in zip(steps, results):
        diagnostics += result.diagnostics
        if not result.ok and not result.judged:
            diagnostics.append(_note(
                f"门禁步骤失败: {result.label}",
                f"退出码 {result.rc}" + ("(超时)" if result.timed_out else ""),
                code=step.get("fail_code"),
            ))
    return {
        "diagnostics": diagnostics,
        "exit_code": _EXIT_OK if ok else _EXIT_RED,
        "data": data,
        "results": results,
    }


def _cmd_test(ns: argparse.Namespace) -> Result:
    root = _root_of(ns)
    if ns.list:
        response = _call_core("test", root, {"list": True})
        return Result("test", response["exit_code"], response["diagnostics"], response["data"], [],
                      {"list": True})

    diagnostics: list[dict] = []
    gate = _shape_gate(root, ("plugin", "product"))
    if _has_error(gate):
        return Result("test", _EXIT_USAGE, gate, {}, [])

    if not ns.no_build:
        compose_diagnostics, exit_code, _ = _compose(root, None)
        diagnostics += compose_diagnostics
        if exit_code != _EXIT_OK:
            return Result("test", exit_code, diagnostics, {}, [])
        build = _call_core("build", root, {"jobs": int(ns.jobs or 0)})
        diagnostics += build["diagnostics"]
        if build["exit_code"] != _EXIT_OK:
            return Result("test", build["exit_code"], diagnostics, build["data"], [])
        written, write_diagnostics = _persist(root, build["files"])
        diagnostics += write_diagnostics
        _, built = runner.run_steps(
            root, list(build["data"].get("steps") or []), jobs=ns.jobs,
            judge=_judge_callback(root), out=_out(ns),
        )
        if not built:
            diagnostics.append(_note("门禁前置的构建未通过", "先修 `brickie build`"))
            return Result("test", _EXIT_RED, diagnostics, build["data"], [])

    outcome = _run_test(root, ns.name, jobs=ns.jobs, diagnostics=diagnostics, out=_out(ns))
    extra = {"results": [_result_summary(r) for r in outcome["results"]]}
    return Result("test", outcome["exit_code"], outcome["diagnostics"], outcome["data"], [], extra)


def _cmd_run(ns: argparse.Namespace) -> Result:
    root = _root_of(ns)
    if not ns.no_build:
        built = _build_for_tools(ns)
        if built.exit_code != _EXIT_OK:
            built.command = "run"
            return built
    args: dict = {}
    if ns.timeout_s:
        args["timeout_s"] = int(ns.timeout_s)
    response = _call_core("run", root, args)
    data = response["data"]
    steps = list(data.get("steps") or [])
    if steps and not ns.json:
        print(f"QEMU: {' '.join(steps[0]['argv'])}")
        print("      (Ctrl-A X 退出)")
    results, ok = runner.run_steps(root, steps, jobs=1, out=_out(ns))
    return Result(
        "run", _EXIT_OK if ok else _EXIT_RED, response["diagnostics"], data, [],
        {"results": [_result_summary(r) for r in results]},
    )


def _cmd_size(ns: argparse.Namespace) -> Result:
    return _tool_command(ns, "size")


def _cmd_disasm(ns: argparse.Namespace) -> Result:
    root = _root_of(ns)
    if not ns.no_build:
        built = _build_for_tools(ns)
        if built.exit_code != _EXIT_OK:
            built.command = "disasm"
            return built
    response = _call_core("disasm", root, {"head": int(ns.head or 120)})
    steps = list((response.get("data") or {}).get("steps") or [])
    results, ok = runner.run_steps(root, steps, jobs=1, out=_out(ns))
    return Result(
        "disasm", _EXIT_OK if ok else _EXIT_RED, response["diagnostics"], response["data"], [],
        {"results": [_result_summary(r) for r in results]},
    )


def _tool_command(ns: argparse.Namespace, which: str) -> Result:
    root = _root_of(ns)
    if not ns.no_build:
        built = _build_for_tools(ns)
        if built.exit_code != _EXIT_OK:
            built.command = which
            return built
    response = _call_core(which, root, {})
    steps = list((response.get("data") or {}).get("steps") or [])
    results, ok = runner.run_steps(root, steps, jobs=1, out=_out(ns))
    return Result(
        which, _EXIT_OK if ok else _EXIT_RED, response["diagnostics"], response["data"], [],
        {"results": [_result_summary(r) for r in results]},
    )


def _build_for_tools(ns: argparse.Namespace) -> Result:
    """``run`` / ``size`` / ``disasm`` 的"先构建"复用 ``build`` 的流水线。"""
    root = _root_of(ns)
    diagnostics, exit_code, _ = _compose(root, None)
    if exit_code != _EXIT_OK:
        return Result("build", exit_code, diagnostics, {}, [])
    response = _call_core("build", root, {"jobs": int(getattr(ns, "jobs", 0) or 0)})
    diagnostics += response["diagnostics"]
    if response["exit_code"] != _EXIT_OK:
        return Result("build", response["exit_code"], diagnostics, response["data"], [])
    written, write_diagnostics = _persist(root, response["files"])
    diagnostics += write_diagnostics
    results, ok = runner.run_steps(
        root, list((response.get("data") or {}).get("steps") or []),
        jobs=int(getattr(ns, "jobs", 0) or 0), judge=_judge_callback(root), out=_out(ns),
    )
    if not ok:
        diagnostics.append(_note("构建未通过: run/size/disasm 被挡住", "先修 `brickie build`"))
    return Result(
        "build", _EXIT_OK if ok else _EXIT_RED, diagnostics, response["data"], written,
        {"args": {"build": True, "quiet": True}},
    )


def _cmd_clean(ns: argparse.Namespace) -> Result:
    root = _root_of(ns)
    response = _call_core("clean", root, {"all": bool(ns.all)})
    diagnostics = list(response["diagnostics"])
    if response["exit_code"] != _EXIT_OK:
        return Result("clean", response["exit_code"], diagnostics, response["data"], [])

    removed: list[str] = []
    for rel in (response.get("data") or {}).get("paths") or []:
        target = (root / str(rel)).resolve()
        root_resolved = root.resolve()
        # 守卫: 只删 root 之内的相对路径, 且不删 root 本身(路径来自声明面, 仍要防手抖)。
        if target == root_resolved or root_resolved not in target.parents:
            diagnostics.append(_note(
                f"拒绝删除 root 之外的路径: {rel}",
                "`clean` 只清声明面给出的、落在仓库内的派生路径",
            ))
            continue
        if target.is_dir():
            shutil.rmtree(target, ignore_errors=True)
        elif target.exists():
            target.unlink()
        else:
            continue
        removed.append(str(rel))
    return Result("clean", _EXIT_OK, diagnostics, response["data"], removed,
                  {"removed": removed})


def _cmd_check(ns: argparse.Namespace) -> Result:
    root = _root_of(ns)
    profile = _enum(ns.profile, ("dev", "release"), "--profile")
    scopes = [s for s in ("deps", "iface", "tax", "priv") if getattr(ns, f"scope_{s}")]
    if ns.scope_all or not scopes:
        scopes = ["deps", "iface", "tax", "priv"]

    extra = {"profile": profile}
    gate = _shape_gate(root, ("plugin", "product"))
    if _has_error(gate):
        return Result("check", _EXIT_USAGE, gate, {}, [], extra)

    response = _call_core("check", root, {"profile": profile, "scopes": scopes})
    return Result(
        "check",
        response["exit_code"],
        gate + response["diagnostics"],
        response["data"],
        [],
        extra,
    )


def _cmd_dep(ns: argparse.Namespace, group: str) -> Result:
    if group == "add":
        dep, _, rng = str(ns.dep).partition("@")
        args: dict = {"plugin": ns.plugin, "dep": dep}
        if rng:
            args["range"] = rng
        if ns.kind:
            args["kind"] = _enum(ns.kind, ("init", "runtime", "type"), "--kind")
        if ns.phase:
            args["phase"] = _enum(ns.phase, ("early", "core", "late", "app"), "--phase")
        return _readonly(ns, "dep add", "dep-add", args, shape=("plugin", "product"))
    if group == "rm":
        args = {"plugin": ns.plugin, "dep": ns.dep}
        if ns.kind:
            args["kind"] = _enum(ns.kind, ("init", "runtime", "type"), "--kind")
        return _readonly(ns, "dep rm", "dep-rm", args, shape=("plugin", "product"))
    if group == "tree":
        args = {}
        if ns.kind:
            args["kind"] = _enum(ns.kind, ("init", "runtime", "type", "all"), "--kind")
        return _readonly(ns, "dep tree", "dep-tree", args, shape=("plugin", "product"))
    if group == "graph":
        args = {"format": _enum(ns.format, ("dot", "mermaid", "json"), "--format")}
        return _readonly(ns, "dep graph", "dep-graph", args, shape=("plugin", "product"))
    if group == "why":
        return _readonly(
            ns, "dep why", "dep-why", {"from": ns.a, "to": ns.b},
            shape=("plugin", "product"), extra={"from": ns.a, "to": ns.b},
        )
    if group == "index":
        return _writing(ns, "dep index", "dep-index", {}, shape=("plugin", "product"))
    if group == "closure":
        return _readonly(ns, "dep closure", "closure", {}, shape=("plugin", "product"))
    raise UsageError(f"未实现的命令 `dep {group}`")


def _cmd_ver(ns: argparse.Namespace, group: str) -> Result:
    if group == "show":
        return _readonly(ns, "ver show", "ver-show", {"id": ns.id},
                         shape=("plugin",), extra={"id": ns.id})
    if group == "bump":
        rule = _enum(ns.rule, ("major", "minor", "revise"), "--rule")
        return _writing(ns, "ver bump", "ver-bump", {"id": ns.id, "rule": rule},
                        shape=("plugin",), extra={"id": ns.id})
    raise UsageError(f"未实现的命令 `ver {group}`")


def _cmd_iface(ns: argparse.Namespace, group: str) -> Result:
    if group == "list":
        return _readonly(ns, "iface list", "iface-list", {}, shape=("plugin",))
    if group == "show":
        return _readonly(ns, "iface show", "iface-show", {"id": ns.id},
                         shape=("plugin",), extra={"id": ns.id})
    if group == "diff":
        args = {"id": ns.id}
        if ns.strict_params:
            args["strict_params"] = True
        return _readonly(ns, "iface diff", "iface-diff", args,
                         shape=("plugin",), extra={"id": ns.id})
    if group == "status":
        args = {"id": ns.id, "check": bool(ns.check)}
        if ns.strict_params:
            args["strict_params"] = True
        return _writing(ns, "iface status", "iface-status", args, shape=("plugin",),
                        write=not ns.check, extra={"id": ns.id})
    if group == "publish":
        args = {"id": ns.id, "check": bool(ns.check)}
        if ns.note:
            args["note"] = ns.note
        if ns.set:
            args["set"] = ns.set
        if ns.profile:
            args["profile"] = _enum(ns.profile, ("dev", "release"), "--profile")
        if ns.strict_params:
            args["strict_params"] = True
        return _writing(ns, "iface publish", "iface-publish", args, shape=("plugin",),
                        write=not ns.check, extra={"id": ns.id})
    if group in ("freeze", "deprecate", "undeprecate"):
        unit_id, entry = _split_entry(ns.id_or_entry)
        args = {"id": unit_id, "note": ns.note}
        if entry:
            args["entry"] = entry
        return _writing(ns, f"iface {group}", f"iface-{group}", args, shape=("plugin",),
                        extra={"id": unit_id, "entry": entry})
    if group == "unfreeze":
        args = {"id": ns.id, "note": ns.note}
        return _writing(ns, "iface unfreeze", "iface-unfreeze", args, shape=("plugin",),
                        extra={"id": ns.id})
    if group == "refreeze":
        args = {"id": ns.id}
        if ns.note:
            args["note"] = ns.note
        return _writing(ns, "iface refreeze", "iface-refreeze", args, shape=("plugin",),
                        extra={"id": ns.id})
    raise UsageError(f"未实现的命令 `iface {group}`")


def _dispatch(ns: argparse.Namespace) -> Result:
    if ns.command == "new":
        return _cmd_new(ns)
    if ns.command == "init":
        return _cmd_init(ns)
    if ns.command == "gen":
        return _cmd_gen(ns)
    if ns.command == "check":
        return _cmd_check(ns)
    if ns.command == "build":
        return _cmd_build(ns)
    if ns.command == "clean":
        return _cmd_clean(ns)
    if ns.command == "run":
        return _cmd_run(ns)
    if ns.command == "size":
        return _cmd_size(ns)
    if ns.command == "disasm":
        return _cmd_disasm(ns)
    if ns.command == "test":
        return _cmd_test(ns)
    group = getattr(ns, "group_cmd", None)
    if ns.command == "dep":
        return _cmd_dep(ns, group)
    if ns.command == "ver":
        return _cmd_ver(ns, group)
    if ns.command == "iface":
        return _cmd_iface(ns, group)
    raise UsageError(f"未实现的命令 `{ns.command}`")


def _cmd_version(ns: argparse.Namespace) -> int:
    """``--version [--deps]``(G-01): 版本与依赖指纹来自 core 的 ``version`` 命令。

    core 缺席时**降级**打印 L5 自身版本(``--version`` 不该因为原生件还没编出来而
    卡住 make/CI 的自检); 但 ``--deps`` 拿不到指纹时按环境错(退出码 2)报出。
    """
    root = Path(".").resolve()
    try:
        response = _call_core("version", root, {})
    except NativeError as exc:
        data = {"version": __version__, "protocol": PROTOCOL}
        diagnostic = {
            "code": exc.code, "severity": "info", "target": "", "file": "", "span": "",
            "message": f"降级为 L5 前端版本: {exc}",
            "hint": "编出 brickie-core 后 `--version` 会带语言/依赖指纹",
        }
        fallback_exit = _EXIT_OK if not ns.version_deps else _EXIT_USAGE
        if ns.json:
            print(present.emit_json(present.json_payload(
                command="version", exit_code=fallback_exit,
                diagnostics=[diagnostic], data=data)))
        else:
            print(f"brickie {__version__}")
            if ns.version_deps:
                print(present.emit_diagnostics_text([diagnostic]), file=sys.stderr)
        return fallback_exit

    data = response["data"]
    exit_code = response["exit_code"]
    if ns.json:
        print(present.emit_json(present.json_payload(
            command="version", exit_code=exit_code,
            diagnostics=response["diagnostics"], data=data)))
        return exit_code
    if not ns.version_deps:
        print(f"brickie {data.get('version', __version__)}")
        return exit_code
    print(present.render_text("version", data, {"deps_requested": True}))
    return exit_code


# ------------------------------------------------------------------ 呈现/入口


def _error_diagnostic(message: str, code: str | None = None, severity: str = "error") -> dict:
    return {
        "code": code,
        "severity": severity,
        "target": "",
        "file": "",
        "span": "",
        "message": message,
        "hint": "",
    }


def _present(ns: argparse.Namespace, result: Result) -> None:
    result.diagnostics = present.sort_diagnostics(result.diagnostics)
    if ns.json:
        print(present.emit_json(present.json_payload(
            command=result.command,
            exit_code=result.exit_code,
            diagnostics=result.diagnostics,
            data=result.data,
            written=result.written,
        )))
        return
    text = present.emit_diagnostics_text(result.diagnostics)
    if text:
        print(text, file=sys.stderr)
    if result.data:
        result.extra["ok"] = result.exit_code == _EXIT_OK
        body = present.render_text(result.command, result.data, result.extra)
        if body:
            print(body)


def _report_failure(ns: argparse.Namespace, command: str, diagnostic: dict) -> int:
    """用法错 / 环境错(NativeError / WriteError)的统一出口: 退出码 2。"""
    if ns.json:
        print(present.emit_json(present.json_payload(
            command=command, exit_code=_EXIT_USAGE,
            diagnostics=[diagnostic], data={})))
    else:
        print(f"brickie: error: {diagnostic['message']}", file=sys.stderr)
    return _EXIT_USAGE


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    ns = parser.parse_args(argv)
    ns.json = bool(getattr(ns, "json", False))
    ns.root = getattr(ns, "root", ".") or "."

    if not getattr(ns, "version", False) and getattr(ns, "version_deps", False):
        parser.error("--deps 只能与 --version 一起使用")
    if getattr(ns, "version", False):
        return _cmd_version(ns)

    if not ns.command:
        parser.print_help()
        return _EXIT_USAGE

    try:
        result = _dispatch(ns)
    except UsageError as exc:
        return _report_failure(ns, _command_path(ns), _error_diagnostic(str(exc)))
    except NativeError as exc:
        return _report_failure(ns, _command_path(ns), _error_diagnostic(str(exc), exc.code))
    except writer.WriteError as exc:
        return _report_failure(ns, _command_path(ns), _error_diagnostic(str(exc)))
    except BrokenPipeError:  # `brickie ... | head`
        return _EXIT_OK

    _present(ns, result)
    return result.exit_code
