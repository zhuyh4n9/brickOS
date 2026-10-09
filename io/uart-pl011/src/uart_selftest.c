/*
 * brickOS prototype v0.2.0 — io/uart-pl011 的**自检套件**(io/uart-pl011/src/uart_selftest.c)
 *
 * 设计依据与裁定: `docs/decisions/0010-plugin-selftest.md`(自检与核心代码分离)。
 *
 * ## 为什么自检不在 `uart_pl011.c` 里(本文件存在的理由)
 *   测试入口不是器件的对外面(见 `include/br/io/br_uart_pl011.h` 的说明): 它若进 golden
 *   接口 hash, "加一条用例"就变成了接口变更。挪进 `src/` 后生产文件只剩驱动本体;
 *   用例仍在同一插件的编译单元集里(`[build].sources` 覆盖 `src/` 下的全部 `.c`)。
 *
 * ## 它测什么, 与生产代码的边界在哪
 *   器件往返(自编号 `TC-IO-*`): 只经**公开 API** 驱动(br_stat / br_open_err /
 *   br_file_* / ioctl), 不看任何内部符号 —— 否则"设备经 devfs + cdev 适配层真的能打开"
 *   这件事就没有被判据覆盖。
 *   **例外只有"平台事实"**: base / refclk / baud 三个常量(与 platform 的 console 同源)
 *   从 `src/pl011_internal.h` 取 —— 那不是"内部机制", 而是套件要断言的**平台数据真值**;
 *   在生产文件与套件里各写一遍, 判据就成了"复制品与复制品一致"。寄存器访问**不**从那里
 *   借: 套件不碰 MMIO(它经 ioctl 看设备自己的回答), 所以那个头里也没有寄存器访问器。
 *
 * ## 与 br_plugin_manager_selftest() 的契约
 *   返回**失败项数**(0 = 全绿); 日志自己打(`[IOCONF] PASS/FAIL ...` + `SUMMARY`)。
 *   **失败不停机** —— 红绿由门禁判(`tests/gates.toml` 的 forbid 里有 `[IOCONF] FAIL`)。
 */
#include <br/io/br_uart_pl011.h>

#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/core/br_types.h>
#include <br/dev/br_dev.h>
#include <br/vfs/br_vfs.h>

#include "pl011_internal.h"

/* 自检入口原型(定义在本文件末尾; 生成物 `plugin_desc.c` 的 `.selftest` 钩子引用它)。 */
int uart_pl011_selftest(void);

/* =====================================================================
 * 一致性用例(自编号 `TC-IO-*`; 登记在 br-wa-test-001)
 *
 *    只经**公开 API**驱动(br_stat / br_open_err / br_file_*), 不看任何内部符号 ——
 *    否则"设备经 devfs + cdev 适配层真的能打开"这件事就没有被判据覆盖。
 *    ★ 每个用例自己开、自己关。哪怕中途红了也必须收尾, 否则独占没释放, 后面的用例会连着
 *      红成一片(把"一个真 bug"变成"十二个假 bug"), 而根因反而被淹没。
 *    ★ 打印 `[IOCONF] PASS/FAIL TC-IO-00N <说明>`, 末尾 SUMMARY; 返回失败数。
 * ===================================================================== */

#define IO_CONF_PATH   "/dev/" BR_UART_PL011_DEV_NAME

/* 用例里反复出现的两条命令按头文件注释拼出来(与本文件 ioctl 的 nr 口径一致) */
#define IO_CONF_CMD_GET_INFO  BR_IOR(BR_UART_IOC_MAGIC, BR_UART_IOC_GET_INFO, br_uart_pl011_info_t)
#define IO_CONF_CMD_SET_BAUD  BR_IOW(BR_UART_IOC_MAGIC, BR_UART_IOC_SET_BAUD, br_u32)

static br_u32 s_io_pass;
static br_u32 s_io_fail;

static void io_conf(br_bool ok, const char *tag, const char *what)
{
    if (ok == BR_TRUE) {
        s_io_pass++;
        br_log_info("[IOCONF] PASS %s %s", tag, what);
    } else {
        s_io_fail++;
        br_log_info("[IOCONF] FAIL %s %s", tag, what);
    }
}

int uart_pl011_selftest(void)
{
    s_io_pass = 0u;
    s_io_fail = 0u;

    br_log_info("[IOCONF] uart-pl011 conformance (%s: stat/open/ioctl/write/poll/close)",
                IO_CONF_PATH);

    /* ---- TC-IO-001: 设备节点可见且是 FILE 类型 ---- */
    {
        br_stat_t  st = { 0u, 0u, 0u };
        const int  rc = br_stat(IO_CONF_PATH, &st);
        io_conf((rc == 0) && (st.type == BR_INODE_FILE), "TC-IO-001",
                "br_stat(" IO_CONF_PATH ") 成功且 type = FILE");
    }

    /* ---- TC-IO-002: 独占性(7-01 §1: 策略归驱动) ---- */
    {
        int        e1 = 0;
        int        e2 = 0;
        br_file_t *f1 = br_open_err(IO_CONF_PATH, BR_O_RDONLY | BR_O_NONBLOCK, &e1);
        br_file_t *f2 = br_open_err(IO_CONF_PATH, BR_O_RDONLY | BR_O_NONBLOCK, &e2);

        const br_bool ok = (f1 != BR_NULL) && (e1 == 0) &&
                           (f2 == BR_NULL) && (e2 == BR_ERR(BR_EBUSY));
        io_conf(ok, "TC-IO-002",
                "首次 open 成功; 已被打开时第二次 open ⇒ -EBUSY");

        /* 收尾必须在判据之外: f2 若意外成功也得关掉, 否则后面的用例全被独占挡住 */
        if (f2 != BR_NULL) {
            (void)br_file_close(f2);
        }
        if (f1 != BR_NULL) {
            (void)br_file_close(f1);
        }
    }

    /* ---- TC-IO-003: GET_INFO 回答平台事实 ---- */
    {
        int                  err  = 0;
        br_file_t           *f    = br_open_err(IO_CONF_PATH, BR_O_RDWR | BR_O_NONBLOCK, &err);
        br_uart_pl011_info_t info = { 0u, 0u, 0u, 0u, 0u };
        int                  rc   = BR_ERR(BR_EINVAL);

        if (f != BR_NULL) {
            rc = br_file_ioctl(f, IO_CONF_CMD_GET_INFO, &info);
        }

        const br_bool ok = (f != BR_NULL) && (err == 0) && (rc == 0) &&
                           (info.base == (br_u32)BR_PL011_BASE) &&
                           (info.refclk_hz == BR_PL011_REFCLK_HZ) &&
                           (info.baud == BR_PL011_BAUD);
        io_conf(ok, "TC-IO-003",
                "GET_INFO: base=0x09000000 refclk=24MHz baud=115200");

        if (f != BR_NULL) {
            (void)br_file_close(f);
        }
    }

    /* ---- TC-IO-004: write 走通(字节出现在 QEMU console 上是证据, 不是判据) ----
     * 设备 write **字节透明**: 不做 '\n' → CRLF 转换(那是 console 的策略, 见
     * console_pl011.c)⇒ 测试串自己带 CRLF, 免得出阶梯状换行。 */
    {
        static const char msg[] = "uart-pl011: /dev/" BR_UART_PL011_DEV_NAME
                                  " write path OK\r\n";
        int        err = 0;
        br_file_t *f   = br_open_err(IO_CONF_PATH, BR_O_RDWR | BR_O_NONBLOCK, &err);
        br_s64     n   = BR_ERR(BR_EINVAL);

        if (f != BR_NULL) {
            n = br_file_write(f, msg, (br_size_t)(sizeof msg - 1u));
        }

        const br_bool ok = (f != BR_NULL) && (err == 0) &&
                           (n == (br_s64)(sizeof msg - 1u));
        io_conf(ok, "TC-IO-004", "write 短字面量返回字节数");

        if (f != BR_NULL) {
            (void)br_file_close(f);
        }
    }

    /* ---- TC-IO-005: 空 RX FIFO + 非阻塞 ⇒ -EAGAIN ----
     * 前提是"FIFO 是空的"(启动后没有人往 QEMU 串口输过字符)。若主机侧先敲了键, 本用例会
     * 如实报红 —— 那是前提不成立, 不是驱动错; 不为了"永远绿"而先 flush(那会把这条判据
     * 变成了另一件事的判据)。 */
    {
        int        err = 0;
        br_file_t *f   = br_open_err(IO_CONF_PATH, BR_O_RDONLY | BR_O_NONBLOCK, &err);
        char       one = '\0';
        br_s64     n   = 0;

        if (f != BR_NULL) {
            n = br_file_read(f, &one, 1u);
        }

        const br_bool ok = (f != BR_NULL) && (err == 0) && (n == BR_ERR(BR_EAGAIN));
        io_conf(ok, "TC-IO-005", "空 RX FIFO + BR_O_NONBLOCK: read ⇒ -EAGAIN");

        if (f != BR_NULL) {
            (void)br_file_close(f);
        }
    }

    /* ---- TC-IO-006: poll 置 POLLOUT 且**清掉**未置位的位 ---- */
    {
        int        err = 0;
        br_file_t *f   = br_open_err(IO_CONF_PATH, BR_O_RDWR | BR_O_NONBLOCK, &err);
        br_u32     ev  = 0xffffffffu;   /* 预置全 1: 验证"未置位的位必须被清掉" */
        int        rc  = BR_ERR(BR_EINVAL);

        if (f != BR_NULL) {
            rc = br_file_poll(f, &ev);
        }

        const br_bool ok = (f != BR_NULL) && (err == 0) && (rc == 0) &&
                           ((ev & BR_POLLOUT) != 0u) &&
                           ((ev & ~(BR_POLLIN | BR_POLLOUT)) == 0u);
        io_conf(ok, "TC-IO-006", "poll: TX 不满 ⇒ BR_POLLOUT, 其余事件位为 0");

        if (f != BR_NULL) {
            (void)br_file_close(f);
        }
    }

    /* ---- TC-IO-007: 错魔数 / 未知 nr ⇒ -ENOTSUP(不是 -EINVAL) ---- */
    {
        int        err  = 0;
        br_file_t *f    = br_open_err(IO_CONF_PATH, BR_O_RDWR | BR_O_NONBLOCK, &err);
        int        r_magic = BR_ERR(BR_EINVAL);
        int        r_nr    = BR_ERR(BR_EINVAL);

        if (f != BR_NULL) {
            r_magic = br_file_ioctl(f, BR_IO('z', 0u), BR_NULL);
            r_nr    = br_file_ioctl(f, BR_IO(BR_UART_IOC_MAGIC, 9u), BR_NULL);
        }

        const br_bool ok = (f != BR_NULL) && (err == 0) &&
                           (r_magic == BR_ERR(BR_ENOTSUP)) &&
                           (r_nr == BR_ERR(BR_ENOTSUP));
        io_conf(ok, "TC-IO-007",
                "错魔数 BR_IO('z',0) 与未知 nr 均 ⇒ -ENOTSUP");

        if (f != BR_NULL) {
            (void)br_file_close(f);
        }
    }

    /* ---- TC-IO-008: cdev 的 file 面没有 lseek/fsync(槽位为空 ⇒ -ENOTSUP) ---- */
    {
        int        err    = 0;
        br_file_t *f      = br_open_err(IO_CONF_PATH, BR_O_RDWR | BR_O_NONBLOCK, &err);
        br_s64     r_seek = 0;
        int        r_sync = 0;

        if (f != BR_NULL) {
            r_seek = br_file_lseek(f, 0, BR_SEEK_SET);
            r_sync = br_file_fsync(f);
        }

        const br_bool ok = (f != BR_NULL) && (err == 0) &&
                           (r_seek == (br_s64)BR_ERR(BR_ENOTSUP)) &&
                           (r_sync == BR_ERR(BR_ENOTSUP));
        io_conf(ok, "TC-IO-008",
                "cdev 句柄的 lseek/fsync ⇒ -ENOTSUP(v1 形态: 适配层槽位为空)");

        if (f != BR_NULL) {
            (void)br_file_close(f);
        }
    }

    /* ---- TC-IO-009: close 返回 0 且独占随之释放 ---- */
    {
        int         e1 = 0;
        int         e2 = 0;
        br_file_t  *f  = br_open_err(IO_CONF_PATH, BR_O_RDWR | BR_O_NONBLOCK, &e1);
        const int   r_close  = (f != BR_NULL) ? br_file_close(f) : BR_ERR(BR_EINVAL);
        br_file_t  *g  = br_open_err(IO_CONF_PATH, BR_O_RDONLY | BR_O_NONBLOCK, &e2);
        const int   r_close2 = (g != BR_NULL) ? br_file_close(g) : BR_ERR(BR_EINVAL);

        const br_bool ok = (f != BR_NULL) && (e1 == 0) && (r_close == 0) &&
                           (g != BR_NULL) && (e2 == 0) && (r_close2 == 0);
        io_conf(ok, "TC-IO-009", "close 返回 0 且其后 open 再次成功(独占已释放)");
    }

    /* ---- TC-IO-010: 需要参数的调用给 NULL ⇒ -EINVAL(与 -ENOTSUP 是两件事) ---- */
    {
        int        err    = 0;
        br_file_t *f      = br_open_err(IO_CONF_PATH, BR_O_RDWR | BR_O_NONBLOCK, &err);
        int        r_poll = 0;
        int        r_info = 0;
        int        r_baud = 0;

        if (f != BR_NULL) {
            r_poll = br_file_poll(f, BR_NULL);
            r_info = br_file_ioctl(f, IO_CONF_CMD_GET_INFO, BR_NULL);
            r_baud = br_file_ioctl(f, IO_CONF_CMD_SET_BAUD, BR_NULL);
        }

        const br_bool ok = (f != BR_NULL) && (err == 0) &&
                           (r_poll == BR_ERR(BR_EINVAL)) &&
                           (r_info == BR_ERR(BR_EINVAL)) &&
                           (r_baud == BR_ERR(BR_EINVAL));
        io_conf(ok, "TC-IO-010",
                "poll(NULL events) / GET_INFO / SET_BAUD 的 NULL arg ⇒ -EINVAL");

        if (f != BR_NULL) {
            (void)br_file_close(f);
        }
    }

    /* ---- TC-IO-011: SET_BAUD 的可表示性校验与"当前波特率"更新 ----
     * QEMU 的 stdio 串口后端不按波特率限速, 所以这里改除数**不会**把后续日志打花; 真硬件上
     * 会连带改掉 console 的速率(抬头 ⑦)⇒ 用例因此最后一定把 115200 写回。 */
    {
        int                  err    = 0;
        br_file_t           *f      = br_open_err(IO_CONF_PATH, BR_O_RDWR | BR_O_NONBLOCK, &err);
        br_u32               too_hi = 4000000u;   /* > refclk/16 ⇒ IBRD 算出 0 */
        br_u32               zero   = 0u;         /* baud=0 ⇒ 除数为 0, 必须显式拒绝 */
        br_u32               good   = 9600u;      /* 可表示: IBRD=156 FBRD=16 */
        br_u32               back   = BR_PL011_BAUD;
        int                  r_hi = 0;
        int                  r_zero = 0;
        int                  r_good = 0;
        int                  r_info1 = 0;
        int                  r_back = 0;
        int                  r_info2 = 0;
        br_uart_pl011_info_t info1 = { 0u, 0u, 0u, 0u, 0u };
        br_uart_pl011_info_t info2 = { 0u, 0u, 0u, 0u, 0u };

        if (f != BR_NULL) {
            r_hi    = br_file_ioctl(f, IO_CONF_CMD_SET_BAUD, &too_hi);
            r_zero  = br_file_ioctl(f, IO_CONF_CMD_SET_BAUD, &zero);
            r_good  = br_file_ioctl(f, IO_CONF_CMD_SET_BAUD, &good);
            r_info1 = br_file_ioctl(f, IO_CONF_CMD_GET_INFO, &info1);
            r_back  = br_file_ioctl(f, IO_CONF_CMD_SET_BAUD, &back);
            r_info2 = br_file_ioctl(f, IO_CONF_CMD_GET_INFO, &info2);
        }

        const br_bool ok = (f != BR_NULL) && (err == 0) &&
                           (r_hi == BR_ERR(BR_EINVAL)) &&
                           (r_zero == BR_ERR(BR_EINVAL)) &&
                           (r_good == 0) && (r_info1 == 0) && (info1.baud == good) &&
                           (r_back == 0) && (r_info2 == 0) && (info2.baud == BR_PL011_BAUD);
        io_conf(ok, "TC-IO-011",
                "SET_BAUD: 4MHz/0 ⇒ -EINVAL; 9600 生效并回读; 复原 115200");

        if (f != BR_NULL) {
            (void)br_file_close(f);
        }
    }

    /* ---- TC-IO-012: FLUSH_RX 把 FIFO 抽干 ---- */
    {
        int                  err   = 0;
        br_file_t           *f     = br_open_err(IO_CONF_PATH, BR_O_RDWR | BR_O_NONBLOCK, &err);
        br_uart_pl011_info_t info  = { 0u, 0u, 0u, 0u, 0u };
        int                  r_flush = 0;
        int                  r_info  = 0;

        if (f != BR_NULL) {
            r_flush = br_file_ioctl(f, BR_IO(BR_UART_IOC_MAGIC, BR_UART_IOC_FLUSH_RX), BR_NULL);
            r_info  = br_file_ioctl(f, IO_CONF_CMD_GET_INFO, &info);
        }

        const br_bool ok = (f != BR_NULL) && (err == 0) && (r_flush == 0) &&
                           (r_info == 0) && (info.rx_available == 0u);
        io_conf(ok, "TC-IO-012", "FLUSH_RX 之后 rx_available == 0");

        if (f != BR_NULL) {
            (void)br_file_close(f);
        }
    }

    br_log_info("[IOCONF] SUMMARY pass=%u fail=%u total=%u",
                s_io_pass, s_io_fail, s_io_pass + s_io_fail);
    return (int)s_io_fail;
}
