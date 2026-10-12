# `adapter/` — uboot-mmc 的**brickOS 适配层**

> **一句话**: `src/` 是"上游 u-boot 的代码", `adapter/` 是"我们的代码"。
> 两者的边界不是靠约定, 而是靠**依赖方向**与**门禁**(`tools/check-third-party-isolation.sh`)。

## 为什么要有这个目录

本插件是 u-boot `drivers/mmc/**`(GPL-2.0+)的移植件。移植必然要接上本项目的运行环境
(日志/内存/时间/中断/信号量/块设备), 而"接环境"这件事**不该写进上游派生文件里** ——
否则:

- **归属不可判定**: 谁也说不清某个函数是上游的还是我们的, 将来换上游版本无法估代价;
- **隔离是假的**: `src/` 里散落十来条 `#include <br/...>`, "目录隔离"就只剩目录名;
- **审阅看不见边界**: 改动 review 时无法一眼看出"这行是移植的必要偏离还是我们的私货"。

## 依赖方向(单向)

```
        ┌─────────────────────────── src/ (上游派生, GPL-2.0+) ───────────────┐
        │  mmc_core.c   sdhci_host.c   mmc_regs.h   emmc_internal.h          │
        └───────────────────────────────┬────────────────────────────────────┘
                                        │ 只经 mmc_compat.h 取运行环境
                                        ▼
        ┌────────────────────────── adapter/ (本项目, adapter) ──────────────┐
        │  mmc_compat.h   ← 唯一 include <br/...> 的地方                     │
        │  emmc_bdev.c    ← bdev 对接 + PCIe 绑定 + 生命周期钩子             │
        │  emmc_selftest.c← TC-EMMC-*(经 bdev API 的端到端)                  │
        └────────────────────────────────────────────────────────────────────┘
```

**`src/` → `adapter/` 单向**。反向(`adapter/` include `src/` 的头)是允许的 ——
适配层当然要认识被适配的代码; 但 `src/` 只许经 `mmc_compat.h` 拿环境。

## 文件清单与归属

| 文件 | 归属 | 上游对应物 |
|---|---|---|
| `src/mmc_core.c` | 上游派生 | `drivers/mmc/mmc.c` + `drivers/mmc/mmc_write.c` |
| `src/sdhci_host.c` | 上游派生 | `drivers/mmc/sdhci.c` + `drivers/mmc/sdhci-adma.c` |
| `src/mmc_regs.h` | 上游派生(摘录) | `include/mmc.h` + `include/sdhci.h` |
| `src/emmc_internal.h` | 上游派生(改写) | `drivers/mmc/mmc_private.h` |
| `adapter/mmc_compat.h` | **本项目** | 无(u-boot 的环境由 u-boot 自己提供) |
| `adapter/emmc_bdev.c` | **本项目** | 无(上游对应物是 `mmc-uclass.c` 的 DM/blk 层, 未移植) |
| `adapter/emmc_selftest.c` | **本项目** | 无(u-boot 的 mmc 测试在 `test/dm/mmc.c`, 形态不同) |

## 判据(机械的, 见门禁)

`bash tools/check-third-party-isolation.sh` 对本件断言:

1. `src/` 与 `adapter/` 都在且非空;
2. **`src/**` 里的文件不出现 `#include <br/...>`**;
3. `third_party/` 之外无人 `#include` 本件的头(三方件是叶子);
4. **`src/**` 每个文件都带 `SPDX-License-Identifier`**(上游每个文件都有, 派生件丢了
   它就是署名回归)。

> ★ 门禁只判**边界在不在**, 不判"代码写得好不好"。它拦不住"把 brickOS 逻辑用一层
> 宏伪装进 src/"; 那要靠 review, 判据是 `mmc_compat.h` 是否只是**环境**。
