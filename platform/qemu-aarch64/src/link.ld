/*
 * brickOS prototype v0.1.0 — 链接脚本(aarch64 / QEMU virt)
 *
 * 已还清(ADR-0005): 设计侧链接脚本是 Platform 插件的产出物之一
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

    /*
     * 插件描述符段(设计 1-01 §13.3 / 3-05 §2.2): 生成物
     * `build/gen/<plugin>/plugin_desc.c` 里 `BR_PLUGIN_SECTION` 的对象落在这里,
     * plugin_manager 经 `__br_plugins_start/__br_plugins_stop` 枚举。
     *
     * 三条都不是可选的:
     *   - `KEEP`: 描述符没有 C 引用者(`static const` + `used`), `--gc-sections`
     *     会把它当垃圾 —— 段必须显式保根;
     *   - `ALIGN(8)`: `br_plugin_t` 含指针; `BR_PLUGIN_SECTION` 的 aligned(8) 只保证
     *     **段内**对齐, 段起点的对齐要靠链接脚本(设计写 aligned(4) 对含指针结构不足);
     *   - **普通赋值**(不是 PROVIDE): 这两个符号必须恒存在, 且不能在别处被定义 ——
     *     与 `.br_extable` 同款; `PROVIDE` 的"有人定义就不覆盖"语义在这里是隐患。
     *
     * 与 `.br_extable` 的**唯一写法差别**: 这里给 `.br_plugins` 自己的输出段(而不是
     * 折进 `.rodata`)。理由 = 可验证性: `objdump -h` 只列输出段, 折进去就"看不见段
     * 是否存在/多大"; 而 3-05 §2.2 把 `.br_plugins` 当成一个有名有界的段 + 边界符号。
     * 它仍属 `rodata` 的 PT_LOAD(R 权限, 不会把镜像变成 RWX 大段)。
     */
    .br_plugins : ALIGN(8) {
        __br_plugins_start = .;
        KEEP(*(.br_plugins))
        __br_plugins_stop = .;
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
