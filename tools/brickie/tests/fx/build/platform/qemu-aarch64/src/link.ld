/* fx/build 的链接脚本桩(构建族用例只断言它进了 -Wl,-T 的位置)。 */
ENTRY(_start)
SECTIONS { . = 0x40000000; .text : { *(.text*) } }
