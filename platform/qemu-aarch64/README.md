# platform/qemu-aarch64

> **它曾经不是插件**: 以前由 Makefile 直接把这一组文件编进镜像, 没有 manifest、没有
> 描述符、没有组合期校验 —— 欠债登记为 `WORKAROUND(br-wa-entry-001)`。本目录收敛为
> 插件后, 该 workaround 的 ① ② 两条已还清(见 `docs/decisions/0001-platform-plugin-manifest.md`),
> **③ 仍欠**: `start.S` 的调用点还应由 `.br_plugins` 段的枚举驱动(属 M0 运行期)。

QEMU virt(aarch64)平台插件: reset 汇编 + 异常向量桩 + 链接脚本 + PL011 早期 console +
arch timer + **GICv3 中断控制器 + 板级 IRQ 绑定表 + 目标侧一致性用例**。
设计出处: `1-03` §1 插件清单第一行(Platform, M0)、`3-02` 中断管理(Stage 1)、
`1-01` §8 的三层模式。

## 声明面(唯一真值)

| 项 | 值 | 出处 |
|---|---|---|
| `plugin_type` | `platform` | 每镜像恰 1(`1-01` §6.3 表 A) |
| `api_type` | `native` | 平台抛 native 面(§3.1) |
| `phase` | `core` | 第 ② 个完成点 = CORE(`§7.2` 判例: `sched-coop → platform` 两边 CORE, 不误杀) |
| 特权级别 | `P4`(`machine`) | MMU/页表/PIC/早期 console 属 platform(`§3.4`) |
| 接口单元 ① | `platform/qemu-aarch64#plat` | 14 个 `br_plat_*` 符号(平台契约 + 中断初始化 + 一致性入口) |
| 接口单元 ② | `platform/qemu-aarch64#gicv3` | 6 个 `br_gicv3_*` 符号 + INTID 分界宏 + 两个配置/信息类型 |
| 版本 | `0.1.0.0` | `COMPAT_GEN.MAJOR.MINOR.REVISE` |

## 三层归属(`1-01` §8 / `3-02` §1.3 在本目录的落点)

| 层 | 本目录里的文件 | 内容 |
|---|---|---|
| **数据与特化**(platform 数据) | `src/board_irq.c`、`src/plat_qemu_virt.c`、`include/br/board_irq.h` | virq ↔ INTID 绑定表、PIC 实例与 MMIO 基址、静态 prio/trigger、板级设备名 |
| **控制器方言**(设计归 ISA 共享库, 本原型同目录 + 文件边界) | `src/gicv3.c`、`include/br/platform/br_gicv3.h` | GICv3 寄存器序列: ack(1020–1023 折算)/eoi(EOImode=0)/mask/unmask/prio(向低优先级量化)/trigger(ICFGR + SGI)/affinity/PMR |
| **异常向量桩**(设计归 ISA 共享库) | `src/vectors.S` | 16 槽向量表 + 保存/恢复汇编(帧布局与 core 的 `br_exc.h` 同一处真值) |
| 平台入口与用例 | `src/start.S`、`src/irq_conf.c` | reset/BSS/TPIDR_EL1 初始化; `TC-IRQ-*` 的 in-image 一致性用例 |

⇒ 设计上 ISA 库是**独立于插件的一层**; 本原型还没抽出该层, 欠债登记为
`WORKAROUND(br-wa-isa-001)`(见根目录 `WORKAROUNDS.md`)。

## 目录

```
plugin.toml                          人写   ← 插件级唯一真值(BRV-D4); 声明两个接口单元
include/br/platform/br_plat.h        人写   ← Platform Entry 契约(含 IRQ 初始化/一致性入口)
include/br/platform/br_gicv3.h       人写   ← GICv3 方言契约(ISA 层)
include/br/board_irq.h               人写   ← 板级 virq 名(设计 3-02 §3.1 的**退路**: 静态头)
src/start.S                          人写   ← reset + BSS + br_irq_cpu_init(平台入口链第一跳)
src/vectors.S                        人写   ← 16 槽异常向量表 + 保存/恢复桩(ISA 层)
src/link.ld                          人写   ← 链接脚本(段布局 + .br_extable 收集 + .stack)
src/console_pl011.c                  人写   ← 早期 console(轮询 PL011)
src/timer_arch.c                     人写   ← arch timer 读数(CNTFRQ_EL0 / CNTPCT_EL0)
src/gicv3.c                          人写   ← GICv3 方言(实现 br_pic_ops_t)
src/board_irq.c                      人写   ← 绑定表 + br_plat_irq_init + timer PPI + 触发/hwirq 查询
src/irq_conf.c                       人写   ← in-image 一致性用例(TC-IRQ-*, 见 6-01 §3.7)
src/plat_qemu_virt.c                 人写   ← 平台实现(name/isa/early_init/park/异常落点)
tests/smoke.toml                     人写   ← v0.4 才被消费(§0 边界)
```

生成物(不在本目录): `build/gen/platform/qemu-aarch64/plugin_desc.c` —— 由
`brickie gen` 从 `plugin.toml` 重建; **v0.1 不编译它**(V-9: 零编译依赖),
它是声明面产物。

## 常用命令

```sh
brickie check --root .                                  # 声明面完备性(dev)
brickie check --root . --profile release                # 发布级门禁
brickie dep tree --kind all                             # 本插件在闭包里的位置
brickie iface list                                      # 两个单元的状态
brickie iface show platform/qemu-aarch64#plat           # 平台契约面
brickie iface show platform/qemu-aarch64#gicv3          # GICv3 方言面
brickie iface publish platform/qemu-aarch64#gicv3 --note docs/decisions/0002-*.md
brickie gen --root .                                    # 重建描述符; --check 只比对
```

镜像侧的两道门禁(真要跑 QEMU):

```sh
make smoke        # 启动 + MainLoop + 延时判据 + 中断心跳 + 一致性摘要
make irq-test     # 逐用例要求 PASS(TC-IRQ-*/GIC-*)
```

## 边界(诚实声明)

- v0.1 的 `brickie check` 只管**声明面**完备性: 不保证"编得过 / 跑得对"。上面
  `make smoke` / `make irq-test` 才是行为判据, 它们真跑 QEMU。
- **QEMU 机器型号必须钉 `gic-version=3`**(根 Makefile 的 `QEMUFLAGS`): QEMU virt 的
  缺省是 GICv2。镜像里的驱动写的是 GICv3 的 MMIO 与系统寄存器, 配错机器型号的症状是
  "PIC 初始化完毕却永远收不到中断"。
- 本插件里仍**不做**: 页表/恒等映射/region 表/cache 维护(`src/plat_qemu_virt.c` 文件头
  有逐条说明); 中断侧不做 Stage 2(分发到 bh/线程)、亲和性/均衡/IPI(v2b)、
  `CAP_NEST` 嵌套、`br_pic_save/restore`(PM)。
