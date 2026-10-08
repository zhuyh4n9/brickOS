# brickie v0.1 验收矩阵(V-1…V-19)

> 用途: 把 `brickie-v0.1.md` §10 的 19 条验收项映射到**可执行用例**, 作为
> `tests/run.sh` 的施工图与回归基线。每行给出: 判据 / 用例形态 / 需要的 fixture /
> 观测点(退出码 + 码 + 文本)。
> 状态图例: ⬜未写 ｜ 🟡部分 ｜ ✅已覆盖(有可执行用例且实测全绿) ｜ ⏸不适用/待拍。
>
> **现状(实测)**: `bash tests/run.sh` = **通过 542 / 失败 0**(15s 级; 含 V-18 的 27 条)。
> 二进制解析口径与 `python/brickie/native.py` 一致: `build/host/<triple>/bin` 优先,
> 退到 `prebuilts/seed/brickie/<triple>/bin`; 用例显式注入 `BRICKIE_CORE`/`BRICKIE_GEN`。
> 快照基线在 `tests/snap/*.json`(N-4), 用 `make test-snap` 重生成。

## 0. 环境与夹具(fixtures)

夹具在 `tests/fx/<name>/`(**40 个**, 静态 TOML 树); 每条用例 `cp -r` 到 `mktemp -d`
的私有工作区再跑。`fx/quad`(V-1 的四类同树)不落静态夹具 —— 由用例即时用 4 次
`brickie new` 构造, 这样"生成器 + 校验器"是同一批产物, 不会各自漂移。

| 夹具 | 内容 | 用途 |
|---|---|---|
| `fx/empty/` | 单插件空树 | check 基线 |
| `fx/cycle/` | service/a→b→c→a init 环 | V-3 |
| `fx/phase-bad/` | 提供方 late 晚于依赖方 core | V-8(R1) |
| `fx/phase-assert/` | `[[dep]].phase=early` × 提供方 late | V-8(R2) |
| `fx/phase-ok/` | `sched/coop → platform/qemu-aarch64`(两边 core) | V-8 **不误杀正例** |
| `fx/tax-01-app-ability/` `tax-02-native-adapter/` `tax-03-iface-leaf/` | 三条硬禁则反例 | V-8 |
| `fx/tax-ok/` | app→interface→ability | V-8 正例 |
| `fx/export-tax16|17|18|19/` | 导出面不变量四种违例 | V-12 |
| `fx/thirdparty/` | `third_party` 无 export | V-12 正例 |
| `fx/skin-one/` `fx/skin-multi/` | skin 单/多提供者、分类相等 | V-12 正例 |
| `fx/sqlite/` | app→三方件 + adapter 仅 `kind=type` | V-12 正例②③ |
| `fx/req-iface/` `fx/req-iface-plain/` | 含/不含 `requires_iface` 的同一组合 | V-10 ①③ + V-14 + V-10② 对照 |
| `fx/ver-conflict/` | `requires_iface` 钉 `compat_gen=3` × 提供方 4 | V-7(`VER-0001`) |
| `fx/ver-range/` | 结构依赖 `range` 越界(提供方 major=1, 要 `>=2.0.0`) | V-7(`VER-0002`) |
| `fx/budget/` | `[[res]]` 求和 > `[budget]` | V-7 + V-19③ |
| `fx/res-conflict/` | 两插件同 IRQ + 同 DMA | V-19④ |
| `fx/priv-ability-p3/` `fx/priv-p2-map/` | 越级 / `map` 需 P4 | V-19①② |
| `fx/priv-pool-ok/` `fx/priv-platform-p4/` | 池生命周期 P2 / platform P4 正例 | V-19 正交表 |
| `fx/iface-macro|ops|enum|struct/` | 已冻结单元 + 可变条目 | V-15 / V-17 |
| `fx/unfreeze/` `fx/ver-matrix/` | 已冻结单元 | V-16 / V-4 |
| `fx/dependents/` | app→iface→crypto + `requires_iface` 消费方 | V-6 |
| `fx/publish/` | 单单元 | V-5 / V-11④ / N-3 |
| `fx/truth/` | 单单元(用例 sed 改 `hash_scope`/`truth`) | V-11② |
| `fx/ver-badstr/` `fx/ver-badrange/` `fx/ver-pad/` | 版本串/range 形态 | V-13 |
| `fx/showcase/` | platform+sched+service+interface+app+product 全绿组合 | 命令面全覆盖 + 快照 |

## 1. 逐条验收

| # | 判据(§10 原文摘要) | 用例(run.sh 段落) | 观测点 | 状态 |
|---|---|---|---|---|
| V-1 | `new` 四类 × `c` 各一套, 生成后立刻 `check` 0 错误 | `V-1` 段: 逐类 6 件产物 + `check --json`; 四类同树 | exit 0; `summary.errors==0`; 6/6 产物 | ✅ |
| V-2 | 重复 `gen` 无 diff; `--check` 逐字节一致 | `V-2` 段 | 二次 `gen` 报"已是最新" + 树指纹相同; `--check` exit 0; 篡改后 exit 1 且无码诊断 | ✅ |
| V-3 | 造环 ⇒ 报**完整环路径**(结构化边列表) + exit 1 | `V-3` 段(fx/cycle) | `check` exit 1 + 文本 `service/a → b → c → a`; `closure --json` `data.cycles[0].path/edges`; 码 `BRV-MF-0001`(见 §3) | ✅ |
| V-4 | 推进表逐行; (a)(c) 验 `COMPAT_GEN`/`MAJOR` 互不牵连 | `V-4` 段(fx/ver-matrix) | (a) `1.1.0.0`; (b) `1.1.1.0`; (c) `1.2.0.0`; (d) `1.2.1.0`; (e) `1.2.1.1`; (f) `0.1.0.0` 不动; `--rule compat_gen` ⇒ exit 2 | ✅ |
| V-5 | `publish` 产出快照+lock+CHANGELOG+影响报告; 面未变=空操作; `--check` 独立重算一致 | `V-5` 段(fx/publish) | 首次 `written=3`; 二次 `noop=true, written=0` 且逐字节不变; `status --check` exit 0; 改快照 hash ⇒ exit 1 | ✅ |
| V-6 | 改 frozen 条目时列全部直接/传递依赖者 + 标失配者; 缺 `--note` ⇒ exit 1 | `V-6` 段(fx/dependents) | `changes[0].kind=CHANGED`; `direct=2/transitive=3/unsatisfied=1` 且点名 `service/consumer`; 缺 `--note` ⇒ exit 1; `--note` 不存在 ⇒ exit 2 | ✅ |
| V-7 | 单版本闭包正确; `compat_gen` 冲突 `VER-0001`; `range` 越界 `VER-0002`; 预算超限红 | `V-7` 段(fx/ver-conflict, ver-range, showcase) | 两码各中一次 + exit 1; 闭包 5 插件、每条 init 边提供方在前、`totals` 与 `platform_capacity` 分列 | ✅ |
| V-8 | 三条硬禁则正/反; 相位单调反例; **`sched-coop → platform` 不误杀** | `V-8` 段(fx/tax-*, phase-*) | 三反例 exit 1 + 点名双方; `DEP-0009` / `DEP-0010` 各中一次且不串; 正例 exit 0 | ✅ |
| V-9 | 零编译依赖(**跑测试**不需要 cc/cargo/nm); `--json` schema 稳定 | `V-9` 段 + `N-4` | 精简 PATH(仅 python3/sh/coreutils)+ 自举种子**单文件**入口跑通 new→check→closure→publish→status→gen; 脚本内无 cargo/g++; 结构化快照 | ✅ |
| V-10 | `requires_iface` ① schema ② 不参与求解 ③ 只报 info | `V-10` 段(fx/req-iface) | ① 缺字段 ⇒ exit 2 + `MF-0001`(见 §3); ② 与 `req-iface-plain` 的 `closure --json` **逐字节相同**; ③ `iface show` 两条 info, span 点名 `compat.requires_iface[0]` | ✅ |
| V-11 | 快照头/`--json` 带 `hash_scope`/`truth`; 成对(出现 sym/header ⇒ 红); `NOT_ABI` 提示; `[[export]].hash` 与快照不等 ⇒ 红 | `V-11` 段(fx/truth, publish) | `decl/decl` 绿; `sym`/`header` 各 ⇒ `IFACE-0011` exit 1; 文本/JSON 都有 `NOT_ABI`; 改 hash ⇒ `IFACE-0012` exit 1 | ✅ |
| V-12 | 四种违例各有反例(`TAX-0016/17/18/19`), 三方件无 export 通过, **skin 三小项** | `V-12` 段(fx/export-*, thirdparty, skin-*, sqlite) | 四码各命中一次 + exit 1; 四个正例 exit 0 | ✅ |
| V-13 | `compat_gen` 缺失 ⇒ `0003`; 4 段版本串 ⇒ `0005`; 4 段 `range` ⇒ `0006`; 缺段右补 0; `--set` 回退 ⇒ `0007` | `V-13` 段(fx/ver-badstr, ver-badrange, ver-pad, publish) | `0005/0006` exit 2; `>=0.1` ≡ `>=0.1.0` 且规范化为 `>=0.1.0`; `--set 0.0.9.0` ⇒ `VER-0007` exit 1; **`VER-0003` 无构造点**(见 §3) | ✅ |
| V-14 | 同一输入两种 profile 两种结论 | `V-14` 段(fx/req-iface) | dev exit 0 + `VER-0004` **info**; release exit 1 + **error**; 两份快照 | ✅ |
| V-15 | append vs modify 条目级判定 | `V-15` 段(fx/iface-enum/struct/ops/macro) | `EXTENDED`(枚举末尾)绿、`ADDED`(新函数)绿且 `compat_gen` 不动; `CHANGED`(枚举重排/结构体加字段/ops 加槽)`verdict=red` 且未解冻 publish exit 1 | ✅ |
| V-16 | `unfreeze` 无 `--note` ⇒ 红; 窗口内 release ⇒ `IFACE-0009`; 空解冻 refreeze ⇒ 四段不动 | `V-16` 段(fx/unfreeze) | 无 `--note` ⇒ exit **2**(argparse 必填, 见 §3); 窗口内 release exit 1 + `IFACE-0009`、dev exit 0; 空解冻 ⇒ 版本串不变 | ✅ |
| V-17 | `BR_MAX 16→4096` 与 ops 加槽**必须**不同 hash; 枚举重排被 hash 感知 | `V-17` 段(fx/iface-*) | 三组 `declaration_hash` 均不同; 声明顺序不影响 hash; 同输入两次 publish 逐字节相同 | ✅ |
| V-18 | 粘合层纯度三条 | `tests/purity.sh`(run.sh 调) | (a) `--selftest` 在 `PATH=/nonexistent` 下 `ok 432 cases`; (b) 3 个夹具的"桩替换"诊断序列/数量/码集合与真实 core 完全一致; (c) 7 条禁止模式零命中 + 4 条正向对照 | ✅ |
| V-19 | 四条: ① P3/P4 或 `map` ⇒ `PRIV-0001` ② `P2` 声明 `map` ⇒ 红 ③ 预算超限红+差值 ④ IRQ/DMA 冲突红+双方 | `V-19` 段(fx/priv-*, budget, res-conflict) | ①② `PRIV-0001`; ③ 差值 188 KiB / 6 KiB; ④ 2 条诊断点名 `service/irq-a` 与 `service/irq-b`; 另两条正交表正例绿 | ✅ |

## 2. 反向验收(防跑偏)

| # | 判据 | 用例 | 状态 |
|---|---|---|---|
| N-1 | §5.4 明确不做的命令**一条都不在** `--help` 里 | `N-1` 段: 顶层命令面恰为 `check,dep,gen,iface,init,new,ver`; 逐名反查 10 个禁名; `iface`/`dep`/`ver` 子命令面锁定; `iface check` 不出现 | ✅ |
| N-2 | 未交付命令直接调用 ⇒ 退出码 2 | `N-2` 段: 10 个禁名 + `iface check` + 未知子命令 + 无子命令 | ✅ |
| N-3 | `brickie.lock` / `api/iface/**` 只由机器写; 两次 publish 幂等 | `N-3` 段 + `V-5` | ✅ |
| N-4 | 快照/诊断 JSON 快照测试(协议版本锁定) | `N-4` 段: `tests/snap/{version,check-clean,check-cycle,check-release-ver0004,dep-tree,dep-closure,iface-list,iface-show,iface-status,new-json,publish}.json` | ✅ |

## 3. 实现裁定 / 覆盖边界(判据读法, 不是缺陷)

下列几处"V-* 的字面措辞"与"实现裁定"不同; 实现侧的裁定都已在
`rust/README.md` §10 登记, 用例按**裁定**断言并在标签里写明:

| # | 字面判据 | 实测(裁定) | 依据 |
|---|---|---|---|
| 1 | V-3: 环报 `BRV-DEP-*` | 报 **`BRV-MF-0001`** + 完整边路径 | BRV-D8 未给环分配码, 裁定 R-9 借 `MF-0001`; `rust/README` §10 R-9 |
| 2 | V-13: `compat_gen` 缺失 ⇒ `VER-0003` | 报 **`BRV-MF-0001` + exit 2**(L5 schema 形状门), `VER-0003` 在 v0.1 **无构造点** | V-10① 明写"缺 `id`/`compat_gen`/`range` 即 `MF-*` 形状类"; 结构依赖不钉代(F3), 接口依赖只走 `requires_iface` 的形状门 |
| 3 | V-16: `unfreeze` 无 `--note` ⇒ **红** | CLI 把 `--note` 声明为 argparse 必填 ⇒ 退出码 **2**(用法错档) | BRV-D9: 2 = 用法或环境错; "阻断"成立, 但不是校验红 1 |
| 4 | V-6: 缺 `--note` ⇒ 退码 1 | 对 `publish`(命中"须解冻"的变更集)成立; `unfreeze` 走第 3 条 | §5.3.4 规则 1 / §6.5 表 |
| 5 | V-13: `--set` 低于当前 ⇒ `0007` | 需**面确实变了**才触发 —— 面未变时 `publish` 先短路成空操作(§6.5 幂等), 不对 `--set` 做校验 | §6.5 幂等 + §5.2 单调 |
| 6 | V-4: `COMPAT_GEN`/`MAJOR` 互不牵连 | (a) 只动 `COMPAT_GEN`; (c) 只动 `MAJOR`; 双向验证 | §5.2 |
| 7 | V-15: `SEMANTIC` | v0.1 **不产出** `SEMANTIC`(声明面不可观测纯语义变更) ⇒ 无对应用例 | 裁定 S-11 |
| 8 | V-15: 弃用周期"两个 minor 无使用" | v0.1 只做**声明面零使用**硬门, 符号面计数需 v0.2 ⇒ 未覆盖 | 裁定 S-16 |
| 9 | V-19: 资源/预算/调度冲突的专属码 | 借 `BRV-MF-0001`, 消息点名双方/差值 | 裁定 R-9 |
| 10 | V-7: 拓扑序"提供方在前" | 用例断言**不变量**"每条 init 边的提供方在前"(无 init 边的节点位次不受约束), 而非固定列表 | §7.6 / solver 实现 |

## 4. 实测发现的实现缺陷

> 口径: **缺陷** = 实现行为与设计/契约的书面要求不符(不含"测试期望写错");
> 测试不因它们放宽断言 —— 相关用例按裁定断言(见 §3), 缺陷在这里单独记录。
>
> **两条都已在合入前修复**(D-1 由 `prebuilts/` 属主重发种子、D-2 由 `brickie-core`
> 统一有效冻结态口径), 修复后 `make tools-test` = 542/0、`make tools-prebuilt-check`
> 三件全 ok。下面保留**原始复现与期望/实际**, 作为回归判据(谁再改回去, 这两条就会复现)。

### D-1 进库自举种子落后于源码 ⇒ 全新 checkout 的自举路径不可用

* 复现:
  ```sh
  cd prototype
  make tools-prebuilt-check          # 只读门禁
  # 或直接问种子 core:
  mkdir -p /tmp/t && cd /tmp/t && mkdir -p service/x && printf 'schema = 1\n[plugin]\nname = "service/x"\nplugin_type = "ability"\napi_type = "native"\nlang = "c"\nsubkind = "service"\nphase = "late"\nversion = "0.1.0.0"\n[compat]\ncore = ">=1.0.0"\nhash_scope = "decl"\ntruth = "decl"\n' > service/x/plugin.toml
  echo '{"protocol":1,"command":"check","root":"'"$PWD"'"}' \
    | /home/zhuyh/workspace/unikernel-dev/prototype/prebuilts/seed/brickie/x86-64/linux/bin/brickie-core
  ```
* 期望: `make tools-prebuilt-check` 全 ok; 种子三件 `{brickie,brickie-gen,brickie-core}` 与
  本次构建一致 ⇒ 无 `build/` 的 checkout 也能跑全命令面。
* 实际: `make tools-prebuilt-check` 报
  `FAIL: 自举种子落后于源码: prebuilts/seed/brickie/x86-64/linux/bin/brickie` 与
  `.../brickie-core`(只有 `brickie-gen` 一致); 种子 `brickie-core` 对 `check` 回
  `exit_code 2` + `error: - 命令 check 尚未实现(check.rs(agent-B): check)` ——
  它是**首刀**的构建(不含 `check`/`closure`/`iface-*`)。
* 影响: `prebuilts/seed/**` 是"没装 g++/cargo 的 checkout"的唯一自举路径 ⇒ 该路径上
  `brickie check` / `dep closure` / `iface *` 全部不可用; `V-9` 用例的"种子单文件入口"
  之所以仍能全绿, 是因为它按查找顺序**显式使用 `build/host/<triple>/bin` 的 core/gen**。
* 处置: **已修复** —— `make tools-prebuilt` 重发三件种子(与本次构建逐字节一致);
  现在在无 `build/` 的 checkout 上, 种子 `brickie` 单文件即可跑 `check`/`closure`/`iface *`。

### D-2 `iface show` 在解冻窗口里报告"声明面的冻结态", 与窗口状态/门禁不一致

* 复现:
  ```sh
  cp -r tools/brickie/tests/fx/unfreeze /tmp/fz && cd /tmp/fz && echo rfc > rfc.md
  export BRICKIE_CORE=…/build/host/x86-64/linux/bin/brickie-core BRICKIE_GEN=…/brickie-gen PYTHONPATH=…/python
  python3 -m brickie iface publish service/crypto#crypto
  python3 -m brickie iface unfreeze service/crypto#crypto --note rfc.md
  python3 -m brickie iface show service/crypto#crypto --json
  python3 -m brickie check --profile release
  grep freeze_state api/iface/service/crypto/crypto.toml
  ```
* 期望: `iface show` 与 `check`/`publish`/快照用**同一条有效冻结态**规则(声明 OR 快照),
  窗口内应显示 `unfreezing`(`rust/README` §10 S-19 已把该规则写成"publish 保留有效冻结态")。
* 实际: 磁盘快照 `freeze_state = "unfreezing"`, `check --profile release` 正确报
  `BRV-IFACE-0009`; 但 `iface show --json` 的 `data.unit.freeze_state = "frozen"`,
  `data.freeze_state` 缺失(文本表现为"冻结态: frozen")、`source = "snapshot+declaration"`
  ⇒ **读者会以为该单元不在窗口里**。
* 影响: 呈现层误导(不改变判定); `iface status` 只比 hash, 故 `consistent=true` 也不报。
* 处置: **已修复** —— `iface list`/`iface show` 改用与 `check`/`publish`/`ver show` 同一的
  `effective_freeze(声明, 快照)` 口径; 现在窗口内 `iface show --json` 给
  `data.freeze_state = "unfreezing"`、`data.unit.freeze_state = "unfreezing"`, 并新增
  `declaration_freeze_state` / `snapshot_freeze_state` 两个字段把"声明 vs 发布记录"显式分开
  (协议快照 `snap/iface-show.json` 已随之更新)。

## 5. 运行方式

```sh
cd prototype
make tools                     # 只编 L5/L2(宿主 g++, 不碰交叉工具链)
make tools-test                # 生成器自检 + tests/run.sh(含 V-18)
make -C tools/brickie test-purity   # 只跑 V-18 三条判据
make -C tools/brickie test-v        # 详细模式
make -C tools/brickie test-snap     # 重生成 tests/snap/*.json(更新协议基线)
```

`tests/run.sh` 的输入只有: `build/host/<triple>/bin/{brickie-core,brickie-gen}`(或种子)、
系统 `python3`、coreutils。**不需要** `cc`/`g++`/`cargo`/`nm` 在场。
