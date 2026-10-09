/*
 * brickOS prototype v0.2.0 — 通用设备注册表(framework/dev-core)
 *
 * 设计依据: `docs/8-device/8-01-device.md` §1.1/§1.3(体系分层与框架件归属)、§2(注册表
 * 与子分类注册协议)、§3(形状归子分类, dev-core **不定义任何 ops 形状**)、
 * §4(错误模型 = 负 errno)、§5(ioctl 编码)。逐条裁定见 `docs/decisions/0009-vfs-storage-stack.md`。
 *
 * ## 本文件做的**全部**事情(就三件, R-S4 的防线)
 *   ① 一张静态注册表(唯一扁平命名空间)+ 命名规则执法;
 *   ② 把 `br_dev_class_entry_t`(含 open_file 钩子)**原样**存下来, 按名/按下标取回;
 *   ③ ioctl 编码宏(在头文件里, 纯编译期)。
 * 它不知道 cdev/bdev/flash 是什么, 也不调用任何子分类的 ops —— 形状与语义在子分类框架。
 *
 * ## 为什么注册表放在**插件**而不是 core
 *   设计 D19/`1-01` §4.5: "能力框架成为插件" —— 不选设备框架的产品不该为它付 RAM/代码;
 *   而纪律(API 面进 golden/门禁)与 core 相同。所以这里 `br_*` 前缀是治理纪律, 不是 core 本体。
 *
 * ## 为什么重名在**运行期**也执法
 *   设备名是驱动在运行期拼出来的字符串(可能含 SoC 实例号), `brickie check` 的声明面
 *   看不到它 —— 与 `br_service_publish` 的 `-EEXIST` 同型: 运行期自证一次极便宜,
 *   而"两个驱动抢同一个 /dev/uart0"是那种**静默**把一方废掉的错。
 */
#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/core/br_types.h>
#include <br/dev/br_dev.h>

/* 生命周期钩子原型(名 = symbol_prefix + 相; 由生成物 `plugin_desc.c` 引用;
 * 钩子属组合期契约, **不**进 [[export]] ⇒ 不参与接口 hash, 故不在对外头里声明)。 */
int dev_core_early_init(void);
int dev_core_init(void);
int dev_core_start(void);

/* ==================================================================== 注册表 */

/*
 * 静态表(原型口径: 无动态分配 —— 与 IRQ/PIC 池、TCB 池同一手法)。
 * `name` 与 `entry` 分开存: 名字是**注册表的键**(设计 `8-01` §2 把 name 放在
 * `br_dev_add` 的形参里而不是结构体里), 类型依赖也因此不必让 entry 承载字符串生命周期。
 */
typedef struct dev_slot {
    br_bool              used;
    char                 name[BR_DEV_NAME_MAX];
    br_dev_class_entry_t entry;
} dev_slot_t;

static dev_slot_t s_devs[BR_DEV_MAX];
static br_u32     s_count;   /* 条数(紧凑: 删空不留洞 —— v1 无注销, 但保持"前 count 项有效"的不变量) */

/* ==================================================================== 命名规则 */

br_bool br_dev_name_valid(const char *name)
{
    if (name == BR_NULL) {
        return BR_FALSE;
    }

    br_u32 i = 0u;
    for (; name[i] != '\0'; i++) {
        const char c = name[i];
        if (i == 0u) {
            /* 首字符必须小写字母(设计 `8-01` §2: `[a-z][a-z0-9]*`)。
             * 为什么禁止首字符是数字: devfs 节点名是路径分量, 而 `7-01` §1 的命名规则
             * 明确 `[a-z]` 开头 —— 数字开头会让"设备名"与"可能的编号后缀"混淆。 */
            if (c < 'a' || c > 'z') {
                return BR_FALSE;
            }
            continue;
        }
        const br_bool digit = (c >= '0' && c <= '9');
        const br_bool lower = (c >= 'a' && c <= 'z');
        if (!digit && !lower) {
            return BR_FALSE;   /* 大写 / '_' / '-' / '/' 一律不允许(无斜杠: 名 = 路径分量) */
        }
    }

    /* 长度: 至少 1 字符, 且必须给结尾 '\0' 留位(否则 devfs 侧的拷贝会截断)。 */
    return (i > 0u && i < (br_u32)BR_DEV_NAME_MAX) ? BR_TRUE : BR_FALSE;
}

/* ==================================================================== 查找与登记 */

static br_u32 find_index(const char *name)
{
    if (name == BR_NULL) {
        return (br_u32)-1;
    }
    for (br_u32 i = 0u; i < s_count; i++) {
        if (s_devs[i].used == BR_FALSE) {
            continue;
        }
        const char *a = s_devs[i].name;
        const char *b = name;
        while (*a != '\0' && *a == *b) {
            a++;
            b++;
        }
        if (*a == '\0' && *b == '\0') {
            return i;
        }
    }
    return (br_u32)-1;
}

int br_dev_add(const char *name, const br_dev_class_entry_t *entry)
{
    if (entry == BR_NULL) {
        return BR_ERR(BR_EINVAL);
    }
    if (br_dev_name_valid(name) == BR_FALSE) {
        return BR_ERR(BR_EINVAL);
    }
    /* class_id = NONE 的条目没有形状解释者 ⇒ 谁也没法打开它, 属调用方的错。
     * 显式拒绝比"存下来但永远打不开"诚实。 */
    if (entry->class_id == BR_CLASS_NONE) {
        return BR_ERR(BR_EINVAL);
    }
    if (find_index(name) != (br_u32)-1) {
        return BR_ERR(BR_EEXIST);
    }
    if (s_count >= (br_u32)BR_DEV_MAX) {
        return BR_ERR(BR_ENOSPC);
    }

    dev_slot_t *s = &s_devs[s_count];
    s->used  = BR_TRUE;
    s->entry = *entry;                 /* 结构体按值存: 调用方（子分类框架）的静态表可复用 */

    br_u32 i = 0u;
    for (; name[i] != '\0' && i + 1u < (br_u32)BR_DEV_NAME_MAX; i++) {
        s->name[i] = name[i];
    }
    s->name[i] = '\0';

    s_count++;
    return BR_OK;
}

const br_dev_class_entry_t *br_dev_lookup(const char *name)
{
    const br_u32 i = find_index(name);
    return (i == (br_u32)-1) ? BR_NULL : &s_devs[i].entry;
}

br_u32 br_dev_count(void)
{
    return s_count;
}

const char *br_dev_name_at(br_u32 index)
{
    if (index >= s_count || s_devs[index].used == BR_FALSE) {
        return BR_NULL;
    }
    return s_devs[index].name;
}

const br_dev_class_entry_t *br_dev_entry_at(br_u32 index)
{
    if (index >= s_count || s_devs[index].used == BR_FALSE) {
        return BR_NULL;
    }
    return &s_devs[index].entry;
}

/* ==================================================================== 生命周期 */

/* EARLY 相: 无动作。注册表的静态量是 BSS(已由 start.S 清零), 且此刻无线程/无堆 ——
 * 设备注册要等 **CORE 相**(子分类框架的 init), 那时堆已就绪(br_malloc 可用于驱动私有)。 */
int dev_core_early_init(void)
{
    return 0;
}

/* CORE 相: 无动作。dev-core 没有需要"初始化"的状态(BSS 即初值)——
 * 刻意**不**造一个空初始化 API(R-S4: 框架件不许往里塞没有消费者的东西)。 */
int dev_core_init(void)
{
    return 0;
}

/* START 相: 本件的一致性用例**不在这里跑** —— ADR-0010 之后由 core 的
 * `br_plugin_manager_selftest()` 经描述符的 `.selftest` 钩子统一驱动(见 src/dev_selftest.c)。
 * 这里只留一行注册表摘要作启动证据(与 vfs-core 的 start 同型: start 只报状态)。 */
int dev_core_start(void)
{
    br_log_info("dev: %u device(s) registered", br_dev_count());
    return 0;
}
