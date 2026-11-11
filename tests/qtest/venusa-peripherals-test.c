/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"

#define MTIME 0xe0030000
#define ECLIC_PENDING(irq) (0xe0021000 + (irq) * 4)
#define PSRAM_CTRL 0x44600000
#define SD 0x44a00000
#define USB 0x41000000
#define USB_CLOCK 0x43100034
#define USB_CTRL1 0x43100040

static char *flash_path, *sd_path, *otp_path;

static QTestState *start(bool sd)
{
    g_autofree char *flash = g_shell_quote(flash_path);
    g_autofree char *card = g_shell_quote(sd_path);
    g_autofree char *drive = sd ? g_strdup_printf(
        "-drive if=none,id=sdcard,file=%s,format=raw", card) : g_strdup("");
    return qtest_initf("-machine venusa-rd-evb "
                       "-drive if=pflash,format=raw,file=%s %s", flash, drive);
}

/* The firmware's shared TX/RX FIFO layout consumes 3840 of 4096 bytes. */
static void usb_empty_port(void)
{
    static const uint8_t sizes[] = { 3, 7, 7, 7, 6, 3, 3, 3 };
    QTestState *q = start(false);
    unsigned offset = 0;

    qtest_writel(q, USB_CLOCK, 4);
    uint32_t config = qtest_readl(q, USB_CTRL1);
    qtest_writel(q, USB_CTRL1, config & ~(1u << 18));
    qtest_writel(q, USB_CTRL1, (config & ~(1u << 18)) | (1u << 14));
    g_assert_cmphex(qtest_readl(q, USB_CTRL1), ==, 1u << 14);
    for (unsigned ep = 0; ep < 8; ep++) {
        qtest_writeb(q, USB + 0x0e, ep);
        qtest_writeb(q, USB + 0x62, sizes[ep]);
        qtest_writew(q, USB + 0x64, offset / 8);
        qtest_writeb(q, USB + 0x63, sizes[ep]);
        qtest_writew(q, USB + 0x66, offset / 8);
        offset += 8u << sizes[ep];
    }
    g_assert_cmpuint(offset, ==, 3840);
    qtest_writeb(q, USB + 0x0b, 0xff);
    qtest_writew(q, USB + 0x06, 1);
    qtest_writew(q, USB + 0x08, 0);
    qtest_writeb(q, USB + 0x01, qtest_readb(q, USB + 0x01) | 0x20);
    qtest_writeb(q, USB + 0x60, qtest_readb(q, USB + 0x60) | 1);
    g_assert_cmphex(qtest_readb(q, USB + 0x60), ==, 5);
    g_assert_cmphex(qtest_readb(q, USB + 0x01), ==, 0x20);
    g_assert_cmphex(qtest_readb(q, USB + 0x0b), ==, 0xff);
    g_assert_cmphex(qtest_readw(q, USB + 0x06), ==, 1);
    g_assert_cmphex(qtest_readw(q, USB + 0x08), ==, 0);
    offset = 0;
    for (unsigned ep = 0; ep < 8; ep++) {
        qtest_writeb(q, USB + 0x0e, ep);
        g_assert_cmpuint(qtest_readb(q, USB + 0x0e), ==, ep);
        g_assert_cmpuint(qtest_readb(q, USB + 0x62), ==, sizes[ep]);
        g_assert_cmpuint(qtest_readb(q, USB + 0x63), ==, sizes[ep]);
        g_assert_cmpuint(qtest_readw(q, USB + 0x64), ==, offset / 8);
        g_assert_cmpuint(qtest_readw(q, USB + 0x66), ==, offset / 8);
        offset += 8u << sizes[ep];
    }
    qtest_clock_step(q, 1000000000);
    g_assert_cmphex(qtest_readw(q, USB + 0x02), ==, 0);
    g_assert_cmphex(qtest_readw(q, USB + 0x04), ==, 0);
    g_assert_cmphex(qtest_readb(q, USB + 0x0a), ==, 0);
    g_assert_cmphex(qtest_readb(q, ECLIC_PENDING(56)) & 1, ==, 0);
    /* Gating is not reset: configuration survives when the clock returns. */
    qtest_writel(q, USB_CLOCK, 0);
    qtest_clock_step(q, 1000000000);
    qtest_writel(q, USB_CLOCK, 4);
    g_assert_cmphex(qtest_readb(q, USB + 0x60), ==, 5);
    g_assert_cmpuint(qtest_readw(q, USB + 0x64), ==, 472);
    qtest_writel(q, 0x43100014, 1u << 8);
    g_assert_cmphex(qtest_readl(q, USB_CTRL1), ==, 1u << 14);
    g_assert_cmphex(qtest_readb(q, USB + 0x60), ==, 0);
    g_assert_cmphex(qtest_readb(q, USB + 0x01), ==, 0);
    g_assert_cmphex(qtest_readb(q, USB + 0x0b), ==, 0);
    for (unsigned ep = 0; ep < 8; ep++) {
        qtest_writeb(q, USB + 0x0e, ep);
        g_assert_cmphex(qtest_readb(q, USB + 0x62), ==, 0);
        g_assert_cmphex(qtest_readb(q, USB + 0x63), ==, 0);
        g_assert_cmphex(qtest_readw(q, USB + 0x64), ==, 0);
        g_assert_cmphex(qtest_readw(q, USB + 0x66), ==, 0);
    }
    qtest_system_reset(q);
    g_assert_cmphex(qtest_readl(q, USB_CLOCK), ==, 0);
    g_assert_cmphex(qtest_readl(q, USB_CTRL1), ==, 0);
    qtest_writel(q, USB_CLOCK, 4);
    g_assert_cmphex(qtest_readb(q, USB + 0x0e), ==, 0);
    g_assert_cmphex(qtest_readb(q, ECLIC_PENDING(56)) & 1, ==, 0);
    qtest_quit(q);
}

static void usb_configuration(void)
{
    QTestState *q = start(false);

    qtest_writel(q, USB_CLOCK, 4);
    qtest_writel(q, USB_CTRL1, (1u << 18) | (1u << 14));
    g_assert_cmphex(qtest_readb(q, USB + 0x60), ==, 0);
    qtest_writel(q, USB_CTRL1, 1u << 14);
    qtest_writeb(q, USB + 0x60, 1);
    qtest_writeb(q, USB + 0x60, qtest_readb(q, USB + 0x60) & ~1);
    g_assert_cmphex(qtest_readb(q, USB + 0x60), ==, 0);
    /* usb_reg.h: SUSPEND_MODE[1] and HSMODE[4] are status, not controls. */
    qtest_writeb(q, USB + 0x01, 0x33);
    g_assert_cmphex(qtest_readb(q, USB + 0x01), ==, 0x21);
    qtest_writeb(q, USB + 0x01, 0x12);
    g_assert_cmphex(qtest_readb(q, USB + 0x01), ==, 0);
    qtest_writew(q, USB + 0x06, 0xff);
    qtest_writew(q, USB + 0x08, 0xfe);
    g_assert_cmphex(qtest_readw(q, USB + 0x06), ==, 0xff);
    g_assert_cmphex(qtest_readw(q, USB + 0x08), ==, 0xfe);
    /* Distinct directions in one bank, and exact 4 KiB allocation limits. */
    qtest_writeb(q, USB + 0x62, 8);
    qtest_writew(q, USB + 0x64, 256);
    qtest_writeb(q, USB + 0x63, 0x18);
    g_assert_cmphex(qtest_readb(q, USB + 0x62), ==, 8);
    g_assert_cmphex(qtest_readb(q, USB + 0x63), ==, 0x18);
    g_assert_cmphex(qtest_readw(q, USB + 0x64), ==, 256);
    g_assert_cmphex(qtest_readw(q, USB + 0x66), ==, 0);
    qtest_clock_step(q, 1000000000);
    g_assert_cmphex(qtest_readw(q, USB + 0x02), ==, 0);
    g_assert_cmphex(qtest_readw(q, USB + 0x04), ==, 0);
    g_assert_cmphex(qtest_readb(q, USB + 0x0a), ==, 0);
    g_assert_cmphex(qtest_readb(q, ECLIC_PENDING(56)) & 1, ==, 0);
    qtest_quit(q);
}

static void usb_spi_reset_isolation(void)
{
    const uint32_t spi[] = { 0x43e00000, 0x43f00000 };
    QTestState *q = start(false);

    for (unsigned i = 0; i < 2; i++) {
        qtest_writel(q, 0x4310001c + i * 4, 0x13800000);
        qtest_writel(q, 0x4310001c + i * 4, 0x93800000);
        qtest_writel(q, spi[i] + 0x10, 0x700);
        qtest_writel(q, spi[i] + 0x40, i + 1);
    }
    qtest_writel(q, USB_CLOCK, 4);
    qtest_writel(q, USB_CTRL1, 1u << 14);
    qtest_writeb(q, USB + 0x60, 1);
    qtest_writeb(q, USB + 0x01, 0x20);
    qtest_writel(q, 0x43100014, 1u << 8);
    g_assert_cmphex(qtest_readb(q, USB + 0x60), ==, 0);
    g_assert_cmphex(qtest_readb(q, USB + 0x01), ==, 0);
    for (unsigned i = 0; i < 2; i++) {
        g_assert_cmphex(qtest_readl(q, spi[i] + 0x10), ==, 0x700);
        g_assert_cmphex(qtest_readl(q, spi[i] + 0x40), ==, i + 1);
    }
    qtest_writeb(q, USB + 0x60, 1);
    qtest_writeb(q, USB + 0x01, 0x20);
    qtest_writeb(q, USB + 0x0e, 3);
    qtest_writeb(q, USB + 0x62, 3);
    qtest_writew(q, USB + 0x64, 8);
    for (unsigned i = 0; i < 2; i++) {
        qtest_writel(q, 0x43100014, 1u << (3 + i));
        g_assert_cmphex(qtest_readl(q, spi[i] + 0x10), ==, 0);
        g_assert_cmphex(qtest_readl(q, spi[i] + 0x40), ==, 0);
        g_assert_cmphex(qtest_readl(q, spi[1 - i] + 0x10), ==, 0x700);
        g_assert_cmphex(qtest_readl(q, spi[1 - i] + 0x40), ==, 2 - i);
        g_assert_cmphex(qtest_readb(q, USB + 0x60), ==, 5);
        g_assert_cmphex(qtest_readb(q, USB + 0x01), ==, 0x20);
        g_assert_cmphex(qtest_readb(q, USB + 0x0e), ==, 3);
        g_assert_cmphex(qtest_readb(q, USB + 0x62), ==, 3);
        g_assert_cmphex(qtest_readw(q, USB + 0x64), ==, 8);
        g_assert_cmphex(qtest_readl(q, USB_CTRL1), ==, 1u << 14);
        g_assert_cmphex(qtest_readb(q, ECLIC_PENDING(56)) & 1, ==, 0);
        qtest_writel(q, spi[i] + 0x10, 0x700);
        qtest_writel(q, spi[i] + 0x40, i + 1);
    }
    qtest_quit(q);
}

typedef struct USBRejected {
    const char *name;
    uint32_t address, value;
    unsigned size;
    bool read;
} USBRejected;

static const USBRejected usb_rejections[] = {
    { "tx-ready", USB + 0x12, 2, 1 },
    { "fifo", USB + 0x20, 0x12345678, 4 },
    { "dma", USB + 0x204, 1, 4 },
    { "unknown-read", USB, 0, 1, true },
    { "wide-enable", USB + 0x06, 0, 4 },
    { "narrow-enable", USB + 0x06, 0, 1 },
    { "wide-power", USB + 0x01, 0, 2 },
    { "power-resume", USB + 0x01, 4, 1 },
    { "power-reset", USB + 0x01, 8, 1 },
    { "power-softconn", USB + 0x01, 0x40, 1 },
    { "power-isoupdate", USB + 0x01, 0x80, 1 },
    { "power-all", USB + 0x01, 0xff, 1 },
    { "qword", USB + 0x08, 0, 8 },
    { "unaligned-address", USB + 0x65, 0, 2 },
    { "index", USB + 0x0e, 8, 1 },
    { "fifo-size", USB + 0x62, 10, 1 },
    { "fifo-reserved", USB + 0x63, 0x20, 1 },
    { "fifo-address", USB + 0x64, 512, 2 },
    { "fifo-end", USB + 0x64, 257, 2 },
    { "double-fifo", USB + 0x63, 0x19, 1 },
    { "host-request", USB + 0x60, 2, 1 },
    { "gated", USB + 0x60, 1, 1 },
    { "device-session", USB + 0x60, 1, 1 },
    { "utmi8-session", USB + 0x60, 1, 1 },
    { "config-reserved", USB_CTRL1, 1u << 31, 4 },
    { "config-bist", USB_CTRL1, 1u << 20, 4 },
    { "active-iddig", USB_CTRL1, (1u << 18) | (1u << 14), 4 },
    { "active-utmi8", USB_CTRL1, 0, 4 },
    { "status-write", USB + 0x02, 0, 2 },
    { "ir-reset", 0x43100014, 1u << 7, 4 },
};

static void usb_rejected(const void *opaque)
{
    const USBRejected *test = opaque;

    if (g_test_subprocess()) {
        QTestState *q = start(false);
        qtest_set_expected_status(q, 1);
        qtest_writel(q, USB_CLOCK, 4);
        qtest_writel(q, USB_CTRL1, 1u << 14);
        if (!strcmp(test->name, "gated")) {
            qtest_writel(q, USB_CLOCK, 0);
        } else if (!strcmp(test->name, "device-session")) {
            qtest_writel(q, USB_CTRL1, (1u << 18) | (1u << 14));
        } else if (!strcmp(test->name, "utmi8-session")) {
            qtest_writel(q, USB_CTRL1, 0);
        } else if (g_str_has_prefix(test->name, "active-")) {
            qtest_writeb(q, USB + 0x60, 1);
        } else if (!strcmp(test->name, "fifo-end")) {
            qtest_writeb(q, USB + 0x62, 8);
        }
        if (test->read) {
            qtest_readb(q, test->address);
        } else if (test->size == 1) {
            qtest_writeb(q, test->address, test->value);
        } else if (test->size == 2) {
            qtest_writew(q, test->address, test->value);
        } else if (test->size == 4) {
            qtest_writel(q, test->address, test->value);
        } else {
            qtest_writeq(q, test->address, test->value);
        }
        qtest_quit(q);
        return;
    }
    g_test_trap_subprocess(NULL, 5 * G_USEC_PER_SEC, 0);
    g_test_trap_assert_failed();
    if (test->address == USB_CTRL1) {
        g_test_trap_assert_stderr("*VenusA unsupported write*"
                                 "address=0x43100040*");
    } else if (test->address == 0x43100014) {
        g_test_trap_assert_stderr("*VenusA unsupported write*"
                                 "address=0x43100014*");
    } else {
        g_autofree char *message = g_strdup_printf(
            "*ListenAI MUSB unsupported %s offset=0x%x size=%u*",
            test->read ? "read" : "write", test->address - USB, test->size);
        g_test_trap_assert_stderr(message);
    }
}

static void systimer(void)
{
    QTestState *q = start(false);
    qtest_writel(q, MTIME + 8, 5);
    qtest_writel(q, MTIME + 12, 0);
    qtest_clock_step(q, 4999);
    g_assert_cmpuint(qtest_readl(q, MTIME), ==, 4);
    g_assert_cmpuint(qtest_readb(q, ECLIC_PENDING(7)) & 1, ==, 0);
    qtest_clock_step(q, 1);
    g_assert_cmpuint(qtest_readl(q, MTIME), ==, 5);
    g_assert_cmpuint(qtest_readb(q, ECLIC_PENDING(7)) & 1, ==, 1);
    qtest_writel(q, MTIME + 8, 7);
    g_assert_cmpuint(qtest_readb(q, ECLIC_PENDING(7)) & 1, ==, 0);
    qtest_clock_step(q, 100);
    qtest_writel(q, MTIME + 0xff8, 1);
    qtest_clock_step(q, 100000);
    g_assert_cmpuint(qtest_readl(q, MTIME), ==, 5);
    qtest_writel(q, MTIME + 0xff8, 0);
    qtest_clock_step(q, 1899);
    g_assert_cmpuint(qtest_readb(q, ECLIC_PENDING(7)) & 1, ==, 0);
    qtest_clock_step(q, 1);
    g_assert_cmpuint(qtest_readb(q, ECLIC_PENDING(7)) & 1, ==, 1);
    qtest_writel(q, MTIME + 0xffc, 1);
    g_assert_cmpuint(qtest_readb(q, ECLIC_PENDING(3)) & 1, ==, 1);
    qtest_writel(q, MTIME + 0xffc, 0);
    g_assert_cmpuint(qtest_readb(q, ECLIC_PENDING(3)) & 1, ==, 0);
    qtest_system_reset(q);
    g_assert_cmpuint(qtest_readl(q, MTIME), ==, 0);
    g_assert_cmpuint(qtest_readb(q, ECLIC_PENDING(7)) & 1, ==, 0);
    qtest_quit(q);
}

static void psram(void)
{
    QTestState *q = start(false);
    static const uint32_t reset_modes[] = {
        0x8d09, 0x938d, 0xa093, 0x40a0, 0x0540, 0, 0, 0, 0x0905, 0x0101,
    };
    for (unsigned i = 0; i < G_N_ELEMENTS(reset_modes); i++) {
        g_assert_cmphex(qtest_readl(q, PSRAM_CTRL + 0x800 + i * 4),
                       ==, reset_modes[i]);
    }
    qtest_writel(q, 0x38700000, 0x11223344);
    g_assert_cmphex(qtest_readl(q, 0x38f00000), ==, 0x11223344);
    qtest_writel(q, 0x38f00000, 0xaabbccdd);
    g_assert_cmphex(qtest_readl(q, 0x38700000), ==, 0xaabbccdd);
    qtest_writel(q, PSRAM_CTRL + 0x800, 0x22);
    g_assert_cmphex(qtest_readl(q, PSRAM_CTRL + 0x800) & 0xff, ==, 0x22);
    g_assert_cmphex(qtest_readl(q, PSRAM_CTRL + 0x820) >> 8, ==, 0x22);
    qtest_system_reset(q);
    g_assert_cmphex(qtest_readl(q, 0x38700000), ==, 0xaabbccdd);
    g_assert_cmphex(qtest_readl(q, PSRAM_CTRL + 0x800), ==, 0x8d09);
    qtest_quit(q);
}

static uint32_t command(QTestState *q, unsigned index, uint32_t argument,
                        unsigned flags)
{
    qtest_writel(q, SD + 0x30, UINT32_MAX);
    qtest_writel(q, SD + 8, argument);
    qtest_writew(q, SD + 0xe, (index << 8) | flags);
    uint32_t status = qtest_readl(q, SD + 0x30);
    g_assert_cmphex(status & 0xffff8000, ==, 0);
    g_assert_cmphex(status & 1, ==, 1);
    return qtest_readl(q, SD + 0x10);
}

static void sd_transfers(void)
{
    QTestState *q = start(true);
    qtest_clock_step(q, 1000000000);
    g_assert_cmphex(qtest_readl(q, SD + 0x40), ==, 0x27200080);
    g_assert_cmphex(qtest_readl(q, SD + 0x44), ==, 0x2f77);
    g_assert_cmphex(qtest_readl(q, SD + 0x24) & 0x70000, ==, 0x20000);
    qtest_writel(q, SD + 0x34, UINT32_MAX);
    qtest_writel(q, SD + 0x38, UINT32_MAX);
    qtest_writeb(q, SD + 0x29, 15);
    qtest_writew(q, SD + 0x2c, 7);
    command(q, 0, 0, 0);
    g_assert_cmphex(command(q, 8, 0x1aa, 2), ==, 0x1aa);
    command(q, 55, 0, 2);
    g_assert_cmphex(command(q, 41, 0xff8000, 2) & 0x80000000, ==, 0x80000000);
    command(q, 2, 0, 1);
    uint32_t rca = command(q, 3, 0, 2) & 0xffff0000;
    command(q, 7, rca, 3);
    qtest_writew(q, SD + 4, 512);
    qtest_writew(q, SD + 6, 1);
    qtest_writew(q, SD + 12, 2);
    command(q, 24, 512, 0x3a);
    for (unsigned n = 0; n < 128; n++) {
        qtest_writel(q, SD + 32, 0xa5000000 + n);
    }
    g_assert_cmphex(qtest_readl(q, SD + 0x30) & 2, ==, 2);
    g_assert_cmphex(qtest_readb(q, ECLIC_PENDING(39)) & 1, ==, 1);
    qtest_writel(q, SD + 0x30, UINT32_MAX);
    g_assert_cmphex(qtest_readb(q, ECLIC_PENDING(39)) & 1, ==, 0);
    qtest_writew(q, SD + 6, 1);
    qtest_writew(q, SD + 12, 0x12);
    command(q, 17, 512, 0x3a);
    for (unsigned n = 0; n < 128; n++) {
        g_assert_cmphex(qtest_readl(q, SD + 32), ==, 0xa5000000 + n);
    }
    qtest_writew(q, SD + 6, 1);
    qtest_writew(q, SD + 12, 0x11);
    qtest_writel(q, SD, 0x20010000);
    g_assert_cmphex(qtest_readw(q, SD + 12) & 1, ==, 1);
    command(q, 17, 512, 0x3a);
    g_assert_cmphex(qtest_readl(q, SD), ==, 0x20010200);
    for (unsigned n = 0; n < 128; n++) {
        g_assert_cmphex(qtest_readl(q, 0x20010000 + n * 4), ==, 0xa5000000 + n);
    }
    g_assert_cmphex(qtest_readl(q, SD + 0x30) & 2, ==, 2);
    qtest_quit(q);
    FILE *f = fopen(sd_path, "rb");
    uint32_t data[128];
    g_assert_nonnull(f);
    g_assert_cmpint(fseek(f, 512, SEEK_SET), ==, 0);
    g_assert_cmpuint(fread(data, sizeof(data), 1, f), ==, 1);
    fclose(f);
    for (unsigned n = 0; n < 128; n++) {
        g_assert_cmphex(le32_to_cpu(data[n]), ==, 0xa5000000 + n);
    }
}

static void dma_channels(void)
{
    QTestState *q = start(false);
    const uint32_t base = 0x40000000;
    for (unsigned n = 0; n < 16; n++) {
        qtest_writel(q, 0x20010000 + n * 4, 0x78340000 + n);
    }
    qtest_writel(q, base + 0x310, 0x3f3f);
    qtest_writel(q, base + 0x398, 1);
    for (unsigned channel = 0; channel < 6; channel++) {
        uint32_t off = base + channel * 0x58;
        uint32_t destination = 0x20011000 + channel * 0x100;
        qtest_writel(q, off, 0x20010000);
        qtest_writel(q, off + 8, destination);
        qtest_writel(q, off + 0x18, 0x25);
        qtest_writel(q, off + 0x1c, 16);
        qtest_writel(q, base + 0x3a0, 0x101u << channel);
        for (unsigned n = 0; n < 16; n++) {
            g_assert_cmphex(qtest_readl(q, destination + n * 4), ==, 0x78340000 + n);
        }
        g_assert_cmphex(qtest_readl(q, off), ==, 0x20010040);
        g_assert_cmphex(qtest_readl(q, off + 8), ==, destination + 64);
        g_assert_cmphex(qtest_readl(q, off + 0x1c), ==, 0x100010);
        g_assert_cmphex(qtest_readl(q, base + 0x3a0), ==, 0);
        for (unsigned irq = 0; irq < 6; irq++) {
            g_assert_cmphex(qtest_readb(q, ECLIC_PENDING(43 + irq)) & 1,
                           ==, irq == channel);
        }
        qtest_writel(q, base + 0x338, 1u << channel);
        g_assert_cmphex(qtest_readb(q, ECLIC_PENDING(43 + channel)) & 1, ==, 0);
    }
    qtest_system_reset(q);
    g_assert_cmphex(qtest_readl(q, base + 0x398), ==, 0);
    g_assert_cmphex(qtest_readl(q, base + 0x2c0), ==, 0);
    qtest_quit(q);
}

static void dma_descriptor_writeback(void)
{
    QTestState *q = start(false);
    const uint32_t base = 0x40000000, lli = 0x20012000;
    for (unsigned i = 0; i < 8; i++) { qtest_writel(q, 0x20010000 + i * 4, 0xaabb0000 + i); }
    for (unsigned i = 0; i < 2; i++) {
        uint32_t descriptor = lli + i * 32;
        qtest_writel(q, descriptor, 0x20010000 + i * 16);
        qtest_writel(q, descriptor + 4, 0x20011000 + i * 16);
        qtest_writel(q, descriptor + 8, i ? 0 : lli + 32);
        qtest_writel(q, descriptor + 12, i ? 0x25 : 0x18000025);
        qtest_writel(q, descriptor + 16, 4);
    }
    qtest_writel(q, base + 0x10, lli);
    qtest_writel(q, base + 0x18, 0x18000025);
    qtest_writel(q, base + 0x398, 1);
    g_assert_cmphex(qtest_readl(q, lli + 16), ==, 4);
    qtest_writel(q, base + 0x3a0, 0x101);
    for (unsigned i = 0; i < 8; i++) { g_assert_cmphex(qtest_readl(q, 0x20011000 + i * 4), ==, 0xaabb0000 + i); }
    g_assert_cmphex(qtest_readl(q, lli + 16), ==, 0x100004);
    g_assert_cmphex(qtest_readl(q, lli + 48), ==, 0x100004);
    qtest_quit(q);
}

static void calendar(void)
{
    QTestState *q = start(false);
    const uint32_t base = 0x43400000;
    qtest_writel(q, base + 0x0c, 0x173b3b);
    qtest_writel(q, base + 0x10, 0x0418021d);
    qtest_writel(q, base + 4, 1);
    qtest_writel(q, base, 2);
    qtest_writel(q, base + 0x1c, 0);
    qtest_writel(q, base + 0x20, 0x00180301);
    qtest_writel(q, base + 4, 0x10030);
    qtest_clock_step(q, 999999999);
    g_assert_cmphex(qtest_readl(q, base + 8) & 3, ==, 0);
    qtest_clock_step(q, 1);
    g_assert_cmphex(qtest_readl(q, base + 0x14), ==, 0);
    g_assert_cmphex(qtest_readl(q, base + 0x18), ==, 0x05180301);
    g_assert_cmphex(qtest_readl(q, base + 8) & 3, ==, 3);
    g_assert_cmphex(qtest_readb(q, ECLIC_PENDING(22)) & 1, ==, 1);
    qtest_writel(q, base + 4, 0x100);
    g_assert_cmphex(qtest_readl(q, base + 8) & 3, ==, 1);
    qtest_writel(q, base + 4, 0x20000);
    g_assert_cmphex(qtest_readb(q, ECLIC_PENDING(22)) & 1, ==, 0);
    qtest_writel(q, base + 4, 0x10000);
    g_assert_cmphex(qtest_readb(q, ECLIC_PENDING(22)) & 1, ==, 1);
    qtest_writel(q, base + 4, 0x200);
    g_assert_cmphex(qtest_readb(q, ECLIC_PENDING(22)) & 1, ==, 0);
    qtest_system_reset(q);
    g_assert_cmphex(qtest_readl(q, base + 0x18), ==, 0x06000101);
    qtest_quit(q);
}

static void dual_timers(void)
{
    QTestState *q = start(false);
    for (unsigned block = 0; block < 2; block++) {
        uint32_t base = 0x43200000 + 0x100000 * block;
        for (unsigned channel = 0; channel < 2; channel++) {
            uint32_t reg = base + 0x20 * channel;
            qtest_writel(q, reg, 2);
            qtest_writel(q, reg + 8, 0xe2);
            qtest_clock_step(q, 187499);
            g_assert_cmphex(qtest_readl(q, reg + 4), ==, 0);
            g_assert_cmphex(qtest_readb(q, ECLIC_PENDING(20 + block)) & 1, ==, 0);
            qtest_clock_step(q, 1);
            g_assert_cmphex(qtest_readb(q, ECLIC_PENDING(20 + block)) & 1, ==, 1);
            g_assert_cmphex(qtest_readl(q, reg + 4), ==, 2);
            qtest_writel(q, reg + 12, 1);
            g_assert_cmphex(qtest_readb(q, ECLIC_PENDING(20 + block)) & 1, ==, 0);
            qtest_writel(q, reg + 8, 0x62);
            qtest_clock_step(q, 1000000);
            g_assert_cmphex(qtest_readl(q, reg + 4), ==, 2);
        }
    }
    qtest_quit(q);
}

static void gpt_timers(void)
{
    QTestState *q = start(false);
    const uint32_t base = 0x44000000;
    qtest_writel(q, 0x43100028, 0x40);
    for (unsigned channel = 0; channel < 2; channel++) {
        for (unsigned down = 0; down < 2; down++) {
            qtest_writel(q, 0x43100014, 0x200);
            g_assert_cmphex(qtest_readl(q, base), ==, 0x10);
            g_assert_cmphex(qtest_readl(q, base + 0x80), ==, 0x7bf0);
            qtest_writel(q, base + 4 + 4 * channel, 0x0180095f);
            qtest_writel(q, base + 12 + 4 * channel, 8);
            qtest_writel(q, base + 20 + 4 * channel, 9);
            qtest_writel(q, base + 0xa0, 1u << channel);
            uint32_t cfg = (1u << (14 + 4 * channel)) |
                           (down << (6 + 4 * channel));
            qtest_writel(q, base + 0x1c, cfg);
            qtest_writel(q, base + 12 + 4 * channel, 0x8008);
            qtest_writel(q, base + 0x1c, cfg | (1u << channel) |
                         (1u << (24 + channel)));
            qtest_clock_step(q, 999999);
            g_assert_cmphex(qtest_readl(q, base + 0x84), ==, 0);
            uint32_t count = qtest_readl(q, base + 20 + 4 * channel) >> 16;
            g_assert_cmphex(count, ==, down ? 0 : 9);
            qtest_clock_step(q, 1);
            uint32_t pending = 1u << (6 + channel);
            g_assert_cmphex(qtest_readl(q, base + 0x84), ==, pending);
            g_assert_cmphex(qtest_readb(q, ECLIC_PENDING(34)) & 1, ==, 0);
            qtest_writel(q, base + 0x80, 0x7bf0 & ~pending);
            g_assert_cmphex(qtest_readb(q, ECLIC_PENDING(34)) & 1, ==, 1);
            g_assert_cmphex(qtest_readl(q, base + 0x80) >> 16, ==, pending);
            qtest_writel(q, base + 0x84, pending << 16);
            g_assert_cmphex(qtest_readb(q, ECLIC_PENDING(34)) & 1, ==, 0);
            /* A gated module preserves a partially elapsed timer clock. */
            qtest_clock_step(q, 150000);
            qtest_writel(q, 0x43100028, 0);
            count = qtest_readl(q, base + 20 + 4 * channel);
            qtest_clock_step(q, 10000000);
            g_assert_cmphex(qtest_readl(q, base + 20 + 4 * channel), ==, count);
            qtest_writel(q, 0x43100028, 0x40);
            qtest_clock_step(q, 849999);
            g_assert_cmphex(qtest_readl(q, base + 0x84), ==, 0);
            qtest_clock_step(q, 1);
            g_assert_cmphex(qtest_readl(q, base + 0x84), ==, pending);
        }
    }
    /* Two independent 8-bit timers share one channel clock and raw IRQ word. */
    qtest_writel(q, 0x43100014, 0x200);
    qtest_writel(q, base + 4, 0x0180095f);
    qtest_writel(q, base + 12, 0x10);
    qtest_writel(q, base + 20, 0x0301);
    qtest_writel(q, base + 0xa0, 1);
    qtest_writel(q, base + 12, 0x8010);
    qtest_writel(q, base + 0x1c, 0x05000001);
    qtest_clock_step(q, 200000);
    g_assert_cmphex(qtest_readl(q, base + 0x84), ==, 0x40);
    qtest_writel(q, base + 0x84, 0x00400000);
    qtest_clock_step(q, 200000);
    g_assert_cmphex(qtest_readl(q, base + 0x84), ==, 0x100);
    qtest_writel(q, base + 0x84, 0x01000000);
    qtest_clock_step(q, 10000000);
    g_assert_cmphex(qtest_readl(q, base + 0x84), ==, 0);
    g_assert_cmphex(qtest_readl(q, base + 20), ==, 0x03010301);
    /* Free-run uses the full counter width, independently of reload. */
    qtest_writel(q, 0x43100014, 0x200);
    qtest_writel(q, base + 4, 0x0180095f);
    qtest_writel(q, base + 12, 0x8008);
    qtest_writel(q, base + 20, 9);
    qtest_writel(q, base + 0xa0, 1);
    qtest_writel(q, base + 0x1c, 0x01008001);
    qtest_clock_step(q, INT64_C(6553600000) - 1);
    g_assert_cmphex(qtest_readl(q, base + 0x84), ==, 0);
    g_assert_cmphex(qtest_readl(q, base + 20) >> 16, ==, 65535);
    qtest_clock_step(q, 1);
    g_assert_cmphex(qtest_readl(q, base + 0x84), ==, 0x40);
    /* Keepgo resumes the stopped count; an external T0 divider is latched. */
    qtest_writel(q, 0x43100014, 0x200);
    qtest_writel(q, 0x43100028, 0x62);
    qtest_writel(q, 0x43100028, 0x42);
    qtest_writel(q, base + 4, 0x0180095f);
    qtest_writel(q, base + 12, 0x8008);
    qtest_writel(q, base + 20, 9);
    qtest_writel(q, base + 0xa0, 1);
    qtest_writel(q, base + 0x1c, 0x0100c041);
    qtest_clock_step(q, 600000);
    g_assert_cmphex(qtest_readl(q, base + 20) >> 16, ==, 6);
    qtest_writel(q, base + 0x1c, 0x0100c044);
    qtest_clock_step(q, 10000000);
    g_assert_cmphex(qtest_readl(q, base + 20) >> 16, ==, 6);
    qtest_writel(q, base + 0x1c, 0x0100c041);
    qtest_clock_step(q, 1399999);
    g_assert_cmphex(qtest_readl(q, base + 0x84), ==, 0);
    qtest_clock_step(q, 1);
    g_assert_cmphex(qtest_readl(q, base + 0x84), ==, 0x40);
    qtest_quit(q);
}

static void otp_mapping(void)
{
    QTestState *q = start(false);
    for (unsigned word = 0; word < 16; word++) {
        g_assert_cmphex(qtest_readl(q, 0x45500080 + 4 * word), ==, 0x97530000 + word);
        g_assert_cmphex(qtest_readl(q, 0x45501000 + 4 * word), ==, 0x97530000 + word);
    }
    qtest_system_reset(q);
    g_assert_cmphex(qtest_readl(q, 0x45500090), ==, 0x97530004);
    qtest_quit(q);
}

static void aon_timer(void)
{
    QTestState *q = start(false);
    const uint32_t base = 0x45300000, gate = 0x45000054;
    qtest_writel(q, gate, 4);
    qtest_writel(q, base + 8, 1);
    qtest_writel(q, base, 0x41000002);
    qtest_clock_step(q, 40000);
    g_assert_cmpuint(qtest_readl(q, base + 4), ==, 1);
    qtest_writel(q, gate, 0);
    qtest_clock_step(q, 1000000);
    g_assert_cmpuint(qtest_readl(q, base + 4), ==, 1);
    qtest_writel(q, gate, 4);
    qtest_clock_step(q, 53749);
    g_assert_cmpuint(qtest_readb(q, ECLIC_PENDING(64)) & 1, ==, 0);
    qtest_clock_step(q, 1);
    g_assert_cmpuint(qtest_readb(q, ECLIC_PENDING(64)) & 1, ==, 1);
    g_assert_cmphex(qtest_readl(q, 0x45000014) & 1, ==, 1);
    qtest_writel(q, base + 12, 1);
    g_assert_cmpuint(qtest_readb(q, ECLIC_PENDING(64)) & 1, ==, 0);
    qtest_writel(q, 0x45000020, 1);
    g_assert_cmphex(qtest_readl(q, 0x45000014) & 1, ==, 0);
    qtest_writel(q, base, 0x51000002);
    qtest_clock_step(q, 93750);
    g_assert_cmpuint(qtest_readb(q, ECLIC_PENDING(64)) & 1, ==, 1);
    qtest_writel(q, 0x45000058, 2);
    g_assert_cmpuint(qtest_readb(q, ECLIC_PENDING(64)) & 1, ==, 0);
    qtest_clock_step(q, 1000000);
    g_assert_cmpuint(qtest_readl(q, base + 4), ==, 0);
    qtest_quit(q);
}

static void watchdogs(void)
{
    QTestState *q = start(false);
    const uint32_t base = 0x44400000, gate = 0x45000054;
    qtest_writel(q, base + 0x10, 1);
    g_assert_cmpuint(qtest_readl(q, base + 0x10), ==, 0);
    qtest_writel(q, base + 0x18, 0x5aa5);
    qtest_writel(q, base + 0x10, 0x100);
    qtest_writel(q, base + 0x10, 0x200);
    g_assert_cmphex(qtest_readl(q, base + 0x10), ==, 0x100);
    qtest_writel(q, gate, 0x100);
    qtest_writel(q, base + 0x18, 0x5aa5);
    qtest_writel(q, base + 0x10, 5);
    qtest_clock_step(q, 1000000);
    qtest_writel(q, gate, 0);
    /* This is the PCLK gate; the independent RC32k counter keeps running. */
    qtest_clock_step(q, 999999);
    g_assert_cmpuint(qtest_readb(q, ECLIC_PENDING(36)) & 1, ==, 0);
    qtest_clock_step(q, 1);
    g_assert_cmpuint(qtest_readb(q, ECLIC_PENDING(36)) & 1, ==, 1);
    qtest_writel(q, gate, 0x100);
    g_assert_cmpuint(qtest_readb(q, ECLIC_PENDING(36)) & 1, ==, 1);
    qtest_writel(q, base + 0x1c, 1);
    qtest_clock_step(q, 10000000);
    g_assert_cmpuint(qtest_readb(q, ECLIC_PENDING(36)) & 1, ==, 0);
    qtest_writel(q, base + 0x18, 0x5aa5);
    qtest_writel(q, base + 0x10, 13);
    qtest_clock_step(q, 3000000);
    qtest_writel(q, gate, 0);
    qtest_clock_step(q, 2999999);
    g_assert_cmphex(qtest_readl(q, 0x4500004c) & 0x200000, ==, 0);
    qtest_clock_step(q, 1);
    g_assert_cmphex(qtest_readl(q, 0x4500004c) & 0x200000, ==, 0x200000);
    g_assert_cmpuint(qtest_readl(q, base + 0x10), ==, 0);
    qtest_writel(q, 0x20010000, 0x12345678);
    qtest_writel(q, 0x4310000c, 1);
    qtest_writel(q, 0x45000054, 0x104);
    qtest_writel(q, 0x45300000, 0x4100ffff);
    qtest_writel(q, base + 0x18, 0x5aa5);
    qtest_writel(q, base + 0x10, 13);
    qtest_clock_step(q, 6000000);
    qtest_qmp_eventwait(q, "RESET");
    g_assert_cmphex(qtest_readl(q, 0x4500004c), ==, 0x200000);
    g_assert_cmphex(qtest_readl(q, 0x4310000c), ==, 1);
    g_assert_cmphex(qtest_readl(q, 0x20010000), ==, 0x12345678);
    g_assert_cmphex(qtest_readl(q, 0x45000054), ==, 0x104);
    uint32_t remaining = qtest_readl(q, 0x45300004);
    g_assert_cmpuint(remaining, <, 0xffff);
    qtest_clock_step(q, 31250);
    g_assert_cmpuint(qtest_readl(q, 0x45300004), ==, remaining - 1);
    qtest_quit(q);
}

static void cluster_interrupts(void)
{
    QTestState *q = start(false);
    const uint64_t idu = 0x47000000, spi = 0x44200000;
    g_assert_cmpuint(qtest_readl(q, idu + 0xc084), ==, 2);
    g_assert_cmpuint(qtest_readl(q, idu + 0xc090), ==, 80);
    g_assert_cmphex(qtest_readl(q, idu + 0x80), ==, 0x3ff);
    qtest_writel(q, idu + 0x80, 0);
    qtest_writel(q, idu + 0x80, 1);
    g_assert_cmpuint(qtest_readl(q, idu + 0x80), ==, 0);
    qtest_writel(q, idu + 0x80, UINT32_MAX);
    g_assert_cmphex(qtest_readl(q, idu + 0x80), ==, 0x3ff);
    qtest_writel(q, idu + 0x403c, 0); /* SPIB IRQ 35, external 15. */
    qtest_writel(q, spi + 0x10, 0x700);
    qtest_writel(q, spi + 0x38, 16);
    qtest_writel(q, spi + 0x20, 0x42000002);
    qtest_writel(q, spi + 0x24, 0x9f);
    g_assert_cmpuint(qtest_readb(q, ECLIC_PENDING(35)), ==, 0);
    qtest_writel(q, idu + 0x403c, 3);
    g_assert_cmpuint(qtest_readb(q, ECLIC_PENDING(35)), ==, 1);
    qtest_writel(q, idu + 0x803c, 2); /* Core 1 claims first. */
    qtest_writel(q, idu + 0x803c, 1);
    g_assert_cmpuint(qtest_readl(q, idu + 0x803c), ==, 2);
    g_assert_cmpuint(qtest_readb(q, ECLIC_PENDING(35)), ==, 0);
    qtest_writel(q, idu + 0x803c, UINT32_MAX);
    g_assert_cmpuint(qtest_readb(q, ECLIC_PENDING(35)), ==, 1);
    qtest_writel(q, spi + 0x3c, 16);
    g_assert_cmpuint(qtest_readb(q, ECLIC_PENDING(35)), ==, 0);
    qtest_writel(q, idu + 0x3ffc, 0x55010000);
    g_assert_cmphex(qtest_readl(q, idu), ==, 0x55000002);
    g_assert_cmpuint(qtest_readb(q, ECLIC_PENDING(19)), ==, 1);
    qtest_writel(q, idu, 2);
    g_assert_cmpuint(qtest_readb(q, ECLIC_PENDING(19)), ==, 0);
    qtest_system_reset(q);
    g_assert_cmpuint(qtest_readl(q, idu), ==, 0);
    g_assert_cmpuint(qtest_readl(q, idu + 0x403c), ==, 3);
    g_assert_cmpuint(qtest_readl(q, idu + 0x803c), ==, 3);
    qtest_quit(q);
}

static void nor_block_persistence(void)
{
    QTestState *q = start(false);
    const uint64_t base = 0x44200000;
    qtest_writel(q, base + 0x10, 0x780); /* 8-bit packed FIFO. */
    qtest_writel(q, base + 0x20, 0x47000000); /* Command only. */
    qtest_writel(q, base + 0x24, 0x06); /* Write enable. */
    qtest_writel(q, base + 0x28, 0x10000);
    qtest_writel(q, base + 0x24, 0x20); /* 4 KiB sector erase. */
    g_assert_cmphex(qtest_readl(q, 0x30010000), ==, UINT32_MAX);
    qtest_writel(q, base + 0x24, 0x06);
    qtest_writel(q, base + 0x2c, 0x12345678);
    qtest_writel(q, base + 0x20, 0x41003000); /* Four bytes to program. */
    qtest_writel(q, base + 0x24, 0x02);
    g_assert_cmphex(qtest_readl(q, 0x30010000), ==, 0x12345678);
    qtest_system_reset(q);
    g_assert_cmphex(qtest_readl(q, 0x30010000), ==, 0x12345678);
    qtest_quit(q);
    FILE *file = fopen(flash_path, "rb");
    g_assert_nonnull(file);
    g_assert_cmpint(fseek(file, 0x10000, SEEK_SET), ==, 0);
    uint32_t stored;
    g_assert_cmpuint(fread(&stored, sizeof(stored), 1, file), ==, 1);
    fclose(file);
    g_assert_cmphex(le32_to_cpu(stored), ==, 0x12345678);
    q = start(false);
    g_assert_cmphex(qtest_readl(q, 0x30010000), ==, 0x12345678);
    qtest_quit(q);
}

static void image(const char *pattern, char **path, size_t length)
{
    int fd = g_file_open_tmp(pattern, path, NULL);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, length), ==, 0);
    close(fd);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    image("venusa-test-flash-XXXXXX", &flash_path, 16 * 1024 * 1024);
    image("venusa-test-sd-XXXXXX", &sd_path, 64 * 1024 * 1024);
    image("venusa-test-otp-XXXXXX", &otp_path, 64);
    FILE *otp = fopen(otp_path, "wb");
    g_assert_nonnull(otp);
    for (unsigned i = 0; i < 16; i++) {
        uint32_t value = cpu_to_le32(0x97530000 + i);
        g_assert_cmpuint(fwrite(&value, 4, 1, otp), ==, 1);
    }
    fclose(otp);
    g_setenv("LISEM_QEMU_OTP_IMAGE", otp_path, true);
    qtest_add_func("venusa/cluster-interrupts", cluster_interrupts);
    qtest_add_func("venusa/nor-block-persistence", nor_block_persistence);
    qtest_add_func("venusa/usb-empty-port", usb_empty_port);
    qtest_add_func("venusa/usb-configuration", usb_configuration);
    qtest_add_func("venusa/usb-spi-reset-isolation", usb_spi_reset_isolation);
    for (unsigned i = 0; i < G_N_ELEMENTS(usb_rejections); i++) {
        g_autofree char *name = g_strdup_printf("venusa/usb-reject/%s",
                                               usb_rejections[i].name);
        qtest_add_data_func(name, &usb_rejections[i], usb_rejected);
    }
    qtest_add_func("venusa/systimer", systimer);
    qtest_add_func("venusa/psram", psram);
    qtest_add_func("venusa/sd-transfers", sd_transfers);
    qtest_add_func("venusa/dma-channels", dma_channels);
    qtest_add_func("venusa/dma-descriptor-writeback", dma_descriptor_writeback);
    qtest_add_func("venusa/calendar", calendar);
    qtest_add_func("venusa/dual-timers", dual_timers);
    qtest_add_func("venusa/gpt-timers", gpt_timers);
    qtest_add_func("venusa/otp-mapping", otp_mapping);
    qtest_add_func("venusa/aon-timer", aon_timer);
    qtest_add_func("venusa/watchdogs", watchdogs);
    int result = g_test_run();
    unlink(flash_path); unlink(sd_path); unlink(otp_path);
    g_free(flash_path); g_free(sd_path); g_free(otp_path);
    return result;
}
