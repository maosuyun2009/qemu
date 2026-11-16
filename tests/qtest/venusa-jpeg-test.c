/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qapi/error.h"
#include "qobject/qdict.h"
#include "qobject/qjson.h"
#include <jpeglib.h>

#define JPEG 0x42001800
static char *flash;

static QTestState *start(void)
{
    g_autofree char *path = g_shell_quote(flash);
    return qtest_initf("-machine venusa-rd-evb "
                       "-drive if=pflash,format=raw,file=%s", path);
}

static void registers(void)
{
    QTestState *q = start();

    g_assert_cmphex(qtest_readl(q, JPEG + 0x74), ==, 0);
    qtest_writel(q, JPEG + 0x74, 2);
    g_assert_cmphex(qtest_readl(q, JPEG + 0x74), ==, 2);
    qtest_writel(q, JPEG + 0x5800 + 0x20, 0x7a0);
    g_assert_cmphex(qtest_readl(q, JPEG + 0x5800 + 0x20), ==, 0x7a0);
    qtest_writel(q, JPEG + 0x804, 10);
    qtest_writel(q, JPEG + 0x44, 13);
    qtest_writel(q, 0x43100014, 1u << 24);
    g_assert_cmphex(qtest_readl(q, JPEG + 0x74), ==, 0);
    g_assert_cmphex(qtest_readl(q, JPEG + 0x804), ==, 0);
    g_assert_cmphex(qtest_readl(q, JPEG + 0x40), ==, 0);
    qtest_quit(q);
}

/* Program the hardware tables from a real baseline JPEG, as the driver does. */
static void configure(QTestState *q, unsigned width, unsigned height,
                      unsigned sampling, bool patterned, uint8_t **data,
                      unsigned long *length)
{
    struct jpeg_compress_struct c = {0};
    struct jpeg_error_mgr error;
    uint8_t row[640 * 3];
    JSAMPROW lines[] = {row};
    uint32_t symbols[336] = {0};

    c.err = jpeg_std_error(&error);
    jpeg_create_compress(&c);
    jpeg_mem_dest(&c, data, length);
    c.image_width = width;
    c.image_height = height;
    c.input_components = 3;
    c.in_color_space = JCS_YCbCr;
    jpeg_set_defaults(&c);
    c.comp_info[0].h_samp_factor = sampling ? 2 : 1;
    c.comp_info[0].v_samp_factor = sampling == 2 ? 2 : 1;
    for (unsigned i = 0; i < width; i++) {
        row[i * 3] = 81;
        row[i * 3 + 1] = 90;
        row[i * 3 + 2] = 240;
    }
    jpeg_start_compress(&c, true);
    while (c.next_scanline < height) {
        if (patterned) {
            for (unsigned x = 0; x < width; x++) {
                row[x * 3] = 30 + (x * 7 + c.next_scanline * 3) % 180;
                row[x * 3 + 1] = 60 + x * 2 % 120;
                row[x * 3 + 2] = 80 + c.next_scanline * 4 % 120;
            }
        }
        jpeg_write_scanlines(&c, lines, 1);
    }
    jpeg_finish_compress(&c);
    static const uint8_t zigzag[] = {
         0, 1, 8,16, 9, 2, 3,10,17,24,32,25,18,11, 4, 5,
        12,19,26,33,40,48,41,34,27,20,13, 6, 7,14,21,28,
        35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,
        58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63
    };
    for (unsigned id = 0; id < 2; id++) {
        for (unsigned i = 0; i < 64; i++) {
            qtest_writel(q, JPEG + 0x3800 + (id * 64 + i) * 4,
                         c.quant_tbl_ptrs[id]->quantval[zigzag[i]]);
        }
        for (unsigned dc = 0; dc < 2; dc++) {
            JHUFF_TBL *t = dc ? c.dc_huff_tbl_ptrs[id] : c.ac_huff_tbl_ptrs[id];
            unsigned table = id * 2 + dc;
            unsigned code = 0, n = 0, base = dc ? 162 : id ? 174 : 0;
            uint32_t minimum[16], packed[4] = {0};
            for (unsigned i = 0; i < 16; i++) {
                minimum[i] = code;
                qtest_writel(q, JPEG + 0x3000 + (table * 16 + i) * 4,
                             (base + n - code) & 511);
                for (unsigned j = 0; j < t->bits[i + 1]; j++, n++) {
                    if (dc) {
                        symbols[base + n] |= t->huffval[n] << (id * 4);
                    } else {
                        symbols[base + n] = t->huffval[n];
                    }
                }
                code = (code + t->bits[i + 1]) << 1;
            }
            uint64_t bits = 0;
            for (unsigned i = 0; i < 8; i++) {
                bits = bits << (i + 1) | (minimum[i] & ((1u << (i + 1)) - 1));
            }
            packed[2] = bits;
            packed[3] = bits >> 32;
            for (unsigned i = 8; i < 16; i++) {
                unsigned word = i < 12 ? 1 : 0;
                packed[word] = packed[word] << 8 | (minimum[i] & 255);
            }
            for (unsigned i = 0; i < 4; i++) {
                qtest_writel(q, JPEG + 0x2800 + (table * 4 + i) * 4, packed[i]);
            }
        }
    }
    for (unsigned i = 0; i < 336; i++) {
        qtest_writel(q, JPEG + 0x2000 + i * 4, symbols[i]);
    }
    jpeg_destroy_compress(&c);
    qtest_writel(q, JPEG + 4, sampling == 2 ? 3 : sampling ? 1 : 5);
    qtest_writel(q, JPEG + 0x804, 10);
    qtest_writel(q, JPEG + 0x808,
                 DIV_ROUND_UP(width, sampling ? 16 : 8) *
                 DIV_ROUND_UP(height, sampling == 2 ? 16 : 8) - 1);
    qtest_writel(q, JPEG + 0x810, sampling == 2 ? 0x30 : sampling ? 0x10 : 0);
    qtest_writel(q, JPEG + 0x814, 7);
    qtest_writel(q, JPEG + 0x818, 7);
    qtest_writel(q, JPEG + 0x6c, width << 16 | height);
    qtest_writel(q, JPEG + 0x10, DIV_ROUND_UP(width, sampling ? 16 : 8) *
                 DIV_ROUND_UP(height, sampling == 2 ? 16 : 8) *
                 (sampling == 2 ? 6 : sampling ? 4 : 3) * 64);
}

static unsigned entropy_offset(const uint8_t *data, unsigned length)
{
    for (unsigned i = 2; i + 4 < length;) {
        g_assert_cmphex(data[i], ==, 255);
        unsigned size = data[i + 2] * 256 + data[i + 3];
        if (data[i + 1] == 0xda) {
            return i + 2 + size;
        }
        i += 2 + size;
    }
    g_assert_not_reached();
}

static void feed(QTestState *q, const uint8_t *ecs, unsigned size)
{
    qtest_writel(q, 0x43100034, qtest_readl(q, 0x43100034) | (1u << 8));
    qtest_writel(q, JPEG + 0x18, size);
    qtest_writel(q, JPEG + 0x14, size);
    qtest_writel(q, JPEG + 0x44, 13);
    qtest_writel(q, JPEG + 0x800, 1);
    qtest_writel(q, JPEG + 8, 1);
    qtest_writel(q, JPEG + 12, 1);
    qtest_writel(q, JPEG + 0x74, 1);
    for (unsigned i = 0; i < ROUND_UP(size, 4); i += 4) {
        uint32_t value = 0;
        for (unsigned j = 0; j < 4 && i + j < size; j++) {
            value |= (uint32_t)ecs[i + j] << (j * 8);
        }
        /* Exercise the last complete word in the FIFO alias window. */
        qtest_writel(q, 0x420037fc, value);
    }
    g_assert_cmphex(qtest_readl(q, JPEG + 0x40), ==, 0);
    g_assert_cmpuint(qtest_readl(q, JPEG + 0x1c), ==, 0);
    qtest_clock_step(q, 1);
    g_assert_cmphex(qtest_readl(q, JPEG + 0x40), ==, 0);
}

/* Independent oracle: decode the original JPEG, then address each MCU block. */
static void check_pixels(QTestState *q, const uint8_t *data, unsigned long size,
                         bool patterned)
{
    struct jpeg_decompress_struct d = {0};
    struct jpeg_error_mgr error;
    JSAMPARRAY planes[3];
    unsigned offset = 0;

    d.err = jpeg_std_error(&error);
    jpeg_create_decompress(&d);
    jpeg_mem_src(&d, data, size);
    jpeg_read_header(&d, true);
    d.raw_data_out = true;
    d.dct_method = JDCT_ISLOW;
    jpeg_start_decompress(&d);
    for (unsigned c = 0; c < 3; c++) {
        jpeg_component_info *ci = &d.comp_info[c];
        planes[c] = (*d.mem->alloc_sarray)((j_common_ptr)&d, JPOOL_IMAGE,
                         ROUND_UP(ci->width_in_blocks, ci->h_samp_factor) * 8,
                         ci->v_samp_factor * 8);
    }
    while (d.output_scanline < d.output_height) {
        g_assert_cmpuint(jpeg_read_raw_data(&d, planes,
                                          d.max_v_samp_factor * 8), >, 0);
        for (unsigned m = 0; m < d.MCUs_per_row; m++) {
            for (unsigned c = 0; c < 3; c++) {
                jpeg_component_info *ci = &d.comp_info[c];
                for (unsigned block = 0;
                     block < ci->h_samp_factor * ci->v_samp_factor; block++) {
                    for (unsigned y = 0; y < 8; y++) {
                        for (unsigned x = 0; x < 8; x += 4) {
                            unsigned by = block / ci->h_samp_factor * 8 + y;
                            unsigned bx = (m * ci->h_samp_factor +
                                           block % ci->h_samp_factor) * 8 + x;
                            uint32_t value = qtest_readl(q, 0x42002ffc);
                            for (unsigned j = 0; j < 4; j++) {
                                unsigned actual = (value >> (j * 8)) & 255;
                                g_assert_cmpuint(actual, ==, planes[c][by][bx + j]);
                                if (!patterned) {
                                    const int known[] = {81, 90, 240};
                                    g_assert_cmpint(abs((int)actual - known[c]), <=, 2);
                                }
                            }
                            offset += 4;
                        }
                    }
                }
            }
        }
    }
    g_assert_cmpuint(offset, ==, qtest_readl(q, JPEG + 0x10));
    jpeg_finish_decompress(&d);
    jpeg_destroy_decompress(&d);
}

static void decode(gconstpointer opaque)
{
    unsigned sampling = GPOINTER_TO_UINT(opaque) % 3;
    bool patterned = GPOINTER_TO_UINT(opaque) >= 3;
    QTestState *q = start();
    uint8_t *data = NULL;
    unsigned long length = 0;

    configure(q, patterned ? 32 : 16, 16, sampling, patterned, &data, &length);
    unsigned offset = entropy_offset(data, length), size = length - offset;
    if (GPOINTER_TO_UINT(opaque) >= 6) {
        g_autofree uint8_t *ecs = g_malloc(size + 1);
        memcpy(ecs, data + offset, size - 2);
        ecs[size - 2] = 0; /* Firmware look-ahead byte, inside SRC_LEN. */
        ecs[size - 1] = 255;
        ecs[size] = 0xd9;
        feed(q, ecs, size + 1);
    } else {
        feed(q, data + offset, size);
    }
    qtest_clock_step(q, 10000000);
    g_assert_cmphex(qtest_readl(q, JPEG + 0x40), ==, 9);
    g_assert_cmpuint(qtest_readl(q, JPEG + 0x1c), ==,
                     qtest_readl(q, JPEG + 0x10));
    g_assert_cmpuint(qtest_readb(q, 0xe0021000 + 61 * 4), ==, 0);
    qtest_writel(q, JPEG + 0x44, 4);
    g_assert_cmpuint(qtest_readb(q, 0xe0021000 + 61 * 4), ==, 1);
    check_pixels(q, data, length, patterned);
    g_autoptr(QDict) reply = qtest_qmp(q, "{'execute':'qom-get',"
        "'arguments':{'path':'/machine','property':'x-lisa-snapshot'}}");
    QObject *snapshot = qobject_from_json(qdict_get_str(reply, "return"), &error_abort);
    QDict *jpeg = qdict_get_qdict(qobject_to(QDict, snapshot), "jpeg");
    g_assert_nonnull(jpeg);
    g_assert_cmpuint(qdict_get_int(jpeg, "decoded_frames"), ==, 1);
    g_assert_cmpuint(qdict_get_int(jpeg, "output_bytes"), ==,
                     qtest_readl(q, JPEG + 0x10));
    qobject_unref(snapshot);
    g_assert_cmphex(qtest_readl(q, JPEG + 0x40), ==, 13);
    qtest_writel(q, JPEG + 0x40, 13);
    g_assert_cmpuint(qtest_readb(q, 0xe0021000 + 61 * 4), ==, 0);
    free(data);
    qtest_quit(q);
}

static void rejected(gconstpointer opaque)
{
    QTestState *q = start();
    uint8_t *data = NULL;
    unsigned long length = 0;
    unsigned which = GPOINTER_TO_UINT(opaque);

    configure(q, which == 5 ? 17 : 16, which == 6 ? 17 : 16,
              2, false, &data, &length);
    unsigned offset = entropy_offset(data, length), size = length - offset;
    if (which >= 5) {
        qtest_writel(q, JPEG + 0x10, 768); /* Complete padded MCU storage. */
    } else if (which == 0) {
        size -= 2; /* Missing EOI must not be repaired by libjpeg. */
    } else if (which == 1) {
        data[offset] = 255;
        data[offset + 1] = 0xd9; /* Premature EOI. */
    } else if (which == 2) {
        qtest_writel(q, JPEG + 0x10, 64); /* Wrong output length. */
    } else if (which == 3) {
        qtest_writel(q, JPEG + 0x6c, 641u << 16 | 16);
    } else {
        qtest_writel(q, JPEG + 0x3800, 0); /* Invalid quantizer. */
    }
    feed(q, data + offset, size);
    qtest_writel(q, JPEG + 0x44, 0);
    qtest_clock_step(q, 10000000);
    g_assert_cmphex(qtest_readl(q, JPEG + 0x40), ==, 0);
    g_assert_cmpuint(qtest_readl(q, JPEG + 0x1c), ==, 0);
    g_assert_cmpuint(qtest_readb(q, 0xe0021000 + 61 * 4), ==, 0);
    free(data);
    qtest_quit(q);
}

static void cancellation(gconstpointer opaque)
{
    QTestState *q = start();
    uint8_t *data = NULL;
    unsigned long length = 0;
    unsigned which = GPOINTER_TO_UINT(opaque);

    configure(q, 16, 16, 2, false, &data, &length);
    unsigned offset = entropy_offset(data, length);
    feed(q, data + offset, length - offset);
    qtest_writel(q, JPEG + 0x44, 0);
    uint32_t clocks = qtest_readl(q, 0x43100034);
    qtest_writel(q, 0x43100034, clocks & ~(1u << 8));
    qtest_clock_step(q, 10000000);
    g_assert_cmphex(qtest_readl(q, JPEG + 0x40), ==, 0);
    g_assert_cmpuint(qtest_readl(q, JPEG + 0x1c), ==, 0);
    if (which == 1) {
        qtest_writel(q, 0x43100014, 1u << 24);
    } else if (which == 2) {
        qtest_writel(q, JPEG + 0x800, 0);
    }
    qtest_writel(q, 0x43100034, clocks);
    g_assert_cmphex(qtest_readl(q, JPEG + 0x40), ==, 0);
    qtest_clock_step(q, 10000000);
    g_assert_cmphex(qtest_readl(q, JPEG + 0x40), ==, which ? 0 : 9);
    g_assert_cmpuint(qtest_readb(q, 0xe0021000 + 61 * 4), ==, which ? 0 : 1);
    free(data);
    qtest_quit(q);
}

static void repeated_frames(void)
{
    QTestState *q = start();

    for (unsigned frame = 0; frame < 3; frame++) {
        uint8_t *data = NULL;
        unsigned long length = 0;
        qtest_writel(q, 0x43100014, 1u << 24);
        configure(q, 32, 16, frame % 3, true, &data, &length);
        unsigned offset = entropy_offset(data, length);
        feed(q, data + offset, length - offset);
        qtest_clock_step(q, 10000000);
        g_assert_cmphex(qtest_readl(q, JPEG + 0x40), ==, 9);
        check_pixels(q, data, length, true);
        g_assert_cmphex(qtest_readl(q, JPEG + 0x40), ==, 13);
        free(data);
    }
    qtest_quit(q);
}

static void dma_raster(void)
{
    const uint32_t dma = 0x44900000, src = 0x20010000, dst = 0x20012000;
    enum { width = 32, height = 16, bytes = width * height * 2 };
    QTestState *q = start();
    uint8_t *data = NULL, actual[bytes], row[width * 3];
    unsigned long length = 0;
    struct jpeg_decompress_struct d = {0};
    struct jpeg_error_mgr error;
    JSAMPROW line[] = {row};

    configure(q, width, height, 1, true, &data, &length);
    unsigned offset = entropy_offset(data, length), size = length - offset;
    g_autofree uint8_t *ecs = g_malloc0(ROUND_UP(size, 4));
    memcpy(ecs, data + offset, size);
    qtest_memwrite(q, src, ecs, ROUND_UP(size, 4));
    qtest_memset(q, dst, 0xa5, bytes + 4);
    qtest_writel(q, 0x43100034, qtest_readl(q, 0x43100034) | 0x110);
    qtest_writel(q, 0x431000a0, qtest_readl(q, 0x431000a0) & ~0x00300030u);
    qtest_writel(q, JPEG + 0x18, size);
    qtest_writel(q, JPEG + 0x14, size);
    qtest_writel(q, JPEG + 0x44, 0);
    qtest_writel(q, JPEG + 0x800, 1);
    qtest_writel(q, JPEG + 8, 1);
    qtest_writel(q, JPEG + 12, 1);
    qtest_writel(q, JPEG + 0x74, 1);
    qtest_writel(q, dma + 0x184, 1);
    /* jpeg_img_format_codec2dma_switch copies input geometry/format to output. */
    qtest_writel(q, dma + 0x108, 0x33);
    qtest_writel(q, dma + 0x128, width | height << 16);
    qtest_writel(q, dma + 0x148, width | height << 16);
    qtest_writel(q, dma + 0x168, width * 2 | (width * 2) << 16);
    qtest_writel(q, dma + 0xa8, 0x42002800);
    qtest_writel(q, dma + 0xc8, dst);
    qtest_writel(q, dma + 0xe8, bytes);
    qtest_writel(q, dma + 8, 1 | (2 << 3) | (2 << 5) | (2 << 8) |
                             (1 << 23) | (5u << 28));
    qtest_writel(q, dma + 0x208, 1);
    qtest_writel(q, dma + 0xa4, src);
    qtest_writel(q, dma + 0xc4, 0x42003000);
    qtest_writel(q, dma + 0xe4, ROUND_UP(size, 4));
    qtest_writel(q, dma + 4, 1 | (1 << 1) | (2 << 3) | (2 << 5) |
                             (2 << 10) | (4u << 28));
    qtest_writel(q, dma + 0x204, 1);
    g_assert_cmphex(qtest_readl(q, JPEG + 0x40), ==, 0);
    g_assert_cmphex(qtest_readl(q, dst), ==, 0xa5a5a5a5);
    qtest_clock_step(q, 100000000);
    g_assert_cmphex(qtest_readl(q, dma + 0x284) & 1, ==, 1);
    g_assert_cmphex(qtest_readl(q, dma + 0x288) & 1, ==, 1);
    g_assert_cmphex(qtest_readl(q, JPEG + 0x40), ==, 13);
    qtest_memread(q, dst, actual, bytes);
    g_assert_cmphex(qtest_readl(q, dst + bytes), ==, 0xa5a5a5a5);
    d.err = jpeg_std_error(&error);
    jpeg_create_decompress(&d);
    jpeg_mem_src(&d, data, length);
    jpeg_read_header(&d, true);
    d.out_color_space = JCS_YCbCr;
    d.do_fancy_upsampling = false;
    d.dct_method = JDCT_ISLOW;
    jpeg_start_decompress(&d);
    for (unsigned y = 0; y < height; y++) {
        g_assert_cmpuint(jpeg_read_scanlines(&d, line, 1), ==, 1);
        for (unsigned x = 0; x < width; x += 2) {
            const uint8_t expected[] = {row[x * 3], row[x * 3 + 1],
                                       row[(x + 1) * 3], row[x * 3 + 2]};
            g_assert_cmpmem(actual + (y * width + x) * 2, 4, expected, 4);
        }
    }
    jpeg_finish_decompress(&d);
    jpeg_destroy_decompress(&d);
    free(data);
    qtest_quit(q);
}

/* Encode fixtures use deliberately non-default canonical tables. The full
 * reference JPEG supplies only the firmware-owned headers to the decoder. */
typedef struct EncodeFixture {
    uint8_t *jpeg;
    unsigned long size;
    unsigned width, height, bytes;
    bool standard;
    uint8_t raster[32 * 16 * 2], mcu[32 * 16 * 2];
} EncodeFixture;

static void configure_encoder(QTestState *q, EncodeFixture *f, unsigned quant)
{
    struct jpeg_compress_struct c = {0};
    struct jpeg_error_mgr error;
    uint8_t samples[3][32 * 16];
    JSAMPROW lines[3][8];
    JSAMPARRAY planes[] = {lines[0], lines[1], lines[2]};
    unsigned at = 0;

    f->bytes = f->width * f->height * 2;
    c.err = jpeg_std_error(&error);
    jpeg_create_compress(&c);
    jpeg_mem_dest(&c, &f->jpeg, &f->size);
    c.image_width = f->width;
    c.image_height = f->height;
    c.input_components = 3;
    c.in_color_space = JCS_YCbCr;
    jpeg_set_defaults(&c);
    c.comp_info[0].h_samp_factor = 2;
    c.comp_info[0].v_samp_factor = 1;
    c.raw_data_in = true;
    c.dct_method = JDCT_ISLOW;
    for (unsigned id = 0; id < 2; id++) {
        for (unsigned i = 0; i < 64; i++) {
            unsigned v = quant + i % 4;
            c.quant_tbl_ptrs[id]->quantval[i] = v;
        }
        JHUFF_TBL *dc = c.dc_huff_tbl_ptrs[id], *ac = c.ac_huff_tbl_ptrs[id];
        if (!f->standard) {
            memset(dc, 0, sizeof(*dc));
            memset(ac, 0, sizeof(*ac));
            dc->bits[4] = 12;
            ac->bits[8] = 162;
            for (unsigned i = 0; i < 12; i++) {
                dc->huffval[i] = i;
            }
            for (unsigned i = 0; i < 162; i++) {
                ac->huffval[i] = i < 160 ? (i / 10) * 16 + i % 10 + 1 : i == 160 ? 0 : 240;
            }
        }
        for (unsigned kind = 0; kind < 2; kind++) {
            JHUFF_TBL *t = kind ? ac : dc;
            unsigned code = 0, n = 0;
            for (unsigned len = 1; len <= 16; len++) {
                for (unsigned j = 0; j < t->bits[len]; j++, n++, code++) {
                    unsigned symbol = t->huffval[n];
                    unsigned index = kind ? (symbol == 0 ? 160 : symbol == 240 ? 161 :
                                     (symbol >> 4) * 10 + (symbol & 15) - 1) : symbol;
                    unsigned first = kind ? id * 176 : 352 + id * 16;
                    qtest_writel(q, JPEG + 0x5800 + (first + index) * 4,
                                 (len - 1) << 8 | (code & 255));
                }
                code <<= 1;
            }
        }
    }
    static const unsigned zigzag[64] = {
        0,1,8,16,9,2,3,10,17,24,32,25,18,11,4,5,
        12,19,26,33,40,48,41,34,27,20,13,6,7,14,21,28,
        35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,
        58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63
    };
    for (unsigned id = 0; id < 2; id++) {
        for (unsigned i = 0; i < 64; i++) {
            unsigned v = c.quant_tbl_ptrs[id]->quantval[zigzag[i]];
            /* Firmware reciprocal representation, including the q=1 bypass. */
            static const unsigned reciprocal[] = {
                0, 1, 0x400, 0x2ab, 0x200, 0xb33, 0x155, 0xa49,
                0x100, 0x9c7, 0xcd, 0x12e9, 0x955, 0x93b, 0x1249, 0x911,
                0x80, 0x8f1, 0x11c7, 0x11af
            };
            g_assert_cmpuint(v, <, G_N_ELEMENTS(reciprocal));
            qtest_writel(q, JPEG + 0x3800 + (id * 64 + i) * 4, reciprocal[v]);
        }
    }
    for (unsigned y = 0; y < f->height; y++) {
        for (unsigned x = 0; x < f->width; x++) {
            samples[0][y * f->width + x] = 48 + x * 3 + y * 2;
            f->raster[(y * f->width + x) * 2] = samples[0][y * f->width + x];
            if (!(x & 1)) {
                unsigned pos = y * f->width / 2 + x / 2;
                samples[1][pos] = 80 + x * 2;
                samples[2][pos] = 160 + y * 2;
                f->raster[(y * f->width + x) * 2 + 1] = samples[1][pos];
                f->raster[(y * f->width + x) * 2 + 3] = samples[2][pos];
            }
        }
    }
    jpeg_start_compress(&c, true);
    for (unsigned my = 0; my < f->height; my += 8) {
        for (unsigned component = 0; component < 3; component++) {
            for (unsigned y = 0; y < 8; y++) {
                unsigned stride = component ? f->width / 2 : f->width;
                lines[component][y] = samples[component] + (my + y) * stride;
            }
        }
        g_assert_cmpuint(jpeg_write_raw_data(&c, planes, 8), ==, 8);
        for (unsigned mx = 0; mx < f->width; mx += 16) {
            for (unsigned block = 0; block < 4; block++) {
                unsigned component = block < 2 ? 0 : block - 1;
                unsigned x = block < 2 ? mx + block * 8 : mx / 2;
                for (unsigned y = 0; y < 8; y++) {
                    memcpy(f->mcu + at, lines[component][y] + x, 8);
                    at += 8;
                }
            }
        }
    }
    jpeg_finish_compress(&c);
    jpeg_destroy_compress(&c);
    qtest_writel(q, JPEG + 4, 8); /* SDK leaves decoder output format set. */
    qtest_writel(q, JPEG + 0x10, f->bytes);
    qtest_writel(q, JPEG + 0x18, f->bytes);
    qtest_writel(q, JPEG + 0x68, f->width << 16 | f->height);
    qtest_writel(q, JPEG + 0x804, 2);
    qtest_writel(q, JPEG + 0x808, f->bytes / 256 - 1);
    qtest_writel(q, JPEG + 0x810, 0x10);
    qtest_writel(q, JPEG + 0x814, 7);
    qtest_writel(q, JPEG + 0x818, 7);
    qtest_writel(q, 0x43100034, qtest_readl(q, 0x43100034) | 0x110);
    qtest_writel(q, JPEG + 0x44, 5); /* Only EDMA_DONE is unmasked. */
    for (unsigned i = 0; i < 6; i++) {
        const unsigned starts[] = {0x800, 0x70, 8, 0x64, 12, 0x74};
        qtest_writel(q, JPEG + starts[i], 1);
    }
}

static void encoder_feed(QTestState *q, const EncodeFixture *f)
{
    for (unsigned i = 0; i < f->bytes; i += 4) {
        if (i == f->bytes - 4) {
            qtest_clock_step(q, 1000000);
            g_assert_cmpuint(qtest_readl(q, JPEG + 0x1c), ==, 0);
            g_assert_cmphex(qtest_readl(q, JPEG + 0x40), ==, 0);
        }
        uint32_t v = f->mcu[i] | f->mcu[i + 1] << 8 |
                     f->mcu[i + 2] << 16 | (uint32_t)f->mcu[i + 3] << 24;
        qtest_writel(q, 0x42002ffc, v);
    }
    g_assert_cmphex(qtest_readl(q, JPEG + 0x40), ==, 0);
    g_assert_cmpuint(qtest_readl(q, JPEG + 0x1c), ==, 0);
    qtest_clock_step(q, 1);
    g_assert_cmpuint(qtest_readl(q, JPEG + 0x1c), ==, 0);
}

static void check_encoded(const EncodeFixture *f, const uint8_t *ecs, unsigned size)
{
    unsigned header = entropy_offset(f->jpeg, f->size);
    g_assert_cmpmem(ecs, size, f->jpeg + header, f->size - header - 2);
    g_autofree uint8_t *jpeg = g_malloc(header + size + 2);
    memcpy(jpeg, f->jpeg, header);
    memcpy(jpeg + header, ecs, size);
    jpeg[header + size] = 255;
    jpeg[header + size + 1] = 0xd9;
    struct jpeg_decompress_struct d = {0};
    struct jpeg_error_mgr error;
    uint8_t row[32 * 3];
    JSAMPROW line[] = {row};
    d.err = jpeg_std_error(&error);
    jpeg_create_decompress(&d);
    jpeg_mem_src(&d, jpeg, header + size + 2);
    jpeg_read_header(&d, true);
    d.out_color_space = JCS_YCbCr;
    d.do_fancy_upsampling = false;
    jpeg_start_decompress(&d);
    while (d.output_scanline < d.output_height) {
        unsigned y = d.output_scanline;
        jpeg_read_scanlines(&d, line, 1);
        for (unsigned x = 0; x < f->width; x++) {
            unsigned pos = (y * f->width + x) * 2;
            g_assert_cmpint(abs(row[x * 3] - f->raster[pos]), <=, 5);
            g_assert_cmpint(abs(row[x * 3 + 1] - f->raster[pos - (x & 1) * 2 + 1]), <=, 5);
            g_assert_cmpint(abs(row[x * 3 + 2] - f->raster[pos - (x & 1) * 2 + 3]), <=, 5);
        }
    }
    g_assert_cmpint(d.err->num_warnings, ==, 0);
    jpeg_finish_decompress(&d);
    jpeg_destroy_decompress(&d);
}

static void encode_pio(gconstpointer opaque)
{
    unsigned quant = GPOINTER_TO_UINT(opaque) & 255;
    QTestState *q = start();
    EncodeFixture f = {.width = 16, .height = 8,
                       .standard = !!(GPOINTER_TO_UINT(opaque) & 256)};
    configure_encoder(q, &f, quant);
    encoder_feed(q, &f);
    qtest_clock_step(q, 1000000);
    g_assert_cmphex(qtest_readl(q, JPEG + 0x40), ==, 1);
    g_assert_cmpuint(qtest_readb(q, 0xe0021000 + 61 * 4), ==, 0);
    unsigned size = qtest_readl(q, JPEG + 0x1c);
    g_test_message("ECS length: %u unpadded bytes", size);
    g_assert_cmpuint(size, >, 0);
    g_autofree uint8_t *ecs = g_malloc(ROUND_UP(size, 4));
    for (unsigned i = 0; i < ROUND_UP(size, 4); i += 4) {
        g_assert_cmphex(qtest_readl(q, JPEG + 0x40), ==, 1);
        uint32_t v = qtest_readl(q, 0x420037fc);
        for (unsigned j = 0; j < 4; j++) { ecs[i + j] = v >> (j * 8); }
    }
    g_assert_cmphex(qtest_readl(q, JPEG + 0x40), ==, 9);
    g_assert_cmpuint(qtest_readb(q, 0xe0021000 + 61 * 4), ==, 1);
    check_encoded(&f, ecs, size);
    qtest_writel(q, JPEG + 0x40, 9);
    g_assert_cmpuint(qtest_readb(q, 0xe0021000 + 61 * 4), ==, 0);
    free(f.jpeg);
    qtest_quit(q);
}

static void queue_encoder_output(QTestState *q, uint32_t dst, unsigned capacity)
{
    const uint32_t dma = 0x44900000;
    qtest_writel(q, dma + 0xa4, 0x42003000);
    qtest_writel(q, dma + 0xc4, dst);
    qtest_writel(q, dma + 0xe4, capacity);
    qtest_writel(q, dma + 4, 1 | (2 << 3) | (2 << 5) | (2 << 8) |
                             (2 << 15) | (1 << 17) | (4u << 28));
    qtest_writel(q, dma + 0x204, 1);
}

static void encode_dma(gconstpointer opaque)
{
    const uint32_t dma = 0x44900000, src = 0x20010004, dst = 0x20012004;
    enum { capacity = 4096 };
    unsigned width = GPOINTER_TO_UINT(opaque);
    EncodeFixture f = {.width = width, .height = width / 2};
    QTestState *q = start();
    configure_encoder(q, &f, 8);
    qtest_writel(q, 0x431000a0, qtest_readl(q, 0x431000a0) & ~0x00300030u);
    qtest_memset(q, src - 4, 0xa5, f.bytes + 8);
    qtest_memwrite(q, src, f.raster, f.bytes);
    qtest_memset(q, dst - 4, 0xa5, capacity + 8);
    if (width == 32) {
        /* Firmware arms the output capacity before starting pixel input. */
        queue_encoder_output(q, dst, capacity);
    }
    qtest_writel(q, dma + 0x184, 2); /* JPEG encode, YUYV. */
    qtest_writel(q, dma + 0x108, 0x33);
    qtest_writel(q, dma + 0x128, f.width | f.height << 16);
    qtest_writel(q, dma + 0x148, f.width | f.height << 16);
    qtest_writel(q, dma + 0x168, f.width * 2 | f.width * 2 << 16);
    qtest_writel(q, dma + 0xa8, src);
    qtest_writel(q, dma + 0xc8, 0x42002800);
    qtest_writel(q, dma + 0xe8, f.bytes);
    qtest_writel(q, dma + 8, 1 | (1 << 1) | (2 << 3) | (2 << 5) |
                             (2 << 10) | (2 << 15) | (1 << 17) |
                             (1 << 23) | (5u << 28));
    qtest_writel(q, dma + 0x208, 1);
    g_assert_cmpuint(qtest_readl(q, JPEG + 0x1c), ==, 0);
    g_assert_cmphex(qtest_readl(q, dst), ==, 0xa5a5a5a5);
    qtest_clock_step(q, 10000000);
    g_assert_cmphex(qtest_readl(q, dma + 0x288) & 1, ==, 1);
    g_assert_cmphex(qtest_readl(q, JPEG + 0x40), ==, width == 32 ? 9 : 1);
    g_assert_cmpuint(qtest_readb(q, 0xe0021000 + 61 * 4), ==, width == 32);
    unsigned size = qtest_readl(q, JPEG + 0x1c), padded = ROUND_UP(size, 4);
    g_assert_cmpuint(size, >, 0);
    g_assert_cmpuint(padded, <, capacity);
    if (width == 16) {
        /* Output capacity is not the ECS length. Gate the codec while DMA waits. */
        uint32_t clocks = qtest_readl(q, 0x43100034);
        qtest_writel(q, 0x43100034, clocks & ~0x100u);
        queue_encoder_output(q, dst, capacity);
        qtest_clock_step(q, 10000000);
        g_assert_cmphex(qtest_readl(q, dst), ==, 0xa5a5a5a5);
        g_assert_cmphex(qtest_readl(q, JPEG + 0x40), ==, 1);
        qtest_writel(q, 0x43100034, clocks);
        qtest_clock_step(q, 10000000);
    }
    g_assert_cmphex(qtest_readl(q, JPEG + 0x40), ==, 9);
    g_assert_cmpuint(qtest_readb(q, 0xe0021000 + 61 * 4), ==, 1);
    g_assert_cmphex(qtest_readl(q, dma + 0x284) & 1, ==, 0);
    g_assert_cmpuint(qtest_readl(q, dma + 0x324), ==, capacity - padded);
    g_autofree uint8_t *ecs = g_malloc(capacity);
    qtest_memread(q, dst, ecs, capacity);
    check_encoded(&f, ecs, size);
    for (unsigned i = padded; i < capacity; i++) { g_assert_cmphex(ecs[i], ==, 0xa5); }
    g_assert_cmphex(qtest_readl(q, src - 4), ==, 0xa5a5a5a5);
    g_assert_cmphex(qtest_readl(q, src + f.bytes), ==, 0xa5a5a5a5);
    g_assert_cmphex(qtest_readl(q, dst - 4), ==, 0xa5a5a5a5);
    g_assert_cmphex(qtest_readl(q, dst + capacity), ==, 0xa5a5a5a5);
    qtest_writel(q, dma + 0x360, 2); /* Firmware stops the capacity transfer. */
    qtest_writel(q, JPEG + 0x800, 0);
    qtest_clock_step(q, 10000000);
    g_assert_cmphex(qtest_readl(q, dst + padded), ==, 0xa5a5a5a5);
    free(f.jpeg);
    qtest_quit(q);
}

static void encode_suspend(gconstpointer opaque)
{
    unsigned which = GPOINTER_TO_UINT(opaque);
    EncodeFixture f = {.width = 16, .height = 8};
    QTestState *q = start();
    configure_encoder(q, &f, 8);
    encoder_feed(q, &f);
    uint32_t clocks = qtest_readl(q, 0x43100034);
    qtest_writel(q, 0x43100034, clocks & ~0x100u);
    qtest_clock_step(q, 1000000);
    g_assert_cmphex(qtest_readl(q, JPEG + 0x40), ==, 0);
    g_assert_cmpuint(qtest_readl(q, JPEG + 0x1c), ==, 0);
    if (which == 1) {
        qtest_writel(q, 0x43100014, 1u << 24);
        g_assert_cmphex(qtest_readl(q, JPEG + 0x5800 + 352 * 4), ==, 0);
        g_assert_cmphex(qtest_readl(q, JPEG + 0x3800), ==, 0);
    } else if (which == 2) {
        qtest_writel(q, JPEG + 0x800, 0);
        qtest_writel(q, JPEG + 0x74, 0);
        g_assert_cmphex(qtest_readl(q, JPEG + 0x5800 + 352 * 4), ==, 0x300);
        g_assert_cmphex(qtest_readl(q, JPEG + 0x3800), ==, 0x100);
    }
    qtest_writel(q, 0x43100034, clocks);
    /* Functional latency is 1000 + pixel bytes; one ns elapsed before gating. */
    qtest_clock_step(q, 1254);
    g_assert_cmpuint(qtest_readl(q, JPEG + 0x1c), ==, 0);
    qtest_clock_step(q, 1);
    g_assert_cmphex(qtest_readl(q, JPEG + 0x40), ==, which ? 0 : 1);
    g_assert_cmpuint(qtest_readl(q, JPEG + 0x1c), ==,
                     which ? 0 : f.size - entropy_offset(f.jpeg, f.size) - 2);
    g_assert_cmpuint(qtest_readb(q, 0xe0021000 + 61 * 4), ==, 0);
    if (which == 2) {
        /* Stopping cancels pending work but retains the programmed tables. */
        const unsigned starts[] = {0x800, 8, 12, 0x74};
        for (unsigned i = 0; i < G_N_ELEMENTS(starts); i++) {
            qtest_writel(q, JPEG + starts[i], 1);
        }
        encoder_feed(q, &f);
        qtest_clock_step(q, 1000000);
        g_assert_cmphex(qtest_readl(q, JPEG + 0x40), ==, 1);
    }
    free(f.jpeg);
    qtest_quit(q);
}

static void encode_reject(gconstpointer opaque)
{
    unsigned which = GPOINTER_TO_UINT(opaque);
    EncodeFixture f = {.width = 16, .height = 8};
    QTestState *q = start();
    configure_encoder(q, &f, 8);
    qtest_writel(q, JPEG + 0x800, 0);
    qtest_writel(q, JPEG + 0x74, 0);
    switch (which) {
    case 0: qtest_writel(q, JPEG + 0x3800, 0); break;
    case 1: qtest_writel(q, JPEG + 0x5800 + 352 * 4, 0xfff); break;
    case 2: qtest_writel(q, JPEG + 0x5800 + 352 * 4, 0x301); break;
    case 3: qtest_writel(q, JPEG + 4, 2); break; /* 420 not calibrated. */
    case 4: qtest_writel(q, JPEG + 0x68, 17u << 16 | 8); break;
    case 5: qtest_writel(q, JPEG + 0x808, 1); break;
    case 6: qtest_writel(q, JPEG + 0x804, 0); break; /* Grayscale. */
    case 7: qtest_writel(q, JPEG + 4, 16); break; /* Mirror. */
    case 8: qtest_writel(q, JPEG + 0x3800, 0x3801); break;
    case 9: qtest_writel(q, JPEG + 0x68, 656u << 16 | 8); break;
    case 10: qtest_writel(q, JPEG + 0x68, 16u << 16 | 9); break;
    case 11: qtest_writel(q, JPEG + 0x804, 6); break; /* Restart markers. */
    default: g_assert_not_reached();
    }
    qtest_writel(q, JPEG + 0x800, 1);
    qtest_writel(q, JPEG + 8, 1);
    qtest_writel(q, JPEG + 12, 1);
    qtest_writel(q, JPEG + 0x74, 1);
    encoder_feed(q, &f);
    qtest_clock_step(q, 1000000);
    g_assert_cmphex(qtest_readl(q, JPEG + 0x40), ==, 0);
    g_assert_cmpuint(qtest_readl(q, JPEG + 0x1c), ==, 0);
    g_assert_cmpuint(qtest_readb(q, 0xe0021000 + 61 * 4), ==, 0);
    free(f.jpeg);
    qtest_quit(q);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    int fd = g_file_open_tmp("venusa-jpeg-flash-XXXXXX", &flash, NULL);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, 16 * 1024 * 1024), ==, 0);
    close(fd);
    qtest_add_func("/venusa/jpeg/registers", registers);
    for (unsigned i = 0; i < 3; i++) {
        g_autofree char *name = g_strdup_printf("/venusa/jpeg/encode-suspend-%u", i);
        qtest_add_data_func(name, GUINT_TO_POINTER(i), encode_suspend);
    }
    for (unsigned i = 0; i < 12; i++) {
        g_autofree char *name = g_strdup_printf("/venusa/jpeg/encode-reject-%u", i);
        qtest_add_data_func(name, GUINT_TO_POINTER(i), encode_reject);
    }
    qtest_add_data_func("/venusa/jpeg/encode-pio-q1", GUINT_TO_POINTER(1), encode_pio);
    qtest_add_data_func("/venusa/jpeg/encode-pio-standard-huffman", GUINT_TO_POINTER(264), encode_pio);
    qtest_add_data_func("/venusa/jpeg/encode-dma-16x8", GUINT_TO_POINTER(16), encode_dma);
    qtest_add_data_func("/venusa/jpeg/encode-dma-32x16", GUINT_TO_POINTER(32), encode_dma);
    qtest_add_data_func("/venusa/jpeg/encode-pio-q8", GUINT_TO_POINTER(8), encode_pio);
    qtest_add_data_func("/venusa/jpeg/encode-pio-q16", GUINT_TO_POINTER(16), encode_pio);
    for (unsigned i = 0; i < 6; i++) {
        g_autofree char *name = g_strdup_printf("/venusa/jpeg/decode-%u", i);
        qtest_add_data_func(name, GUINT_TO_POINTER(i), decode);
    }
    for (unsigned i = 0; i < 7; i++) {
        g_autofree char *name = g_strdup_printf("/venusa/jpeg/rejected-%u", i);
        qtest_add_data_func(name, GUINT_TO_POINTER(i), rejected);
    }
    qtest_add_data_func("/venusa/jpeg/clock-pause", GUINT_TO_POINTER(0), cancellation);
    qtest_add_data_func("/venusa/jpeg/reset-cancel", GUINT_TO_POINTER(1), cancellation);
    qtest_add_data_func("/venusa/jpeg/stop-cancel", GUINT_TO_POINTER(2), cancellation);
    qtest_add_data_func("/venusa/jpeg/firmware-lookahead", GUINT_TO_POINTER(7), decode);
    qtest_add_func("/venusa/jpeg/dma-422-raster", dma_raster);
    qtest_add_func("/venusa/jpeg/repeated-frames", repeated_frames);
    int ret = g_test_run();
    unlink(flash);
    g_free(flash);
    return ret;
}
