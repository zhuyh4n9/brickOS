# 架构 + 契约治理 + 路线图 设计评审(00 / 01 / 02)

> 评审人: subagent(架构模块, 行号经父 agent 抽查核实; 行号基于 695 行当前版——评审期间主文档 §3 图被用户并行重排过一次, 687→695 行, §3 之后的节有 ~8 行漂移, 节号不受影响)
> 评审对象: `tangramOS/docs/1-architecture/00-architecture.md`、`01-api-contract-governance.md`、`02-roadmap.md`; 交叉证据实读 `06-device.md`/`07-concrete-fs.md`/`08-core-api-list.md`/`13-toolchain.md`/`18-plugin-dev.md`/`19-test.md`/`04-vfs.md`
> 评审维度: 完备性(矛盾、技术点是否清晰、架构是否存在问题)
> 意见分档: P0 = 矛盾/架构缺陷(必须解决); P1 = 完备性缺口/技术不清晰(应当解决); P2 = 改进建议
> 范围说明: 本报告 24 条均为架构模块独立发现——段名/错误码两表/相位与 start 回调/sched_ops 槽位/trace 相关/CLI/组合校验清单/sched_class 双义/devfs 与 dev-core 依赖/-ENOTSUP 系列/标识符命名类已由其他模块评审覆盖(`../review1/` 与本系列 02–06), 不再重复

## 模块总评

三篇主文档的决策骨架——D1–D23 决策表、D12–D15 治理机器(三态/golden/三层门禁)、依赖宪法四规则与 DoD 清单——是全文档集中最扎实的部分, D18 双角色拆分与 §7.5 验证经济学论证链完整自洽。聚焦架构层独有问题后, 最大风险有三: (1) 总体分层图对 Platform 的依赖方向画错(Platform→Scheduler), 与 §6.3"最底层"、§8 三层模式、§9 启动序系统性错位, 且依赖宪法四规则缺少"插件类别"这个执法输入, 规则 1 的方向校验实际不可执行; (2) 运行期错误与域支撑面在 00/02 层无归宿——启动失败分支、§14 HSM/Cortex-R 目标域的隔离与内存模型支撑、A/B 与镜像契约均未设计; (3) 治理时序自洽性——M3 冻结的第一批(tg-mem/tg-mm/tg-irq)恰是 v2 语义冲击最重的组, R8 承认 v2 过载却未计入弃用周期负担, D18 引力中心的缓解措施是"写进风格规范"这类无执法机制的口号。

---

## [P0] Platform 在分层模型中的位置与依赖方向自相矛盾
- **位置**: `00-architecture.md` §3 图 L152–160 vs §6.3 L349、§8 L536–537、§9 L559–560; `18-plugin-dev.md` L25
- **问题**: 总图以实线箭头声明"层间单向流(§7.2)": `L3E(Platform) -down-> L2(Scheduler) -down-> L1(Core)`, 即 Platform 依赖调度插件。但 (1) §6.3 依赖方向列写 Platform 是"最底层"; (2) §8 三层模式中 Platform 是 core 机制接口(tg_mm/tg_pic)的实现者, 不消费调度器; (3) 按 §6.5 init 依赖语义("先 init 完我才能 init"), Platform 依赖调度器 ⇒ 须晚于调度器初始化, 与 §9(platform.early_init 最先、EARLY 才注册调度器)直接冲突; (4) 18-plugin-dev 路由表写 Platform"依赖: —"。§7.2 依赖宪法图(L399–447)不含 Platform/Scheduler/驱动, 四规则无法覆盖"core 机制由 Platform 实现并回调"这一核心关系——依赖宪法最大的结构性漏洞。
- **证据**: L159–160 "L3E -down-> L2 / L2 -down-> L1"; L349 "| Platform | reset 汇编…中断控制器实现… | 每 SoC 一个 | 最底层 |"; L537 "ISA_LIB --> CORE_IF : 实现并填表"; L559 ":platform.early_init"; `18-plugin-dev.md` L25 "Platform | PIC ops 表 / 早期 console / region / 链接脚本 | —"
- **建议**: 总图改为 L3E→L1 一条边(Platform 仅依赖 core 的 native/填表 API), 加注记"core 机制经 ops 表回调 Platform(运行期调用依赖, §6.5 允许)"; §6.3"最底层"改为与 §8 一致的表述; §7.2 补第 5 条规则或脚注, 明确"Platform 的 init 依赖指向 core、运行期经 ops 表被 core 反向调用"不违反单向流。

## [P0] 决策表 D19 残留已被 D21 撤销的依赖结论
- **位置**: `00-architecture.md` §1 L56(D19) vs L58(D21); `06-device.md` §1 L16
- **问题**: D19"主要推论"仍写"派生: vfs-core→dev-core(设备路由+适配)", 而 D21 已纯化 vfs-core(设备依赖移出、依赖面收缩到 core), 06-device §1 依赖表写 vfs-core 仅依赖 core。同一决策表两行结论相反; 单独读 D19 的实现者会造出错误依赖方向。06 的 SD-12 已用删除线记录该修订, 主文档决策表未同步。
- **证据**: L56 "派生: vfs-core→dev-core(设备路由+适配), svc-posix→vfs-core"; L58 "**vfs-core 纯化**(设备依赖移出, 依赖面收缩到 core)"; `06-device.md` L16 "core(纯 VFS, D21: 撤销设备路由)"
- **建议**: D19 推论改为"派生: cdev-core→dev-core + vfs-core(类型), svc-posix→vfs-core——D21 修订后 vfs-core 仅依赖 core", 加"(D21 修订)"标记; 决策表表头加一句"后继决策修订前代推论时, 前代行必须同步划改"。

## [P0] §7.3 "host 直通模式"与 D18 的"唯一实现 / fd 表唯一主人"冲突
- **位置**: `00-architecture.md` §7.3 L467 vs D18 L55、§7.4 L473、§7.2 规则 3 L455; §13 L653
- **问题**: D18 明确 open/read/pthread/socket"唯一实现"是 svc-posix、fd 表唯一主人, iface-posix 只是"再导出 + stdio/errno 接线"薄皮肤。§7.3 却允许"iface-posix 可直通宿主机 open()"——按字面实现, host 组合中出现第二份 open/read 实现与第二张 fd 表(宿主 libc 的), 直接违反规则 3(共享状态唯一主人)与 D13"不转移所有权"。该条是 v0.4(D18 之前)遗留, 未随 POSIX 实现服务化改写; 所引 `03-debug.md` §4 只讲 ASan 直通, 不支持"接口直通"。
- **证据**: L467 "**host 直通模式**: host 平台插件上 iface-posix 可直通宿主机 open()"; L55 "POSIX 实现=普通服务(open/read/pthread/socket 唯一实现, fd 表唯一主人…)"; L473 "**薄皮肤**: 再导出 svc-posix 符号(D13 reexport, 不转移所有权)"; L653 "tg test # host 平台插件 + 接口直通"
- **建议**: 把 host 直通下沉为 svc-posix 的 host 后端(svc-posix 在 platform/host 上把 tg_open 路由到宿主 open, fd 表仍归 svc-posix 单主人), iface-posix 保持纯再导出; §7.3 与 §13 措辞改为"svc-posix host 后端直通"。

## [P1] 启动失败路径无定义(init 回调失败后的系统行为)
- **位置**: `00-architecture.md` §9 L552–579、§6.1 L331–333; `02-roadmap.md` §5 L186–196; `12-plugin-mgr.md` §2 L23
- **问题**: 描述符三个生命周期回调均返回 int, 但三篇主文档未定义返回非零的后果: 中止启动? 跳过该插件? panic? 打印什么诊断? §9 只有 happy path; 02 §5 DoD 八项无此项; 全部相关表述只剩 12-plugin-mgr 大纲一句"错误路径(init 失败 = 启动失败)"且标"待成文"。对"新产品只做组合"的 OS, init 失败是组合错误的第一现场, 没有约定意味着产物是静默挂死(且无看门狗兜底)。
- **证据**: L331–333 "int (*early_init)(void); int (*init)(void); int (*start)(void);"; L563–569 ":plugin_manager 扫描 .tg_plugins → 拓扑排序(环 = 硬错误); :EARLY … :CORE 各插件 init(堆可用) …"(无失败分支); `12-plugin-mgr.md` L23 "错误路径(init 失败 = 启动失败)"
- **建议**: §9 增补失败分支: 任一回调非零 → 经早期 console/bridge 打印(插件名、相位、返回码)→ 停在可调试状态(WFI 循环 + panic 通道存活); "启动失败语义"列入 02 §5 DoD(建议归入第 7 项里程碑验收标准, M0 即验收"init 失败可观测")。

## [P1] D15/M3 冻结与 R8 v2 过载的互动: 第一批冻结组恰是 v2 语义冲击最重的组, 治理流程未设计
- **位置**: `08-core-api-list.md` §15 L373、§0 L18; `01-api-contract-governance.md` §2.2 L53、§2.6.5 L184–187; `19-test.md` R-6 L18; `00-architecture.md` §16 R8 L683、D15 L52
- **问题**: 08 §15 第一批(M3)冻结 tg-mem/tg-mm/tg-irq/tg-svc——但这四组恰是 v2 已预见的语义变更重灾区: v2 重定位使 R-6 不变量"dma_addr == vaddr(v1 恒等, CA-6)"失效(tg-mem 组语义变更); v2 实现 tg_mm_map(v1 为空实现, tg-mm 组语义变更); v2b SMP 使 tg_irq_lock 的全局互斥效力弱化(tg-irq 组语义变更, 08 §0 已把 SMP 原语列在面外)。按 01 §2.2 阈值表, frozen 语义变更 = "完整决策记录 + 弃用周期"——即 R8 已承认过载的 v2 将再叠加成批弃用周期的治理负担, 且 02 §4"一个能力只有进了哪个版本和没进"没有任何"冻结面跨主版本演化"的协议。该角度与已被覆盖的"tg-sched 冻结时点 ≥v3"不同: 这里说的是第一批(v1 即冻结)与 v2 变更波的碰撞。
- **证据**: L373 "**第一批(M3, 变化少的先冻)**: tg-mem / tg-mm / tg-irq(基础五件)/ tg-svc"; `19-test.md` L18 "**R-6** | `dma_addr == vaddr`(v1 恒等)"; `08` L363 "CA-6 …v1 恒等映射下 == vaddr | 为 v2 重定位预留结构形状"; `01` L53 "frozen 语义变更(阻塞行为/错误码/ISR 安全性) | 完整决策记录 + 弃用周期"; `00` L683 "**R8 v2.0 过载**…preempt+SMP+重定位+crypto+EROFS+ramdump+Rust 插件挤在一版"
- **建议**: 在 01 §2.6.5 增设"主版本边界演化协议": 允许 v1→v2 边界用一次批量决策记录覆盖一组已预告的语义变更(单一弃用周期), 并在冻结时于 golden 版本戳附带"v2 已知变更预告"(重定位→R-6 失效、SMP→irq_lock 语义、map 转正); R8 的分期说明中把"冻结面弃用波"计入 v2 工作量。

## [P1] 依赖宪法四规则的可执法性: 缺"插件类别"输入, 规则 1 方向校验与数量约束不可机械执行
- **位置**: `00-architecture.md` §6.1 L321–334、§7.2 L451–456、§6.3 L345–356、§6.4 L360–365
- **问题**: 规则 1 的执法机制写"组合期依赖图方向校验", 但方向校验需要知道每个插件属于哪一层/哪一类——描述符字段(name/ver/api_rev/sched_class/deps/abi_id/api_syms/res/三个回调)中**没有类别字段**, §6.3 的八类分类只是文档表格, 组合器无从判定"箭头是否指向更靠近 core"。同因, §6.3 三个数量约束(Scheduler 恰一、Platform 每 SoC 一个、APP 恰 1)也无法校验("零调度器/双 APP"组合静默通过构建期)。且 §6.4 六条组合期校验清单中**没有"方向校验"条目**——规则 1 的执法机制没有落进执法清单。规则 3 的"状态归属声明"同样无载体: 描述符/manifest 均无状态归属字段, api_syms 仅限 Interface 插件, svc-posix 等需要真导出符号的非接口插件(08 §13.3)没有任何符号面声明机制。
- **证据**: L453 "| 1 | **单向流**…接口插件是**严格叶子**(仅被 APP 依赖)… | 组合期依赖图方向校验 |"; L321–334 描述符字段表(无类别字段; L329 "api_syms …/* 仅 Interface 插件 */"); L360–365 六条校验(无方向校验、无数量校验); L455 "| 3 | **共享状态唯一主人**(fd 表=single owner) | D13 符号独占 + 状态归属声明 |"
- **建议**: 描述符增加 `uint8_t class`(TG_CLASS_PLATFORM/SCHED/FRAMEWORK/IO/FS/SERVICE/IFACE/APP, append-only 进 golden); §6.4 增加第 7 条"依赖方向校验(按类别层级表)+ 类别数量约束(Scheduler=1, Platform=1, APP=1)"; 为规则 3 在 manifest 定义 `owns_state`(共享状态归属声明)键, 非 Interface 插件的导出符号面(svc-posix)复用 api_syms 机制扩展到 Service 类。

## [P1] D10 双保险的"静态分析"半边没有机制: 无技术路线、无工具归属、无 DoD 项
- **位置**: `00-architecture.md` §1 D10 L47、§16 R1 L680; `06-device.md` §4 L185; `19-test.md` R-3 L15; `13-toolchain.md` §2 L19–26; `02-roadmap.md` §5 L186–196
- **问题**: D10 定为"双保险: 组合期校验 + 静态分析", R1 依赖它抓"sched_class 声明撒谎", 06 §4/19 R-3 又把 ISR 白名单执法挂到"静态扫描"——但全部文档没有一处定义这个静态分析器: 分析对象(源码/IR/链接期符号图)、技术路线(SAFE_PREEMPT 是语义性质, 需要 lockset/抢占点可达性分析, 并非显然可静态判定)、工具归属(13-toolchain 大纲 7 项与 02 §5 DoD 8 项均无静态分析条目)。双保险实际只有组合期一半有落点; "静态分析"在多份成文文档中被当作已存在机制引用。
- **证据**: L47 "| D10 | sched_class 校验严格度 | **双保险: 组合期校验 + 静态分析** | 对应 R1 |"; L680 "sched_class 声明可能撒谎——D10 双保险(组合期校验 + 静态分析)+ conformance 矩阵执法"; `06-device.md` L185 "…conformance 静态检查执法(D10/R1)"; `19-test.md` L15 "白名单外调用 = 静态扫描红 + debug 运行时断言"; `13-toolchain.md` §2 大纲(manifest/CLI/组合器/构建系统/golden 生成器/conformance 运行器/镜像产物——无静态分析)
- **建议**: 13-toolchain §2 增设"静态分析器"条目并列入 DoD: 先做可机械化的两项(ISR 白名单 = 调用图可达性分析、DMA/符号纪律 = pattern 扫描), SAFE_PREEMPT 声明暂降级为"conformance 矩阵执法 + 评审抽查"并记录在 D10 注记; 明确分析宿主(建议 IR 级, 与工具链同仓)。

## [P1] §7.5 复用经济学论证有洞, §7.6 引力中心风险的缓解措施不可执行
- **位置**: `00-architecture.md` §7.5 论证 2/3 L486–487、§7.6 代价 1 L519
- **问题**: (1) 论证 3 称"POSIX 编码的服务在不带 svc-posix 的组合中不可用——这是**组合期可见**的取舍(manifest 闭包)", 但"可见"没有工具支撑: manifest/`tg check` 不输出"引入该服务将拖入 svc-posix→vfs-core→(rootfs tmpfs)整栈"的闭包增量与 RAM 代价, 组合者看不到这笔账。(2) §7.6 代价 1 的缓解是"一方服务 native-first **写进风格规范**; svc-posix 在 manifest 显式可见"——风格规范不是门禁, 无 CI 规则、无度量指标(如一方服务对 svc-posix 符号的引用计数), "manifest 显式可见"对引力中心毫无约束力, 该风险实际无缓解。(3) 论证 2 的"换皮肤永不重验服务"未计入 D18 结构性新增的重验成本: svc-posix 现处于中间节点, native API 任一变更 → svc-posix 重验 → 其全部 POSIX 下游(sqlite 类)连锁重验——这笔账在"验证经济学"论证里缺席。
- **证据**: L487 "POSIX 编码的服务在**不带 svc-posix 的组合**中不可用——这是组合期可见的取舍(manifest 闭包), 不是运行期惊喜"; L519 "svc-posix 成为引力中心——中间件自然滑向全 POSIX 编码, native API 使用萎缩(Unikraft 教训)。缓解: 一方服务 native-first 写进风格规范; svc-posix 在 manifest 显式可见"; L486 "换皮肤永不重验服务"
- **建议**: (1) `tg check` 增加"闭包增量报告"(选定服务时打印拉入的插件链与 ΣRAM); (2) 把 native-first 变成 CI 度量: 一方服务的 svc-posix 符号引用计数门禁(阈值进 manifest), 超限需决策记录豁免; (3) §7.5 论证补第三笔账("svc-posix 中间节点的连锁重验成本")并在 R2 风险中点名。

## [P1] 挂载期设备可用性与 init 拓扑的缺口: 设备名绑定不产生 init-DAG 边(D21 挂载计划落地机制)
- **位置**: `07-concrete-fs.md` §6 L62、§4 L41、§2 L21; `00-architecture.md` §9 L567–569、D21 L58; `06-device.md` §1 L27–29; `02-roadmap.md` L70
- **问题**: littlefs 在 CORE 相位执行挂载并读介质(需 flash/bdev 设备已注册), 但 07 §6 列出的 init 依赖"{vfs-core, fs/tmpfs, cdev-core}"不含任何驱动; 驱动的依赖声明只有 cdev-core/bdev-core。设备绑定是运行期按名 lookup, 不产生 init-DAG 边; 02 L70 的闭包机制只覆盖"挂载计划含 '/' 或 '/dev' 即拉入 FS 插件", 未覆盖"设备名→驱动"。结果: 驱动与 littlefs 在 CORE 相位相对顺序不确定, "nor0 未注册→挂载失败"成为组合的随机事件(叠加启动失败路径未定义)。另: 挂载点生成存在双机制无优先级——tmpfs"预建目录列表"(07 §2)与"挂载点缺失自动 mkdir"(D21/07 §6)并存, 何者优先/冲突时谁赢未说明。(07 L62 漏列 bdev-core 一项已由 vfs-device 评审覆盖, 不重提。)
- **证据**: `07` L62 "init 顺序由 manifest 依赖声明保证: … fs/littlefs → {vfs-core, fs/tmpfs, cdev-core}"; L41 "manifest 配置: 挂载点(/data) / **设备名** / block_cycles"; `00` L567–569 ":CORE 各插件 init(堆可用) FS 插件执行挂载计划: tmpfs→/ · devfs→/dev · littlefs→/data"; `06-device.md` L27–29 "uart / can / adc / gpio / display 驱动 | cdev-core(…)"; `02` L70 "挂载计划含 \"/\" 或 \"/dev\" 即拉入"(无设备名条款); `07` L21 "预建 /dev /data /tmp 等目录由 manifest **预建目录列表**生成" vs L61 "挂载点在父 FS 缺失时自动 mkdir"
- **建议**: 在主文档 D21 或 12-plugin-mgr 定一条: (a) 组合器从挂载计划"设备名"反查提供方插件, 自动加入 init 依赖边(与"含 '/' 即拉入"同机制); 或 (b) 驱动一律在 EARLY 相位注册设备(early_init 静态注册、无堆), 写入相位约定。同时一句话定序: 预建目录列表先生效, 自动 mkdir 仅兜底。

## [P1] 正文 [?] 待定点过半无归宿(§17 只登记 D7/D8)
- **位置**: `00-architecture.md` §17 L690–695、§4 L176、§4.5 L260、§5.1 L291、§5.2 L303; `02-roadmap.md` §5 L186–196; `08-core-api-list.md` §6 L160
- **问题**: 主文档定义"[?] = 待定决策点", 但 §17 只登记 D7/D8。至少四个 [?] 无归宿: footprint(L176)、sbrk(L260)、LTO 特化(L291)、TT work 帧槽位(L303)——均不在 02 §5 DoD 八项内。其中 sbrk 在 08 §6 L160 已写成定论("sbrk 挂接点: svc-posix 的 libc stub → 本组"), 主文档仍挂 [?], 两文档状态不同步。
- **证据**: L690–695(§17 仅两行); L176 "…2–8KB RAM(不含堆与线程栈)[?]"; L260 "`sbrk` 指向 core 堆[?]"; L291 "ops 调用可经 LTO 特化[?]"; L303 "TT → 下一帧槽位[?]"; `08` L160 "sbrk 挂接点: svc-posix 的 libc stub → 本组(主文档 §4.5)"
- **建议**: 建立 [?] 登记纪律: 每个 [?] 必须出现在 §17 或某骨架文档的开放问题表并给归宿(版本/DoD 项); 本次先补四条——footprint→DoD 第 7 项附推导与测量口径; sbrk→落定并同步两文档; LTO 特化→19-test 性能基准项; TT 帧槽位→10-sched 大纲第 7 条。

## [P1] 镜像产物形态/启动加载契约/A/B 更新没有设计归宿
- **位置**: `00-architecture.md` §4.3 L211; `02-roadmap.md` §1 v2.0 L78、§5 L186–196; `13-toolchain.md` §2 L25
- **问题**: v2 重定位的动机被表述为"A/B 更新与 v3 动态模块布局的地基", 但 A/B 更新(双分区选择、bootloader 握手、回滚判据)在全部 21 篇文档中只出现这一句——不在 v2 域表、插件清单、未排期清单或 DoD 任何一处。镜像产物本身(ELF/bin、入口协议、QEMU 加载方式、flash 布局、产线烧录)也只存在于 13-toolchain 骨架大纲一句, DoD 第 5 项只覆盖"链接脚本、QEMU 脚本"。
- **证据**: L211 "镜像可加载到任意 VA; A/B 更新与 v3 动态模块布局的地基"; `02` L78 同句; §5 L186–196 八项 DoD 无镜像/启动项; `13-toolchain.md` L25 "7. 镜像产物: 布局、符号表、trace id 表(03)随镜像分发"
- **建议**: DoD 增补第 9 项"镜像与启动契约"(格式、入口、加载/烧录、trace id 伴生文件), 把 13-toolchain 大纲第 7 条升格为正式交付; A/B 若是 v2 重定位的真实动机, 在 02 v2.0 域表加一行并给设计归属, 否则从动机中删除。

## [P1] 热路径无任何量化性能/延迟预算(延迟面)
- **位置**: `00-architecture.md` §4 L176、§14 L663; `19-test.md` §6 L143; `04-vfs.md` §7 L161
- **问题**: 三份主文档对性能只有定性形容词("秒级启动""极便宜""零开销"), 延迟面零预算: IRQ→bh 延迟上限、上下文切换成本、tg_open 走查成本、trace 事件写入成本、ISR 最大驻留时间均无目标值。§14 把"仪表 HMI、帧渲染节奏"列为 APP 关注点, 却没有帧周期/尾延迟约束; R-S3"无 inode cache 的性能上限需 M3 基准数据"连判定标尺都没有; 19-test 把 benchmark 留为开放问题, 路线图/DoD 无性能验收项。
- **证据**: L176 "目标 footprint: ~4–16KB flash / 2–8KB RAM(不含堆与线程栈)[?]"; L663 "HMI、帧渲染节奏"; `19-test.md` L143 "性能基准(吞吐/延迟)是否并入本框架——倾向独立 benchmark 面, 与兼容性正交"; `04-vfs.md` L161 "深路径/大目录开销需 M3 基准数据, 再决定 v2 是否引入"
- **建议**: 在 00 或 02 增设"性能预算表": 每热路径一行(IRQ→bh、ctx switch、tg_open、sem_give、trace 事件、work 派发), 给出 v1.0 QEMU 参考值与目标区间; R-S3 的 M3 基准数据对照该表判红; benchmark 面在 19-test §6 落为确定交付(独立 benchmark 文档 + DoD 归属)。

## [P1] "严格叶子"与"接口叠接口(再导出)"许可矛盾——措辞修正之外仍有实质漏洞
- **位置**: `00-architecture.md` §7.2 L449 vs L170、L406、规则 1 L453; §6.3 L355
- **问题**: (区别于已被覆盖的"没有任何插件依赖 vs 仅被 APP 依赖"措辞问题: 即使改为"除 APP 外无任何插件依赖", 下列矛盾仍在。) §7.2 单向流语义明文许可"同类内部允许**接口叠接口(再导出)**"——iface-A 再导出 iface-B 的符号即形成 iface-A→iface-B 依赖边, B 的入边来自非 APP 插件, 严格叶子仍被打破; 且 D13"再导出不转移所有权"未覆盖 iface→iface 方向(两个薄皮肤谁拥有符号? 裁剪 B 时 A 的再导出如何处理?)。15-interface 大纲也没有这一场景的条目。
- **证据**: L449 "单向流语义: 箭头只许指向'更靠近 core'; **同类内部允许接口叠接口(再导出)**…"; L170 "**接口插件是严格叶子**: 系统中没有任何插件依赖它们 ⇒ 可替换性最大化"; L453 "接口插件是**严格叶子**(仅被 APP 依赖)"
- **建议**: 二选一成文: (a) 禁止 iface→iface 依赖("叠加"仅指多接口共存, 再导出只许指向 Service/core); 或 (b) 严格叶子定义为"入边只允许来自 APP 与其他 Interface", 并在 15-interface 大纲加"iface→iface 再导出的所有权与裁剪规则"条目。

## [P1] §14 三域映射的隔离与内存模型支撑缺口: HSM 无资产隔离故事, Cortex-R 无 MMU 路径
- **位置**: `00-architecture.md` §14 L663–664、§2.2 L74/L84、§4.3 L210–212; `02-roadmap.md` vx.0 L146
- **问题**: (1) HSM 行列出"crypto 服务、key 存储、host 接口、安全日志 | 密钥策略"——但 §2.2 明确"不进 EL2/EL3, 不用 TrustZone"、L84"野指针可破坏一切", MPU 插件是 vx.0 且未排期(`02` L146 标 [?]): 单地址空间全特权下, 密钥材料可被任何代码(含 APP 与任意出错驱动)直接读走, HSM 域的核心诉求(密钥资产保护)在 v1–v3 没有任何机制故事或边界声明(如"密钥仅经 crypto 服务 API 暴露 + 物理封装假设")。(2) 仪表行写"Cortex-A/R SoC"——Cortex-R 无 MMU, v1/v2 的整个内存契约(恒等映射页表、region 属性、重定位、tg_mm)对 Cortex-R 不适用, 替代路径(MPU 插件)无排期无接口草案; §2.2 的 Cortex-M/R 行只讲了运行级别, 未讲内存模型如何落地。
- **证据**: L664 "| HSM | 安全核 | sched-preempt(v2) | iface-posix + iface-pkcs11 [?] | crypto 服务、key 存储… | 密钥策略 |"; L74 "ARM Cortex-A | EL1 裸跑 | 不进 EL2/EL3, 不用 TrustZone 分世界"; L84 "无硬件隔离: 野指针可破坏一切"; §4.3 表(v1.0 恒等映射 MMU / v2.0 重定位 / vx.0 MPU 插件); `02` L146 "| MPU 插件 [?] | 待定(框架件?) | D4 分段元数据启用, 插件级内存保护 | core |"
- **建议**: §14 增加"域支撑矩阵"(每域所需能力 × 版本供给): HSM 行注明"资产隔离 = 无(原型期依赖物理封装/外置安全核; vx MPU 前不承诺)", 或补一段 HSM 资产访问边界设计; Cortex-R 在 §4.3 分期表加一行"v1/v2 内存契约仅适用 MMU 平台, Cortex-R 路径待 MPU 插件(vx, 未排期)", §14 仪表行的"Cortex-A/R"注明 R 族的支撑状态。

## [P1] §2.2 单异常级"红利/代价"论证不完备: 缓解清单夸大当前防护, 缺失代价未列
- **位置**: `00-architecture.md` §2.2 L83–85
- **问题**: 代价段写"野指针可破坏一切。缓解: MMU region 属性 + **插件分段(D4)** + 将来 MPU 插件"——D4 明文是"暂不做 MPU, 按插件分段**保留元数据**", 分段在 v1/v2 不提供任何实际保护力, 把它列为三个缓解之一夸大了当前防护; MMU region 属性只拦截"越出已映射区/权限违例"型错误, 不阻止同一可写 RAM 内的任意写(所有插件同池)。代价清单还缺三项: ①线程栈同居 core 堆无 guard(guard 推 vx), 栈溢出直接踩邻栈/邻数据; ②设备 DMA 写绕过 CPU 侧 MMU 语义, region 属性对 DMA 越界写不设防(仅靠驱动纪律); ③生产镜像(无 host ASan、target ASan vx 实验)期的故障遏制策略未定义(重启? 谁触发?)。
- **证据**: L84 "无硬件隔离: 野指针可破坏一切。缓解: MMU region 属性(v1 起恒等映射即有 RO/NX/device 保护, §4.3)+ 插件分段(D4)+ 将来 MPU 插件"; D4(L43) "**暂不做 MPU, 按插件分段保留元数据**"
- **建议**: 缓解清单改写为"v1 实际防护 = region 属性(仅限越界/权限型错误) + debug 期工具(host ASan/trace/ramdump)", 分段与 MPU 移入"未来(vx)"并注明当前为元数据; 代价清单补"线程栈无 guard 至 vx""DMA 越界写不受 MMU 约束(驱动纪律承担)""生产期故障遏制策略未定义"三条。

## [P2] §2.2 目标 ISA 表含 RISC-V 行, 与 D9 裁剪冲突
- **位置**: `00-architecture.md` §2.2 L76 vs §1 D9 L46
- **问题**: D9 定 aarch64 并写明"riscv 不作卫生检查(评审裁剪)", 但 §2.2 目标 ISA 表仍有 RISC-V M-mode 行(连 QEMU 参数都给出), 易被读作 RISC-V 在目标集; 02 §0 的 rCore 路径指迁移到别的内核, 非本 OS 支持 RISC-V。
- **证据**: L76 "| RISC-V | M-mode | `-bios none` 直入 M-mode |"; L46 "D9 | ISA | **aarch64** | …riscv 不作卫生检查(评审裁剪)"
- **建议**: RISC-V 行加注"(非当前目标, D9 裁剪; 本表仅说明单异常级在各 ISA 上的含义)"或删除; Cortex-M/R 行可保留(MPU 插件确实预留)。

## [P2] M2 交付的 svc-posix 范围与 M2 的 sqlite 验证目标不匹配
- **位置**: `02-roadmap.md` §3 L171 vs §1 L39; `00-architecture.md` §7.6 L514
- **问题**: M2 行写"svc-posix(fd/stdio 子集)", 而同一行验证目标是"sqlite 模式 A 移植跑通"; sqlite 模式 A 的 POSIX 需求明确含 pthread_mutex/pread/pwrite/usleep(L514), v1.0 域表的 svc-posix 范围也是"fd/VFS/stdio/pthread 子集"。按 M2 行字面交付则 sqlite 跑不通。
- **证据**: L171 "svc-posix(fd/stdio 子集)"; L39 "svc-posix(D18: POSIX 运行时服务, fd/VFS/stdio/pthread 子集)"; `00` L514 "pread/pwrite/ftruncate/unlink/stat/fstat/usleep/gettimeofday/pthread_mutex/mmap(可关)"
- **建议**: M2 行改为"svc-posix(fd/VFS/stdio/pthread 子集)"; 16-service 大纲第 1 条给出 M2 必须实现的最小符号清单(以 sqlite 模式 A 需求为验收)。

## [P2] 02 §5 DoD 第 2 项产出列句尾截断("头文件草案待")
- **位置**: `02-roadmap.md` §5 L189
- **问题**: DoD 第 2 项产出列以"…(生命周期状态机/golden 管线/变更产物清单); 头文件草案待"结尾, 句子截断——"待"什么没有说; DoD 是 README 触发条件的依据, 截断项无法判定完成度。
- **证据**: L189 "**清单+管理机制已完成**: `docs/2-os-core/08-core-api-list.md`(48 函数+3 宏…)+ `docs/1-architecture/01-api-contract-governance.md` §2.6(…); 头文件草案待"
- **建议**: 补全为"头文件草案**待起草(本项剩余工作, 建议 M2.5 前)**"或明确"头文件草案另立第 9 项"。

## [P2] 01 §2.3 golden 示例签名与 08 权威签名不一致
- **位置**: `01-api-contract-governance.md` §2.3 L63 vs `08-core-api-list.md` §2 L43–44
- **问题**: 01 的 golden 文件示例写 `FUNC tg_task_create (const tg_task_attr*, tg_thread**)`, 08 的权威签名是 `(tg_thread_t **t, const tg_task_attr_t *attr, …)`——参数顺序与类型名都不一致。golden 被定义为"生成物, 勿手编", 示例却与真值不同: 实现者照示例写比对器会误判 08 的真实产物。
- **证据**: `01` L63 "FUNC tg_task_create (const tg_task_attr*, tg_thread**)  # v1.0"; `08` L43–44 "int tg_task_create (tg_thread_t **t, const tg_task_attr_t *attr, …)"
- **建议**: 01 示例改为与 08 逐字一致(或注明"仅示意记录格式, 实际以生成器输出为准")。

## [P2] v2 的 lwip 依赖"netdev 设备类 [?]"未定却已排期, 决策无载体无期限
- **位置**: `02-roadmap.md` §1 L92/L99; `06-device.md` §8 O-S5 L235; `00-architecture.md` §17 L690–695
- **问题**: v2.0 插件清单把 service/lwip 的依赖写成"netdev 设备类 [?]"——一个未定的设备类支撑一个已排期的 v2 交付物。O-S5 说"v2.0 设计前必须定", 但该决策既不在主文档 §17 待定表(只有 D7/D8), 也没有 v2 设计阶段的 DoD(02 §5 只覆盖 v1.0 设计), 更无决策期限机制——[?] 挂在依赖列里, 无人负责收口。
- **证据**: L92 "| `service/lwip` ★ | Service | 网络栈: svc-posix socket 路由的实现方 | netdev 设备类 [?] |"; L99 "前置设计: **netdev**——…v2.0 设计前必须定"; `06-device.md` L235 "O-S5 …v2.0 设计前定"
- **建议**: 把 netdev 登记进 §17 待定决策表(倾向: netdev-core 第三个子分类框架, 依赖 dev-core); 02 增设"v2.0 设计 DoD"小节(哪怕三条: netdev 定稿/SMP 锁语义定稿/socket 表主人定稿), 使 v2 前置决策有归宿。

## [P2] §14 三域映射的典型插件组合大多未排期, 与 §0 价值主张之间缺一页桥
- **位置**: `00-architecture.md` §14 L663–664、§0 L22 vs `02-roadmap.md` 未排期 L154–155
- **问题**: §0 的核心主张是"仪表、HSM 产品线共享同一个内核与插件库, 新产品只写 APP+选插件"; 但 §14 两域的典型组合——dashboard 的"display/2D、CAN/LIN、touch、EROFS、UDS、日志"、HSM 的"crypto 服务、key 存储、host 接口、安全日志"——其中 display/can/lin/uds/log 均在 02"未排期"清单("随真实 SoC, M4 选配或量产基座"), touch 更是全文档未出现。两节之间缺一句定位说明, 读者会以为这些域插件在本 OS 路线图内。
- **证据**: L663 "| 仪表 dashboard | Cortex-A/R SoC | …display/2D、CAN/LIN、touch、EROFS、UDS、日志 | HMI、帧渲染节奏 |"; `02` L154–155 "service/uds(UDS 诊断, 仪表域)、service/ota、service/log…io/display / io/can / io/lin 等产品驱动(随真实 SoC, M4 选配或量产基座)"
- **建议**: §14 表加一列"本 OS 原型验证 vs 量产基座"(dashboard: core/调度/EROFS/接口在本 OS 验证, display/CAN/LIN/touch/UDS 归量产基座或 M4 选配), 并在 §0 主张后补一句"域插件的大头按战略落在量产基座, 本 OS 验证的是组合模型与契约"。

## [P2] §15 对比表对 Unikraft 的两行表述不准确, 与 02 §0 自身定位矛盾
- **位置**: `00-architecture.md` §15 L672–674 vs `02-roadmap.md` §0 L24
- **问题**: §15 写 Unikraft"调度策略: **固定**"、"应用 API: **libc**"——Unikraft 实际以可选库提供调度(uksched 族的 coop/preempt 实现)与 libc 选择(newlib/musl/nolibc), 两行失实; 且与 02 §0 自己的"Unikraft 路径最顺(**同为 C/aarch64/细粒度库组合**)"矛盾——若对方也是细粒度库组合, 用"组合粒度: 插件级 vs 库级"作差异点就不成立。真正的差异(契约治理机器 golden/门禁、单异常级、域接口皮肤)反而没写进差异行。对以 Unikraft 为量产迁移目标的项目, 对目标平台的失实对比会误导选型论证。
- **证据**: L672 "| 组合粒度 | 插件级(含调度器+接口) | 无(仅调度) | Kconfig+dev tree | **库级** | 静态配置生成 |"; L673 "| 调度策略 | 插件(协作/抢占/时间表) | 固定 | 固定(可配置) | **固定** | 静态表 |"; L674 "| 应用 API | 插件(POSIX/极简/域标准) | FreeRTOS API | 固定+POSIX shim | **libc** | RTE 风格 |"; `02` L24 "aarch64 + C ABI + 插件化 → **Unikraft 路径最顺**(同为 C/aarch64/细粒度库组合)"
- **建议**: Unikraft 列改为"调度策略: 库可选(coop/preempt)""应用 API: libc 可选(newlib/musl/nolibc)"; TangramOS 列的差异点改写为真正独有项——"调度器/接口/驱动均为被治理契约(golden+三层门禁)""单异常级+单一地址空间(车规深度嵌入式语境)"。

## [P2] §16 风险清单编号乱序(R1, R2, R7, R8, R9, R3, R4, R5, R6)
- **位置**: `00-architecture.md` §16 L680–688
- **问题**: 风险按插入顺序排列(1.R1 2.R2 3.**R7** 4.**R8** 5.**R9** 6.**R3** 7.R4 8.R5 9.R6), 编号与序号交叉; 多份文档按"R8""R4"引用风险(如 D6 引 R8、08 引 R4), 读者在清单里需跳跃查找, 也暗示该表无维护纪律(新增即追加)。
- **证据**: L680–688 依次为 "1. **R1…** 2. **R2…** 3. **R7 native API 冻结纪律…** 4. **R8 v2.0 过载…** 5. **R9 原型→量产迁移…** 6. **R3 范围蔓延…** 7. **R4…** 8. **R5…** 9. **R6…**"
- **建议**: 按 R1–R9 顺序重排(或按严重度排序并保留编号列), 表头加一句"新增风险追加 R10+, 不复用已删编号"。

## [P2] D22 "poll/close 由 devfs 适配层"的决策文本与 tg_cdev_ops 实含 poll/close 槽位矛盾
- **位置**: `00-architecture.md` D22 L59; `06-device.md` §3 L148–149/L177; `07-concrete-fs.md` §3 L30
- **问题**: D22 决策结论写"所有设备类别 ops 预留 ioctl/suspend/resume(**poll/close 由 devfs 适配层**)"——读作"设备 ops 不含 poll/close, 由适配层提供"; 但 06 §3 的 `tg_cdev_ops` 结构体**含有** poll/close 槽位(L148–149), 07 §3 又说适配层"open 建会话/read/write/ioctl/poll/close **转发**"。三处对"poll/close 归属"的口径不一致: 决策文本说"由适配层(不在 ops)", 结构体说"在 ops", 07 说"适配层转发到 ops"。D22 是进 golden 的布局决策, 文本与结构体的归属口径必须一致, 否则 golden 定稿时二选一。
- **证据**: L59 "**所有设备类别 ops 预留 ioctl / suspend / resume(poll/close 由 devfs 适配层)**"; `06-device.md` L148–149 "int (*poll)(void *sess, uint32_t *events); int (*close)(void *sess);"(在 tg_cdev_ops 内); L177 "file 面向的 `poll` / `close` 由 devfs 适配层(cdev-core 通用 `tg_file_ops`)提供"; `07` L30 "cdev-core 提供通用会话适配 `tg_file_ops`(open 建会话/read/write/ioctl/poll/**close 转发**…)"
- **建议**: D22 行改为"poll/close 槽位保留于设备 ops(cdev 为会话级), 文件面由 cdev-core 通用适配层转发; bdev/flash 同理", 06 §3 统一同一句式; 明确"适配层提供的是 tg_file_ops 面槽位, 设备 ops 槽位是其后端"。

## [P2] TT_SAFE 的周期/WCET 元数据没有声明位置, TT 插件配 preempt 的合法性未定义
- **位置**: `00-architecture.md` §5.3 L313/L315、§6.1 L321–334
- **问题**: §5.3 要求"tt ⇒ 全部 TT_SAFE 且**周期元数据闭合**", 但描述符与资源字段(res: RAM/栈/IRQ/DMA)均无周期/WCET 字段, manifest 规范(12 骨架)未预留; 且三调度器组合矩阵只写了各自调度器的要求, TT_SAFE 插件配 sched-preempt 是否合法没有答案(TT_SAFE 是否为 SAFE_PREEMPT 子集未定义)。
- **证据**: L313 "| `TT_SAFE` + 周期/WCET 元数据 | 满足时间表声明 | 供 sched-tt 排表与静态 WCET 分析 |"; L315 "tt ⇒ 全部 TT_SAFE 且周期元数据闭合"; L321–334 描述符字段表(无元数据字段)
- **建议**: 描述符或 manifest 预留 `tt_meta`(period/wcet/offset)可选字段(v3 前 NULL); §5.3 组合校验规则补一行: "TT_SAFE ⇒ 视为 SAFE_PREEMPT(允许配 preempt)"或"TT_SAFE 仅可配 tt"(二选一成文)。

---

## 统计

| 严重度 | 数量 |
|---|---|
| P0(矛盾/架构缺陷) | 3 |
| P1(完备性缺口/技术不清晰) | 11 |
| P2(改进建议) | 10 |
| **合计** | **24** |

## 附注

(1) 已按排除清单核对 `../review1/` 与本系列其他模块评审(02-os-core / 04-debug-test / 05-plugin-ecosystem / 06-cross-doc-consistency), 本报告 24 条均不与之重复; 与既有结论边界接近的三条(严格叶子、D15×R8 互动、挂载拓扑)已在正文标注区别点。(2) 行号基于 `00-architecture.md` 695 行当前版本(评审期间 §3 图被并行重排一次, 全部行号已复核); 01/02 自本次评审开始以来未被修改。(3) 本评审只读未写。

## 核对无误的方面(供参考)

- D18 双角色拆分与 §7.5 验证经济学论证链完整自洽; §7.6 移植双模式与 02 路线图 M2 验证(sqlite 模式 A)对齐
- D12–D15 治理机器(三态生命周期/golden 管线/三层门禁/冻结计划)在 00/01/08 间闭环
- 02-roadmap v1.0 插件清单(17 件)与 v2/v3/vx 清单、M0–M4 里程碑与主文档决策版本归属一致
- §0 战略语境(持久资产/契约优先于实现)与 §15 对比表其余单元格定位准确
