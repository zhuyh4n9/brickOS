"""声明面的**单条记录形状**校验(BRV-D2) —— 自带最小 JSON-Schema 子集, 只用标准库。

## 边界(§8 的允许/禁止)

**允许**: 按 ``schema/*.schema.json`` 做单条记录形状校验, 失败 ⇒ ``BRV-MF-0001`` +
退出码 2(contract §8 / BRV-D9)。

**禁止**: 跨字段 / 跨文件不变量 —— 例如 ``export.api_iface == [plugin].api_type``
(TAX-0016)、``compat_gen`` 是否缺失(VER-0003)、版本串段数(VER-0005)、``range``
语义(VER-0006)、闭包与拓扑。那些**一律由 brickie-core 报码**, 所以本模块的 schema
里**故意不写**版本串段数/range 的 pattern(否则会把 core 的码盖成 MF-0001)。

## 支持的关键字(够用即止, 不追求完备)

``type`` / ``properties`` / ``required`` / ``items`` / ``enum`` / ``oneOf`` /
``additionalProperties`` / ``minimum`` / ``pattern``

**不引入 ``jsonschema`` 第三方依赖**(§9.3 的零依赖纪律): 这里实现的是一个**更小的
允许集**, 恰好覆盖 ``schema/`` 下的四份文件(见 python/brickie/README.md)。
"""

from __future__ import annotations

import json
import os
import re
import tomllib
from functools import lru_cache
from pathlib import Path

from .native import repo_root, tool_root

#: 声明面形状错误(RBV-D8 的 MF-0001: "必填字段缺失 / 字段形状不符 schema")
MF_CODE = "BRV-MF-0001"

#: 形状校验支持的记录类型 → schema 文件名
SCHEMA_FILES = {
    "plugin": "plugin.schema.json",   # <plugin>/plugin.toml      (contract §7.1)
    "product": "product.schema.json", # <root>/product.toml       (contract §7.2)
    "lock": "lock.schema.json",       # <root>/brickie.lock       (contract §7.3)
    "index": "index.schema.json",     # build/index/dependents.json
}

#: 记录类型 → 解析器(TOML 是唯一的人写表达格式; lock/index 是机器 JSON)
_TOML_KINDS = frozenset({"plugin", "product"})


# ------------------------------------------------------------------ schema 定位


def schema_search_roots() -> list[Path]:
    """schema 目录的查找顺序(第一个命中的目录为准)。

    1. ``$BRICKIE_SCHEMA_ROOT`` —— 用例 / 安装形态覆盖;
    2. ``<tool_root>/schema/`` —— **规范位置**: 源码树是 `tools/brickie/schema/`,
       入口 ELF 里是载荷解包根 `<work>/schema/`(`BRICKIE_TOOL_ROOT` 指向它,
       见 `freeze.py` 的 `PAYLOAD_ROOTS = ("python", "templates", "schema")`);
    3. ``python/brickie/schema/`` —— 兼容读法(早期用符号链接镜像兜底; 现为**可选**,
       目录不存在就直接跳过, 不做镜像);
    4. ``<repo_root>/tools/brickie/schema/`` —— 从工作目录之外的兜底。
    """
    roots: list[Path] = []
    override = os.environ.get("BRICKIE_SCHEMA_ROOT")
    if override:
        roots.append(Path(override).expanduser())
    roots.append(tool_root() / "schema")
    roots.append(Path(__file__).resolve().parent / "schema")
    roots.append(repo_root() / "tools" / "brickie" / "schema")
    return roots


def schema_path(name: str) -> Path | None:
    """返回某一类记录的 schema 文件路径; 四处都找不到时返回 ``None``(降级)。"""
    filename = SCHEMA_FILES.get(name)
    if filename is None:
        return None
    for root in schema_search_roots():
        candidate = root / filename
        if candidate.is_file():
            return candidate
    return None


@lru_cache(maxsize=None)
def _load_file(path: str) -> dict:
    with open(path, "rb") as handle:
        return json.loads(handle.read().decode("utf-8"))


def load_schema(name: str) -> dict | None:
    """读入并缓存一份 schema; 找不到返回 ``None``。"""
    path = schema_path(name)
    if path is None:
        return None
    return _load_file(str(path))


def any_schema_available() -> bool:
    return any(schema_path(name) is not None for name in SCHEMA_FILES)


# ------------------------------------------------------------------ 校验器


def _type_name(value: object) -> str:
    return {
        dict: "object",
        list: "array",
        str: "string",
        bool: "boolean",
        int: "integer",
        float: "number",
        type(None): "null",
    }.get(type(value), type(value).__name__)


def _type_ok(value: object, expected: str) -> bool:
    if expected == "object":
        return isinstance(value, dict)
    if expected == "array":
        return isinstance(value, list)
    if expected == "string":
        return isinstance(value, str)
    if expected == "boolean":
        return isinstance(value, bool)
    if expected == "null":
        return value is None
    if expected == "integer":
        return isinstance(value, int) and not isinstance(value, bool)
    if expected == "number":
        return isinstance(value, (int, float)) and not isinstance(value, bool)
    return True  # 未知 type 关键字: 不误杀(校验器是"更小的允许集")


def _span(pointer: str) -> str:
    """把内部指针 ``/export[0]/entries[2]/sig`` 变成 TOML 风格的 span。

    contract §3 要求 span 用 TOML 路径(如 ``export[0].entries[2].sig``)。
    """
    if not pointer:
        return ""
    text = pointer.lstrip("/").replace("/", ".")
    return text


def _walk(value: object, schema: object, pointer: str, out: list[tuple[str, str]]) -> None:
    """把 ``value`` 对 ``schema`` 的每一处不符追加到 ``out``(span, message)。"""
    if not isinstance(schema, dict):
        return

    expected = schema.get("type")
    if isinstance(expected, str) and not _type_ok(value, expected):
        out.append((_span(pointer), f"类型应为 {expected}, 实际 {_type_name(value)}"))
        return

    if "enum" in schema:
        allowed = schema["enum"]
        if isinstance(allowed, list) and value not in allowed:
            out.append((_span(pointer), f"取值必须是 {allowed} 之一, 实际 {value!r}"))

    if "oneOf" in schema and isinstance(schema["oneOf"], list):
        matches = 0
        for branch in schema["oneOf"]:
            probe: list[tuple[str, str]] = []
            _walk(value, branch, pointer, probe)
            if not probe:
                matches += 1
        if matches != 1:
            out.append((_span(pointer), f"oneOf: 应恰好命中 1 个分支, 实际命中 {matches} 个"))

    if isinstance(value, dict):
        for key in schema.get("required", []) or []:
            if key not in value:
                out.append((_span(f"{pointer}/{key}"), f"缺少必填字段 `{key}`"))
        properties = schema.get("properties", {})
        if isinstance(properties, dict):
            for key, sub in properties.items():
                if key in value:
                    _walk(value[key], sub, f"{pointer}/{key}", out)
        if schema.get("additionalProperties") is False and isinstance(properties, dict):
            for key in value:
                if key not in properties:
                    out.append(
                        (_span(f"{pointer}/{key}"), f"未知字段 `{key}`(additionalProperties=false)")
                    )
    elif isinstance(value, list):
        items = schema.get("items")
        if isinstance(items, dict):
            for index, item in enumerate(value):
                _walk(item, items, f"{pointer}[{index}]", out)
    elif isinstance(value, str) and isinstance(schema.get("pattern"), str):
        try:
            matched = re.search(schema["pattern"], value) is not None
        except re.error:
            matched = True
        if not matched:
            out.append((_span(pointer), f"不匹配 pattern {schema['pattern']!r}: {value!r}"))
    elif isinstance(value, (int, float)) and not isinstance(value, bool):
        minimum = schema.get("minimum")
        if isinstance(minimum, (int, float)) and value < minimum:
            out.append((_span(pointer), f"应 ≥ {minimum}, 实际 {value}"))


def validate(record: object, schema: dict) -> list[tuple[str, str]]:
    """校验单条记录, 返回 ``[(span, message)]``(空 = 通过)。"""
    out: list[tuple[str, str]] = []
    _walk(record, schema, "", out)
    return out


def _diag(rel: str, target: str, span: str, message: str) -> dict:
    return {
        "code": MF_CODE,
        "severity": "error",
        "target": target,
        "file": rel,
        "span": span,
        "message": message,
        "hint": "声明面形状问题(BRV-D2); 跨字段不变量由 brickie-core 报码",
    }


def diagnostics_for(record: object, schema_name: str, *, file: str, target: str = "") -> list[dict]:
    """对一条已解析的记录做形状校验, 产出 ``BRV-MF-0001`` 诊断。"""
    schema = load_schema(schema_name)
    if schema is None:
        return []
    return [_diag(file, target, span, message) for span, message in validate(record, schema)]


def _record_target(kind: str, record: object) -> str:
    """诊断的 ``target``: 插件/产品名(形状错也要能定位到人)。"""
    if not isinstance(record, dict):
        return ""
    section = record.get("plugin") if kind == "plugin" else record.get("product")
    if isinstance(section, dict):
        name = section.get("name")
        return name if isinstance(name, str) else ""
    return ""


def _parse_record(kind: str, text: str) -> object:
    """按记录类型解析。

    * ``plugin`` / ``product``: TOML(§7 的"TOML 是唯一表达格式");
    * ``index``: JSON(``build/index/dependents.json``);
    * ``lock``: ``brickie.lock`` 的格式在 v0.1 未钉死(contract §7.3 只说"见 lock.schema.json"),
      故**先试 TOML 再退 JSON** —— 不让 L5 的解析口味成为第二种真值。
    """
    if kind in _TOML_KINDS:
        return tomllib.loads(text)
    if kind == "index":
        return json.loads(text)
    if kind == "lock":
        try:
            return tomllib.loads(text)
        except tomllib.TOMLDecodeError:
            return json.loads(text)
    return json.loads(text)


def check_declaration(kind: str, rel: str, text: str) -> list[dict]:
    """解析 + 形状校验一份声明文件。

    * 解析失败(TOML/JSON 语法) ⇒ 一条 ``BRV-MF-0001``(BRV-D9: TOML 语法归退出码 2);
    * schema 文件缺失 ⇒ 空列表(**降级**, 不误杀; 编排层会补一条 info 说明)。
    """
    if schema_path(kind) is None:
        return []

    try:
        record = _parse_record(kind, text)
    except (tomllib.TOMLDecodeError, json.JSONDecodeError, UnicodeDecodeError) as exc:
        return [_diag(rel, "", "", f"解析失败({kind}): {exc}")]

    return diagnostics_for(record, kind, file=rel, target=_record_target(kind, record))


def missing_schema_diagnostic() -> dict:
    """schema 目录整体缺失时的**降级提示**(info; 不决定退出码)。"""
    searched = ", ".join(str(root) for root in schema_search_roots())
    return {
        "code": None,
        "severity": "info",
        "target": "",
        "file": "",
        "span": "",
        "message": "未找到 schema/ 目录, 已跳过单条记录形状校验(BRV-D2); "
                   f"可用 $BRICKIE_SCHEMA_ROOT 指定; 已查找: {searched}",
        "hint": "形状校验是 L5 的允许职责之一; 缺失时判定仍由 brickie-core 承担",
    }
