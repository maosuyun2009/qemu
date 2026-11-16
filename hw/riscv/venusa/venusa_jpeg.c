/* SPDX-License-Identifier: GPL-2.0-or-later */
/* VenusA baseline codec: programmed tables, entropy and MCU pixel FIFOs. */
#include "qemu/osdep.h"
#include "hw/riscv/venusa/venusa_jpeg.h"
#include "hw/irq.h"
#include "qemu/module.h"
#include "qemu/error-report.h"
#include "qemu/bswap.h"
#include <jpeglib.h>
#include <jerror.h>
#include <setjmp.h>

#define R(s, off) ((s)->regs[(off) / 4])
#define SYMBOL 0x2000
#define MINIMUM 0x2800
#define HUFFBASE 0x3000
#define QUANT 0x3800
#define HENC 0x5800

bool venusa_jpeg_encoding(const VenusAJPEG *s)
{
    return !(R(s, 4) & 1);
}

static void access_error(VenusAJPEG *s, hwaddr off, unsigned size,
                         bool write, uint64_t value)
{
    error_report("VenusA JPEG unsupported %s offset=0x%" HWADDR_PRIx
                 " size=%u", write ? "write" : "read", off, size);
    if (s->fault) {
        s->fault(s->fault_opaque, VENUSA_JPEG_BASE + off, size, write, value);
    }
}

static void update(VenusAJPEG *s)
{
    qemu_set_irq(s->irq, !!(R(s, 0x40) & ~R(s, 0x44) & 13));
    if (s->request_changed) {
        s->request_changed(s->fault_opaque);
    }
}

static void put_byte(GByteArray *b, uint8_t value)
{
    g_byte_array_append(b, &value, 1);
}
static void put_word(GByteArray *b, unsigned value)
{
    put_byte(b, value >> 8);
    put_byte(b, value);
}
static void segment(GByteArray *b, unsigned marker, unsigned length)
{
    put_byte(b, 255);
    put_byte(b, marker);
    put_word(b, length);
}

/* The chip stores canonical minimum codes and (symbol index - code) modulo
 * 512, not JPEG DHT counts. Reconstruct those counts without reading guest
 * headers. The last code length has no following minimum; unused slots may
 * be represented by additional unreachable 16-bit codes. */
static bool huffman(VenusAJPEG *s, GByteArray *b, unsigned table)
{
    unsigned dc = table & 1, id = table >> 1;
    unsigned start = dc ? 162 : id ? 174 : 0, capacity = dc ? 12 : 162;
    unsigned code = 0, total = 0;
    uint8_t counts[16];
    uint64_t packed =
        ((uint64_t)R(s, MINIMUM + table * 16 + 12) << 32) | R(s, MINIMUM + table * 16 + 8);
    for (unsigned i = 0; i < 16; i++) {
        unsigned base = R(s, HUFFBASE + table * 64 + i * 4);
        unsigned minimum, count;
        if (i < 8) {
            unsigned shift = 36 - (i + 1) * (i + 2) / 2;
            minimum = (packed >> shift) & ((1u << (i + 1)) - 1);
        } else {
            minimum = (R(s, MINIMUM + table * 16 + (i < 12 ? 4 : 0)) >> ((3 - i % 4) * 8)) & 255;
        }
        if (minimum != (code & (i < 8 ? (1u << (i + 1)) - 1 : 255)) || base > 511 ||
            ((base + code) & 511) != start + total) {
            return false;
        }
        if (i < 15) {
            unsigned next = R(s, HUFFBASE + table * 64 + (i + 1) * 4);
            count = (base - next - code) & 511;
        } else {
            count = MIN(capacity - total, 65535 - code);
        }
        if (count > 255 || total + count > capacity || code + count >= (1u << (i + 1))) {
            return false;
        }
        counts[i] = count;
        total += count;
        code = (code + count) << 1;
    }
    segment(b, 0xc4, 19 + total);
    put_byte(b, (dc ? 0 : 16) | id);
    g_byte_array_append(b, counts, 16);
    for (unsigned i = 0; i < total; i++) {
        unsigned value = R(s, SYMBOL + (start + i) * 4);
        if (value > 255) {
            return false;
        }
        put_byte(b, dc ? (value >> (id * 4)) & 15 : value);
    }
    return true;
}

typedef struct JPEGDecode {
    struct jpeg_decompress_struct codec;
    struct jpeg_error_mgr error;
    jmp_buf jump;
    uint8_t *planes[3];
    bool lookahead;
} JPEGDecode;

static void decode_error(j_common_ptr codec)
{
    JPEGDecode *d = codec->client_data;
    longjmp(d->jump, 1);
}

static void decode_warning(j_common_ptr codec, int level)
{
    JPEGDecode *d = codec->client_data;
    /* ls_jpeg supplies one zero look-ahead byte immediately before EOI. */
    if (level < 0 && !(d->lookahead &&
        codec->err->msg_code == JWRN_EXTRANEOUS_DATA &&
        codec->err->msg_parm.i[0] == 1 &&
        codec->err->msg_parm.i[1] == JPEG_EOI)) {
        decode_error(codec);
    }
}

static bool decode(VenusAJPEG *s)
{
    unsigned width = (R(s, 0x6c) >> 16) & 4095, height = R(s, 0x6c) & 2047;
    unsigned components = (R(s, 0x804) & 3) + 1, format = (R(s, 4) >> 1) & 3;
    unsigned hs = components == 1 ? 1 : format == 2 ? 1 : 2;
    unsigned vs = components == 1 ? 1 : format == 1 ? 2 : 1;
    unsigned columns = DIV_ROUND_UP(width, hs * 8), rows = DIV_ROUND_UP(height, vs * 8);
    unsigned blocks = hs * vs + (components == 3 ? 2 : 0);
    unsigned quant_mask = 0, huff_mask = 0, length = R(s, 0x18);
    g_autoptr(GByteArray) stream = g_byte_array_new();
    bool success = false;
    /* Partial MCUs need explicit edge-block semantics; do not emit dummy
     * plane storage as decoded pixels. The supported 640x480 modes align. */
    if (!width || !height || width > 640 || height > 480 ||
        width % (hs * 8) || height % (vs * 8) ||
        R(s, 0x10) > VENUSA_JPEG_MAX_PIXELS ||
        length < 2 || length > VENUSA_JPEG_MAX_SOURCE ||
        s->input[length - 2] != 255 || s->input[length - 1] != JPEG_EOI ||
        (components != 1 && components != 3) || format == 3 ||
        !(R(s, 4) & 1) || (R(s, 4) & ~7u) || !(R(s, 0x804) & 8) ||
        (R(s, 0x60) & 6) || (R(s, 0x74) & ~1u) || R(s, 0x20) ||
        ((R(s, 0x804) & 4) && R(s, 0x80c) == 65535) ||
        R(s, 0x808) != columns * rows - 1 ||
        R(s, 0x10) != columns * rows * blocks * 64) {
        return false;
    }
    for (unsigned c = 0; c < components; c++) {
        unsigned cfg = R(s, 0x810 + c * 4);
        if ((cfg >> 4) + 1 != (c ? 1 : hs * vs) || ((cfg >> 2) & 3) > 1) {
            return false;
        }
        quant_mask |= 1u << ((cfg >> 2) & 3);
        huff_mask |= (1u << ((cfg & 1) * 2 + 1)) | (1u << (cfg & 2));
    }
    put_byte(stream, 255);
    put_byte(stream, 0xd8);
    for (unsigned q = 0; q < 2; q++) {
        if (!(quant_mask & (1u << q))) {
            continue;
        }
        segment(stream, 0xdb, 131);
        put_byte(stream, 16 | q);
        for (unsigned i = 0; i < 64; i++) {
            unsigned value = R(s, QUANT + (q * 64 + i) * 4);
            if (!value || value > 65535) {
                return false;
            }
            put_word(stream, value);
        }
    }
    for (unsigned table = 0; table < 4; table++) {
        if ((huff_mask & (1u << table)) && !huffman(s, stream, table)) {
            return false;
        }
    }
    segment(stream, 0xc0, 8 + components * 3);
    put_byte(stream, 8);
    put_word(stream, height);
    put_word(stream, width);
    put_byte(stream, components);
    for (unsigned c = 0; c < components; c++) {
        put_byte(stream, c + 1);
        put_byte(stream, c ? 0x11 : (hs << 4) | vs);
        put_byte(stream, (R(s, 0x810 + c * 4) >> 2) & 3);
    }
    if (R(s, 0x804) & 4) {
        segment(stream, 0xdd, 4);
        put_word(stream, R(s, 0x80c) + 1);
    }
    segment(stream, 0xda, 6 + components * 2);
    put_byte(stream, components);
    for (unsigned c = 0; c < components; c++) {
        unsigned cfg = R(s, 0x810 + c * 4);
        put_byte(stream, c + 1);
        put_byte(stream, ((cfg & 1) << 4) | ((cfg >> 1) & 1));
    }
    put_byte(stream, 0);
    put_byte(stream, 63);
    put_byte(stream, 0);
    g_byte_array_append(stream, s->input, R(s, 0x18));
    JPEGDecode *d = g_new0(JPEGDecode, 1);
    d->codec.err = jpeg_std_error(&d->error);
    d->error.error_exit = decode_error;
    d->error.emit_message = decode_warning;
    d->codec.client_data = d;
    d->lookahead = length >= 3 && s->input[length - 3] == 0;
    if (setjmp(d->jump)) {
        goto done;
    }
    jpeg_create_decompress(&d->codec);
    jpeg_mem_src(&d->codec, stream->data, stream->len);
    jpeg_read_header(&d->codec, TRUE);
    d->codec.raw_data_out = TRUE;
    d->codec.dct_method = JDCT_ISLOW;
    jpeg_start_decompress(&d->codec);
    s->output_size = R(s, 0x10);
    s->output = g_malloc0(s->output_size);
    JSAMPROW lines[3][16];
    JSAMPARRAY planes[3] = {lines[0], lines[1], lines[2]};
    for (unsigned c = 0; c < components; c++) {
        unsigned cw = columns * (c ? 1 : hs) * 8, ch = (c ? 1 : vs) * 8;
        d->planes[c] = g_malloc0(cw * ch);
        for (unsigned y = 0; y < ch; y++) {
            lines[c][y] = d->planes[c] + cw * y;
        }
    }
    unsigned offset = 0;
    for (unsigned row = 0; row < rows; row++) {
        if (jpeg_read_raw_data(&d->codec, planes, vs * 8) != vs * 8) {
            goto done;
        }
        for (unsigned col = 0; col < columns; col++) {
            for (unsigned c = 0; c < components; c++) {
                unsigned h = c ? 1 : hs, v = c ? 1 : vs;
                for (unsigned by = 0; by < v; by++) {
                    for (unsigned bx = 0; bx < h; bx++) {
                        for (unsigned y = 0; y < 8; y++) {
                            memcpy(s->output + offset, lines[c][by * 8 + y] + (col * h + bx) * 8,
                                   8);
                            offset += 8;
                        }
                    }
                }
            }
        }
    }
    success = jpeg_finish_decompress(&d->codec);
done:
    jpeg_destroy_decompress(&d->codec);
    for (unsigned c = 0; c < 3; c++) {
        g_free(d->planes[c]);
    }
    g_free(d);
    return success;
}

static const uint8_t natural_order[64] = {
    0,1,8,16,9,2,3,10,17,24,32,25,18,11,4,5,
    12,19,26,33,40,48,41,34,27,20,13,6,7,14,21,28,
    35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,
    58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63,
};

typedef struct JPEGEncode {
    struct jpeg_compress_struct codec;
    struct jpeg_error_mgr error;
    struct jpeg_destination_mgr destination;
    jmp_buf jump;
    uint8_t *stream, *planes[3];
    size_t capacity;
} JPEGEncode;

static void encode_error(j_common_ptr codec)
{
    JPEGEncode *e = codec->client_data;
    longjmp(e->jump, 1);
}

static void encode_warning(j_common_ptr codec, int level)
{
    if (level < 0) {
        encode_error(codec);
    }
}

static void encode_destination_init(j_compress_ptr codec) { }
static void encode_destination_finish(j_compress_ptr codec) { }
static boolean encode_destination_full(j_compress_ptr codec)
{
    encode_error((j_common_ptr)codec);
    return FALSE;
}

static bool encode_huffman(VenusAJPEG *s, JPEGEncode *e, unsigned id, bool ac)
{
    unsigned entries = ac ? 162 : 12, first = ac ? id * 176 : 352 + id * 16;
    unsigned codes[256] = {0}, lengths[256] = {0};
    JHUFF_TBL **dest = ac ? &e->codec.ac_huff_tbl_ptrs[id] : &e->codec.dc_huff_tbl_ptrs[id];
    if (!*dest) {
        *dest = jpeg_alloc_huff_table((j_common_ptr)&e->codec);
    }
    memset(*dest, 0, sizeof(**dest));
    for (unsigned i = 0; i < entries; i++) {
        unsigned word = R(s, HENC + (first + i) * 4);
        if (word == 0xfff) { continue; } /* Unused all-ones code. */
        if (word > 0xfff) { return false; }
        unsigned len = (word >> 8) + 1;
        unsigned code = word & 255;
        if (len > 8) { code |= (1u << len) - 256; }
        if (code >= (1u << len) - 1) { return false; }
        unsigned symbol = ac ? (i < 160 ? ((i / 10) << 4) | (i % 10 + 1) : i == 160 ? 0 : 240) : i;
        lengths[symbol] = len; codes[symbol] = code;
    }
    unsigned next = 0, count = 0;
    for (unsigned len = 1; len <= 16; len++) {
        for (;;) {
            int symbol = -1;
            for (unsigned i = 0; i < 256; i++) {
                if (lengths[i] == len && (symbol < 0 || codes[i] < codes[symbol])) { symbol = i; }
            }
            if (symbol < 0) { break; }
            if (codes[symbol] != next || (*dest)->bits[len] == 255) { return false; }
            next++; lengths[symbol] = 0;
            (*dest)->bits[len]++;
            (*dest)->huffval[count++] = symbol;
        }
        next <<= 1;
    }
    /* libjpeg may emit no bits for an undefined symbol. Require complete
     * baseline alphabets rather than silently producing corrupt entropy. */
    return count == entries;
}

static bool encode(VenusAJPEG *s)
{
    unsigned width = (R(s, 0x68) >> 16) & 4095, height = R(s, 0x68) & 2047;
    unsigned components = (R(s, 0x804) & 3) + 1, format = (R(s, 4) >> 1) & 3;
    unsigned hs = components == 1 || format == 2 ? 1 : 2;
    unsigned vs = components != 1 && format == 1 ? 2 : 1;
    unsigned columns = DIV_ROUND_UP(width, hs * 8), rows = DIV_ROUND_UP(height, vs * 8);
    unsigned blocks = hs * vs + (components == 3 ? 2 : 0);
    /* shortcut: only aligned baseline 422 is calibrated; reject other modes
     * until their pixel-stream layout is verified. */
    if (!width || !height || width > 640 || height > 480 ||
        width % 16 || height % 8 || components != 3 || format != 0 ||
        (R(s, 4) & ~8u) || R(s, 0x804) != 2 || (R(s, 0x60) & ~8u) ||
        (R(s, 0x74) & ~1u) || R(s, 0x20) || R(s, 0x14) ||
        R(s, 0x808) != columns * rows - 1 ||
        R(s, 0x10) != columns * rows * blocks * 64 ||
        R(s, 0x18) != R(s, 0x10)) {
        return false;
    }
    JPEGEncode *e = g_new0(JPEGEncode, 1);
    bool success = false;
    e->codec.err = jpeg_std_error(&e->error);
    e->error.error_exit = encode_error;
    e->error.emit_message = encode_warning;
    e->codec.client_data = e;
    if (setjmp(e->jump)) {
        goto done;
    }
    jpeg_create_compress(&e->codec);
    /* Baseline entropy has a finite worst case including byte stuffing. */
    e->capacity = (size_t)s->received * 8 + 4096;
    e->stream = g_malloc(e->capacity);
    e->destination.init_destination = encode_destination_init;
    e->destination.empty_output_buffer = encode_destination_full;
    e->destination.term_destination = encode_destination_finish;
    e->destination.next_output_byte = e->stream;
    e->destination.free_in_buffer = e->capacity;
    e->codec.dest = &e->destination;
    e->codec.image_width = width;
    e->codec.image_height = height;
    e->codec.input_components = components;
    e->codec.in_color_space = components == 1 ? JCS_GRAYSCALE : JCS_YCbCr;
    jpeg_set_defaults(&e->codec);
    e->codec.raw_data_in = TRUE;
    e->codec.dct_method = JDCT_ISLOW;
    e->codec.optimize_coding = FALSE;
    e->codec.restart_interval = (R(s, 0x804) & 4) ? R(s, 0x80c) + 1 : 0;
    for (unsigned c = 0; c < components; c++) {
        unsigned cfg = R(s, 0x810 + c * 4);
        if ((cfg >> 4) + 1 != (c ? 1 : hs * vs) || ((cfg >> 2) & 3) > 1) { goto done; }
        jpeg_component_info *ci = &e->codec.comp_info[c];
        ci->h_samp_factor = c ? 1 : hs;
        ci->v_samp_factor = c ? 1 : vs;
        ci->quant_tbl_no = (cfg >> 2) & 3;
        ci->dc_tbl_no = cfg & 1;
        ci->ac_tbl_no = (cfg >> 1) & 1;
        if (!encode_huffman(s, e, ci->dc_tbl_no, false) ||
            !encode_huffman(s, e, ci->ac_tbl_no, true)) {
            goto done;
        }

        unsigned q = ci->quant_tbl_no;
        if (!e->codec.quant_tbl_ptrs[q]) {
            e->codec.quant_tbl_ptrs[q] = jpeg_alloc_quant_table((j_common_ptr)&e->codec);
        }
        for (unsigned i = 0; i < 64; i++) {
            unsigned word = R(s, QUANT + (q * 64 + i) * 4);
            unsigned shift = word >> 11, mantissa = word & 2047;
            if (!mantissa || shift > 7) { goto done; }
            /* A reciprocal word of one selects the unity quantizer bypass. */
            unsigned value = word == 1 ? 1 :
                             ((2048u << shift) + mantissa / 2) / mantissa;
            if (!value || value > 255) { goto done; }
            e->codec.quant_tbl_ptrs[q]->quantval[natural_order[i]] = value;
        }
    }

    jpeg_start_compress(&e->codec, TRUE);
    JSAMPROW lines[3][16];
    JSAMPARRAY planes[3] = {lines[0], lines[1], lines[2]};
    for (unsigned c = 0; c < components; c++) {
        unsigned cw = columns * (c ? 1 : hs) * 8, ch = (c ? 1 : vs) * 8;
        e->planes[c] = g_malloc(cw * ch);
        for (unsigned y = 0; y < ch; y++) { lines[c][y] = e->planes[c] + cw * y; }
    }
    unsigned offset = 0;
    for (unsigned row = 0; row < rows; row++) {
        for (unsigned col = 0; col < columns; col++) {
            for (unsigned c = 0; c < components; c++) {
                unsigned h = c ? 1 : hs, v = c ? 1 : vs;
                for (unsigned by = 0; by < v; by++) {
                    for (unsigned bx = 0; bx < h; bx++) {
                        for (unsigned y = 0; y < 8; y++) {
                            memcpy(lines[c][by * 8 + y] + (col * h + bx) * 8, s->input + offset, 8);
                            offset += 8;
                        }
                    }
                }
            }
        }
        if (jpeg_write_raw_data(&e->codec, planes, vs * 8) != vs * 8) { goto done; }
    }
    jpeg_finish_compress(&e->codec);

    size_t length = e->capacity - e->destination.free_in_buffer, pos = 2;
    /* The hardware outputs only ECS. The firmware supplies all markers. */
    while (pos + 4 <= length && e->stream[pos] == 255) {
        unsigned marker = e->stream[pos + 1], size = lduw_be_p(e->stream + pos + 2);
        if (size < 2 || pos + 2 + size > length) { goto done; }
        pos += 2 + size;
        if (marker == 0xda) { break; }
    }
    if (pos >= length - 2 || e->stream[length - 2] != 255 ||
        e->stream[length - 1] != 0xd9) {
        goto done;
    }
    if (length - pos - 2 > 0xffffff) { goto done; }
    R(s, 0x1c) = length - pos - 2;
    s->output_size = ROUND_UP(R(s, 0x1c), 4);
    s->output = g_malloc0(s->output_size);
    memcpy(s->output, e->stream + pos, R(s, 0x1c));
    success = true;
done:
    jpeg_destroy_compress(&e->codec);
    for (unsigned c = 0; c < 3; c++) { g_free(e->planes[c]); }
    g_free(e->stream);
    g_free(e);
    return success;
}

static void cancel(VenusAJPEG *s)
{
    timer_del(s->timer);
    s->pending = s->active = s->decoded = s->failed = false;
    s->received = s->consumed = s->output_size = 0;
    s->remaining_ns = 0;
    s->input_started = s->output_started = false;
    R(s, 0x1c) = 0;
    g_clear_pointer(&s->output, g_free);
}

static void reject(VenusAJPEG *s)
{
    s->failed = true;
    s->pending = false;
    timer_del(s->timer);
    g_clear_pointer(&s->output, g_free);
    s->output_size = 0;
    error_report("VenusA JPEG rejected invalid stream or configuration");
}

bool venusa_jpeg_request(VenusAJPEG *s, bool write)
{
    if (!s->clock || !s->active || s->failed) {
        return false;
    }
    /* SRC_LEN covers the process; EDMA_SIZE describes an individual transfer. */
    return write ? s->input_started && !s->pending &&
                   s->received < ROUND_UP(R(s, 0x18), 4) :
                   s->output_started && s->decoded &&
                   s->consumed < s->output_size;
}

static void complete(void *opaque)
{
    VenusAJPEG *s = opaque;

    if (!s->pending || !s->clock || !s->active) {
        return;
    }
    s->pending = false;
    if (venusa_jpeg_encoding(s) ? encode(s) : decode(s)) {
        if (!venusa_jpeg_encoding(s)) {
            s->decoded_frames++;
        }
        s->decoded = true;
        if (!venusa_jpeg_encoding(s)) {
            R(s, 0x1c) = s->output_size;
        }
        R(s, 0x40) |= venusa_jpeg_encoding(s) ? 1 : 9;
    } else {
        reject(s);
    }
    update(s);
}

void venusa_jpeg_clock(VenusAJPEG *s, bool enabled)
{
    if (s->clock == enabled) {
        return;
    }
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (s->pending) {
        if (enabled) {
            timer_mod(s->timer, now + s->remaining_ns);
        } else {
            s->remaining_ns = MAX(INT64_C(1),
                                 (int64_t)timer_expire_time_ns(s->timer) - now);
            timer_del(s->timer);
        }
    }
    s->clock = enabled;
    update(s);
}

static unsigned register_mask(hwaddr off)
{
    switch (off) {
    case 4: return 31;
    case 8: case 12: case 0x64: case 0x70: case 0x800: return 1;
    case 0x10: case 0x18: case 0x1c: return 0xffffff;
    case 0x14: return 0xfffff;
    case 0x20: return 3;
    case 0x40: case 0x44: return 13;
    case 0x60: case 0x74: case 0x804: return 15;
    case 0x68: case 0x6c: return 0x0ffff7ff;
    case 0x808: return 0x3ffffff;
    case 0x80c: return 0xffff;
    case 0x810: case 0x814: case 0x818: case 0x81c: return 255;
    default:
        if (off >= SYMBOL && off < SYMBOL + 336 * 4) {
            return 255;
        }
        if (off >= MINIMUM && off < MINIMUM + 16 * 4) {
            return UINT32_MAX;
        }
        if (off >= HUFFBASE && off < HUFFBASE + 64 * 4) {
            return 511;
        }
        if (off >= QUANT && off < QUANT + 128 * 4) {
            return 65535;
        }
        if (off >= HENC && off < HENC + 384 * 4) {
            return 0xfff;
        }
        return 0;
    }
}

static uint64_t read_reg(void *opaque, hwaddr local, unsigned size)
{
    VenusAJPEGWindow *w = opaque;
    VenusAJPEG *s = w->jpeg;
    hwaddr off = local + w->offset;

    if (size != 4 || (off & 3) || local > 0x800 - size) {
        access_error(s, off, size, false, 0);
        return 0;
    }
    unsigned output = venusa_jpeg_encoding(s) ? 0x1800 : 0x1000;
    if (off >= output && off < output + 0x800) {
        if (!venusa_jpeg_request(s, false) ||
            s->output_size - s->consumed < 4) {
            access_error(s, off, size, false, 0);
            return 0;
        }
        uint32_t value = ldl_le_p(s->output + s->consumed);
        s->consumed += 4;
        s->output_bytes += MIN(4u, R(s, 0x1c) - (s->consumed - 4));
        if (s->consumed == s->output_size) {
            R(s, 0x40) |= venusa_jpeg_encoding(s) ? 8 : 4;
            g_clear_pointer(&s->output, g_free);
        }
        update(s);
        return value;
    }
    if (!register_mask(off)) {
        access_error(s, off, size, false, 0);
        return 0;
    }
    return R(s, off);
}

static void write_reg(void *opaque, hwaddr local, uint64_t value, unsigned size)
{
    VenusAJPEGWindow *w = opaque;
    VenusAJPEG *s = w->jpeg;
    hwaddr off = local + w->offset;

    if (size != 4 || (off & 3) || local > 0x800 - size) {
        access_error(s, off, size, true, value);
        return;
    }
    unsigned input = venusa_jpeg_encoding(s) ? 0x1000 : 0x1800;
    if (off >= input && off < input + 0x800) {
        if (!venusa_jpeg_request(s, true) ||
            s->received > sizeof(s->input) - 4) {
            access_error(s, off, size, true, value);
            return;
        }
        stl_le_p(s->input + s->received, value);
        s->received += 4;
        if (s->received == ROUND_UP(R(s, 0x18), 4)) {
            s->pending = true;
            /* Bounded functional latency, not silicon-cycle timing. */
            s->remaining_ns = 1000 + R(s, 0x10);
            timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                               s->remaining_ns);
        }
        update(s);
        return;
    }
    unsigned mask = register_mask(off);
    if (!mask || (value & ~((uint64_t)mask)) || off == 0x1c) {
        access_error(s, off, size, true, value);
        return;
    }
    if (off == 8 || off == 12 || off == 0x64 || off == 0x70) {
        if (value && off == 12) {
            if (venusa_jpeg_encoding(s)) { s->output_started = true; }
            else { s->input_started = true; }
        }
        if (value && (off == 8 || off == 0x64)) {
            if (venusa_jpeg_encoding(s)) { s->input_started = true; }
            else { s->output_started = true; }
        }
        update(s);
        return;
    }
    if (s->active && off != 0x40 && off != 0x44 && off != 0x74 &&
        off != 0x800) {
        access_error(s, off, size, true, value);
        return;
    }
    if ((off == 0x800 || off == 0x74) && !(value & 1)) {
        cancel(s);
    }
    if (off == 0x40) {
        R(s, off) &= ~value;
    } else {
        R(s, off) = value;
    }
    if ((R(s, 0x800) & R(s, 0x74) & 1) && !s->active) {
        s->active = true;
        if (!R(s, 0x18) ||
            R(s, 0x18) > (venusa_jpeg_encoding(s) ?
                         VENUSA_JPEG_MAX_PIXELS : VENUSA_JPEG_MAX_SOURCE) ||
            (venusa_jpeg_encoding(s) &&
             (R(s, 0x18) != R(s, 0x10) || (R(s, 0x18) & 3))) ||
            !R(s, 0x10) || R(s, 0x10) > VENUSA_JPEG_MAX_PIXELS) {
            reject(s);
        }
    }
    update(s);
}

static const MemoryRegionOps ops = {
    .read = read_reg,
    .write = write_reg,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
};

void venusa_jpeg_reset(VenusAJPEG *s)
{
    cancel(s);
    memset(s->regs, 0, sizeof(s->regs));
    update(s);
}

static void reset(DeviceState *dev)
{
    venusa_jpeg_reset(VENUSA_JPEG(dev));
}

static void instance_init(Object *obj)
{
    VenusAJPEG *s = VENUSA_JPEG(obj);

    s->clock = true;
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, complete, s);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq);
    /* Separate 2 KiB windows preserve TCG subpage dispatch alignment. */
    for (unsigned i = 0; i < G_N_ELEMENTS(s->windows); i++) {
        VenusAJPEGWindow *w = &s->windows[i];
        g_autofree char *name = g_strdup_printf("venusa-jpeg-%u", i);
        w->jpeg = s;
        w->offset = i == 8 ? HENC : i * 0x800;
        memory_region_init_io(&w->io, obj, &ops, w, name, 0x800);
        sysbus_init_mmio(SYS_BUS_DEVICE(s), &w->io);
    }
}

static void class_init(ObjectClass *oc, const void *data)
{
    device_class_set_legacy_reset(DEVICE_CLASS(oc), reset);
}

static void finalize(Object *obj)
{
    VenusAJPEG *s = VENUSA_JPEG(obj);
    timer_free(s->timer);
    g_free(s->output);
}

static const TypeInfo jpeg_type = {
    .name = TYPE_VENUSA_JPEG,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(VenusAJPEG),
    .instance_init = instance_init,
    .class_init = class_init,
    .instance_finalize = finalize,
};

static void register_types(void)
{
    type_register_static(&jpeg_type);
}
type_init(register_types)
