/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include <jpeglib.h>
#include "qapi/error.h"
#include "qobject/qdict.h"
#include "qobject/qjson.h"

#define USB 0x41000000
#define USB_CLOCK 0x43100034
#define USB_CTRL1 0x43100040
#define ECLIC_PENDING(irq) (0xe0021000 + (irq) * 4)
#define EP(n) (USB + 0x100 + 16 * (n))
#define FIFO(n) (USB + 0x20 + 4 * (n))

static char *flash_path, *image_path;
static uint8_t *jpeg_data;
static gsize jpeg_size;

static QTestState *start(void)
{
    g_autofree char *flash = g_shell_quote(flash_path);
    QTestState *q;

    g_setenv("LISEM_QEMU_UVC_IMAGE", image_path, true);
    q = qtest_initf("-machine venusa-rd-evb "
                    "-drive if=pflash,format=raw,file=%s", flash);
    g_unsetenv("LISEM_QEMU_UVC_IMAGE");
    qtest_writel(q, 0x43100034, 4);
    qtest_writel(q, 0x43100040, 1u << 14);
    qtest_writeb(q, USB + 1, 0x20);
    qtest_writew(q, USB + 6, 1);
    qtest_writeb(q, USB + 0xb, 0xf7);
    qtest_writeb(q, USB + 0x60, 1);
    return q;
}

static QTestState *device_start(void)
{
    g_autofree char *flash = g_shell_quote(flash_path);
    return qtest_initf("-machine venusa-rd-evb "
                       "-drive if=pflash,format=raw,file=%s", flash);
}

static void device_disconnected(void)
{
    QTestState *q = device_start();

    qtest_writel(q, USB_CLOCK, 4);
    qtest_writel(q, USB_CTRL1, (1u << 18) | (1u << 14));
    g_assert_cmphex(qtest_readb(q, USB + 0x60), ==, 0x80);
    /* SDK uses read/modify/write: the read-only BDEVICE bit must be harmless. */
    qtest_writeb(q, USB + 1, 0x20);
    qtest_writeb(q, USB + 0x60, qtest_readb(q, USB + 0x60) | 1);
    qtest_writeb(q, USB + 1, 0x60);
    g_assert_cmphex(qtest_readb(q, USB + 1), ==, 0x60);
    g_assert_cmphex(qtest_readb(q, USB + 0x60), ==, 0x81);
    g_assert_cmphex(qtest_readb(q, USB + 0x0a), ==, 0);
    g_assert_cmphex(qtest_readb(q, ECLIC_PENDING(56)) & 1, ==, 0);
    qtest_clock_step(q, 1000000000);
    g_assert_cmphex(qtest_readb(q, USB + 0x0a), ==, 0);
    g_assert_cmphex(qtest_readb(q, ECLIC_PENDING(56)) & 1, ==, 0);

    qtest_writel(q, USB_CLOCK, 0);
    qtest_writel(q, USB_CLOCK, 4);
    g_assert_cmphex(qtest_readb(q, USB + 1), ==, 0x60);
    qtest_writeb(q, USB + 1, 0x20);
    g_assert_cmphex(qtest_readb(q, USB + 1), ==, 0x20);
    qtest_writeb(q, USB + 1, 0x60);
    qtest_writel(q, 0x43100014, 1u << 8);
    g_assert_cmphex(qtest_readb(q, USB + 1), ==, 0);
    g_assert_cmphex(qtest_readb(q, USB + 0x60), ==, 0x80);
    g_assert_cmphex(qtest_readb(q, USB + 0x0a), ==, 0);
    g_assert_cmphex(qtest_readb(q, ECLIC_PENDING(56)) & 1, ==, 0);
    qtest_quit(q);
}

static void ep0_done(QTestState *q)
{
    qtest_clock_step(q, 125000);
    g_assert_cmphex(qtest_readw(q, USB + 2) & 1, ==, 1);
    g_assert_cmphex(qtest_readb(q, EP(0) + 2) & 0x94, ==, 0);
}

/* The same SETUP/data/status register sequence as ChryUSB handle_ep0(). */
static unsigned control(QTestState *q, uint8_t type, uint8_t request,
                        uint16_t value, uint16_t index, uint16_t length,
                        uint8_t *data)
{
    uint8_t setup[8] = { type, request };
    unsigned actual = 0;

    stw_le_p(setup + 2, value);
    stw_le_p(setup + 4, index);
    stw_le_p(setup + 6, length);
    for (unsigned i = 0; i < sizeof(setup); i++) {
        qtest_writeb(q, FIFO(0), setup[i]);
    }
    qtest_writeb(q, EP(0) + 2, 0x0a);
    ep0_done(q);
    while (actual < length) {
        unsigned n = MIN(64, length - actual);
        if (type & 0x80) {
            qtest_writeb(q, EP(0) + 2, 0x20);
            ep0_done(q);
            g_assert_cmphex(qtest_readb(q, EP(0) + 2) & 1, ==, 1);
            n = qtest_readw(q, EP(0) + 8);
            g_assert_cmpuint(n, <=, MIN(64, length - actual));
            for (unsigned i = 0; i < n; i++) {
                data[actual + i] = qtest_readb(q, FIFO(0));
            }
            qtest_writeb(q, EP(0) + 2, 0);
        } else {
            for (unsigned i = 0; i < n; i++) {
                qtest_writeb(q, FIFO(0), data[actual + i]);
            }
            qtest_writeb(q, EP(0) + 2, 2);
            ep0_done(q);
        }
        actual += n;
        if (n < 64) {
            break;
        }
    }
    qtest_writeb(q, EP(0) + 2, (type & 0x80) ? 0x42 : 0x60);
    ep0_done(q);
    qtest_writeb(q, EP(0) + 2, 0);
    return actual;
}

static void bus_reset(QTestState *q)
{
    g_assert_cmphex(qtest_readb(q, USB + 0xa) & 0x10, ==, 0x10);
    qtest_writeb(q, USB + 1, 0x28);
    qtest_clock_step(q, 20000000);
    qtest_writeb(q, USB + 1, 0x20);
    qtest_clock_step(q, 20000000);
    g_assert_cmphex(qtest_readb(q, USB + 1) & 0x10, ==, 0x10);
    /* Bus reset does not physically unplug/replug the attached camera. */
    g_assert_cmphex(qtest_readb(q, USB + 0xa) & 0x30, ==, 0);
}

static void enumerate(void)
{
    QTestState *q = start();
    uint8_t data[512];
    bool vc = false, vs = false, mjpeg = false, frame = false, iso = false;

    bus_reset(q);
    g_assert_cmpuint(control(q, 0x80, 6, 0x100, 0, 18, data), ==, 18);
    g_assert_cmpuint(data[0], ==, 18);
    g_assert_cmpuint(data[7], ==, 64);
    control(q, 0, 5, 1, 0, 0, NULL);
    qtest_writeb(q, USB + 0x80, 1);
    g_assert_cmpuint(control(q, 0x80, 6, 0x200, 0, 9, data), ==, 9);
    unsigned size = lduw_le_p(data + 2);
    g_assert_cmpuint(size, <=, sizeof(data));
    g_assert_cmpuint(control(q, 0x80, 6, 0x200, 0, size, data), ==, size);
    for (unsigned i = 0; i < size; i += data[i]) {
        g_assert_cmpuint(data[i], >=, 2);
        g_assert_cmpuint(data[i], <=, size - i);
        if (data[i + 1] == 4) {
            vc |= data[i + 5] == 14 && data[i + 6] == 1;
            vs |= data[i + 5] == 14 && data[i + 6] == 2;
        } else if (data[i + 1] == 0x24 && data[i + 2] == 6) {
            mjpeg = true;
        } else if (data[i + 1] == 0x24 && data[i + 2] == 7) {
            g_assert_cmpuint(lduw_le_p(data + i + 5), ==, 640);
            g_assert_cmpuint(lduw_le_p(data + i + 7), ==, 480);
            g_assert_cmpuint(ldl_le_p(data + i + 21), ==, 500000);
            frame = true;
        } else if (data[i + 1] == 5) {
            iso |= data[i + 2] == 0x81 && (data[i + 3] & 3) == 1;
        }
    }
    g_assert_true(vc && vs && mjpeg && frame && iso);
    control(q, 0, 9, 1, 0, 0, NULL);
    g_assert_cmpuint(control(q, 0x80, 8, 0, 0, 1, data), ==, 1);
    g_assert_cmpuint(data[0], ==, 1);
    qtest_quit(q);
}

static QTestState *configured(void)
{
    QTestState *q = start();

    bus_reset(q);
    control(q, 0, 5, 1, 0, 0, NULL);
    qtest_writeb(q, USB + 0x80, 1);
    control(q, 0, 9, 1, 0, 0, NULL);
    return q;
}

static void probe_commit(void)
{
    QTestState *q = configured();
    uint8_t data[26], committed[26];

    for (unsigned selector = 1; selector <= 2; selector++) {
        g_assert_cmpuint(control(q, 0xa1, 0x81, selector << 8, 1, 26, data),
                         ==, 26);
        g_assert_cmpuint(data[2], ==, 1);
        g_assert_cmpuint(data[3], ==, 1);
        g_assert_cmpuint(ldl_le_p(data + 4), ==, 500000);
        g_assert_cmpuint(ldl_le_p(data + 18), ==, 32768);
        g_assert_cmpuint(ldl_le_p(data + 22), ==, 1024);
        stw_le_p(data, 1); /* Fix the frame interval, as ChryUSB does. */
        g_assert_cmpuint(control(q, 0x21, 1, selector << 8, 1, 26, data),
                         ==, 26);
        control(q, 0xa1, 0x81, selector << 8, 1, 26, committed);
        g_assert_cmpmem(data, 26, committed, 26);
    }
    for (unsigned request = 0x82; request <= 0x84; request++) {
        control(q, 0xa1, request, 0x100, 1, 26, data);
        g_assert_cmpuint(ldl_le_p(data + 4), ==, request == 0x84 ? 0 : 500000);
    }
    control(q, 0xa1, 0x85, 0x100, 1, 2, data);
    g_assert_cmpuint(lduw_le_p(data), ==, 26);
    control(q, 0xa1, 0x86, 0x100, 1, 1, data);
    g_assert_cmpuint(data[0], ==, 3); /* GET and SET supported. */
    control(q, 1, 11, 1, 1, 0, NULL);
    control(q, 0x81, 10, 0, 1, 1, data);
    g_assert_cmpuint(data[0], ==, 1);
    qtest_quit(q);
}

static QDict *camera_state(QTestState *q)
{
    g_autoptr(QDict) response = qtest_qmp(q, "{'execute':'qom-get',"
        "'arguments':{'path':'/machine/soc/usb/camera',"
        "'property':'x-lisem-uvc-state'}}");
    QObject *obj = qobject_from_json(qdict_get_str(response, "return"),
                                    &error_abort);
    return qobject_to(QDict, obj);
}

static QTestState *stream_start(void)
{
    QTestState *q = configured();
    uint8_t probe[26];

    control(q, 0xa1, 0x81, 0x100, 1, 26, probe);
    control(q, 0x21, 1, 0x100, 1, 26, probe);
    control(q, 0x21, 1, 0x200, 1, 26, probe);
    control(q, 1, 11, 1, 1, 0, NULL);
    qtest_writeb(q, USB + 0x0e, 1);
    qtest_writeb(q, USB + 0x63, 7);
    qtest_writew(q, USB + 0x66, 8);
    qtest_writeb(q, USB + 0x8c, 1);
    qtest_writeb(q, EP(1) + 12, 0x51); /* HS, ISO, device endpoint 1. */
    qtest_writew(q, EP(1) + 4, 1024);
    qtest_writeb(q, EP(1) + 13, 1);
    qtest_writew(q, USB + 8, 2);
    return q;
}

static unsigned receive(QTestState *q, uint8_t *data)
{
    qtest_writeb(q, EP(1) + 6, 0x20);
    qtest_clock_step(q, 125000);
    g_assert_cmphex(qtest_readw(q, USB + 4), ==, 2);
    g_assert_cmphex(qtest_readb(q, EP(1) + 6), ==, 1);
    unsigned n = qtest_readw(q, EP(1) + 8);
    g_assert_cmpuint(n, <=, 1024);
    for (unsigned i = 0; i < n; i++) {
        data[i] = qtest_readb(q, FIFO(1));
    }
    qtest_writeb(q, EP(1) + 6, 0);
    return n;
}

static void streaming(void)
{
    QTestState *q = stream_start();
    uint8_t packet[1024], frame[32768];
    unsigned fid = 0;

    /* Empty transactions before the first virtual frame, not synthetic data. */
    g_assert_cmpuint(receive(q, packet), ==, 0);
    g_autoptr(QDict) state = camera_state(q);
    g_assert_cmpuint(qdict_get_int(state, "frames"), ==, 0);
    qtest_clock_set(q, qdict_get_int(state, "next_frame_ns") - 1);
    qobject_unref(state);
    state = camera_state(q);
    g_assert_cmpuint(qdict_get_int(state, "frames"), ==, 0);
    qtest_clock_step(q, 1);
    for (unsigned f = 0; f < 2; f++) {
        unsigned size = 0;
        do {
            unsigned n = receive(q, packet);
            g_assert_cmpuint(n, >=, 2);
            g_assert_cmpuint(packet[0], ==, 2);
            g_assert_cmphex(packet[1] & 0xfc, ==, 0x80);
            if (!size) {
                if (f) {
                    g_assert_cmpuint(packet[1] & 1, !=, fid);
                }
                fid = packet[1] & 1;
            }
            g_assert_cmpuint(packet[1] & 1, ==, fid);
            g_assert_cmpuint(size + n - 2, <=, sizeof(frame));
            memcpy(frame + size, packet + 2, n - 2);
            size += n - 2;
        } while (!(packet[1] & 2));
        g_assert_cmpmem(frame, size, jpeg_data, jpeg_size);
        qobject_unref(state);
        state = camera_state(q);
        g_assert_cmpuint(qdict_get_int(state, "completed_frames"), ==, f + 1);
        g_assert_cmpuint(qdict_get_int(state, "bytes"), ==, (f + 1) * jpeg_size);
        g_assert_cmpuint(receive(q, packet), ==, 0);
        if (!f) {
            int64_t next = qdict_get_int(state, "frame_ns") + 50000000;
            qtest_clock_set(q, next);
        }
    }
    control(q, 1, 11, 0, 1, 0, NULL);
    qtest_clock_step(q, 100000000);
    qobject_unref(state);
    state = camera_state(q);
    g_assert_false(qdict_get_bool(state, "streaming"));
    g_assert_cmpuint(qdict_get_int(state, "frames"), ==, 2);
    qtest_quit(q);
}

static void reset_control(void)
{
    QTestState *q = configured();
    const uint8_t setup[] = { 0x80, 6, 0, 1, 0, 0, 18, 0 };

    for (unsigned i = 0; i < sizeof(setup); i++) {
        qtest_writeb(q, FIFO(0), setup[i]);
    }
    qtest_writeb(q, EP(0) + 2, 0x0a);
    ep0_done(q);
    qtest_writeb(q, USB + 1, 0x28);
    qtest_clock_step(q, 20000000);
    qtest_writeb(q, USB + 1, 0x20);
    qtest_writeb(q, USB + 0x80, 0);
    /* No new SETUP: old descriptor bytes must not survive the reset. */
    qtest_writeb(q, EP(0) + 2, 0x20);
    qtest_clock_step(q, 125000);
    g_assert_cmphex(qtest_readb(q, EP(0) + 2) & 5, ==, 4);
    uint8_t data[18];
    g_assert_cmpuint(control(q, 0x80, 6, 0x100, 0, 18, data), ==, 18);
    qtest_quit(q);
}

static void flush_rx(void)
{
    QTestState *q = stream_start();
    g_autoptr(QDict) state = camera_state(q);
    qtest_clock_set(q, qdict_get_int(state, "next_frame_ns"));
    qtest_writeb(q, EP(1) + 6, 0x20);
    qtest_clock_step(q, 125000);
    g_assert_cmpuint(qtest_readw(q, EP(1) + 8), >, 2);
    qtest_writeb(q, EP(1) + 6, qtest_readb(q, EP(1) + 6) | 0x10);
    g_assert_cmphex(qtest_readb(q, EP(1) + 6) & 1, ==, 0);
    g_assert_cmpuint(qtest_readw(q, EP(1) + 8), ==, 0);
    uint8_t packet[1024];
    g_assert_cmpuint(receive(q, packet), >, 2);
    qtest_quit(q);
}

static void invalid_speed(void)
{
    if (g_test_subprocess()) {
        QTestState *q = start();
        qtest_writeb(q, USB + 1, 8);
        qtest_clock_step(q, 20000000);
        qtest_writeb(q, USB + 1, 0);
        qtest_quit(q);
        g_error("High-speed-only camera accepted full-speed configuration");
    }
    g_test_trap_subprocess(NULL, 5 * G_USEC_PER_SEC, 0);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr("*MUSB unsupported write offset=0x1*");
}

/* libjpeg produces an actual baseline 640x480 JPEG, not marker-only bytes. */
static void make_image(void)
{
    struct jpeg_compress_struct c;
    struct jpeg_error_mgr error;
    uint8_t row[640 * 3];
    FILE *file = fopen(image_path, "wb");

    g_assert_nonnull(file);
    c.err = jpeg_std_error(&error);
    jpeg_create_compress(&c);
    jpeg_stdio_dest(&c, file);
    c.image_width = 640;
    c.image_height = 480;
    c.input_components = 3;
    c.in_color_space = JCS_RGB;
    jpeg_set_defaults(&c);
    jpeg_set_quality(&c, 75, true);
    jpeg_start_compress(&c, true);
    while (c.next_scanline < c.image_height) {
        for (unsigned x = 0; x < 640; x++) {
            row[x * 3] = x / 4;
            row[x * 3 + 1] = c.next_scanline / 2;
            row[x * 3 + 2] = 48;
        }
        JSAMPROW rows[] = { row };
        jpeg_write_scanlines(&c, rows, 1);
    }
    jpeg_finish_compress(&c);
    jpeg_destroy_compress(&c);
    fclose(file);
    g_assert_true(g_file_get_contents(image_path, (char **)&jpeg_data,
                                      &jpeg_size, NULL));
    g_assert_cmpuint(jpeg_size, <=, 32768);
}

static void invalid_image(const void *opaque)
{
    const char *kind = opaque;

    if (g_test_subprocess()) {
        if (!strcmp(kind, "oversize")) {
            uint8_t bytes[32769] = { 0xff, 0xd8 };
            g_assert_true(g_file_set_contents(image_path, (char *)bytes,
                                              sizeof(bytes), NULL));
        } else if (!strcmp(kind, "progressive")) {
            for (unsigned i = 2; i + 1 < jpeg_size; i++) {
                if (jpeg_data[i] == 0xff && jpeg_data[i + 1] == 0xc0) {
                    jpeg_data[i + 1] = 0xc2;
                    break;
                }
            }
            g_assert_true(g_file_set_contents(image_path, (char *)jpeg_data,
                                              jpeg_size, NULL));
        } else if (!strcmp(kind, "corrupt")) {
            /* Keep SOI/EOI and SOF0, corrupt the entropy-coded scan. */
            memset(jpeg_data + jpeg_size - 100, 0xff, 98);
            g_assert_true(g_file_set_contents(image_path, (char *)jpeg_data,
                                              jpeg_size, NULL));
        } else if (!strcmp(kind, "truncated")) {
            g_assert_true(g_file_set_contents(image_path, (char *)jpeg_data,
                                              jpeg_size - 2, NULL));
        } else if (!strcmp(kind, "missing")) {
            g_assert_cmpint(unlink(image_path), ==, 0);
        } else {
            g_assert_true(g_file_set_contents(image_path, "not a JPEG", -1,
                                              NULL));
        }
        QTestState *q = start();
        qtest_quit(q);
        g_error("Invalid UVC image was accepted");
    }
    g_test_trap_subprocess(NULL, 5 * G_USEC_PER_SEC, 0);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr("*UVC image*baseline 640x480 JPEG*32768*");
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    int fd = g_file_open_tmp("musb-flash-XXXXXX", &flash_path, NULL);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, 16 * 1024 * 1024), ==, 0);
    close(fd);
    fd = g_file_open_tmp("musb-jpeg-XXXXXX", &image_path, NULL);
    g_assert_cmpint(fd, >=, 0);
    close(fd);
    make_image();
    qtest_add_func("musb/device/disconnected", device_disconnected);
    qtest_add_func("musb/uvc/enumerate", enumerate);
    qtest_add_func("musb/uvc/probe-commit", probe_commit);
    qtest_add_func("musb/uvc/streaming", streaming);
    qtest_add_func("musb/uvc/reset-control", reset_control);
    qtest_add_func("musb/uvc/flush-rx", flush_rx);
    qtest_add_func("musb/uvc/invalid-speed", invalid_speed);
    static const char *invalid[] = {
        "oversize", "progressive", "corrupt", "truncated", "missing", "not-jpeg",
    };
    for (unsigned i = 0; i < ARRAY_SIZE(invalid); i++) {
        g_autofree char *name = g_strdup_printf("musb/uvc/invalid/%s", invalid[i]);
        qtest_add_data_func(name, invalid[i], invalid_image);
    }
    int result = g_test_run();
    unlink(flash_path);
    unlink(image_path);
    g_free(flash_path);
    g_free(image_path);
    g_free(jpeg_data);
    return result;
}
