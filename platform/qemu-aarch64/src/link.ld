/*
 * brickOS prototype v0.1.0 — 链接脚本(aarch64 / QEMU virt)
 *
 * WORKAROUND(br-wa-entry-001): 设计侧链接脚本是 Platform 插件的产出物之一
 * (设计 4-02 §2 第 4 项「物理(二进制)布局映射」; 组合器生成链接脚本片段)。
 * v0.1.0 由本文件手工承担, 未经 `br` 生成 —— 见 WORKAROUNDS.md。
 *
 * 地址选择: QEMU virt 的 RAM 起于 0x40000000; 传统上内核加载在 0x40080000
 * (留出 512 KiB 给固件/ATF 的惯例)。本原型无固件依赖, 沿用该惯例以免
 * 将来换加载器时踩地址。
 */

OUTPUT_ARCH(aarch64)
OUTPUT_FORMAT(elf64-littleaarch64)
ENTRY(_start)

/*
 * 显式分段, 不做"一个 RWX 大段": 没有 MMU 时 RWX 无害, 但镜像会被 ELF 工具
 * 报 `has a LOAD segment with RWX permissions` —— 而且将来接引导器/加载器时
 * 段权限是唯一能表达"这段可写"的手段, 现在就分对, 免得以后返工。
 *   text   : R+X  —— 代码 + 向量表
 *   rodata : R    —— 只读常量
 *   data   : RW   —— 数据 + BSS
 */
PHDRS
{
    text   PT_LOAD FLAGS(5);   /* PF_R | PF_X */
    rodata PT_LOAD FLAGS(4);   /* PF_R        */
    data   PT_LOAD FLAGS(6);   /* PF_R | PF_W */
}

SECTIONS
{
    . = 0x40080000;
    __image_start = .;

    /* .text.boot 必须最先: Platform Entry 的入口地址 = 镜像基址 */
    .text : ALIGN(16) {
        KEEP(*(.text.boot))
        *(.text.vectors)
        *(.text .text.*)
    } :text
    . = ALIGN(4096);

    .rodata : ALIGN(16) {
        *(.rodata .rodata.*)

        /*
         * extable(fixup 表, 设计 3-02 §10.4): 由 `BR_EXTABLE_ENTRY` 宏经
         * `.pushsection .br_extable` 发射、链接器收集 —— **链接期生成, 无运行期初始化**
         * (这正是"extable 必须先于任何可能触发它的代码就绪"的落地方式)。
         * 段名含 '.', GNU ld 不会自动生成 __start_/__stop_ ⇒ 边界符号在此显式提供
         * (与 3-01 §13.3 对 `.br_plugins` 的同一手法)。
         */
        . = ALIGN(8);
        __br_extable_start = .;
        KEEP(*(.br_extable))
        __br_extable_stop = .;
    } :rodata
    . = ALIGN(4096);

    .data : ALIGN(16) {
        *(.data .data.*)
    } :data

    /* BSS: start.S 会清零这一区间, 链接器保证大小为 8 的倍数 */
    . = ALIGN(8);
    __bss_start = .;
    .bss : ALIGN(8) {
        *(.bss .bss.*)
        *(COMMON)
    } :data
    . = ALIGN(8);
    __bss_end = .;

    __image_end = .;

    /*
     * 启动栈 64 KiB: 通过推进位置计数器预留, **不产生任何段内容**,
     * 所以镜像文件不会因此变大; RAM 由 QEMU 预置为零。
     * (放一个真 .stack 段会往 ELF 里塞 64 KiB 零, 没有必要。)
     */
    . = ALIGN(16);
    __stack_bottom = .;
    . += 64 * 1024;
    __stack_top = .;

    /* 编译器/链接器噪声: 裸机镜像不需要 */
    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
        *(.ARM.exidx*)
        *(.ARM.attributes)
    }
}
