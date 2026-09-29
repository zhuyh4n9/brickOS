# 01-architecture 模块评审意见（文档表述质量）

> 评审对象: `tangramOS/docs/1-architecture/00-architecture.md`(695 行) / `01-api-contract-governance.md`(242 行) / `02-roadmap.md`(197 行)
> 评审方式: 分模块 subagent 评审 + 主评审全文交叉复核（每条均已对照原文行号核实）
> 评审重点: 胡言乱语/技术性错误/前后矛盾/表达错误/格式引用
> 严重度: 高 = 误导设计决策或明显胡言乱语; 中 = 易误导读者或明显表达问题; 低 = 错别字/格式瑕疵
> 行号说明: 评审期间主文档 §3 架构图被并行重排（687→695 行, §3 之后行号整体 +8）; 本文主文档行号已按 695 行现行版本校准, 01/02 两篇未变动。

## 00-architecture.md（主文档）

### 中

- **L56** [前后矛盾] D19 决策行「派生: **vfs-core→dev-core**(设备路由+适配), svc-posix→vfs-core」
  - D19 行仍保留该依赖方向，与 D21（L58「**vfs-core 纯化**(设备依赖移出)」）、§4.5（L240「纯 VFS」）、§7.2 依赖图（L436 `CDEVC2 --> VFSC2`，方向恰好相反）直接矛盾。佐证: 06-device.md SD-12 已用删除线标注「~~vfs-core 依赖 dev-core + cdev-core~~ → SD-13/D21 修订: 撤销」，但主文档 D19 行未像 D11 那样补修订标注。同病: 04-vfs.md L109（该处定级「高」，见 `04-vfs-device-comment.md`）。
  - 建议: D19 推论列补注「→ D21 修订: 该依赖已撤销，设备路由改经 devfs/cdev-core 侧」。
- **L391** [前后矛盾] §7.1 表「能力注册表(设备/挂载/服务表) | 机制: 插件公地 | **core**(§4.5)」
  - §4.5（L240「设备/挂载注册表已拆为框架件插件」）与 D19「core 收缩为 native API + 服务注册表」均已把设备/挂载注册表移出 core，仅服务表留在 core；本行所引的 §4.5 恰恰否定了本行。
  - 建议: 该行改为「服务表 → core；设备注册表 → dev-core；挂载表 → vfs-core（框架件）」。
- **L311/L315** [前后矛盾] 「SAFE_PREEMPT … 在**任意**调度器下正确」 vs 「tt ⇒ 全部 TT_SAFE 且周期元数据闭合」
  - 若 SAFE_PREEMPT 在任意调度器下正确，tt 组合不应强制全部 TT_SAFE；若 tt 还需周期/WCET 元数据，则「任意」表述过头——两条规则互斥。
  - 建议: 改为「tt ⇒ SAFE_PREEMPT 或 TT_SAFE（且周期元数据闭合）」，或收紧 L303 为「任意抢占/协作型调度器下正确」。
- **L269** [格式引用] `uint32_t sched_class; /* PREEMPT | COOP | TT (§5.3) */`
  - §5.3 定义的是插件侧兼容类别 SAFE_PREEMPT/COOP_ONLY/TT_SAFE，并非 PREEMPT/COOP/TT（那是 §5.2 表的列值）；且 `sched_class` **同名两义**（调度器 ops L269 vs 插件描述符 L326），交叉引用指向了错误的概念。
  - 建议: 注释改引 §5.2，并考虑将调度器侧字段改名（如 `sched_kind`）以消除同名歧义。
- **L559–562** [技术错误] §9 启动序列「platform.early_init … 恒等映射页表(tg_mm) / 早期 console」→「core.init **BSS 清零** · …」
  - BSS 清零被排在已运行 C 代码、已构造恒等映射页表、已使用早期 console **之后**: 若页表/console 的运行状态位于 .bss，会被后置清零破坏；且 C 代码本不应在 BSS 未清零时运行（L349 Platform 行含「reset 汇编」，惯常做法正是在那里清 BSS）。
  - 建议: 将 BSS 清零移入 reset/platform 早期（汇编）步骤，并从 core.init 描述中删除。
- **L331–333/L343/L564–575** [前后矛盾] 描述符三回调与启动序列的映射缺失
  - 描述符三回调（early_init/init/start）与 §6.2 四阶段（EARLY/CORE/LATE/APP）的映射全文未说明；§9 序列在「全局开中断」后直接 `app.start()`，**没有任何插件 start() 被调用的步骤**；§9 又用「init」同时指称 CORE（L568）与 LATE（L571）两阶段，回调名与阶段名错位。
  - 建议: 在 §6.2 给出回调↔阶段映射表，并在 §9「全局开中断」与「app.start()」之间补「各插件 start()」步骤。
- **L659/L666** [前后矛盾] 「## 14. **三域映射**」
  - 表内仅仪表 dashboard、HSM 两行，L658 自述「**两域**在插件层几乎不共享」；02-roadmap L197 亦称「三域示例」——标题与内容自相矛盾。
  - 建议: 标题改「两域映射」或补第三域，并同步修改 02 L197。
- **L408/L431** [前后矛盾] §7.2 依赖图「[iface-min / iface-pkcs11 …] as IF_O」+「IF_O --> SX」
  - iface-min 是「极薄别名层, 直通 native API」（L474）、依赖仅 core（02-roadmap L67），图中却与 iface-pkcs11 共享同一条指向 lwIP/crypto/trace 的依赖边；该边只对 pkcs11 成立。
  - 建议: 将 iface-min 移出该节点，或为节点内两类插件分别画边。
- **L512** [前后矛盾]（主评审补充）§7.6 模式 B 行「依赖面: 纯 native(**tg_fs**)」
  - `tg_fs` 这个 API 前缀全库不存在: 08 §0 明确 native API 不含文件/挂载（`tg_open`/`tg_file_*` 归 **vfs-core 框架件契约**，非 native）。模式 B 的依赖面表述引用了不存在的 API 族。
  - 建议: 改「纯 native + vfs-core 框架件契约（`tg_open`/`tg_file_*`）」。
- **L331 vs L561–563** [前后矛盾]（主评审补充，跨文档组同报）§6.1 `early_init`「**无堆**/无线程/关中断」 vs §9 `core.init`（含**堆**）先于 EARLY 执行
  - EARLY 回调运行时堆已由 core.init 建立，「无堆」与 §9 时序矛盾（11-memory L32 因此写出了无出处的「PRE_ARCH」）。→ 统一（改 §6.1 为「不使用堆」，或调整 §9 表述）。详见 `06-cross-doc-consistency-comment.md` B8。

### 低

- **L3** [格式引用] 「所有图为 PlantUML(```plantuml 块; …」——行内三反引号与后文单反引号不配对，Markdown 渲染会出现杂散 ```、内联代码断裂。→ 改「PlantUML（```plantuml 代码块…）」。
- **L364** [表达错误] 「IRQ/**外带**独占冲突检测」——「外带」疑为「外设」（或 DMA）之误，与 L322 res 字段「RAM/栈/IRQ/DMA」对不上。→ 更正为「IRQ/DMA 独占冲突检测」。
- **L75** [技术错误] 「ARM Cortex-M/R | 恒特权态 | 不切 PSP 非特权模式」——Cortex-M 上栈指针选择（CONTROL.SPSEL/PSP）与特权态（CONTROL.nPRIV）**正交**: 使用 PSP 不等于进入非特权模式，把「切 PSP」当「非特权模式」属概念混用。→ 改「不进入非特权线程模式（CONTROL.nPRIV 保持 0）」。
- **L72–77 vs L46** [前后矛盾·存疑] §2.2 表题「目标 ISA」仍含「RISC-V | M-mode」行，而 D9 已定 ISA=aarch64 且「riscv 不作卫生检查(评审裁剪)」本身表达晦涩。→ 表头改「ISA 家族映射（现役: aarch64）」，消除与 D9 的表面冲突。
- **L680–688** [格式引用] 风险清单呈现顺序为 R1, R2, **R7, R8, R9, R3**, R4, R5, R6——R7–R9 被插入 R3 之前，编号乱序。→ 按 R1–R9 重排或重新编号。
- **L675** [技术错误] §15 比较表 FreeRTOS 列「特权级 | 单级」——FreeRTOS 提供 MPU 变体（FreeRTOS-MPU），支持特权/非特权任务分离，「单级」过于绝对。→ 改「单级（可选 MPU 分离）」。
- **L212** [表达错误·存疑] 「vx.0 | MPU 插件 | Cortex-M/R **或 A 核补充**」——Cortex-A 无 MPU（PMSA 属 M/R 体系），「A 核补充」指代不明，字面与硬件事实冲突。→ 改「Cortex-M/R（A 核继续用 MMU region）」。
- **L500** [表达错误] `TG_PLUGIN(sqlite, .deps = {"svc-posix@>=1.0"}, ...)`——与 §6.1 定义的 deps 形状（`const tg_dep_t *deps; /* {name, ">=1.0,<2.0", phase} 数组 */`）不一致: 示例是字符串，声明是结构体数组。→ 示例改为 `{{"svc-posix", ">=1.0", …}}` 形式。
- **L10** [格式引用]（主评审补充）「吸收评审意见(`../comment/00-architecture-comment.md`, 仓库外)」——相对路径从 docs/1-architecture/ 解析为 docs/comment/，不存在（实际在仓库外两层之上）。→ 改 `../../comment/…` 或写明说明性位置。

## 01-api-contract-governance.md

### 中

- **L22/L51 vs L126/L148/L182** [前后矛盾] experimental 变更流程两说
  - 「随时可碎, 不通知」（L22）/「experimental 区任意改动 | **无**」（L51 阈值表） vs 「EXPERIMENTAL --> EXPERIMENTAL : 随时改(**CA 记录**)」（L126）/「experimental 新增/改动 | **CA 记录**」（L182 产物清单）——同一文档对 experimental 变更是否需要留痕给出两种答案。
  - 建议: 统一口径——若 CA 记录只是轻量存档而非流程门槛，在 §2.1 与阈值表注明「无流程门槛，但留 CA 存档」。
- **L77/L126/L149/L184/L234** [前后矛盾] 三调度器矩阵与版本路线冲突
  - 「矩阵: {sched-preempt, sched-coop, sched-tt} × 同一套测试」＋升格前置「**conformance 矩阵全绿**」＋D15 落地约束「M3: 契约已被**三调度器**+多接口插件压测」——按路线图 sched-preempt=v2.0、sched-tt=v3.0，M3（=v1.0 完整化）时只有 sched-coop 存在，三调度器矩阵不可能全绿，D15 的论证前提在 M3 时点不成立（08 §15 同病，见 `02-os-core-comment.md`）。
  - 建议: L234 改「已被 sched-coop + 多接口插件压测，sched 相关组待三调度器齐备后升格」；L126/L149 的「矩阵全绿」限定为「已有调度器矩阵全绿」。
- **L191** [前后矛盾] 「标注了 `TG_API` 但 golden 无记录 = 红」
  - 与 L132「EXPERIMENTAL … 不在 golden(CI 不保护)」及 L113（experimental 符号同样有标注）冲突: experimental 符号「有标注且不在 golden」，按此规则必然全部判红，规则未按 FROZEN/EXPERIMENTAL 区分。
  - 建议: 改「标注 `TG_API_FROZEN`/`TG_API_DEPRECATED` 而 golden 无记录 = 红; `TG_API_EXPERIMENTAL` 不入 golden」。
- **L63** [前后矛盾]（跨文档组补充）golden 示例「`FUNC tg_task_create (const tg_task_attr*, tg_thread**)`」
  - 参数顺序（attr 在前）与 08 §2 L43 规格「`tg_task_create(tg_thread_t **t, const tg_task_attr_t *attr, …)`」（句柄在前）相反——golden 示例与权威清单矛盾。→ 以 08 为准修示例（详见 `06-cross-doc-consistency-comment.md` B5）。

### 低

- **L102** [前后矛盾] 「若 D14 答案为"是", 布局从 day 1 按硬契约对待」——文档头 L4 与 §4 L233 均已定稿 D14=Day1 二进制兼容，此处仍是未决口吻，属陈旧表述。→ 改「D14 已定 Day1，布局从 day 1 按硬契约对待」。
- **L78** [技术错误·存疑] 「coop 下过不了"抢占安全"用例 ⇒ SAFE_PREEMPT 声明为假」——coop 调度下不发生抢占，「抢占安全」用例无法在 coop 下真正行使，实质检验力在 sched-preempt 侧；句子暗示 coop 是执法主场。→ 改述为「在 sched-preempt 下过不了 ⇒ 声明为假（矩阵执法）」。
- **L63/L89/L238** [表达错误] 「`tg_thread`」/「`tg_thread_t*`」/「`typedef struct tg_thread tg_thread_t;`」——同一类型多种写法并存: golden 示例用 tg_thread、纪律条款用 tg_thread_t、08 用 tg_thread_t、主文档 §5.1 又通篇 tg_thread（typedef 后均合法，但文档应统一风格）。→ 统一为 `tg_thread_t`，golden 记录示例与主文档 §5.1 同步。
- **L148/L161/L182** [表达错误] 「CA 记录」——缩写全文未展开，仅 L115 指向 08 §14(CA-*)，读者必须跳转外部文档才能知道 CA 指什么。→ 首次出现处补注「CA 记录 = 08 §14 的契约决策记录(CA-*)」。
- **L3**（主评审补充）[表达错误] 「回答两个问题: 契约怎么变更(D12), 接口插件怎么声明与叠加(D13)」——标题与正文实际覆盖 D12–D15 四个决策（D14/D15 见 §2.5/§4），「两个问题」口径与内容不符。→ 补第三问或改为「契约治理三件事: 变更(D12)/叠加(D13)/兼容(D14–D15)」。
- **L17**（主评审补充）[格式引用] §2.1 状态图「experimental ──▶ frozen ──▶ deprecated ──▶ 移除」——遗漏 §2.6.2 L129 的「DEPRECATED→FROZEN（撤销弃用）」回边，线性图与状态机不一致。→ 状态图补回边或加注「撤销弃用见 §2.6.2」。
- **L118**（主评审补充）[表达错误] 「**单一事实来源是头文件 + 构建产物**, 文档与 golden 都是衍生物」——「单一事实来源」却是两个来源，自相矛盾（本意: 声明看头文件、符号真值看构建产物）。→ 改「声明的事实来源是头文件，符号真值是构建产物；文档与 golden 都是衍生物」。

## 02-roadmap.md

### 中

- **L11 vs L25** [技术错误] 「量产产品计划在 rCore / Unikraft 等**成熟 unikernel** 上二次开发」 vs L25「rCore 路径(Rust/RISC-V **教学核**)」
  - 同一文件对 rCore 的定性自相矛盾（成熟 vs 教学核）；且 rCore 是带用户态/多进程的教学系统，严格说并非 unikernel，「成熟」亦名不副实。
  - 建议: 改「rCore（教学核，原型验证）/ Unikraft（较成熟的 unikernel）」。
- **L81** [前后矛盾] v2.0 debug 行「mini ramdump + target 侧 memleak(arena 记账) + **红区/金丝雀**」
  - 红区/金丝雀归入 v2.0，而主文档 §11（L632）标「TLSF 红区+金丝雀(**v1.x**)」、03-debug L49 同——同一能力两处版本归属冲突。
  - 建议: 统一（以 v1.x 为准改 roadmap，或反向修订主文档与 03-debug，二选一）。

### 低

- **L80** [表达错误·存疑] 「EROFS(只读压缩**根**/资源分区)」——D21 与 v1.0 行已定 tmpfs 挂为 rootfs("/")，「压缩根」与 rootfs 归属矛盾或歧义。→ 改「只读压缩代码/资源分区（挂 /assets）」。
- **L41** [格式引用] 「(§7.6: 模式 A 直链 svc-posix 跑通…)」——§7.6 是主文档章节，本文件并无 §7.6，裸节号引用断链。→ 改「(主文档 §7.6: …)」。
- **L63 vs L171** [前后矛盾] 插件表「svc-posix … fd/stdio/**pthread** 子集 + libc stub」（M2） vs M2 里程碑行「svc-posix(fd/stdio 子集)」——pthread 是否属 M2 交付范围，两处描述不一致。→ 统一两处子集描述（或注明 pthread 后置）。
- **L60/L68** [前后矛盾] 「io/uart-pl011 … 依赖 cdev-core | **M0**(轮询)/M2(tty)」、app/hello「依赖 iface | **M0**」——其所依赖的 cdev-core/iface 插件均为 M2 交付，M0 时尚不存在；M0 轮询 console 实为 platform 早期 console（L51 platform 行已含 PL011）。→ 注明「M0 阶段仅作 console、不注册设备/不依赖 iface」。
- **L189** [表达错误] 「…§2.6(生命周期状态机/golden 管线/变更产物清单); **头文件草案待**」——句子截断（「待」后缺字）；且 DoD「产出形态」一格混入进度汇报（「已完成: …」），条目可验证性受损。→ 补全为「头文件草案待写」。
- **L3/L197** [格式引用] 「依据: `../comment/00-architecture-comment.md`(仓库外)」/「文档导览(**00–03**)」——前者按本文件位置解析为 tangramOS/docs/comment/…，实际在仓库外两层之上；后者 docs/1-architecture 仅 00–02 三篇，03 在 4-debug，指代不明。→ 相对路径改说明性位置；导览范围改「00–02」或写明 03 所指。
- **L43/L172 vs 主文档 L630** [前后矛盾·存疑] 「最小 debug bridge(memread / trace 流)」（v1.0 行）/M3「bridge 最小集」 vs 主文档 §11（L630）「**v1.x 最小集**」、03-debug §2「v1.x 起最小集」——路线图定为 v1.0(M3)，另两处标 v1.x，口径不一致。→ 统一版本归属（建议 v1.0(M3) 起最小集）。
- **L156**（主评审补充）[格式引用] 「iface-autosar-ish(主文档 §7.4 [?])」——§7.4 只列 iface-posix/iface-min/iface-pkcs11 加一条泛化注，无此插件；全库仅本行与 15-interface L15 出现该名。→ 改引 15-interface，或在主文档 §7.4 补域标准示例。

## 一致性核对通过项（供置信）

- **交叉引用核验**: 被引用的 08-core-api-list、6-vfs-device 四篇、03-debug、19-test 及全部 `docs/…md` 路径均存在，章节号绝大多数有效（失效处均已列入上表）。
- **用户手写笔记（`../00-architecture-comment.md`）全部决定已落实且无矛盾**: 恒等映射 v1.0/重定位 v2.0/MPU vx.0、bottom half v1.0、coop v1.0/preempt v2.0/tt v3.0、trace v1.0/mini ramdump v2.0/crypto v2.0、VFS/Block Device v1.0/EROFS v2.0/Page Cache vx.0、动态加载 v3.0+鉴权、core C v1.0→C+Rust v4.0、插件 Rust v2.0、SMP 单核 v1.0/多核 v2.0、aarch64、依赖与环管理、描述符版本管理——在 roadmap/主文档/各骨架篇中均已对应。
- D1–D23 决策表主体、§7.2 依赖宪法四规则、§8 三层模式、§10 存储栈图（与 06-device §1 逐行一致）、§15 对比表其余单元格，技术表述正确。

## 总体评价

该模块文档整体质量较高: 分层模型、依赖治理、契约冻结机制的主线逻辑自洽，**未发现胡言乱语或「高」级硬伤**，交叉引用的文件与章节号几乎全部有效。主要表述质量问题是「**改一半**」: D20–D23 演进后，主文档 D19 决策行、§7.1 归属表、§7.2 依赖图边未同步（06-device.md 已规范使用删除线标注修订，主文档未跟进）；治理文档存在 experimental 变更流程、golden 门禁规则（L191 误杀）、M3 冻结前置条件三处内部矛盾；主文档与路线图间还有红区/金丝雀、debug bridge 的版本归属冲突。建议做一次专门的「决策演进一致性」清稿（对齐 D19/§7.1/§7.2），并对主文档与路线图做一次版本归属对表。

**问题统计: 高 0 / 中 16 / 低 24，共 40 条**（subagent 原报 32 条: 高 0/中 13/低 19; 主评审补充 8 条: 00 的 L504、L324vs§9、L10, 01 的 L63 参数顺序、L3、L17、L118, 02 的 L156）。
