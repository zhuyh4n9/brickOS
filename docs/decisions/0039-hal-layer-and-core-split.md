# 0039 — 分层重构：`hal` 成为一等类型（与 ability 平级），core 拆分为三个 HAL 砖

> 状态: **已落地**（`brickie check` 0 错 0 警；`brickie build` 153 步含 `ar` × 28 + 符号审计全绿；
> `brickie test -j8` 11 条 QEMU + 7 条宿主 + 11 条脚本门禁全绿；`make tools-test` 714/714；
> `brickie-core --selftest` 586/586）
>
> 落点: `tools/brickie/rust/src/{rules.rs,model.rs,show.rs,build.rs,check.rs,schema/*}`、
> **新增** `hal/{int,mm,sched}/`（3 个 HAL 砖）、`hal/*-core`（原 `framework/`，6 件改名）、
> `core/c_core/`（保留 boot 链 + base + plugin manager）、`runtime/core/brick.toml`（8 条 reexport 重指向）、
> `platform/qemu-aarch64`、`tests/gates.toml`、`product.toml`
>
> 回灌: 设计 `1-01` §6.2（层与相位）、`2-01` §7（声明面是唯一真值）、`4-02` §2（插件布局）；
> 契约 `docs/contract.md` §3（新码 `BRV-TAX-0025`）、§9 R-23/R-24

## 0. 一句话

**`hal` 从 `ability` 的 subkind 升为一等 `plugin_type`**（基础设施层，位于 ability 之下、platform 之上），
`framework/` 整体改名 `hal/`；`core/c_core` 按“机制”拆成 **`hal/int` + `hal/mm` + `hal/sched`** 三个砖，
core 只留**启动链 + base + 插件管理器**。消费者一行清单都不用改 —— 它们依赖的是皮肤 `runtime/core`。

## 1. 分层

```
platform/qemu-aarch64              SoC 层：GICv3 / PL011 / 页表 / 向量表（实现 HAL 契约）
      ↑  implements + calls
hal/int   hal/mm   hal/sched       ★ 新增：基础设施层 = 机制（中断框架/三池/调度框架）
hal/{dev,cdev,bdev,vfs,file-table,pci}-core   ← 原 framework/，改名不变内容
      ↑  [[dep]] 选中（ability selects hal）
core/c_core                        启动链 br_core_main + base(log/time/trace/string/panic) + 插件管理器 + svc
      ↑
runtime/posix · driver/* · fs/* · sched/{rr,coop} · service/*   ← ability = 具体实现
      ↑  [[dep]]
iface/{core,min,posix}             Interface 皮肤（APP 的唯一门）
      ↑
app/hello
```

`core → hal`、`hal → platform`、`ability → hal`、`platform → hal` 四条边都在方向表里；
**`hal → ability` 是禁止的**（基础设施不依赖具体实现），这是本刀新加的那条。

## 2. 为什么 `hal` 不能是 subkind

上一版把 `framework/` 当作 `ability` 的 `subkind = "framework"`。三个事实让这条路走不通：

1. **方向表按 `plugin_type` 判边**，subkind 不参与。要让 “platform 依赖 HAL、HAL 不依赖 ability”，
   必须让 HAL 在类型轴上与 ability **不同**，否则只能给 platform 开一个“依赖 ability”的口子。
2. **相位/生命周期按类型推导**（`derive_phase` / `init_phase_of`）。HAL 的初始化顺序由**启动链硬编码**
   （`br_irq_cpu_init` → platform early_init → `br_mem_init`/`br_mm_activate` → 相位机），
   写 `init` 边会让 `show brick-init-flow` 与真实顺序分叉；HAL 的属性是“无 init 钩子 + early”，
   与 ability 的 “core/late” 契约不同。
3. **语义**：ability 是“具体实现”，HAL 是“机制”。把机制塞进实现层的子类，等于让
   “选择”与“被选择”在同一个类型里 —— 用户本轮的原话就是“ability 选中 hal”。

落地代价只有一处：`plugin_type` 枚举加一个值，`NAMESPACES` 加 `hal`，方向表加两行。

## 3. 类型与命名空间（新）

| 项 | 值 |
|---|---|
| `plugin_type` | `app \| interface \| ability \| **hal** \| platform \| core` |
| `subkind` | `scheduler \| io \| fs \| service`（**删掉 `framework`**，subkind 从此只细分 ability） |
| 命名空间 | `app \| iface \| platform \| core \| **hal** \| sched \| driver \| fs \| service \| runtime` |
| 新码 | **`BRV-TAX-0025`**：命名空间与 `plugin_type` 必须一致（`hal/` ⇒ `hal`，`service/` ⇒ `ability` …） |

`BRV-TAX-0025` 是必要的：`hal/foo` 若声明 `plugin_type = "ability"`，方向表会按 ability 判它，
分层在纸上就破了。形状错（错类型）与语义错（错层）现在各有一个码。

## 4. core 拆分

| 砖 | 内容 | 单元（`[[export]]`） | RAM | 相位 |
|---|---|---|---|---|
| `core/c_core` | `main.c`（启动链）+ `base/`（log/trace/time/string/panic）+ `plugin/`（管理器 + svc） | 11：`log console time trace svc plugin types main error errno version` | 8 KiB | early |
| `hal/int` | `irq_core/irq_pic/irq_domain/irq_fault` | 4：`irq pic fault exc` | 3 KiB | early |
| `hal/mm` | `mem/page/tlsf/mm` | 2：`mem mm` | 2 KiB | early |
| `hal/sched` | `sched_core/switch.S/sync/work_core` | 3：`sched sync work` | 3 KiB | early |

* 头文件跟着砖走：`<br/core/br_irq.h>` → **`<br/hal/br_irq.h>`**（`br_pic/br_fault/br_exc` 同 `hal/int`；
  `br_mem/br_mm` 同 `hal/mm`；`br_sched/br_sync/br_work` 同 `hal/sched`）。`<br/core/>` 只剩 base + plugin + types。
* `[ver] 阶段`不变：所有砖 `version = "0.2.0.0"`。
* `switch.S` 与 TCB 布局（`sched_internal.h`/`sched_ctx.h`）**必须同砖**：汇编读 C 结构偏移，
  分开就多一个“两份真值”的共享头。
* `hal/sched` 的 `[build].selftest_sources = ["selftest/*.c"]`：三个套件（sched/sync/work）随框架搬走，
  入口 `br_sched_selftest` 等声明在 `<br/hal/selftest.h>`，由 core 的聚合器调用
  ⇒ 镜像仍只有**一个** selftest 驱动（ADR-0010），不重复跑。
* `hal/sched` **不写 `[selftest]` 块**：它没有套件入口，只有被聚合器调用的三个入口函数。

### 4.1 新增能力：按插件的 `selftest_sources`

以前 `[build].selftest_sources` 只在 `product.toml` 里、只编进 core 单元。现在**每个插件**都能声明自己的
（插件根相对），`product.toml` 的那份保留为 **core 的默认值**（兼容）。理由很直接：套件跟着实现走，
拆砖后 core 不能再替 `hal/sched` 声明它的测试源。

## 5. IRQ → 调度器的方向反转（拆分的唯一真障碍）

`hal/int` 的 IRQ 出口必须换栈重调度并跑下半部，而这两个机制在 `hal/sched`。若直接调用，
`hal/int → hal/sched → hal/int` 是双向依赖（`nm` 实测 4 个符号、1 个双向环）。

处置：**机制拥有者注册，框架分发**（与 `br_sched_ops_t`、`br_plat_*` 同款）：

```c
/* br_irq.h —— hal/int 只留槽位 */
typedef struct br_irq_hooks {
    void (*irq_exit)(void);                                  /* 换栈/重调度，可能不返回 */
    int  (*bh_submit)(void (*fn)(void *), void *arg);
    br_u32 (*bh_drain)(br_u32 max_items);
    br_bool (*in_bh)(void);
} br_irq_hooks_t;
void br_irq_set_hooks(const br_irq_hooks_t *hooks);
```

`hal/sched` 的 `early_init` 装上它（早期中断关闭，不存在半装状态被观察到的窗口）。
结果：`hal/int` **不再 include** 任何 `hal/sched` 头，`nm` 里那条双向边消失。

配套两个常量的归属也修正了（它们原来借 `hal/sched` 的头）：

* `BR_IRQ_BH_BUDGET`（每出口的 drain 上限）归 `hal/int`（出口策略），`hal/sched` 的
  `BR_WORK_BH_BUDGET` 变成它的别名 —— 一个数只有一个真值；
* 下半部是否编进镜像由 **`BR_IRQ_HAVE_BH`**（`hal/int`）表达，SLOW 域的降级点是它 ——
  原来用 `hal/sched` 的 `BR_WORKQ_DEPTH`，那是“跨砖偷常量”。

## 6. 消费侧：零改动

`runtime/core` 是 core 层的唯一门（ADR-0038），所以拆分对消费者**不可见**：

* 20 条 `reexport_of` 里，8 条改指向 `hal/{int,mm,sched}#{irq,pic,fault,exc,mem,mm,sched,sync,work}`；
* 皮肤的 `[[dep]]` 从 1 条变 4 条（`core/c_core` + 三个 hal 砖，`kind = "type"`）；
* **21 份消费者清单一行没改**，`iface/*`、`app/hello` 也没改。

`hal/*` 砖自己**不**走 `runtime/core` 皮肤：它们在 trusted base 内（与 platform 同层），
直接 `[[dep]] core/c_core`。皮肤的作用是“把 ability/interface 与 core 的身份解耦”，HAL 不需要这层解耦。

## 7. 与既有裁定的关系

| 既有 | 本条的影响 |
|---|---|
| ADR-0031（core 是第一类插件） | 不变：core 仍唯一、仍无生命周期；只是它的**机制**分到了兄弟砖 |
| ADR-0038（core 一等接口化 + 三条源码纪律） | 皮肤机制被复用为拆分的缓冲层；R1/R2/R3 在本刀继续执法（新增的 3 个 wrapper `.c` 就是靠 R1 才必须自带原型） |
| R-19/R-21（直接依赖授权） | 不变，但 `hal/**` 的 include 现在必须逐砖声明（本刀补了 5 条边：platform→3 hal、cdev/vfs→hal/mm、file-table→hal/int） |
| `BRV-TAX-0020`（core 唯一） | 不变：仍 1 个 core；HAL 是 3 个**独立类型**的砖 |
| `br-wa-isa-001`（ISA 库未独立） | 未动，但更清晰了：`hal/cpu` **不做成插件**（`switch.S` 必须和 TCB 布局同砖，`vectors.S` 在 platform），ISA 库仍是那条路 |

## 8. 验证

| 项 | 结果 |
|---|---|
| `brickie check`（五域） | 0 错 / 0 警 / 28 info（全是“再导出未冻结单元”，dev 档） |
| `brickie build` | **153** 步（原 144：+3 个 hal 砖 × 归档 + 编译 + 审计），符号审计 0 违规 |
| `brickie test -j8` | 11 QEMU + 7 宿主 + 11 脚本门禁全绿 |
| `make tools-test` | 714 / 714（快照 `show-bricks` 增 `hal` 一项） |
| `brickie-core --selftest` | 586 / 586 |
| 闭包 | 28 插件（+3 hal），RAM 173 KiB / 预算 256，类型分布 `ability 14, hal 9, interface 2, app 1, platform 1, core 1` |

## 9. 已知限制

1. **`hal/int` 的 SLOW 域降级**现在是“镜像级”开关（`BR_IRQ_HAVE_BH`），不是“选没选 `hal/sched`”的
   自动推导 —— 因为 `hal/int` 不可能 include `hal/sched` 的头去问它。真正的自动推导需要生成期把
   “闭包里有没有 hal/sched”变成 `-D`（一次 30 行的构建期改动），本刀没做。
2. **`hal/*` 的 `phase = "early"`** 表示“无 init 钩子”，它们的初始化顺序由 `br_core_main` 硬编码；
   `show brick-init-flow` 只按相位排序，因此 HAL 的**相对顺序不在那张图里**（如实登记）。
3. `plugin_manage` 仍是 core 的一部分：相位机是“驱动者”，不能由它驱动的表来初始化（见上一轮分析 §3.5）。
