/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"

#define LUNA 0x46000000
static char *flash;
static QTestState *start(void)
{
    g_autofree char *path = g_shell_quote(flash);
    return qtest_initf("-machine venusa-rd-evb -drive "
                       "if=pflash,format=raw,file=%s", path);
}
static void control(void)
{
    QTestState *q = start();
    qtest_writel(q, LUNA + 0xc0, 0x7fff);
    g_assert_cmphex(qtest_readl(q, LUNA + 0xc0), ==, 0x7fff);
    qtest_writel(q, LUNA + 0xd4, 0xfffffffe);
    g_assert_cmphex(qtest_readl(q, LUNA + 0xd4), ==, 0xfffffe);
    g_assert_cmphex(qtest_readl(q, LUNA + 0xd8), ==, 0);
    g_assert_cmphex(qtest_readl(q, LUNA + 0xdc), ==, 0);
    qtest_writel(q, LUNA + 0x100, 4);
    qtest_writel(q, LUNA + 0x1d8, UINT32_MAX);
    qtest_writel(q, LUNA + 0x1d4, UINT32_MAX);
    qtest_writel(q, LUNA + 0xe0, 0x44c0);
    g_assert_cmphex(qtest_readl(q, LUNA + 0x100), ==, 4);
    g_assert_cmphex(qtest_readl(q, LUNA + 0xe0), ==, 0x44c0);
    qtest_writel(q, 0x43100014, 1u << 27);
    g_assert_cmphex(qtest_readl(q, LUNA + 0x100), ==, 0);
    g_assert_cmphex(qtest_readl(q, LUNA + 0xd8), ==, 0);
    g_assert_cmpuint(qtest_readb(q, 0xe00210a8), ==, 0);
    qtest_quit(q);
}

static void private_ram(void)
{
    QTestState *q = start();
    uint8_t expected[0x4000], actual[0x4000];

    qtest_writel(q, 0x43100034, 0x80);
    for (unsigned bank = 0; bank < 8; bank++) {
        for (unsigned i = 0; i < sizeof(expected) / 4; i++) {
            unsigned word = bank * (sizeof(expected) / 4) + i;
            stl_le_p(expected + i * 4, (word * 0x9e3779b9u) ^ 0x12345678u);
        }
        qtest_memwrite(q, 0x20080000 + bank * sizeof(expected),
                      expected, sizeof(expected));
    }
    qtest_writel(q, LUNA + 0xc0, 0x7fff);
    for (unsigned bank = 0; bank < 8; bank++) {
        for (unsigned i = 0; i < sizeof(expected) / 4; i++) {
            unsigned word = bank * (sizeof(expected) / 4) + i;
            stl_le_p(expected + i * 4, (word * 0x9e3779b9u) ^ 0x12345678u);
        }
        qtest_memread(q, 0x20080000 + bank * sizeof(actual),
                     actual, sizeof(actual));
        g_assert_cmpmem(actual, sizeof(actual), expected, sizeof(expected));
    }
    qtest_quit(q);
}
static void mtq_real_rgb_task(void)
{
    /* SDK luna_api_rgb_sub128, SHA256 b5de91390e0dbe6f333354626a080c10c
     * 69df9afcc27aec26b6e512793c54737: actual program bytes, not an operator stub. */
    static const uint32_t program[] = {
        0xe0403c40, 0x88000005, 0x88200002, 0x88e00004, 0x41880800, 0x90000000,
        0x90111000, 0x90002001, 0x90082001, 0x90003400, 0xe0401c00, 0x41928000,
        0x40720001, 0x41930001, 0x41940000, 0x40740800, 0x41951000, 0x40751800,
        0x960020f0, 0xf0000003, 0x80000014, 0x88600007, 0x89200002, 0x88200007,
        0x88e00004, 0xe0401c00, 0x10e00008, 0x45070003, 0x51070002, 0x62320700,
        0x40720001, 0x41930001, 0x96002260, 0xe0401c00, 0x11000000, 0x418d0001,
        0x406d0001, 0x418e0001, 0x10e00008, 0x55070003, 0x450700ff, 0x51070008,
        0x622c0700, 0x90800820, 0xf0000042, 0x8000000c, 0x40680080, 0x41880080,
        0x40690080, 0x41890080, 0x406a0080, 0x418a0080, 0x406b0080, 0x418b0080,
        0x88000020, 0x88e00002, 0x8840000b, 0x88800005, 0xe0401c00, 0x10e00008,
        0x55070003, 0x4507003f, 0x51070006, 0x62280700, 0x418a0001, 0x90000000,
        0x90111000, 0x40712004, 0x90082001, 0x90003700, 0x418f0011, 0x41900002,
        0x90013082, 0xe0401c00, 0x4068ffff, 0x4188ffff, 0x406a0001, 0x418a0001,
        0x62090800, 0x620b0a00, 0x40762424, 0x40770010, 0x41985542, 0x407829c3,
        0x41995542, 0x407929c3, 0x419a5421, 0x407a0102, 0x419b0001, 0x98120023,
        0xe0401c00, 0x12800004, 0x12400008, 0x41930001, 0xa4000011, 0xf0000015,
        0x80000031, 0x10e00000, 0x5107001f, 0x0b070001, 0x0d180003, 0x61000000,
        0x81000000, 0x61000000, 0x0cb01e00,
    };
    QTestState *q = start();
    const uint32_t code = 0x20010000, param = 0x20018000;
    const uint32_t sq = 0x20019000, cq = 0x20019100;
    qtest_writel(q, 0x43100034, 0x80);
    for (unsigned i = 0; i < G_N_ELEMENTS(program); i++)
        qtest_writel(q, code + i * 4, program[i]);
    qtest_writel(q, param, 0x80001000); qtest_writel(q, param + 4, 0x1100);
    qtest_writel(q, param + 8, 4); qtest_writel(q, param + 12, 0);
    qtest_writel(q, 0x20021000, 0xff807f00);
    qtest_writel(q, 0x20021100, 0xaaaaaaaa);
    qtest_writel(q, sq, 0x70081); qtest_writel(q, sq + 4, code); qtest_writel(q, sq + 8, param);
    qtest_writel(q, LUNA + 0x114, sq); qtest_writel(q, LUNA + 0x134, cq);
    qtest_writew(q, LUNA + 0x1c0, 2); qtest_writew(q, LUNA + 0x1c8, 2);
    qtest_writel(q, LUNA + 0x14c, UINT32_MAX);
    qtest_writew(q, LUNA + 0x10c, 1); qtest_writel(q, LUNA + 0x1b8, 0x1001);
    qtest_writel(q, LUNA + 0xd4, UINT32_MAX);
    qtest_writel(q, LUNA + 0x1d8, UINT32_MAX);
    qtest_writel(q, LUNA + 0x100, 0x83);
    g_assert_cmphex(qtest_readl(q, 0x20021100), ==, 0xaaaaaaaa);
    g_assert_cmphex(qtest_readl(q, cq), ==, 0);
    qtest_writel(q, 0x20021000, 0); /* Inputs are already snapshotted. */
    qtest_clock_step(q, 100000);
    g_assert_cmphex(qtest_readl(q, 0x20021100), ==, 0x7f00ff80);
    g_assert_cmphex(qtest_readl(q, cq), ==, 0x70181);
    g_assert_cmpuint(qtest_readl(q, cq + 4), >, 0);
    g_assert_cmphex(qtest_readl(q, LUNA + 0x168), ==, 1);
    g_assert_cmphex(qtest_readl(q, LUNA + 0x1dc), ==, 0);
    g_assert_cmphex(qtest_readl(q, LUNA + 0x2e0) & 1, ==, 1);
    g_assert_cmphex(qtest_readb(q, 0xe00210a8) & 1, ==, 0);
    qtest_writel(q, LUNA + 0x1d8, 0);
    g_assert_cmphex(qtest_readb(q, 0xe00210a8) & 1, ==, 1);
    qtest_writel(q, LUNA + 0x1d4, UINT32_MAX);
    g_assert_cmphex(qtest_readb(q, 0xe00210a8) & 1, ==, 0);
    qtest_quit(q);
}

static void reject(const void *opaque)
{
    bool scheduler = GPOINTER_TO_UINT(opaque);
    if (g_test_subprocess()) {
        QTestState *q = start();
        qtest_set_expected_status(q, 1);
        qtest_writel(q, LUNA + (scheduler ? 0x100 : 0xc0), scheduler ? 0x20 : 1);
        qtest_quit(q);
        return;
    }
    g_test_trap_subprocess(NULL, 5 * G_USEC_PER_SEC, 0);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr(scheduler ? "*unsupported MTQ controller mode*" :
                             "*LUNA partial reset or completion-clear mask unsupported*");
}
int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    int fd = g_file_open_tmp("venusa-luna-flash-XXXXXX", &flash, NULL);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, 16 * 1024 * 1024), ==, 0);
    close(fd);
    qtest_add_func("venusa/luna/control-reset", control);
    qtest_add_func("venusa/luna/private-ram", private_ram);
    qtest_add_func("venusa/luna/mtq-real-rgb-task", mtq_real_rgb_task);
    qtest_add_data_func("venusa/luna/reject-partial-reset", GUINT_TO_POINTER(0), reject);
    qtest_add_data_func("venusa/luna/reject-scheduler", GUINT_TO_POINTER(1), reject);
    int result = g_test_run();
    unlink(flash);
    g_free(flash);
    return result;
}
