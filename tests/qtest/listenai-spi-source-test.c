/* SPDX-License-Identifier: GPL-2.0-or-later */
/* External pins -> real SPI receive FIFO/DMA, never a guest-memory injector. */
#include "qemu/osdep.h"
#include "libqtest.h"

#define SPI0 0x43e00000
#define DMA 0x40000000
#define DEST 0x20010004
static char *flash, *source_file;

static void payload(const uint8_t *bytes, unsigned size)
{
    g_autofree uint8_t *data = g_malloc(size + 12);
    memcpy(data, "LSPI0001", 8);
    for (unsigned i = 0; i < 4; i++) { data[8+i] = size >> (8*i); }
    memcpy(data + 12, bytes, size);
    g_assert_true(g_file_set_contents(source_file, (char *)data, size + 12, NULL));
}
static QTestState *start(unsigned mode, bool alt, const char *extra)
{
    g_autofree char *f = g_shell_quote(flash), *input = g_shell_quote(source_file);
    return qtest_initf("-machine venusa-rd-evb -drive if=pflash,format=raw,file=%s "
        "-device listenai-spi-source,id=source,sink=/machine/soc,clock-pad=%u,mosi-pad=%u,cs-pad=%u,"
        "file=%s,clock-hz=1000000,mode=%u,delay-ns=1000,gap-ns=1000,period-ns=100000,repeats=1%s",
        f, alt ? 22 : 16, alt ? 25 : 19, alt ? 23 : 17, input, mode, extra);
}
static void configure(QTestState *q, unsigned mode, bool alt, bool merge)
{
    for (unsigned i = alt ? 22 : 16; i <= (alt ? 25 : 19); i++) {
        qtest_writel(q, 0x44100000 + i * 4, 5);
    }
    /* RX-only camera leaves MISO as GPIO input. */
    qtest_writel(q, 0x44100000 + (alt ? 24 : 18) * 4, 0);
    qtest_writel(q, 0x4310001c, 0x93800000);
    qtest_writel(q, SPI0 + 0x10, 0x704 | mode | (merge ? 0x80 : 0));
    qtest_writel(q, SPI0 + 0x20, 2u << 24);
    qtest_writel(q, SPI0 + 0x38, 16);
}
static void receive_mode(const void *opaque)
{
    unsigned spec = GPOINTER_TO_UINT(opaque), mode = spec & 3;
    const uint8_t data[] = {0x91, 0x37, 0xd5};
    payload(data, sizeof(data));
    QTestState *q = start(mode, spec & 4, "");
    configure(q, mode, spec & 4, false);
    qtest_clock_step(q, 24999);
    g_assert_cmphex(qtest_readl(q, SPI0 + 0x3c) & 16, ==, 0);
    qtest_clock_step(q, 1);
    g_assert_cmphex(qtest_readl(q, SPI0 + 0x3c) & 16, ==, 16);
    g_assert_cmpuint(qtest_readl(q, SPI0 + 0x64), ==, sizeof(data));
    for (unsigned i = 0; i < sizeof(data); i++) {
        g_assert_cmphex(qtest_readl(q, SPI0 + 0x2c), ==, data[i]);
    }
    qtest_quit(q);
}
static void dma_guard(void)
{
    const uint8_t data[] = {0x91,0x37,0xd5,0x2a,0x80,0x6b,0xe1};
    payload(data, sizeof(data));
    QTestState *q = start(0, true, ""); configure(q, 0, true, true);
    for (unsigned i = 0; i < 4; i++) { qtest_writel(q, DEST - 4 + i*4, 0xabababab); }
    qtest_writel(q, DMA, SPI0 + 0x2c); qtest_writel(q, DMA + 8, DEST);
    qtest_writel(q, DMA + 0x18, 0x200425); qtest_writel(q, DMA + 0x1c, 2);
    qtest_writel(q, DMA + 0x44, 6u << 7); qtest_writel(q, DMA + 0x398, 1);
    qtest_writel(q, DMA + 0x3a0, 0x101); qtest_writel(q, SPI0 + 0x30, 8);
    qtest_clock_step(q, 56999);
    g_assert_cmphex(qtest_readl(q, SPI0 + 0x3c) & 16, ==, 0);
    g_assert_cmphex(qtest_readl(q, DEST + 4), ==, 0xabababab);
    qtest_clock_step(q, 101);
    g_assert_cmphex(qtest_readl(q, DEST), ==, 0x2ad53791);
    g_assert_cmphex(qtest_readl(q, DEST + 4), ==, 0x00e16b80);
    g_assert_cmphex(qtest_readl(q, DEST - 4), ==, 0xabababab);
    g_assert_cmphex(qtest_readl(q, DEST + 8), ==, 0xabababab);
    g_assert_cmphex(qtest_readl(q, DMA + 0x2c0) & 1, ==, 1);
    qtest_quit(q);
}
static void disconnected(const void *opaque)
{
    const uint8_t data[] = {0xe7}; payload(data, sizeof(data));
    QTestState *q = start(0, false, ""); configure(q, 0, false, false);
    if (GPOINTER_TO_UINT(opaque)) { qtest_writel(q, 0x4310001c, 0); }
    else { qtest_writel(q, 0x44100000 + 16*4, 0); }
    qtest_clock_step(q, 100000);
    g_assert_cmpuint(qtest_readl(q, SPI0 + 0x64), ==, 0);
    g_assert_cmphex(qtest_readl(q, SPI0 + 0x34) & 0x1f00, ==, 0);
    qtest_quit(q);
}
static void reset_partial(void)
{
    const uint8_t data[] = {0xe7}; payload(data, sizeof(data));
    QTestState *q = start(0, false, ""); configure(q, 0, false, false);
    qtest_clock_step(q, 4499); /* Fewer than eight sampled bits. */
    qtest_system_reset(q); qtest_clock_step(q, 100000);
    g_assert_cmpuint(qtest_readl(q, SPI0 + 0x64), ==, 0);
    g_assert_cmphex(qtest_readl(q, SPI0 + 0x3c) & 16, ==, 0);
    qtest_quit(q);
}
static void reject(const void *opaque)
{
    unsigned which = GPOINTER_TO_UINT(opaque);
    if (g_test_subprocess()) {
        const uint8_t data[] = {0xa5}; payload(data, sizeof(data));
        const char *extra = "";
        if (which == 0) { g_file_set_contents(source_file, "bad-header-123", 14, NULL); }
        if (which == 1) { uint8_t b[12] = "LSPI0001"; g_file_set_contents(source_file, (char *)b, sizeof(b), NULL); }
        if (which == 2) { uint8_t b[13] = "LSPI0001"; b[8]=2; g_file_set_contents(source_file, (char *)b, sizeof(b), NULL); }
        if (which == 3) { extra = ",clock-pad=19"; }
        if (which == 4) { extra = ",cs-pad=64"; }
        if (which == 5) { extra = ",mode=4"; }
        if (which == 6) { extra = ",clock-hz=0"; }
        if (which == 7) { extra = ",period-ns=1"; }
        if (which == 8) { uint8_t b[12] = "LSPI0001"; b[8]=1; b[10]=0x20; g_file_set_contents(source_file, (char *)b, sizeof(b), NULL); }
        if (which == 9) { uint8_t b[14] = "LSPI0001"; b[8]=1; g_file_set_contents(source_file, (char *)b, sizeof(b), NULL); }
        if (which == 10) { g_file_set_contents(source_file, "LSPI0001", 8, NULL); }
        if (which == 11) { extra = ",sink=/machine/soc/spi0"; }
        if (which == 12) { extra = ",delay-ns=9223372036854775807"; }
        QTestState *q = start(0, false, extra); qtest_quit(q); return;
    }
    g_test_trap_subprocess(NULL, 5 * G_USEC_PER_SEC, 0);
    g_test_trap_assert_failed(); g_test_trap_assert_stderr("*SPI source*");
}
int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    int fd = g_file_open_tmp("listenai-spi-source-XXXXXX", &source_file, NULL);
    g_assert_cmpint(fd, >=, 0); close(fd);
    fd = g_file_open_tmp("listenai-spi-flash-XXXXXX", &flash, NULL);
    g_assert_cmpint(fd, >=, 0); g_assert_cmpint(ftruncate(fd,16*1024*1024), ==, 0); close(fd);
    for (unsigned mode=0; mode<4; mode++) {
        g_autofree char *name=g_strdup_printf("spi-source/mode%u",mode);
        qtest_add_data_func(name,GUINT_TO_POINTER(mode),receive_mode);
    }
    qtest_add_data_func("spi-source/alternate-route",GUINT_TO_POINTER(4),receive_mode);
    qtest_add_func("spi-source/dma-guard",dma_guard);
    qtest_add_data_func("spi-source/unrouted",GUINT_TO_POINTER(0),disconnected);
    qtest_add_data_func("spi-source/gated",GUINT_TO_POINTER(1),disconnected);
    qtest_add_func("spi-source/reset-partial",reset_partial);
    for (unsigned i=0;i<13;i++) {
        g_autofree char *name=g_strdup_printf("spi-source/reject/%u",i);
        qtest_add_data_func(name,GUINT_TO_POINTER(i),reject);
    }
    int result=g_test_run(); unlink(source_file); unlink(flash);
    g_free(source_file); g_free(flash); return result;
}
