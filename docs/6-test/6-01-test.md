# 6-01 — 测试框架与 core-api 兼容性用例

> 章节: **6-test**。状态: **用例目录成文**; 运行基建/矩阵细则为骨架。
> 来源: `docs/1-architecture/1-02-api-contract-governance.md` §2.3(三层门禁)/§2.6(管理机制——升格 frozen 的矩阵前置)、`docs/3-os-core/3-01-core-api-list.md`(每函数规格/CA 决策)、`docs/1-architecture/1-03-roadmap.md`(conformance APP)。
> 定位: 层 1(golden diff)与层 3(版本矩阵)是**管线**(1-02 §2.6.4, 无行为用例); **本篇 = 层 2 语义一致性套件的用例目录**——"编过了但行为变了"的机械抓手。

## 缩略词(abbreviations)

| 缩略词 | 英文全称 | 说明 |
|---|---|---|
| **ALL** | — | 测试矩阵口径: 三调度器(coop/preempt/tt)全部运行 |
| **API** | Application Programming Interface | 应用程序接口 |
| **APP** | Application | 应用(插件类别: 唯一业务逻辑, 恰一个, 不被任何插件依赖) |
| **ASan** | AddressSanitizer | 内存错误检测器(host 平台插件上白捡, CI 常开) |
| **CA-1–CA-10** | — | core native API 契约决策编号(3-01 §14) |
| **errno(E*)** | error number | POSIX 错误码; core 以**负值**返回 `-EINVAL`/`-EAGAIN`/`-ETIMEDOUT`/`-ENOTSUP`/`-EBUSY`/`-EEXIST`/`-EIO`/`-ENODEV`/`-ENOMEM`/`-ENOSPC` 等, 域用子集 |
| **FAST / SLOW** | — | 级联中断域双上下文契约(CA-9): FAST = ISR 内可读寄存器, SLOW = 仅下半部(bh)可读 |
| **FIFO** | First-In First-Out | 先进先出(队列语义) |
| **host / target** | — | host = 宿主机(同一套语义用例跑成 Linux 进程); target = 目标机(QEMU 或真硬件) |
| **HSM** | Hardware Security Module | 硬件安全模块(第二产品域; §3.9 TC-HSM 组 = v1.x/M5 样例的用例载体) |
| **INV-*** | invariant | 被测语义不变量编号(6-01 §1) |
| **ISR** | Interrupt Service Routine | 中断服务例程(中断上下文中的处理函数) |
| **MMU** | Memory Management Unit | 内存管理单元 |
| **PL061** | ARM PrimeCell GPIO (PL061) | ARM 通用 GPIO 外设(测试用级联中断域样例) |
| **POSIX** | Portable Operating System Interface | 可移植操作系统接口 |
| **QEMU** | Quick Emulator | 开源模拟器(v1 的主要验证平台) |
| **SD-1–SD-15** | — | 存储与设备域决策编号(7-01/7-02/8-01 §决策记录) |
| **SGI** | Software Generated Interrupt | 软件生成中断(核间/测试触发中断) |
| **TC-*** | test case | 测试用例编号(`TC-<组>-NNN`) |
| **TLSF** | Two-Level Segregated Fit | O(1) 动态内存分配算法(此仓库的堆池实现) |

> **编号约定**: `INV-*` = 不变量(§1); `TC-*` = 用例编号(`TC-<组>-NNN`); `CA-*`/`SD-*` = 决策记录。

## 1. 被测不变量(语义规则)

用例目录围绕六条不变量组织, 每条对应 3-01 的既有决策:

| # | 不变量 | 依据 |
|---|---|---|
| **INV-1** | 阻塞 API 超时统一: 任何超时到期 → **`-ETIMEDOUT`**(`ZERO` = 即时超时, 同码; svc-posix 层再映射 POSIX 的 EAGAIN/EBUSY) | CA-4 |
| **INV-2** | **不早醒**: sleep/超时到期不早于期限; 晚到无上界承诺(统计上报, 不判红) | 3-01 §2.1 |
| **INV-3** | ISR 白名单执法: 白名单外调用 = 静态扫描红 + debug 运行时断言 | CA-3 |
| **INV-4** | 错误码 ∈ core 域负 errno 子集(扫描验证) | 3-01 §11(SD-10 core 域子集) |
| **INV-5** | clock_now 单调不减 | 3-01 §4 |
| **INV-6** | `dma_addr == vaddr`(v1 恒等) | CA-6 |

> **本设计发现的契约缺口(已修)**: 3-01 §11 错误码子集原缺 `-ETIMEDOUT`(CA-4 的超时约定无对应错误码)——测试设计前置暴露, 已补入 3-01 §11。这正是"用例先行"的价值: 1-02 §2.6.5 规定 **frozen 升格前 conformance 用例必须先行**, 用例设计即契约评审。

## 2. 运行基建(骨架)

| 平台 | 跑什么 | 为什么 |
|---|---|---|
| **host 平台插件** | 全部语义用例(除 ISR/真时序类) | CI 秒级 + 完整 ASan 白捡(1-03) |
| **QEMU target** | ISR 类/中断时序类/级联域 | 真中断只存在于目标 |
| **conformance APP** | 用例表驱动: {id, 前置, 步骤, 期望, 适用调度, 平台} | 1-03 v1.0 插件清单的 `app/conformance` |

- 断言失败 → panic → **bridge 独立通道上报**(`docs/5-debug/5-01-debug.md` §2); v2 起失败即 ramdump(5-01 §3)
- 结果经 trace 事件上报(5-01 §1), 用例 id 进 trace——失败现场可回放
- 精度类用例(INV-2)输出**分布统计**(min/avg/max), 供性能回归参考, 不判红

## 3. core-api 兼容性用例目录

适用调度列: **ALL** = 三调度器矩阵全跑; 标注者只在对应调度器下有意义。平台列: host / target。

### 3.1 任务组(TC-TASK)

| id | 用例 | 期望 | 调度 | 平台 |
|---|---|---|---|---|
| TC-TASK-001 | create → self==句柄 → join 回收, exit_code 传递 | join 返回 0, code 一致 | ALL | host |
| TC-TASK-002 | attr 校验: NULL attr / 栈 < BR_STACK_MIN | `-EINVAL` | ALL | host |
| TC-TASK-003 | join 自身 / 二次 join | `-EINVAL` | ALL | host |
| TC-TASK-004 | sleep(1ms) 实测时长 | ≥ 1ms(INV-2), 晚到统计 | ALL | host |
| TC-TASK-005 | sleep_until(已过期期限) | 立即返回 0 | ALL | host |
| TC-TASK-006 | yield 语义: 就绪队列有另一任务时交错 | coop: 交错发生(强制切换点); preempt/tt: 仅断言不死锁(3-01 §2: yield 为提示) | ALL | host |
| TC-TASK-007 | 线程从 entry 正常返回(trampoline 路径) | 经 br_task_exit, 不崩 | ALL | host |

### 3.2 同步组(TC-SYNC)

| id | 用例 | 期望 | 调度 | 平台 |
|---|---|---|---|---|
| TC-SYNC-001 | mutex 互斥: 双线程交叉计数 10k 次 | 计数精确无丢失 | ALL | host |
| TC-SYNC-002 | lock_to(ZERO): 未锁 / 已锁 | 0 / `-ETIMEDOUT`(INV-1) | ALL | host |
| TC-SYNC-003 | lock_to(1ms) 于已锁 | `-ETIMEDOUT` 且耗时 ≥ 1ms | ALL | host |
| TC-SYNC-004 | sem init(2): take×2 后 take(ZERO) | 第三次 `-ETIMEDOUT` | ALL | host |
| TC-SYNC-005 | **ISR 内 sem_give** → 线程 take 醒来 | 唤醒成功(白名单 INV-3) | ALL | target |
| TC-SYNC-006 | cond signal → wait 返回 | 0(无虚假唤醒) | ALL | host |
| TC-SYNC-007 | cond broadcast: N 等待者 | 全醒且各自重取锁 | ALL | host |
| TC-SYNC-008 | cond_wait 超时 | `-ETIMEDOUT`, 持锁状态恢复 | ALL | host |
| TC-SYNC-009 | `BR_MUTEX_DEFINE` 静态 vs `init` 动态 | 行为等价(CA-2) | ALL | host |

### 3.3 时间组(TC-TIME)

| id | 用例 | 期望 | 调度 | 平台 |
|---|---|---|---|---|
| TC-TIME-001 | clock_now 连续 1e6 次 | 单调不减(INV-5) | ALL | host |
| TC-TIME-002 | 单位校准: sleep(1s) 对 host 参考钟 | 1.0–2.0s(CA-1 微秒; 校准容差防单位量级错, 不早醒由 INV-2/TC-TASK-004 执法) | ALL | host |
| TC-TIME-003 | deadline_from_now(近 UINT64_MAX) | 饱和不回绕 | ALL | host |

### 3.4 工作队列组(TC-WORK)

| id | 用例 | 期望 | 调度 | 平台 |
|---|---|---|---|---|
| TC-WORK-001 | submit×N 顺序 | FIFO 执行 | ALL | host |
| TC-WORK-002 | **ISR 内 submit** | bh 执行(白名单 INV-3) | ALL | target |
| TC-WORK-003 | 队列满(深度 D) | 第 D+1 次 `-EAGAIN` | ALL | host |
| TC-WORK-004 | work 内再 submit | 执行(队列未满) | ALL | host |

### 3.5 内存组(TC-MEM)

| id | 用例 | 期望 | 调度 | 平台 |
|---|---|---|---|---|
| TC-MEM-001 | malloc/free 往返 + 对齐 | ≥ BR_MALLOC_ALIGN | ALL | host |
| TC-MEM-002 | 碎片压力: 交错 alloc/free 后最大块仍可分配 | TLSF 有界碎片 | ALL | host |
| TC-MEM-003 | calloc 零化 / realloc 保内容 | 成立 | ALL | host |
| TC-MEM-004 | contig: align=4096 分配, free(ptr,size) 回收 | 地址对齐(CA-7) | ALL | host |
| TC-MEM-005 | page: alloc(3 页)连续, free 后复分配 | 成立(CA-8) | ALL | host |
| TC-MEM-006 | dma_alloc | `dma_addr == vaddr`(INV-6) | ALL | host |
| TC-MEM-007 | heap_usage 记账 | alloc/free 增减匹配 | ALL | host |

### 3.6 MMU 组(TC-MM)

| id | 用例 | 期望 | 调度 | 平台 |
|---|---|---|---|---|
| TC-MM-001 | region_add 重叠/重复 | `-EINVAL` | ALL | host |
| TC-MM-002 | cache_flush 于 DEVICE 非缓存区 | 0(无操作) | ALL | target |
| TC-MM-003 | map/unmap(v1) | `-ENOTSUP`(签名在, 实现未到——主文档风险 R4 的签名先行验证) | ALL | target |

### 3.7 中断组(TC-IRQ, target-only)

| id | 用例 | 期望 | 调度 | 平台 |
|---|---|---|---|---|
| TC-IRQ-001 | register(SGI)→enable→自触发 | ISR 运行, arg 传递, ack 由 core 桩完成 | ALL | target |
| TC-IRQ-002 | 重复 register | `-EBUSY` | ALL | target |
| TC-IRQ-003 | disable 期间触发不达, enable 后恢复 | 成立 | ALL | target |
| TC-IRQ-004 | irq_lock 嵌套: 内层 unlock 不开中断 | 外层 unlock 才开 | ALL | target |
| TC-IRQ-005 | ISR 内调 malloc(debug 构建) | 运行时断言(INV-3 执法) | ALL | target |
| TC-IRQ-101 | 级联域 FAST(PL061): 子中断分发/ack/逐子屏蔽 | 逐子正确(3-01 §8.1) | ALL | target(M4 [?]) |
| TC-IRQ-102 | 级联域 SLOW: demux 在 bh, 子 handler 线程上下文 | 双上下文契约成立 | ALL | target(M4 [?]) |

### 3.8 服务注册表组(TC-SVC)

| id | 用例 | 期望 | 调度 | 平台 |
|---|---|---|---|---|
| TC-SVC-001 | publish → lookup 往返 | ops 指针一致 | ALL | host |
| TC-SVC-002 | 重复 publish | `-EEXIST` | ALL | host |
| TC-SVC-003 | lookup 缺失 | NULL | ALL | host |

### 3.9 HSM 样例组(TC-HSM, v1.x/M5)

样例: `docs/9-app/9-02-hsm-sample.md`(D24); 本组 = 该样例的 conformance 载体(9-02 §11 A6)。适用调度 ALL; 平台按用例标注。

| id | 用例 | 期望 | 调度 | 平台 |
|---|---|---|---|---|
| TC-HSM-101 | host 命令环回: INFO 帧 → 响应帧, 帧 CRC/序号正确 | 响应字段一致, 无丢帧 | ALL | target |
| TC-HSM-102 | keyring 生命周期: GEN → ACTIVATE → SUSPEND/RESUME → DESTROY 后使用 | 状态机成立; 墓碑后使用 → `-ENODEV`(9-02 §5.1) | ALL | host |
| TC-HSM-103 | 策略: 用途位不符 / **越限调用** / 试导出不可导出密钥 | 拒绝 `-EPERM`(O-H8)且**必留审计** | ALL | host |
| TC-HSM-104 | 审计链: 追加 N 条后 `AUDIT` 自校验; 篡改 1 条 | 未篡改通过; 篡改失败 | ALL | host |
| TC-HSM-105 | 审计持久化: 重启后链连续(摘要链跨 boot) | 链连续, 序号不重置 | ALL | target |
| TC-HSM-106 | 置换等价: 同一用例子集在 `sched-coop` 与 `sched-preempt`(v2)下行为一致 | 结果一致(源码零改写, 9-02 §10 表 B) | ALL | host(**前置 v2.0**) |

> **M5 DoD 边界**: 上表 **101–105 属 M5**(9-02 §11 A6); **106 前置 sched-preempt(v2.0)**, 不属 M5 DoD —— 9-02 §1.3 已声明 sched-preempt 对样例是"置换维、非前置"。
> **A1(组合即产品)的执法方式**: 不设行为用例——它由**组合器机械执法**(`brickie check` 的插件清单 + 符号表 diff, 9-02 §11 A1), 属层 1/层 3 的管线产物而非层 2 语义用例(1-02 §2.3 分工)。

> **本组暴露的契约缺口(待收口)**: 策略拒绝需要 `-EPERM`, 现有错误码子集无权限码——见 `docs/9-app/9-02-hsm-sample.md` §4.2 O-H8 与 `docs/3-os-core/3-01-core-api-list.md` §11 的待收口说明(先例同 INV-1 的 `-ETIMEDOUT`)。

**规模**: 49 用例 × 3 调度器 = 147 矩阵运行(host 秒级 + target 分钟级); 其中 TC-HSM 组 6 例(101–105 属 M5, 106 前置 v2.0)。

> §3.1–§3.8(core-api 兼容性)合计 43 用例 × 3 = 129; §3.9(HSM 组, 域特有, 百位 1xx 编号)追加 6 例。

## 4. 三调度器矩阵与 sched_class 执法(D10)

- **矩阵** = {sched-coop, sched-preempt, sched-tt} × 全部适用用例(1-02 §2.3 层 2)
- **升格前置**(1-02 §2.6.2): EXPERIMENTAL→FROZEN 的整组升格, 矩阵必须全绿——**矩阵是门禁, 不是事后报告**
- **sched_class 执法**(D10 双保险 = 组合期校验 + 静态分析; conformance 矩阵 = R1 的执法载体): 插件声明 `SAFE_PREEMPT` ⇒ 其用例必须在 preempt 矩阵全绿——矩阵失败 = CI 门禁红、阻断升格; 声明被判为假后应改声明为 COOP_ONLY(改后与 preempt 组合才在组合期被拒)
- **tt 特有**(v3): 表驱动调度的**确定性断言**——同一调度表两次运行的时序事件序列逐字节一致 [?]
- 抢占类用例(PI 互斥/优先级反转)随 sched-preempt 落地追加(v2, 本目录不含)

## 5. 用例编写规范

- 命名 `TC-<组>-NNN`(百位以上 = 域特有, 如 TC-IRQ-1xx 级联域、**TC-HSM-1xx HSM 样例域**)
- 每用例五元组: {前置, 步骤, 期望, 适用调度, 平台}; 期望必须可机械判定
- **新 API 进 frozen 前必须有用例**(1-02 §2.6.5"用例先行")——用例 PR 与升格 PR 的先后由 CI 检查
- 用例禁止依赖时序巧合(除 INV-2 统计类); host 与 target 的差异显式声明

## 6. 开放问题

| # | 问题 |
|---|---|
| — | 级联域 conformance 夹具(PL061)提前到 M2, 还是随 M4(3-01 §8.1 [?]) |
| — | tt 确定性断言的粒度(事件序列逐字节 vs 关键序) |
| — | 性能基准(吞吐/延迟)是否并入本框架——倾向独立 benchmark 面, 与兼容性正交 |
