/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qapi/error.h"
#include "qobject/qdict.h"
#include "qobject/qjson.h"
#define BASE 0x42000800
#define FIFO 0x42001000
#define DMA 0x40000000
#define DMA2D 0x44900000
static char *flash, *pixels;
static QTestState *start_with_source(const char *source_options)
{
    g_autofree char *f = g_shell_quote(flash);
    g_autofree char *p = g_shell_quote(pixels);
    g_autofree char *extra = source_options ? g_strdup_printf(
        " -device listenai-video-source,id=source,sink=/machine/soc,file=%s,%s",
        p, source_options) : g_strdup("");
    QTestState *q = qtest_initf("-machine venusa-rd-evb -drive "
        "if=pflash,format=raw,file=%s%s", f, extra);
    qtest_writel(q, 0x43100034, 0x800);
    return q;
}
static QTestState *start(bool source)
{
    return start_with_source(source ?
        "width=4,height=4,pixel-hz=1000000,frames=1,delay-ns=1000000,period-ns=1000000" : NULL);
}
static void pin(QTestState *q, unsigned n, unsigned value)
{ qtest_set_irq_in(q, "/machine/soc", "dvp-input", n, value); }
static void byte_edge(QTestState *q, unsigned value, bool rising)
{
    pin(q, 3, value); pin(q, 2, rising ? 0 : 1); pin(q, 2, rising ? 1 : 0);
}
static void byte(QTestState *q, unsigned value)
{ byte_edge(q, value, false); }
static void begin_edge(QTestState *q, bool vsync_active_low, bool rising)
{
    int inactive = vsync_active_low;
    int active = !inactive;
    pin(q, 0, active); byte_edge(q, 0, rising);
    pin(q, 0, inactive); byte_edge(q, 0, rising); pin(q, 1, 1);
}
static void begin(QTestState *q)
{ begin_edge(q, false, false); }
static void configure_format(QTestState *q, unsigned width, unsigned height,
                             unsigned format, unsigned polarity)
{
    qtest_writel(q, BASE, width); qtest_writel(q, BASE + 4, height);
    qtest_writel(q, BASE + 0x10, polarity); qtest_writel(q, BASE + 0x18, format);
    qtest_writel(q, BASE + 0x24, 0xfff); qtest_writel(q, BASE + 0x1c, 1);
}
static void configure(QTestState *q, unsigned width, unsigned height)
{ configure_format(q, width, height, 0, 2); }
static void receive(void)
{
    QTestState *q = start(false); configure(q, 4, 1);
    qtest_clock_step(q, 1000000);
    g_assert_cmphex(qtest_readl(q, BASE + 0x34), ==, 0x10);
    qtest_writel(q, BASE + 0x24, 0x10);
    begin(q);
    g_assert_cmphex(qtest_readl(q, BASE + 0x34), ==, 0x90);
    for (unsigned i = 0; i < 8; i++) { byte(q, 0x10 + i); }
    g_assert_cmphex(qtest_readl(q, FIFO), ==, 0x13121110);
    g_assert_cmphex(qtest_readl(q, FIFO), ==, 0x17161514);
    g_assert_cmphex(qtest_readl(q, BASE + 0x34) & 0x200, ==, 0x200);
    g_assert_cmpint(qtest_readb(q, 0xe0021000 + 57 * 4), ==, 1);
    qtest_writel(q, BASE + 0x28, 0xfff);
    g_assert_cmpint(qtest_readb(q, 0xe0021000 + 57 * 4), ==, 0);
    g_assert_cmphex(qtest_readl(q, BASE + 0x28), ==, 0);
    qtest_quit(q);
}
static void dma2d_rgb888_to_yuyv(void)
{
    QTestState *q = start(false);
    const uint8_t rgb[] = {
        255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255
    };
    const uint8_t expected[] = {
        77, 64, 149, 138, 29, 192, 255, 118
    };
    uint8_t actual[sizeof(expected)];

    qtest_writel(q, 0x43100034, qtest_readl(q, 0x43100034) | 16);
    qtest_writel(q, 0x431000a0, 1u << 15); /* DVP to GPDMA2D, not CMDMA. */
    qtest_writel(q, BASE + 0x20, 0x101);
    configure_format(q, 4, 1, 0x10, 0);
    begin(q);
    memset(actual, 0xa5, sizeof(actual));
    qtest_memwrite(q, 0x20030000, actual, sizeof(actual));

    qtest_writel(q, DMA2D + 3 * 4, 1 | (2u << 3) | (2u << 5) |
                 (2u << 8) | (2u << 15) | (3u << 28) | (1u << 23));
    qtest_writel(q, DMA2D + 0x220 + 3 * 4, 1);
    qtest_writel(q, DMA2D + 0x240 + 3 * 4, 0x1e);
    qtest_writel(q, DMA2D + 0x100 + 3 * 4, 0x1030); /* RGB888 -> YUYV422. */
    qtest_writel(q, DMA2D + 0x120 + 3 * 4, 4 | (1u << 16));
    qtest_writel(q, DMA2D + 0x140 + 3 * 4, 4 | (1u << 16));
    qtest_writel(q, DMA2D + 0x160 + 3 * 4, 12 | (8u << 16));
    qtest_writel(q, DMA2D + 0xa0 + 3 * 4, 0x42001000);
    qtest_writel(q, DMA2D + 0xc0 + 3 * 4, 0x20030000);
    qtest_writel(q, DMA2D + 0xe0 + 3 * 4, sizeof(rgb));
    qtest_writel(q, DMA2D + 0x200 + 3 * 4, 1);
    qtest_clock_step(q, 100000);
    g_assert_cmphex(qtest_readl(q, 0x20030000), ==, 0xa5a5a5a5);
    for (unsigned i = 0; i < 4; i++) { byte(q, rgb[i]); }
    qtest_writel(q, BASE + 0x20, 0);
    qtest_clock_step(q, 100000);
    g_assert_cmpuint(qtest_readl(q, DMA2D + 0x320 + 3 * 4), ==, sizeof(rgb));
    qtest_writel(q, BASE + 0x20, 0x101);
    qtest_clock_step(q, 100000);
    g_assert_cmphex(qtest_readl(q, 0x20030000), ==, 0xa5a5a5a5);
    g_assert_cmpuint(qtest_readl(q, DMA2D + 0x320 + 3 * 4), ==, 8);
    g_assert_cmphex(qtest_readl(q, DMA2D + 0x280 + 3 * 4) & 1, ==, 0);
    qtest_writel(q, DMA2D + 0x360, 1u << 3);
    for (unsigned i = 4; i < sizeof(rgb); i++) { byte(q, rgb[i]); }
    qtest_clock_step(q, 100000);
    g_assert_cmphex(qtest_readl(q, 0x20030000), ==, 0xa5a5a5a5);
    /* Restart both ends: the cancelled partial RGB pair must not survive. */
    qtest_writel(q, 0x43100014, 1u << 21);
    qtest_writel(q, BASE + 0x20, 0x101);
    configure_format(q, 4, 1, 0x10, 0);
    pin(q, 1, 0);
    begin(q);
    qtest_writel(q, DMA2D + 0x260 + 3 * 4, 31);
    qtest_writel(q, DMA2D + 0x200 + 3 * 4, 1);
    for (unsigned i = 0; i < sizeof(rgb); i++) { byte(q, rgb[i]); }
    qtest_clock_step(q, 100000);
    qtest_memread(q, 0x20030000, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), expected, sizeof(expected));
    g_assert_cmpuint(qtest_readl(q, DMA2D + 0x340 + 3 * 4), ==, 1);
    g_assert_cmphex(qtest_readl(q, DMA2D + 0x280 + 3 * 4) & 1, ==, 1);
    qtest_quit(q);
}

static void dma2d_yuyv_copy(void)
{
    QTestState *q = start(false);
    const uint8_t input[] = {16, 80, 32, 90, 48, 100, 64, 110};
    uint8_t expected[12], actual[12];
    memset(expected, 0xa5, sizeof(expected));
    qtest_memwrite(q, 0x20030000, expected, sizeof(expected));
    qtest_writel(q, 0x43100034, qtest_readl(q, 0x43100034) | 16);
    qtest_writel(q, 0x431000a0, 1u << 15);
    qtest_writel(q, BASE + 0x20, 0x101);
    configure(q, 4, 1); begin(q);
    qtest_writel(q, DMA2D + 12, 1 | (2u << 3) | (2u << 5) | (2u << 8) |
                 (2u << 15) | (3u << 28) | (1u << 23));
    qtest_writel(q, DMA2D + 0x220 + 12, 1);
    qtest_writel(q, DMA2D + 0x100 + 12, 0x33);
    qtest_writel(q, DMA2D + 0x120 + 12, 0x10004);
    qtest_writel(q, DMA2D + 0x140 + 12, 0x10004);
    qtest_writel(q, DMA2D + 0x160 + 12, 0x80008);
    qtest_writel(q, DMA2D + 0xa0 + 12, FIFO);
    qtest_writel(q, DMA2D + 0xc0 + 12, 0x20030000);
    qtest_writel(q, DMA2D + 0xe0 + 12, sizeof(input));
    qtest_writel(q, DMA2D + 0x200 + 12, 1);
    qtest_clock_step(q, 100000);
    g_assert_cmphex(qtest_readl(q, 0x20030000), ==, 0xa5a5a5a5);
    for (unsigned i = 0; i < sizeof(input); ++i) byte(q, input[i]);
    qtest_clock_step(q, 100000);
    memcpy(expected, input, sizeof(input));
    qtest_memread(q, 0x20030000, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), expected, sizeof(expected));
    g_assert_cmphex(qtest_readl(q, DMA2D + 0x280 + 12) & 1, ==, 1);
    qtest_quit(q);
}

static void overflow_reset(void)
{
    QTestState *q = start(false); configure(q, 130, 1); begin(q);
    for (unsigned i = 0; i < 260; i++) { byte(q, i & 255); }
    g_assert_cmphex(qtest_readl(q, BASE + 0x34) & 0x28, ==, 0x28);
    for (unsigned i = 0; i < 64; i++) {
        uint32_t expected = (i * 4) | ((i * 4 + 1) << 8) | ((i * 4 + 2) << 16) | ((i * 4 + 3) << 24);
        g_assert_cmphex(qtest_readl(q, FIFO), ==, expected);
    }
    qtest_writel(q, 0x43100014, 1u << 21);
    g_assert_cmphex(qtest_readl(q, BASE + 0x1c), ==, 0);
    g_assert_cmphex(qtest_readl(q, BASE + 0x34), ==, 0x10);
    qtest_quit(q);
}
static void dma_source(void)
{
    QTestState *q = start(true);
    qtest_writel(q, 0x431000a0, (1u << 3) | (1u << 15) | (1u << 19));
    qtest_writel(q, DMA + 0x398, 1);
    qtest_writel(q, DMA, FIFO); qtest_writel(q, DMA + 8, 0x20030000);
    qtest_writel(q, DMA + 0x18, 1 | (2 << 1) | (2 << 4) | (2 << 9) | (2 << 20));
    qtest_writel(q, DMA + 0x1c, 8);
    qtest_writel(q, DMA + 0x44, 3 << 7);
    qtest_writel(q, DMA + 0x3a0, 0x101);
    qtest_writel(q, BASE + 0x20, 0x101);
    configure(q, 4, 4);
    qtest_clock_step(q, 1005999);
    g_assert_cmphex(qtest_readl(q, 0x20030000), ==, 0);
    qtest_clock_step(q, 1);
    g_assert_cmphex(qtest_readl(q, 0x20030000), ==, 0x83828180);
    qtest_clock_step(q, 40000);
    for (unsigned i = 0; i < 8; i++) {
        uint32_t a = 0x80 + i * 4;
        g_assert_cmphex(qtest_readl(q, 0x20030000 + i * 4), ==, a | ((a+1)<<8) | ((a+2)<<16) | ((a+3)<<24));
    }
    g_assert_cmphex(qtest_readl(q, DMA + 0x2c0) & 1, ==, 1);
    g_assert_cmphex(qtest_readl(q, BASE + 0x34) & 0x240, ==, 0x240);
    qtest_quit(q);
}
static void rgb888_crop_fifo(void)
{
    QTestState *q = start(false);
    qtest_writel(q, BASE + 8, 1);
    configure_format(q, 4, 1, 0x10, 0);
    begin(q);
    for (unsigned i = 0; i < 15; i++) { byte(q, i); }
    g_assert_cmphex(qtest_readl(q, FIFO), ==, 0x06050403);
    g_assert_cmphex(qtest_readl(q, FIFO), ==, 0x0a090807);
    g_assert_cmphex(qtest_readl(q, FIFO), ==, 0x0e0d0c0b);
    g_assert_cmphex(qtest_readl(q, BASE + 0x34) & 0x200, ==, 0x200);
    qtest_quit(q);
}
static void active_low_and_rising_pclk(void)
{
    QTestState *q = start_with_source(
        "width=4,height=4,pixel-hz=1000000,frames=1,delay-ns=1000000,"
        "period-ns=1000000,vsync-active-low=true,pclk-inverted=true");
    configure_format(q, 4, 4, 0, 9);
    qtest_clock_step(q, 1005999);
    qtest_clock_step(q, 40000);
    for (unsigned i = 0; i < 8; i++) {
        uint32_t a = 0x80 + i * 4;
        g_assert_cmphex(qtest_readl(q, FIFO), ==,
            a | ((a + 1) << 8) | ((a + 2) << 16) | ((a + 3) << 24));
    }
    g_assert_cmphex(qtest_readl(q, BASE + 0x34) & 0x200, ==, 0x200);
    qtest_quit(q);
}
static void disabled_no_frame(void)
{
    QTestState *q = start(false);
    qtest_writel(q, BASE, 4); qtest_writel(q, BASE + 4, 1);
    qtest_writel(q, BASE + 0x18, 0x10); qtest_writel(q, BASE + 0x1c, 0);
    begin(q); for (unsigned i = 0; i < 12; i++) { byte(q, i); }
    g_assert_cmphex(qtest_readl(q, BASE + 0x34) & 0x80, ==, 0);
    g_assert_cmphex(qtest_readl(q, FIFO), ==, 0);
    qtest_writel(q, BASE + 0x14, 1);
    g_assert_cmphex(qtest_readl(q, BASE + 0x34), ==, 0x10);
    qtest_quit(q);
}
static void source_stats(void)
{
    QTestState *q = start(true);
    qtest_clock_step(q, 1100000);
    g_autoptr(QDict) result = qtest_qmp(q, "{'execute':'qom-get',"
        "'arguments':{'path':'/machine/peripheral/source',"
        "'property':'x-lisem-video-source'}}");
    QObject *data = qobject_from_json(qdict_get_str(result, "return"), &error_abort);
    QDict *stats = qobject_to(QDict, data);
    g_assert_cmpuint(qdict_get_int(stats, "emitted_frames"), ==, 1);
    g_assert_cmpuint(qdict_get_int(stats, "emitted_bytes"), ==, 32);
    qobject_unref(data); qtest_quit(q);
}
static void crop_gate_cancel(void)
{
    QTestState *q = start(false);
    qtest_writel(q, BASE + 8, 1); qtest_writel(q, BASE + 0xc, 1);
    configure(q, 2, 1);
    begin(q);
    for (unsigned i = 0; i < 8; i++) { byte(q, i); }
    pin(q, 1, 0); byte(q, 0); pin(q, 1, 1);
    for (unsigned i = 0; i < 8; i++) { byte(q, 0x20 + i); }
    g_assert_cmphex(qtest_readl(q, FIFO), ==, 0x25242322);
    qtest_writel(q, BASE + 0x14, 1);
    g_assert_cmphex(qtest_readl(q, BASE), ==, 2);
    g_assert_cmphex(qtest_readl(q, BASE + 4), ==, 1);
    g_assert_cmphex(qtest_readl(q, BASE + 0x34), ==, 0x10);
    qtest_writel(q, 0x43100034, 0);
    begin(q); for (unsigned i = 0; i < 8; i++) { byte(q, 0x50 + i); }
    g_assert_cmphex(qtest_readl(q, BASE + 0x34), ==, 0x10);
    qtest_writel(q, 0x43100034, 0x800);
    qtest_writel(q, BASE + 0x1c, 0);
    begin(q); for (unsigned i = 0; i < 8; i++) { byte(q, 0x70 + i); }
    g_assert_cmphex(qtest_readl(q, FIFO), ==, 0);
    g_assert_cmphex(qtest_readl(q, BASE + 0x34), ==, 0x10);
    qtest_quit(q);
}
static void rejected(const void *opaque)
{
    uint32_t offset = GPOINTER_TO_UINT(opaque);
    if (g_test_subprocess()) {
        QTestState *q = start(false); qtest_set_expected_status(q, 1);
        qtest_writel(q, BASE + offset, offset == 0x18 ? 0x18 : 0x80000000);
        qtest_quit(q); return;
    }
    g_test_trap_subprocess(NULL, 5 * G_USEC_PER_SEC, 0);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr("*VenusA unsupported write*address=0x420008*");
}
int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    int fd = g_file_open_tmp("venusa-dvp-flash-XXXXXX", &flash, NULL);
    g_assert_cmpint(fd, >=, 0); g_assert_cmpint(ftruncate(fd, 16 * 1024 * 1024), ==, 0); close(fd);
    fd = g_file_open_tmp("venusa-dvp-pixels-XXXXXX", &pixels, NULL);
    g_assert_cmpint(fd, >=, 0);
    uint8_t data[32]; for (unsigned i = 0; i < 32; i++) { data[i] = 0x80 + i; }
    g_assert_cmpint(write(fd, data, sizeof(data)), ==, sizeof(data)); close(fd);
    qtest_add_func("venusa/dvp/receive", receive);
    qtest_add_func("venusa/dvp/dma2d-rgb888-to-yuyv", dma2d_rgb888_to_yuyv);
    qtest_add_func("venusa/dvp/dma2d-yuyv-copy", dma2d_yuyv_copy);
    qtest_add_func("venusa/dvp/overflow-reset", overflow_reset);
    qtest_add_func("venusa/dvp/dma-source", dma_source);
    qtest_add_func("venusa/dvp/crop-gate-cancel", crop_gate_cancel);
    qtest_add_func("venusa/dvp/rgb888-crop-fifo", rgb888_crop_fifo);
    qtest_add_func("venusa/dvp/active-low-rising-pclk", active_low_and_rising_pclk);
    qtest_add_func("venusa/dvp/disabled-no-frame", disabled_no_frame);
    qtest_add_func("venusa/dvp/source-stats", source_stats);
    qtest_add_data_func("venusa/dvp/reject/format", GUINT_TO_POINTER(0x18), rejected);
    qtest_add_data_func("venusa/dvp/reject/control", GUINT_TO_POINTER(0x10), rejected);
    int result = g_test_run(); unlink(flash); unlink(pixels); g_free(flash); g_free(pixels); return result;
}
