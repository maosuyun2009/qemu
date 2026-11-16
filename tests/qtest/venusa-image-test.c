/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "libqtest.h"

#define DMA 0x44900000
#define RGB 0x4200b000
#define RAM 0x20010000
#define CTRL_COPY (1u | (2u << 1) | (2u << 3) | (2u << 5) | (2u << 13) | (2u << 15))
#define CTRL_RGB (1u | (1u << 1) | (2u << 3) | (2u << 5) | (2u << 10) | (2u << 13) | (2u << 15))
static char *flash, *capture_path;
static QTestState *start(void)
{
    g_autofree char *path = g_shell_quote(flash);
    QTestState *q = qtest_initf("-machine venusa-rd-evb -drive if=pflash,format=raw,file=%s", path);
    qtest_writel(q, 0x43100034, qtest_readl(q, 0x43100034) | 16);
    qtest_writel(q, 0x43100030, (1u << 27) | (1u << 26) | (1u << 22));
    qtest_writel(q, 0x43100014, (1u << 16) | (1u << 25));
    unlink(capture_path);
    return q;
}
static void descriptor(QTestState *q, unsigned ch, uint32_t src, uint32_t dst, uint32_t size)
{
    qtest_writel(q, DMA + 0xa0 + ch * 4, src);
    qtest_writel(q, DMA + 0xc0 + ch * 4, dst);
    qtest_writel(q, DMA + 0xe0 + ch * 4, size);
    qtest_writel(q, DMA + 0x200 + ch * 4, 1);
}
static void normal_shadow(void)
{
    QTestState *q = start();
    for (unsigned i = 0; i < 32; i++) { qtest_writel(q, RAM + i * 4, 0x12340000 + i); }
    for (unsigned ch = 0; ch < 6; ch++) {
        g_assert_cmphex(qtest_readl(q, DMA + ch * 4), ==, 0x0019e040);
        qtest_writel(q, DMA + ch * 4, CTRL_COPY);
        qtest_writel(q, DMA + 0x220 + ch * 4, 1);
        qtest_writel(q, DMA + 0x240 + ch * 4, 0x1e);
        descriptor(q, ch, RAM, RAM + 0x1000 + ch * 256, 32);
        g_assert_cmpuint(qtest_readl(q, DMA + 0x200 + ch * 4), ==, 0);
        descriptor(q, ch, RAM + 32, RAM + 0x1020 + ch * 256, 32);
        g_assert_cmpuint(qtest_readl(q, DMA + 0x200 + ch * 4), ==, 1);

    }
    qtest_clock_step(q, 100000);
    for (unsigned ch = 0; ch < 6; ch++) {
        for (unsigned i = 0; i < 16; i++) {
            g_assert_cmphex(qtest_readl(q, RAM + 0x1000 + ch * 256 + i * 4), ==, 0x12340000 + i);
        }
        g_assert_cmpuint(qtest_readl(q, DMA + 0x340 + ch * 4), ==, 1);
        g_assert_cmpuint(qtest_readl(q, DMA + 0x320 + ch * 4), ==, 0);
        g_assert_cmphex(qtest_readl(q, DMA + 0x280 + ch * 4) & 5, ==, 5);
        g_assert_cmpuint(qtest_readb(q, 0xe0021000 + (49 + ch) * 4), ==, 1);
        qtest_writel(q, DMA + 0x260 + ch * 4, 1);
        g_assert_cmpuint(qtest_readb(q, 0xe0021000 + (49 + ch) * 4), ==, 0);
    }
    qtest_quit(q);
}
static void cancel_gate(void)
{
    QTestState *q = start();
    qtest_writel(q, RAM, 0x76543210);
    qtest_writel(q, DMA, CTRL_COPY);
    descriptor(q, 0, RAM, RAM + 0x100, 4);
    qtest_writel(q, 0x43100034, qtest_readl(q, 0x43100034) & ~16u);
    qtest_clock_step(q, 100000);
    g_assert_cmphex(qtest_readl(q, RAM + 0x100), ==, 0);
    qtest_writel(q, 0x43100034, qtest_readl(q, 0x43100034) | 16);
    qtest_clock_step(q, 100000);
    g_assert_cmphex(qtest_readl(q, RAM + 0x100), ==, 0x76543210);
    descriptor(q, 0, RAM, RAM + 0x104, 4);
    qtest_writel(q, DMA + 0x360, 1);
    qtest_clock_step(q, 100000);
    g_assert_cmphex(qtest_readl(q, RAM + 0x104), ==, 0);
    g_assert_cmphex(qtest_readl(q, DMA + 0x280) & 16, ==, 16);
    descriptor(q, 0, RAM, RAM + 0x108, 4);
    qtest_writel(q, 0x43100014, 1u << 16);
    qtest_clock_step(q, 100000);
    g_assert_cmphex(qtest_readl(q, RAM + 0x108), ==, 0);
    qtest_quit(q);
}
static void dma2d_m2m_yuyv_to_bgr(void)
{
    QTestState *q = start();
    const uint8_t input[] = { 80, 100, 180, 160, 0xee, 0xee };
    const uint8_t expected[] = { 31, 66, 124, 131, 166, 224, 0xa5, 0xa5 };
    uint8_t actual[sizeof(expected)];

    qtest_memwrite(q, RAM, input, sizeof(input));
    memset(actual, 0xa5, sizeof(actual));
    qtest_memwrite(q, RAM + 0x1000, actual, sizeof(actual));
    qtest_writel(q, DMA + 4 * 4, 1 | (2u << 1) | (2u << 3) |
                 (2u << 5) | (1u << 23)); /* One word per grant splits the last pixel. */
    qtest_writel(q, DMA + 0x220 + 4 * 4, 1);
    qtest_writel(q, DMA + 0x100 + 4 * 4, 3); /* YUYV422 -> BGR888. */
    qtest_writel(q, DMA + 0x120 + 4 * 4, 2 | (1u << 16));
    qtest_writel(q, DMA + 0x140 + 4 * 4, 2 | (1u << 16));
    qtest_writel(q, DMA + 0x160 + 4 * 4, 6 | (8u << 16));
    descriptor(q, 4, RAM, RAM + 0x1000, 4); /* Non-scaling image length counts input bytes. */
    qtest_clock_step(q, 100000);
    qtest_memread(q, RAM + 0x1000, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), expected, sizeof(expected));
    g_assert_cmpuint(qtest_readl(q, DMA + 0x340 + 4 * 4), ==, 1);
    g_assert_cmphex(qtest_readl(q, DMA + 0x280 + 4 * 4) & 1, ==, 1);
    qtest_quit(q);
}

static void jpeg_waits_for_pixels(void)
{
    QTestState *q = start();
    const unsigned ch = 2;
    qtest_writel(q, 0x42001804, 1); /* Pixel output is a decoder request. */
    const uint32_t ctrl = 1 | (2u << 3) | (2u << 5) | (2u << 8) | (5u << 28);

    qtest_writel(q, RAM, 0x76543210);
    qtest_writel(q, DMA + ch * 4, ctrl);
    qtest_writel(q, DMA + 0x220 + ch * 4, 1);
    descriptor(q, ch, 0x42002800, RAM, 4);
    qtest_clock_step(q, 1000000);
    g_assert_cmphex(qtest_readl(q, RAM), ==, 0x76543210);
    g_assert_cmpuint(qtest_readl(q, DMA + 0x320 + ch * 4), ==, 4);
    g_assert_cmphex(qtest_readl(q, DMA + 0x280 + ch * 4) & 1, ==, 0);
    qtest_writel(q, DMA + 0x360, 1u << ch);
    qtest_clock_step(q, 1000000);
    g_assert_cmphex(qtest_readl(q, RAM), ==, 0x76543210);
    qtest_quit(q);
}

static void rgb_pixels(void)
{
    QTestState *q = start();
    const uint16_t values[] = { 0xf800,0x07e0,0x001f,0xffff,0,0xffe0,0xf81f,0x07ff };
    for (unsigned i = 0; i < 8; i++) { qtest_writew(q, RAM + i * 2, values[i]); }
    qtest_writel(q, RGB + 4, 0x30001 | (2u << 4) | (2u << 6));
    qtest_writel(q, RGB + 8, 1 | (1u << 8) | (1u << 16) | (1u << 20));
    qtest_writel(q, RGB + 12, 1 | (1u << 8));
    qtest_writel(q, RGB + 16, 4 | (2u << 16));
    qtest_writel(q, RGB + 0x18, 0x3c);
    qtest_writel(q, DMA, CTRL_RGB);
    descriptor(q, 0, RAM, RGB + 0xf4, 16);
    qtest_writel(q, RGB, 0x101);
    qtest_clock_step(q, 100000);
    g_assert_cmphex(qtest_readl(q, RGB + 0x24) & 3, ==, 1);
    g_assert_cmpuint(qtest_readb(q, 0xe0021000 + 60 * 4), ==, 1);
    qtest_writel(q, RGB + 0x1c, 3);
    g_assert_cmpuint(qtest_readb(q, 0xe0021000 + 60 * 4), ==, 0);
    gchar *contents = NULL; gsize len = 0;
    g_assert_true(g_file_get_contents(capture_path, &contents, &len, NULL));
    const uint8_t expected[] = {255,0,0,0,255,0,0,0,255,255,255,255,
                               0,0,0,255,255,0,255,0,255,0,255,255};
    g_assert_cmpuint(len, ==, 11 + sizeof(expected));
    g_assert_cmpmem(contents, 11, "P6\n4 2\n255\n", 11);
    g_assert_cmpmem(contents + 11, sizeof(expected), expected, sizeof(expected));
    g_free(contents);
    qtest_quit(q);
}
static void starvation_no_frame(void)
{
    QTestState *q = start();
    qtest_writel(q, RGB + 4, 0x30021);
    qtest_writel(q, RGB + 8, 0);
    qtest_writel(q, RGB + 12, 0);
    qtest_writel(q, RGB + 16, 4 | (2u << 16));
    qtest_writel(q, RGB, 0x101);
    qtest_clock_step(q, 100000);
    g_assert_cmphex(qtest_readl(q, RGB + 0x24) & 0x17, ==, 0x16);
    g_assert_false(g_file_test(capture_path, G_FILE_TEST_EXISTS));
    for (unsigned i = 0; i < 4; i++) { qtest_writel(q, RGB + 0xf4, 0xf80007e0); }
    qtest_clock_step(q, 100000);
    g_assert_cmphex(qtest_readl(q, RGB + 0x24) & 3, ==, 2);
    g_assert_false(g_file_test(capture_path, G_FILE_TEST_EXISTS));
    qtest_writel(q, 0x43100014, 1u << 25);
    qtest_writel(q, RGB + 4, 0xa0);
    qtest_writel(q, RGB + 8, 0x00110101);
    qtest_writel(q, RGB + 12, 0x101);
    qtest_writel(q, RGB + 16, 0x00020004);
    for (unsigned i = 0; i < 4; i++) { qtest_writel(q, RGB + 0xf4, 0xf80007e0); }
    qtest_writel(q, RGB, 0x101);
    qtest_clock_step(q, 100000);
    g_assert_cmphex(qtest_readl(q, RGB + 0x24) & 3, ==, 1);
    g_assert_true(g_file_test(capture_path, G_FILE_TEST_EXISTS));
    qtest_quit(q);
}
static void rgb_clock_boundaries(void)
{
    QTestState *q = start();
    qtest_writel(q, RGB + 4, 0x30021);
    qtest_writel(q, RGB + 8, 1 | (1u << 8) | (1u << 16) | (1u << 20));
    qtest_writel(q, RGB + 12, 1 | (1u << 8));
    qtest_writel(q, RGB + 16, 4 | (2u << 16));
    for (unsigned i = 0; i < 4; i++) { qtest_writel(q, RGB + 0xf4, 0xf80007e0); }
    qtest_writel(q, RGB, 0x101);
    qtest_clock_step(q, 83); /* Preserve a fractional startup pixel across a divider change. */
    uint32_t cfg = qtest_readl(q, 0x43100030);
    qtest_writel(q, 0x43100030, (cfg & ~((1u << 26) | (15u << 22))) | (2u << 22));
    qtest_writel(q, 0x43100030, (cfg & ~(15u << 22)) | (2u << 22));
    qtest_clock_step(q, 167);
    g_assert_cmphex(qtest_readl(q, RGB + 0x24) & 2, ==, 0);
    qtest_clock_step(q, 1);
    g_assert_cmphex(qtest_readl(q, RGB + 0x24) & 2, ==, 2);
    cfg = qtest_readl(q, 0x43100030);
    qtest_writel(q, 0x43100030, cfg & ~(1u << 27));
    qtest_clock_step(q, 1000000);
    g_assert_cmphex(qtest_readl(q, RGB + 0x24) & 1, ==, 0);
    qtest_writel(q, 0x43100030, cfg);
    qtest_clock_step(q, 2249); /* 27 remaining pixel ticks at 12MHz, including the fraction. */
    g_assert_cmphex(qtest_readl(q, RGB + 0x24) & 1, ==, 0);
    qtest_clock_step(q, 1);
    g_assert_cmphex(qtest_readl(q, RGB + 0x24) & 1, ==, 1);
    qtest_quit(q);
}
static void rgb_pixel_deadline(void)
{
    for (unsigned late = 0; late < 2; late++) {
        QTestState *q = start();
        qtest_writel(q, RGB + 4, 0x300a0);
        qtest_writel(q, RGB + 8, 0x00110101);
        qtest_writel(q, RGB + 12, 0x101);
        qtest_writel(q, RGB + 16, 0x00020004);
        qtest_writel(q, RGB, 0x101);
        qtest_clock_step(q, late ? 876 : 874);
        for (unsigned i = 0; i < 4; i++) { qtest_writel(q, RGB + 0xf4, 0xf80007e0); }
        qtest_clock_step(q, 10000);
        g_assert_cmphex(qtest_readl(q, RGB + 0x24) & 1, ==, !late);
        g_assert_cmpint(g_file_test(capture_path, G_FILE_TEST_EXISTS), ==, !late);
        qtest_quit(q);
    }
}
static void rgb_frame_period(void)
{
    QTestState *q = start();
    qtest_writel(q, RGB + 4, 0x300a0);
    qtest_writel(q, RGB + 8, 0x00110101);
    qtest_writel(q, RGB + 12, 0x101);
    qtest_writel(q, RGB + 16, 0x00020004);
    for (unsigned i = 0; i < 8; i++) { qtest_writel(q, RGB + 0xf4, 0xf80007e0); }
    qtest_writel(q, RGB, 0x111);
    qtest_clock_step(q, 167);
    g_assert_cmphex(qtest_readl(q, RGB + 0x24) & 3, ==, 2);
    qtest_clock_step(q, 1125);
    g_assert_cmphex(qtest_readl(q, RGB + 0x24) & 3, ==, 1);
    qtest_clock_step(q, 374);
    g_assert_cmphex(qtest_readl(q, RGB + 0x24) & 3, ==, 1);
    qtest_clock_step(q, 1); /* 36 clocks between consecutive SOF edges. */
    g_assert_cmphex(qtest_readl(q, RGB + 0x24) & 3, ==, 2);
    qtest_clock_step(q, 1125);
    g_assert_cmphex(qtest_readl(q, RGB + 0x24) & 3, ==, 1);
    qtest_clock_step(q, 375);
    g_assert_cmphex(qtest_readl(q, RGB + 0x24) & 3, ==, 2);
    qtest_clock_step(q, 1125);
    g_assert_cmphex(qtest_readl(q, RGB + 0x24) & 3, ==, 2); /* No third frame data. */
    qtest_quit(q);
}
static void rgb_packed_formats(void)
{
    for (unsigned input = 0; input < 2; input++) {
        for (unsigned output = 0; output < 3; output++) {
            for (unsigned swap = 0; swap < 2; swap++) {
                QTestState *q = start();
                qtest_writel(q, RGB + 4, input << 4 | output << 6);
                qtest_writel(q, RGB + 8, 0x00110101 | swap << 28);
                qtest_writel(q, RGB + 12, 0x101);
                qtest_writel(q, RGB + 16, 0x00020004);
                uint8_t bytes[32], expected[24];
                unsigned size = input ? 4 : 3;
                for (unsigned i = 0; i < 8; i++) {
                    bytes[i * size] = 0x96;
                    bytes[i * size + 1] = 0x5a;
                    bytes[i * size + 2] = 0x33;
                    if (input) { bytes[i * size + 3] = 0xa5; }
                    uint8_t r = swap ? 0x96 : 0x33, g = 0x5a, b = swap ? 0x33 : 0x96;
                    if (output == 1) { r = (r >> 2) * 255 / 63; g = (g >> 2) * 255 / 63; b = (b >> 2) * 255 / 63; }
                    if (output == 2) { r = (r >> 3) * 255 / 31; g = (g >> 2) * 255 / 63; b = (b >> 3) * 255 / 31; }
                    expected[i * 3] = r; expected[i * 3 + 1] = g; expected[i * 3 + 2] = b;
                }
                for (unsigned i = 0; i < size * 8; i += 4) {
                    qtest_writel(q, RGB + 0xf4, (uint32_t)bytes[i] | (uint32_t)bytes[i+1] << 8 |
                                 (uint32_t)bytes[i+2] << 16 | (uint32_t)bytes[i+3] << 24);
                }
                qtest_writel(q, RGB, 0x101);
                qtest_clock_step(q, 100000);
                gchar *contents = NULL; gsize len = 0;
                g_assert_true(g_file_get_contents(capture_path, &contents, &len, NULL));
                g_assert_cmpuint(len, ==, 35);
                g_assert_cmpmem(contents + 11, sizeof(expected), expected, sizeof(expected));
                g_free(contents);
                qtest_quit(q);
            }
        }
    }
}
static void rgb_fifo_latches(void)
{
    QTestState *q = start();
    qtest_writel(q, RGB + 4, 0x300a1);
    g_assert_cmphex(qtest_readl(q, RGB + 0x24), ==, 0x14);
    for (unsigned i = 0; i < 16; i++) { qtest_writel(q, RGB + 0xf4, i); }
    g_assert_cmphex(qtest_readl(q, RGB + 0x24), ==, 0x3c);
    qtest_writel(q, RGB + 0x1c, 63);
    g_assert_cmphex(qtest_readl(q, RGB + 0x24), ==, 0x28);
    qtest_writel(q, RGB + 4, 0x300a0);
    g_assert_cmphex(qtest_readl(q, RGB + 0x24), ==, 0x3c);
    qtest_writel(q, RGB + 0x1c, 63);
    g_assert_cmphex(qtest_readl(q, RGB + 0x24), ==, 0x14);
    qtest_quit(q);
}
static void shadow_irq_and_byte_lanes(void)
{
    QTestState *q = start();
    for (unsigned i = 0; i < 24; i++) { qtest_writeb(q, DMA + 0x220 + i, 0); }
    qtest_writel(q, RAM, 0x12345678);
    qtest_writel(q, DMA, CTRL_COPY);
    qtest_writel(q, DMA + 0x220, 1);
    qtest_writel(q, DMA + 0x240, 30);
    descriptor(q, 0, RAM, RAM + 0x1000, 16);
    qtest_writeb(q, DMA + 0x220, 0);
    qtest_writeb(q, DMA + 0x240, 31);
    qtest_clock_step(q, 100000);
    g_assert_cmphex(qtest_readl(q, DMA + 0x440), ==, 0x80000010);
    g_assert_cmphex(qtest_readl(q, DMA + 0x380), ==, RAM);
    g_assert_cmphex(qtest_readl(q, DMA + 0x3a0), ==, RAM + 0x1000);
    g_assert_cmphex(qtest_readl(q, DMA + 0x2a0), ==, 1);
    g_assert_cmpuint(qtest_readb(q, 0xe0021000 + 49 * 4), ==, 1);
    qtest_writeb(q, DMA + 0x260, 1);
    g_assert_cmphex(qtest_readl(q, DMA + 0x280), ==, 4);
    g_assert_cmpuint(qtest_readb(q, 0xe0021000 + 49 * 4), ==, 0);
    qtest_quit(q);
}
static void narrow_and_address_modes(void)
{
    QTestState *q = start();
    uint8_t source[64], actual[32], expected[32], guard[32];
    for (unsigned i = 0; i < sizeof(source); i++) { source[i] = i + 1; }
    memset(guard, 0xa5, sizeof(guard));
    qtest_memwrite(q, RAM, source, sizeof(source));
    for (unsigned width = 0; width < 3; width++) {
        unsigned unit = 1u << width;
        for (unsigned mode = 0; mode < 3; mode++) {
            qtest_writel(q, DMA + 0x360, 1);
            uint32_t ctrl = 5 | width << 3 | width << 5;
            if (mode == 0) { ctrl |= 1u << 8; } /* Source descending. */
            if (mode == 1) { ctrl |= 2u << 8; } /* Source fixed. */
            if (mode == 2) { ctrl |= 2u << 10; } /* Destination fixed. */
            qtest_writel(q, DMA, ctrl);
            qtest_memwrite(q, RAM + 0x1000, guard, sizeof(guard));
            descriptor(q, 0, mode == 0 ? RAM + 32 : RAM, RAM + 0x1000, 16);
            qtest_clock_step(q, 100000);
            memcpy(expected, guard, sizeof(guard));
            for (unsigned i = 0; i < 16; i += unit) {
                unsigned src = mode == 0 ? 32 - i : mode == 1 ? 0 : i;
                unsigned dst = mode == 2 ? 0 : i;
                memcpy(expected + dst, source + src, unit);
            }
            qtest_memread(q, RAM + 0x1000, actual, sizeof(actual));
            g_assert_cmpmem(actual, sizeof(actual), expected, sizeof(expected));
        }
    }
    qtest_quit(q);
}
static void mixed_width_copy(void)
{
    QTestState *q = start();
    uint8_t source[67], actual[71], expected[71];
    for (unsigned i = 0; i < sizeof(source); i++) { source[i] = i + 17; }
    memset(expected, 0xa5, sizeof(expected));
    qtest_memwrite(q, RAM + 1, source, sizeof(source));
    qtest_memwrite(q, 0x38801000, expected, sizeof(expected));
    /* Literal programmed by luna_api_gpdma_start: byte source, word output. */
    qtest_writel(q, DMA + 20, 0x0019e0c5);
    qtest_writel(q, DMA + 0x220 + 20, 1);
    descriptor(q, 5, RAM + 1, 0x38801000, sizeof(source));
    qtest_memread(q, 0x38001000, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), expected, sizeof(expected));
    qtest_clock_step(q, 100000);
    memcpy(expected, source, sizeof(source));
    qtest_memread(q, 0x38001000, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), expected, sizeof(expected));
    g_assert_cmphex(qtest_readl(q, DMA + 0x280 + 20) & 1, ==, 1);
    g_assert_cmpuint(qtest_readb(q, 0xe0021000 + 54 * 4), ==, 1);
    qtest_quit(q);
}
static void strided_copy(void)
{
    QTestState *q = start();
    uint8_t source[256], actual[256], expected[256];
    for (unsigned i = 0; i < sizeof(source); i++) { source[i] = i ^ 0x69; }
    /* SG/DS are independent byte counters; both final groups are partial. */
    for (unsigned enables = 1; enables < 4; enables++) {
        unsigned sg = enables & 1 ? 5 : 256, ds = enables & 2 ? 12 : 256;
        memset(expected, 0xa5, sizeof(expected));
        qtest_memwrite(q, RAM, source, sizeof(source));
        qtest_memwrite(q, 0x38802000, expected, sizeof(expected));
        qtest_writel(q, DMA, 0x0019e0c5 |
                     (enables & 1 ? 1u << 22 : 0) |
                     (enables & 2 ? 1u << 21 : 0));
        qtest_writel(q, DMA + 0x20, 5);
        qtest_writel(q, DMA + 0x40, 3);
        qtest_writel(q, DMA + 0x60, 12);
        qtest_writel(q, DMA + 0x80, 8);
        descriptor(q, 0, RAM, 0x38802000, 67);
        qtest_clock_step(q, 100000);
        for (unsigned i = 0; i < 67; i++) {
            expected[i + i / ds * 8] = source[i + i / sg * 3];
        }
        qtest_memread(q, 0x38002000, actual, sizeof(actual));
        g_assert_cmpmem(actual, sizeof(actual), expected, sizeof(expected));
        qtest_memread(q, RAM, actual, sizeof(actual));
        g_assert_cmpmem(actual, sizeof(actual), source, sizeof(source));
    }
    qtest_quit(q);
}
static void strided_shadow_clock_cancel(void)
{
    QTestState *q = start();
    uint8_t source[256], actual[128], expected[128];
    for (unsigned i = 0; i < sizeof(source); i++) { source[i] = i + 1; }
    memset(expected, 0xa5, sizeof(expected));
    qtest_memwrite(q, RAM, source, sizeof(source));
    qtest_memwrite(q, RAM + 0x1000, expected, sizeof(expected));
    qtest_memwrite(q, RAM + 0x2000, expected, sizeof(expected));
    qtest_writel(q, DMA, 0x0079e0c5);
    qtest_writel(q, DMA + 0x220, 1);
    qtest_writel(q, DMA + 0x20, 5);
    qtest_writel(q, DMA + 0x40, 3);
    qtest_writel(q, DMA + 0x60, 12);
    qtest_writel(q, DMA + 0x80, 8);
    descriptor(q, 0, RAM, RAM + 0x1000, 67);
    /* Queue a differently packed/strided block while the first is active. */
    qtest_writel(q, DMA, (0x0079e0c5 & ~0x78u) | (2u << 3));
    qtest_writel(q, DMA + 0x20, 8);
    qtest_writel(q, DMA + 0x40, 4);
    qtest_writel(q, DMA + 0x60, 3);
    qtest_writel(q, DMA + 0x80, 2);
    descriptor(q, 0, RAM, RAM + 0x2000, 9);
    g_assert_cmphex(qtest_readl(q, DMA + 0x3c0), ==, 3);
    g_assert_cmphex(qtest_readl(q, DMA + 0x3e0), ==, 0x80000005);
    g_assert_cmphex(qtest_readl(q, DMA + 0x400), ==, 8);
    g_assert_cmphex(qtest_readl(q, DMA + 0x420), ==, 0x8000000c);
    qtest_clock_step(q, 1000); /* One 16-word grant, not 16 source bytes. */
    g_assert_cmpuint(qtest_readl(q, DMA + 0x320), ==, 3);
    g_assert_cmphex(qtest_readl(q, DMA + 0x280) & 1, ==, 0);
    for (unsigned i = 0; i < 64; i++) {
        expected[i + i / 12 * 8] = source[i + i / 5 * 3];
    }
    qtest_memread(q, RAM + 0x1000, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), expected, sizeof(expected));
    qtest_writel(q, 0x43100034, qtest_readl(q, 0x43100034) & ~16u);
    qtest_clock_step(q, 100000);
    g_assert_cmpuint(qtest_readl(q, DMA + 0x320), ==, 3);
    qtest_writel(q, 0x43100034, qtest_readl(q, 0x43100034) | 16);
    qtest_clock_step(q, 1000);
    g_assert_cmpuint(qtest_readl(q, DMA + 0x320), ==, 9);
    g_assert_cmphex(qtest_readl(q, DMA + 0x3c0), ==, 4);
    g_assert_cmphex(qtest_readl(q, DMA + 0x3e0), ==, 0x80000008);
    g_assert_cmphex(qtest_readl(q, DMA + 0x400), ==, 2);
    g_assert_cmphex(qtest_readl(q, DMA + 0x420), ==, 0x80000003);
    /* Live control writes must not alter the queued descriptor's packing. */
    qtest_writel(q, DMA, 0x0019e0c5);
    qtest_clock_step(q, 100000);
    for (unsigned i = 64; i < 67; i++) {
        expected[i + i / 12 * 8] = source[i + i / 5 * 3];
    }
    qtest_memread(q, RAM + 0x1000, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), expected, sizeof(expected));
    memset(expected, 0xa5, sizeof(expected));
    for (unsigned i = 0; i < 9; i++) {
        expected[i + i / 3 * 2] = source[i + i / 8 * 4];
    }
    qtest_memread(q, RAM + 0x2000, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), expected, sizeof(expected));
    g_assert_cmpuint(qtest_readb(q, 0xe0021000 + 49 * 4), ==, 1);
    qtest_writel(q, DMA + 0x260, 31);
    qtest_writel(q, DMA, 0x0079e0c5);
    qtest_writel(q, DMA + 0x60, 12);
    qtest_writel(q, DMA + 0x80, 8);
    descriptor(q, 0, RAM, RAM + 0x3000, 67);
    descriptor(q, 0, RAM, RAM + 0x4000, 67);
    qtest_clock_step(q, 1000);
    g_assert_cmpuint(qtest_readl(q, DMA + 0x320), ==, 3);
    qtest_writel(q, DMA + 0x360, 1);
    qtest_clock_step(q, 100000);
    g_assert_cmphex(qtest_readl(q, RAM + 0x3000), ==, 0x04030201);
    g_assert_cmphex(qtest_readl(q, RAM + 0x3000 + 104), ==, 0);
    g_assert_cmphex(qtest_readl(q, RAM + 0x4000), ==, 0);
    g_assert_cmpuint(qtest_readl(q, DMA + 0x200), ==, 0);
    g_assert_cmphex(qtest_readl(q, DMA + 0x280) & 17, ==, 16);
    qtest_quit(q);
}

static void mixed_width_matrix(void)
{
    QTestState *q = start();
    uint8_t source[64], actual[64], expected[64];
    for (unsigned i = 0; i < sizeof(source); i++) { source[i] = i + 1; }
    qtest_memwrite(q, RAM, source, sizeof(source));
    for (unsigned sw = 0; sw < 3; sw++) {
        for (unsigned dw = 0; dw < 3; dw++) {
            for (unsigned mode = 0; mode < 4; mode++) {
                unsigned su = 1u << sw, du = 1u << dw;
                unsigned sm = mode == 1 ? 1 : mode == 2 ? 2 : 0;
                unsigned dm = mode == 3 ? 2 : 0;
                memset(expected, 0xa5, sizeof(expected));
                qtest_memwrite(q, RAM + 0x1000, expected, sizeof(expected));
                qtest_writel(q, DMA, 5 | sw << 3 | dw << 5 | sm << 8 | dm << 10);
                descriptor(q, 0, RAM + 32, RAM + 0x1000, 19);
                qtest_clock_step(q, 100000);
                for (unsigned i = 0; i < 19; i++) {
                    unsigned src = sm == 1 ? 32 - i / su * su + i % su :
                                   sm == 2 ? 32 + i % su : 32 + i;
                    unsigned dst = dm == 2 ? i % du : i;
                    expected[dst] = source[src];
                }
                qtest_memread(q, RAM + 0x1000, actual, sizeof(actual));
                g_assert_cmpmem(actual, sizeof(actual), expected, sizeof(expected));
            }
        }
    }
    qtest_quit(q);
}

static void alias_overlap_order(void)
{
    QTestState *q = start();
    uint8_t source[64], expected[64], actual[64];
    for (unsigned i = 0; i < sizeof(source); i++) { source[i] = i + 1; }
    memcpy(expected, source, sizeof(source));
    qtest_memwrite(q, 0x38002000, source, sizeof(source));
    qtest_writel(q, DMA, 0x0019e0c5);
    descriptor(q, 0, 0x38802000, 0x38002004, 19);
    for (unsigned i = 0; i < 19; i += 4) {
        unsigned n = MIN(4, 19 - i);
        memcpy(expected + i + 4, expected + i, n);
    }
    qtest_clock_step(q, 100000);
    qtest_memread(q, 0x38002000, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), expected, sizeof(expected));
    qtest_quit(q);
}
static void psram_alias(void)
{
    QTestState *q = start();
    qtest_writel(q, 0x38000020, 0x11223344);
    qtest_writel(q, DMA, CTRL_COPY);
    descriptor(q, 0, 0x38800020, RAM, 4);
    qtest_clock_step(q, 100000);
    g_assert_cmphex(qtest_readl(q, RAM), ==, 0x11223344);
    descriptor(q, 0, RAM, 0x38800030, 4);
    qtest_clock_step(q, 100000);
    g_assert_cmphex(qtest_readl(q, 0x38000030), ==, 0x11223344);
    qtest_quit(q);
}
static void priority_burst_order(void)
{
    QTestState *q = start();
    qtest_writel(q, RAM, 0xfeed0123);
    qtest_writel(q, DMA, CTRL_COPY | (3u << 19));
    qtest_writel(q, DMA + 4, CTRL_COPY);
    descriptor(q, 0, RAM + 0x100, RAM + 0x200, 4);
    descriptor(q, 1, RAM, RAM + 0x100, 4);
    qtest_clock_step(q, 100000);
    g_assert_cmphex(qtest_readl(q, RAM + 0x100), ==, 0xfeed0123);
    g_assert_cmphex(qtest_readl(q, RAM + 0x200), ==, 0xfeed0123);
    qtest_quit(q);
}
static void rgb_format_capacity(void)
{
    QTestState *q = start();
    const unsigned words[] = {24, 32, 16};
    for (unsigned format = 0; format < 3; format++) {
        qtest_writel(q, RGB + 4, 0x30000 | (format << 4));
        qtest_writel(q, RGB + 0x1c, 63);
        for (unsigned i = 0; i < words[format] - 1; i++) {
            qtest_writel(q, RGB + 0xf4, 0);
        }
        g_assert_cmphex(qtest_readl(q, RGB + 0x24) & 0x28, ==, 0);
        qtest_writel(q, RGB + 0xf4, 0);
        g_assert_cmphex(qtest_readl(q, RGB + 0x24) & 0x28, ==, 0x28);
        qtest_writel(q, RGB + 8, 0x112233);
        qtest_writel(q, RGB + 16, 0x00030004);
        qtest_writel(q, RGB + 4, (format << 4) | 1);
        g_assert_cmphex(qtest_readl(q, RGB + 4), ==, format << 4);
        g_assert_cmphex(qtest_readl(q, RGB + 8), ==, 0x112233);
        g_assert_cmphex(qtest_readl(q, RGB + 16), ==, 0x00030004);
        g_assert_cmphex(qtest_readl(q, RGB + 0x24) & 0x28, ==, 0x28);
    }
    qtest_quit(q);
}
typedef struct NormalRejected {
    const char *name;
    uint32_t bank, value, control;
} NormalRejected;
static const NormalRejected converter_rejected[] = {
    { "format", 0x100, 0x33 },
    { "geometry-reserved", 0x120, 0x08010002 },
    { "stride-reserved", 0x160, 0x40080006 },
    { "length", 0xe0, 8 },
    { "decrement", 0, CTRL_COPY | (1u << 23) | (1u << 8) },
    { "alias", 0xc0, RAM },
    { "output-span", 0xc0, 0x20085ffc },
};
static void reject_converter(const void *opaque)
{
    const NormalRejected *r = opaque;
    if (g_test_subprocess()) {
        QTestState *q = start();
        const unsigned ch = 4;
        qtest_set_expected_status(q, 1);
        qtest_writel(q, DMA + ch * 4, CTRL_COPY | (1u << 23));
        qtest_writel(q, DMA + 0x100 + ch * 4, 3);
        qtest_writel(q, DMA + 0x120 + ch * 4, 0x00010002);
        qtest_writel(q, DMA + 0x140 + ch * 4, 0x00010002);
        qtest_writel(q, DMA + 0x160 + ch * 4, 0x00080006);
        qtest_writel(q, DMA + 0xa0 + ch * 4, RAM);
        qtest_writel(q, DMA + 0xc0 + ch * 4, RAM + 0x1000);
        qtest_writel(q, DMA + 0xe0 + ch * 4, 4);
        qtest_writel(q, DMA + r->bank + ch * 4, r->value);
        qtest_writel(q, DMA + 0x200 + ch * 4, 1);
        qtest_quit(q);
        return;
    }
    g_test_trap_subprocess(NULL, 5 * G_USEC_PER_SEC, 0);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr("*VenusA unsupported write*");
}
static const NormalRejected normal_rejected[] = {
    { "sg-zero", 0x20, 0 },
    { "ds-zero", 0x60, 0 },
    { "sg-count-24bit", 0x20, 0x1000000 },
    { "ds-count-24bit", 0x60, 0x1000000 },
    { "sg-gap-24bit", 0x40, 0x1000000 },
    { "ds-gap-24bit", 0x80, 0x1000000 },
    { "sg-count-alignment", 0x20, 6, 0x0079e0d5 },
    { "sg-gap-alignment", 0x40, 2, 0x0079e0d5 },
    { "ds-count-alignment", 0x60, 6 },
    { "ds-gap-alignment", 0x80, 2 },
    { "sg-decrement", 0, 0x0079e1c5 },
    { "ds-fixed", 0, 0x0079e8c5 },
    { "peripheral-sg", 0, CTRL_RGB | (1u << 22) },
    { "peripheral-unequal", 0, CTRL_RGB & ~(3u << 3) },
    { "image-sg", 0, CTRL_COPY | (1u << 23) | (1u << 22) },
    { "invalid-width", 0, 0x0019e0dd },
    { "src-span", 0xa0, 0x20085ffc },
    { "dst-span", 0xc0, 0x20085ffc },
    { "src-overflow", 0xa0, 0xfffffffc },
    { "dst-overflow", 0xc0, 0xfffffffc },
    { "stride-overflow", 0x40, 0xffffff },
    { "src-decrement-underflow", 0, 0x0019e1c5 },
    { "src-alignment", 0xa0, RAM + 1, 0x0079e0d5 },
    { "dst-alignment", 0xc0, RAM + 0x1001 },
    { "pending-sg-metadata", 0x20, 16, 0x0079e0c5 },
};
static void reject_normal(const void *opaque)
{
    const NormalRejected *r = opaque;
    if (g_test_subprocess()) {
        QTestState *q = start();
        qtest_set_expected_status(q, 1);
        qtest_writel(q, DMA, r->control ? r->control : 0x0079e0c5);
        qtest_writel(q, DMA + 0x20, 8);
        qtest_writel(q, DMA + 0x40, 4);
        qtest_writel(q, DMA + 0x60, 12);
        qtest_writel(q, DMA + 0x80, 8);
        qtest_writel(q, DMA + 0xa0, RAM);
        qtest_writel(q, DMA + 0xc0, RAM + 0x1000);
        qtest_writel(q, DMA + 0xe0, 67);
        if (!strcmp(r->name, "image-sg")) {
            qtest_writel(q, DMA + 0x100, 0x44);
            qtest_writel(q, DMA + 0x120, 0x00010004);
            qtest_writel(q, DMA + 0x140, 0x00010004);
            qtest_writel(q, DMA + 0x160, 0x00040004);
            qtest_writel(q, DMA + 0x4f0, 1);
            qtest_writel(q, DMA + 0xe0, 4);
        }
        if (!strcmp(r->name, "src-decrement-underflow")) {
            qtest_writel(q, DMA + 0xa0, 0);
        }
        if (!strcmp(r->name, "stride-overflow")) {
            qtest_writel(q, DMA + 0xe0, 0xffffff);
        }
        if (!strcmp(r->name, "pending-sg-metadata")) {
            qtest_writel(q, DMA + 0x200, 1);
            qtest_writel(q, DMA + 0x200, 1);
        }
        qtest_writel(q, DMA + r->bank, r->value);
        qtest_writel(q, DMA + 0x200, 1);
        /* Rejection must happen at queue time, before even the first grant. */
        qtest_quit(q); return;
    }
    g_test_trap_subprocess(NULL, 5 * G_USEC_PER_SEC, 0);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr("*VenusA unsupported write*");
}
typedef struct Rejected { const char *name; uint32_t address, value; } Rejected;
static const Rejected rejected[] = {
    { "channel1-image-mode", DMA + 0x104, 1 },
    { "outside-memory", DMA + 0x200, 1 },
    { "command-fifo", DMA + 0x4c4, 16 },
    { "read-only-status", DMA + 0x280, 1 },
    { "reserved-register", DMA + 0x18, 0 },
    { "serial-rgb", RGB, 0x101 },
    { "pending-shadow", DMA + 0xa0, RAM + 64 },
};
static void reject(const void *opaque)
{
    const Rejected *r = opaque;
    if (g_test_subprocess()) {
        QTestState *q = start();
        qtest_set_expected_status(q, 1);
        if (r->address == RGB) {
            qtest_writel(q, RGB + 4, 0x420);
            qtest_writel(q, RGB + 16, 0x00020004);
        }
        if (r->address == DMA + 0xa0) {
            qtest_writel(q, DMA, CTRL_COPY);
            descriptor(q, 0, RAM, RAM + 0x1000, 32);
            descriptor(q, 0, RAM, RAM + 0x1020, 32);
        }
        if (r->address == DMA + 0x200) {
            qtest_writel(q, DMA, CTRL_COPY);
            qtest_writel(q, DMA + 0xa0, 0x28000000);
            qtest_writel(q, DMA + 0xc0, RAM);
            qtest_writel(q, DMA + 0xe0, 4);
        }
        qtest_writel(q, r->address, r->value);
        if (r->address == DMA + 0x200) { qtest_clock_step(q, 100000); }
        qtest_quit(q); return;
    }
    g_test_trap_subprocess(NULL, 5 * G_USEC_PER_SEC, 0);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr("*VenusA unsupported write*");
}
static void jpeg_encode_waits(void)
{
    QTestState *q = start();
    qtest_writel(q, RAM, 0x76543210);
    qtest_writel(q, RAM + 4, 0xa5a5a5a5);
    /* Reset selects encode. No codec start or pixel input means no requests. */
    qtest_writel(q, DMA + 4, 1 | (2 << 3) | (2 << 5) | (2 << 8) |
                             (1 << 17) | (4u << 28));
    descriptor(q, 1, 0x42003000, RAM + 4, 64);
    qtest_writel(q, DMA + 8, 1 | (1 << 1) | (2 << 3) | (2 << 5) |
                             (2 << 10) | (1 << 17) | (5u << 28));
    descriptor(q, 2, RAM, 0x42002800, 64);
    qtest_clock_step(q, 1000000);
    g_assert_cmphex(qtest_readl(q, RAM), ==, 0x76543210);
    g_assert_cmphex(qtest_readl(q, RAM + 4), ==, 0xa5a5a5a5);
    for (unsigned ch = 1; ch <= 2; ch++) {
        g_assert_cmpuint(qtest_readl(q, DMA + 0x320 + ch * 4), ==, 64);
        g_assert_cmphex(qtest_readl(q, DMA + 0x280 + ch * 4) & 1, ==, 0);
    }
    qtest_writel(q, DMA + 0x360, 6);
    qtest_quit(q);
}

static void jpeg_reject_route(gconstpointer opaque)
{
    unsigned which = GPOINTER_TO_UINT(opaque);
    if (g_test_subprocess()) {
        QTestState *q = start();
        qtest_set_expected_status(q, 1);
        if (which == 0 || which == 1) {
            /* DMA2D and common-DMA mux fields must both select JPEG. */
            qtest_writel(q, 0x431000a0, 1u << (which ? 21 : 5));
        }
        bool write = which != 2;
        qtest_writel(q, DMA + 8, 1 | (write ? 1 << 1 : 0) |
                                 (2 << 3) | (2 << 5) |
                                 (write ? 2 << 10 : 2 << 8) | (5u << 28));
        descriptor(q, 2, write ? RAM : 0x42002800,
                   write ? 0x42002800 : RAM, 4);
        qtest_clock_step(q, 1000000);
        qtest_quit(q);
        return;
    }
    g_test_trap_subprocess(NULL, 5 * G_USEC_PER_SEC, 0);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr("*VenusA unsupported *address=0x42002800*");
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    int fd = g_file_open_tmp("venusa-image-flash-XXXXXX", &flash, NULL);
    g_assert_cmpint(fd, >=, 0); g_assert_cmpint(ftruncate(fd, 16 * 1024 * 1024), ==, 0); close(fd);
    fd = g_file_open_tmp("venusa-rgb-capture-XXXXXX", &capture_path, NULL);
    g_assert_cmpint(fd, >=, 0); close(fd);
    g_setenv("LISEM_QEMU_RGB_CAPTURE", capture_path, true);
    qtest_add_func("venusa/dma2d/normal-shadow", normal_shadow);
    qtest_add_func("venusa/dma2d/m2m-yuyv-to-bgr", dma2d_m2m_yuyv_to_bgr);
    for (unsigned i = 0; i < G_N_ELEMENTS(converter_rejected); i++) {
        g_autofree char *name = g_strdup_printf("venusa/dma2d/converter-reject/%s",
                                                converter_rejected[i].name);
        qtest_add_data_func(name, &converter_rejected[i], reject_converter);
    }
    qtest_add_func("venusa/dma2d/jpeg-waits", jpeg_waits_for_pixels);
    qtest_add_func("venusa/dma2d/jpeg-encode-waits", jpeg_encode_waits);
    for (unsigned i = 0; i < 3; i++) {
        g_autofree char *name = g_strdup_printf("venusa/image/jpeg-reject-route-%u", i);
        qtest_add_data_func(name, GUINT_TO_POINTER(i), jpeg_reject_route);
    }
    qtest_add_func("venusa/dma2d/clock-cancel-reset", cancel_gate);
    qtest_add_func("venusa/dma2d/shadow-irq-byte-lanes", shadow_irq_and_byte_lanes);
    qtest_add_func("venusa/dma2d/narrow-address-modes", narrow_and_address_modes);
    qtest_add_func("venusa/dma2d/priority-burst-order", priority_burst_order);
    qtest_add_func("venusa/dma2d/psram-alias", psram_alias);
    qtest_add_func("venusa/dma2d/mixed-width-copy", mixed_width_copy);
    qtest_add_func("venusa/dma2d/strided-copy", strided_copy);
    qtest_add_func("venusa/dma2d/strided-shadow-clock-cancel", strided_shadow_clock_cancel);
    qtest_add_func("venusa/dma2d/mixed-width-matrix", mixed_width_matrix);
    qtest_add_func("venusa/dma2d/alias-overlap-order", alias_overlap_order);
    qtest_add_func("venusa/rgb/format-capacity-soft-reset", rgb_format_capacity);
    qtest_add_func("venusa/rgb/actual-pixels", rgb_pixels);
    qtest_add_func("venusa/rgb/starvation-no-frame", starvation_no_frame);
    qtest_add_func("venusa/rgb/clock-phase", rgb_clock_boundaries);
    qtest_add_func("venusa/rgb/pixel-deadline", rgb_pixel_deadline);
    qtest_add_func("venusa/rgb/frame-period", rgb_frame_period);
    qtest_add_func("venusa/rgb/fifo-latches", rgb_fifo_latches);
    qtest_add_func("venusa/rgb/packed-formats", rgb_packed_formats);
    for (unsigned i = 0; i < G_N_ELEMENTS(rejected); i++) {
        g_autofree char *name = g_strdup_printf("venusa/image/reject/%s", rejected[i].name);
        qtest_add_data_func(name, &rejected[i], reject);
    }
    for (unsigned i = 0; i < G_N_ELEMENTS(normal_rejected); i++) {
        g_autofree char *name = g_strdup_printf("venusa/dma2d/reject/%s", normal_rejected[i].name);
        qtest_add_data_func(name, &normal_rejected[i], reject_normal);
    }
    int result = g_test_run();
    unlink(flash); unlink(capture_path); g_free(flash); g_free(capture_path);
    return result;
}
