#!/usr/bin/env python3
#
# brickOS prototype — "适配层不许夹带三方件源码副本"门禁(ADR-0026 / ADR-0029)
#
# 为什么需要它:
#   ADR-0026 把三方件的纪律从"目录隔离"改成"**给上游它依赖的环境**": `third_party/` 只放
#   逐字节上游原件(有 sha256 锁与 `check-third-party-pristine.sh`), 一切适配写在
#   `adapter/<插件>/{compat,glue}`。但"适配"与"把上游源码抄一份到 adapter 再改名"这两件事
#   **在文件系统上长得一模一样** —— 上一轮的 `adapter/uboot-fat/src/fat_core.c` 与上游
#   `fs/fat/fat.c` 的行级相似度就有 0.58(改了缩进/改名, 逻辑是同一份)。
#   本脚本把那条判据变成机械的: **adapter/ 下的文件与任何 vendor 原件的相似度不得越线**。
#
# 判据(逐条机械):
#   ① 归一化: 去空行、去纯注释行、去 `#` 开头的预处理行, 再去首尾空白 —— 只留下"代码行";
#      这样"加注释/换缩进/换 include 写法"不会被算成差异, 也不会被算成相似。
#   ② 对 `adapter/**` 下每个 .c/.h, 与 `third_party/**` 下每个 .c/.h 算
#      `difflib.SequenceMatcher` 行级相似度;
#   ③ 越线 = 相似度 >= 阈值(缺省 0.30) **且** 双方归一化行数都 >= 最小行数(缺省 30);
#   ④ 允许**具名豁免**(EXEMPT): 每条豁免必须写明文件、上游文件与理由 —— 豁免是"记账",
#      不是"消音"; 未具名的越线一律红。
#
# 用法: python3 tools/check-adapter-copies.py [--threshold 0.30] [--min-lines 30] [--all]
# 退出码: 0 = 全过, 1 = 有越线, 2 = 用法/环境错
#
# 输出口径(供门禁 require 匹配): "PASS: adapter 无三方件源码副本"

import argparse
import difflib
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# 具名豁免: (adapter 相对路径, vendor 相对路径) -> 理由。
# 空 = 当前没有任何豁免。
EXEMPT = {
    # ("adapter/uboot-xxx/compat/yyy.h", "third_party/uboot-imx/include/yyy.h"):
    #     "为什么这份相似是不可避免的",
}


def norm(path):
    """归一化成"代码行"列表(去空行/纯注释行/预处理行)。"""
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            text = f.read()
    except OSError:
        return None

    out = []
    in_block = False
    for raw in text.splitlines():
        s = raw.strip()
        if in_block:
            if "*/" in s:
                in_block = False
            continue
        if s.startswith("/*"):
            if "*/" not in s:
                in_block = True
            continue
        if s.startswith("*") or s.startswith("//") or s == "":
            continue
        if s.startswith("#"):
            continue
        out.append(s)
    return out


def collect(root, exts=(".c", ".h")):
    files = []
    for base, dirs, names in os.walk(root):
        dirs[:] = [d for d in dirs if d not in (".git", "build")]
        for n in names:
            if n.endswith(exts):
                files.append(os.path.join(base, n))
    return sorted(files)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--threshold", type=float, default=0.30)
    ap.add_argument("--min-lines", type=int, default=30)
    ap.add_argument("--all", action="store_true", help="打印每个 adapter 文件的最高相似度")
    args = ap.parse_args()

    adapter_root = os.path.join(ROOT, "adapter")
    vendor_root = os.path.join(ROOT, "third_party")
    if not os.path.isdir(adapter_root) or not os.path.isdir(vendor_root):
        print("FAIL: 找不到 adapter/ 或 third_party/", file=sys.stderr)
        return 2

    adapters = collect(adapter_root)
    vendors = collect(vendor_root)
    if not adapters or not vendors:
        print("FAIL: adapter/ 或 third_party/ 下没有 .c/.h", file=sys.stderr)
        return 2

    vnorm = {}
    for v in vendors:
        n = norm(v)
        if n is not None:
            vnorm[v] = n

    status = 0
    worst_all = 0.0
    for a in adapters:
        na = norm(a)
        if not na:
            continue
        rel_a = os.path.relpath(a, ROOT)
        best = 0.0
        best_v = ""
        for v, nv in vnorm.items():
            if len(na) < args.min_lines or len(nv) < args.min_lines:
                continue
            r = difflib.SequenceMatcher(None, na, nv).ratio()
            if r > best:
                best, best_v = r, v
        worst_all = max(worst_all, best)
        rel_v = os.path.relpath(best_v, ROOT) if best_v else ""
        if args.all:
            print("     %5.2f  %s%s" % (best, rel_a, ("  ~= " + rel_v) if rel_v else ""))
        if best >= args.threshold and (rel_a, rel_v) not in EXEMPT:
            print("FAIL: %s 与上游原件 %s 的行级相似度 %.2f >= %.2f —— "
                  "adapter/ 只许放**适配**(环境/对接), 不许放三方件源码的副本(ADR-0026 D-3)"
                  % (rel_a, rel_v, best, args.threshold), file=sys.stderr)
            status = 1

    if status == 0:
        print("PASS: adapter 无三方件源码副本(阈值 %.2f / 最小行数 %d / 实测最高 %.2f)"
              % (args.threshold, args.min_lines, worst_all))
    return status


if __name__ == "__main__":
    sys.exit(main())
