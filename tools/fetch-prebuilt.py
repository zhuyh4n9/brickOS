#!/usr/bin/env python3
"""fetch-prebuilt — 把 prebuilts/toolchain/ 工具集取到本地(锁版本 + 校验 + 解压 + 落戳)。

## 为什么要有它

`prebuilts/toolchain/` **不进版本库**(它是派生目录, 体积以 GB 计), 所以需要一条**可重放**的
命令把它重建出来。可重放的含义是: 版本与 URL 钉在 `prebuilts/toolchain.lock.toml` 里,
下载后**必须**校验 SHA256 —— 校验不过就报错退出, 绝不"凑合用一个差不多的"。

这条纪律对齐设计侧 `4-04`/`2-02` BR-D3 的修订口径:
"每语言最小依赖集 + **锁版本** + **可离线复现**"。

## 引导依赖(刻意压到最小)

只依赖 **Python 3 标准库**: `urllib`(下载) + `hashlib`(校验) + `tarfile`/`lzma`/
`compression.zstd`(解压) + `zipfile` + 自带的 ar 解析(.deb)。
**不需要** curl / wget / tar / xz / zstd / ar / dpkg-deb / unzip。

  * `compression.zstd` 是 Python **3.14** 起进标准库的。更低版本若碰到 zstd 压缩的
    载荷(新发行版的 .deb 都是), 会退回调用 `zstd` 或 `tar --zstd` 命令。

## 用法

    python3 tools/fetch-prebuilt.py                 # 取齐缺失件
    python3 tools/fetch-prebuilt.py <id> [...]      # 只取指定件
    python3 tools/fetch-prebuilt.py --list          # 列清单(不下载)
    python3 tools/fetch-prebuilt.py --check         # 只检查是否已就位(不下载)
    python3 tools/fetch-prebuilt.py --verify        # 重算 cache 内文件的 SHA256
    python3 tools/fetch-prebuilt.py --force         # 忽略缓存重下
    python3 tools/fetch-prebuilt.py --print-hash <url>   # 取某 URL 的 sha256(维护锁用)

退出码: 0 成功 / 1 校验或解压失败 / 2 用法或环境错
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
PREBUILT = REPO_ROOT / "prebuilts" / "toolchain"     # 派生侧(不进版本库); 见 ADR-0005
CACHE = PREBUILT / ".cache"
STAMP = PREBUILT / ".stamp"
LOCK = REPO_ROOT / "prebuilts/toolchain.lock.toml"
TOOLCHAIN_MK = PREBUILT / "toolchain.mk"
# 写进 toolchain.mk 的路径(相对仓库根), 由脚本自己算, 不硬编码
PREBUILT_REL = PREBUILT.relative_to(REPO_ROOT).as_posix()

# 读块**故意小**: 慢链路上若按 1 MB 读, 一个 read() 要凑满 1 MB 才返回 ⇒
# 落盘粒度就是 1 MB(进度不可见, 断线白收最多 1 MB)。64 KB 兼顾吞吐与可观测。
# 配合 read1(): 有多少收多少, 不阻塞凑数。
CHUNK = 1 << 16
UA = "brickOS-fetch-prebuilts/toolchain/1 (+https://github.com/zhuyh4n9/brickOS)"


def die(msg: str, code: int = 1) -> "NoReturn":  # noqa: F821
    print(f"FAIL: {msg}", file=sys.stderr)
    raise SystemExit(code)


def info(msg: str) -> None:
    print(f"  {msg}")


def human(n: int) -> str:
    return f"{n / 1048576:.1f} MB" if n >= 1048576 else f"{n / 1024:.0f} KB"


# --------------------------------------------------------------------- 锁文件

def load_lock() -> dict:
    if not LOCK.is_file():
        die(f"找不到锁文件 {LOCK}", 2)
    with LOCK.open("rb") as fh:
        lock = tomllib.load(fh)
    if lock.get("version") != 1:
        die(f"{LOCK.name}: 不认识的 version={lock.get('version')!r}(期望 1)", 2)
    arts = lock.get("artifact") or []
    if not arts:
        die(f"{LOCK.name}: 没有任何 [[artifact]] 条目", 2)
    seen: set[str] = set()
    for a in arts:
        for key in ("id", "url", "sha256", "kind", "filename", "into"):
            if not a.get(key):
                die(f"{LOCK.name}: 条目缺 {key}: {a!r}", 2)
        if a["id"] in seen:
            die(f"{LOCK.name}: id 重复: {a['id']}", 2)
        seen.add(a["id"])
        if len(a["sha256"]) != 64 or any(c not in "0123456789abcdef" for c in a["sha256"]):
            die(f"{LOCK.name}: {a['id']} 的 sha256 不是 64 位小写十六进制", 2)
    return lock


# ------------------------------------------------------------------ 下载/校验

def sha256_of(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(CHUNK), b""):
            h.update(chunk)
    return h.hexdigest()


def download(url: str, dest: Path, expect_sha: str, *, attempts: int = 60) -> bool:
    """断点续传 + 重试的下载; 成功(末尾 SHA256 匹配)返回 True。

    **为什么续传必须写在这里**(踩过的坑): 早期版本把"缓存里那个文件校验不过"直接
    当成 miss 并删掉重下 —— 结果把另一路正在续传的 14 MB 分片删了, 两路下载互相破坏。
    分片的正确语义是"**尚未完成的下载**", 不是"坏缓存"。所以:

      * 分片**保留**, 用 HTTP Range 从它的大小接着下(服务器不支持 Range 则截断重来);
      * 只在**整份下完之后**才判 SHA256;
      * 校验不过**也不删** —— 留下供比对, 并提示用 `--force`(ADR-0002 §2-4)。
    """
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
                    # 服务器忽略了 Range ⇒ 从头来, 且**必须截断**(否则会接出垃圾)
                    print(f"    (服务器不支持续传, 从头下)")
                    have, mode = 0, "wb"
                total = 0
                crange = resp.headers.get("Content-Range")
                clen = resp.headers.get("Content-Length")
                if crange and "/" in crange:
                    total = int(crange.rsplit("/", 1)[1])
                elif clen:
                    total = have + int(clen)
                done = have
                read = getattr(resp, "read1", resp.read)   # read1: 有多少给多少
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
                # 服务器认为已经给完了 ⇒ 拿现有文件去校验, 不要无限重试
                if expect_sha and sha256_of(dest) == expect_sha:
                    return True
                print(f"    上游报告范围越界(416), 但现有文件校验不符 —— 停止")
                return False
            print(f"    第 {attempt} 次失败(HTTP {exc.code}), 重试…")
            time.sleep(min(2 * attempt, 15))
            continue
        except (urllib.error.URLError, OSError, TimeoutError) as exc:
            got = dest.stat().st_size if dest.is_file() else 0
            print(f"\n    第 {attempt} 次中断({type(exc).__name__}), 已收 {human(got)}, 续传…")
            time.sleep(min(2 * attempt, 15))
            continue

        if not expect_sha:
            return True                      # 只探哈希: 下完一轮即成功
        if sha256_of(dest) == expect_sha:
            return True
        print("    本轮结束但 SHA256 未通过, 继续续传…")

    return bool(expect_sha) and dest.is_file() and sha256_of(dest) == expect_sha


# --------------------------------------------------------------------- 解压

def _zstd_reader(path: Path):
    """返回可读的 zstd 流。优先标准库(Py3.14+), 否则退回外部命令。"""
    try:
        from compression import zstd  # type: ignore[import-not-found]
        return zstd.ZstdFile(path, "rb")
    except ImportError:
        pass
    if shutil.which("zstd"):
        return subprocess.Popen(["zstd", "-dc", str(path)], stdout=subprocess.PIPE).stdout
    if shutil.which("tar"):
        return subprocess.Popen(["tar", "--zstd", "-xOf", str(path)], stdout=subprocess.PIPE).stdout
    die("需要 zstd 载荷, 但既没 compression.zstd 也没有 zstd/tar 命令(装 zstd, 或用 Python 3.14+)")
    raise AssertionError  # 不可达


def _open_tar(path: Path):
    if path.name.endswith(".tar.zst"):
        return tarfile.open(fileobj=_zstd_reader(path), mode="r|")
    # ⚠ 必须是 `r|*`(**带星号**)。tarfile 的 mode 里 `r|` 的含义是「**未压缩**的 tar 流」,
    #   透明压缩要 `r|*`。写成 `r|` 就会拿 xz 字节当 tar 头读 ⇒ ReadError: invalid header。
    #   这个星号值 1.7 小时: 91.5 MB 下完之后卡在解压, 现象酷似"文件坏了", 而其实
    #   文件的 SHA256 与上游官方值**逐位一致** —— 别删了重下, 改这一处即可。
    return tarfile.open(path, mode="r|*")   # `*` = 透明压缩(xz/gz/bz2)


def _strip_member(member: tarfile.TarInfo, strip: int):
    if strip <= 0:
        return member
    parts = Path(member.name).parts
    if len(parts) <= strip:
        if member.isdir():
            return None                       # 顶层目录本身: 跳过
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
            # 只保留可移植的成员类型; 拒绝绝对路径 / 逃逸到目录外
            target = (dest / member.name).resolve()
            if not str(target).startswith(str(dest.resolve())):
                die(f"压缩包含逃逸路径: {member.name}")
            return tarfile.data_filter(member, str(dest))
        tar.extractall(path=dest, filter=flt)


def extract_zip(path: Path, dest: Path) -> None:
    dest.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(path) as zf:
        for zinfo in zf.infolist():
            target = (dest / zinfo.filename).resolve()
            if not str(target).startswith(str(dest.resolve())):
                die(f"zip 含逃逸路径: {zinfo.filename}")
        zf.extractall(dest)


def _ar_entries(path: Path):
    """极简 ar 解析: 返回 (name, offset, size)。够读 .deb 用(成员名都短)。"""
    with path.open("rb") as fh:
        if fh.read(8) != b"!<arch>\n":
            die(f"{path.name} 不是 ar 归档")
        while True:
            header = fh.read(60)
            if len(header) < 60:
                return
            name = header[0:16].decode("ascii", "replace").strip()
            try:
                size = int(header[48:58].decode("ascii").strip())
            except ValueError:
                die(f"{path.name}: ar 成员 `{name}` 的大小字段非法")
            offset = fh.tell()
            yield name.rstrip("/"), offset, size
            fh.seek(offset + size + (size & 1))


def extract_deb(path: Path, dest: Path, strip: int, tmp: Path) -> None:
    """从 .deb 里取出 data.tar.* 再解压。不依赖 ar / dpkg-deb。"""
    data_entry = None
    with path.open("rb") as fh:
        for name, offset, size in _ar_entries(path):
            if name.startswith("data.tar"):
                fh.seek(offset)
                payload = fh.read(size)
                data_entry = (name, payload)
                break
    if data_entry is None:
        die(f"{path.name}: 找不到 data.tar.* 成员")
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
    if dest.exists():
        shutil.rmtree(dest)
    if kind in ("tar.xz", "tar.gz", "tar.zst", "tar.bz2"):
        extract_tar(blob, dest, strip)
    elif kind == "zip":
        extract_zip(blob, dest)
    elif kind == "deb":
        extract_deb(blob, dest, strip, CACHE / ".tmp")
    else:
        die(f"{art['id']}: 不认识的 kind={kind!r}")


# ------------------------------------------------------------------- 生成物

def write_toolchain_mk(lock: dict) -> None:
    """据锁文件的 `[toolchain]` 表生成 Makefile 片段。

    **只声明真正就位的件**: 取件支持按 id 取(比如先只取 make/ninja), 那种状态下
    PREBUILT_CROSS 指向的目录并不存在 —— 若照样宣告 `PREBUILT_READY := 1`,
    Makefile 就会拿着一个空路径去编镜像。就位与否以**目录存在**为准, 不以意图为准。
    """
    tc = lock.get("toolchain") or {}
    arts = lock["artifact"]
    ready: dict[str, bool] = {}
    for key, rel in tc.items():
        # 该路径归属哪一个 artifact(按 into 前缀最长匹配)
        owner = None
        for art in arts:
            into = art["into"]
            if rel == into or rel.startswith(into + "/"):
                if owner is None or len(into) > len(owner["into"]):
                    owner = art
        ready[key] = bool(owner) and toolchain_probe(key, rel).exists()

    all_ready = all(ready.values()) and all(is_ready(a) for a in arts)

    lines = [
        "# 由 tools/fetch-prebuilt.py 生成 —— **勿手改**(改它请改 prebuilts/toolchain.lock.toml)",
        "#",
        "# 作用: 把 prebuilts/toolchain/ 里各产物的**实际路径**告诉 Makefile, 免得 Makefile 去猜",
        "# 发行包解出来的目录名。",
        "#",
        "# 注意: Makefile **不信**这里的任何标记 —— 它逐个用 $(wildcard) 检查二进制是否",
        "# 真的存在(prebuilts/toolchain/ 允许只取一部分)。下面的 READY 只用于人读与门禁。",
        "",
        f"PREBUILT_READY := {1 if all_ready else 0}",
        f"PREBUILT := {PREBUILT_REL}",
        "",
    ]
    for key, rel in sorted(tc.items()):
        var = "PREBUILT_" + key.upper()
        mark = "" if ready[key] else "   # ← 尚未就位"
        lines.append(f"{var} := $(PREBUILT)/{rel}{mark}")
    lines.append("")
    TOOLCHAIN_MK.write_text("\n".join(lines), encoding="utf-8")
    info(f"写出 {TOOLCHAIN_MK.relative_to(REPO_ROOT)}"
         f"({'全部就位' if all_ready else '部分就位'})")


def toolchain_probe(key: str, rel: str) -> Path:
    """把 `[toolchain]` 的一项翻译成"应该存在的那个文件"。

    值分两类:
      * **路径** —— 例如 `make/bin/make`, 直接判存在;
      * **前缀** —— 以 `-` 结尾(如 `.../bin/aarch64-none-elf-`, 即 CROSS_COMPILE 前缀)。
        它**不是文件**, 拿它 exists() 永远为假 —— 早期实现在一切正常时报 MISS toolchain.cross。
        前缀的探针 = `<前缀>gcc`。
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


# --------------------------------------------------------------------- 主流程

def fetch_one(art: dict, *, force: bool, verify_only: bool) -> bool:
    aid = art["id"]
    blob = CACHE / art["filename"]
    print(f"[{aid}] {art['version']}  ({art['kind']})")

    if verify_only:
        if not blob.is_file():
            info("cache 里没有, 跳过")
            return False
        got = sha256_of(blob)
        if got == art["sha256"]:
            info(f"SHA256 ok  {got[:16]}…")
            return True
        info(f"SHA256 **不符**  期望 {art['sha256'][:16]}…  实得 {got[:16]}…")
        return False

    if force and blob.is_file():
        info("--force: 丢弃现有文件重下")
        blob.unlink()

    if blob.is_file() and sha256_of(blob) == art["sha256"]:
        info(f"cache 命中且校验通过  {human(blob.stat().st_size)}")
    else:
        if blob.is_file():
            # 分片 = **尚未完成的下载**, 不是坏缓存 ⇒ 续传, 不要删
            info(f"发现未完成的分片 {human(blob.stat().st_size)}, 从这里续传")
        info(f"下载 {art['url']}")
        if not download(art["url"], blob, art["sha256"]):
            got = sha256_of(blob) if blob.is_file() else "(无文件)"
            die(f"{aid}: 下载未完成或 SHA256 不符\n"
                f"  期望 {art['sha256']}\n  实得 {got}\n"
                f"  文件留在 {blob}: 供比对(上游换内容 ⇒ 显式更新锁文件); "
                f"若本地已损坏 ⇒ 用 --force 重下")
        info(f"SHA256 ok  {human(blob.stat().st_size)}")

    if not is_ready(art):
        info(f"解压 → prebuilts/toolchain/{art['into']}")
        extract(art, blob)
        STAMP.mkdir(parents=True, exist_ok=True)
        stamp_path(aid).write_text(art["sha256"] + "\n", encoding="utf-8")
    else:
        info("已就位")
    return True


def check(lock: dict) -> int:
    bad = 0
    print("prebuilt 就位检查:")
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
    print("PASS: prebuilt 已就位" if bad == 0 else f"FAIL: {bad} 项缺失(跑 make prebuilt 取件)")
    return 0 if bad == 0 else 1


def print_hash(url: str) -> int:
    """维护锁文件用: 下载并打印 sha256(不落盘到 prebuilts/toolchain/)。"""
    tmp = CACHE / ".tmp" / "hash-probe"
    tmp.parent.mkdir(parents=True, exist_ok=True)
    download(url, tmp, "")
    print(f"{sha256_of(tmp)}  {tmp.stat().st_size}  {url}")
    tmp.unlink(missing_ok=True)
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        prog="fetch-prebuilt",
        description="取 prebuilts/toolchain/ 工具集(锁版本 + SHA256 校验 + 解压)",
    )
    ap.add_argument("ids", nargs="*", help="只取这些 id(缺省 = 全部)")
    ap.add_argument("--list", action="store_true", help="列清单后退出")
    ap.add_argument("--check", action="store_true", help="只检查是否已就位")
    ap.add_argument("--verify", action="store_true", help="重算缓存文件的 SHA256")
    ap.add_argument("--force", action="store_true", help="忽略缓存重下")
    ap.add_argument("--print-hash", metavar="URL", help="下载并打印该 URL 的 sha256(维护锁用)")
    args = ap.parse_args(argv)

    if args.print_hash:
        return print_hash(args.print_hash)

    lock = load_lock()

    if args.list:
        print(f"{LOCK.relative_to(REPO_ROOT)}  (version {lock['version']})")
        for a in lock["artifact"]:
            print(f"  {a['id']:<20} {a['version']:<12} {a['kind']:<8} → prebuilts/toolchain/{a['into']}")
            print(f"  {'':<20} {a['url']}")
            if a.get("why"):
                print(f"  {'':<20} 用途: {a['why']}")
        return 0

    if args.check:
        return check(lock)

    arts = lock["artifact"]
    if args.ids:
        want = set(args.ids)
        unknown = want - {a["id"] for a in arts}
        if unknown:
            die(f"未知 id: {', '.join(sorted(unknown))}(用 --list 看可用 id)", 2)
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
