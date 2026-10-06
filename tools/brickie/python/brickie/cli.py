"""brickie 的 CLI(L5 前端)。

v0.1 已交付的命令面只有 ``new``(checklist §5.2 的 P0 批次);其余命令按批次排期,
**不要**在没有规格出处时"顺手加一个"—— 未交付的命令必须不出现在 ``--help`` 里
(§5.5 的反向验收)。

本模块只做: 参数解析(BRV-D9 的退出码 2 由 argparse 承担)、调原生侧、呈现结果。
所有判定与错误码都来自 ``brickie-gen`` 的响应。
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from . import PROTOCOL, __version__
from .native import NativeError, call, templates_root
from .writer import scan_existing, write_artifacts

_SEVERITY_LABEL = {"error": "error", "warning": "warning", "info": "note"}

# argparse 的用法错与 BRV-D9 的退出码 2 是同一档
_EXIT_OK = 0
_EXIT_USAGE = 2

_EPILOG = """\
v0.1 的边界: 本工具是**声明面完备性检查器** —— 保证组合在逻辑上自洽,
不保证"编得过 / 跑得对";任何命令都不会要求 cc / cargo / nm 在场(brickie-v0.1 §0)。
"""


def _add_common(parser: argparse.ArgumentParser) -> None:
    """``--json`` 与 ``--root`` 同时挂在根与各子命令上。

    理由: checklist §5.3 的 **X-8**(``--json`` 是全局旗标还是逐命令旗标)尚未裁定;
    两种写法都接受, 裁定任何一种之后都不必改用户脚本。
    """
    parser.add_argument(
        "--json",
        action="store_true",
        help="机器可读输出(JSON 到 stdout)",
    )
    parser.add_argument(
        "--root",
        default=".",
        metavar="DIR",
        help="仓库根目录(默认当前目录);插件目录与 build/gen/ 都相对它解析",
    )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="brickie",
        description="brickie — brickOS 组合期工具(host 侧, 零编译依赖)",
        epilog=_EPILOG,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    _add_common(parser)
    parser.add_argument("--version", action="store_true", help="打印版本后退出")

    sub = parser.add_subparsers(dest="command", metavar="<command>")

    new = sub.add_parser(
        "new",
        parents=[],
        help="生成插件骨架(§8.4)",
        description="生成插件骨架: plugin.toml + 实现/头文件/用例/README + 描述符生成物。",
    )
    _add_common(new)
    # 位置参数形式 = checklist §5.3 的 X-1 裁定结果: new <plugin_type> <name>
    # 取值**不用** argparse choices —— 合法性判定归原生侧, 这样报出的是
    # BRV/用法诊断而不是 argparse 的英文清单。
    new.add_argument("plugin_type", metavar="<plugin_type>", help="app | interface | ability | platform")
    new.add_argument("name", metavar="<name>", help="插件名, 推荐 <namespace>/<short>(§8.3)")
    new.add_argument("--api", default="native", metavar="<api_type>", help="native(默认) | runtime_adapter | third_party")
    new.add_argument("--lang", default="c", metavar="<lang>", help="c(v0.1 只交付 c)")
    new.add_argument("--subkind", default=None, metavar="<subkind>", help="ability 细分;裸名必须显式给")
    new.add_argument("--force", action="store_true", help="只对生成物目录生效(§8.4)")

    return parser


# ------------------------------------------------------------------ 呈现

def _emit_diagnostics_text(diagnostics: list[dict]) -> None:
    for diag in diagnostics:
        label = _SEVERITY_LABEL.get(diag.get("severity", ""), diag.get("severity", "?"))
        code = diag.get("code") or "-"
        where = diag.get("file") or diag.get("target") or ""
        head = f"{label}: {code}"
        if where:
            head += f" [{where}]"
        print(f"{head} {diag.get('message', '')}", file=sys.stderr)
        if diag.get("hint"):
            print(f"       提示: {diag['hint']}", file=sys.stderr)


def _kind_label(kind: str) -> str:
    return "人写" if kind == "human" else "生成"


def _emit_new_text(response: dict, args: dict, written: list[str]) -> None:
    by_path = {a["path"]: a for a in response.get("artifacts", [])}
    planned = response.get("planned", [])
    if not written:
        return
    print(
        f"{args['name']}  插件骨架已生成"
        f"({args['plugin_type']} / {args['api_type']} / {args['lang']})"
    )
    for item in planned:
        if item["path"] not in by_path:
            continue
        print(f"  {_kind_label(item['kind']):<4} {item['path']}")
    print()
    print("下一步: brickie check")


def _emit_json(payload: dict) -> None:
    print(json.dumps(payload, ensure_ascii=False, indent=2, sort_keys=False))


# ------------------------------------------------------------------ 命令

def _cmd_new(ns: argparse.Namespace) -> int:
    root = Path(ns.root).resolve()
    args = {
        "plugin_type": ns.plugin_type,
        "name": ns.name,
        "api_type": ns.api,
        "lang": ns.lang,
        "subkind": ns.subkind,
        "force": bool(ns.force),
    }
    templates = str(templates_root())

    # 第一步: 让原生侧给出"本次会写哪些路径"(规则在原生侧), 我们据此扫盘。
    plan = call(
        {
            "protocol": PROTOCOL,
            "command": "plan-new",
            "templates_root": templates,
            "args": args,
            "context": {},
        }
    )
    if plan.get("exit_code", _EXIT_USAGE) != _EXIT_OK:
        _report(plan, ns.json, written=[])
        return int(plan.get("exit_code", _EXIT_USAGE))

    existing = scan_existing(root, [p["path"] for p in plan.get("planned", [])])

    # 第二步: 正式生成。是否覆盖 / 是否冲突由原生侧判定。
    response = call(
        {
            "protocol": PROTOCOL,
            "command": "new",
            "templates_root": templates,
            "args": args,
            "context": {"existing": existing},
        }
    )

    written: list[str] = []
    if response.get("exit_code", _EXIT_USAGE) == _EXIT_OK:
        written = write_artifacts(root, response.get("artifacts", []))

    _report(response, ns.json, written=written, args=args)
    return int(response.get("exit_code", _EXIT_USAGE))


def _report(response: dict, as_json: bool, written: list[str], args: dict | None = None) -> None:
    if as_json:
        payload = dict(response)
        payload["written"] = written
        _emit_json(payload)
        return
    _emit_diagnostics_text(response.get("diagnostics", []))
    if written and args is not None:
        _emit_new_text(response, args, written)


# ------------------------------------------------------------------ 入口

def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    ns = parser.parse_args(argv)

    if ns.version and not ns.command:
        print(f"brickie {__version__}")
        return _EXIT_OK

    if not ns.command:
        parser.print_help()
        return _EXIT_USAGE

    try:
        if ns.command == "new":
            return _cmd_new(ns)
    except NativeError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return _EXIT_USAGE
    except BrokenPipeError:  # `brickie ... | head`
        return _EXIT_OK

    parser.error(f"未实现的命令 `{ns.command}`")  # 不会到达: 子命令表就是全集
    return _EXIT_USAGE
