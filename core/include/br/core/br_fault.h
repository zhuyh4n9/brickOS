/*
 * brickOS prototype v0.1.0 — fault(同步异常)路径: 分槽 / 分类 / extable / panic
 *
 * 设计依据: `docs/3-os-core/3-02-int.md` §10 ——
 *   §10.2 向量表分槽与**屏蔽位处理**(fault 桩一律显式全屏蔽 DAIF, 不依赖硬件行为);
 *   §10.3 EC 分类与决策表;
 *   §10.4 extable(fixup: 非法访存降级为返回错误码);
 *   §10.5 handler 链(**注册 API 属 v2**, 本篇不实现; 签名在 3-02 §14.5 已定稿);
 *   §10.6 double fault 兜底(不调用任何 handler, 直接 `br_panic_bare`)。
 *
 * ★ panic 的两种入口(3-02 §8.3.1): ISR/fault 路径**一律** `br_panic_bare()`
 *   (无锁/无堆/无调度器/不写 trace 环); `br_panic()` 是线程侧便利入口。
 *   两者都是终止性的, 不返回。
 */
#ifndef BR_CORE_BR_FAULT_H
#define BR_CORE_BR_FAULT_H

#include <br/core/br_types.h>
#include <br/core/br_exc.h>

/* ---- ESR_EL1.EC(异常类)取值(3-02 §10.3 的决策表) ---- */
#define BR_EC_UNKNOWN        0x00u   /* 未定义指令(代码被踩/跳飞) ⇒ fatal */
#define BR_EC_FP_SIMD        0x07u   /* FP/SIMD 访问陷阱(CPACR_EL1.FPEN 未开)⇒ fatal, **必须点名** */
#define BR_EC_SVC64          0x15u   /* 单异常级无 syscall ⇒ fatal */
#define BR_EC_HVC64          0x16u
#define BR_EC_SMC64          0x17u
#define BR_EC_MSR_MRS        0x18u   /* 被 trap 的系统寄存器访问 ⇒ fatal(诊断打印编码) */
#define BR_EC_IABT_LOWER     0x20u   /* 取指到非法/无权限地址 ⇒ fatal */
#define BR_EC_IABT_SAME      0x21u
#define BR_EC_DABT_LOWER     0x24u   /* 数据访存非法/权限/对齐 ⇒ **先查 extable**, 查不到才 fatal */
#define BR_EC_DABT_SAME      0x25u
#define BR_EC_BRK64          0x3Cu   /* 断点 ⇒ v1 fatal; v2 转交 debug bridge */

/* ESR_EL1.EC 字段位置 */
#define BR_ESR_EC_SHIFT      26u
#define BR_ESR_EC_MASK       0x3Fu
#define BR_ESR_ISS_MASK      0x01FFFFFFu

/* ---- extable(3-02 §10.4; ISA 层机制, **不占 native API 面**) ----
 *
 * ★ 表项为什么必须带 `reg` 与 `errno_val`: 只记 `{pc, fixup}` 的话, 修复路径
 *   **没有任何办法知道**该把错误码放进哪个寄存器 —— `"=r"(ret)` 只是编译器的约束,
 *   运行期的 fault 路径看不到它。表项不带这两个字段, 两个已命名的消费者
 *   (debug bridge 的 MEMRD、驱动 probe 自检)**根本无法实现**。
 */
typedef struct {
    br_uintptr_t pc;        /* 可能 fault 的指令地址(与 ELR_EL1 比对) */
    br_uintptr_t fixup;     /* 修复标签(写回 ELR_EL1 继续执行) */
    br_u8        reg;       /* 返回值写入哪个通用寄存器(0..30; 31 = xzr 表示不写) */
    br_u8        rsv[3];
    br_s32       errno_val; /* 写入的值(约定用负 errno) */
} br_extable_t;             /* 24 B */

_Static_assert(sizeof(br_extable_t) == 24u, "extable 表项布局漂移");

/*
 * 在 pc 上查 extable。命中 ⇒ 调用方(fault 路径)改 ELR 并按表项写寄存器。
 * v0.1 实现: 对链接期收集的 `.br_extable` 段做**线性扫描**(表项数是个位数)。
 * 设计 §10.4 期望"按 pc 有序 ⇒ 二分": 段由链接器收集, 顺序 = 链接顺序,
 * **没有无运行期初始化的排序点**(在 fault 上下文排序是禁止的), 故 v0.1 选线性;
 * 表项增长后由 ISA 层在链接期生成有序表(记入 README 的实现裁定 P-IRQ-2)。
 */
const br_extable_t *br_extable_lookup(br_uintptr_t pc);

/*
 * 发射一条 extable 表项。
 *
 * 用法(必须与"可能 fault 的访问"在**同一个 asm 模板**内, 标签用 `%=` 保唯一):
 *
 *   __asm__ volatile(
 *       ".Lprobe" "%=:\n\t"                 // insn 标签
 *       "   ldr %w[tmp], [%[addr]]\n\t"
 *       "   mov %w[ret], wzr\n\t"
 *       "   b   .Lfixdone" "%=\n\t"
 *       ".Lfixup" "%=:\n\t"                 // fixup 标签
 *       "   mov %w[ret], %w[baderr]\n\t"
 *       ".Lfixdone" "%=\n\t"
 *       BR_EXTABLE_ENTRY(.Lprobe%=, .Lfixup%=, 0, -14)   // reg=0(x0), errno=-EFAULT
 *       : ... );
 *
 * `.quad <label>` 是对**本 asm 模板内**的 `.L` 局部标签的汇编期引用 ⇒ 不产生
 * 全局符号(CA-10: 插件导出面近零), 也不需要任何运行期初始化。
 */
#define BR_EXTABLE_ENTRY(insn, fixup, regno, errval)                       \
    ".pushsection .br_extable, \"a\", %%progbits\n\t"                      \
    ".balign 8\n\t"                                                       \
    ".quad " #insn "\n\t"                                                 \
    ".quad " #fixup "\n\t"                                                \
    ".byte " #regno "\n\t"                                                \
    ".zero 3\n\t"                                                         \
    ".long " #errval "\n\t"                                               \
    ".popsection\n\t"

/* ---- 入口与 panic(3-02 §14.2 / §8.3.1) ---- */

/*
 * fault 全流程: `in_fault++`(≥2 ⇒ double fault 走 bare panic) → EC 分类 →
 * extable fixup(命中 ⇒ 改帧 + `goto OUT`)→ 否则 fatal panic。
 * ★ `in_fault` 必须在**每条非致命退出路径**上递减(3-02 §10.6): 漏了就毁掉 ramdump。
 *   实现用"入口/出口各一处"的结构消除错误(所有非致命分支都 `goto OUT`)。
 */
void br_fault_enter(br_exc_frame_t *f);

/*
 * bare panic: 任何上下文(含 ISR/fault/double fault)可用。
 * 只轮询 console 输出最小信息 + 停机; **无锁/无堆/无调度器/不写 trace 环**
 * (与 5-01 §2「panic 通道独立」同一原则)。不返回。
 */
BR_NORETURN void br_panic_bare(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

/* thread-only panic(完整路径; v0.1 与 bare 的差别只有门口一句提示)。不返回。 */
BR_NORETURN void br_panic(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

#endif /* BR_CORE_BR_FAULT_H */
