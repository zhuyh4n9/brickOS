# review1 修复决策表（FIX-DECISIONS）

> 用途： 按 `comment/review1/` 各评审文件的意见修订 `tangramOS/docs/`。本表解决全部「二选一/多选一」选项与跨文档一致性问题。
> 执行规则： **评审文件（01–06）给明细与行号，本表给最终裁决**；两者冲突时以本表为准；文件现行文本是唯一真值（行号可能有 ±1 漂移，按文本锚点定位）。
> 总原则： 08 + 主文档为契约真值；「冻结」一词保留给 D15 升格流程，设计期一律用「定稿」；全库统一符号/段名/版本口径；不改变文档结构与语气（中文、半角标点、简洁风格）。

## G. 全局裁决（跨文档）

- **G-A1 trace 记录长度 = 16B（定）**： 保留主文档 §11、02-roadmap L64、03-debug L10 的「16B」文字不动；修改 03-debug 结构体 `arg[3]` → `arg[2]`（4+2+1+1+8=16），宏 `TRACE_EVT(id, a, b, c)` → `TG_TRACE_EVT(id, a, b)`，`typedef struct __packed` → `typedef struct __attribute__((packed))`。
- **G-A2 红区/金丝雀 = v1.x（定）**： 改 02-roadmap v2.0 debug 行（注明红区/金丝雀已于 v1.x 落地）；主文档 §11 与 03-debug §4 不动。
- **G-A3 运行时挂载 = 事件模型 v2 设计 / 挂载落地不早于 v3（定）**： 04-vfs（L97、SD-4 行 L152、O-S3 行 L162）、07-concrete-fs（L63、O-S3 行 L105）统一此口径；roadmap L135 保持 v3 并对齐措辞。
- **G-A4 debug bridge = v1.0(M3) 起最小集（定）**： 主文档 §11 debug bridge 行、03-debug §2 标题改「v1.0(M3) 起最小集」；roadmap 不动（其 v1.0/M3 表述已正确）。
- **G-A5 「冻结」术语（定）**： M2 时点的表述一律改「定稿/定好签名（主文档风险 R4）」；「冻结/升格 frozen」只用于 D15 流程（M3/v1.0 分批升格）。涉及 08 L26/L171、05-bdev L57、11-memory L14/L31。
- **G-A6 tg-sched 升格前置（定）**： 放宽为「**当期已交付调度器** × 同一套语义测试全绿」，并注明 sched-preempt(v2.0)/sched-tt(v3.0) 交付后必须追加对应矩阵行并保持全绿。涉及 08 L24/L374、01 L126/L149/L184/L234。
- **G-B1 链接段名 = `.tg_plugins`（定，从主文档）**： 08 L335/L338/L367、12-plugin-mgr L21 全部改带 s。
- **G-B2 边界符号 = `__tg_plugins_start` / `__tg_plugins_stop`（定）**： 由链接脚本 PROVIDE 定义别名（段名含 `.` 非 C 合法标识符，GNU ld 不自动生成 `__start_/__stop_` 边界符号）。
- **G-B3 trace 宏 = `TG_TRACE_EVT`（定）**： 改 03-debug；08 L284 已正确不动。
- **G-B4 内联访问器宏 = `TG_API_INLINE_FROZEN`（定）**： 改 08 L314（补「布局入 golden、每例需 CA 记录」语义）。
- **G-B5 `tg_task_create` 参数顺序 = 08 规格为准（句柄在前）（定）**： 改 01 L63 golden 示例为 `FUNC tg_task_create (tg_thread**, const tg_task_attr*)`；类型名统一 `tg_thread_t`（01 L63 的 `STRUCT tg_thread` → `STRUCT tg_thread_t`）。
- **G-B6 `sched_ops` 槽位 = 主文档 §5.1 现有名（定）**： `sched_ops.create`→`sched_ops.thread_ready`（入列）；`sched_ops.switch_out`→`sched_ops.thread_block` + `sched_ops.pick_next`；`sched_ops.yield`→core 发起 + `sched_ops.pick_next`。
- **G-B7/B8/B9 生命周期与启动时序（定）**： 四相 EARLY→CORE→LATE→APP；删除无出处的 PRE_ARCH/PIC/ARCH 子相；主文档 §6.1 `early_init` 注释「无堆」→「不使用堆」；主文档 §9：BSS 清零移入 reset 汇编步骤、`core.init` 描述删「BSS 清零」、全局开中断与 `app.start()` 之间补「各插件 start()」；主文档 §6.2 补「回调↔阶段映射表」；12-plugin-mgr L13/L21–23、11-memory L32 同步。
- **G-C1 D10 双保险语义（定）**： 双保险 = **组合期校验 + 静态分析**；conformance 矩阵 = **R1 的执法载体**（不是双保险成员）；矩阵失败 = CI 门禁红/阻断升格（**不是**组合期拒绝——组合期校验识别不出声明撒谎）。涉及 09-int L11、10-sched L12、19-test L127、08 L286、06-device L185、18-plugin-dev L72。
- **G-C2 D8 表述（定）**： 「最后两个未落定决策之一（与 D7 并列， 主文档 §17）」。
- **G-C4 严格叶子（定）**： 统一句式「**除 APP 外**没有任何插件依赖它们」。涉及主文档 L170/L406/L486、15-interface L8。
- **G-C5 abi_id（定）**： 「描述符带 `abi_id` 字段（D14: Day1 即存在, v2 二进制分发起生效）」。
- **G-D1 SD-10 错误码（定）**： **SD-10 全集 = 两域并集（11 码）**: `-EIO/-EAGAIN/-EINVAL/-ENOMEM/-ENODEV/-ENOTSUP/-EBUSY/-EEXIST/-ETIMEDOUT/-ENOSPC/-EROFS`；各域取子集并注明——core 域子集（9 码， 08 §11）、设备/存储域子集（7 码， 06-device §4）；19-test 不变量 INV-4 改引「08 §11」。
- **G-D2 `-ENOSYS` → `-ENOTSUP`**（19-test TC-MM-003）。
- **G-D3 `-EOPNOTSUPP` → `-ENOTSUP`**（07-concrete-fs L32）。
- **G-D4 errno 表述统一「errno = -ret（零转换）」**（16-service L19）。
- **G-E10 评审笔记路径（定）**： 用户手写笔记 `00-architecture-comment.md` 位于仓库外、不随库分发——改说明性表述，不写会解析失败的相对路径。
- **G-F1 用例数 = 43 × 3 调度器 = 129（定）**： 19-test L121、README L19。
- **G-G1 「挂载表单路由」→「挂载表·单路由」**： 04-vfs L4、06-device L16/L40/L77、主文档 L240/L416/L588、02-roadmap L56。
- **G-G3 19-test 不变量改名 INV-1…INV-6**： 全篇 `R-n` → `INV-n`（表头与全部引用 L45/L55/L58/L68/L77/L90/L109 等）；08 L289「R-1」→「INV-1」；19-test L99 裸「R4」→「主文档风险 R4」。
- **G-36 08 §4 补 clock_now 单调性**： 加一行「`tg_clock_now` 单调不减（19-test INV-5 执法）」，使 19-test INV-5 的依据成立。
- **G-42 §14「三域映射」→「两域映射」（定）**： 主文档 §14 标题改「两域映射」；02-roadmap L197「三域示例」改「两域示例（主文档 §14）」。

## F1. `docs/1-architecture/00-architecture.md`（主文档）

1. **L3** 「所有图为 PlantUML(```plantuml 块; …」→「所有图为 PlantUML(plantuml 代码块; VSCode/IDEA 插件或 GitLab 原生渲染, CLI: `plantuml docs/*.md` 直出)。」——行内不再出现裸三反引号。
2. **L10** 「吸收评审意见(`../comment/00-architecture-comment.md`, 仓库外)」→「吸收评审意见(用户手写笔记 `00-architecture-comment.md`, 仓库外、不随库分发)」（G-E10）。
3. **L56（D19 行， 推论列）**「派生: vfs-core→dev-core(设备路由+适配), svc-posix→vfs-core」→「派生: ~~vfs-core→dev-core(设备路由+适配)~~ → **D21 修订**: 该依赖已撤销, 设备路由改经 devfs/cdev-core 侧(见 D21); svc-posix→vfs-core」（仿 D11/SD-12 的删除线修订风格）。
4. **L72（§2.2 表头）**「| 目标 ISA | 运行级别 | 说明 |」→「| ISA 家族映射(现役: aarch64, D9) | 运行级别 | 说明 |」。
5. **L75** 「| ARM Cortex-M/R | 恒特权态 | 不切 PSP 非特权模式 |」→「| ARM Cortex-M/R | 恒特权态 | 不进入非特权线程模式(CONTROL.nPRIV 保持 0) |」。
6. **L170** 「接口插件是严格叶子**: 系统中没有任何插件依赖它们」→「…: **除 APP 外**没有任何插件依赖它们」（G-C4）。
7. **L212** 「| vx.0 | MPU 插件 | Cortex-M/R 或 A 核补充; 启用 D4 分段元数据 |」→「| vx.0 | MPU 插件 | Cortex-M/R(A 核继续用 MMU region); 启用 D4 分段元数据 |」。
8. **L240（§4.5）**「`tg_open` 挂载表单路由」→「`tg_open` 挂载表·单路由」（G-G1）。
9. **L269（§5.1 `tg_sched_ops`）** `uint32_t sched_class; /* PREEMPT | COOP | TT (§5.3) */` → `uint32_t sched_kind; /* 调度器类别: PREEMPT | COOP | TT(§5.2; 与插件侧 sched_class(§5.3)区分) */`（字段改名， 消除同名两义）。
10. **L315（§5.3）**「tt ⇒ 全部 TT_SAFE 且周期元数据闭合」→「tt ⇒ SAFE_PREEMPT 或 TT_SAFE(周期元数据闭合)」。
11. **L331（§6.1）** `int (*early_init)(void); /* 无堆/无线程/关中断 */` → `/* 不使用堆/无线程/关中断(堆已由 core.init 建立, 本相约定不使用) */`（G-B8）。
12. **L343（§6.2）** 段后补「回调↔阶段映射表」（G-B9）：
   ```
   描述符回调与阶段的映射(§9 为运转视图):

   | 阶段 | 调用的回调 | 运行环境 |
   |---|---|---|
   | EARLY | `early_init`(全部插件, 拓扑序) | 不使用堆 / 无线程 / 关中断 |
   | CORE | `init`(非 Service/Interface 插件; FS 挂载计划在此执行) | 堆可用, 调度器已锁定, 中断仍关 |
   | LATE | `init`(Service → Interface 依序) | 同 CORE |
   | 全局开中断后 | `start`(全部插件) → `app.start()` | 中断可用, 可创建线程 |
   ```
13. **L364（§6.4 第 5 条）**「IRQ/外带独占冲突检测」→「IRQ/DMA 独占冲突检测」。
14. **L406/L408/L431（§7.2 图）**： G_IF 包标题改「Interface 插件 — 严格叶子: 除 APP 外没有任何插件依赖它们」（G-C4）；节点拆分： `[iface-min / iface-pkcs11 …] as IF_O` → 两个节点 `[iface-min(直通 native)] as IF_MIN` 与 `[iface-pkcs11 …(域标准)] as IF_O`；边改为 `APPN --> IF_P` / `APPN --> IF_MIN` / `APPN --> IF_O` / `IF_P --> SPX : 再导出(D13)` / `IF_O --> SX` / 新增 `IF_MIN --> COREN : 直通 native(依赖仅 core)`（消除 iface-min 共享指向 SX 的错误边）。
15. **L410** 「服务间可声明依赖(init DAG 无环)」→「init-DAG 无环」（全库统一连字符）。
16. **L416（§7.2 图 VFSC2 节点）**「tg_open 挂载表单路由」→「tg_open 挂载表·单路由」（G-G1）。
17. **L486（§7.5 论证 2）**「系统中没有任何插件依赖接口插件」→「除 APP 外没有任何插件依赖接口插件」（G-C4）。
18. **L500（§7.6 示例）** `TG_PLUGIN(sqlite, .deps = {"svc-posix@>=1.0"}, ...);` → `TG_PLUGIN(sqlite, .deps = (const tg_dep_t[]){{"svc-posix", ">=1.0", TG_PHASE_LATE}}, ...);  /* deps 形状见 §6.1: {name, 区间, phase} 数组 */`
19. **L512（§7.6 模式 B 行， 依赖面列）**「纯 native(tg_fs)」→「纯 native + vfs-core 框架件契约(`tg_open`/`tg_file_*`)」（`tg_fs` 前缀不存在）。
20. **§9 启动序列（L557–575）**（G-B7/B9）：
    - `:reset\nplatform 向量表(汇编);` → `:reset\nplatform 向量表(汇编) · BSS 清零(汇编);`
    - `:core.init\nBSS 清零 · 堆 · 中断框架 · 注册表 · 调度框架对象(无线程);` → `:core.init\n堆 · 中断框架 · 注册表 · 调度框架对象(无线程);`
    - `:CORE\n各插件 init(堆可用)` → `:CORE\n非服务插件 init(堆可用)`
    - 在 `:全局开中断;` 与 `:app.start()` 之间插入 `:各插件 start()\n中断可用, 可创建线程;`
21. **L588（§10 图 VFS 节点）**「tg_open 挂载表单路由 · v1.0」→「tg_open 挂载表·单路由 · v1.0」（G-G1）。
22. **L630（§11 debug bridge 行）**「v1.x 最小集」→「v1.0(M3) 最小集」（G-A4）。
23. **L659（§14 标题）**「三域映射」→「两域映射」（G-42；正文两行不动）。
24. **L675（§15 表）** FreeRTOS 特权级列「单级」→「单级(可选 MPU 分离)」。
25. **L680–688（§16 风险清单）**： 按 R1–R9 顺序重排（现行为 R1,R2,R7,R8,R9,R3,R4,R5,R6 → 改为 R1,R2,R3,R4,R5,R6,R7,R8,R9， 列表序号 1–9 与 R 号一致， 内容逐字保留）。

## F2. `docs/1-architecture/01-api-contract-governance.md`

1. **L3** 「回答两个问题: 契约怎么变更(D12), 接口插件怎么声明与叠加(D13)。」→「契约治理三件事: 变更怎么管(D12)、接口插件怎么声明与叠加(D13)、二进制兼容与冻结时点(D14–D15)。」
2. **L17（§2.1 状态图）** 代码块内补回边行： 在「experimental ──(升级评审)──▶ frozen ──(标注)──▶ deprecated ──(弃用周期)──▶ 移除」下一行加「frozen ◀──(撤销弃用, §2.6.2)── deprecated」。
3. **L22（§2.1 表 experimental 行）**「随时可碎, 不通知」→「随时可碎, 无流程门槛(改动留 CA 存档, §2.2)」。
4. **L51（§2.2 阈值表）**「| experimental 区任意改动 | 无 |」→「| experimental 区任意改动 | 无流程门槛(留 CA 存档) |」。
5. **L63（§2.3 层 1 golden 示例）**： `FUNC tg_task_create (const tg_task_attr*, tg_thread**)` → `FUNC tg_task_create (tg_thread**, const tg_task_attr*)`；`STRUCT tg_thread  # opaque` → `STRUCT tg_thread_t  # opaque`（G-B5）。
6. **L78（§2.3 层 2）**「coop 下过不了“抢占安全”用例 ⇒ SAFE_PREEMPT 声明为假」→「在 sched-preempt 下过不了“抢占安全”用例 ⇒ SAFE_PREEMPT 声明为假(矩阵执法; coop 下不发生抢占, 检验力在 preempt 侧)」。
7. **L102（§2.5）**「若 D14 答案为“是”, 布局从 day 1 按硬契约对待」→「D14 已定 Day1 二进制兼容, 布局从 day 1 按硬契约对待」。
8. **L118（§2.6.1）**「**单一事实来源是头文件 + 构建产物, 文档与 golden 都是衍生物**」→「**声明的事实来源是头文件, 符号真值是构建产物; 文档与 golden 都是衍生物**」。
9. **L126（§2.6.2 图）**「前置: conformance 矩阵全绿」→「前置: 已交付调度器矩阵全绿」（G-A6）。
10. **§2.6.2 图后（L144 代码块结束处与 L146 表之间）** 加注： 「> 注: 「CA 记录」= `docs/2-os-core/08-core-api-list.md` §14 的契约决策记录(CA-*)——轻量存档; experimental 区改动无流程门槛、仅留此存档(§2.2)。」（首次出现处展开缩写）
11. **L149（转换表）**「**conformance 矩阵全绿**(层 2)」→「**当期已交付调度器的 conformance 矩阵全绿**(层 2)」（G-A6）。
12. **L184（§2.6.5 表）**「**矩阵全绿前置**」→「**当期已交付调度器矩阵全绿前置**」（G-A6）。
13. **L191（§2.6.6 层 1 双保险）**「标注了 `TG_API` 但 golden 无记录 = 红」→「标注 `TG_API_FROZEN`/`TG_API_DEPRECATED` 而 golden 无记录 = 红; `TG_API_EXPERIMENTAL` 不入 golden(§2.6.2)」。
14. **L234（§4 D15 行）**「M3: 契约已被三调度器+多接口插件压测, 按子系统分批升格 frozen」→「M3: 契约已被 sched-coop + 多接口插件压测(sched-preempt/tt 交付后补跑矩阵并保持全绿), 按子系统分批升格 frozen」（G-A6）。

## F3. `docs/1-architecture/02-roadmap.md`

1. **L3** 「依据: `../comment/00-architecture-comment.md`(仓库外)评审意见。」→「依据： 用户手写笔记 `00-architecture-comment.md`(仓库外、不随库分发)评审意见。」（G-E10）
2. **L11** 「**量产产品计划在 rCore / Unikraft 等成熟 unikernel 上二次开发**」→「**量产产品计划在成熟基座上二次开发——Unikraft(较成熟的 unikernel)/ rCore(教学核, 原型验证)**」。
3. **L41** 「(§7.6: 模式 A 直链 svc-posix 跑通…)」→「(主文档 §7.6: 模式 A 直链 svc-posix 跑通…)」。
4. **L56** 「tg_file/tg_open 挂载表单路由/挂载表」→「tg_file/tg_open 挂载表·单路由/挂载表」（G-G1）。
5. **L60（io/uart-pl011 行）**「| `io/uart-pl011` | I/O | PL011: 早期轮询 console → 中断 tty | cdev-core | M0(轮询)/M2(tty) |」→「| `io/uart-pl011` | I/O | PL011: 早期轮询 console(M0: platform 早期 console, 不注册设备)→ 中断 tty(M2: 注册 cdev) | —(M0)/cdev-core(M2) | M0(轮询)/M2(tty) |」
6. **L63（svc-posix 行）**「POSIX 运行时(D18): fd/stdio/pthread 子集 + libc stub」→「POSIX 运行时(D18): fd/stdio 子集 + libc stub(pthread 子集后置)」——与 L171 M2 行「svc-posix(fd/stdio 子集)」对齐。
7. **L68（app/hello 行）**「| `app/hello` + conformance | APP | 启动链演示(M0)+ native API conformance 首版(M3, **用例目录: `docs/5-test/19-test.md`**) | iface | M0/M3 |」→「| `app/hello` + conformance | APP | 启动链演示(M0: 直接主循环, 不依赖 iface)+ native API conformance 首版(M3, **用例目录: `docs/5-test/19-test.md`**) | —(M0)/iface(M3) | M0/M3 |」
8. **L80** 「**EROFS**(只读压缩根/资源分区; …)」→「**EROFS**(只读压缩代码/资产分区, 挂 /assets; …)」。
9. **L81（v2.0 debug 行）**「**mini ramdump** + target 侧 memleak(arena 记账) + 红区/金丝雀」→「**mini ramdump** + target 侧 memleak(arena 记账)——红区/金丝雀已于 v1.x 落地(03-debug §4), 不属 v2.0 交付」（G-A2）。
10. **L135** 「…与 USB/事件模型联动, 若落地也在 v3 [?]。」→「…——事件模型 v2 设计(与 USB 栈联动), 运行时挂载落地不早于 v3 [?]。」（G-A3）
11. **L156** 「- `iface-autosar-ish`(主文档 §7.4 [?])」→「- `iface-autosar-ish`(车规域标准示例, 见 `docs/9-interface/15-interface.md` §1 [?])」（G-E9）。
12. **L189** 「头文件草案待」→「头文件草案待写」。
13. **L192（DoD 第 5 项）**「构建系统与仓库骨架(CMake 选型论证、目录布局、链接脚本、QEMU 脚本)」→「构建系统与仓库骨架(构建系统选型论证——make/CMake/自研 [?], 见 `docs/7-toolchain/13-toolchain.md` §3; 目录布局、链接脚本、QEMU 脚本)」（与 13-toolchain 的中性表述对齐）。
14. **L197** 「文档导览(00–03)、三域示例」→「文档导览(00–02; 03 在 4-debug)、两域示例(主文档 §14)」（G-42 + 导览范围修正）。

## F4. `docs/2-os-core/08-core-api-list.md`

1. **L7** 「详细设计约定: 每组两小节——…」→「详细设计约定: 以任务组为样例(§2.1/§2.2)——每组两小节: **语义规格**(每函数: 错误/阻塞/归属)+ **实现设计**(数据结构/不变量/竞态); …」（其余保留）。
2. **L24（§1 表 tg-sched 行）**「**第二批**(需三调度器 conformance 矩阵压测)」→「**第二批**(需当期已交付调度器的 conformance 矩阵压测; sched-preempt/tt 交付后追加矩阵行)」（G-A6）。
3. **L26（§1 表 tg-mm 行）**「第一批(签名 M2 起冻结, R4)」→「第一批(签名 M2 起定稿——主文档风险 R4; M3/v1.0 升格 frozen, D15)」（G-A5）。
4. **L39** 「coop 忽略(§5.3)」→「coop 忽略(主文档 §5.2)」（改引正确章节并全限定）。
5. **L61** 「core 构造 + `sched_ops.create`」→「core 构造 + `sched_ops.thread_ready`(入列)」（G-B6）。
6. **L63** 「core + `sched_ops.switch_out`」→「core + `sched_ops.thread_block` + `sched_ops.pick_next`」（G-B6）。
7. **L64** 「`sched_ops.yield`」→「core 发起 + `sched_ops.pick_next`」（G-B6）。
8. **L65** 「`-EINVAL`(负值)」→「`-EINVAL`(如 `TG_TIMEOUT_INF`——`tg_time_t` 为无符号, 无“负值”可言)」。
9. **L72（线程状态机）**「`NEW → READY ⇄ RUNNING → BLOCKED(等待队列+超时表) → ZOMBIE →(join 回收)`」→「`NEW → READY ⇄ RUNNING`; `RUNNING → BLOCKED`(等待队列+超时表)与 `BLOCKED → READY`(唤醒); `RUNNING → ZOMBIE`(`tg_task_exit` 仅自身可调)→(join 回收)」。
10. **L75** 「栈约束: `stack_size ≥ TG_STACK_MIN`(2K [?]), 8 字节对齐; 栈 guard = vx(红区)」→「栈约束: `stack_size ≥ TG_STACK_MIN`(2K [?]), **初始 SP 16 字节对齐**(AAPCS64: 栈大小为 16 的倍数); 栈保护区(guard region)= vx.0」。
11. **L105** 「coop 下锁退化(§5.2)」→「coop 下锁退化(主文档 §5.2)」。
12. **L118** 「ns 的 32 位乘法溢出陷阱多」→「ns 计量的 32 位中间量换算溢出陷阱多」；**L358（CA-1 行）**「ns 的 32 位乘法溢出陷阱」→「ns 计量的 32 位中间量换算溢出陷阱」。
13. **L127** 「(资源预算, §6.4)」→「(资源预算, 主文档 §6.4)」。
14. **L148** 「v1 消费者: 页表自用; v1.x/v2: 无 SG DMA 大缓冲、page cache、EROFS 解压页、v3 模块加载」→「v1 消费者: 无(恒等映射页表由 platform early_init 先于页池构造, 不经本 API——TC-MM-003: v1 `tg_mm_map` = -ENOTSUP); v1.x/v2: 无 SG DMA 大缓冲、page cache、EROFS 解压页; v3: 模块加载」。
15. **L158** 「(§6.4 资源总账)」→「(主文档 §6.4 资源总账)」。
16. **L170** 「/* platform early_init 声明恒等区(§9) */」→「/* platform early_init 声明恒等区(主文档 §9) */」。
17. **L171** 「/* v2 重定位主用; v1 签名冻结(R4) */」→「/* v2 重定位主用; v1 签名定稿(R4), M3/v1.0 升格 frozen(D15) */」（G-A5）。
18. **L262（§9）**「(`docs/6-vfs-device/04-vfs.md` §0.1 消费者表即用法总览)」→「(`docs/6-vfs-device/06-device.md` §1 消费者依赖声明表即用法总览)」（G-E2）。
19. **L263** 「init 顺序由依赖声明保证(§6.5)」→「init 顺序由依赖声明保证(主文档 §6.5)」。
20. **L265/273（§10 表）** `tg_mm_region_add` 行加注「兼属 native API」：「| `tg_mm_region_add`(§7; 兼属 native API, §1/§6) | Platform 插件(early_init) | 本文 §7 |」。
21. **L286** 「conformance 静态扫描执法(D10/R1)」→「静态扫描执法(D10 双保险之一; conformance 矩阵 = R1 执法)」（G-C1）。
22. **L289（§11 错误码）**→「**错误码**(SD-10 全集的 core 域子集; 全集与设备域子集见 `docs/6-vfs-device/06-device.md` §4): int 返回, 负 errno 子集 `-EIO/-EAGAIN/-EINVAL/-ENOMEM/-ENODEV/-ENOTSUP/-EBUSY/-EEXIST/-ETIMEDOUT`(末项 = 阻塞超时统一, INV-1/CA-4——`docs/5-test/19-test.md` 用例设计前置补入); svc-posix 取 `-ret` 作 errno。」（G-D1/G-G3）
23. **§4（时间组， L118 段后）** 加一行：「- `tg_clock_now` 单调不减(19-test INV-5 执法)」（G-36）。
24. **L314** `#define TG_API_INLINE static inline          /* 布局受控访问器(01 §2.4) */` → `#define TG_API_INLINE_FROZEN static inline   /* 布局受控访问器(01 §2.4/§2.6.3: 布局入 golden, 每例需 CA 记录) */`（G-B4）。
25. **L335** `section(".tg_plugin")` → `section(".tg_plugins")`（G-B1）。
26. **L338** `extern const tg_plugin_t __start_.tg_plugin[], __stop_.tg_plugin[];` → `extern const tg_plugin_t __tg_plugins_start[], __tg_plugins_stop[];\n/* 边界符号由链接脚本 PROVIDE 定义(段名含 '.', 非 C 合法标识符, GNU ld 不自动生成 __start_/__stop_) */`（G-B2）。
27. **L367（CA-10 行）**「(描述符 `.tg_plugin` section 收集, 近零导出面)」→「(描述符 `.tg_plugins` section 收集, 近零导出面)」（G-B1）。
28. **L374（§15 第二批）**→「**第二批(M3 后, conformance 矩阵压测后)**: `tg-sched`——任务/同步与调度器策略强耦合, 必须 `{当期已交付调度器} × 同一套语义测试` 全绿后升格(01 §2.3 层 2); v1.0(M3) 时点仅有 sched-coop, sched-preempt(v2.0)/sched-tt(v3.0) 交付后必须在其上追加矩阵行并保持全绿」（G-A6）。

## F5. `docs/2-os-core/09-int.md`

1. **L11** 「conformance 静态扫描执法(D10/R1)」→「静态扫描执法(D10 双保险之一)+ conformance 矩阵(R1 执法)」（G-C1）。
2. **L14** 「prio/trigger 解释权在 platform(**06 §8 三层模式**); core 只传递」→「prio/trigger 解释权在 platform(**主文档 §8 三层模式**); core 只传递」（G-E1）。
3. **L19** 「共享 IRQ(v1 是否支持 [?])」→「共享 IRQ(已定: v1 非目标——每根物理线恰一属主, 复用走级联域, 08 §8.1)」。
4. **L20** 「与调度锁的关系(coop/preempt 下的差异)」→「与调度器内部锁/抢占禁止的关系(coop/preempt 下的差异)」。
5. **L29** 「**中断线程化**(最后一个未落定的架构决策)」→「**中断线程化**(最后两个未落定决策之一, 与 D7 并列——主文档 §17)」（G-C2）。
6. **L30** 「| — | 共享 IRQ(v1: 不支持? 嵌入式单用途引脚通常独占) |」→「| — | 共享 IRQ(已定: v1 非目标, 见 08 §8.1——每根物理线恰一属主, 复用走级联域) |」。

## F6. `docs/2-os-core/10-sched.md`

1. **L12** 「**sched_class 声明**(D10): SAFE_PREEMPT / COOP_ONLY / TT_SAFE——插件声明 + conformance 双保险」→「**sched_class 声明**(D10): SAFE_PREEMPT / COOP_ONLY / TT_SAFE——插件声明 + 组合期校验 + 静态分析(D10 双保险)+ conformance 矩阵(R1 执法)」（G-C1）。
2. **L21** 「tickless 超时轮: 绝对期限队列、…」→「tickless 超时队列: 绝对期限、…」（G-G4）。
3. **L31** 「preempt 是否需要显式 `TG_PREEMPT_POINT()` 宏(长临界区)」→「…宏(长无阻塞计算段/长临界路径)」。

## F7. `docs/2-os-core/11-memory.md`

1. **L8** 「core 提供**一个堆 + MMU 接口 + DMA 约定**; 虚拟内存政策 = … vx MPU(用户反转后的政策, 主文档 §4.3)。」→「core 提供**三个池(TLSF 堆 / contig 池 / 页池, CA-7/CA-8)+ MMU 接口 + DMA 约定**; 虚拟内存政策 = v1 恒等映射(属性隔离)/ v2 重定位 / vx MPU(v0.4 评审反转后的政策, 主文档 §4.3)。」
2. **L14** 「map/unmap 签名 v1 冻结(R4)」→「map/unmap 签名 v1 起定稿(R4; 升格 frozen 走 D15)」（G-A5）。
3. **L31** 「R4 | DMA/cache API 签名 M2 冻结——实现细节(aarch64 ISA 库归属)待成文」→「R4 | DMA/cache API 签名 M2 起定稿(升格 frozen 走 D15)——实现细节(aarch64 ISA 库归属)待成文」（G-A5）。
4. **L32** 「堆的启动时序: 需要在插件 init 前可用 ⇒ 早期初始化点(§9 的 PRE_ARCH 后)」→「堆的启动时序: 需要在插件 init 前可用 ⇒ 初始化点 = core.init(主文档 §9: 早于 EARLY 相)」（G-B7）。
5. **L37** 「`docs/1-architecture/02-roadmap.md` §5 第 1 项(D7/D8 之外的收敛项)」→「…§5 第 2 项(D7/D8 之外的收敛项)」（G-E4）。

## F8. `docs/2-os-core/12-plugin-mgr.md`

1. **L12** 「v2 描述符带 `abi_id`(D14)」→「描述符带 `abi_id` 字段(D14: Day1 即存在, v2 二进制分发起生效)」（G-C5）。
2. **L13** 「**生命周期**(§6.2): EARLY(PRE_ARCH/PIC/ARCH)/CORE/LATE 三相; 启动序列见主文档 §9」→「**生命周期**(§6.2): EARLY → CORE → LATE → APP 四相(回调↔阶段映射见主文档 §6.2); 启动序列见主文档 §9」（G-B7）。
3. **L21** 「描述符二进制细节: 段位置(`.tg_plugin`)+ `__start/__stop` 边界枚举(机制见 08 §13.3)、abi_id 编码(D14)」→「描述符二进制细节: 段位置(`.tg_plugins`)+ 边界符号枚举(`__tg_plugins_start/__tg_plugins_stop`, 链接脚本 PROVIDE——段名含 `.` 非 C 合法标识符; 机制见 08 §13.3)、abi_id 编码(D14)」（G-B1/B2）。
4. **L22** 「生命周期状态机: 注册→init→运行; …」→「生命周期状态机: 注册→early_init→init→start→运行; …」（G-B9）。
5. **L23** 「相(EARLY/CORE/LATE)内排序」→「相(EARLY/CORE/LATE/APP)内排序」。

## F9. `docs/2-os-core/17-service-mgmt.md`

1. **L20** 「trace 环(MPSC 无锁例外)」→「trace 环(主人 = trace 服务, 多生产者经 MPSC 无锁写入——所有权与访问协议正交)」。

## F10. `docs/3-plugin-dev/18-plugin-dev.md`

1. **L4** 「驱动、文件系统、服务、接口、平台、调度器、APP 全部是插件。八类各有契约」→「驱动、文件系统、服务、接口、平台、调度器、**框架件**、APP 全部是插件。八类各有契约」（G-E11）。
2. **L27（路由表框架件行， 依赖列）**「core / 互相」→「core / 框架件间单向(cdev-core→dev-core 与 vfs-core, bdev-core→dev-core)」。
3. **L31（Interface 行， 依赖列）**「svc-posix 等服务」→「svc-posix 等服务或 core(iface-min 直通)」。
4. **L32（APP 行， 依赖列）**「Interface」→「Interface(或直调 native)」。
5. **L42** 「禁阻塞/malloc/持锁返回」→「禁阻塞/malloc/持锁跨 ISR 返回」。
6. **L62** 「init 时序: 依赖方 publish 在先、消费方 lookup 在后(init-DAG 保证)」→「init 时序: 被依赖方(服务方)publish 在先、依赖方(消费方)lookup 在后(init-DAG 保证; 17-service-mgmt §1)」。
7. **L72（静态扫描行）**「| 静态扫描 | ISR 白名单 / DMA 纪律 / 符号命名 | D10/R1 执法 |」→「| 静态扫描 | ISR 白名单(CA-3)/ DMA 纪律(主文档 R4)/ 符号命名(D13/CA-10) | 静态分析 = D10 双保险之一; conformance 矩阵 = R1 执法 |」（G-C1）。

## F11. `docs/4-debug/03-debug.md`

1. **L13–19（trace 结构体）**（G-A1）：
   ```c
   typedef struct __attribute__((packed)) tg_trace_evt {
       uint32_t tsc;      /* 时间戳低 32 位, 回绕由解码器处理 */
       uint16_t evt;      /* 事件 id(由 manifest 分配) */
       uint8_t  ctx;      /* 0=ISR, 1..n=线程 id */
       uint8_t  narg;
       uint32_t arg[2];
   } tg_trace_evt_t;
   ```
   结构体后加注行：「(4+2+1+1+8 = 16B, 2 的幂记录长度——环形缓冲索引可用掩码; 与主文档 §11 / 02-roadmap 的「16B」口径一致)」
2. **L23** 「API: `TRACE_EVT(id, a, b, c)` 宏; …」→「API: `TG_TRACE_EVT(id, a, b)` 宏(D16: 宏 `TG_*`; 08 §11 白名单引用此名); …」（G-B3）。
3. **L27（§2 标题）**「debug bridge(类 adb, v1.x 起最小集)」→「debug bridge(类 adb, v1.0(M3) 起最小集)」（G-A4）。
4. **L37** 「aarch64 同步异常(向量表分槽, `ESR_EL1`/`FAR_ELx` 区分 fault 与 IRQ)」→「aarch64 同步异常(向量表分槽区分 fault 与 IRQ; `ESR_EL1`(EC)/`FAR_EL1` 定位 fault 细节)」。
5. **L41** 「预留 dump 区(manifest 声明)LZ4 压缩」→「预留 dump 区(manifest 声明)后 LZ4 压缩」。

## F12. `docs/5-test/19-test.md`

1. **L13–18（不变量表）**： `R-1`…`R-6` → `INV-1`…`INV-6`；**INV-4 行**→「| **INV-4** | 错误码 ∈ core 域负 errno 子集(扫描验证) | 08 §11(SD-10 core 域子集) |」（G-D1/G-G3）；INV-5 依据保持「08 §4」（08 §4 将补单调性语义， 见 F4-23）。
2. **L45** 「(R-2)」→「(INV-2)」；**L55** 「(R-1)」→「(INV-1)」；**L58** 「(白名单 R-3)」→「(白名单 INV-3)」；**L68** 「(R-5)」→「(INV-5)」；**L77** 「(白名单 R-3)」→「(白名单 INV-3)」；**L90** 「(R-6)」→「(INV-6)」；**L109** 「(R-3 执法)」→「(INV-3 执法)」。
3. **L69（TC-TIME-002）**「| TC-TIME-002 | 单位校准: sleep(1s) 对 host 参考钟 | 0.9–2.0s(CA-1 微秒) | ALL | host |」→「| TC-TIME-002 | 单位校准: sleep(1s) 对 host 参考钟 | 1.0–2.0s(CA-1 微秒; 校准容差防单位量级错, 不早醒由 INV-2/TC-TASK-004 执法) | ALL | host |」。
4. **L99（TC-MM-003）**「`-ENOSYS`(签名在, 实现未到——R4 冻结验证)」→「`-ENOTSUP`(签名在, 实现未到——主文档风险 R4 的签名先行验证)」（G-D2/G-G3）。
5. **L121** 「**规模**: ~40 用例 × 3 调度器 ≈ 120 矩阵运行…」→「**规模**: 43 用例 × 3 调度器 = 129 矩阵运行…」（G-F1）。
6. **L127** 「**sched_class 双保险**(D10): 插件声明 `SAFE_PREEMPT` ⇒ 其用例必须在 preempt 矩阵全绿; coop 下跑“抢占安全”用例失败 ⇒ 声明为假, 组合期拒绝」→「**sched_class 执法**(D10 双保险 = 组合期校验 + 静态分析; conformance 矩阵 = R1 的执法载体): 插件声明 `SAFE_PREEMPT` ⇒ 其用例必须在 preempt 矩阵全绿——矩阵失败 = CI 门禁红、阻断升格; 声明被判为假后应改声明为 COOP_ONLY(改后与 preempt 组合才在组合期被拒)」（G-C1）。

## F13. `docs/6-vfs-device/04-vfs.md`

1. **L4** 「`tg_open` 挂载表单路由」→「`tg_open` 挂载表·单路由」（G-G1）。
2. **L5** 「本篇决策 SD-1/SD-3/SD-4/SD-7(全局编号, 分篇列出)」→「本篇决策 SD-1/SD-3/SD-4/SD-7/SD-15(全局编号, 分篇列出)」（G-E7）。
3. **L30** `tg_file_t *tg_open(const char *name_or_path, uint32_t flags);` → `tg_file_t *tg_open(const char *path, uint32_t flags);`。
4. **L44** 「驱动声明 `TG_CDEV_F_EXCLUSIVE` → 二次 open 返回 -EBUSY(uart 类典型)」→「独占设备(uart 类典型): 驱动在 `ops->open` 会话建立时自行返回 `-EBUSY`(独占性策略归驱动, cdev 注册契约不设 flags 载体——`06-device` §3)」。
5. **L54** `/* 卷级 sync(fsync 的上游) */` → `/* 卷级 sync(对应 POSIX sync, 与文件级 fsync 并列) */`。
6. **§1 `tg_file_ops` 结构体（L25 `poll` 槽位后）** 加预留槽位： `int     (*poll_attach)(tg_file_t *f, void *wait);  /* v2 预留: wait-queue 注册(NULL → -ENOTSUP, D22/D14 预留即免布局破坏) */`
7. **L103（§3 v2 行）** 「**v2**: `file_ops` 增加 `poll_attach`(wait-queue 注册), 精确唤醒——…」→「**v1 预留 / v2 实现**: `tg_file_ops` 自 v1 即预留 `poll_attach` 槽位(NULL → -ENOTSUP, 与 `tg_dentry_ops` 同法——D22/D14: ops 布局入 golden, 后补字段 = 布局破坏); v2 实现 wait-queue 注册与精确唤醒——**必须与 preempt 同期**(否则抢占产品上轮询延迟毛刺, 见 R-S1)」。
8. **L109（高）** 「- **依赖 vfs-core(D19)**: open()→`tg_open` 的底层原语; 设备路径由 vfs-core 路由至 dev-core」→「- **依赖 vfs-core(D19/D21)**: open()→`tg_open` 的底层原语; 设备路径经挂载表路由至 fs/devfs, 由 devfs 经 open_file 钩子接入 dev-core 侧(§4.2)」。
9. **L152（SD-4 行）** 「v1 静态挂载(manifest 生成配置), 运行时挂载 v2 | 热插拔需要事件模型, 与 USB 栈同期」→「v1 静态挂载(manifest 生成配置); 运行时挂载: 事件模型 v2 设计、落地不早于 v3(O-S3) | 热插拔需要事件模型, 与 USB 栈同期」（G-A3）。
10. **L162（O-S3 行）** 「运行时挂载/热插拔事件模型——与 USB 栈、v2 事件系统联动」→「运行时挂载/热插拔——事件模型 v2 设计(与 USB 栈联动), 挂载落地不早于 v3」（G-A3）。

## F14. `docs/6-vfs-device/05-bdev.md`

1. **L5** 「本篇决策 SD-2(bdev 侧)/SD-8/SD-9。」→「本篇决策 SD-8/SD-9; 另涉 SD-2 的 bdev 侧(决策记录见 `06-device` §7)。」
2. **§1 代码块后（L34 段后）** 加： 「- **成功返回值**: `read/write` 成功返回传输扇区数(§5 时序图「返回 n_lba」即此约定); 失败 = SD-10 负 errno」。
3. **L51** 「**写穿优先**(v-x)」→「**写穿优先**(vx.0)」。
4. **L52** 「(`07-concrete-fs` §3)」→「(`07-concrete-fs` §5)」（G-E3）。
5. **L57** 「(R4: v1 冻结签名)」→「(R4: 签名 v1 起定稿; 升格 frozen 走 D15)」（G-A5）。

## F15. `docs/6-vfs-device/06-device.md`

1. **L5** 「本篇决策 SD-2/SD-5/SD-6/SD-10/SD-11/SD-12。」→「本篇决策 SD-2/SD-5/SD-6/SD-10/SD-11/SD-12/SD-13/SD-14。」（G-E7）
2. **L13（§1 表 dev-core 行， 依赖列）**「core native」→「core native + vfs-core(类型, 仅头文件——open_file 钩子引用 `tg_file_ops`, §2)」。
3. **L16** 「`tg_open` 挂载表单路由」→「`tg_open` 挂载表·单路由」（G-G1）。
4. **L23** 「cdev-core(钩子就序)」→「cdev-core(钩子就绪)」。
5. **L40（§1 图 VFSC 节点）**「tg_open(挂载表单路由)」→「tg_open(挂载表·单路由)」（G-G1）。
6. **§1 图（L62 `CDEVC --> VFSC` 行后）** 加边： `DEVC --> VFSC : 类型(open_file 钩子引用 tg_file_ops)`。
7. **L77（存储栈图 VFS 节点）**「tg_open 挂载表单路由」→「tg_open 挂载表·单路由」（G-G1）。
8. **L137** 「flash 是**字符型介质**(按地址 program/erase, 无扇区抽象)」→「flash 是**字符型介质**(按地址 program/erase, 无磁盘式扇区抽象——擦除以 block 为粒度, 见 `tg_flash_geom_t`)」。
9. **L157** `uint32_t read_size, prog_size;   /* 页大小 */` → `uint32_t read_size, prog_size;   /* 最小读粒度 / 编程页大小 */`。
10. **L177** 「**统一预留槽位(D22)**: 所有设备类别 ops 预留 `ioctl` / `suspend` / `resume`(设备级); file 面向的 `poll` / `close` 由 devfs 适配层(cdev-core 通用 `tg_file_ops`)提供。NULL = -ENOTSUP。」→「**统一预留槽位(D22)**: 所有设备类别 ops 预留 `ioctl` / `suspend` / `resume`(suspend/resume 恒为设备级; ioctl 于 cdev 为会话级(`sess` 参数)、于 bdev/flash 为设备级); file 面向的 `poll` / `close` 由 **cdev-core** 通用 `tg_file_ops` 适配层提供(devfs 经 open_file 钩子取得该适配)。NULL = -ENOTSUP。」
11. **L185** 「conformance 静态检查执法(D10/R1)」→「静态检查执法(D10 双保险之一; conformance 矩阵 = R1 执法)」（G-C1）。
12. **L186（SD-10 错误模型）**→「- **错误模型**(SD-10): int 返回, **负 errno**。SD-10 全集(两域并集) = `-EIO/-EAGAIN/-EINVAL/-ENOMEM/-ENODEV/-ENOTSUP/-EBUSY/-EEXIST/-ETIMEDOUT/-ENOSPC/-EROFS`, 各域取子集; **设备/存储域子集**: `-EIO/-ENODEV/-ENOSPC/-EINVAL/-ENOTSUP/-EBUSY/-EROFS`(core 域子集见 `docs/2-os-core/08-core-api-list.md` §11); svc-posix 直接取 `-ret` 作 errno(零转换)」（G-D1）。
13. **L187** 「并发: 设备 ops 由驱动自锁(v1 单锁即可); FS 侧见 `07-concrete-fs`」→「并发: 设备 ops 由驱动自锁(v1 单锁即可); FS 侧并发 v1 未另行约定(单 APP + 驱动自锁)」（G-E5）。
14. **L196** `/* type = 子分类字母: 'u' uart, 'c' can, 'b' bdev, 'f' flash, 'd' display ... */` → `/* type = 设备/驱动族魔数字母(每驱动族一个, 全局唯一分配): 'u' uart, 'c' can, 'b' bdev, 'f' flash, 'd' display ... */`。
15. **L219（SD-2 行）**「flash 是字符型介质(无扇区抽象)」→「flash 是字符型介质(无磁盘式扇区抽象)」。
16. **L222（SD-10 行）**「| SD-10 | 错误 = 负 errno 子集, svc-posix 零转换 | 单一错误空间 |」→「| SD-10 | 错误 = 负 errno(SD-10 全集 + 各域子集, 设备域 7 码见 §4), svc-posix 零转换 | 单一错误空间 |」。
17. **L226（SD-14 行）**「(poll/close 由 devfs 适配层)」→「(poll/close 由 cdev-core 通用 tg_file_ops 适配层提供, devfs 经钩子取得)」。

## F16. `docs/6-vfs-device/07-concrete-fs.md`

1. **L13（总览表 devfs 写路径）**「只 open/poll(设备语义)」→「n/a——read/write/ioctl 转发设备 ops(设备语义)」。
2. **L32** 「unlink/mkdir → -EOPNOTSUPP(…)」→「unlink/mkdir → -ENOTSUP(…)」（G-D3）。
3. **L33** 「devfs 只依赖 dev-core(枚举/钩子协议)与 vfs-core(挂载)——**不依赖任何子分类形状**」→「devfs 依赖 dev-core(枚举/钩子协议)、cdev-core(init 依赖, 保证钩子就绪)与 vfs-core(挂载)——**不依赖任何子分类的 ops 形状**」。
4. **L49** 「XIP(NOR 零拷贝执行)是诱惑但 cache 一致性代价未评估(O-S1)」→「XIP(NOR 零拷贝执行)是诱惑但 cache 一致性代价未评估(通路前提见 O-S1)」。
5. **L62** 「fs/littlefs → {vfs-core, fs/tmpfs, cdev-core}」→「fs/littlefs → {vfs-core, fs/tmpfs, cdev-core + bdev-core(QEMU bdev 适配)}」。
6. **L63** 「运行时挂载(USB 热插拔) = v2 议题(O-S3, 与事件模型联动)」→「运行时挂载(USB 热插拔): 事件模型 = v2 议题, 挂载落地不早于 v3(O-S3)」（G-A3）。
7. **§7 时序图（L67–87）**： participant 增加 `participant "vfs-core" as VF`；`PX -> LF: file_ops.write(file, buf, 512)` → `PX -> VF: tg_file_write(file, buf, 512)` + `VF -> LF: fops->write(file, buf, 512)`；`LF --> PX: 512` → `LF --> VF: 512` + `VF --> PX: 512`；`PX -> LF: file_ops.fsync(file)` → `PX -> VF: tg_file_fsync(file)` + `VF -> LF: fops->fsync(file)`；`LF --> PX: 0` → `LF --> VF: 0` + `VF --> PX: 0`（svc-posix 不应跳过 vfs-core 统一入口直达 fops）。
8. **L94** 「设备 ops 统一预留 ioct/PM 钩子」→「设备 ops 统一预留 ioctl/PM 钩子」。
9. **L97** 「**SD-2/SD-12**(`06-device`): 子分类与 open_file 钩子是 devfs 的底座」→「**SD-2/SD-13**(`06-device`): 子分类与 open_file 钩子是 devfs 的底座」（G-E6）。
10. **L104（O-S1 行）**「EROFS-on-NOR 的 XIP 是仪表域诱惑, cache 一致性代价未评估」→「EROFS-on-NOR 的 XIP 是仪表域诱惑(通路前提: NOR 内存映射窗口直读 + 元数据经适配 bdev, 路径待设计), cache 一致性代价未评估」。
11. **L105（O-S3 行）**「但挂/卸载文件系统需 v2 事件模型」→「但挂/卸载文件系统需 v2 事件模型(设计议题), 运行时挂载落地不早于 v3」（G-A3）。

## F17. `docs/7-toolchain/13-toolchain.md`

1. **L11** 「**CLI `tg`**(§13): `tg add <plugin>`(依赖闭包)/ `tg build`(组合+链接)/ `tg run`(QEMU)/ `tg check`(校验)」→「**CLI `tg`**(主文档 §13): `tg init`/ `tg add <plugin>`(依赖闭包)/ `tg build`(组合+链接)/ `tg run`(QEMU)/ `tg test`(conformance 运行器, 本篇 §2 第 6 项); `tg check` 等其余命令见 §2 大纲 [?]」。
2. **L23** 「5. golden 生成器: 头文件解析 → `api/frozen/*.txt`; abidiff 集成」→「5. golden 生成器: 构建产物符号表(`nm --defined-only`)→ `api/frozen/*.txt` + abidiff 布局比对(01 §2.6.4/08 §13.4: 真值是构建产物; 头文件解析仅用于 D13 模块→符号映射)」。
3. **L31** 「构建系统: make/cmake/自研 [?]——倾向最小依赖(make + 脚本)」→「构建系统: make/cmake/自研 [?]——倾向最小依赖(make + 脚本; roadmap §5 第 5 项为中性「构建系统选型论证」, 结论在选型论证时定)」。

## F18. `docs/8-app/14-app.md`

1. **L8** 「但它是终点(没有插件依赖 APP; 只有接口被它依赖)」→「但它是终点(没有插件依赖 APP; APP 只依赖 Interface(或直调 native))」。
2. **L11** 「APP 依赖 **Interface 插件**(严格叶子, §6.3); 可声明依赖服务(sqlite 类经 svc-posix, D18)」→「APP 依赖 **Interface 插件**(严格叶子, §6.3)或直调 native; 服务经注册表按名取用(§6.3; manifest 级 APP→Service 依赖是否允许 = 开放问题, 见 §3)」——D18 的内容是「三方中间件可声明依赖 svc-posix」, 不是 APP。
3. **L30（高）** 「“单 APP”边界: APP 内多线程/多进程语义不存在——文档化这个限制」→「“单 APP”边界: APP 内**多进程**语义不存在(无 fork/exec)——文档化这个限制; 多线程存在(§2 大纲第 5 条, 主文档 §2.1「调度框架存在的理由」)」。

## F19. `docs/9-interface/15-interface.md`

1. **L8** 「**严格叶子**: 没有任何插件依赖它们——这是依赖宪法(§7.2)的单向流保证」→「**严格叶子**: 除 APP 外没有任何插件依赖它们——这是依赖宪法(§7.2)的单向流保证(APP 亦是插件, 但它是唯一允许的依赖方)」（G-C4）。
2. **L13** 「接口只再导出」→「接口只做薄皮肤(再导出/别名 + 接线, 如 iface-min 直通 native), 不拥有状态」。

## F20. `docs/10-service/16-service.md`

1. **L15** 「服务清单: trace(M2)/ dbg-bridge(M3)/ svc-posix(M2)/ lwip(v2 ★)/ crypto(v2)/ modload+ed25519(v3)」→「服务清单: trace(M2)/ dbg-bridge(M3)/ svc-posix(M2)/ lwip(v2 ★)/ crypto(v2)/ ramdump(v2)/ modload+ed25519(v3)」。
2. **L19** 「errno 映射(-ret)」→「errno = -ret(零转换)」（G-D4）。

## F21. `docs/README.md`

1. **L19** 「**测试框架与 core-api 兼容性用例**(40 用例 × 三调度器矩阵)」→「(43 用例 × 三调度器矩阵)」（G-F1）。
2. **L31** 「驱动/FS/服务/接口/平台/调度器/APP 全部是插件。八类契约分散在领域文档」→「驱动/FS/服务/接口/平台/调度器/框架件/APP 全部是插件。八类契约分散在领域文档」（G-E11）。

---

## 验证清单（verification agents 用）

对每篇文档逐条核对： 评审文件（01–06）中涉该文档的每条意见（高/中/低全部）+ 本表对应条目是否已正确落实；检查是否有漏改、改错、或新引入的不一致；对明确漏改处做最小修正；输出逐条状态表（已修 / 部分修 / 未修 / 不适用）与总体结论。

## H. 验证后残留裁决（主代理在 21 个验证报告后追加）

21 篇验证全部「通过」后， 各验证代理标记的「待人工复核」项裁决如下（均已执行）：

- **H1 主文档「poll/close 由 devfs 适配层」三处（L5 变更记录 / L59 D22 行 / L630 §10 条目）**: 评审 04 号 L177 条目明示「主文档 D22 同句式」为同病 → 统一改为「poll/close 由 cdev-core 通用 tg_file_ops 适配层提供(devfs 经钩子取得)」， 与 06-device §3/§7 口径一致。
- **H2 01-api L25 `TG_DEPRECATED`**: 短式标注名与全篇规范名（§2.6.3 表、L114）不一致 → 改 `TG_API_DEPRECATED`。
- **H3 01-api L64 golden FUNC 行**: 按评审 01 号 L63/L89/L238「统一为 tg_thread_t, golden 记录示例同步」的明确意图 → `tg_thread**`/`tg_task_attr*` 全型名化为 `tg_thread_t**`/`tg_task_attr_t*`（与 08 §2 权威签名一致; G-B5 裁决文本漏 `_t` 属笔误）。
- **H4 15-interface L8「薄皮肤: 只做再导出与接线」**: L13 修复后同篇口径差（缺「别名」形态）→ 改「只做再导出/别名 + 接线」。
- **H5 08 三处裸 § 引用（L78「§5.1」/L165「§8」/L333「§7.2 规则 4」）**: 属 E8「L39/L105 等」同类项 → 全限定为「主文档 §5.1」「主文档 §8」「主文档 §7.2 规则 4」。
- **保留不改（评审未点名， 判定为合法/低优先）**: 12-plugin-mgr L12「(元契约, 冻结)」（D14 布局硬契约语境的合理用法）; 18-plugin-dev L31 验证列「叶子检查(无被依赖)」（检查项简称， 非 C4 目标句式）。

全局残留 grep 终检（两轮 40+ 模式）全部通过；仅有的命中均为合法子串（新宏名 `TG_TRACE_EVT`、主文档「互相需要」论述环诱惑、08 的 `__start_/__stop_` 机制注释）。
