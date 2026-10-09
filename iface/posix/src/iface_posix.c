/*
 * iface/posix — POSIX 薄皮肤(实现; 人写)
 *
 * 本文件只有三个空钩子: Interface 插件是**严格叶子 + 无状态**(设计 D18 / `1-01` §7.4),
 * 它不实现任何 POSIX 函数 —— 那些在 `runtime/posix`(单元 `posix`)。
 * 描述符实例不在这里, 在生成物 `build/gen/iface/posix/plugin_desc.c`(ADR-0005 §8.4)。
 */
#include <iface/posix/posix.h>

/* EARLY 相(不用堆/无线程/关中断): 无动作。 */
int iface_posix_early_init(void)
{
    return 0;
}

/* LATE 相(interface 的 ② 完成点): 皮肤无状态, 无事可做。
 * ★ 这里**不**去"检查 runtime/posix 是否 init 过" —— 那需要读提供方的私有状态;
 *   顺序由 `[[dep]] kind = "init"` + 拓扑序保证, 不靠运行期互相打探。 */
int iface_posix_init(void)
{
    return 0;
}

/* START 相(全局开中断后): 无动作。 */
int iface_posix_start(void)
{
    return 0;
}
