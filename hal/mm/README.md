# hal/mm — 内存机制（基础设施层）

> `plugin_type = "hal"`（ADR-0039）：只放**机制**，池的**容量**来自 platform 的 region 表。

## 内容

| 文件 | 内容 |
|---|---|
| `src/mem.c` | 三池（heap / contig / page / DMA）的领取与记账、`br_malloc/free/calloc/realloc` 系列、归属标签、红区 |
| `src/tlsf.c` | TLSF 分配器本体 |
| `src/page.c` | 页池位图与 `br_page_alloc/free` |
| `src/mm.c` | 地址映射 `br_mm_*`：ops 表由 platform 注册（恒等映射、cache 维护），本砖不命名任何 platform 插件 |
| `src/mm_plugin.c` | 插件壳：`br_mem_init()` / `br_mm_activate()` 由启动链阶段 3 调用（顺序硬编码在 `br_core_main`） |

公共头：`<br/hal/br_mem.h>` `<br/hal/br_mm.h>`（ADR-0039 之前是 `<br/core/…>`）。

## 声明面

| 项 | 值 |
|---|---|
| `plugin_type` / `api_type` | `hal` / `native` |
| `phase` | `early`（无 init 钩子） |
| `[[dep]]` | `core/c_core`（log/string/panic） |
| `[[export]]` | 2 个单元：`mem` `mm` |
| `[[res]]` | 2 KiB（控制块/位图/映射表的静态上界；**池本体**归 platform 的容量声明） |

## 边界

* 池**比例**写死在 platform 的 region 表里（`WORKAROUNDS.md` `br-wa-mem-001`：budget → region 的生成链路仍未做）；
* 页表**构造**在 `platform/src/mmu.c`，本砖只调 ops 表；
* 不含 `hal/mm` 以外的分配器（POSIX 的 `malloc` 别名在 `provider/posix`，最终落到这里）。
