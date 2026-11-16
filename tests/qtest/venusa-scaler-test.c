/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "libqtest.h"

#define DMA 0x44900000
#define RAM 0x20010000
#define IMAGE_CTRL (1u | (2u << 1) | (2u << 3) | (2u << 5) | \
                   (2u << 13) | (2u << 15) | (1u << 23))

static char *flash;
static QTestState *start(void)
{
    g_autofree char *path = g_shell_quote(flash);
    QTestState *q = qtest_initf("-machine venusa-rd-evb -drive if=pflash,format=raw,file=%s", path);
    qtest_writel(q, 0x43100034, qtest_readl(q, 0x43100034) | 16);
    qtest_writel(q, 0x43100030, (1u << 27) | (1u << 26) | (1u << 22));
    qtest_writel(q, 0x43100014, (1u << 16) | (1u << 25));
    return q;
}

static void first_tile_box_average(void)
{
    QTestState *q = start();
    const uint8_t input[] = {
        16, 128, 16, 128, 32, 128, 32, 128,
        16, 128, 16, 128, 32, 128, 32, 128,
        64, 128, 64, 128, 80, 128, 80, 128,
        64, 128, 64, 128, 80, 128, 80, 128,
    };
    uint8_t output[12] = { 0 };

    qtest_memwrite(q, RAM, input, sizeof(input));
    qtest_memwrite(q, RAM + 0x100, output, sizeof(output));
    qtest_writel(q, DMA + 0x100, 3); /* YUYV422 -> BGR888. */
    qtest_writel(q, DMA + 0x120, 4 | (4u << 16));
    qtest_writel(q, DMA + 0x140, 2 | (2u << 16));
    qtest_writel(q, DMA + 0x160, 8 | (6u << 16));
    qtest_writel(q, DMA + 0x4f0, 0x6239); /* 2x2 merge, final tile/band. */
    qtest_writel(q, DMA + 0x4f4, 4096 | (4096u << 16));
    qtest_writel(q, DMA + 0x4f8, 0);
    qtest_writel(q, DMA, IMAGE_CTRL);
    qtest_writel(q, DMA + 0x220, 1);
    qtest_writel(q, DMA + 0x240, 0x1e);
    qtest_writel(q, DMA + 0xa0, RAM);
    qtest_writel(q, DMA + 0xc0, RAM + 0x100);
    qtest_writel(q, DMA + 0xe0, sizeof(output));
    qtest_writel(q, DMA + 0x200, 1);

    qtest_memread(q, RAM + 0x100, output, sizeof(output));
    g_assert_cmpuint(output[0], ==, 0);
    qtest_clock_step(q, 100000);
    qtest_memread(q, RAM + 0x100, output, sizeof(output));
    const uint8_t expected[] = {
        16, 16, 16, 32, 32, 32,
        64, 64, 64, 80, 80, 80,
    };
    g_assert_cmpmem(output, sizeof(output), expected, sizeof(expected));
    qtest_quit(q);
}

static void configure(QTestState *q, unsigned iw, unsigned ih, unsigned ow,
                      unsigned oh, unsigned is, unsigned os, uint32_t fmt,
                      uint32_t scaler, uint32_t phase, uint32_t start_phase)
{
    qtest_writel(q, DMA + 0x100, fmt);
    qtest_writel(q, DMA + 0x120, iw | ih << 16);
    qtest_writel(q, DMA + 0x140, ow | oh << 16);
    qtest_writel(q, DMA + 0x160, is | os << 16);
    qtest_writel(q, DMA + 0x4f0, scaler);
    qtest_writel(q, DMA + 0x4f4, phase);
    qtest_writel(q, DMA + 0x4f8, start_phase);
    qtest_writel(q, DMA, IMAGE_CTRL);
    qtest_writel(q, DMA + 0x220, 1);
    qtest_writel(q, DMA + 0x240, 30);
}

static void queue(QTestState *q, uint32_t src, uint32_t dst, unsigned bytes)
{
    qtest_writel(q, DMA + 0xa0, src);
    qtest_writel(q, DMA + 0xc0, dst);
    qtest_writel(q, DMA + 0xe0, bytes);
    qtest_writel(q, DMA + 0x200, 1);
}

static void overlap_skips_one_merged_sample(void)
{
    uint8_t input[32], actual[8];
    const uint8_t y[] = {0, 2, 10, 12, 20, 22, 30, 32};
    const uint8_t expected[][8] = {
        {11, 11, 11, 26, 26, 26, 0xa5, 0xa5},
        {16, 16, 16, 31, 31, 31, 0xa5, 0xa5},
    };
    for (unsigned row = 0; row < 2; row++) {
        for (unsigned x = 0; x < 8; x++) {
            input[row * 16 + x * 2] = y[x];
            input[row * 16 + x * 2 + 1] = 128;
        }
    }
    for (unsigned i = 0; i < G_N_ELEMENTS(expected); i++) {
        QTestState *q = start();
        unsigned phase = i * 2048;
        memset(actual, 0xa5, sizeof(actual));
        qtest_memwrite(q, RAM, input, sizeof(input));
        qtest_memwrite(q, RAM + 0x100, actual, sizeof(actual));
        configure(q, 8, 2, 2, 1, 16, 8, 3, 0xe239, 0x10001800, phase);
        queue(q, RAM, RAM + 0x100, 6);
        qtest_clock_step(q, 100000);
        qtest_memread(q, RAM + 0x100, actual, sizeof(actual));
        g_assert_cmpmem(actual, sizeof(actual), expected[i], sizeof(expected[i]));
        g_assert_cmphex(qtest_readl(q, DMA + 0x4fc), ==, 0x03000000 | phase);
        qtest_quit(q);
    }
}
static void unaligned_bgr_roi(void)
{
    QTestState *q = start();
    const uint8_t input[] = {5, 11, 29, 37, 41, 59, 67, 83, 97, 109, 113, 127};
    uint8_t actual[16], expected[16];
    memset(expected, 0xa5, sizeof(expected));
    qtest_memwrite(q, RAM + 1, input, sizeof(input));
    qtest_memwrite(q, RAM + 0x100, expected, sizeof(expected));
    configure(q, 2, 2, 2, 2, 6, 6, 0, 0x6009, 0x10001000, 0);
    queue(q, RAM + 1, RAM + 0x101, sizeof(input));
    qtest_memread(q, RAM + 0x100, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), expected, sizeof(expected));
    qtest_clock_step(q, 100000);
    memcpy(expected + 1, input, sizeof(input));
    qtest_memread(q, RAM + 0x100, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), expected, sizeof(expected));
    qtest_quit(q);
}

static void fractional_y8(void)
{
    QTestState *q = start();
    uint8_t input[] = {0, 40, 80, 120}, actual[9];
    const uint8_t expected[] = {0,20,40,40,60,80,80,100,120};
    qtest_memwrite(q, RAM, input, sizeof(input));
    configure(q, 2, 2, 3, 3, 2, 3, 0x44, 0x600f, 0x08000800, 0);
    queue(q, RAM, RAM + 0x100, sizeof(actual));
    qtest_clock_step(q, 100000);
    qtest_memread(q, RAM + 0x100, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), expected, sizeof(expected));
    /* Q12 half-pixel origin and two-pixel downsampling step. */
    const uint8_t line[] = {0,20,40,60,80};
    qtest_memwrite(q, RAM, line, sizeof(line));
    configure(q, 5, 1, 2, 1, 8, 4, 0x44, 0x6009, 0x10002000, 2048);
    queue(q, RAM, RAM + 0x200, 2);
    g_assert_cmphex(qtest_readl(q, DMA + 0x4fc), ==, 0x03000800);
    qtest_clock_step(q, 100000);
    g_assert_cmpuint(qtest_readb(q, RAM + 0x200), ==, 10);
    g_assert_cmpuint(qtest_readb(q, RAM + 0x201), ==, 50);
    qtest_quit(q);
}

static void box_average(void)
{
    QTestState *q = start();
    uint8_t input[16], actual[4];
    const uint8_t expected[] = {20,36,84,100};
    for (unsigned i=0;i<16;i++) { input[i]=i*8; }
    qtest_memwrite(q,RAM,input,sizeof(input));
    configure(q,4,4,2,2,4,2,0x44,0x6239,0x10001000,0);
    queue(q,RAM,RAM+0x100,4);
    qtest_clock_step(q,100000);
    qtest_memread(q,RAM+0x100,actual,4);
    g_assert_cmpmem(actual,4,expected,4);
    qtest_quit(q);
}

static void channel_order(void)
{
    QTestState *q = start();
    for (unsigned input_rgb=0;input_rgb<2;input_rgb++) {
        for (unsigned output_rgb=0;output_rgb<2;output_rgb++) {
            uint8_t input[] = {input_rgb?12:200,64,input_rgb?200:12};
            uint8_t expected[] = {output_rgb?12:200,64,output_rgb?200:12}, actual[3];
            qtest_memwrite(q,RAM,input,3);
            qtest_memset(q,RAM+0x100,0xa5,8);
            configure(q,1,1,1,1,3,3,(input_rgb<<12)|(output_rgb<<13),0x6009,0x10001000,0);
            queue(q,RAM,RAM+0x100,3);
            qtest_clock_step(q,100000);
            qtest_memread(q,RAM+0x100,actual,3);
            g_assert_cmpmem(actual,3,expected,3);
            g_assert_cmphex(qtest_readb(q,RAM+0x103),==,0xa5);
        }
    }
    const uint8_t packed[][4] = {{100,160,120,80},{160,100,80,120},
                                {100,80,120,160},{80,100,160,120}};
    const uint8_t expected[] = {155,123,32,175,143,52};
    for (unsigned order=0;order<4;order++) {
        uint8_t actual[6];
        qtest_memwrite(q,RAM,packed[order],4);
        configure(q,2,1,2,1,4,6,3|(order<<8),0x6009,0x10001000,0);
        queue(q,RAM,RAM+0x100,6);
        qtest_clock_step(q,100000);
        qtest_memread(q,RAM+0x100,actual,6);
        g_assert_cmpmem(actual,6,expected,6);
    }
    qtest_quit(q);
}

static void tiled_stride(void)
{
    QTestState *q = start();
    uint8_t input[128], actual[48];
    for (unsigned y=0;y<4;y++) {
        for (unsigned x=0;x<16;x+=2) {
            unsigned i=y*32+x*2;
            input[i]=(x+1)*10; input[i+1]=128;
            input[i+2]=(x+2)*10; input[i+3]=128;
        }
    }
    qtest_memwrite(q,RAM,input,sizeof(input));
    qtest_memset(q,RAM+0x100,0xa5,52);
    for (unsigned tile=0;tile<2;tile++) {
        configure(q,8,4,4,2,32,24,3,tile?0x6239:0x4239,0x10001000,0);
        queue(q,RAM+tile*16,RAM+0x100+tile*12,24);
        qtest_clock_step(q,100000);
        qtest_memread(q,RAM+0x100,actual,48);
        for (unsigned y=0;y<2;y++) for (unsigned x=0;x<8;x++) {
            unsigned value=(!tile && x>=4)?0xa5:15+20*x;
            for (unsigned c=0;c<3;c++) {
                g_assert_cmpuint(actual[y*24+x*3+c],==,value);
            }
        }
    }
    g_assert_cmphex(qtest_readl(q,RAM+0x130),==,0xa5a5a5a5);
    qtest_quit(q);
}

static void gate_cancel_reset(gconstpointer opaque)
{
    unsigned action=GPOINTER_TO_UINT(opaque);
    QTestState *q=start();
    uint8_t input[128], actual[128], paused[128];
    memset(input,42,sizeof(input));
    qtest_memwrite(q,RAM,input,sizeof(input));
    configure(q,64,2,64,2,64,64,0x44,0x6009,0x10001000,0);
    queue(q,RAM,RAM+0x100,sizeof(input));
    qtest_clock_step(q,84); /* First DMA grant at a 24MHz clock. */
    qtest_writel(q,0x43100034,qtest_readl(q,0x43100034)&~16u);
    qtest_memread(q,RAM+0x100,paused,sizeof(paused));
    g_assert_cmpuint(paused[0],==,42);
    g_assert_cmpuint(paused[31],==,42);
    g_assert_cmpuint(paused[32],==,0);
    g_assert_cmpuint(qtest_readl(q,DMA+0x320),==,96);
    g_assert_cmpuint(paused[127],==,0);
    qtest_clock_step(q,1000000);
    qtest_memread(q,RAM+0x100,actual,sizeof(actual));
    g_assert_cmpmem(actual,sizeof(actual),paused,sizeof(paused));
    if(action==1) qtest_writel(q,DMA+0x360,1);
    if(action==2) qtest_writel(q,DMA+0x364,1);
    qtest_writel(q,0x43100034,qtest_readl(q,0x43100034)|16u);
    qtest_clock_step(q,1000000);
    qtest_memread(q,RAM+0x100,actual,sizeof(actual));
    if(action) {
        g_assert_cmpmem(actual,sizeof(actual),paused,sizeof(paused));
        g_assert_cmphex(qtest_readl(q,DMA+0x280)&1,==,0);
    } else {
        g_assert_cmpmem(actual,sizeof(actual),input,sizeof(input));
        g_assert_cmphex(qtest_readl(q,DMA+0x280)&1,==,1);
        qtest_writel(q,DMA+0x260,1);
        g_assert_cmphex(qtest_readb(q,0xe0021000+49*4)&1,==,0);
    }
    qtest_quit(q);
}

static void shadow_snapshot(void)
{
    QTestState *q=start();
    uint8_t input[128], actual[128];
    memset(input,17,sizeof(input));qtest_memwrite(q,RAM,input,sizeof(input));
    configure(q,64,2,64,2,64,64,0x44,0x6009,0x10001000,0);
    queue(q,RAM,RAM+0x100,128);
    const uint8_t second[]={0,40,80,120}, expected[]={0,20,40,40,60,80,80,100,120};
    qtest_memwrite(q,RAM+0x300,second,4);
    configure(q,2,2,3,3,2,3,0x44,0x600f,0x08000800,0);
    queue(q,RAM+0x300,RAM+0x400,9);
    /* Live register writes must not alter either captured descriptor. */
    qtest_writel(q,DMA+0x4f4,0);
    qtest_clock_step(q,1000000);
    qtest_memread(q,RAM+0x100,actual,128);
    g_assert_cmpmem(actual,128,input,128);
    qtest_memread(q,RAM+0x400,actual,9);
    g_assert_cmpmem(actual,9,expected,9);
    qtest_quit(q);
}

static void partial_rgb_grant(void)
{
    QTestState *q=start();
    uint8_t input[36],actual[36];
    for(unsigned i=0;i<36;i++)input[i]=i;
    qtest_memwrite(q,RAM,input,sizeof(input));
    qtest_memset(q,RAM+0x100,0xa5,sizeof(actual));
    configure(q,12,1,12,1,36,36,0x3000,0x6009,0x10001000,0);
    queue(q,RAM,RAM+0x100,36);
    qtest_clock_step(q,84);
    qtest_memread(q,RAM+0x100,actual,sizeof(actual));
    g_assert_cmpmem(actual,32,input,32);
    g_assert_cmpuint(actual[32],==,0xa5);
    g_assert_cmpuint(qtest_readl(q,DMA+0x320),==,4);
    g_assert_cmpuint(qtest_readl(q,DMA+0x280)&1,==,0);
    /* The partially emitted pixel retains the component already sampled. */
    qtest_writeb(q,RAM+32,199);
    qtest_clock_step(q,100000);
    qtest_memread(q,RAM+0x100,actual,sizeof(actual));
    g_assert_cmpmem(actual,sizeof(actual),input,sizeof(input));
    qtest_quit(q);
}

static void merge_factors(void)
{
    const unsigned codes[]={1,2,4,8}, factors[]={2,4,8,16};
    const uint8_t first[]={9,26,60,128}, last[]={247,230,196,128};
    for(unsigned k=0;k<4;k++) {
        QTestState *q=start();
        uint8_t input[256],actual[64];
        unsigned w=16/factors[k];
        for(unsigned i=0;i<256;i++)input[i]=i;
        qtest_memwrite(q,RAM,input,sizeof(input));
        configure(q,16,16,w,w,16,w,0x44,0x6011|(codes[k]<<5)|(codes[k]<<9),0,0);
        queue(q,RAM,RAM+0x400,w*w);
        qtest_clock_step(q,100000);
        qtest_memread(q,RAM+0x400,actual,w*w);
        g_assert_cmpuint(actual[0],==,first[k]);
        g_assert_cmpuint(actual[w*w-1],==,last[k]);
        qtest_quit(q);
    }
}

static void invalid(gconstpointer opaque)
{
    unsigned which=GPOINTER_TO_UINT(opaque);
    if(g_test_subprocess()) {
        QTestState *q=start();
        configure(q,4,4,2,2,4,2,0x44,0x6239,0x10001000,0);
        uint32_t src=RAM,dst=RAM+0x100;unsigned length=4;
        switch(which) {
        case 0:qtest_writel(q,DMA+0x4f4,0x10000000);break;
        case 1:qtest_writel(q,DMA+0x160,3|(2u<<16));break;
        case 2:dst=RAM;break;
        case 3:src=0xfffffffc;break;
        case 4:dst=0xfffffffc;break;
        case 5:qtest_writel(q,DMA+0x100,0x41);break;
        case 6:qtest_writel(q,DMA+0x4f0,0xe239);break;
        case 7:qtest_writel(q,DMA+0x4f0,0x6238);break;
        case 8:qtest_writel(q,DMA+0x120,3|(4u<<16));break;
        case 9:qtest_writel(q,DMA+0x4f0,0x6279);break;
        case 10:length=3;break;
        case 11:qtest_writel(q,DMA+0x4f4,0x10000800);break;
        case 12:
            configure(q,16,1,16,1,16,16,0x44,0x6009,0x10001000,0);
            src=0x38000000;dst=0x38800004;length=16;break;
        }
        queue(q,src,dst,length);
        qtest_clock_step(q,1000000);
        qtest_quit(q);
        g_error("Unsupported scaler configuration was accepted");
    }
    g_test_trap_subprocess(NULL,5*G_USEC_PER_SEC,0);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr("*VenusA unsupported write*");
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    int fd = g_file_open_tmp("venusa-scaler-flash-XXXXXX", &flash, NULL);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, 16 * 1024 * 1024), ==, 0);
    close(fd);
    qtest_add_func("venusa/scaler/first-tile-box-average", first_tile_box_average);
    qtest_add_func("venusa/scaler/fractional-y8", fractional_y8);
    qtest_add_func("venusa/scaler/box-average", box_average);
    qtest_add_func("venusa/scaler/channel-order", channel_order);
    qtest_add_func("venusa/scaler/tiled-stride", tiled_stride);
    qtest_add_func("venusa/scaler/overlap-merged-sample", overlap_skips_one_merged_sample);
    qtest_add_func("venusa/scaler/unaligned-bgr-roi", unaligned_bgr_roi);
    qtest_add_func("venusa/scaler/shadow-snapshot", shadow_snapshot);
    qtest_add_func("venusa/scaler/partial-rgb-grant", partial_rgb_grant);
    qtest_add_func("venusa/scaler/merge-factors", merge_factors);
    qtest_add_data_func("venusa/scaler/clock-gate",GUINT_TO_POINTER(0),gate_cancel_reset);
    qtest_add_data_func("venusa/scaler/cancel",GUINT_TO_POINTER(1),gate_cancel_reset);
    qtest_add_data_func("venusa/scaler/reset",GUINT_TO_POINTER(2),gate_cancel_reset);
    for(unsigned i=0;i<13;i++) {
        g_autofree char *name=g_strdup_printf("venusa/scaler/invalid/%u",i);
        qtest_add_data_func(name,GUINT_TO_POINTER(i),invalid);
    }
    int result = g_test_run();
    unlink(flash);
    g_free(flash);
    return result;
}
