# io/uart-pl011

QEMU virt 的 PL011 UART 作为 **cdev** 注册 ⇒ 设备出现在 `/dev/uart0`, 可经 `br_open` +
`br_file_*` 做设备语义的读写/ioctl/poll。**v1 是轮询 cdev, 不是中断 tty。**

> 设计出处: `1-03` §1 插件清单 —— "`io/uart-pl011` | I/O | PL011: 早期轮询 console
> (**M0: platform 早期 console, 不注册设备**) → **中断 tty (M2: 注册 cdev)**"; 本件落地的是
> M2 的 **形态 A**(`8-01` §1.2); `8-01` §1.1(体系分层: 器件 → 设备类 → dev-core →(devfs)→ VFS)、
> `8-01` §3(cdev 会话式 ops + D22 预留 `suspend`/`resume`)、`8-01` §4(SD-6 阻塞语义 / SD-10 负 errno /
> ISR 禁令)、`8-01` §5(ioctl 用 Linux 兼容 32 位编码, 本器件族魔数 `'u'`)、`8-01` §6(驱动编写者
> 契约: 实现 ops 表 + 三条纪律 + 声明资源)、`7-01` §1 末段(独占设备策略**归驱动**: uart 类在
> `ops->open` 里返回 `-EBUSY`)、`7-01` §3(SD-7 poll 分期: v1 = 查询就绪位, 等待侧"轮询 + sleep")。
> 逐条裁定(尤其"v1 为什么是轮询 cdev 而不是中断 tty")见 `docs/decisions/0009-vfs-storage-stack.md`
> §2.7 / §3 裁定 10, 并登记 `WORKAROUND(br-wa-io-001)`。

## 声明面(`plugin.toml` 是唯一真值)

| 项 | 值 | 依据 |
|---|---|---|
| `plugin_type` | `ability` | I/O 驱动是能力插件 |
| `api_type` | `native` | 消费者是 native 插件 |
| `subkind` | `io` | 器件插件 |
| `phase` | `core` | ability 且 subkind ≠ service ⇒ CORE |
| 依赖(init) | `framework/cdev-core` `>=0.1.0` | 注册点 `br_cdev_register` 属 cdev-core(`8-01` §6"驱动 → cdev-core"); 拓扑序保证框架件先于消费者初始化 |
| 依赖(type) | `framework/dev-core` `>=0.1.0` | 头文件用它的 ioctl 编码宏(`BR_IOR/IOW/IO`)与 `br_dev_name_valid`; 编码宏的归属是 dev-core(`8-01` §5) |
| 依赖(runtime) | `framework/vfs-core`(symbol `br_open_err`) | 器件一致性用例与真实消费者都经 vfs 的公开文件面访问, 本件**不开后门** |
| 导出面 | 单元 `uart-pl011`(`sha256:d893563c…`, `unfrozen`/`experimental`): **5 宏 + 1 类型布局**(自检入口已移出 `[[export]]`, 故无函数) | 器件契约: ioctl 编码 + info 结构 |
| `[selftest]` | `cases = 12`; 钩子 `uart_pl011_selftest()` 在描述符里, 由 core 在全部 `start()` 之后驱动 | `plugin.toml` `[selftest]`; ADR-0010 |
| 资源 | `ram` 1 KiB: 每会话 `cdev_file_t` + session ≈ 32 B(core 堆); 静态部分只有 ops 表与器件记录; **不声明 `stack`** | `[[res]]`; ADR-0009 §3 裁定 10: 平台的栈容量是给**插件自己的线程**的预算, 不开线程的驱动不该占 |
| `sched_class` | `SAFE_PREEMPT` | 阻塞 read 会 `br_task_sleep` |

导出面(消费者要用的**器件契约**): `BR_UART_PL011_DEV_NAME`(`"uart0"`)、`BR_UART_IOC_MAGIC`
(`'u'`)、`BR_UART_IOC_GET_INFO` / `FLUSH_RX` / `SET_BAUD` 三个 nr、`br_uart_pl011_info_t`
(`{base, refclk_hz, baud, rx_available, rx_dropped}`)。自检入口 `uart_pl011_selftest()`
**不在**导出面里 —— 它是描述符的 `.selftest` 钩子(与生命周期钩子同一条纪律), 见下。

## 实现什么(契约面 = `include/br/io/br_uart_pl011.h`)

- **注册**: CORE init 调 `br_cdev_register("uart0", &s_ops, …)` ⇒ 注册即出现在 `/dev`
  (`fs/devfs` 实时枚举 dev-core 注册表)。EARLY 相无动作(console 已由 platform 建好, 此刻无线程
  无堆); START 相只打一行就绪日志 —— 本器件的 `[IOCONF]` 套件已与生产分离, 由 core 在全部
  `start()` 之后驱动(见「一致性用例」)。
- **会话与独占性(策略归驱动)**: 会话 = `{flags, rx_dropped}`(本文件私有, 不透明地交给 cdev
  适配层); 已有会话时第二次 `open` 直接 **`-EBUSY`**, `close` 放开。不采"按 flags 允许多读者":
  v1 只有单 APP, 多读只会带来"谁吃掉这个字节"的歧义。
- **阻塞语义(SD-6/SD-7)**: `read` 在 FIFO 空时——非阻塞 ⇒ `-EAGAIN`; 阻塞 ⇒
  `br_task_sleep(1ms)` 后重试(**v1 没有超时**, 不假装知道对端何时打字; 超时兜底归上层)。
  `write` 逐字节等 `FR.TXFF` 清零, 是**忙等**不是 sleep(等的是自己刚写进去的数据往外挪一格,
  上界 ≈ FIFO 深度 × 位时间, 为它睡 1 ms 会砍吞吐——两种等待性质不同, 机制就不同)。
- **3 个 ioctl**: `GET_INFO`(`BR_IOR('u',0,…)`, 回答平台事实 + 当前 baud + RX 状态)、
  `FLUSH_RX`(`BR_IO('u',1)`, 抽干 RX FIFO)、`SET_BAUD`(`BR_IOW('u',2,u32)`, 校验可表示性后
  重算 IBRD/FBRD; 4 MHz(> refclk/16)与 0 都 ⇒ `-EINVAL`)。错魔数/未知 nr ⇒ `-ENOTSUP`
  (与"需要参数的调用给 NULL"的 `-EINVAL` 是两件事)。
- **overrun 计数的诚实来源**: 采样 **`UARTRSR[3]`**(接收状态)进会话的 `rx_dropped`, 并写
  `UARTICR.OEIC` 尽设备侧清源义务。**不采**"`FR[3]` + `ICR[3]`": ARM DDI 0183 里 `FR[3]` 是
  **BUSY**(发送中)而不是 OE, `ICR[3]` 是保留位——拿错位会把"正在移位输出"误记成丢字节。
- **`rx_available` 的诚实上限**: PL011 的 FR 没有 FIFO 水位字段(只有 RXFE/RXFF)⇒ 只报下界
  `0`(空)/ `16`(满, FIFO 深 16)/ 其余 `1`(至少 1 个); 报一个猜的中间值才是撒谎。
- **只复用不 bring-up**: 本驱动不重写 `LCR_H`(8N1 + FIFO 使能是 console 的线路设置), `CR`
  也只在 `SET_BAUD` 里"回读 → 清 → 写除数 → **原样写回回读值**", 不自己拼 `UARTEN|TXE|RXE`。
  `SET_BAUD` 因此会**连带改变内核日志/panic 通道的速率**(两个形态共用同一条 PL011)——风险写在
  头文件与 workaround 条目里, 套件用完后一定把 115200 写回。

**与 platform 早期 console 的关系(同一块硬件, 两种形态 —— 设计内, 不是重复)**:
`platform/qemu-aarch64/src/console_pl011.c` 是 M0 早期 console(轮询 putc, 形态 C standalone,
**不注册设备**)——内核日志与 panic 通道, panic 路径不许依赖插件栈; 本件是 M2 形态 A 侧。本驱动
对寄存器**只读** DR/FR/RSR/回读 IBRD/FBRD/CR, **只写** DR/IBRD/FBRD/CR(SET_BAUD)/ICR(清源),
绝不动 `LCR_H` 与探测期的 `CR`。反过来的做法(让本件拥有初始化、console 调它)会让 panic 通道
依赖一个可能没被组合进来的插件, 故不采。

## 边界纪律 / 不做什么

- **v1 不开 UART 中断**(`br-wa-io-001`): 不写 `IMSC`、不注册 ISR(绑定表里的 `BR_IRQ_UART0`
  无人注册)、没有 RX 中断驱动的唤醒; 阻塞 read 靠 `br_task_sleep(1ms)` 轮询(设计 SD-7 的 v1
  口径), `poll` 只回答"当前就绪位"、不做等待。也因此 v1 没有并发、不需要锁。M2 后半段落地中断
  tty 时, 本件的 **ops 形状不用改** —— 变的只是 read 的等待机制与 `IMSC`/ISR 的加入。
- **没有 tty 层**: 不做 line discipline / 行缓冲 / 回显; 若需要 tty 语义, 设计上另起
  `io/tty` 或 `service/tty`, **不塞进驱动**(`br-wa-io-001` 的还债动作 ②)。设备 `write` 字节透明,
  不做 `\n` → CRLF 转换(那是 console 的策略)。
- **不带自己的线程/栈**: 不声明 `[[res]].stack` —— ops 全部运行在**调用者的**栈上(消费者线程
  或 main 线程)。
- **不拥有线路设置**: 见上, `LCR_H` 绝不写、`CR` 用回读值恢复; 真产品应经 termios 层协调
  (console 是线路的主人)。
- **全部函数 thread-only**: 会 `br_malloc`/`br_free`/`br_task_sleep`、会自旋等 TX —— `8-01` §4
  的 ISR 禁令(设备/存储域 API 白名单为空)在本件成立。
- **不导出"器件动作 API"**: 打开/读写/ioctl 全部走 `br_file_*`(经 devfs + cdev-core 适配层);
  本件的对外面只有头里那些**器件契约**(ioctl 编码与 info 结构, 消费者要用)与一致性入口。
  `suspend`/`resume`(D22 设备级 PM 钩子)只占槽位, v1 无调用方(O-S6)。
- **平台数据是直写常量**: 基址 `0x09000000` / 参考时钟 24 MHz / 115200 8N1 暂时写死在驱动里
  (与 console 同源常量); "平台数据插件化"是后续刀。

## 一致性用例

`uart_pl011_selftest()` 在 `src/uart_selftest.c`, 只经**公开 API**驱动
(`br_stat` / `br_open_err` / `br_file_*`), 不看任何内部符号 —— 否则"设备经 devfs + cdev 适配层
真的能打开"这件事就没有判据覆盖。逐例打印 `[IOCONF] PASS/FAIL <tag> <desc>`, 末尾一行:

```
[IOCONF] SUMMARY pass=%u fail=%u total=%u
```

12 例 `TC-IO-001..012`: 节点可见且 `type = FILE` / 独占 `-EBUSY` / `GET_INFO` 回答平台事实 /
`write` 返回字节数 / 空 RX + 非阻塞 ⇒ `-EAGAIN` / `poll` 置 `POLLOUT` 且清未置位位 / 错魔数
与未知 nr ⇒ `-ENOTSUP` / `lseek`+`fsync` ⇒ `-ENOTSUP` / `close` 释放独占 / 需参数调用给 NULL
⇒ `-EINVAL` / `SET_BAUD` 可表示性校验与回读 / `FLUSH_RX` 后 `rx_available == 0`。
**每个用例自己开、自己关**(哪怕中途红了也收尾), 否则独占没释放会把"一个真 bug"变成"十二个假
bug"。由描述符的 `.selftest` 钩子被 `br_plugin_manager_selftest()` 在**全部 `start()` 之后**
统一调用(返回失败项数; 失败不停机); 套件要断言的平台事实(base / refclk / baud)从
`src/pl011_internal.h` 取 —— 那是生产与套件共用的**一份**真值, 不是两份复制品。
门禁 `fs-test` 要求 `\[IOCONF\] SUMMARY pass=[0-9]* fail=0 ` 且禁止 `[IOCONF] FAIL`。

## 目录

```
plugin.toml                        人写   ← 插件级唯一真值(本目录不改它)
include/br/io/br_uart_pl011.h      人写   ← 器件契约(名字 / ioctl 编码 / info 结构)
src/uart_pl011.c                   人写   ← ops 表 + 注册 + ioctl(只留驱动本体)
src/uart_selftest.c                人写   ← [IOCONF] 用例(由 core 驱动, 只走公开 API)
src/pl011_internal.h               人写   ← PL011 平台常量(生产与套件共用的一份真值)
README.md                          ← 本文件
tests/smoke.toml                   ← 声明面用例骨架(与 in-image TC-IO-* 同 id)
```

## 已知欠账(不在本插件目录可修)

- `br-wa-io-001`: **v1 是"轮询 cdev", 不是设计 `1-03` §1 写的"M2: 中断 tty"** —— 不开 UART
  中断、没有 RX 中断唤醒、没有 tty 层; 另外 `SET_BAUD` 会连带改变内核日志速率(与 platform 的
  早期 console 共用同一个 PL011)。还债动作: ① 注册 `BR_IRQ_UART0` 的 ISR + ISR→RX 环形缓冲
  →`br_sched_wake` 唤醒等待者, 把 read 从"轮询 sleep"改成"阻塞在等待队列上"; ② 若需要 tty
  语义则另起 `io/tty` 或 `service/tty`(line discipline 归它)。判据 = 无数据时 read 不占 CPU、
  有数据时被中断唤醒, 且日志里能看到 UART 中断计数。登记表见根目录 `WORKAROUNDS.md`。
- `br-wa-test-001`: `TC-IO-001..012` 是**自编号** —— 设计 `6-01` 的用例表里设备域整组不存在。
  还债动作: 先补出 `6-01` 的设备域用例组, 再把自编号改回正式编号。
- 平台数据(基址/时钟/除数)直写常量; `BR_IRQ_UART0` 已在 platform 的绑定表里但本件不用它
  (见上)。
