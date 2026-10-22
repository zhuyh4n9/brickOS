# hal/int — 中断框架（基础设施层）

> `plugin_type = "hal"`（ADR-0039）：与 `ability` **平级、位于其下**。只放**机制**，不放策略。

## 内容

| 文件 | 内容 |
|---|---|
| `src/irq_core.c` | 描述符池、CPU 局部状态、生命周期（ack → ISR → 单点 eoi）、三层屏蔽、风暴检测、下半部出口 |
| `src/irq_pic.c` | PIC 注册表（`br_pic_ops` 由 platform 提供）、绑定表 |
| `src/irq_domain.c` | 级联域（SLOW 域走 bh demux） |
| `src/irq_fault.c` | 同步异常：分槽/EC 分类/extable fixup/panic |
| `src/int_plugin.c` | 插件壳：两个空钩子（状态是 BSS，`br_irq_cpu_init()` 由启动链阶段 1 调用） |

公共头：`<br/hal/br_irq.h>` `<br/hal/br_pic.h>` `<br/hal/br_fault.h>` `<br/hal/br_exc.h>`
（ADR-0039 之前它们在 `<br/core/…>` 下）。

## 为什么 `hal/int` 不依赖 `hal/sched`

IRQ 出口必须换栈重调度（`hal/sched`）并跑下半部（`hal/sched`），而基础设施不能依赖上层。
处置是**机制拥有者注册**（与 `br_sched_ops_t`、`br_plat_*` 同款）—— `br_irq.h` 只暴露槽位：

```c
typedef struct br_irq_hooks {
    void (*irq_exit)(void);
    int  (*bh_submit)(void (*fn)(void *), void *arg);
    br_u32 (*bh_drain)(br_u32 max_items);
    br_bool (*in_bh)(void);
} br_irq_hooks_t;
void br_irq_set_hooks(const br_irq_hooks_t *hooks);   /* hal/sched 的 early_init 调用 */
```

未安装（boot 早期）是合法状态：出口直接返回被中断的线程，`br_irq_in_atomic()` 恒假。

## 两个常量的归属

| 常量 | 归属 | 理由 |
|---|---|---|
| `BR_IRQ_BH_BUDGET` | 本砖 | 每次出口 drain 的上限是**出口策略**；`hal/sched` 的 `BR_WORK_BH_BUDGET` 是它的别名（一个数只有一个真值） |
| `BR_IRQ_HAVE_BH` | 本砖 | “镜像里有没有下半部”是**镜像属性**：0 时 SLOW 域在建域处就被拒绝（`irq_domain.c`），不会“接受了却永不被 demux” |

## 声明面（唯一真值）

| 项 | 值 |
|---|---|
| `plugin_type` / `api_type` | `hal` / `native` |
| `phase` | `early`（无 init 钩子；初始化顺序由 `br_core_main` 硬编码） |
| `[[dep]]` | `core/c_core`（base：log/trace/panic/error） |
| `[[export]]` | 4 个单元：`irq` `pic` `fault` `exc` |

## 谁依赖它

`platform`（实现 PIC/异常向量并调用其 API）、`hal/sched`（IRQ 出口钩子）、
`hal/file-table`（`br_irq_lock`）、`core/c_core`（启动链阶段 1）、
以及经皮肤 `runtime/core#irq` 的所有消费者。
