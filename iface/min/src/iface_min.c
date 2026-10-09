/*
 * iface/min — core native 极简别名皮肤(实现; 人写)
 *
 * 三个空钩子: Interface 插件是**严格叶子 + 无状态**(设计 `1-01` §7.4)。
 * 它不实现任何 core 函数 —— 那些在 core 里(单一真值), 本件只把声明转出。
 * 描述符实例不在这里, 在生成物 `build/gen/iface/min/plugin_desc.c`(ADR-0005 §8.4)。
 */
#include <iface/min/min.h>

/* EARLY 相(不用堆/无线程/关中断): 无动作。 */
int iface_min_early_init(void)
{
    return 0;
}

/* LATE 相(interface 的 ② 完成点): 无状态, 无事可做。 */
int iface_min_init(void)
{
    return 0;
}

/* START 相(全局开中断后): 无动作。 */
int iface_min_start(void)
{
    return 0;
}
