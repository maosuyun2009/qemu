/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Six-channel VenusA DMA2D generation: normal transfers and shadow queue. */
#include "qemu/osdep.h"
#include "hw/dma/venusa_dma2d.h"
#include "system/address-spaces.h"
#include "qemu/host-utils.h"

static void fail(VenusADMA2D *s, hwaddr off, bool write, uint32_t v)
{
    s->cb.fail(s->cb.opaque, s->base + off, 4, write, v);
}
static uint32_t status(VenusADMA2D *s, unsigned ch)
{
    uint32_t mask = (s->regs[(0x240 + ch * 4) / 4] & ~1u) |
                    s->channels[ch].working.block_mask;
    return s->channels[ch].raw & ~mask & 31;
}
static void irq(VenusADMA2D *s, unsigned ch)
{
    s->cb.irq(s->cb.opaque, ch,
              status(s, ch) && s->channels[ch].working.irq_enable);
}
static void activate(VenusADMA2D *s, unsigned ch)
{
    VenusADMA2DChannel *c = &s->channels[ch];
    if (!c->active && c->pending && (s->regs[ch] & 1)) {
        c->working = c->queued;
        c->transferred = 0;
        c->packed_left = c->packed_pos = 0;
        c->image_pending_len = c->image_pending_pos = 0;
        c->image_stream_len = 0;
        c->image_pixels = c->image_in_bytes = c->image_out_bytes = 0;
        c->src_cursor = c->working.src;
        c->dst_cursor = c->working.dst;
        c->pending = false;
        c->active = true;
        c->raw |= 4;
        irq(s, ch);
    }
}
static bool ready(VenusADMA2D *s, unsigned ch)
{
    VenusADMA2DChannel *c = &s->channels[ch];
    if (!c->active || !(s->regs[ch] & 1)) { return false; }
    unsigned mode = (c->working.control >> 1) & 3;
    return mode == 2 || s->cb.request(s->cb.opaque, c->working.control >> 28,
                                     mode == 0 ? c->src_cursor : c->dst_cursor,
                                     mode != 0);
}
static Int128 multiply(uint64_t a, uint64_t b)
{
    uint64_t lo, hi;
    mulu64(&lo, &hi, a, b);
    return int128_make128(lo, hi);
}
static void sync_clock(VenusADMA2D *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (s->clock_enabled && s->clock_n && !int128_eq(s->clock_work, int128_zero())) {
        s->clock_work = int128_sub(s->clock_work,
                                  multiply(now - s->event_epoch, s->clock_n));
    }
    s->event_epoch = now;
}
static void arm(VenusADMA2D *s)
{
    Int128 work = int128_add(s->clock_work, int128_make64(s->clock_n - 1));
    /* Late CPU/timer dispatch can leave signed service debt; clamp before conversion. */
    Int128 wait = int128_max(int128_one(), int128_divs(work, int128_make64(s->clock_n)));
    if (!int128_le(wait, int128_make64(INT64_MAX - s->event_epoch))) {
        fail(s, 0, true, 0); return;
    }
    timer_mod(s->event, s->event_epoch + int128_get64(wait));
}
void venusa_dma2d_request(VenusADMA2D *s)
{
    if (s->servicing || !s->clock_enabled || !s->clock_n || timer_pending(s->event)) {
        return;
    }
    for (unsigned i = 0; i < VENUSA_DMA2D_CHANNELS; i++) {
        activate(s, i);
        if (ready(s, i)) {
            if (int128_eq(s->clock_work, int128_zero())) {
                s->clock_work = multiply(UINT64_C(2000000000), s->clock_d);
            }
            s->event_epoch = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            arm(s);
            return;
        }
    }
}
static uint32_t next_address(uint32_t a, unsigned mode, unsigned bytes)
{
    return mode == 0 ? a + bytes : mode == 1 ? a - bytes : a;
}

static uint64_t normal_address(uint32_t base, unsigned offset,
                               unsigned width, unsigned mode,
                               uint32_t group, uint32_t gap)
{
    if (group) { return (uint64_t)base + offset + (uint64_t)(offset / group) * gap; }
    uint64_t step = offset / width * width;
    return mode == 0 ? (uint64_t)base + offset :
           mode == 1 ? (uint64_t)base - step + offset % width :
                       (uint64_t)base + offset % width;
}

static bool normal_region_ok(VenusADMA2D *s, uint32_t base, unsigned len,
                             unsigned width, unsigned mode, uint32_t group,
                             uint32_t gap, bool write)
{
    uint64_t first = mode == 1 ? (uint64_t)base - (len - 1) / width * width : base;
    uint64_t end = mode == 0 ? (uint64_t)base + len :
                              (uint64_t)base + MIN(len, width);
    if (group) { end += (uint64_t)((len - 1) / group) * gap; }
    return first < end && end <= UINT64_C(0x100000000) &&
           end - first <= UINT32_MAX &&
           s->cb.memory_allowed(s->cb.opaque, first, end - first, write);
}

/* One output beat packs/unpacks source units without padding the byte count. */
static bool normal_beat(VenusADMA2D *s, unsigned ch)
{
    VenusADMA2DChannel *c = &s->channels[ch];
    VenusADMA2DDescriptor *w = &c->working;
    unsigned sw = 1u << ((w->control >> 3) & 3);
    unsigned dw = 1u << ((w->control >> 5) & 3);
    unsigned sm = (w->control >> 8) & 3, dm = (w->control >> 10) & 3;
    unsigned sg = (w->control & (1u << 22)) ? w->sg_num : 0;
    unsigned ds = (w->control & (1u << 21)) ? w->ds_num : 0;
    unsigned n = MIN(dw, w->length - c->transferred);
    uint8_t bytes[4];
    for (unsigned j = 0; j < n; j++) {
        if (!c->packed_left) {
            unsigned offset = c->transferred + j;
            unsigned count = MIN(sw, w->length - offset);
            hwaddr src = normal_address(w->src, offset, sw, sm, sg, w->sg_itv);
            if (address_space_read(&address_space_memory, src,
                    MEMTXATTRS_UNSPECIFIED, c->packed, count) != MEMTX_OK) {
                fail(s, 0xa0 + ch * 4, true, w->src); return false;
            }
            c->packed_left = count;
            c->packed_pos = 0;
            c->src_cursor = normal_address(w->src, offset + count, sw, sm, sg, w->sg_itv);
        }
        bytes[j] = c->packed[c->packed_pos++];
        c->packed_left--;
    }
    hwaddr dst = normal_address(w->dst, c->transferred, dw, dm, ds, w->ds_itv);
    if (address_space_write(&address_space_memory, dst, MEMTXATTRS_UNSPECIFIED,
                            bytes, n) != MEMTX_OK) {
        fail(s, 0xc0 + ch * 4, true, w->dst); return false;
    }
    c->transferred += n;
    c->dst_cursor = normal_address(w->dst, c->transferred, dw, dm, ds, w->ds_itv);
    return true;
}

typedef struct VenusAPixel {
    uint32_t r, g, b, y;
} VenusAPixel;

static unsigned merge_factor(unsigned code)
{
    switch (code) {
    case 0: return 1;
    case 1: return 2;
    case 2: return 4;
    case 4: return 8;
    case 8: return 16;
    default: return 0;
    }
}

static uint8_t image_component(int value)
{
    /* Shared functional model: full-range BT.601, signed floor at Q8. */
    return MIN(255, MAX(0, value >= 0 ? value / 256 : -((-value + 255) / 256)));
}

static bool scaler_address_ok(VenusADMA2D *s, hwaddr address, unsigned size,
                              bool write)
{
    uint64_t end = address + size;
    return size && end <= UINT64_C(0x100000000) &&
           s->cb.memory_allowed(s->cb.opaque, address, size, write);
}

static bool scaler_disjoint(uint32_t src, uint64_t in_size,
                            uint32_t dst, uint64_t out_size)
{
    hwaddr si, di, sn = in_size, dn = out_size;
    MemoryRegion *sm = address_space_translate(&address_space_memory, src,
                                              &si, &sn, false, MEMTXATTRS_UNSPECIFIED);
    MemoryRegion *dm = address_space_translate(&address_space_memory, dst,
                                              &di, &dn, true, MEMTXATTRS_UNSPECIFIED);
    if (sn < in_size || dn < out_size ||
        !(memory_region_is_ram(sm) || memory_region_is_romd(sm)) ||
        !memory_region_is_ram(dm)) {
        return false;
    }
    /* Alias windows can have different guest addresses but identical backing. */
    uintptr_t sp = (uintptr_t)memory_region_get_ram_ptr(sm) + si;
    uintptr_t dp = (uintptr_t)memory_region_get_ram_ptr(dm) + di;
    return sp + in_size <= dp || dp + out_size <= sp;
}

static bool scaler_read_pixel(VenusADMA2D *s, const VenusADMA2DDescriptor *w,
                              unsigned x, unsigned y, VenusAPixel *p)
{
    uint8_t data[4];
    unsigned pair;
    hwaddr address;
    if (x >= w->in_width || y >= w->in_height) {
        return false;
    }
    if (w->in_format == 4) {
        address = (hwaddr)w->src + (hwaddr)y * w->in_stride + x;
        if (!scaler_address_ok(s, address, 1, false) ||
            address_space_read(&address_space_memory, address,
                               MEMTXATTRS_UNSPECIFIED, data, 1) != MEMTX_OK) {
            return false;
        }
        p->r = p->g = p->b = p->y = data[0];
        return true;
    }
    if (w->in_format == 0) {
        address = (hwaddr)w->src + (hwaddr)y * w->in_stride + x * 3;
        if (!scaler_address_ok(s, address, 3, false) ||
            address_space_read(&address_space_memory, address,
                               MEMTXATTRS_UNSPECIFIED, data, 3) != MEMTX_OK) {
            return false;
        }
        if (w->in_rgb) {
            p->r = data[0]; p->g = data[1]; p->b = data[2];
        } else {
            p->b = data[0]; p->g = data[1]; p->r = data[2];
        }
        p->y = (77 * p->r + 150 * p->g + 29 * p->b + 128) >> 8;
        return true;
    }
    if (w->in_format != 3) {
        return false;
    }
    pair = x & ~1u;
    address = (hwaddr)w->src + (hwaddr)y * w->in_stride + pair * 2;
    if (!scaler_address_ok(s, address, 4, false) ||
        address_space_read(&address_space_memory, address,
                           MEMTXATTRS_UNSPECIFIED, data, 4) != MEMTX_OK) {
        return false;
    }
    unsigned yi = x & 1 ? 2 : 0, ui = 1, vi = 3;
    switch (w->in_yuv) {
    case 1: yi = x & 1 ? 3 : 1; ui = 0; vi = 2; break; /* UYVY */
    case 2: yi = x & 1 ? 2 : 0; ui = 3; vi = 1; break; /* YVYU */
    case 3: yi = x & 1 ? 3 : 1; ui = 2; vi = 0; break; /* VYUY */
    default: break;
    }
    /* Filter native Y/U/V components before conversion and saturation. */
    p->r = p->y = data[yi];
    p->g = data[ui];
    p->b = data[vi];
    return true;
}

static bool scaler_sample(VenusADMA2D *s, const VenusADMA2DDescriptor *w,
                          unsigned x, unsigned y, VenusAPixel *out)
{
    unsigned mx = w->merge_x, my = w->merge_y;
    uint64_t count = (uint64_t)mx * my;
    VenusAPixel p;
    memset(out, 0, sizeof(*out));
    for (unsigned yy = 0; yy < my; yy++) {
        for (unsigned xx = 0; xx < mx; xx++) {
            if (!scaler_read_pixel(s, w, x * mx + xx, y * my + yy, &p)) {
                return false;
            }
            out->r += p.r; out->g += p.g; out->b += p.b; out->y += p.y;
        }
    }
    /* Merge is an arithmetic box average rounded to nearest. */
    out->r = (out->r + count / 2) / count;
    out->g = (out->g + count / 2) / count;
    out->b = (out->b + count / 2) / count;
    out->y = (out->y + count / 2) / count;
    return true;
}

static bool scaler_pixel(VenusADMA2D *s, const VenusADMA2DDescriptor *w,
                         unsigned ox, unsigned oy, VenusAPixel *out)
{
    uint64_t px = (uint64_t)w->start_x + (uint64_t)ox * w->phase_x;
    uint64_t py = (uint64_t)w->start_y + (uint64_t)oy * w->phase_y;
    unsigned x = px >> 12, y = py >> 12, fx = px & 0xfff, fy = py & 0xfff;
    unsigned mw = w->in_width / w->merge_x, mh = w->in_height / w->merge_y;
    VenusAPixel a, b, c, d;
    if (x >= mw || y >= mh) { return false; }
    if (x + 1 >= mw && fx && !w->last_tile) { return false; }
    if (y + 1 >= mh && fy && !w->last_band) { return false; }
    if (x + 1 >= mw) { x = mw - 1; fx = 0; }
    if (y + 1 >= mh) { y = mh - 1; fy = 0; }
    if (!scaler_sample(s, w, x, y, &a)) { return false; }
    b = c = d = a;
    if (fx && !scaler_sample(s, w, x + 1, y, &b)) { return false; }
    if (fy && !scaler_sample(s, w, x, y + 1, &c)) { return false; }
    if (fx && fy) {
        if (!scaler_sample(s, w, x + 1, y + 1, &d)) { return false; }
    } else {
        d = fx ? b : c;
    }
    uint64_t wx = fx, wy = fy, iw = 4096 - wx, ih = 4096 - wy;
#define INTERP(field) ((a.field * iw * ih + b.field * wx * ih + \
                        c.field * iw * wy + d.field * wx * wy + 0x800000) >> 24)
    out->r = INTERP(r); out->g = INTERP(g); out->b = INTERP(b); out->y = INTERP(y);
#undef INTERP
    return true;
}

static void scaler_encode_pixel(const VenusADMA2DDescriptor *w,
                                const VenusAPixel *p, uint8_t data[3])
{
    if (w->out_format == 4) {
        data[0] = p->y;
    } else if (w->out_format == 0) {
        uint8_t r = p->r, g = p->g, b = p->b;
        if (w->in_format == 3) {
            int y = p->r * 256, u = (int)p->g - 128, v = (int)p->b - 128;
            r = image_component(y + 359 * v);
            g = image_component(y - 183 * v - 88 * u);
            b = image_component(y + 444 * u);
        }
        data[0] = w->out_rgb ? r : b;
        data[1] = g;
        data[2] = w->out_rgb ? b : r;
    } else {
        g_assert_not_reached();
    }
}

static bool scaler_transfer(VenusADMA2D *s, unsigned ch, unsigned beats)
{
    VenusADMA2DChannel *c = &s->channels[ch];
    VenusADMA2DDescriptor *w = &c->working;
    unsigned out_bpp = w->out_format == 4 ? 1 : 3;
    unsigned budget = beats * (1u << ((w->control >> 5) & 3));
    while (budget && c->transferred < w->length) {
        unsigned pixel = c->transferred / out_bpp, part = c->transferred % out_bpp;
        unsigned x = pixel % w->out_width, y = pixel / w->out_width;
        unsigned n = MIN(out_bpp - part, budget);
        hwaddr dst = (hwaddr)w->dst + (hwaddr)y * w->out_stride + x * out_bpp + part;
        if (!part) {
            VenusAPixel p;
            if (!scaler_pixel(s, w, x, y, &p)) {
                fail(s, 0xa0 + ch * 4, true, w->src); return false;
            }
            scaler_encode_pixel(w, &p, c->scaler_pixel);
        }
        if (!scaler_address_ok(s, dst, n, true) ||
            address_space_write(&address_space_memory, dst, MEMTXATTRS_UNSPECIFIED,
                                c->scaler_pixel + part, n) != MEMTX_OK) {
            fail(s, 0xc0 + ch * 4, true, w->dst); return false;
        }
        c->transferred += n;
        c->dst_cursor = dst + n;
        budget -= n;
    }
    if (c->transferred == w->length) {
        c->active = false;
        c->raw |= 1;
        activate(s, ch);
        irq(s, ch);
    }
    return true;
}

static void image_rgb_to_yuv(const VenusAPixel *p, uint8_t *y, uint8_t *u,
                             uint8_t *v)
{
    /* Full-range BT.601 at Q8, rounded before saturation and pair averaging. */
    int r = p->r, g = p->g, b = p->b;
    *y = image_component(77 * r + 150 * g + 29 * b + 128);
    *u = image_component(32768 - 43 * r - 85 * g + 128 * b + 128);
    *v = image_component(32768 + 128 * r - 107 * g - 21 * b + 128);
}

static void image_done(VenusADMA2D *s, unsigned ch)
{
    VenusADMA2DChannel *c = &s->channels[ch];
    c->active = false;
    c->raw |= 1;
    activate(s, ch);
    irq(s, ch);
}

static bool image_write_pending(VenusADMA2D *s, unsigned ch, unsigned *budget)
{
    VenusADMA2DChannel *c = &s->channels[ch];
    unsigned left = c->image_pending_len - c->image_pending_pos;
    unsigned n = MIN(left, *budget);
    hwaddr dst = c->image_pending_dst + c->image_pending_pos;
    if (!n) {
        return true;
    }
    if (!scaler_address_ok(s, dst, n, true) ||
        address_space_write(&address_space_memory, dst, MEMTXATTRS_UNSPECIFIED,
                            c->image_pending + c->image_pending_pos, n) != MEMTX_OK) {
        fail(s, 0xc0 + ch * 4, true, c->working.dst);
        return false;
    }
    c->image_pending_pos += n;
    c->image_out_bytes += n;
    *budget -= n;
    if (c->image_pending_pos == c->image_pending_len) {
        c->image_pending_len = c->image_pending_pos = 0;
    }
    return true;
}

static bool image_transfer(VenusADMA2D *s, unsigned ch, unsigned beats)
{
    VenusADMA2DChannel *c = &s->channels[ch];
    VenusADMA2DDescriptor *w = &c->working;
    unsigned mode = (w->control >> 1) & 3;
    unsigned width = 1u << ((w->control >> 3) & 3);
    unsigned budget = beats * width;
    unsigned pixels = (unsigned)w->out_width * w->out_height;

    if (mode == 2) {
        while (budget && c->active && (c->image_pending_len || c->image_pixels < pixels)) {
            if (!c->image_pending_len) {
                VenusAPixel p;
                unsigned x = c->image_pixels % w->out_width;
                unsigned y = c->image_pixels / w->out_width;
                if (!scaler_read_pixel(s, w, x, y, &p)) {
                    fail(s, 0xa0 + ch * 4, true, w->src);
                    return false;
                }
                scaler_encode_pixel(w, &p, c->image_pending);
                c->image_pending_len = 3;
                c->image_pending_pos = 0;
                c->image_pending_dst = (hwaddr)w->dst +
                    (hwaddr)y * w->out_stride + x * 3;
                c->image_pixels++;
                c->image_in_bytes += 2;
            }
            if (!image_write_pending(s, ch, &budget)) {
                return false;
            }
        }
        c->transferred = c->image_in_bytes;
    } else {
        /* P2M RGB888 is the only FIFO conversion supported. Keep source
         * words separate from output pixels: a 4-byte grant splits RGB data. */
        for (unsigned b = 0; b < beats && c->active && c->image_pixels < pixels; b++) {
            if (!ready(s, ch)) {
                break;
            }
            if (c->image_stream_len + width > sizeof(c->image_stream) ||
                address_space_read(&address_space_memory, c->src_cursor,
                    MEMTXATTRS_UNSPECIFIED, c->image_stream + c->image_stream_len,
                    width) != MEMTX_OK) {
                fail(s, 0xa0 + ch * 4, true, w->src);
                return false;
            }
            c->image_stream_len += width;
            c->image_in_bytes += width;
            c->transferred = c->image_in_bytes;
            c->src_cursor = next_address(c->src_cursor,
                                         (w->control >> 8) & 3, width);
            while (c->image_stream_len >= 6 && c->image_pixels + 2 <= pixels) {
                VenusAPixel p0 = { c->image_stream[0], c->image_stream[1],
                                   c->image_stream[2], 0 };
                VenusAPixel p1 = { c->image_stream[3], c->image_stream[4],
                                   c->image_stream[5], 0 };
                uint8_t y0, y1, u0, u1, v0, v1, data[4];
                image_rgb_to_yuv(&p0, &y0, &u0, &v0);
                image_rgb_to_yuv(&p1, &y1, &u1, &v1);
                data[0] = y0; data[1] = (u0 + u1 + 1) / 2;
                data[2] = y1; data[3] = (v0 + v1 + 1) / 2;
                unsigned x = c->image_pixels % w->out_width;
                unsigned y = c->image_pixels / w->out_width;
                hwaddr dst = (hwaddr)w->dst + (hwaddr)y * w->out_stride + x * 2;
                if (!scaler_address_ok(s, dst, sizeof(data), true) ||
                    address_space_write(&address_space_memory, dst,
                        MEMTXATTRS_UNSPECIFIED, data, sizeof(data)) != MEMTX_OK) {
                    fail(s, 0xc0 + ch * 4, true, w->dst);
                    return false;
                }
                c->image_pixels += 2;
                c->image_out_bytes += sizeof(data);
                c->image_stream_len -= 6;
                memmove(c->image_stream, c->image_stream + 6,
                        c->image_stream_len);
            }
        }
    }
    if (c->image_pixels == pixels && !c->image_pending_len) {
        image_done(s, ch);
    }
    return true;
}
/* JPEG channel 2 maps Y0/Y1/Cb/Cr MCU blocks into a raster YUYV image.
 * Input bytes still traverse the peripheral FIFO and the DMA clocked grants. */
static hwaddr jpeg_raster_address(const VenusADMA2DDescriptor *w,
                                  uint32_t base, uint32_t offset)
{
    unsigned mcu = offset / 256, block = (offset % 256) / 64;
    unsigned sample = offset % 64, row = sample / 8, col = sample % 8;
    unsigned x = (mcu % (w->jpeg_width / 16)) * 16;
    unsigned y = (mcu / (w->jpeg_width / 16)) * 8 + row;
    unsigned byte = block < 2 ? (col + block * 8) * 2 :
                               col * 4 + (block == 2 ? 1 : 3);
    return (hwaddr)base + y * w->jpeg_stride + x * 2 + byte;
}

static void transfer(void *opaque)
{
    VenusADMA2D *s = opaque;
    if (!s->clock_enabled) { return; }
    sync_clock(s);
    s->servicing = true;
    /* One granted burst owns the bus. Zero is the highest priority. */
    unsigned chosen = VENUSA_DMA2D_CHANNELS, priority = 4;
    for (unsigned ch = 0; ch < VENUSA_DMA2D_CHANNELS; ch++) {
        unsigned prio = (s->channels[ch].working.control >> 19) & 3;
        if (ready(s, ch) && prio < priority) { chosen = ch; priority = prio; }
    }
    if (chosen < VENUSA_DMA2D_CHANNELS) {
        unsigned ch = chosen;
        VenusADMA2DChannel *c = &s->channels[ch];
        VenusADMA2DDescriptor *w = &c->working;
        unsigned width = 1u << ((w->control >> 3) & 3);
        unsigned mode = (w->control >> 1) & 3;
        unsigned burst[] = { 1, 4, 8, 16 };
        unsigned beats = burst[(w->control >> 15) & 3];
        /* A granted burst remains granted after the watermark deasserts. */
        if (w->scaler) {
            scaler_transfer(s, ch, beats);
            goto transfer_done;
        }
        if (w->converter) {
            image_transfer(s, ch, beats);
            goto transfer_done;
        }
        for (unsigned b = 0; b < beats && c->active; b++) {
            uint8_t bytes[4];
            if (mode == 2) {
                if (!normal_beat(s, ch)) { break; }
                goto beat_done;
            }
            if (mode == 0 && !ready(s, ch)) { break; }
            if ((mode != 0 && !w->jpeg_width &&
                 !s->cb.memory_allowed(s->cb.opaque, c->src_cursor, width, false)) ||
                (mode != 1 && !w->jpeg_width &&
                 !s->cb.memory_allowed(s->cb.opaque, c->dst_cursor, width, true))) {
                fail(s, 0xa0 + ch * 4, true, w->src); break;
            }
            if (w->jpeg_width) {
                /* Encode is the inverse mapping: actual raster memory reads
                 * form MCU blocks, then traverse the clocked pixel FIFO. */
                uint32_t base = mode == 1 ? w->src : w->dst;
                bool write = mode == 0;
                for (unsigned j = 0; j < width; j++) {
                    hwaddr address = jpeg_raster_address(w, base, c->transferred + j);
                    if (!s->cb.memory_allowed(s->cb.opaque, address, 1, write)) {
                        fail(s, (write ? 0xc0 : 0xa0) + ch * 4, true, base);
                        goto transfer_done;
                    }
                }
                if (mode == 1) {
                    for (unsigned j = 0; j < width; j++) {
                        if (address_space_read(&address_space_memory,
                                jpeg_raster_address(w, base, c->transferred + j),
                                MEMTXATTRS_UNSPECIFIED, bytes + j, 1) != MEMTX_OK) {
                            fail(s, 0xa0 + ch * 4, true, w->src);
                            goto transfer_done;
                        }
                    }
                } else if (address_space_read(&address_space_memory, c->src_cursor,
                               MEMTXATTRS_UNSPECIFIED, bytes, width) != MEMTX_OK) {
                    fail(s, 0xa0 + ch * 4, true, w->src);
                    goto transfer_done;
                }
                if (mode == 0) {
                    for (unsigned j = 0; j < width; j++) {
                        if (address_space_write(&address_space_memory,
                                jpeg_raster_address(w, base, c->transferred + j),
                                MEMTXATTRS_UNSPECIFIED, bytes + j, 1) != MEMTX_OK) {
                            fail(s, 0xc0 + ch * 4, true, w->dst);
                            goto transfer_done;
                        }
                    }
                } else if (address_space_write(&address_space_memory, c->dst_cursor,
                               MEMTXATTRS_UNSPECIFIED, bytes, width) != MEMTX_OK) {
                    fail(s, 0xc0 + ch * 4, true, w->dst);
                    goto transfer_done;
                }
            } else {
                if (address_space_read(&address_space_memory, c->src_cursor,
                        MEMTXATTRS_UNSPECIFIED, bytes, width) != MEMTX_OK) {
                    fail(s, 0xa0 + ch * 4, true, w->src); break;
                }
                if (address_space_write(&address_space_memory, c->dst_cursor,
                        MEMTXATTRS_UNSPECIFIED, bytes, width) != MEMTX_OK) {
                    fail(s, 0xc0 + ch * 4, true, w->dst); break;
                }
            }
            c->src_cursor = next_address(c->src_cursor, (w->control >> 8) & 3, width);
            c->dst_cursor = next_address(c->dst_cursor, (w->control >> 10) & 3, width);
            c->transferred += width;
beat_done:
            if (c->transferred == w->length) {
                c->active = false;
                c->raw |= 1;
                activate(s, ch);
                irq(s, ch);
                /* Preserve a hardware-visible block boundary for callbacks. */
                break;
            }
        }
    }
transfer_done:
    s->servicing = false;
    for (unsigned ch = 0; ch < VENUSA_DMA2D_CHANNELS; ch++) {
        if (ready(s, ch)) {
            /* Bounded bus service; preserve fractional HCLK deadlines. */
            s->clock_work = int128_add(s->clock_work,
                                      multiply(UINT64_C(32000000000), s->clock_d));
            arm(s);
            return;
        }
    }
    s->clock_work = int128_zero();
}
static void queue(VenusADMA2D *s, unsigned ch)
{
    VenusADMA2DChannel *c = &s->channels[ch];
    uint32_t ctrl = s->regs[ch];
    unsigned mode = (ctrl >> 1) & 3, width = (ctrl >> 3) & 3;
    unsigned dst_width = (ctrl >> 5) & 3;
    uint32_t src = s->regs[(0xa0 + 4 * ch) / 4];
    uint32_t dst = s->regs[(0xc0 + 4 * ch) / 4];
    uint32_t len = s->regs[(0xe0 + 4 * ch) / 4];
    bool image = ctrl & (1u << 23);
    bool sg = ctrl & (1u << 22), ds = ctrl & (1u << 21);
    uint32_t sg_num = s->regs[(0x20 + ch * 4) / 4];
    uint32_t sg_itv = s->regs[(0x40 + ch * 4) / 4];
    uint32_t ds_num = s->regs[(0x60 + ch * 4) / 4];
    uint32_t ds_itv = s->regs[(0x80 + ch * 4) / 4];
    uint32_t geometry = s->regs[(0x120 + ch * 4) / 4];
    uint32_t iw = geometry & 2047, ih = (geometry >> 16) & 2047;
    uint32_t strides = s->regs[(0x160 + ch * 4) / 4];
    uint32_t stride = strides & 0x3fff;
    uint32_t format = s->regs[(0x100 + ch * 4) / 4];
    uint32_t output_geometry = s->regs[(0x140 + ch * 4) / 4];
    unsigned ow = output_geometry & 2047, oh = (output_geometry >> 16) & 2047;
    unsigned out_stride = strides >> 16;
    bool dvp_copy = image && ch == 3 && format == 0x33;
    bool converter = image && (ch == 3 || ch == 4) && !dvp_copy;
    /* Channel 2 converts aligned YUYV422 rasters to/from JPEG MCU blocks. */
    if (image && ch == 2 && ((mode != 0 && mode != 1) || width != 2 ||
        ((ctrl >> (mode == 0 ? 10 : 8)) & 3) != 0 || (ctrl >> 28) != 5 ||
        s->regs[0x184 / 4] != (mode == 0 ? 1 : 2) || format != 0x33 ||
        (geometry & 0xf800f800u) || !iw || iw > 640 || iw % 16 ||
        !ih || ih > 480 || ih % 8 || stride != iw * 2 ||
        strides != (stride | (stride << 16)) || output_geometry != geometry ||
        len != iw * ih * 2)) {
        fail(s, ch * 4, true, ctrl); return;
    }
    if (image && ch != 0 && ch != 2 && !converter && !dvp_copy) {
        fail(s, ch * 4, true, ctrl); return;
    }
    if (dvp_copy && (mode != 0 || width != 2 || (ctrl >> 28) != 3 ||
        ((ctrl >> 10) & 3) || src != 0x42001000 ||
        (geometry & 0xf800f800u) || !iw || !ih || (iw & 1) ||
        output_geometry != geometry || strides != (iw * 2 | (iw * 2 << 16)) ||
        len != iw * ih * 2 || !scaler_address_ok(s, dst, len, true))) {
        fail(s, ch * 4, true, ctrl); return;
    }
    unsigned in_format = format & 7, out_format = (format >> 4) & 7;
    unsigned in_yuv = (format >> 8) & 3;
    uint32_t sc = s->regs[0x4f0 / 4], phases = s->regs[0x4f4 / 4];
    uint32_t starts = s->regs[0x4f8 / 4];
    unsigned mx = merge_factor((sc >> 5) & 15), my = merge_factor((sc >> 9) & 15);
    unsigned px = sc & 8 ? phases & 0xffff : 4096;
    unsigned py = sc & 8 ? phases >> 16 : 4096;
    /* YUV tile alignment may prepend one merged sample before the fractional phase. */
    unsigned sx = (starts & 0xfff) + ((sc & 0x8000) ? 4096 : 0), sy = starts >> 16;
    unsigned in_bpp = in_format == 4 ? 1 : in_format == 3 ? 2 : 3;
    unsigned out_bpp = out_format == 4 ? 1 : out_format == 3 ? 2 : 3;
    if (converter &&
        (width != 2 || ((geometry | output_geometry) & 0xf800f800u) ||
         (strides & 0xc000c000u) || ((ctrl >> 10) & 3) ||
         (ch == 4 && (mode != 2 || ((ctrl >> 8) & 3) || format != 3 || iw != ow || ih != oh ||
                      !iw || !ih || (iw & 1) || stride < iw * 2 ||
                      out_stride < ow * 3 || len != (uint64_t)iw * ih * 2)) ||
         (ch == 3 && (mode != 0 || format != 0x1030 || iw != ow || ih != oh ||
                      !iw || !ih || (iw & 1) || stride != iw * 3 ||
                      out_stride < ow * 2 || len != (uint64_t)ih * stride ||
                      src != 0x42001000)))) {
        fail(s, ch * 4, true, ctrl); return;
    }
    if (image && ch == 0) {
        if (mode != 2 || width != 2 || ((ctrl >> 8) & 15) ||
            !(sc & 1) || ((sc & 0x8000) && (!(sc & 8) || in_format != 3)) || !mx || !my ||
            (!(sc & 16) && (mx != 1 || my != 1)) ||
            (!(sc & 8) && (starts || (phases && phases != 0x10001000))) ||
            !px || !py || (px != 4096 && !!(sc & 2) != (px < 4096)) ||
            (py != 4096 && !!(sc & 4) != (py < 4096)) ||
            (in_format != 0 && in_format != 3 && in_format != 4) ||
            (out_format != 0 && out_format != 4) || (format & ~0x3377u) ||
            (in_format != 3 && in_yuv) || (in_format != 0 && (format & 0x1000)) ||
            (out_format != 0 && (format & 0x2000)) ||
            ((geometry | output_geometry) & 0xf800f800u) || (strides & 0xc000c000u) ||
            !iw || !ih || !ow || !oh || iw % mx || ih % my ||
            (in_format == 3 && (iw & 1)) ||
            stride < iw * in_bpp || out_stride < ow * out_bpp ||
            len != (uint64_t)ow * oh * out_bpp) {
            fail(s, ch * 4, true, ctrl); return;
        }
        uint64_t last_x = sx + (uint64_t)(ow - 1) * px;
        uint64_t last_y = sy + (uint64_t)(oh - 1) * py;
        uint64_t in_span = (uint64_t)(ih - 1) * stride + iw * in_bpp;
        uint64_t out_span = (uint64_t)(oh - 1) * out_stride + ow * out_bpp;
        if ((last_x >> 12) >= iw / mx || (last_y >> 12) >= ih / my ||
            (!(sc & 0x2000) && (last_x & 4095) && (last_x >> 12) + 1 >= iw / mx) ||
            (!(sc & 0x4000) && (last_y & 4095) && (last_y >> 12) + 1 >= ih / my) ||
            !scaler_address_ok(s, src, in_span, false) ||
            !scaler_address_ok(s, dst, out_span, true) ||
            !scaler_disjoint(src, in_span, dst, out_span)) {
            fail(s, ch * 4, true, ctrl); return;
        }
    }
    uint32_t unsupported = (1u << 12) | (1u << 18) |
                           (0xfu << 24);
    if (c->pending || width > 2 || dst_width > 2 ||
        ((mode != 2 || image) && width != dst_width) ||
        ((ctrl >> 8) & 3) == 3 || ((ctrl >> 10) & 3) == 3 ||
        mode == 3 || (ctrl & unsupported) ||
        /* Peripheral flow is supported only by the JPEG FIFO requests. Its
         * capacity descriptor may remain active until firmware stops it. */
        ((ctrl & (1u << 17)) &&
         (mode == 2 || ((ctrl >> 28) != 4 && (ctrl >> 28) != 5))) ||
        ((sg || ds) && (image || mode != 2)) ||
        (sg && (!sg_num || sg_num > 0xffffff || sg_itv > 0xffffff ||
                ((sg_num | sg_itv) & ((1u << width) - 1)) || ((ctrl >> 8) & 3))) ||
        (ds && (!ds_num || ds_num > 0xffffff || ds_itv > 0xffffff ||
                ((ds_num | ds_itv) & ((1u << dst_width) - 1)) || ((ctrl >> 10) & 3))) ||
        !len || len > 0xffffff ||
        /* Image ROI coordinates are byte addresses, including 24-bit RGB pixels. */
        (!(image && ch == 0) && ((src & ((1u << width) - 1)) ||
                               (dst & ((1u << dst_width) - 1)))) ||
        (mode != 2 && (len & ((1u << width) - 1))) ||
        (mode == 1 && (((ctrl >> 10) & 3) != 2 || width != 2)) ||
        (mode == 0 && (((ctrl >> 8) & 3) != 2 || width != 2)) ||
        (!image && format) || s->regs[(0x300 + ch * 4) / 4]) {
        fail(s, ch * 4, true, ctrl); return;
    }
    if (!image && mode == 2 &&
        (!normal_region_ok(s, src, len, 1u << width, (ctrl >> 8) & 3,
                           sg ? sg_num : 0, sg_itv, false) ||
         !normal_region_ok(s, dst, len, 1u << dst_width, (ctrl >> 10) & 3,
                           ds ? ds_num : 0, ds_itv, true))) {
        fail(s, ch * 4, true, ctrl); return;
    }
    if (converter && ch == 4) {
        uint64_t in_span = (uint64_t)(ih - 1) * stride + iw * 2;
        uint64_t out_span = (uint64_t)(oh - 1) * out_stride + ow * 3;
        if (!scaler_address_ok(s, src, in_span, false) ||
            !scaler_address_ok(s, dst, out_span, true) ||
            !scaler_disjoint(src, in_span, dst, out_span)) {
            fail(s, ch * 4, true, ctrl); return;
        }
    } else if (converter) {
        uint64_t out_span = (uint64_t)(oh - 1) * out_stride + ow * 2;
        if (!scaler_address_ok(s, dst, out_span, true)) {
            fail(s, ch * 4, true, ctrl); return;
        }
    }
    c->queued = (VenusADMA2DDescriptor){
        .control = ctrl, .src = src, .dst = dst, .length = len,
        .sg_num = sg_num, .sg_itv = sg_itv,
        .ds_num = ds_num, .ds_itv = ds_itv,
        .irq_enable = s->regs[(0x220 + 4 * ch) / 4],
        .block_mask = s->regs[(0x240 + 4 * ch) / 4] & 1,
        .jpeg_width = image && ch == 2 ? iw : 0,
        .jpeg_stride = image && ch == 2 ? stride : 0,
        .in_width = iw, .in_height = ih, .out_width = ow, .out_height = oh,
        .in_stride = stride, .out_stride = out_stride,
        .phase_x = px, .phase_y = py, .start_x = sx, .start_y = sy,
        .in_format = in_format, .out_format = out_format,
        .in_yuv = in_yuv, .in_rgb = !!(format & 0x1000),
        .out_rgb = !!(format & 0x2000),
        .merge_x = mx, .merge_y = my, .scaler = image && ch == 0,
        .converter = converter,
        .last_tile = !!(sc & 0x2000), .last_band = !!(sc & 0x4000),
    };
    c->pending = true;
    if (s->clock_enabled) { activate(s, ch); }
    venusa_dma2d_request(s);
}
static void cancel(VenusADMA2D *s, unsigned ch)
{
    VenusADMA2DChannel *c = &s->channels[ch];
    c->active = c->pending = false;
    c->raw |= 16;
    irq(s, ch);
    for (unsigned i = 0; i < VENUSA_DMA2D_CHANNELS; i++) {
        if (s->channels[i].active) { return; }
    }
    timer_del(s->event);
    s->clock_work = int128_zero();
}
static uint64_t read_reg32(void *opaque, hwaddr off, unsigned size)
{
    VenusADMA2D *s = opaque;
    unsigned ch = (off & 31) / 4, bank = off & ~31;
    if (size != 4 || (off & 3)) { fail(s, off, false, 0); return 0; }
    if (ch < 6 && (bank <= 0x160 || (bank >= 0x200 && bank <= 0x340))) {
        VenusADMA2DChannel *c = &s->channels[ch];
        switch (bank) {
        case 0x200: return c->pending;
        case 0x260: return 0;
        case 0x280: return c->raw;
        case 0x2a0: return status(s, ch);
        case 0x320: return c->active ? c->working.length - c->transferred : 0;
        case 0x340: return !c->active;
        default: return s->regs[off / 4];
        }
    }
    if (ch < 6 && bank >= 0x380 && bank <= 0x4a0) {
        VenusADMA2DChannel *c = &s->channels[ch];
        if (bank == 0x3c0) { return c->working.sg_itv; }
        if (bank == 0x3e0) { return c->working.sg_num |
                                   ((c->working.control >> 22) & 1u) << 31; }
        if (bank == 0x400) { return c->working.ds_itv; }
        if (bank == 0x420) { return c->working.ds_num |
                                   ((c->working.control >> 21) & 1u) << 31; }
        if (bank == 0x380) { return c->working.src; }
        if (bank == 0x3a0) { return c->working.dst; }
        if (bank == 0x460) { return ((c->working.control >> 8) & 3) << 24 |
                                   ((c->working.control >> 13) & 3) << 22; }
        if (bank == 0x480) { return ((c->working.control >> 10) & 3) << 24 |
                                   ((c->working.control >> 15) & 3) << 22; }
        if (bank == 0x440) { return c->working.length | c->working.block_mask << 30 |
                                  c->working.irq_enable << 31; }
        return 0;
    }
    if (off == 0x184) { return s->regs[off / 4]; }
    if (off == 0x1a0 || off == 0x1c0) { return 0; }
    if (off == 0x360 || off == 0x364 || off == 0x4dc) { return 0; }
    if (off == 0x4fc) {
        const VenusADMA2DDescriptor *w = &s->channels[0].working;
        return (w->start_x & 0xfff) | (w->start_y << 12) |
               ((uint32_t)w->last_tile << 24) | ((uint32_t)w->last_band << 25);
    }
    if (off >= 0x4c0 && off <= 0x500) { return s->regs[off / 4]; }
    fail(s, off, false, 0); return 0;
}
static void write_reg32(void *opaque, hwaddr off, uint64_t value, unsigned size)
{
    VenusADMA2D *s = opaque;
    uint32_t v = value;
    unsigned ch = (off & 31) / 4, bank = off & ~31;
    if (size != 4 || (off & 3)) { fail(s, off, true, v); return; }
    if (ch < 6 && (bank <= 0x160 || (bank >= 0x200 && bank <= 0x300))) {
        VenusADMA2DChannel *c = &s->channels[ch];
        bool shadow_write = (bank >= 0x20 && bank <= 0x80) || bank == 0xa0 || bank == 0xc0 || bank == 0xe0 ||
                            bank == 0x220 || (bank == 0x240 &&
                            ((v ^ s->regs[off / 4]) & 1));
        if (c->pending && shadow_write) { fail(s, off, true, v); return; }
        switch (bank) {
        case 0x200:
            if (!v) { c->pending = false; return; }
            if (v != 1) { fail(s, off, true, v); return; }
            queue(s, ch); return;
        case 0x220:
            if (v > 1) { fail(s, off, true, v); return; }
            break;
        case 0x240:
            if (v & ~31u) { fail(s, off, true, v); return; }
            break;
        case 0x260:
            if (v & ~31u) { fail(s, off, true, v); return; }
            c->raw &= ~v; irq(s, ch); return;
        case 0x280: case 0x2a0: fail(s, off, true, v); return;
        default:
            if (bank >= 0x100 && bank != 0x200 && v &&
                !((ch == 0 || ch == 2 || ch == 3 || ch == 4) &&
                  bank <= 0x160)) {
                fail(s, off, true, v); return;
            }
            break;
        }
        s->regs[off / 4] = v;
        irq(s, ch);
        venusa_dma2d_request(s);
        return;
    }
    if (off == 0x360) {
        if (v & ~63u) { fail(s, off, true, v); return; }
        for (unsigned i = 0; i < 6; i++) { if (v & (1u << i)) { cancel(s, i); } }
        return;
    }
    if (off == 0x364 && v <= 1) { if (v) { venusa_dma2d_reset(s); } return; }
    if (off == 0x184 && v <= 2) { s->regs[off / 4] = v; return; }
    if ((off == 0x1a0 || off == 0x1c0) && !v) { return; }
    if ((off == 0x4c4 || off == 0x4c8) && !(v & ~0x100u)) { return; }
    if (off == 0x4dc && !(v & ~15u)) { return; }
    if ((off == 0x4c0 && !(v & ~3u)) || (off == 0x4e8 && !(v & ~0x33u)) ||
        (off == 0x4d4 && v <= 1) || (off == 0x4d8 && !(v & ~15u)) ||
        (off == 0x4f0 && !(v & ~0xffffu)) ||
        off == 0x4f4 ||
        (off == 0x4f8 && !(v & ~0x0fff0fffu)) ||
        (off == 0x4ec && !v) || (off == 0x500 && !v)) {
        s->regs[off / 4] = v; return;
    }
    fail(s, off, true, v);
}
static uint64_t read_reg(void *opaque, hwaddr off, unsigned size)
{
    VenusADMA2D *s = opaque;
    if ((size != 1 && size != 2 && size != 4) || (off & 3) + size > 4) {
        s->cb.fail(s->cb.opaque, s->base + off, size, false, 0); return 0;
    }
    uint64_t value = read_reg32(s, off & ~3, 4);
    return (value >> (8 * (off & 3))) & (UINT32_MAX >> (32 - size * 8));
}
static void write_reg(void *opaque, hwaddr off, uint64_t value, unsigned size)
{
    VenusADMA2D *s = opaque;
    if ((size != 1 && size != 2 && size != 4) || (off & 3) + size > 4) {
        s->cb.fail(s->cb.opaque, s->base + off, size, true, value); return;
    }
    hwaddr aligned = off & ~3;
    unsigned bank = aligned & ~31;
    uint32_t mask = (UINT32_MAX >> (32 - size * 8)) << (8 * (off & 3));
    uint32_t v = (value << (8 * (off & 3))) & mask;
    bool pulse = bank == 0x200 || bank == 0x260 || aligned == 0x360 ||
                 aligned == 0x364 || aligned == 0x4dc;
    if (size != 4 && !pulse) { v |= read_reg32(s, aligned, 4) & ~mask; }
    write_reg32(s, aligned, v, 4);
}
static const MemoryRegionOps ops = {
    .read = read_reg, .write = write_reg, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};
void venusa_dma2d_reset(VenusADMA2D *s)
{
    timer_del(s->event);
    memset(s->regs, 0, sizeof(s->regs));
    memset(s->channels, 0, sizeof(s->channels));
    s->clock_work = int128_zero();
    s->event_epoch = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    for (unsigned ch = 0; ch < 6; ch++) {
        s->regs[ch] = 0x0019e040;
        s->regs[(0x240 + ch * 4) / 4] = 30;
        irq(s, ch);
    }
    s->regs[0x4c0 / 4] = 1;
    s->regs[0x4e8 / 4] = 0x11;
    s->regs[0x4d8 / 4] = 15;
    s->cb.irq(s->cb.opaque, 6, false);
}
void venusa_dma2d_clock(VenusADMA2D *s, bool enabled, uint64_t n, uint64_t d)
{
    sync_clock(s);
    timer_del(s->event);
    if (!d) { fail(s, 0, true, 0); return; }
    if (s->clock_d && s->clock_d != d) {
        Int128 old = int128_make64(s->clock_d);
        bool debt = int128_lt(s->clock_work, int128_zero());
        Int128 magnitude = debt ? int128_neg(s->clock_work) : s->clock_work;
        Int128 quotient = int128_divu(magnitude, old);
        if (int128_gt(quotient, int128_make64(UINT64_MAX))) {
            fail(s, 0, true, 0); return;
        }
        uint64_t whole = int128_get64(quotient);
        uint64_t rest = int128_get64(int128_remu(magnitude, old));
        s->clock_work = int128_add(multiply(whole, d),
                                  int128_divu(multiply(rest, d), old));
        if (debt) { s->clock_work = int128_neg(s->clock_work); }
    }
    s->clock_enabled = enabled; s->clock_n = n; s->clock_d = d;
    venusa_dma2d_request(s);
}
void venusa_dma2d_init(VenusADMA2D *s, Object *owner, hwaddr base, VenusADMA2DCallbacks cb)
{
    s->base = base; s->cb = cb;
    s->event = timer_new_ns(QEMU_CLOCK_VIRTUAL, transfer, s);
    memory_region_init_io(&s->io, owner, &ops, s, "venusa-dma2d", 0x1000);
    venusa_dma2d_reset(s);
}
