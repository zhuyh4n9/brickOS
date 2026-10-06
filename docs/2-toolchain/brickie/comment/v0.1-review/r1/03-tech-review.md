# br-tool v0.1 评审 · r1/03 — 技术正确性与需求覆盖(独立评审)

> 评审对象: [`docs/2-toolchain/br-tools/br-tool-v0.1.md`](../../../brickie-v0.1.md)(草案, 基线 commit `ad8990a`)
> 评审人: **独立 subagent**(只读, 未与作者共享上下文) —— 视角 = 技术正确性 / 可实现性 / 需求覆盖
> 复核方式: 作者(原型)对每条 P0 做了独立回原文复核, 结论标在 `复核` 字段。
> 结论: **P0 × 8 / P1 × 15 / P2 × 11**。本评审**推翻作者自查的一条"通过项"**(相位单调规则零误杀), 并给出 4 个真实插件安放失败的反例。

## 总评(评审原文)

**成熟度: 骨架完整、语义未闭合——不能直接当 v0.1 实现规格用。**

§0–§2 + BRV-D1–D10 把"v0.1 边界"钉得很死(这点比多数设计稿自觉), §7.5 的纯函数求解器、§6.6 的 `truth/hash_scope` 迁移开关、§3.5 的三不变量方向都对。但**三处硬伤使它在意"可实现"这一栏不及格**:

1. **§5.2 + §6.5 自相矛盾到能一眼证伪**: 每次 `publish` 都 `d+1`, 而 `publish` 又写 `plugin.toml` ⇒ 同一未改动的树连续两次 publish 得 `1.0.0.1 → 1.0.0.2`, 与 §6.5 L470 / V-5 L738 宣称的"重复执行幂等"直接冲突。
2. **§6.2 的 hash 没有文法, §6.1/§8.1 也没有 `entries` 的 TOML 形状** —— 而 `iface_hash` 是整个 v0.1 交付物的身份、`a` 段的推导输入。`macro`/`var`/`service` 条目在 object model 里没有 sig/layout 字段 ⇒ `#define BR_MAX 16→4096`、`service/crypto` 新增 ops 函数都**算出同一 hash、变更集 NONE**。
3. **真实插件在这个分类学下无处安放**: `svc-posix`(runtime_adapter)必须依赖 `service/lwip`(third_party)却被 §3.1 禁则杀死; `iface-pkcs11` 要两个 provider 而 `reexport_of` 是单数; `dev-core → vfs-core` 类型依赖在 `kind = init|runtime` 里无家可归; sqlite 的 `db` 能力成为**无人能合法声明的汇点**。

## P0 设计缺陷

### P0-1 `publish` 非幂等, 与 §6.5/V-5 矛盾; 存在 version 反馈环

| | 内容 |
|---|---|
| 位置 | §5.2 `NONE`(L346)、"重发同面"(L353)、§6.5(L470)、V-5(L738) |
| 反例 | t0 快照 `1.0.0.0/H0`; t1 publish(零源码改动)→ diff=NONE → `d+1` → `1.0.0.1`, 写快照 + `[compat].iface_hash`; t2 再 publish → `1.0.0.2`… **每次调用都涨**。若 diff 把单元 `version` 当输入(§6.1 L394 把 `version` 列为单元字段, §6.2 规则 6 只排除 `status`)→ t2 落入 CHANGED ⇒ `a+1,b+1` ⇒ `2.1.0.0`。**CI 每次运行都红** |
| 复核 | ✅ **成立**, 与作者自查 P0-3 是同一问题的两条独立发现路径(`r1/01` 从"`d` 段无自动信号源"切入, 本评审从"version 反馈环"切入) |
| 建议 | 定义**面内容幂等**: `canonical(new surface)==snapshot.iface_hash` ∧ entries/status 全等 ⇒ 空操作(不涨 d、不写盘、exit 0, `--check` 绿); `d+1` 只留给显式 `--republish`。显式规定 diff 与 hash **忽略** `version`/`hash`/`status`。`--set` 必须 ≥ 当前版本 |

### P0-2 hash 输入无文法; `entries` 无 TOML 形状; `service` 契约不进 hash; `typedef` 表不存在

| | 内容 |
|---|---|
| 位置 | §6.1(L386–398)、§6.2(L404–413)、§8.1(L604–613) |
| 反例 | ①`kind` 含 `macro`/`var`/`service`/`symbol-family`, 却只给 `func` 定义 `sig`、`type` 定义 `layout` ⇒ `#define BR_MAX 16→4096` hash 完全相同, publish 报 `NONE`; ②`form="service"` 的"ops 契约"无字段 ⇒ **`service/crypto` 契约变更不进 hash**(`1-01` D25 的 v1.x 头号交付完全不受治理); ③规则 4 依赖的 `typedef` 表在 §6.1/§8.1/§8.2 不存在(与作者自查 P1-1 同); ④`--strict-params` 是 CLI flag 不是快照字段 ⇒ 跨环境 hash 不同; ⑤"规范化签名/布局摘要"只有"空白归一", 没有语法 |
| 复核 | ✅ **成立**。② 是本评审的**独有高价值发现**: 作者自查只发现"`typedef` 表缺失", 未发现 `macro`/`service` 两类条目**根本没有参与 hash 的字段** |
| 建议 | 给出 canonical text 的逐字节文法 + `hash_rev`; object model 增 `macro.value`/`var.type`/`service.ops`/`typedef` 表; `strict_params` 存进快照; hash 做域分隔; §8.1 的 `entries = []` 展开为 `[[export.entries]]` |

### P0-3 真实插件无法安放(四例, 本评审最有价值的发现)

| # | 反例 | 复核 |
|---|---|---|
| ① | **`svc-posix`(runtime_adapter) → `service/lwip`(third_party)**: §3.1 规定 runtime_adapter"**只允许**依赖 native" ⇒ 红。但 `4-02` §5 明证 lwip 是"携带上游源码(lwip/sqlite)"的**三方插件**, `11-01` §2.1/2.2 与 `1-01` §7.4/§7.5 都要求 svc-posix 的 socket 路由 lwip。反向(`lwip→svc-posix`)合法, 但反向运行期调用只能注册表"不声明", **直接推翻 §3.1 L117"把 `4-04` §3 的答案锁在必须声明上"** | ✅ **成立**。已核 `11-01` 行 40/41(socket 路由 lwip)与 `4-02` §5。另与 `r1/02` 的"不确定项"**双向确认** |
| ② | **`iface-pkcs11`**: PKCS#11 域标准面不属 `native\|runtime_adapter` 任何一值(`10-01` §1"任何域标准 API 都是一个插件"在二值枚举下无解); `reexport_of` 单数装不下 `crypto`+`keyring` 两个 provider | ✅ **成立** |
| ③ | **`dev-core → vfs-core`(仅头文件类型依赖, `8-01` §1.3/O-S7)**: `kind` 只有 `init\|runtime`, 写 runtime ⇒ 按 §7.1"runtime 参与闭包"把 vfs-core 拖进"形态 B", **推翻 D19/O-S7** | ✅ **成立** |
| ④ | **`service/sqlite` 的 `db` 能力**按 `1-01` §7.6 该被"APP/其他服务按名字用", 但 native 不得依赖 third_party、app 又只能填 native ⇒ **只能靠不声明消费** | ✅ **成立**, 与作者自查 P0-4 同源(本评审补出"`kind` 无 `type` 值"这一半) |
| **建议** | 增 `kind="type"`; `reexport_of` 改列表; 接口分类加第三值 `domain`; 明确定义"谁可以依赖 third_party", 否则删掉 `1-01` §7.6 的消费叙事 | — |

### P0-4 四段号退化: `a` 与 `b` 恒同步, 且 M0–M2 期 `a ≡ 0`

| | 内容 |
|---|---|
| 位置 | §5.2(L344–353)、§3.2(L125)、`1-02` D15(L239) |
| 反例 | 只有 `CHANGED(含 frozen)` 与 `REMOVED(含 frozen)` 两行(且两行结果完全相同)触及 `a`; `b` 也只在这两行动 ⇒ 恒有 `a ≡ b-1`。**更严重**: `1-02` D15"M3 前不冻结任何东西"而 v0.1 在 M0/M1 之间(§14 L815)⇒ **`a` 永远不可能合法 +1** ⇒ `^0.1.0.0 ≡ ~0.1.0.0 ≡ *`, §7.4 在 v0.1 生命周期内**不可证伪** |
| 复核 | ✅ **成立, 且比作者自查更进一步**。作者自查已发现 `a≡b`(枚举 165 个可达版本, `b` 恒为 0); 本评审补出**第二层**: 由于 D15 在 M3 前不冻结, v0.1 期间 `a` 恒为初值 ⇒ `^`/`~`/`*` 三者等价, V-4/V-7 只能用**伪造的 `a=1` fixture** 才测得出 |
| 建议 | 二选一: ①`a` 改为提供者显式声明的"接口代"(不由 diff 自动推导), `b` 承担破坏性变更; ②退回三段 + `iface_gen` |

### P0-5 §8 schema 缺 `sched_class` 与 per-plugin RAM/栈, §2 却宣称"全量/声明面可查"

✅ **成立**(与 `r1/02` P0-5 **完全独立命中同一问题** —— 两份独立评审的交叉确认, 可视为定论)。详见 [`r1/02`](02-cross-doc-review.md#p0-5-2-覆盖度表声称可查的两项在其定稿-schema-里没有输入字段)。

### P0-6 §3.4 memory 模型无法表达真实内存面; ops 笛卡尔积无正交约束; P2/P4 自相矛盾

| | 内容 |
|---|---|
| 位置 | §3.4(L159–183)、`3-01` §6/§7(L161–208)、`3-04` §1–§2 |
| 反例 | ①三池是 **heap/contig/页池**(`3-01` L161/L191), 而 `regions = heap\|dma\|mmio\|reserved` **没有 contig/page** ⇒ `br_mem_alloc_contig`(L171)无法声明; ②`ops` 缺 `unmap`, 而 `br_mm_unmap` 是 v1 定稿(L205); ③`granularity="pool"` 无池创建 ops, 而 §3.4 表格首句就写 P2 含"池创建"; ④`byte+map`、`alloc+mmio` 可声明无约束; ⑤**§8.1 示例自身越权**: `ability` 在 P2 声明 `ops=["map","protect"]`, 而 `3-01` L289 把 `br_mm_region_add` 限定 platform |
| 复核 | ✅ **成立**。已核 `3-01` L161"TLSF 堆 + 连续池 + 页池"、L171 `br_mem_alloc_contig`、L205 `br_mm_unmap` |
| 建议 | regions 改为"platform region 表实例 + 池(heap/contig/page)"两轴; ops 补 `unmap`/池生命周期; 写 (ops × granularity) 正交性表; `map/protect` 归 P4; `[privileged.memory]` 只管非默认面, `br_malloc/free` 归 P0 隐式 |

### P0-7 插件名自相矛盾 ⇒ 闭包求解找不到提供者

| | 内容 |
|---|---|
| 位置 | §8.1 L571(`ability/crypto`)vs §8.2 L648(`service/crypto`)vs §8.3 L665–666 |
| 反例 | §8.3 的 namespace 枚举 **不含 `ability`**, §8.4 又按 `service/`→service 推导 subkind ⇒ `ability/crypto` 既不在合法集也无法推导 subkind; 而 `9-02`/`11-01` 用 `service/crypto`/`crypto` ⇒ `br check` 报依赖缺失 |
| 复核 | ✅ **成立**。已核 §8.3 枚举 = `app\|iface\|platform\|sched\|framework\|io\|fs\|service`, 确实不含 `ability` |
| 建议 | 统一为 `service/crypto`, 或把 `ability` 加入 namespace 并给"namespace ≠ plugin_type"映射表 |

### P0-8 Python 无 TOML 写能力, §9.3 白名单禁止引入

| | 内容 |
|---|---|
| 位置 | §9.1 L695/L702、§9.3 L724、§6.5 L466、§7.6 L553 |
| 反例 | stdlib 只有只读 `tomllib`(作者已用 `python3 -c` 验证: **无 writer**); 手写 writer 会**丢掉人写注释、重排键序**(§8.1 的 `plugin.toml` 是"人写"真值)⇒ 每次 publish 整文件 diff, **V-2/V-5 的"无 diff/逐字节一致"失效** |
| 复核 | ✅ **成立**, 且是**贯穿性**的: §6.5 与 §7.6 共 4 个命令都要写 `plugin.toml`, 全部受影响 |
| 建议 | 允许注释保留型 TOML 编辑库(如 `toml_edit`/`tomlkit`), 或规定机器只写 `build/gen/` 与 `api/`, 人写文件仅输出补丁 |

## P1 需要收口(15 条摘要)

| # | 问题 | 复核 |
|---|---|---|
| **P1-1** | **相位单调规则与四相模型不兼容, 误杀合法组合且可反向漏杀** —— `[plugin].phase` 单值, 但每个插件有**两个完成点**(所有插件都有 `early_init`; 非 Service/Interface 的 `init` 在 CORE)。`sched-coop` 须在 EARLY 完成注册(`1-01` §6.2), 而 region 表由 platform `early_init` 声明(`3-01` L289)⇒ platform 填 `core` 时 `phase(B)=core > phase(A)=early` **误杀**; 填 `early` 则 CORE 插件依赖它也过 ⇒ **假阴性** | ✅ **成立, 并推翻作者自查通过项**。作者自查的压测把 platform 假设为 `phase="early"`, 故"零误杀"是**假设的产物**而非结论 —— 已在 `r1/01` 更正 |
| P1-2 | BRV-DEP-0009 与 0010 是同一次检查的两个码(`[[dep]].phase` 缺省 = phase(A)) | ✅ 成立 |
| P1-3 | dependents 只到**插件级**; 需求要"依赖该**接口**(单元)的插件", 而 `requires_iface` v0.1 不扫描 | ✅ 成立(与作者自查 P0-4 的"依赖面"缺口互补) |
| P1-4 | `c` 段定义被自己矩阵推翻(5/8 行涨 c 且不都是新增); `REMOVED(仅 experimental)` 与 `ADDED` 结果**完全一致**; 变更集是五类**集合**而矩阵按单一类别编排, 组合输入无规则 | ✅ 成立 |
| P1-5 | `REMOVED(frozen)` 硬门要"满足弃用周期", 而 §6.4 自认 v0.1 只能 ◐ 统计当前使用者 ⇒ **硬门不可实现** | ✅ 成立(与 `r1/02` P0-2 同源) |
| P1-6 | V-9("无 cc/cargo/nm 跑**完整**测试套件")与 §9.2 的 Cargo/CMake 骨架**互斥**; C++ 模板引擎未定 ⇒ V-2 跨环境不可复现 | ✅ 成立 |
| P1-7 | "Python 禁止业务规则"**不可验证**(`br-core --selftest` 不能证明 Python 没重实现), §10 无对应验收项 | ✅ 成立 |
| P1-8 | 两个预编译二进制的 wheel 矩阵/manylinux 基线/PATH vs 包内解析/**协议版本握手**全部未设计; RV-5 只是事后打印指纹 | ✅ 成立 |
| P1-9 | §3.3"subkind 不参与数量约束与依赖方向"与 §3.2"`subkind=scheduler` **恰 1**"、§7.3"`subkind` 特例"**直接矛盾** | ✅ 成立(已核两处原文) |
| P1-10 | `[compat].iface_hash` 单数 vs `[[export]]` 可多个; `[plugin].version` 与 `[[export]].version` **双真值**; `iface_ref`/`export.hash`/`compat.iface_hash` **三份 hash 无同步规则** | ✅ 成立 |
| P1-11 | `[privileged].level` 语义未定(上限 vs 精确)⇒ §8.1 示例(P2 + irq 资源)**自身矛盾**; 无"声明内容 ⊆ level"检查; **PRIV 域零错误码** | ✅ 成立 |
| P1-12 | **JSON Schema 与引擎双检**: 若 schema 表达了不变量 1, Python 以 exit 2 先拦 ⇒ Rust 的 `BRV-TAX-0016` **永不打印**, **V-12 按写的无法通过** | ✅ 成立(高价值) |
| P1-13 | 版本 arity 不一致(`">=1.0.0"` 3 段 / `"0.1.0.0"` 4 段 / `1-01` §7.6 `">=1.0"` 2 段)无归一化; `mode="exact-hash"` 与"hash 不参与比较"矛盾; **VER 域零错误码** ⇒ V-7 无码可断 | ✅ 成立 |
| P1-14 | V-7 的"版本冲突"**不可构造**: 单版本政策 + 一处 `plugin.toml` ⇒ 求解器永远看不到第二候选, "冲突"实为"缺失" | ✅ 成立 |
| P1-15 | V-10 第三句是**伪验收**(文档检查); V-11 不检查两处 `truth` 相等; V-12 缺 `iface-posix→svc-posix` 必须绿的**正例**(过度执法仍能过); §10 无特权/预算验收项 | ✅ 成立 |

## P2 改进建议(11 条摘要)

`[build]` glob×lang 无交叉检查(P2-1) · 未禁"`form != skin` 却写 `reexport_of`"且无码(P2-2) · `api_iface` 100% 冗余(P2-3) · 规则 7 未定枚举**输出序**⇒ 重排源码不被感知(P2-4) · `lang="cxx"/"rust"` 合法却无"not-yet-supported"码(P2-5) · `iface_changed` 进 lock 属**类别错误**(P2-6) · RV-8 自检无验收项(P2-7) · 需求"操作**哪些**内存"只到类型级、无 region 实例(P2-8) · §2 悄悄纳入四项未请求的校验(P2-9) · 不变量 2 的"**闭包内**"表述错误(该属性只看自身 manifest, P2-10) · V-3 未规定多环时"完整环路径"的规范表示(P2-11)。

## 需求覆盖核对

| # | 需求 | 覆盖状态 | 备注 |
|---|---|---|---|
| 1 | v0.1 仅做四件; 其余排 v0.x | **完整覆盖**(边界需一句收口) | 五件后续能力逐条落到 v0.2–v0.6 ✅。**但** §2 把"资源预算 + IRQ/DMA 独占 + 相位单调 + 分类学禁则 + 特权声明"作为 ✅/◐ 纳入 v0.1, 这四项既不在需求四件里, 也不在"v0.x 后续"清单里(P2-9) |
| 2 | `va.b.c.d`; `a` = 已有接口是否变更 | **语义偏离**(需需求方确认) | **两种解读**: ①**字面** —— 若 `a` 是计数器则 experimental 改动也涨(与 L357 冲突); 若 `a` 是 0/1 标志位则**破坏单调性与字典序比较**; ②**草案解读** —— `a` = 冻结面兼容代数, "本次是否变更"移到 `iface_changed` 布尔。草案选②, 代价是**由版本号看不出 experimental 接口被改/删**(只见 `c+1`, 与纯新增同形)⇒ **需需求方签字** |
| 3 | 发布能回答: 变更/删除/新增/自动版本+hash/三态/**依赖该接口的插件** | **部分覆盖** | 两处缺口: ①"依赖**该接口(单元)**的插件"v0.1 只能答**插件级**; ②`REMOVED(frozen)` 所需弃用周期 v0.1 算不出 ⇒ 硬门不可执行 |
| 4 | TOML, 与 Rust 生态一致 | **完整覆盖**(写路径有洞) | 选型与三条纪律清楚; 洞在 `tomllib` 只读(P0-8) |
| 5 | 两维分类(api_type 三值 / plugin_type 四值含 scheduler) | **完整覆盖**(内部矛盾) | 与需求逐字对应 ✅; 矛盾见 P1-9; A-2 回灌清单**漏了 `9-02` §7.2/§10 与 `8-01` §1.2/§1.3** |
| 6 | cxx + rust + python3, Python3 粘合 | **完整覆盖**(纪律不可验证) | 分层与 JSON over stdio 清楚; "Python 禁止业务规则"不可验证(P1-7) |
| 7 | manifest 核心信息(7 子项) | **部分覆盖** | 缺口: ①memory 三轴中"**操作哪些内存**"只到类型级、无 region 实例; "使用哪些 **memory 接口类型**"被读成 `ops`(操作轴)而非 **API 族**(`br_malloc`/`br_mem_alloc_contig`/`br_dma_alloc`/`br_mm_*`)⇒ **两种解读都要写**; ②三池表达不出(P0-6); ③缺 `sched_class`/per-plugin RAM(P0-5); ④"必须包含版本文件"若指 manifest 内需**引用**该文件则缺字段 |
| 8 | 导出面分类由 `api_type` 决定; third_party 不抛接口 | **完整覆盖**(旗舰判例不闭合) | 三不变量+三码+V-12 正反用例, 是全篇落地最实的一条 ✅。不闭合: 二值枚举命名不了 `iface-pkcs11`、`reexport_of` 单数装不下两个 provider; 三方件消费叙事与禁则互斥(P0-3) |
| 9 | 交付定位 = prototype v1.0 核心交付产物 | **完整覆盖** | 与 `1-03` §5 第 4/5 项、M0/M1 对齐明确 |

## 真实插件逐一安放结果(节选, 含全部失败项)

| 插件 | 安放 | 问题 |
|---|---|---|
| `sched-coop → platform/qemu-aarch64` | ❌ | 相位单调误杀(P1-1) |
| `dev-core → vfs-core` / `cdev-core → vfs-core` | ❌ | 无 `kind="type"`; §8.2 `allow_edges` 示例还漏了这条(P0-3③) |
| `vfs-core → dev-core` | ❌ | D21 已撤销, 但 §7.3 把 `ability→ability` 一律判 ✅, **无法表达单向** |
| **`svc-posix → service/lwip`** | ❌ | §3.1 runtime_adapter"只允许依赖 native" vs `4-02` §5 + `11-01` §2.1/2.2(P0-3①, 最高价值) |
| **`iface-pkcs11`** | ❌ | PKCS#11 在二值枚举里无法命名; `reexport_of` 单数装不下 crypto+keyring(P0-3②) |
| **`service/sqlite`** | ❌ | native 不得依赖 third_party、app 只能填 native ⇒ `1-01` §7.6 的消费叙事只能靠不声明(P0-3④) |
| `service/crypto` | ⚠️ | 名字三套 ⇒ 闭包解析失败(P0-7) |
| `platform/*`、`bdev-core`、`fs/tmpfs`/`devfs`/`littlefs`、`io/*`、`service/keyring`/`hsm-host`/`seclog`/`dbg-bridge`、`iface-posix`(skin 豁免✅)、`iface-min`、`app/*` | ✅ | 可安放 |

## 通过项(评审确认设计得好的地方)

- **§BRV-D10 + §3.5**: 把"接口分类"从独立维度降为不变量(C11), 给出三条**可执法**不变量、三个错误码、V-12 正反用例; `third_party` 无导出面的取舍理由与代价(RV-9)都讲清。
- **§2**: 把"v0.1 到底能做多少校验"逐项映射到 `1-01` §6.4 六项, 显式标 ✅/◐/✗ 并写进 CLI 帮助文本 —— 设计文档里少见的边界自觉。
- **§7.5**: 求解器为**纯函数**、环报完整边路径结构化输出 —— 精确承接 C4 与 `1-03` §3 的 M0 验收。
- **§BRV-D5**: 状态目录表把"生成物/索引不入库、lock 与 `api/iface/**` 入库"分开并给理由(lock 是决策不是派生物)。
- **§5.1 + §6.6**: 把 hash 定义为 build metadata(不参与比较), 并用 `truth`/`hash_scope` 把真值迁移变成**一次可验收的开关** —— 方向正确。
- **§6.3 + §6.5**: 变更集 + 影响报告 + `--note` 硬门, 把 `1-02` §2.2 的 PR 门钩机械化, 是需求 3 的实质落地。

## 评审建议的落地顺序

1. 先拍 **P0-1 / P0-2 / P0-7**(否则实现即返工);
2. 再用 **P0-3 的四个反例**重新推一遍 §3.1/§3.5/§7.3 的边规则;
3. 最后按 **P1-12 / P1-13** 收口 schema 与错误码矩阵。
