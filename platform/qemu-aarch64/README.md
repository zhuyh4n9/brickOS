# platform/qemu-aarch64

> **它曾经不是插件**: 以前由 Makefile 直接把这一组文件编进镜像, 没有 manifest、没有
> 描述符、没有组合期校验 —— 欠债登记为 `WORKAROUND(br-wa-entry-001)`。本目录收敛为
> 插件后, 该 workaround 的 ① ② 两条已还清(见 `docs/decisions/0001-platform-plugin-manifest.md`),
> **③ 仍欠**: `start.S` 的调用点还应由 `.br_plugins` 段的枚举驱动(属 M0 运行期)。

QEMU virt(aarch64)平台插件: reset 汇编 + 向量表 + 链接脚本 + PL011 早期 console +
arch timer。设计出处: `1-03` §1 插件清单第一行(Platform, M0)。

## 声明面(唯一真值)

| 项 | 值 | 出处 |
|---|---|---|
| `plugin_type` | `platform` | 每镜像恰 1(`1-01` §6.3 表 A) |
| `api_type` | `native` | 平台抛 native 面(§3.1) |
| `phase` | `core` | 第 ② 个完成点 = CORE(`§7.2` 判例: `sched-coop → platform` 两边 CORE, 不误杀) |
| 特权级别 | `P4`(`machine`) | MMU/页表/早期 console 属 platform(`§3.4`) |
| 接口单元 | `platform/qemu-aarch64#plat` | 7 个 `br_plat_*` 符号(见 `[[export.entries]]`) |
| 版本 | `0.1.0.0` | `COMPAT_GEN.MAJOR.MINOR.REVISE` |

## 目录

```
plugin.toml                        人写   ← 插件级唯一真值(BRV-D4)
include/br/platform/br_plat.h      人写   ← Platform Entry 契约(对外面)
src/start.S                        人写   ← reset + 向量表(平台入口链的第一跳)
src/link.ld                        人写   ← 链接脚本(段布局 + .stack)
src/console_pl011.c                人写   ← 早期 console(轮询 PL011)
src/timer_arch.c                   人写   ← arch timer(CNTFRQ_EL0 / CNTPCT_EL0)
src/plat_qemu_virt.c               人写   ← 平台实现(name/isa/early_init/park/异常落点)
tests/smoke.toml                   人写   ← v0.4 才被消费(§0 边界)
```

生成物(不在本目录): `build/gen/platform/qemu-aarch64/plugin_desc.c` —— 由
`brickie gen` 从 `plugin.toml` 重建; **v0.1 不编译它**(V-9: 零编译依赖),
它是声明面产物。

## 常用命令

```sh
brickie check --root .                                  # 声明面完备性(dev)
brickie check --root . --profile release                # 发布级门禁
brickie dep tree --kind all                             # 本插件在闭包里的位置
brickie iface show platform/qemu-aarch64#plat           # 接口面(条目 + 版本 + hash)
brickie iface publish platform/qemu-aarch64#plat --note docs/decisions/0001-*.md
brickie gen --root .                                    # 重建描述符; --check 只比对
```

## 边界(诚实声明)

v0.1 只做**声明面**校验; `check` 不保证"编得过 / 跑得对"。本插件里 M0 刻意不做的
(GICv3 / 页表 / region 表 / cache 维护)在 `src/plat_qemu_virt.c` 的文件头有逐条说明。
