# 0001: 工具根名 `br` → `brickie`

> 状态: **已接受** | 影响面: **toolchain**(现行文档的 CLI 引用 + 工具家族命名); **不影响** OS 符号/宏/段前缀(`br_` / `BR_` / `.br_*`)
> 格式依据: `1-02` §2.2(决策记录格式与阈值表: 动机 / 设计 / 替代方案 / 兼容性影响)
> 关闭: `intro` 第 8 行的待办「br tool(考虑重命名)」

## 1. 动机

1. **一名三用**。`br` 同时是 OS 符号/宏/段前缀(`br_` / `BR_` / `.br_*`, D16)、用户可见 CLI 产品名、以及工具家族根名(`br-core` / `br-gen` / …)。OS 契约前缀与宿主工具产品名是两件事, 同串会持续诱发口径漂移(文档里 `br` 到底指哪个, 只能靠上下文判断)。
2. **`br` 在真实 shell 里已被占用**。broot 官方就推荐把 `br` 定义成 shell 函数([Canop/broot#375](https://github.com/Canop/broot/issues/375)), 且 `br` 是 brotli 的流格式名 —— 这是可复现的踩脚, 不是审美问题。
3. **现在改名最便宜**。工具**零代码**(`prototype-v0.1.0/tools/` 只有 `check-workarounds.sh`; 原型 README 把"插件化 / `br`"列为 v0.1 刻意不做), 设计侧只是一次文档 sweep; 拖到 v0.1 成文后成本立即上一个量级。

## 2. 决策

- **工具根名 = `brickie`**(英/澳俚语"砌砖工")。与 brickOS 的"积木"隐喻同源且互相解释: `brickOS` 的积木 ↔ `brickie` 砌它的人(`brickie build --release` 读起来就是"用 brickie 造 brickOS 产品")。
- CLI = `brickie`; 家族 = `<根名>-<角色>`: `brickie-core` / `brickie-gen` / `brickie-model` / `brickie-solve` / `brickie-ver` / `brickie-iface`; 锁定文件 = `brickie.lock`。
- **工具名与 OS 符号前缀解耦**: `br_` / `BR_` / `.br_*` **不变**(D16 的 OS 命名部分继续有效)。

## 3. 候选与否决(留痕)

评判口径取自设计自身两条纪律: (a) 名字不能**主动邀请它做不到的事**(`r1/06` §3.1); (b) 名字要罩住**全命令面**(生成 → 校验 → 构建 → 跑 → 门禁), 不能只罩 v0.1。

| 候选 | 读作 | 评价 |
|---|---|---|
| **`brickie`** ✅ | 砌砖工 | 覆盖面最广(工匠造物, 15 条命令全自洽); 与 brickOS 直系同源; 初筛未见同名软件。**代价 = 语域偏非正式**(正式语域版 `bricklayer` 10 字符过长) |
| `trowel` | 瓦刀(砌工手工具) | 同源, 但只罩"生成/铺"一环, 覆盖面偏窄; 已有同名 repo 与课程项目 |
| `jig` | 工装夹具 | 3 字符最好打, 语义直指幂等/可复现纪律; 但**不属砖域**, 且已有同名 Go cmd 与 PyPI 包 |
| `stud` | 积木凸点(咬合) | 与 D16 隐喻逐字同源; 但一词多义(螺柱), 语义偏"连接点"而非"装配者" |
| `hod` | 灰斗/搬砖工(真实工种) | 短; 但太冷僻, 不传达语义 |
| `wright` | 匠人(`-wright`) | 语义通, 但与砖域无联系, 且撞姓氏 |
| ❌ `scaffold` / `scaff` | 脚手架 | **"脚手架"在软件里已特指代码骨架生成器** —— 只罩 15 条命令里的 1 条(`brickie new`); 且建筑语义是**临时支撑、封顶即拆**, 与"工具链是持久资产"(`2-01` §1)相反。这正是 `r1/06` §3.1 判 `frozen_version` 时那条纪律的同型错配: **命名与语义相反, 属于必须改的那类问题** |
| ❌ `mason` | 泥瓦匠 | 撞 Dart/Flutter 的 **Mason CLI**([mason_cli](https://github.com/munawerdev/mason_cli)); 而它恰好也是脚手架工具、模板术语就叫 **"bricks"**([flutter-bricks](https://github.com/ml-opensource/flutter-bricks)) ⇒ **语义与生态双重撞车** |
| ❌ `forge` / `anvil` / `keystone` / `vault` / `loom` / `toybox` | — | 重撞名(Foundry `forge`、OpenStack Keystone、HashiCorp Vault、Project Loom、BusyBox 系 toybox) |
| ❌ `mortar` | 砂浆 | 是**介质**不是工具; 语义偏"链接"而非"组合 + 门禁" |
| ◐ `bond` | 砖的**砌合方式** + 契约 binding | 双关很妙(砌合规则 ≡ 组合规则), 但通用英文词, 歧义/撞名风险最高 |

**注**: 上表是**初筛**; 按 D16 自身的要求, 正式冻结前仍须做一次完整检索 + 商标核对。

## 4. 改名映射(已落地)

| 位置 | 旧 | 新 |
|---|---|---|
| CLI | `br` | **`brickie`** |
| 工具家族二进制/库 | `br-core` / `br-gen` / `br-model` / `br-solve` / `br-ver` / `br-iface` | `brickie-core` / `brickie-gen` / … |
| C++ 生成器库 | `libbrgen` | `libbrickie-gen` |
| Python 包 / 入口点 | `python/br/`、入口 `br` | `python/brickie/`、入口 `brickie` |
| 锁定文件 | `br.lock` | `brickie.lock` |
| 工具状态目录候选(`2-02` Q5) | `.br/` | `.brickie/`(Q5 已关闭为 `build/`, 此处仅为口径一致) |
| 工具自身目录 / 文档旁支目录 | `br-tools/` | `brickie/` |
| 设计文档 | `docs/2-toolchain/2-02-br-arch.md`、`docs/2-toolchain/br-tools/br-tool-v0.1.md` | `docs/2-toolchain/2-02-brickie-arch.md`、`docs/2-toolchain/brickie/brickie-v0.1.md` |

**落地范围**: 现行设计文档 14 篇 + `intro`(共 176 行)已 sweep; 另手工修订 `1-01`(D16 行 + 命名段)、两份 `编号约定` 注记、`docs/README.md` 索引与 ADR 指引。

## 5. 明确不改的项(防止"顺手全改"造成契约损伤)

| 项 | 理由 |
|---|---|
| `br_` / `BR_` / `.br_*`(OS 符号 / 宏 / 链接段) | D16 的 OS 命名部分; 全库契约 + 原型代码已用(实测 836 / 223 / 18 处) |
| OS 级 `br-*` golden 组名(`br-sched` / `br-mem` / `br-mm` / `br-irq` / `br-svc` / `br-devcore` / `br-cdevcore` / `br-vfscore` / `br-bdevcore` / `br-posix` / `br-crypto` / `br-keyring` / `br-pic`) | 是 **API / golden 组名**, 不是工具家族 |
| 决策号 `BR-D*` / `BRV-D*` / `BRV-Q*` 与错误码 `BRV-<域>-NNNN` | **已分配标识符**: 跨文档引用 + 错误码将进 CI/测试。重编号收益为零、风险实打实。若日后要统一前缀, 单独立一条 ADR |
| `comment/**`(评审存档: `Design/comment/**` 与 `docs/2-toolchain/brickie/comment/**`) | 历史存档按仓库规矩**原样保留旧名**(见 `comment/README.md` 的"历史存档, 正文原样保留旧名与旧前缀"); 旧名对照由本 ADR 承担。**唯一例外**: 存档内指向被改名正文的**相对链接指针**已修复(`../../../br-tool-v0.1.md` → `../../../brickie-v0.1.md`, 3 处), 链接**文字**仍保留旧路径(叙述不变, 只修指针) |
| `br-wa-*`(WORKAROUND 欠债编号)与 `prototype-v0.1.0/**` | 属 `brickOS-prototype-v0.1.0` 分支; `br-wa-*` 是 OS 级欠债编号, 不改(该分支的 CLI 引用见 §7) |

## 6. 顺带拍板的三个子决定(可低成本复议)

1. **`brickie new` 继续叫"插件作者脚手架"** —— 与 `4-01` §7 / `4-02` §2 第 6 项的现有口径一致; "脚手架"一词各归其位(**能力名 ≠ 工具名**)。
2. **不留 `br` 短别名** —— 一个产品名一个真值。要留, 必须写进本 ADR 并限定"仅交互式使用; CI 与文档一律用正名", 否则就是第二套口径(正是 `comment/README.md` 记的"元契约漂移"病根)。
3. **工具自身目录用 `brickie/`**(而非 `brickie-tools/`) —— 家族前缀 `brickie-*` 已表明工具链身份, 避免同义重复。

## 7. 后续(未完成项)

1. **`brickOS-prototype-v0.1.0` 分支 sweep**: 该分支 README / `WORKAROUNDS.md` 中"尚未就绪的 `br` 工具"引用需一并改为 `brickie`(`br-wa-*` id 与 `br_*` 符号不动)。本次只动了 `brickOS-Design` 分支。
2. **正式冻结前做完整检索 + 商标核对**(D16 自身要求)。
3. `2-02` 拍板成文时, 把工具名与"工具名 vs 符号前缀解耦"写进其术语/命名小节。
4. 附带发现(不属本 ADR, 建议单独小修): `1-01` 第 4 行的 v0.11 变更说明在上一轮 TangramOS → brickOS 全局替换中被改成了自指形式(`br_` → `br_`、`BR_` → `BR_`、`.br_*` → `.br_*`、`br-*` → `br-*`), 已失去原意 —— 原意可据 `comment/README.md` 第 3 行复原为 `tg_` → `br_` 等。
