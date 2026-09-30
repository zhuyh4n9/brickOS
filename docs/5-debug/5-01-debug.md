# 5-01 — Debug 基础设施

> 章节: **5-debug**。对应评审意见: trace 插件(v1.0) / 类 adb debug bridge / mini ramdump(v2.0) / ASan·memleak。
> 总原则: **单异常级无硬件隔离 ⇒ debug 体系就是隔离的替代品**, 投入等级对齐契约治理(主文档 R7)。

## 缩略词(abbreviations)

| 缩略词 | 英文全称 | 说明 |
|---|---|---|
| **adb** | Android Debug Bridge | Android 调试桥(debug bridge 的类比对象) |
| **API** | Application Programming Interface | 应用程序接口 |
| **ASan** | AddressSanitizer | 内存错误检测器(host 平台插件上白捡, CI 常开) |
| **COBS** | Consistent Overhead Byte Stuffing | 帧定界编码(bridge 成帧用) |
| **CRC** | Cyclic Redundancy Check | 循环冗余校验 |
| **EC** | Exception Class | 异常类(ESR_EL1 字段, fault 定位用) |
| **ESR_EL1 / FAR_EL1** | Exception Syndrome / Fault Address Register | aarch64 系统寄存器: 异常类与出错地址 |
| **ETH** | Ethernet | 以太网(bridge 通道候选) |
| **I/O** | Input/Output | 输入输出; 插件类别 **IO** = 外设驱动 |
| **IRQ** | Interrupt Request | 中断请求(硬件中断线) |
| **ISR** | Interrupt Service Routine | 中断服务例程(中断上下文中的处理函数) |
| **LZ4** | LZ4 | 快速无损压缩算法(ramdump 压缩用) |
| **memleak** | memory leak | 内存泄漏(per-plugin arena 记账, v2.0) |
| **MMU** | Memory Management Unit | 内存管理单元 |
| **MPSC** | Multi-Producer Single-Consumer | 多生产者单消费者(无锁队列形态) |
| **QEMU** | Quick Emulator | 开源模拟器(v1 的主要验证平台) |
| **RAM** | Random Access Memory | 随机访问存储器 |
| **TCB** | Task Control Block | 任务控制块(任务的全部元数据) |
| **TLSF** | Two-Level Segregated Fit | O(1) 动态内存分配算法(此仓库的堆池实现) |
| **USB** | Universal Serial Bus | 通用串行总线 |
| **VFS** | Virtual File System | 虚拟文件系统(统一可打开模型 + 挂载表) |

> **编号约定**: `D4`/`D16` = 全局决策; `R7` = native API 冻结纪律(风险); `§11` = 主文档 debug 摘要。

## 1. trace 插件 (v1.0)

- 形态: Service 插件, `SAFE_PREEMPT`, **ISR 可用**(trace 最有价值的场景在中断里)
- 存储: 静态环形缓冲(manifest 定尺寸, 4–16KB 级), MPSC 无锁写
- **定长 16B 记录**(环无碎片、解码简单):

```c
typedef struct __attribute__((packed)) tg_trace_evt {
    uint32_t tsc;      /* 时间戳低 32 位, 回绕由解码器处理 */
    uint16_t evt;      /* 事件 id(由 manifest 分配) */
    uint8_t  ctx;      /* 0=ISR, 1..n=线程 id */
    uint8_t  narg;
    uint32_t arg[2];
} tg_trace_evt_t;
```

(4+2+1+1+8 = 16B, 2 的幂记录长度——环形缓冲索引可用掩码; 与主文档 §11 / 1-03-roadmap 的「16B」口径一致)

- 热路径**零字符串**: id→名字的映射表随插件元数据导出, 离线解码
- API: `TG_TRACE_EVT(id, a, b)` 宏(D16: 宏 `TG_*`; 3-01 §11 白名单引用此名); 未启用时编译期整层移除(**零开销**)
- 内建探针: 调度切换(coop 的事件点 / v2 抢占点)、IRQ 进出、插件生命周期、work 提交/执行
- 泄放途径: panic 时 console 倒出 / bridge 实时流 / host 工具解码

## 2. debug bridge(类 adb, v1.0(M3) 起最小集)

- 通道: UART 先行(console 复用); 后续 USB-CDC / ETH(各为 I/O / Service 插件)
- 成帧: COBS + 16-bit CRC, 请求/响应带 echo id
- 命令集: `GETINFO` / `MEMRD` / `MEMWR` / `TRACE_READ`(流式) / `PLUGIN_LIST` / `SELFTEST` / v2 起 `FILE_OP`(经 VFS)
- 主机侧: `tg dbg` 子命令, 可脚本化
- 关键设计: **panic 通道独立** —— 致命态(IRQ 锁死)下走"最后一口气"路径: 轮询 UART、不依赖任何插件栈/调度器/堆, 只允许 GETINFO / MEMRD / TRACE_READ

## 3. mini ramdump (v2.0)

- 触发: aarch64 同步异常(向量表分槽区分 fault 与 IRQ; `ESR_EL1`(EC)/`FAR_EL1` 定位 fault 细节)+ 显式 panic
- core 的 int 模块提供 `tg_fault_handler_register()`; ramdump 插件注册捕获; 默认 handler 只打印最小信息
- 捕获集: 寄存器组、TCB 全集 + 各线程栈、trace 环、内存 region 表、插件表、arena 统计
- 约束: 捕获路径**只用静态缓冲**, 不碰堆/调度器(已经崩了)
- 输出: 预留 dump 区(manifest 声明)后 LZ4 压缩, 或经 bridge 直传
- host 工具: 离线分析(线程时序对照 trace、fault 解码、栈回溯)

## 4. ASan / memleak 策略(分层, 诚实工程)

| 层 | 机制 | 版本 |
|---|---|---|
| **host** | host 平台插件跑 Linux 进程, `-fsanitize=address` **直接可用, 白捡** | v1.0 起, CI 常开 |
| target 便宜替代 | TLSF 红区(malloc header/footer guard)+ freelist 毒化 + 栈 canary | v1.x |
| target memleak | **per-plugin arena 记账**: 分配带插件归属, 关机/dump 时按插件出泄漏报告(与 D4 分段、二进制归属统计同源) | v2.0 |
| target 完整 ASan | 恒等映射 + 预留 shadow 区(RAM/8)——QEMU 内存宽裕; MMU 属性保护 shadow; 移植 `__asan_*` 运行时接口 | vx.0 实验 |

- memleak 归属实现 [?]: 显式 cookie(`tg_malloc_a(arena_id, size)`)或注册表映射, 细节待定
- 原则: **host 侧火力全开免费, target 侧只做便宜而确定的** —— 完整 ASan 是实验, 不是承诺
