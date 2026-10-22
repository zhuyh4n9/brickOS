# hal/bdev-core — 块设备子分类(hal / native)

## 1. 它是什么
接口面快照: `api/iface/hal/bdev-core/bdev.toml`。

`dev-core` 只管"是个设备"(唯一扁平命名空间 + 子分类注册协议 + `open_file` 钩子);**形状归子分类框架**。`cdev-core` 是字符设备子分类,`bdev-core` 是**块设备子分类**: 磁盘型介质(无擦除、有 flush)的扇区读写面。它把块设备注册进 `dev-core` 的命名空间 ⇒ `/dev/<name>` 自动出现(devfs 是注册表的投影)。

```
器件(eMMC / NOR / virtio-blk) → bdev-core(br_bdev_ops: read/write/flush/geometry + DMA/PIO 标示)
  → dev-core(唯一扁平命名空间 + 子分类注册协议) → fs/devfs(/dev 节点)
    → 消费者(FS 插件经 br_bdev_get 绑定; page cache 以堆叠 bdev 构建于其上)
```

## 2. 对外面(全部在这三样里)

```c
typedef struct {                       /* 几何(每次 geometry 调用回答当前值) */
    br_u32 sector_size;                /* 逻辑扇区, 通常 512 */
    br_u64 n_sectors;
    br_u32 max_rw;                     /* 单次传输上限(扇区): > 0 且 <= n_sectors */
    br_u32 align;                      /* 缓冲对齐要求; 0 = 任意 */
    br_u32 flags;                      /* BR_BDEV_F_* */
} br_bdev_geom_t;

typedef struct br_bdev_ops {           /* 成功 = 传输扇区数; 失败 = BR_ERR(BR_E*) */
    int (*read)    (void *priv, br_u64 lba, void *buf, br_size_t n_lba);
    int (*write)   (void *priv, br_u64 lba, const void *buf, br_size_t n_lba);
    int (*flush)   (void *priv);       /* 必须落介质(page cache 依赖, SD-8) */
    int (*geometry)(void *priv, br_bdev_geom_t *g);
    int (*ioctl)   (void *priv, br_u32 cmd, void *arg);   /* D22 预留; NULL ⇒ -ENOTSUP */
    int (*suspend) (void *priv);
    int (*resume)  (void *priv);
} br_bdev_ops_t;

int                  br_bdev_register(const char *name, const br_bdev_ops_t *ops, void *priv);
const br_bdev_ops_t *br_bdev_get(const char *name, void **priv);   /* FS 绑定口(形态 B) */
```

### flags 与"DMA/PIO 必须被指出"

| 位 | 含义 |
|---|---|
| `BR_BDEV_F_READONLY` | 介质不可写(语义位, 不放松注册期必填槽位) |
| `BR_BDEV_F_STACKED` | provider 自身也是 consumer(分区映射器 / page cache 的标记) |
| `BR_BDEV_F_DMA` | 当前数据通路 = DMA |
| `BR_BDEV_F_PIO` | 当前数据通路 = PIO |

## 3. 注册期校验(为什么调一次 geometry)
`br_bdev_register` 的校验顺序(全部在**入表之前**):

1. `ops` 非空;`read`/`write`/`flush`/`geometry` **四个必填槽位**齐备(只读设备用 flags 声明, 不豁免槽位);
2. 名字合规(`br_dev_name_valid`: `[a-z][a-z0-9]*` 风格, 无斜杠/大写/下划线);
3. **调一次 `ops->geometry(priv, &g)`** 并校验几何自洽: `sector_size` 是 2 的幂且非 0;`n_sectors > 0`;`0 < max_rw <= n_sectors`;`align == 0` 或是 2 的幂;`DMA` 与 `PIO` 不同置。

驱动自己的错误码原样上传。错误码: `-EINVAL`(名字/ops/几何非法)/ `-EEXIST`(重名, dev-core 执法)/ `-ENOSPC`(静态池满)。

## 4. 边界(本件**不**做什么)
* **不认识任何具体介质** —— eMMC/NOR/virtio-blk 是对接本件的驱动;
* **不做分区解析**(MBR/GPT 与 `br_bdev_partition` 是 v1.x) —— 本刀只留设计;
* **不做缓存策略** —— page cache 是**堆叠插件**;本件只保证"上层看到同一 `br_bdev_ops`";
* **不做设备生命周期策略** —— 注册 = 上线, 无注销(v1 静态组合, 与 dev-core/cdev-core 同款);
* **不提供 raw 文件面** —— `/dev/<bdev>` 属 v2, 条目的 `open_file` **恒 `BR_NULL`**(看得见、打不开; 与 flash 同款), 并由 TC-BDEV-005 钉成机械判据。

## 5. 依赖与构建

| 边 | kind | 理由 |
|---|---|---|
| `hal/dev-core` | `init` | 注册要走它的注册表 |
| `hal/vfs-core` | `type` | `br_dev_class_entry_t.open_file` 的类型引用 `br_file_ops_t`(O-S7 的类型成本, 与 cdev-core 同款) |

`src/bdev_core.c` = 生产;`src/bdev_selftest.c` = 自检(表在场 = 有自检, `[selftest].cases = 8`);`src/bdev_internal.h` = 自检用的内部视图(容量/占用数, **不导出**)。

## 6. 一致性用例(`TC-BDEV-*`,`tests/smoke.toml` 是声明面骨架)

| id | 判据 |
|---|---|
| TC-BDEV-001 | NULL/空/大写/斜杠/下划线名 ⇒ `-EINVAL` 且注册表不变 |
| TC-BDEV-002 | `ops` 空 / 缺 read\|write\|flush\|geometry ⇒ `-EINVAL` 且名字仍空闲 |
| TC-BDEV-003 | 几何自洽逐条拒绝(0/非 2 幂扇区、0 容量、max_rw 越界、非 2 幂 align、DMA\|PIO 同置) |
| TC-BDEV-004 | 未注册 ⇒ `NULL` 且不改写 `*priv`;`br_bdev_get(NULL, NULL) == NULL` |
| TC-BDEV-005 | 所有 `BR_CLASS_BDEV` 条目 `open_file == NULL`(raw 块访问属 v2) |
| TC-BDEV-006 | 活设备: `br_bdev_get` 的 ops == 条目 `class_priv`;几何自洽;重名重注册 ⇒ `-EEXIST` |
| TC-BDEV-007 | 池簿记: `slot_count <= capacity` 且 `slot_count ==` 注册表 bdev 条目数 |
| TC-BDEV-008 | 活设备: `flags` 只含已知位且 `DMA`/`PIO` 不同置 |

TC-BDEV-006/008 在"组合里还没有 bdev 驱动"时走**不适用(平凡通过)**分支 —— 驱动接入后立刻变成真判据(与 cdev-core 的 TC-CDEV-004 同款)。红绿由门禁 `smoke`/`fs-test` 上的 `[BDEVCONF] SUMMARY pass=N fail=0` 与 forbid `[BDEVCONF] FAIL` 判。
