# 0019 — pthread 面补齐(runtime/posix)+ POSIX 多线程样例 APP

> ★ **后续(ADR-0020)**: 文中 `iface-posix` 已改名为 **`iface/posix`**(头
> `<iface/posix/posix.h>`), 且 APP 的 core native 面改经 **`iface/min`** 皮肤取得; 旧名按
> 当时记录保留。
>
> 状态: **已落地**(`brickie check` 0 错; `brickie build` 通过; `posix-test` 28/0、
> `smoke` 含 `[APPCONF] 7/0` —— 见 §3)。
> 影响面: `runtime/posix/include/pthread.h`(面补齐)、`runtime/posix/include/errno.h`
> (**`errno` 改为宏 + `br_posix_errno()` 每线程槽位**)、`runtime/posix/src/posix.c`
> (线程记录扩展 / 惰性 detach 回收 / mutexattr / once / key / rwlock / barrier)、
> `runtime/posix/src/posix_selftest.c`(+8 用例)、`runtime/posix/plugin.toml`(导出面
> 0.1.1.0; `pthread_*` 条目 73 条; cases 20→28)、`api/iface/runtime/posix/posix.toml`
> (重新发布)、`app/hello/src/main.c`(**重写为 POSIX 多线程样例**)、
> `tests/gates.toml`(smoke 增 `[APPCONF]` 判据 + `APP-PT-*` 点名; posix-test 增 021–028)。
> 设计依据: `3-01` §2(线程)/§3(同步)/§4(时间)、`1-01` §7.3(APP 仅经 Interface)/D18、
> `11-01` §1/§2(POSIX 运行时子集)、`11-02`(TR-A/TR-B 为实现范围; TR-C 不做)。
> 相关: ADR-0014(POSIX 运行时落地; 本 ADR 更新它的 TR-C 边界)、ADR-0015(改名)、
> ADR-0018(APP 经 `iface-posix` 皮肤用 POSIX; 本 ADR 是它的第一个真实消费者)、
> ADR-0017(节拍 HZ —— `usleep`/`nanosleep` 的实际分辨率 = 一拍)。

## 1. 背景

ADR-0014 落地的 pthread 只是 **TR-B 最小集**: `create/join/exit/self/equal/yield`,
属性只有 stack/stacksize, mutex/cond 只有基础操作, 并且有两条**显式欠账**:

1. `errno` 是**全局 int** —— 多线程下"线程 A 的失败被线程 B 读走";
2. `pthread_detach` 返回 **-ENOTSUP** —— core 只有 `br_task_join` 一条回收 ZOMBIE 的路径;
3. 设计 `11-02` 的 TR-C 把 `pthread_key_*`/rwlock 列为不做(理由是"需要 core 的 per-thread
   通用槽位")。

用户要求: **pthread 面补齐**、**样例 APP 用 POSIX 开发**、**APP 里测多线程**。本 ADR 在
**不改 core** 的前提下把这些欠账收掉 —— 关键观察是: pthread 层**自己就有一张"每线程记录"
表**(为 retval/栈而存在), 它天然可以承载每线程 `errno` 与 TLS 槽位。

## 2. 决策(全部在 `runtime/posix` 层内, 不动 core)

### 2.1 每线程 `errno`: 宏 + 层内槽位

* `errno.h`: `int *br_posix_errno(void);` + `#define errno (*br_posix_errno())`
  (POSIX 本来就规定 `errno` 是宏; 删掉了原来的 `int errno;` 全局定义);
* `br_posix_errno()` 返回**当前线程记录**里的 `err_val`; 主线程等非 `pthread_create`
  线程在第一次用时**惰性登记**一条记录, 于是每线程语义对 APP 主线程同样成立;
* 极端情形(`br_task_self()` 不可用 / 记录表满)退化到一个静态兜底槽位 —— `errno`
  永远是合法可写左值。

### 2.2 线程记录 + 惰性 detach 回收

`svc_thread_t` 从 `{t, fn, arg, retval, stack, owns_stack}` 扩为
`{…, detached, err_val, name[16], key_values[8]}`。新增 `svc_reap_detached()`:
把 **detached 且 `br_task_state == ZOMBIE`** 的线程 `br_task_join` 掉、还栈、清记录;
在**每次进入 pthread 层**时调用一次(跳过当前线程自己)。

* `pthread_detach(t)` ⇒ 置位并返回 0;
* `pthread_join(detached)` ⇒ `EINVAL`(POSIX 未定义; glibc 同值);
* **回收时机不精确**(要有后续 pthread 调用), 如实登记; 语义"资源最终回收"成立。

### 2.3 mutex 属性: RECURSIVE / ERRORCHECK 在层内实现

`pthread_mutex_t` 在外层加 `type/owner/count` 三个字段(core 的 `br_mutex_t` 是普通互斥量):

* RECURSIVE: 属主重入 ⇒ `count++`(否则 core 会自死锁); 解锁到 `count==0` 才真正 unlock;
* ERRORCHECK: 同线程重锁 ⇒ `EDEADLK`; 非属主解锁 ⇒ `EPERM`; trylock 自持 ⇒ `EBUSY`;
* `pthread_mutex_timedlock`(MONOTONIC 绝对时刻 → 相对微秒)超时 ⇒ `ETIMEDOUT`;
* `pthread_mutexattr_{init,destroy,settype,gettype,setpshared,getpshared,setprotocol,
  getprotocol}`; `PTHREAD_PRIO_INHERIT/PROTECT` ⇒ `ENOTSUP`(无优先级, v2)。

### 2.4 cond / rwlock / barrier / once / key / name

| 面 | 实现 | 边界(如实) |
|---|---|---|
| `pthread_condattr_*` | `clock` 只接受 `CLOCK_MONOTONIC`(无墙钟, P-1); pshared 记录 | `setclock(REALTIME)` ⇒ `EINVAL` |
| `pthread_rwlock_*` | mutex+cond 之上实现; **writer-preference**(默认)避免写者饿死; rd/wr 的 try/timed 全有 | **不支持同线程递归读**(有写者等待时会自阻) |
| `pthread_barrier_*` | mutex+cond+generation; 恰好一个线程拿 `PTHREAD_BARRIER_SERIAL_THREAD` | — |
| `pthread_once` | `{br_mutex_t, done}` 静态可初始化; 持锁跑 init | init 内递归同一 once = 调用方违约 |
| `pthread_key_*` | 层内 8 个 key 槽 × 每线程记录; `key_create/delete/setspecific/getspecific` | `PTHREAD_DESTRUCTOR_ITERATIONS=4`, 退出跑析构 |
| `pthread_setname_np/getname_np` | 记录里的 `name[16]`; core 的 TCB 名不可改 ⇒ 只本层可见 | 纯诊断 |
| attr | `getstack/setstack`、`set/getdetachstate`、`set/getguardsize` | guard_size 只记录(core 无 guard 页) |

### 2.5 样例 APP: 只用 POSIX 面 + 多线程自判

`app/hello/src/main.c` 重写: 只 `#include <iface-posix/iface_posix.h>`(POSIX 面经 Interface
皮肤, ADR-0018), 线程/同步/延时全用 `pthread_*`/`sem_*`/`usleep`; 只有 `br_log_info`
(观测; `printf` 属 TR-C)与 `br_clock_now`/`br_clock_tick_count`(心跳判据读数)是 core 调用。

`hello_mainloop()` 开头跑 `app_pthread_conformance()`, 7 项自判(每项 `[APPCONF] PASS/FAIL`):

| tag | 内容 |
|---|---|
| `APP-PT-001` | 3 线程 × 500 次 mutex 保护计数 + barrier 起跑 + join 搬 retval |
| `APP-PT-002` | condvar 会合(主线程等全部 worker 就绪后 broadcast 放行) |
| `APP-PT-003` | `pthread_key_*` 每线程值隔离 + 退出析构 |
| `APP-PT-004` | detach: detached 线程照跑; join detached ⇒ `EINVAL` |
| `APP-PT-005` | 每线程 errno 隔离 |
| `APP-PT-006` | rwlock: 读者并发(`active_max>=2`)、写者独占 |
| `APP-PT-007` | `pthread_once` 恰一次 + `setname/getname_np` |

之后进入周期心跳循环(§1 的 smoke 判据不变: `tick=2 `、`us: ok)`、`irq_ticks=[1-9]`)。

## 3. 验证记录

| 判据 | 命令 | 结果 |
|---|---|---|
| 声明面 | `brickie check` | 0 错/0 警(1 提示: 被再导出单元未冻结, dev 允许); 闭包 17 插件 |
| 插件自检 | `brickie test posix-test` | `[POSIXCONF] SUMMARY pass=28 fail=0`(新增 021–028 全绿) |
| **APP 多线程样例** | `brickie test smoke` | `[APPCONF] SUMMARY pass=7 fail=0`; 心跳 `tick=2` / `us: ok)` / `irq_ticks=338` |
| 接口治理 | `brickie iface publish runtime/posix#posix` | 单元 **0.1.0.0 → 0.1.1.0**(ADDED, green); 快照/CHANGELOG/lock 落盘; hash 抄回 plugin.toml |
| 构建 | `brickie build` | 通过 |

## 4. 遗留(如实登记)

1. **`pthread_cancel` / cleanup handler** 不做: 取消点与清理栈需要额外的展开语义, 属 TR-C/后段。
2. **robust mutex**(`EOWNERDEAD`)/ `pthread_mutex_consistent` 不做。
3. **rwlock 的同线程递归读**不支持(见 §2.4); rwlock 无优先级继承。
4. **guard_size 只记录**: core 的栈是调用方给的连续内存, 没有 guard 页 ⇒ 设置成功但无保护效果。
5. **detach 回收时机惰性**(§2.2): 记录要等下一次 pthread 调用才回收; 长跑且不再调 pthread 的
   进程里, detached 记录会占到 TCB 用尽 —— 原型可接受, 真解是 core 的 `br_task_detach`(见
   ADR-0014 的 exit condition)。
6. **key 槽位固定 8** / 线程记录固定 8(与 `BR_TASK_MAX` 对齐): 不是 manifest 裁剪。
7. **每线程 errno 对 ISR/bh 的语义**: 中断上下文没有"线程", `br_posix_errno()` 会记到当时
   正在跑的线程上 —— POSIX 调用本就不该在 ISR 里做, 这里不额外防护。
8. 设计侧 `11-02` 把 `pthread_key_*`/rwlock 列为 TR-C(理由是需要 core 通用槽位); 本 ADR
   用**层内线程记录**绕开了那条前提。设计侧若要收编, 应更新 `11-02` 的 T4/TR-C 边界。
