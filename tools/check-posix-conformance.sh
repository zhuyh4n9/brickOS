#!/usr/bin/env bash
#
# brickOS prototype — iface/posix **与 POSIX 一致**的门禁(ADR-0028)
#
# 为什么需要它:
#   `iface/posix` 是 APP 看见的 POSIX 面(它把 `runtime/posix` 的头原样再导出)。
#   一旦这些头与真 POSIX **不一致**, 后果不是"编不过"而是**更坏**:
#     - 类型宽度不同 ⇒ 同一份代码在两边看到不同的布局(静默错值);
#     - 常量不同 ⇒ 同一个 flag 位在两边的含义不同;
#     - 少一个必需的名字(如 STDIN_FILENO) ⇒ 移植过来的 POSIX 代码编不过。
#   这些都不能靠"读一遍头文件"发现 —— 必须**拿宿主 POSIX 当对照物逐条比**。
#
# 判据(四条, 全部以宿主 glibc 为对照):
#   ① **类型宽度**: `size_t/ssize_t/off_t/dev_t/blksize_t/…` 逐条与宿主 sizeof 相同;
#   ② **常量**: APP 可能比较或传递的常量(`O_*`/`S_I*`/`DT_*`/`SEEK_*`/`FD_*`)值与宿主相同;
#   ③ **函数签名**: 本仓声明的每个 POSIX 函数, 其签名与宿主同名函数**赋值兼容**
#      (做法: 只包含宿主头, 把我们写的原型转成函数指针类型去接宿主函数 ⇒ 不符则编译报错);
#   ④ **结构体成员**: `struct stat` / `struct dirent` 的 POSIX 必需成员**在场**且**类型宽度**正确。
#      ★ **不**比字段偏移: POSIX 不规定 `struct stat` 的成员顺序(那是实现自由),
#        拿 glibc 的偏移当判据会把"glibc 的 ABI"当成"POSIX 的要求"。
#
# 白名单: 少数**有意偏离**的常量(必须在下面的表里写明理由, 否则一律判红)。
#
# 用法: bash tools/check-posix-conformance.sh
# 退出码: 0 = 全过, 1 = 有偏离, 2 = 用法/环境错
set -uo pipefail

cd "$(dirname "$0")/.." || exit 2

exec python3 - "$@" <<'PY'
import re, subprocess, pathlib, sys, tempfile

ROOT = pathlib.Path.cwd()
INC = ROOT/"runtime/posix/include"
CORE_INC = ROOT/"core/include"
PLAT_INC = ROOT/"platform/qemu-aarch64/include"

# ---- 有意偏离的白名单: 名字 → 理由(必须写清"为什么可以不同") -------------------
ALLOW = {
    "FD_SETSIZE":
        "本仓 fd 表上界 BR_FT_MAX=32(fd 0..31), FD_SETSIZE=64 已覆盖**每一个合法 fd**"
        " ⇒ 满足 POSIX 对 FD_SETSIZE 的要求(须容纳实现能创建的所有 fd); "
        "宿主 1024 是 Linux 的选择, 不是 POSIX 的下限。"
        " 取更大只会让 fd_set 占更多栈而无任何可用 fd 支撑。",
}

TYPES = ["size_t","ssize_t","off_t","ino_t","mode_t","nlink_t","dev_t","blksize_t",
         "blkcnt_t","uid_t","gid_t","pid_t","time_t","suseconds_t","useconds_t"]

CONSTS = {
 "O_RDONLY":1,"O_WRONLY":1,"O_RDWR":1,"O_ACCMODE":1,"O_CREAT":1,"O_EXCL":1,"O_NOCTTY":1,
 "O_TRUNC":1,"O_APPEND":1,"O_NONBLOCK":1,"O_DSYNC":1,"O_DIRECT":1,"O_LARGEFILE":1,
 "O_DIRECTORY":1,"O_NOFOLLOW":1,"O_NOATIME":1,"O_CLOEXEC":1,
 "F_DUPFD":1,"F_GETFD":1,"F_SETFD":1,"F_GETFL":1,"F_SETFL":1,
 "S_IFMT":1,"S_IFSOCK":1,"S_IFLNK":1,"S_IFREG":1,"S_IFBLK":1,"S_IFDIR":1,"S_IFCHR":1,
 "S_IFIFO":1,"S_ISUID":1,"S_ISGID":1,"S_ISVTX":1,"S_IRWXU":1,"S_IRUSR":1,"S_IWUSR":1,
 "S_IXUSR":1,"S_IRWXG":1,"S_IRWXO":1,
 "DT_UNKNOWN":1,"DT_FIFO":1,"DT_CHR":1,"DT_DIR":1,"DT_BLK":1,"DT_REG":1,"DT_LNK":1,"DT_SOCK":1,
 "SEEK_SET":1,"SEEK_CUR":1,"SEEK_END":1,
 "STDIN_FILENO":1,"STDOUT_FILENO":1,"STDERR_FILENO":1,
 "FD_SETSIZE":1,
}

# POSIX 必需的 struct 成员(只查"在场 + 类型宽度", 不查偏移)
STRUCT_MEMBERS = {
    "stat":   ["st_dev","st_ino","st_mode","st_nlink","st_uid","st_gid","st_rdev",
               "st_size","st_blksize","st_blocks","st_atime","st_mtime","st_ctime"],
    "dirent": ["d_ino","d_off","d_reclen","d_type","d_name"],
}

HDR_FOR_SIG = {
    "unistd.h":"<unistd.h>","fcntl.h":"<fcntl.h>","dirent.h":"<dirent.h>",
    "sys/stat.h":"<sys/stat.h>","stdio.h":"<stdio.h>","stdlib.h":"<stdlib.h>",
    "string.h":"<string.h>","pthread.h":"<pthread.h>","semaphore.h":"<semaphore.h>",
    "poll.h":"<poll.h>","time.h":"<time.h>","sys/uio.h":"<sys/uio.h>",
    "sys/select.h":"<sys/select.h>",
}
SIG_ALLOW = {"pthread_yield": "宿主标 deprecated(仅告警, 非签名不符)",
             "chown": "宿主该头未声明(chown 在 unistd.h); 探针口径问题, 非签名不符",
             "fchown": "同上"}

tmp = pathlib.Path(tempfile.mkdtemp())
fails = []

def run(argv):
    return subprocess.run(argv, capture_output=True, text=True)

def cc(prog, incs, extra=()):
    src = tmp/"p.c"; src.write_text(prog)
    argv = ["gcc", "-std=gnu11"] + [f"-I{i}" for i in incs] + list(extra) + \
           ["-o", str(tmp/"p"), str(src)]
    r = run(argv)
    return r

# ============================================================ ① 类型宽度
prog = "#include <sys/types.h>\n#include <sys/stat.h>\n#include <unistd.h>\n#include <stdio.h>\nint main(void){\n"
for t in TYPES:
    prog += f' printf("{t} %zu\\n", sizeof({t}));\n'
prog += " return 0;}"
r = cc(prog, [])
if r.returncode:
    print("FAIL: 宿主类型探针编不过:", r.stderr[:400]); sys.exit(2)
host_t = dict(l.split() for l in run([str(tmp/"p")]).stdout.splitlines())

r = cc(prog, [INC, CORE_INC, PLAT_INC], ["-DBR_CFG_TICK_HZ=200"])
if r.returncode:
    print("FAIL: 本仓类型探针编不过:\n", r.stderr[:800]); sys.exit(1)
ours_t = dict(l.split() for l in run([str(tmp/"p")]).stdout.splitlines())

for t in TYPES:
    o, h = ours_t.get(t), host_t.get(t)
    if o is None:
        fails.append(f"类型 {t}: 本仓头里没有定义"); continue
    if o != h:
        fails.append(f"类型 {t}: 本仓 {o} B, POSIX {h} B")

# ============================================================ ② 常量
names = list(CONSTS)
prog = "#define _GNU_SOURCE\n#include <fcntl.h>\n#include <sys/stat.h>\n#include <dirent.h>\n" \
       "#include <unistd.h>\n#include <sys/select.h>\n#include <stdio.h>\nint main(void){\n"
for n in names:
    prog += f'#ifdef {n}\n printf("{n} %lld\\n",(long long)({n}));\n#else\n printf("{n} MISSING\\n");\n#endif\n'
prog += " return 0;}"
r = cc(prog, [])
host_c = dict(l.split() for l in run([str(tmp/"p")]).stdout.splitlines())

def eval_c(expr, table):
    e = re.sub(r"/\*.*?\*/", "", expr).strip()
    e = re.sub(r"\b([0-9]+)[uUlL]+\b", r"\1", e)
    e = re.sub(r"\b0([0-7]+)\b", lambda m: "0o"+m.group(1), e)
    for _ in range(8):
        new = re.sub(r"\b([A-Z_][A-Z0-9_]*)\b",
                     lambda m: str(table[m.group(1)]) if m.group(1) in table else m.group(0), e)
        if new == e: break
        e = new
    if not re.fullmatch(r"[0-9\s\(\)\+\-\*/<>\|&~^%xXa-fFo]+", e):
        return None
    try: return int(eval(e))
    except Exception: return None

defs = {}
for f in sorted(INC.rglob("*.h")):
    text = re.sub(r"/\*.*?\*/", "", f.read_text(), flags=re.S)
    for m in re.finditer(r"^#define\s+([A-Z_][A-Z0-9_]*)\s+(.+)$", text, re.M):
        defs.setdefault(m.group(1), m.group(2).strip())
vals = {}
for _ in range(8):
    for k, v in defs.items():
        if k not in vals:
            got = eval_c(v, vals)
            if got is not None: vals[k] = got

for n in names:
    h = host_c.get(n)
    if h == "MISSING" or h is None: continue
    o = vals.get(n)
    if o is None:
        fails.append(f"常量 {n}: 本仓头里没有定义(POSIX 有)"); continue
    if o != int(h):
        if n in ALLOW: continue
        fails.append(f"常量 {n}: 本仓 {o}, POSIX {h}")

# ============================================================ ③ 函数签名
proto_re = re.compile(r"^\s*([A-Za-z_][\w \t\*]*?)\s+([a-z_]\w*)\s*\(([^;{]*)\)\s*;", re.M)
checked = 0
for hdr, host_inc in HDR_FOR_SIG.items():
    f = INC/hdr
    if not f.exists(): continue
    text = re.sub(r"/\*.*?\*/", "", f.read_text(), flags=re.S)
    for m in proto_re.finditer(text):
        ret, name, params = m.group(1).strip(), m.group(2), m.group(3).strip()
        if name.startswith("br_") or name in SIG_ALLOW: continue
        params = "void" if re.sub(r"\s+"," ",params) in ("void","") else re.sub(r"\s+"," ",params)
        prog = (f"#define _GNU_SOURCE\n#include <stdio.h>\n#include {host_inc}\n"
                f"static {ret} (*probe)({params}) = {name};\nint main(void){{(void)probe;return 0;}}")
        r = cc(prog, [])
        checked += 1
        if r.returncode:
            bad = [l.strip() for l in r.stderr.splitlines()
                   if "incompatible" in l or "conflicting" in l]
            if bad:
                fails.append(f"签名 {hdr}:{name} 与 POSIX 不符 — {bad[0][:160]}")
            elif "undeclared" in r.stderr:
                fails.append(f"签名 {hdr}:{name}: 宿主没有这个符号(本仓自造?)")

# ============================================================ ④ 结构体成员
for st, members in STRUCT_MEMBERS.items():
    prog = ("#define _GNU_SOURCE\n#include <sys/stat.h>\n#include <dirent.h>\n#include <stdio.h>\n"
            "int main(void){\n")
    for mem in members:
        prog += (f' printf("{st}.{mem} %zu\\n", sizeof(((struct {st} *)0)->{mem}));\n')
    prog += " return 0;}"
    r = cc(prog, [])
    if r.returncode:
        print("FAIL: 宿主结构体探针编不过:", r.stderr[:300]); sys.exit(2)
    host_m = dict(l.split() for l in run([str(tmp/"p")]).stdout.splitlines())
    r = cc(prog, [INC, CORE_INC, PLAT_INC], ["-DBR_CFG_TICK_HZ=200"])
    if r.returncode:
        fails.append(f"struct {st}: 本仓头里编不过(成员缺失?) — {r.stderr.splitlines()[0][:160]}"
                     if r.stderr else f"struct {st}: 本仓头里编不过")
        continue
    ours_m = dict(l.split() for l in run([str(tmp/"p")]).stdout.splitlines())
    for mem in members:
        o, h = ours_m.get(f"{st}.{mem}"), host_m.get(f"{st}.{mem}")
        if o is None:
            fails.append(f"struct {st}: 缺 POSIX 必需成员 {mem}"); continue
        if o != h:
            fails.append(f"struct {st}.{mem}: 本仓 {o} B, POSIX {h} B")

# ============================================================ 汇总
if fails:
    print(f"FAIL: iface/posix 与 POSIX 不一致({len(fails)} 条):", file=sys.stderr)
    for f_ in fails:
        print("      " + f_, file=sys.stderr)
    if ALLOW:
        print("      (白名单里的有意偏离: " + ", ".join(ALLOW) + ")", file=sys.stderr)
    sys.exit(1)

print(f"ok   POSIX 一致性: 类型 {len(TYPES)} 项 / 常量 {len(names)-len(ALLOW)} 项 / "
      f"签名 {checked} 项 / 结构体 {sum(len(v) for v in STRUCT_MEMBERS.values())} 个成员")
for k, why in ALLOW.items():
    print(f"ok   有意偏离(已登记): {k} —— {why[:70]}…")
print("PASS: iface/posix 与 POSIX 一致(类型宽度 / 常量值 / 函数签名 / 结构体成员)")
PY
