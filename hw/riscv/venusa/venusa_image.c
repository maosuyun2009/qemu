/* SPDX-License-Identifier: GPL-2.0-or-later */
/* VenusA image-controller bus, IRQ and clock wiring. */
#include "qemu/osdep.h"
#include "system/address-spaces.h"
#include "hw/irq.h"
#include "qapi/error.h"
#include "hw/riscv/venusa/venusa.h"

static void fault(void *opaque, hwaddr address, unsigned size, bool write,
                  uint64_t value)
{ venusa_fail(opaque, address, size, write, value); }
static void dma_irq(void *opaque, unsigned channel, bool level)
{ venusa_irq(opaque, 49 + channel, level); }
static void rgb_irq(void *opaque, bool level)
{ venusa_irq(opaque, 60, level); }
static void jpeg_irq(void *opaque, int n, int level)
{ venusa_irq(opaque, 61, level); }
static void request_changed(void *opaque)
{
    VenusASoC *s = opaque;
    if (s->dma2d.event) { venusa_dma2d_request(&s->dma2d); }
}
static bool memory_allowed(void *opaque, hwaddr address, unsigned size, bool write)
{
    uint64_t end = address + (uint64_t)size;
    if (!size || end > UINT64_C(0x100000000)) { return false; }
    static const uint32_t ranges[][2] = {
        {0x80000,0x82000}, {0x100000,0x102000}, {0x200000,0x202000},
        {0x300000,0x302000}, {0x20000000,0x20086000},
        {0x38000000,0x39000000}
    };
    for (unsigned i=0;i<G_N_ELEMENTS(ranges);i++) {
        if (address >= ranges[i][0] && end <= ranges[i][1]) { return true; }
    }
    return !write && (end <= 0x8000 ||
        (address >= 0x30000000 && end <= 0x31000000));
}
static bool request(void *opaque, unsigned handshake, hwaddr address, bool write)
{
    VenusASoC *s = opaque;
    if ((handshake == 4 && address == VENUSA_JPEG_ECS) ||
        (handshake == 5 && address == VENUSA_JPEG_PIXEL)) {
        bool input = venusa_jpeg_encoding(&s->jpeg) ? handshake == 5 : handshake == 4;
        if (write != input) {
            venusa_fail(s, address, 4, write, handshake);
        }
        if (s->syscfg[0xa0 / 4] & ((1u << handshake) | (1u << (16 + handshake)))) {
            venusa_fail(s, address, 4, write, handshake);
        }
        return venusa_jpeg_request(&s->jpeg, write);
    }
    if (handshake == 3 && address == 0x42001000 && !write) {
        bool route = (s->syscfg[0xa0 / 4] & ((1u << 3) | (1u << 15) | (1u << 19))) == (1u << 15);
        return route && s->dvp.clock && (s->dvp.regs[0x1c / 4] & 1) &&
               s->dvp.request && s->dvp.count;
    }
    if (handshake != 0 || address != 0x4200b0f4 || !write ||
        (s->syscfg[0xa0 / 4] & ((1u << 16) | 1u))) {
        venusa_fail(s, address, 4, write, handshake);
    }
    return venusa_rgb_request(&s->rgb);
}
void venusa_image_clock(VenusASoC *s)
{
    if (!s->rgb.event || !s->dma2d.event) { return; }
    uint32_t cfg = s->syscfg[0x30 / 4];
    if ((cfg & (1u << 26)) && !(s->rgb_clock_config & (1u << 26))) {
        s->rgb_div = (cfg >> 22) & 15;
        if (!s->rgb_div) { s->rgb_div = 16; }
    }
    s->rgb_clock_config = cfg;
    uint64_t n = 24000000, d = s->rgb_div ? s->rgb_div : 1;
    bool enabled = s->rgb_div && (cfg & (1u << 27));
    if (cfg & (1u << 21)) {
        enabled &= (s->buscfg[2] & 1) && (s->buscfg[2] & (1u << 26));
        uint64_t mult = (uint64_t)(s->buscfg[6] & 0x1ff) << 27;
        if (s->buscfg[6] & (1u << 9)) { mult += s->buscfg[5] & 0x7ffffff; }
        n *= mult; d *= (UINT64_C(1) << 27) * (((s->buscfg[3] >> 12) & 15) + 6);
    }
    uint64_t a = n, b = d;
    while (b) { uint64_t r=a%b; a=b; b=r; }
    venusa_rgb_clock(&s->rgb, enabled, n/a, d/a);
    venusa_hclk_ratio(s, &n, &d);
    venusa_dma2d_clock(&s->dma2d, (s->syscfg[0x34/4] & 16) != 0, n, d);
    venusa_jpeg_clock(&s->jpeg, (s->syscfg[0x34/4] & 0x100) != 0);
}
void venusa_image_init(VenusASoC *s)
{
    VenusADMA2DCallbacks dma = { .opaque=s, .irq=dma_irq, .fail=fault,
        .memory_allowed=memory_allowed, .request=request };
    VenusARGBCallbacks rgb = { .opaque=s, .irq=rgb_irq, .fail=fault,
        .request_changed=request_changed };
    venusa_dma2d_init(&s->dma2d, OBJECT(s), 0x44900000, dma);
    venusa_rgb_init(&s->rgb, OBJECT(s), 0x4200b000, rgb);
    memory_region_add_subregion(get_system_memory(), 0x44900000, &s->dma2d.io);
    memory_region_add_subregion(get_system_memory(), 0x4200b000, &s->rgb.io);
    object_initialize_child(OBJECT(s), "jpeg", &s->jpeg, TYPE_VENUSA_JPEG);
    s->jpeg.fault = fault;
    s->jpeg.fault_opaque = s;
    s->jpeg.request_changed = request_changed;
    sysbus_realize(SYS_BUS_DEVICE(&s->jpeg), &error_fatal);
    for (unsigned i = 0; i < G_N_ELEMENTS(s->jpeg.windows); i++) {
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->jpeg), i, VENUSA_JPEG_BASE + s->jpeg.windows[i].offset);
    }
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->jpeg), 0, qemu_allocate_irq(jpeg_irq, s, 0));
    s->rgb_div = 0;
}
void venusa_image_reset(VenusASoC *s)
{
    s->rgb_div=0; s->rgb_clock_config=0;
    venusa_dma2d_reset(&s->dma2d); venusa_rgb_reset(&s->rgb);
    venusa_jpeg_reset(&s->jpeg);
    s->jpeg.decoded_frames = s->jpeg.output_bytes = 0;
    venusa_image_clock(s);
}
