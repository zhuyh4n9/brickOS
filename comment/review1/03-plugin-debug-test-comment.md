# 03-plugin-dev / 04-debug / 05-test 模块评审意见（文档表述质量）

> 评审对象: `tangramOS/docs/3-plugin-dev/18-plugin-dev.md`(82 行) / `docs/4-debug/03-debug.md`(54 行) / `docs/5-test/19-test.md`(144 行)
> 评审方式: 分模块 subagent 评审 + 主评审全文交叉复核（每条均已对照原文行号核实）
> 评审重点: 胡言乱语/技术性错误/前后矛盾/表达错误/格式引用
> 严重度: 高 = 误导设计决策或明显胡言乱语; 中 = 易误导读者或明显表达问题; 低 = 错别字/格式瑕疵

## 18-plugin-dev.md

### 中

- **L62** [前后矛盾] 「- init 时序: **依赖方** publish 在先、消费方 lookup 在后(init-DAG 保证)」
  - 权威表述在 17-service-mgmt.md L14:「发布时机 = **服务方** init; 查找时机 = 依赖方 init」。publish 的主语应是被依赖方（服务方），本句写成了「依赖方」；且在服务依赖关系中「依赖方」与「消费方」本为同一角色，句子把一个角色拆成两方，字面语义颠倒。
  - 建议: 改为「被依赖方（服务方）publish 在先、依赖方（消费方）lookup 在后」。

### 低

- **L4** [前后矛盾] 「驱动、文件系统、服务、接口、平台、调度器、APP 全部是插件。八类各有契约」
  - 枚举只有 7 类，漏「框架件」（dev-core/cdev-core/bdev-core/vfs-core，主文档 §6.3 第三类、v0.6 新增的第 8 类）；§2 路由表倒是齐全。→ 枚举补「框架件」。
- **L27** [表达错误] 「| **框架件** | 能力契约 API(注册表/ops 形状) | core / 互相 |」
  - 依赖列「互相」易读作「可相互依赖」，与主文档 §7.2「框架件间**单向**(cdev-core→dev-core 与 vfs-core, bdev-core→dev-core)」相悖。→ 改「core / 框架件间单向」。
- **L31–32** [前后矛盾] 「| **Interface** | 再导出皮肤 | svc-posix 等服务 |」「| **APP**(恰一) | main | Interface |」
  - 与主文档 §6.3 / 02-roadmap L67 不一致：iface-min 直通 native、依赖 core（不依赖任何服务）；APP 允许「依赖 Interface（**或直调 native**）」。路由表把依赖面收窄了。→ Interface 行注「svc-posix 等服务或 core(iface-min)」；APP 行补「或直调 native」。
- **L42** [表达错误] 「禁阻塞/malloc/持锁返回」
  - 主文档 §4.2 与 08 §8 原文为「持锁**跨 ISR** 返回」，压缩后语义含糊。→ 恢复全称。
- **L72** [格式引用] 「| 静态扫描 | ISR 白名单 / DMA 纪律 / 符号命名 | D10/R1 执法 |」
  - D10/R1 语义为 sched_class 双保险；ISR 白名单（CA-3）、DMA 纪律（R4）、符号命名（D13/CA-10）各有决策号，一并挂在 D10/R1 下不准确。→ 分别标注 CA-3 / R4 / D13·CA-10。

## 03-debug.md

### 高

- **L10 vs L13–19** [技术错误/前后矛盾] 「**定长 16B 记录**(环无碎片、解码简单):」
  - 紧随其后的结构体实为 **20 字节**：`tsc`(4) + `evt`(2) + `ctx`(1) + `narg`(1) + `arg[3]`(12) = 20（有无 `__packed` 均同——arg 天然落在 8 字节偏移、无填充）。而 L23 的 `TRACE_EVT(id, a, b, c)` 恰为 3 参数，`arg[3]` 应是有意为之——那么「16B」就是错的；主文档 §11 L629 与 02-roadmap L64 也都写「16B 事件环形缓冲」，即结构体与三处文字冲突，缓冲尺寸/记录布局会直接误导实现。
  - 建议: 二选一——`arg[2]` + 宏收两参（真 16B），或全链口径改 20B（同步改主文档 §11 与 roadmap）。
  - 附带: `typedef struct __packed tg_trace_evt` 中 `__packed` 是 ARMCC 关键字写法，GCC/Clang 应为 `__attribute__((packed))`；项目工具链为 GCC 系（QEMU/aarch64），建议改正。

### 中

- **L23** [前后矛盾] 「API: `TRACE_EVT(id, a, b, c)` 宏; 未启用时编译期整层移除(**零开销**)」
  - 08 §11 L284 引用同一宏时写作「`TG_TRACE_EVT` 宏(`docs/4-debug/03-debug.md` §1)」，一宏两名对不上；且按自家规范（08 §12「宏/常量 `TG_*`」、D16）无前缀拼写本就违规。
  - 建议: 统一为 `TG_TRACE_EVT`。
- **L37** [技术错误] 「aarch64 同步异常(向量表分槽, `ESR_EL1`/`FAR_ELx` 区分 fault 与 IRQ)」
  - 区分 fault 与 IRQ 靠的是**向量槽位**（前半句对）；ESR_EL1 的 EC 是对同步异常分类的综合征寄存器、FAR_EL1 只对 data/instruction abort 有效——二者用于**刻画 fault**，不承担「区分 fault 与 IRQ」的职责。括号把该职责安到 ESR/FAR 头上是技术性错误。附带: ESR_EL1 与 FAR_ELx 记法混用（EL1/ELx）。
  - 建议: 改为「向量表分槽区分 fault/IRQ；ESR_EL1(EC)/FAR_EL1 定位 fault 细节」。

### 低

- **L41** [表达错误] 「输出: 预留 dump 区(manifest 声明)LZ4 压缩, 或经 bridge 直传」——右括号后缺连接词，读作「声明)LZ4」。→ 加「后」或顿号。
- **L49** [前后矛盾·存疑] 「TLSF 红区…+ 栈 canary | v1.x」——本行与主文档 §11 一致、03 本身无错；但 02-roadmap L81 把「红区/金丝雀」列在 **v2.0** debug 行，两个 anchor 版本口径不一（详见 `06-cross-doc-consistency-comment.md`）。→ 对齐 roadmap，03 无需改。

## 19-test.md

### 高

- **L99** [前后矛盾] 「| TC-MM-003 | map/unmap(v1) | `-ENOSYS`(签名在, 实现未到——R4 冻结验证) | ALL | target |」
  - `-ENOSYS` **不在任何一份错误码子集里**：08 §11 的 core 子集（-EIO/-EAGAIN/-EINVAL/-ENOMEM/-ENODEV/-ENOTSUP/-EBUSY/-EEXIST/-ETIMEDOUT）没有它，06-device 的 SD-10 子集（-EIO/-ENODEV/-ENOSPC/-EINVAL/-ENOTSUP/-EBUSY/-EROFS）也没有；本篇自己的不变量 R-4（L16）就是「错误码 ∈ SD-10 负 errno 子集（扫描验证）」——该用例直接违反自家不变量。全库对「未支持」的既有约定是 -ENOTSUP（04-vfs L18、06-device L177、主文档 D22）。
  - 建议: 期望值改 `-ENOTSUP`，或走决策记录将 -ENOSYS 补入子集。

### 中

- **L127** [技术错误/前后矛盾] 「**sched_class 双保险**(D10): 插件声明 `SAFE_PREEMPT` ⇒ 其用例必须在 preempt 矩阵全绿; coop 下跑"抢占安全"用例失败 ⇒ 声明为假, 组合期拒绝」
  - (a) 主文档 D10 的「双保险」=**组合期校验 + 静态分析**，conformance 矩阵是 R1 中与之并列的第三条执法腿，本句把矩阵执法当成 D10 双保险内容（10-sched L12 亦把「双保险」用成「声明 + conformance」，术语已漂移）; (b)「组合期拒绝」混淆执法时序——矩阵失败是 CI/测试期发现，组合期校验只能比对「声明类别 × 所选调度器」，恰恰识别不出 SAFE_PREEMPT 声明为假（这正是 R1「声明可能撒谎」的前提）。
  - 建议: 改为「CI 门禁红/阻断升格」，拒绝路径写清楚（改声明为 COOP_ONLY 后与 preempt 组合才在组合期被拒）。
- **L16** [格式引用] 「| **R-4** | 错误码 ∈ SD-10 负 errno 子集(扫描验证) | SD-10 |」
  - SD-10 的权威定义在 06-device L186，是**设备域** 7 码子集（无 -ETIMEDOUT/-EAGAIN/-EEXIST/-ENOMEM）；08 §11 又以「SD-10 统一」给出另一份 9 码 core 清单——同号不同表。core-api 扫描按哪份？按 06 份会与 R-1 的 -ETIMEDOUT、TC-WORK-003 的 -EAGAIN、TC-SVC-002 的 -EEXIST 直接打架。且 L9 声称「每条对应 08 的既有决策」，SD-10 并非 08 的决策。
  - 建议: 依据改引「08 §11」，并全库统一 SD-10 口径（详见 `06-cross-doc-consistency-comment.md`）。
- **L47** [前后矛盾] 「| TC-TASK-006 | yield 语义: 就绪队列有另一任务时交错 | 交错发生(coop 强制切换点) | ALL | host |」
  - 标 ALL（三调度器全跑、L126 矩阵须全绿），但 08 §2 对 yield 的契约是「coop: 强制切换点; preempt: **提示**」——preempt 下（如就绪者是低优先级任务）不保证切换；sched-tt（v3 静态表）更由调度表决定。「交错发生」只在 coop 有契约支撑，作为 ALL 断言会让门禁非确定性闪红。
  - 建议: 该行注明 coop 断言强切换、preempt/tt 仅断言不死锁，或在 10-sched 明确 preempt 同优先级 yield 必切换。
- **L69** [前后矛盾·存疑]（主评审判级由低上调）「| TC-TIME-002 | 单位校准: sleep(1s) 对 host 参考钟 | 0.9–2.0s(CA-1 微秒) | ALL | host |」
  - 下界 0.9s 与 R-2「不早醒」及 TC-TASK-004「≥ 1ms」相抵（允许睡 1s 只睡 0.9s = 早醒 10%）。若本意是对 host 参考钟的**校准容差**（防 ms/µs 量级单位错），当前写法会被读成「早醒 10% 可容忍」，与不变量直接冲突。
  - 建议: 期望列注「校准容差，量级防错；不早醒仍由 R-2/TC-TASK-004 执法」，或下界改 ≥1.0s。

### 低

- **L121** [前后矛盾] 「**规模**: ~40 用例 × 3 调度器 ≈ 120 矩阵运行」
  - 逐组实数：TC-TASK 7 + TC-SYNC 9 + TC-TIME 3 + TC-WORK 4 + TC-MEM 7 + TC-MM 3 + TC-IRQ 7（001–005+101+102）+ TC-SVC 3 = **43** 例，43×3 = **129**，非 ~120。用例编号无跳号/重复（101/102 为刻意的域特有百位段，与 L133 规则一致），唯规模句数字与目录不符（README 索引又写「40 用例」，不带 ~）。→ 写实数「43 用例 × 3 调度器 = 129」。
- **L99 与 L11–18** [格式引用] 「R4 冻结验证」/「R-1…R-6」
  - 本篇定义不变量 R-1…R-6，主文档风险表为 R1–R9，仅一连字符之差；L99 又用裸「R4」（主文档风险、指签名冻结），同一文档两套 R 编号并存，极易误读为不变量 R-4（错误码子集）。→ 不变量改前缀（如 INV-*）或全文加注记。
- **L17** [格式引用] 「| **R-5** | clock_now 单调不减 | 08 §4 |」
  - 08 §4 只有类型定义与 CA-1 单位说明，并未载明「单调不减」约定，与 L9「每条对应 08 的既有决策」口径不符（其余五条依据均实存）。→ 回写 08 §4 语义规格，或标「本篇新增，待回写 08」。

## 一致性核对通过项（供置信）

- 18 的八类路由表与主文档 §6.3 一一对应；L34 全局文档号映射与实际目录**完全相符**；`06-device` §3/§6、`07` §6、01 §2.3 层 1/2/3、D22/NULL→-ENOTSUP、SD-3、白名单四件（08 §11）等引用均存在且一致。
- 03: trace = Service 插件/SAFE_PREEMPT/ISR 可用（08 §11 白名单含 trace 宏）；panic 独立通道命令子集（GETINFO/MEMRD/TRACE_READ ⊂ L31 命令集）自洽；ramdump v2.0 与主文档 §4.2/08 §8 的 fault 路径一致；ASan 分层表述技术上站得住（host `-fsanitize=address` 白捡、shadow≈RAM/8、`__asan_*` 运行时接口均正确），**未发现**「ASan 检测所有内存错误」之类原理性胡话；与用户手写笔记的 trace v1.0 / 类 adb bridge / mini ramdump v2.0 / asan·memleak 定位全部吻合。
- 19: 六条不变量其余依据（CA-4、08 §2.1 不早醒、CA-3、CA-6）实存；L20「08 §11 原缺 -ETIMEDOUT、已补入」**为真**（08 §11 确含该码并注明来自 19-test 前置补入）；01 §2.3/§2.6.2/§2.6.5 引用全部对得上；三调度器矩阵与主文档 §5.2 一致；TC-IRQ-101/102 的 PL061/M4 与 08 §8.1、roadmap M4 一致；bridge/ramdump/trace 上报机制与 03 各节一致。

## 总体评价

三篇文档骨架扎实、交叉引用密度极高且绝大多数可回溯（D/CA/SD 编号、§ 引用、目录归属基本对得上），**未发现成段的胡言乱语**，ASan/ramdump/服务时序等技术表述总体可靠。真正的问题集中在**数字与符号级的自相矛盾**：trace 记录 16B vs 结构体 20B、TC-MM-003 的 -ENOSYS 游离于自家错误码契约之外、TRACE_EVT/TG_TRACE_EVT 一宏两名、18 L62 服务 publish 时序主语颠倒——这类问题都会直接进入实现与 CI 门禁，是最优先修复项；其次是术语漂移（D10「双保险」语义漂移、SD-10 同号两表、R/R- 双编号）造成的引用歧义。

**问题统计: 高 2 / 中 7 / 低 10，共 19 条**（其中存疑 2 条: 03 L49 属 anchor 间版本冲突、19 L69 属校准容差语义不明；TC-TIME-002 严重度由 subagent 的低上调为中）。
