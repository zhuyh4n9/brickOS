#!/usr/bin/env python3
#
# brickOS prototype — gate for "no third-party source copies outside third_party/" (ADR-0026 / ADR-0029 / ADR-0033)
#
# Why it is needed:
#   Third-party discipline is "**give upstream the environment it depends on**": `third_party/` holds
#   only byte-for-byte upstream originals (sha256-locked by `check-third-party-pristine.sh`), and all
#   adaptation is written by us as ordinary plugins under the **capability** namespace
#   (`driver/uboot-mmc`, `fs/uboot-fat`, `fs/uboot-erofs`). But "adaptation" and "copy the upstream
#   source and rename it" look **exactly the same on the filesystem** — an earlier round's
#   `fs/uboot-fat/src/fat_core.c` had line-level similarity 0.58 with the upstream
#   `fs/fat/fat.c` (indentation changed / renamed, same logic).
#   This script makes that criterion mechanical: **no .c/.h outside `third_party/` may cross the
#   similarity line against any vendor original**.
#
#   Scope note (ADR-0033): the scan is **repository-wide**, not tied to a directory name. It used to
#   watch `adapter/**`; when that namespace was dissolved, a directory-keyed scan would have silently
#   stopped watching the very files it exists for — the criterion is "is this a copy of upstream",
#   which has nothing to do with where the file sits.
#
# Criteria (mechanical, item by item):
#   (1) normalize: drop blank lines, pure comment lines and `#`-prefixed preprocessor lines, then
#       strip leading/trailing whitespace — leaving only "code lines"; this way "add a comment /
#       change indentation / change the include form" counts neither as a difference nor as similarity.
#   (2) for every .c/.h **outside `third_party/`**, compute `difflib.SequenceMatcher` line-level
#       similarity against every .c/.h under `third_party/`;
#   (3) crossing the line = similarity >= threshold (default 0.30) **and** both normalized line
#       counts >= min lines (default 30);
#   (4) **named exemptions** (EXEMPT) are allowed: each one must name the file, the upstream file and
#       the reason — an exemption is "bookkeeping", not "muting"; any un-named crossing is red.
#
# Usage: python3 tools/check-vendor-copies.py [--threshold 0.30] [--min-lines 30] [--all]
# Exit code: 0 = all pass, 1 = crossing found, 2 = usage/environment error
#
# Output contract (for the gate require match): "PASS: no third-party source copies outside third_party/"

import argparse
import difflib
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Named exemptions: (our relative path, vendor relative path) -> reason.
# Empty = there are currently no exemptions.
EXEMPT = {
    # ("driver/uboot-xxx/compat/yyy.h", "third_party/uboot-imx/include/yyy.h"):
    #     "why this similarity is unavoidable",
}

# Never scanned: the vendor tree itself is the baseline, not a candidate; build/ is derived output;
# prebuilts/ is a checked-in toolchain/seed dump that contains no project source.
PRUNE = {".git", "build", "prebuilts", "third_party"}


def norm(path):
    """Normalize into a list of "code lines" (drop blank lines / pure comment lines / preprocessor lines)."""
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


def collect(root, exts=(".c", ".h"), prune=()):
    files = []
    for base, dirs, names in os.walk(root):
        dirs[:] = [d for d in dirs if d not in prune]
        for n in names:
            if n.endswith(exts):
                files.append(os.path.join(base, n))
    return sorted(files)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--threshold", type=float, default=0.30)
    ap.add_argument("--min-lines", type=int, default=30)
    ap.add_argument("--all", action="store_true", help="print the highest similarity for every scanned file")
    args = ap.parse_args()

    vendor_root = os.path.join(ROOT, "third_party")
    if not os.path.isdir(vendor_root):
        print("FAIL: cannot find third_party/", file=sys.stderr)
        return 2

    candidates = collect(ROOT, prune=PRUNE)
    vendors = collect(vendor_root)
    if not candidates or not vendors:
        print("FAIL: no .c/.h outside third_party/, or none under third_party/", file=sys.stderr)
        return 2

    vnorm = {}
    for v in vendors:
        n = norm(v)
        if n is not None:
            vnorm[v] = n

    status = 0
    worst_all = 0.0
    for a in candidates:
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
            print("FAIL: %s has line-level similarity %.2f >= %.2f with upstream original %s — "
                  "only **adaptation** (environment/glue) may be written by us; copying third-party "
                  "sources outside third_party/ is forbidden (ADR-0026 D-3 / ADR-0033)"
                  % (rel_a, best, args.threshold, rel_v), file=sys.stderr)
            status = 1

    if status == 0:
        print("PASS: no third-party source copies outside third_party/ (threshold %.2f / min lines %d / max observed %.2f)"
              % (args.threshold, args.min_lines, worst_all))
    return status


if __name__ == "__main__":
    sys.exit(main())
