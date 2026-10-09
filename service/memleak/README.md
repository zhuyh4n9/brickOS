# service/memleak

> **泄漏 / 堆破坏报告服务**: 按"归属标签"出存活块账 + 红区/魔数/毒化违约计数。
> 本 README 由实现者维护(声明面 `plugin.toml` 是唯一真值, 不在这里复述)。

## 能力面(声明面 `include/br/debug/br_memleak.h` 的转述)

| 函数 | 一句话 |
|---|---|
| `br_memleak_init()` | LATE 相 init: 注册归属标签(`"memleak"`, 幂等), 返回 0 |
| `br_memleak_report()` | 打印 summary / 逐归属汇总 / 每归属 ≤ 8 条明细; 返回**存活块数** |
| `br_memleak_live()` | 存活块数(`br_heap_walk` 计数); 堆未就绪 ⇒ 0 |
| `br_memleak_live_by_owner(o)` | 某归属的存活块数 |
| `br_memleak_corruptions()` | **当前**违约数(= `br_heap_check()`, 每次调用重算) |
| `br_memleak_owner()` | 本插件自己的归属标签 |

上表是**对外声明面**。自检**不在**这里(ADR-0010 §2.5): 用例在 `src/memleak_selftest.c`,
入口 `memleak_selftest()` 是描述符的 `.selftest` 钩子, 由 **core** 的
`br_plugin_manager_selftest()` 在全部 `start()` 之后统一驱动 —— 跑 `TC-DBG-040..043`,
返回失败项数并打 `[DBGCONF] SUMMARY memleak pass=... fail=... total=...`(失败不停机)。
本插件**不需要** `src/memleak_internal.h`: 四条用例的被测对象全是对外面(本插件的观测函数
与 core 的堆契约), 生产实现里没有要为它们暴露的私有符号。

打印格式(契约):

```
[LEAK] summary live=<n> bytes=<n> owners=<n> corrupt=<n> high_water=<n>
[LEAK] owner=<name> live=<n> bytes=<n>
[LEAK]   blk seq=<n> addr=0x<hex> size=<n> caller=0x<hex>     # 每归属最多 8 条
```

## 设计出处

- `Design/docs/5-debug/5-01-debug.md` §4「ASan / memleak 策略(分层, 诚实工程)」:
  - **target 便宜替代(v1.x)**: TLSF 红区(malloc header/footer guard)+ freelist 毒化
    + 栈 canary —— 由 core 的堆实现出账(`br_heap_stats_t` 的 `redzone_hits` /
    `canary_hits` / `double_free` / `bad_free`), 本插件只呈现。
  - **target memleak(v2.0)**: per-plugin arena 记账 —— 分配带插件归属, 关机/dump 时
    按归属出泄漏报告。
- `Design/docs/3-os-core/3-01-core-api-list.md` §6(`br-heap` 观测与 owner 记账)。
- ADR-0003 §2.6(调试插件分工: memleak 只读 core 的堆观测契约)。

## ★ 诚实声明: "归属标签" ≠ v2 arena

本原型实现的是上面两行的**交集**: core 的便宜机制已到位, 而 v2 的 arena 只做到
**归属标签**(`br_heap_owner_register/set/get` + 块头里的 `owner` 字段):

| v2 arena 的要素 | 本原型 |
|---|---|
| 分配带插件归属 | ✅ 归属标签(块头记录分配时的 current owner) |
| 预算上限(每插件 arena 配额) | ❌ 没有 |
| 强制归属(未标归属即拒绝分配) | ❌ 没有(缺省 `BR_OWNER_NONE`) |
| OOM / 超预算策略 | ❌ 没有 |

所以本插件能回答"**哪些块没还 / 是谁标的 / 在哪分配的**"(明细行的 `caller` 留给 host
侧 `addr2line` / `llvm-symbolizer` 离线解码), **不能**回答"某插件超预算"。
`br_memleak_owner()` 返回的标签只用于**归并报告**, 不构成资源隔离。

## 归属边界: 只读 core 的堆观测契约

本插件**不**建池、**不**改堆、报告路径**不**分配内存。它消费的全是 core 对外头文件里的
观测/记账契约:

- `br_heap_stats_get`(总量/峰值/违约累计)、`br_heap_walk`(逐块存活)、
  `br_heap_check`(当前违约)、`br_heap_usage`(native 面的 used/total 投影)、
  `br_heap_owner_register/name/set/get`。

理由与 `br_irq_stats_get` 的处置同型(3-02 §17.4): **消费方是插件 ⇒ 契约不能藏在 core
内部**。这也是本插件 P0、不声明 `[privileged]` 的原因。

## 关键裁定(实现侧)

1. **归属归并的静态上界与溢出策略**: 固定 **16 个归属槽**(`ML_OWNER_SLOTS`)+ 1 个
   `"(other)"` 溢出槽。槽满后**新出现的**归属(以及此前因溢出未登记的归属)全部并进
   `"(other)"`; summary 的 `owners` 计数把 `"(other)"` 算作**一个**归属。全程零分配 ——
   这是 5-01 §3"捕获路径只用静态缓冲"的直接落点。
2. **明细行不做报告期缓存**: 每个归属重新走一遍 `br_heap_walk`, 每次最多
   `BR_MEMLEAK_MAX_ROWS_PER_OWNER`(8)条。用时间换静态 RAM —— 4 KiB 的插件预算装不下
   16 × 8 的块表。超出的块只体现在该归属的 `live=` 计数里。
3. **`br_memleak_corruptions()` 是"当前状态", 不是累计**: 直接回传 `br_heap_check()`。
   头文件抬头写的"违约数 + 本插件累计观察到的峰值"里, 后半句本原型**不做** ——
   累计口径已在 `br_heap_stats_t`(`redzone_hits` 等), 而用例 `TC-DBG-041` 依赖
   "红区复原后必须回到 0"的语义; 两者相加会把"修好的破坏"永远记成红。
   (声明面签名未变; 这是对该行文档口径的**实现侧收窄**, 记入「待回灌」。)
4. **懒注册**: 插件管理器按相驱动 `memleak_init()`(→ `br_memleak_init()`), 但自检钩子
   可能出现在"未 init 的组合"里(宿主/裁剪镜像); 故 `br_memleak_owner()` 首次需要时就注册
   并缓存成功结果(失败不缓存, 下次再试), init 只是它的显式入口。这样
   `memleak_selftest()`(由 core 驱动)在 `init` 未跑过的组合下也能工作。
5. **不注册 trace 事件名**: 头文件抬头提到 memleak init 还要"注册 trace 事件名", 但
   `plugin.toml` **没有** `service/trace` 依赖边 ⇒ 不造一条未声明的调用边
   (声明面唯一真值的纪律)。留待依赖边被显式声明后再补。

## 依赖与红区约定(集成前提)

- **依赖**: 只有 core。观测面见 `br/core/br_mem.h`(② 观测契约 + ③ 记账契约)。
- **`TC-DBG-041` 依赖 core 的块布局约定**: 用户区 `p..p+size-1`, 紧随其后是红区 ⇒
  请求 64 B 时"越界 1 字节"落在 `p+64`; 红区填充值为 **0xA5**。用例先写 `0xFF` 触发
  违约, 再复原 `0xA5` 让 `br_heap_check()` 回到 0。
  若集成时发现 core 的红区起点/填充值不同, 需同步调整用例(见报告)。

## 被谁调用

- **`service/dump`**: `br_dump_leaks()` → `br_memleak_report()`(只调**呈现面**; dump 不再
  代跑任何自检)。`product.toml [select].plugins` 只选了 `service/dump`, memleak 作为它的
  `[[dep]]` 被**同镜像编译**(源集合由本插件 `plugin.toml` 的 `[build].sources` 声明、
  `brickie build` 按依赖闭包消费); 插件管理器扫 `.br_plugins` 时按 dump 的 init 闭包把它
  带入, LATE 相按拓扑序调到 `memleak_init()` → `br_memleak_init()`。
- **自检**: `memleak_selftest()` 由 **core** 的 `br_plugin_manager_selftest()` 在全部
  `start()` 之后、按 init 拓扑序统一驱动 —— dump 的四条 `kind = "init"` 边保证 memleak 的
  钩子排在 dump 自己的 `dump_selftest()` 之前(后者要观测"堆已被 memleak 用例恢复干净")。
- **APP 不直调 memleak**(`app/hello/plugin.toml` 没有这条 dep), 也不直调 dump。

## 验证

```sh
build/host/<triple>/bin/brickie build   # 全镜像交叉编译(含本插件; 零警告)
make dbg-test                           # 运行期门禁(需 core 堆实现就位)
```

用例(编进镜像, 逐项 `[DBGCONF] PASS/FAIL`), 每例收尾都把堆恢复干净:

| 用例 | 内容 | 判据 |
|---|---|---|
| `TC-DBG-040` | 真泄漏可检出(默认 owner 分配 123 B 后"丢指针") | 标签非 NONE; 本归属存活 +≥1; report 的 live 与独立观测一致且增量归属唯一; 收尾 `br_free` 归还 |
| `TC-DBG-041` | 红区越界可检出 | 踩 `p[64]` ⇒ `br_heap_check()>0`; 复原 `0xA5` ⇒ `==0` |
| `TC-DBG-042` | 双重释放可检出且不破坏堆 | `double_free>=1`; `br_heap_check()==0`; 之后仍能分配/释放 |
| `TC-DBG-043` | 记账匹配 | 10 × 100 B ⇒ `used` 增量 `>=1000`; 全部释放后 `used` **精确**回到基线 |

## 待回灌(发现缺口, 声明面已冻结 ⇒ 只登记, 不擅自加函数)

1. **`br_memleak_corruptions()` 的累计峰值口径**: 头文件抬头提到"+ 本插件累计观察到的
   峰值", 本原型按当前状态实现(见上文裁定 3)。若要累计, 需要明确"峰值"的定义与复位点
   (否则用例判据不可判)。
2. **trace 事件名**: `br_memleak_init()` 的文档提到注册 trace 事件名, 但 plugin.toml
   无 `service/trace` 依赖边 —— 需要先补声明边, 再补调用。
3. **memleak 行数观测**: `br_dump_all()` 只能把 report 的"存活块数"折算成行数
   (下界口径)。若要精确, 需要 `br_memleak_report()` 回传"本次打印行数"的新函数。
4. **v2 arena**: 预算上限 / 强制归属 / OOM 策略 —— 设计 §4 的 v2.0 条目, 本原型不假装。
