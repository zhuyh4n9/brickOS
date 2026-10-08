/* fx/build 的汇编源桩(只用于让 ninja 后端渲染出 asm rule; 不参与真编译)。 */
.section .text
.globl fx_start
fx_start:
    ret
