#ifndef BR_POSIX_SYS_MOUNT_H
#define BR_POSIX_SYS_MOUNT_H

/*
 * `mount()` / `umount()` —— **APP 决定挂载什么、挂在哪**(ADR-0029)。
 *
 * ★ 归属: 本仓的挂载由 **APP** 发起(不是 FS 插件在自己的 start 相自动挂)。
 *   FS 插件只把自己的能力登记成**驱动**(fstype 名), 例如 `"tmpfs"`/`"devfs"`/
 *   `"fat"`/`"erofs"`; APP 用本头的 `mount()` 按名把它们挂到挂载点上。
 *
 * 形参口径与 POSIX(以及 Linux)一致:
 *   `source`         介质类 FS 的设备名(`"emmc0"`); 无介质的(tmpfs/devfs)传 NULL。
 *   `target`         挂载点(绝对路径, 如 `"/"`、`"/fat12"`)。
 *   `filesystemtype` 驱动名, 例如 `"tmpfs"`。
 *   `mountflags`     POSIX 的 flags。**v1 保留**: 本代一律传 0; 非 0 ⇒ -EINVAL
 *                    (宁可显式拒绝, 也不要"收下不用"而让 APP 以为生效了)。
 *   `data`           FS 自有选项。**v1 保留**: 本代一律传 NULL; 非 NULL ⇒ -EINVAL。
 *
 * 错误: `-EINVAL`(target/fstype 为 NULL 或 flags/data 非空)/ `-ENODEV`(没有该 fstype 的
 *       驱动 —— 通常是该 FS 插件没被选进组合)/ `-EEXIST`(该挂载点已挂)/ `-ENOSPC`(表满)/
 *       `-ENODEV`(父挂载不存在)/ 其余原样上传自 FS 的 mount。
 */
int mount(const char *source, const char *target, const char *filesystemtype,
          unsigned long mountflags, const void *data);

/*
 * 卸载。**v1 保留**: 挂载表是静态组合期产物且挂载表持有根 inode, 卸载需要先把该挂载点
 * 下所有打开的文件句柄与瞬态 inode 收回 —— 那是"运行期挂载"(设计 O-S3, 不早于 v3)。
 * 本代恒返回 -ENOTSUP(**显式不实现**, 而不是假装成功)。
 */
int umount(const char *target);

#endif /* BR_POSIX_SYS_MOUNT_H */
