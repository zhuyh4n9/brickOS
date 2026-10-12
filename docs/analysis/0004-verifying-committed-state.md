# 0004 — 怎么验证"**已提交的内容**"(隔离工作树)

> 起因: 本仓有两方并行改动(一方做挂载归 APP / POSIX 一致性, 一方做 fat/mmc 的 vendor 迁移)。
> 主工作树里"跑一遍门禁全绿"**不再等于**"已提交的内容是好的" —— 因为别人的在途改动就躺在旁边,
> 它们会顺手把缺失的文件补齐、把过期的值盖掉。本文记下**验证已提交状态**的办法与三个坑。

## 1. 为什么主工作树的"绿"会骗人

实例(本轮真实发生): `HEAD` 缺两个文件 —— `third_party/uboot-imx/include/rtc_def.h`
(fat 的适配壳要 include 它)与 `tools/check-adapter-copies.py`(门禁已注册但脚本没提交)。
主工作树里它们就躺在磁盘上(别人的未提交产物), 所以：

- `brickie build` **绿**;
- `brickie test` **绿**;
- 而 `git worktree add HEAD` 出来的干净树 **编不过**。

⇒ "在工作树里跑绿了"只证明**工作树**自洽, 不证明**提交**自洽。

## 2. 办法: 隔离工作树 + 软链派生依赖

```bash
cd <repo>
git worktree add --detach <repo>/build/verify HEAD
ln -s <repo>/prebuilts/toolchain <repo>/build/verify/prebuilts/toolchain   # 不进库的派生依赖
ln -s <repo>/.toolchain           <repo>/build/verify/.toolchain           # 见 §3.1
cd <repo>/build/verify
make tools && make tools-core          # 先造出 build/host/**(见 §3.3)
./prebuilts/seed/brickie/x86-64/linux/bin/brickie build --root .
./prebuilts/seed/brickie/x86-64/linux/bin/brickie test  --root .
```

`build/verify` 落在 `build/` 下 ⇒ 被 `.gitignore` 覆盖, 不影响主树, 也不进库;
验证完 `git worktree remove --force build/verify` 收拾掉。

**这一招本轮抓到三处 HEAD 不一致**, 都是主工作树里看不出来的:

| # | 不一致 | 主树为什么看不出来 |
|---|---|---|
| 1 | 缺 `third_party/uboot-imx/include/rtc_def.h` ⇒ **fat 编不过** | 该文件在工作树里存在(未提交) |
| 2 | `tests/gates.toml` 注册了 `check-adapter-copies`, 而**脚本未提交** ⇒ 门禁必红 | 脚本在工作树里存在(未提交) |
| 3 | `prebuilts/README.md` 的指纹表是**旧值**(种子已重发) | 工作树里 README 也被改过, 看起来"有人会修" |

## 3. 三个坑(都不明显, 记下来省一次踩)

### 3.1 工具链发现只往上找**一层** ⇒ 工作树里要自己软链

`tools/brickie/Makefile` 的选择序是 `<repo>/.toolchain` → `<repo>/../.toolchain`。
`<repo>/build/verify` 两处都没有(真身在 `<repo>/.toolchain`, 隔了三层) ⇒ 会退到 PATH 上
apt 的 cargo 1.75 并**明确报错**(那正是我们上一刀加的判据, 这里救了场)。
⇒ 在工作树根软链一份 `.toolchain` 即可(纯环境, 不是仓库改动)。

### 3.2 种子的**逐字节**比对依赖构建路径 ⇒ 别在工作树里下结论

`brickie-core` 是 Rust; crate 路径**会被编进二进制**。同一个 commit 在
`prototype/` 与 `build/verify/` 两个目录编出来的 `brickie-core` **相差 16 字节**
(路径长度不同), 于是 `make tools-prebuilt-check` 在工作树里报"种子落后"。
⇒ **这不是缺陷**, 是路径依赖; 种子的逐字节判据只能在**规范构建路径**(主检出)里跑。
测量:`3068976`(verify) vs `3068960`(prototype)。

### 3.3 `check-build` 门禁有**顺序前提**: 先 `make tools` + `make core`

它干跑 `make tools` 的递归行, 判据之一是"打包命令把 `brickie-core` 嵌进入口 ELF"。
若 `build/host/**/bin/brickie-core` 还不存在, freeze 不会带 `--embed`, 门禁判红 ——
**红的是"还没编 core", 不是 HEAD 有问题**。⇒ 先 `make tools-core` 再跑门禁。
(同理: `make tools` 若在 `tools-core` 之前跑, 入口 ELF 里没嵌 core ⇒ 与种子不一致。)

## 4. 一条给并行开发的纪律

**重发自举种子的人, 必须在同一次改动里刷新 `prebuilts/README.md` §5.1 的指纹表。**
种子与它的指纹是两个必须同步的产物; 分开改就会出现"文档说 A、库里的字节是 B"(本轮第 3 条
正是这么来的)。同理, 往 `tests/gates.toml` 注册门禁时, **脚本本体要同一次提交** ——
否则 HEAD 上那条门禁必红, 而主树里看不出来(本轮第 2 条)。
