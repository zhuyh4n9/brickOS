#!/usr/bin/env python3
"""`--json` 输出的**结构化**查询 / 快照工具(用例的断言后端)。

为什么不是文本 grep / 全文本 diff:
  contract §3 规定诊断数组按 ``(code, file, span, target)`` 稳定排序、§2 规定信封字段;
  验收要锁的是**这些结构**(键集合 / 诊断排序 / 协议版本 / 退出码), 不是某台机器上
  恰好缩进几个空格的文本。因此本模块只做四件事:

  1. 按键路径取值(``get`` / ``count`` / ``has`` / ``keys``);
  2. 把诊断数组压成 ``code|severity|file|span|target`` 的规范元组序列(``diags``),
     供"排序是否稳定""桩替换后诊断集合是否不变"之类的断言使用;
  3. 断言诊断数组确实按契约键有序(``sorted``);
  4. 快照: 把整份响应投影成**去掉散文(message/hint)与动态回写正文**的稳定 JSON,
     与 ``tests/snap/<name>.json`` 逐字节比对(``snapshot``); ``--emit`` 用于重生成。

用法(全部以退出码 0/1 表示断言成立与否, 供 run.sh 直接 check):
  jsonq.py envelope   <resp.json>
  jsonq.py get        <resp.json> <dotted.path>
  jsonq.py keys       <resp.json> <dotted.path>
  jsonq.py count      <resp.json> <dotted.path>
  jsonq.py has        <resp.json> <dotted.path>
  jsonq.py diags      <resp.json>
  jsonq.py diagcodes  <resp.json>
  jsonq.py sorted     <resp.json>          # yes/no
  jsonq.py snapshot   <name> <resp.json>   # 与 tests/snap/<name>.json 比对
  jsonq.py snapshot   --emit <name> <resp.json>
"""

from __future__ import annotations

import json
import pathlib
import re
import sys

SNAP_DIR = pathlib.Path(__file__).resolve().parent / "snap"

# 快照里**故意丢掉**的字段:
#   message / hint   —— 人读散文, 不是协议形状; 锁它会锁住措辞而不是锁住契约
#   expected_snapshot —— `iface status` 不一致时回吐的"应有快照"正文, 属动态回写内容
_DROP_KEYS = {"message", "hint", "expected_snapshot"}
# `--version --json` 的 `data.languages[].version` 是**构建环境的工具链版本**(如 rustc 1.93.1),
# 不是协议形状; 换一台工具链就会让快照红, 属假阳性 ⇒ 从投影里去掉(语言名保留)。
_DROP_TOOLCHAIN = re.compile(r"languages(\[\d+\])?$")
_LONG_STRING = 200


def load(path: str) -> dict:
    return json.loads(pathlib.Path(path).read_text())


def walk(node: object, path: str) -> object:
    """按 ``a.b.0.c`` 取值; 路径为空 ⇒ 取根。"""
    cur = node
    if not path:
        return cur
    for part in path.split("."):
        if isinstance(cur, dict):
            if part not in cur:
                raise KeyError(path)
            cur = cur[part]
        elif isinstance(cur, list):
            cur = cur[int(part)]
        else:
            raise KeyError(path)
    return cur


def flat(value: object) -> str:
    """标量直出; 容器出紧凑 JSON(便于 `check` 比较)。"""
    if isinstance(value, bool):
        return "true" if value else "false"
    if value is None:
        return "null"
    if isinstance(value, (dict, list)):
        return json.dumps(value, ensure_ascii=False, sort_keys=True)
    return str(value)


def diag_tuples(resp: dict) -> list[list[str]]:
    """诊断的规范元组序列(**保持数组原序**, 排序断言才有意义)。"""
    out = []
    for d in resp.get("diagnostics", []):
        out.append([
            str(d.get("code") or ""),
            str(d.get("severity") or ""),
            str(d.get("file") or ""),
            str(d.get("span") or ""),
            str(d.get("target") or ""),
        ])
    return out


def sort_key(t: list[str]) -> tuple:
    # 与 python/brickie/present.sort_diagnostics 同键(contract §3);
    # 无码诊断以 "" 排最前。
    return (t[0], t[2], t[3], t[4])


def project(node: object, path: str = "") -> object:
    """稳定投影: 键序固定、丢散文、丢掉构建环境相关的工具链版本、截断超长字符串。"""
    if isinstance(node, dict):
        out = {}
        for k, v in sorted(node.items()):
            if k in _DROP_KEYS:
                continue
            if k == "version" and _DROP_TOOLCHAIN.search(path):
                continue
            child = f"{path}.{k}" if path else k
            out[k] = project(v, child)
        return out
    if isinstance(node, list):
        return [project(x, f"{path}[{i}]") for i, x in enumerate(node)]
    if isinstance(node, str) and len(node) > _LONG_STRING:
        return f"<str:{len(node)}>"
    return node


def render(value: object) -> str:
    return json.dumps(value, indent=2, ensure_ascii=False, sort_keys=True) + "\n"


def cmd_snapshot(args: list[str]) -> int:
    emit = False
    if args and args[0] == "--emit":
        emit = True
        args = args[1:]
    if len(args) != 2:
        print("用法: jsonq.py [--emit] snapshot <name> <resp.json>", file=sys.stderr)
        return 1
    name, path = args
    proj = render(project(load(path)))
    target = SNAP_DIR / f"{name}.json"
    if emit:
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(proj)
        print(f"emit {target}")
        return 0
    if not target.exists():
        print(f"snapshot 缺失: {target}(用 `jsonq.py --emit snapshot {name} <resp.json>` 生成)")
        return 1
    want = target.read_text()
    if want == proj:
        return 0
    import difflib

    diff = list(difflib.unified_diff(
        want.splitlines(), proj.splitlines(),
        fromfile=f"snap/{name}.json(基线)", tofile="本次输出", lineterm=""))
    print("\n".join(diff[:60]))
    return 1


def main(argv: list[str]) -> int:
    if not argv:
        print(__doc__, file=sys.stderr)
        return 2
    op, rest = argv[0], argv[1:]

    if op == "snapshot":
        return cmd_snapshot(rest)

    if not rest:
        print(f"{op} 需要 <resp.json>", file=sys.stderr)
        return 2
    resp = load(rest[0])

    if op == "envelope":
        print(",".join(sorted(resp.keys())))
        return 0
    if op == "text":
        # 诊断的文本投影(供用例用同一条 grep 走 text/json 两种模式)
        for d in resp.get("diagnostics", []):
            print(f'{d.get("severity", "?")}: {d.get("code") or "-"} '
                  f'[{d.get("file") or d.get("target") or ""}] ({d.get("span") or ""}) '
                  f'{d.get("message", "")} {d.get("hint", "")}')
        return 0
    if op == "diags":
        for t in diag_tuples(resp):
            print("|".join(t))
        return 0
    if op == "diagcodes":
        print(",".join(sorted({t[0] for t in diag_tuples(resp) if t[0]})))
        return 0
    if op == "sorted":
        tuples = diag_tuples(resp)
        print("yes" if tuples == sorted(tuples, key=sort_key) else "no")
        return 0
    if op == "proj":
        sys.stdout.write(render(project(resp)))
        return 0

    path = rest[1] if len(rest) > 1 else ""
    if op == "keys":
        node = walk(resp, path)
        if not isinstance(node, dict):
            print(f"{path} 不是对象", file=sys.stderr)
            return 1
        print(",".join(sorted(node.keys())))
        return 0
    if op == "count":
        print(len(walk(resp, path)))
        return 0
    if op == "has":
        try:
            walk(resp, path)
            print("yes")
        except (KeyError, IndexError, ValueError):
            print("no")
        return 0
    if op == "get":
        print(flat(walk(resp, path)))
        return 0
    print(f"未知操作: {op}", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
