#!/usr/bin/env python3
"""fetch-prebuilt — fetch the prebuilts/toolchain/ toolset locally (pinned versions + verify + unpack + stamp).

## Why this exists

`prebuilts/toolchain/` is **not committed** (it is a derived directory, GBs in size), so a **replayable**
command is needed to rebuild it. Replayable means: versions and URLs are pinned in `prebuilts/toolchain.lock.toml`,
and the download **must** be SHA256-verified — a mismatch aborts with an error, never "make do with something close".

This discipline matches the revised wording of BR-D3 on the design side `4-04`/`2-02`:
"minimal per-language dependency set + **pinned versions** + **offline reproducibility**".

## Bootstrap dependencies (deliberately minimal)

Depends only on the **Python 3 standard library**: `urllib` (download) + `hashlib` (verify) + `tarfile`/`lzma`/
`compression.zstd` (unpack) + `zipfile` + its own ar parser (.deb).
**No** curl / wget / tar / xz / zstd / ar / dpkg-deb / unzip needed.

  * `compression.zstd` entered the standard library in Python **3.14**. Older versions facing a zstd-compressed
    payload (all .deb files from new distros) fall back to calling the `zstd` or `tar --zstd` command.

## Usage

    python3 tools/fetch-prebuilt.py                 # fetch all missing pieces
    python3 tools/fetch-prebuilt.py <id> [...]      # fetch only the given pieces
    python3 tools/fetch-prebuilt.py --list          # list (no download)
    python3 tools/fetch-prebuilt.py --check         # only check what is in place (no download)
    python3 tools/fetch-prebuilt.py --verify        # recompute SHA256 of cached files
    python3 tools/fetch-prebuilt.py --force         # ignore the cache and re-download
    python3 tools/fetch-prebuilt.py --print-hash <url>   # print the sha256 of a URL (for maintaining the lock)

Exit codes: 0 success / 1 verify or unpack failure / 2 usage or environment error
"""

from __future__ import annotations

import argparse
import hashlib
import io
import os
import shutil
import subprocess
import sys
import tarfile
import time
import tomllib
import urllib.error
import urllib.request
import zipfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
PREBUILT = REPO_ROOT / "prebuilts" / "toolchain"     # derived side (not committed); see ADR-0005
CACHE = PREBUILT / ".cache"
STAMP = PREBUILT / ".stamp"
LOCK = REPO_ROOT / "prebuilts/toolchain.lock.toml"
TOOLCHAIN_MK = PREBUILT / "toolchain.mk"
# Paths written into toolchain.mk (repo-root relative), computed by the script, never hardcoded
PREBUILT_REL = PREBUILT.relative_to(REPO_ROOT).as_posix()

# The read block is **deliberately small**: on a slow link, reading 1 MB per read() means it returns only
# once 1 MB is filled ⇒ 1 MB flush granularity (invisible progress, up to 1 MB lost on a disconnect).
# 64 KB balances throughput and observability. Combined with read1(): take what is there, never block to fill.
CHUNK = 1 << 16
UA = "brickOS-fetch-prebuilts/toolchain/1 (+https://github.com/zhuyh4n9/brickOS)"


def die(msg: str, code: int = 1) -> "NoReturn":  # noqa: F821
    print(f"FAIL: {msg}", file=sys.stderr)
    raise SystemExit(code)


def info(msg: str) -> None:
    print(f"  {msg}")


def human(n: int) -> str:
    return f"{n / 1048576:.1f} MB" if n >= 1048576 else f"{n / 1024:.0f} KB"


# --------------------------------------------------------------------- lock file

def load_lock() -> dict:
    if not LOCK.is_file():
        die(f"lock file not found: {LOCK}", 2)
    with LOCK.open("rb") as fh:
        lock = tomllib.load(fh)
    if lock.get("version") != 1:
        die(f"{LOCK.name}: unrecognized version={lock.get('version')!r} (expected 1)", 2)
    arts = lock.get("artifact") or []
    if not arts:
        die(f"{LOCK.name}: no [[artifact]] entries at all", 2)
    seen: set[str] = set()
    for a in arts:
        for key in ("id", "url", "sha256", "kind", "filename", "into"):
            if not a.get(key):
                die(f"{LOCK.name}: entry missing {key}: {a!r}", 2)
        if a["id"] in seen:
            die(f"{LOCK.name}: duplicate id: {a['id']}", 2)
        seen.add(a["id"])
        if len(a["sha256"]) != 64 or any(c not in "0123456789abcdef" for c in a["sha256"]):
            die(f"{LOCK.name}: {a['id']}'s sha256 is not 64 lowercase hex digits", 2)
    return lock


# ------------------------------------------------------------------ download / verify

def sha256_of(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(CHUNK), b""):
            h.update(chunk)
    return h.hexdigest()


def download(url: str, dest: Path, expect_sha: str, *, attempts: int = 60) -> bool:
    """Resumable download with retries; returns True on success (trailing SHA256 matches).

    **Why resume must live here** (a pit we hit): an earlier version treated "the cached file fails
    verification" as a miss and deleted it to re-download — that deleted a 14 MB partial chunk which
    another path was resuming, so the two downloads destroyed each other. The correct meaning of a
    partial chunk is "**an unfinished download**", not "a bad cache". Therefore:

      * keep the partial chunk and continue from its size with an HTTP Range (if the server ignores Range, truncate and restart);
      * judge SHA256 only after the **whole file** has finished;
      * a mismatch **does not delete** it either — keep it for comparison and suggest `--force` (ADR-0002 §2-4).

    **The first fetch must create the directory** (a pit we hit): `prebuilts/toolchain/` is a **derived
    directory** and is not committed, so in a fresh checkout it **does not exist at all**; the destination is
    `<it>/.cache/<filename>` — without mkdir first, every `dest.open()` raises FileNotFoundError, which the
    except below swallows as a "network interruption", so 60 retries each sleep up to 15 s: the symptom is
    "hangs for ten minutes, then reports the download as unfinished".
    """
    dest.parent.mkdir(parents=True, exist_ok=True)

    for attempt in range(1, attempts + 1):
        if dest.is_file():
            if expect_sha and sha256_of(dest) == expect_sha:
                return True
            have = dest.stat().st_size
        else:
            have = 0

        headers = {"User-Agent": UA}
        if have:
            headers["Range"] = f"bytes={have}-"
        req = urllib.request.Request(url, headers=headers)
        mode = "ab" if have else "wb"
        try:
            with urllib.request.urlopen(req, timeout=60) as resp:
                status = getattr(resp, "status", 200)
                if have and status != 206:
                    # server ignored Range ⇒ start over, and **must truncate** (otherwise garbage gets appended)
                    print(f"    (server does not support resume, downloading from scratch)")
                    have, mode = 0, "wb"
                total = 0
                crange = resp.headers.get("Content-Range")
                clen = resp.headers.get("Content-Length")
                if crange and "/" in crange:
                    total = int(crange.rsplit("/", 1)[1])
                elif clen:
                    total = have + int(clen)
                done = have
                read = getattr(resp, "read1", resp.read)   # read1: return whatever is available
                with dest.open(mode) as out:
                    while True:
                        chunk = read(CHUNK)
                        if not chunk:
                            break
                        out.write(chunk)
                        out.flush()
                        done += len(chunk)
                        if total:
                            print(f"\r    {done * 100 // total:3d}%  "
                                  f"{human(done)} / {human(total)}", end="")
                if total:
                    print()
        except urllib.error.HTTPError as exc:
            if exc.code == 416 and have:
                # the server thinks it already sent everything ⇒ verify the existing file, do not retry forever
                if expect_sha and sha256_of(dest) == expect_sha:
                    return True
                print(f"    upstream reports range out of bounds (416), but the existing file does not verify — stopping")
                return False
            print(f"    attempt {attempt} failed (HTTP {exc.code}), retrying…")
            time.sleep(min(2 * attempt, 15))
            continue
        except (urllib.error.URLError, OSError, TimeoutError) as exc:
            got = dest.stat().st_size if dest.is_file() else 0
            print(f"\n    attempt {attempt} interrupted ({type(exc).__name__}), got {human(got)}, resuming…")
            time.sleep(min(2 * attempt, 15))
            continue

        if not expect_sha:
            return True                      # hash probe only: one completed round is success
        if sha256_of(dest) == expect_sha:
            return True
        print("    round finished but SHA256 did not match, resuming…")

    return bool(expect_sha) and dest.is_file() and sha256_of(dest) == expect_sha


# --------------------------------------------------------------------- unpacking

def _zstd_reader(path: Path):
    """Return a readable zstd stream. Prefer the standard library (Py3.14+), else fall back to an external command."""
    try:
        from compression import zstd  # type: ignore[import-not-found]
        return zstd.ZstdFile(path, "rb")
    except ImportError:
        pass
    if shutil.which("zstd"):
        return subprocess.Popen(["zstd", "-dc", str(path)], stdout=subprocess.PIPE).stdout
    if shutil.which("tar"):
        return subprocess.Popen(["tar", "--zstd", "-xOf", str(path)], stdout=subprocess.PIPE).stdout
    die("a zstd payload is required, but neither compression.zstd nor the zstd/tar command is available (install zstd, or use Python 3.14+)")
    raise AssertionError  # unreachable


def _open_tar(path: Path):
    if path.name.endswith(".tar.zst"):
        return tarfile.open(fileobj=_zstd_reader(path), mode="r|")
    # ⚠ It must be `r|*` (**with the star**). In tarfile's mode, `r|` means "**uncompressed** tar stream";
    #   transparent compression needs `r|*`. Writing `r|` reads xz bytes as a tar header ⇒ ReadError: invalid header.
    #   That star cost 1.7 hours: after downloading 91.5 MB it hung in unpacking, looking exactly like a "corrupt file",
    #   while the file's SHA256 matched the upstream official value **bit for bit** — do not delete and re-download, just fix this line.
    return tarfile.open(path, mode="r|*")   # `*` = transparent compression (xz/gz/bz2)


def _strip_member(member: tarfile.TarInfo, strip: int):
    if strip <= 0:
        return member
    parts = Path(member.name).parts
    if len(parts) <= strip:
        if member.isdir():
            return None                       # the top-level directory itself: skip
        return None
    member.name = str(Path(*parts[strip:]))
    if member.linkname:
        lp = Path(member.linkname).parts
        if len(lp) > strip:
            member.linkname = str(Path(*lp[strip:]))
    return member


def extract_tar(path: Path, dest: Path, strip: int) -> None:
    dest.mkdir(parents=True, exist_ok=True)
    with _open_tar(path) as tar:
        def flt(member, _dest=dest):
            member = _strip_member(member, strip)
            if member is None:
                return None
            # Keep only portable member types; reject absolute paths / escapes outside the destination
            target = (dest / member.name).resolve()
            if not str(target).startswith(str(dest.resolve())):
                die(f"archive contains an escaping path: {member.name}")
            return tarfile.data_filter(member, str(dest))
        tar.extractall(path=dest, filter=flt)


def extract_zip(path: Path, dest: Path) -> None:
    dest.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(path) as zf:
        for zinfo in zf.infolist():
            target = (dest / zinfo.filename).resolve()
            if not str(target).startswith(str(dest.resolve())):
                die(f"zip contains an escaping path: {zinfo.filename}")
        zf.extractall(dest)


def _ar_entries(path: Path):
    """Minimal ar parser: yields (name, offset, size). Enough to read .deb files (member names are short)."""
    with path.open("rb") as fh:
        if fh.read(8) != b"!<arch>\n":
            die(f"{path.name} is not an ar archive")
        while True:
            header = fh.read(60)
            if len(header) < 60:
                return
            name = header[0:16].decode("ascii", "replace").strip()
            try:
                size = int(header[48:58].decode("ascii").strip())
            except ValueError:
                die(f"{path.name}: ar member `{name}` has an invalid size field")
            offset = fh.tell()
            yield name.rstrip("/"), offset, size
            fh.seek(offset + size + (size & 1))


def extract_deb(path: Path, dest: Path, strip: int, tmp: Path) -> None:
    """Extract data.tar.* from a .deb and unpack it. No dependency on ar / dpkg-deb."""
    data_entry = None
    with path.open("rb") as fh:
        for name, offset, size in _ar_entries(path):
            if name.startswith("data.tar"):
                fh.seek(offset)
                payload = fh.read(size)
                data_entry = (name, payload)
                break
    if data_entry is None:
        die(f"{path.name}: no data.tar.* member found")
    name, payload = data_entry
    tmp.mkdir(parents=True, exist_ok=True)
    inner = tmp / name
    inner.write_bytes(payload)
    try:
        extract_tar(inner, dest, strip)
    finally:
        inner.unlink(missing_ok=True)


def extract(art: dict, blob: Path) -> None:
    kind = art["kind"]
    dest = PREBUILT / art["into"]
    strip = int(art.get("strip", 0))
    # `merge = true`: several artifacts share one `into` and stack into one tree (clang: one
    # distribution package per component — driver, libLLVM, lld, resource headers). Without it a
    # later artifact would delete what the earlier ones just unpacked. The first entry for a given
    # `into` must NOT set merge: it is the one that clears a stale tree.
    if dest.exists() and not art.get("merge"):
        shutil.rmtree(dest)
    if kind in ("tar.xz", "tar.gz", "tar.zst", "tar.bz2"):
        extract_tar(blob, dest, strip)
    elif kind == "zip":
        extract_zip(blob, dest)
    elif kind == "deb":
        extract_deb(blob, dest, strip, CACHE / ".tmp")
    else:
        die(f"{art['id']}: unrecognized kind={kind!r}")


# ------------------------------------------------------------------- generated output

def write_toolchain_mk(lock: dict) -> None:
    """Generate the Makefile snippet from the lock file's `[toolchain]` table.

    **Declare only what is really in place**: fetching supports per-id selection (say only make/ninja
    first), and in that state the directory PREBUILT_CROSS points to does not exist — declaring
    `PREBUILT_READY := 1` anyway would make the Makefile build the image with an empty path. In-place or
    not is judged by **directory existence**, never by intent.
    """
    tc = lock.get("toolchain") or {}
    arts = lock["artifact"]
    ready: dict[str, bool] = {}
    for key, rel in tc.items():
        # Which artifact owns this path (longest matching `into` prefix)
        owner = None
        for art in arts:
            into = art["into"]
            if rel == into or rel.startswith(into + "/"):
                if owner is None or len(into) > len(owner["into"]):
                    owner = art
        ready[key] = bool(owner) and toolchain_probe(key, rel).exists()

    all_ready = all(ready.values()) and all(is_ready(a) for a in arts)

    lines = [
        "# Generated by tools/fetch-prebuilt.py — **do not edit by hand** (edit prebuilts/toolchain.lock.toml instead)",
        "#",
        "# Purpose: tell the Makefile the **actual paths** of the artifacts under prebuilts/toolchain/, so it need not guess",
        "# the directory names a distro package unpacks to.",
        "#",
        "# Note: the Makefile **trusts no marker** here — it checks each binary with $(wildcard)",
        "# (prebuilts/toolchain/ may be partially fetched). The READY below is for humans and gates only.",
        "",
        f"PREBUILT_READY := {1 if all_ready else 0}",
        f"PREBUILT := {PREBUILT_REL}",
        "",
    ]
    for key, rel in sorted(tc.items()):
        var = "PREBUILT_" + key.upper()
        mark = "" if ready[key] else "   # ← not in place yet"
        lines.append(f"{var} := $(PREBUILT)/{rel}{mark}")
    lines.append("")
    TOOLCHAIN_MK.write_text("\n".join(lines), encoding="utf-8")
    info(f"wrote {TOOLCHAIN_MK.relative_to(REPO_ROOT)}"
         f"({'all in place' if all_ready else 'partially in place'})")


def toolchain_probe(key: str, rel: str) -> Path:
    """Translate one `[toolchain]` entry into "the file that should exist".

    Values come in two kinds:
      * **path** — e.g. `make/bin/make`, tested for existence directly;
      * **prefix** — ending in `-` (e.g. `.../bin/aarch64-none-elf-`, the CROSS_COMPILE prefix).
        It is **not a file**, so exists() on it is always false — an early implementation reported
        MISS toolchain.cross even when everything was fine. The probe for a prefix = `<prefix>gcc`.
    """
    return PREBUILT / (rel + "gcc" if rel.endswith("-") else rel)


def stamp_path(art_id: str) -> Path:
    return STAMP / f"{art_id}.ok"


def is_ready(art: dict) -> bool:
    p = stamp_path(art["id"])
    if not p.is_file():
        return False
    if p.read_text(encoding="utf-8").strip() != art["sha256"]:
        return False
    return (PREBUILT / art["into"]).is_dir()


# --------------------------------------------------------------------- main flow

def fetch_one(art: dict, *, force: bool, verify_only: bool) -> bool:
    aid = art["id"]
    blob = CACHE / art["filename"]
    print(f"[{aid}] {art['version']}  ({art['kind']})")

    if verify_only:
        if not blob.is_file():
            info("not in the cache, skipping")
            return False
        got = sha256_of(blob)
        if got == art["sha256"]:
            info(f"SHA256 ok  {got[:16]}…")
            return True
        info(f"SHA256 **MISMATCH**  expected {art['sha256'][:16]}…  got {got[:16]}…")
        return False

    if force and blob.is_file():
        info("--force: discarding the existing file and re-downloading")
        blob.unlink()

    if blob.is_file() and sha256_of(blob) == art["sha256"]:
        info(f"cache hit and verification passed  {human(blob.stat().st_size)}")
    else:
        if blob.is_file():
            # A partial chunk = **an unfinished download**, not a bad cache ⇒ resume, do not delete
            info(f"found an unfinished partial chunk {human(blob.stat().st_size)}, resuming from there")
        info(f"downloading {art['url']}")
        if not download(art["url"], blob, art["sha256"]):
            got = sha256_of(blob) if blob.is_file() else "(no file)"
            die(f"{aid}: download unfinished or SHA256 mismatch\n"
                f"  expected {art['sha256']}\n  got {got}\n"
                f"  file kept at {blob}: for comparison (if upstream changed content ⇒ update the lock explicitly); "
                f"if the local copy is corrupt ⇒ re-download with --force")
        info(f"SHA256 ok  {human(blob.stat().st_size)}")

    if not is_ready(art):
        info(f"unpacking → prebuilts/toolchain/{art['into']}")
        extract(art, blob)
        STAMP.mkdir(parents=True, exist_ok=True)
        stamp_path(aid).write_text(art["sha256"] + "\n", encoding="utf-8")
    else:
        info("already in place")
    return True


def check(lock: dict) -> int:
    bad = 0
    print("prebuilt in-place check:")
    for art in lock["artifact"]:
        ok = is_ready(art)
        print(f"  {'ok  ' if ok else 'MISS'} {art['id']:<20} prebuilts/toolchain/{art['into']}")
        bad += 0 if ok else 1
    tc = lock.get("toolchain") or {}
    for key, rel in sorted(tc.items()):
        probe = toolchain_probe(key, rel)
        ok = probe.exists()
        shown = rel + "gcc" if rel.endswith("-") else rel
        print(f"  {'ok  ' if ok else 'MISS'} {'toolchain.' + key:<20} prebuilts/toolchain/{shown}")
        bad += 0 if ok else 1
    print("PASS: prebuilt in place" if bad == 0 else f"FAIL: {bad} item(s) missing (run make prebuilt to fetch)")
    return 0 if bad == 0 else 1


def print_hash(url: str) -> int:
    """For maintaining the lock file: download and print the sha256 (not written under prebuilts/toolchain/)."""
    tmp = CACHE / ".tmp" / "hash-probe"
    tmp.parent.mkdir(parents=True, exist_ok=True)
    download(url, tmp, "")
    print(f"{sha256_of(tmp)}  {tmp.stat().st_size}  {url}")
    tmp.unlink(missing_ok=True)
    return 0


def print_env(lock: dict) -> int:
    """Print a **shell snippet** (for `setup.sh` to source): the toolchain bin dirs and the CROSS_COMPILE prefix.

    **Why this is printed here instead of having setup.sh assemble paths**: the single source of truth for
    paths is the lock file (`prebuilts/toolchain.lock.toml`), and the reasoning "where is bin after an entry is
    unpacked" should be implemented in exactly one place. A second round of glob/derivation on the shell side
    would create two truths again — the very trap recorded in ADR-0002 §7.3-1.

    Reads the lock file only, **does not require the fetch to have run** ⇒ a fresh checkout (that has not run
    `make prebuilt`) still gets paths, and the consumer decides whether the directories exist.
    """
    tc = lock.get("toolchain") or {}
    prebuilt_rel = PREBUILT.relative_to(REPO_ROOT).as_posix()
    bin_dirs: list[str] = []
    cross = ""
    make_dir = ""
    for key, rel in sorted(tc.items()):
        if rel.endswith("-"):
            cross = f"{prebuilt_rel}/{rel}"
            d = os.path.dirname(rel)              # .../bin/aarch64-none-elf- → .../bin
        else:
            d = os.path.dirname(rel)              # make/bin/make → make/bin
            if key == "make":
                make_dir = f"{prebuilt_rel}/{d}"
        if d:
            full = f"{prebuilt_rel}/{d}"
            if full not in bin_dirs:
                bin_dirs.append(full)
    print(f"# generated by tools/fetch-prebuilt.py --print-env — path truth = {LOCK.name}")
    print("# all paths are **repo-root relative** (the consumer prepends $BRICKOS_ROOT) — so a forgotten prefix cannot cause typos")
    print(f"PREBUILT_REL='{prebuilt_rel}'")
    print(f"PREBUILT_CROSS='{cross}'")
    print(f"PREBUILT_MAKE_BIN_DIR='{make_dir}'")
    print(f"PREBUILT_BIN_DIRS='{' '.join(bin_dirs)}'")
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        prog="fetch-prebuilt",
        description="fetch the prebuilts/toolchain/ toolset (pinned versions + SHA256 verification + unpack)",
    )
    ap.add_argument("ids", nargs="*", help="fetch only these ids (default = all)")
    ap.add_argument("--list", action="store_true", help="list and exit")
    ap.add_argument("--check", action="store_true", help="only check what is in place")
    ap.add_argument("--verify", action="store_true", help="recompute SHA256 of cached files")
    ap.add_argument("--force", action="store_true", help="ignore the cache and re-download")
    ap.add_argument("--print-hash", metavar="URL", help="download and print the sha256 of a URL (for maintaining the lock)")
    ap.add_argument("--print-env", action="store_true",
                    help="print a shell snippet (toolchain bin dirs + CROSS_COMPILE prefix) for setup.sh to source")
    args = ap.parse_args(argv)

    if args.print_hash:
        return print_hash(args.print_hash)

    lock = load_lock()

    if args.print_env:
        return print_env(lock)

    if args.list:
        print(f"{LOCK.relative_to(REPO_ROOT)}  (version {lock['version']})")
        for a in lock["artifact"]:
            print(f"  {a['id']:<20} {a['version']:<12} {a['kind']:<8} → prebuilts/toolchain/{a['into']}")
            print(f"  {'':<20} {a['url']}")
            if a.get("why"):
                print(f"  {'':<20} purpose: {a['why']}")
        return 0

    if args.check:
        return check(lock)

    arts = lock["artifact"]
    if args.ids:
        want = set(args.ids)
        unknown = want - {a["id"] for a in arts}
        if unknown:
            die(f"unknown id: {', '.join(sorted(unknown))} (use --list for available ids)", 2)
        arts = [a for a in arts if a["id"] in want]

    if args.verify:
        ok = all(fetch_one(a, force=False, verify_only=True) for a in arts)
        return 0 if ok else 1

    for a in arts:
        fetch_one(a, force=args.force, verify_only=False)
    write_toolchain_mk(lock)
    print()
    return check(lock)


if __name__ == "__main__":
    sys.exit(main())
