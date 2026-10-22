# hal/sched — 调度框架（基础设施层）

> `plugin_type = "hal"`（ADR-0039）：**框架**在这里，**策略**在 ability 砖（`sched/rr`、`sched/coop`）。

## 内容

| 文件 | 内容 |
|---|---|
| `src/sched_core.c` | TCB 池、就绪队列、超时扫描、`br_sched_register/run`、IRQ 出口重调度 |
| `src/switch.S` | aarch64 上下文切换（**必须与 TCB 布局同砖**：汇编读 C 结构偏移） |
| `src/sync.c` | 互斥/信号量/条件变量/自旋锁（等待链嵌在 TCB 里，零分配） |
| `src/work_core.c` | 下半部 / 工作队列（bounded / FIFO / 非重入 / bh 禁令） |
| `src/sched_plugin.c` | 插件壳：`early_init` 安装 `br_irq_hooks_t`（IRQ 出口 + 下半部入口） |
| `selftest/*.c` | 三套件：`br_sched_selftest` / `br_sync_selftest` / `br_work_selftest` |

公共头：`<br/hal/br_sched.h>` `<br/hal/br_sync.h>` `<br/hal/br_work.h>` `<br/hal/selftest.h>`。

## 自检的归属

套件随实现搬到这里，但**驱动仍在 core**（ADR-0010：镜像只有一个自检 pass）：

* 源：本砖 `[build].selftest_sources = ["selftest/*.c"]`（仅当 `product.toml [selftest] enabled`）；
* 入口声明：`<br/hal/selftest.h>`；
* 调用：core 的 `br_core_selftest()`（聚合器）—— 所以本砖**不写** `[selftest]` 块，没有 `.selftest` 钩子。

## 声明面

| 项 | 值 |
|---|---|
| `plugin_type` / `api_type` | `hal` / `native` |
| `phase` | `early`（钩子必须在第一次中断之前装好；无 init 钩子） |
| `[[dep]]` | `core/c_core`、`hal/int`（`br_irq_set_hooks` 与 `BR_IRQ_BH_BUDGET`） |
| `[[export]]` | 3 个单元：`sched` `sync` `work` |

## 策略砖怎么接

`sched/rr` / `sched/coop` 是 `ability`（`subkind = "scheduler"`），在 early 相调用
`br_sched_register(&ops)`（恰一次，二次注册 = panic）；框架在启动链末尾 `br_sched_run()` 接管。
