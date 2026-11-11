/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Init-only MUSB host; register layout follows the ChryUSB MUSB port.
 * Supports POWER, TX/RX/USB interrupt status/enables, INDEX, DEVCTL SESSION,
 * and eight banks of TX/RX FIFO size/address configuration within 4 KiB.
 * TX/RX allocations may overlap, as in the shared-FIFO firmware layout.
 * SESSION requires IDDIG=0 and UTMI16=1; incompatible input changes during
 * SESSION are rejected before changing state. POWER retains only HSENABLE[5]
 * and EN_SUSPENDM[0]; SUSPEND_MODE[1]/HSMODE[4] are read-only zero. Resume,
 * reset signaling, device soft-connect and ISO-update controls are unsupported.
 * FSDEV, LSDEV, VBUS-valid, interrupt status and IRQ stay zero: there is no
 * USB bus or attached device.
 * No enumeration, FIFO data, endpoint CSR, DMA or transfer engine is modeled;
 * all other registers, wrong widths and accesses with the clock off fail.
 * Clock gating retains configuration; controller reset clears it but preserves
 * the SoC's external clock/IDDIG/UTMI16 inputs. Analog PHY behavior is absent.
 */
#include "qemu/osdep.h"
#include "hw/usb/listenai_musb.h"
#include "hw/irq.h"
#include "qemu/error-report.h"
#include "qemu/module.h"

static G_NORETURN void unsupported(hwaddr offset, unsigned size, bool write,
                                  uint64_t value)
{
    error_report("ListenAI MUSB unsupported %s offset=0x%" HWADDR_PRIx
                 " size=%u value=0x%" PRIx64,
                 write ? "write" : "read", offset, size, value);
    exit(1);
}

static unsigned register_width(hwaddr offset)
{
    switch (offset) {
    case 0x01: case 0x0a: case 0x0b: case 0x0e:
    case 0x60: case 0x62: case 0x63:
        return 1;
    case 0x02: case 0x04: case 0x06: case 0x08: case 0x64: case 0x66:
        return 2;
    default:
        return 0;
    }
}

static uint64_t read_reg(void *opaque, hwaddr offset, unsigned size)
{
    ListenAIMUSB *s = opaque;

    if (!s->clock_enabled || size != register_width(offset)) {
        unsupported(offset, size, false, 0);
    }
    switch (offset) {
    case 0x01: return s->power;
    case 0x02: case 0x04: case 0x0a: return 0;
    case 0x06: return s->tx_ie;
    case 0x08: return s->rx_ie;
    case 0x0b: return s->usb_ie;
    case 0x0e: return s->index;
    /* HOST follows an active A-session, not a fabricated VBUS/speed input. */
    case 0x60:
        return s->session | ((s->session && !s->iddig) ? 4 : 0);
    case 0x62: case 0x63: return s->fifo_size[s->index][offset - 0x62];
    case 0x64: case 0x66: return s->fifo_addr[s->index][(offset - 0x64) / 2];
    default:
        g_assert_not_reached();
    }
}

static bool fifo_fits(unsigned size, unsigned address)
{
    /* MUSB encodes size as log2(bytes/8), bit 4 doubles the allocation. */
    return !(size & ~0x1fu) && (size & 15) <= 9 &&
           address * 8 + ((8u << (size & 15)) << ((size >> 4) & 1)) <= 4096;
}

static void write_reg(void *opaque, hwaddr offset, uint64_t value,
                      unsigned size)
{
    ListenAIMUSB *s = opaque;
    unsigned direction;

    if (!s->clock_enabled || size != register_width(offset)) {
        goto fail;
    }
    switch (offset) {
    case 0x01:
        /* usb_reg.h: only EN_SUSPENDM[0]/HSENABLE[5] are safe init controls. */
        if (value & 0xcc) {
            goto fail;
        }
        /* SUSPEND_MODE[1]/HSMODE[4] are read-only and stay zero. */
        s->power = value & 0x21;
        return;
    case 0x06:
        s->tx_ie = value;
        return;
    case 0x08:
        s->rx_ie = value;
        return;
    case 0x0b:
        s->usb_ie = value;
        return;
    case 0x0e:
        if (value > 7) {
            goto fail;
        }
        s->index = value;
        return;
    case 0x60:
        /* Only SESSION is writable; a read-modify-write may include HOST. */
        if ((value & ~5u) || ((value & 1) && (s->iddig || !s->utmi16))) {
            goto fail;
        }
        s->session = value & 1;
        return;
    case 0x62: case 0x63:
        direction = offset - 0x62;
        if (!fifo_fits(value, s->fifo_addr[s->index][direction])) {
            goto fail;
        }
        s->fifo_size[s->index][direction] = value;
        return;
    case 0x64: case 0x66:
        direction = (offset - 0x64) / 2;
        if (!fifo_fits(s->fifo_size[s->index][direction], value)) {
            goto fail;
        }
        /* TX/RX may deliberately share an allocation; do not sum them. */
        s->fifo_addr[s->index][direction] = value;
        return;
    default:
        goto fail;
    }
fail:
    unsupported(offset, size, true, value);
}

static const MemoryRegionOps ops = {
    .read = read_reg,
    .write = write_reg,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
};

bool listenai_musb_configure(ListenAIMUSB *s, bool clock_enabled,
                            bool iddig, bool utmi16)
{
    if (s->session && (iddig || !utmi16)) {
        return false;
    }
    s->clock_enabled = clock_enabled;
    s->iddig = iddig;
    s->utmi16 = utmi16;
    return true;
}

void listenai_musb_reset(ListenAIMUSB *s)
{
    s->power = s->index = s->session = s->usb_ie = 0;
    s->tx_ie = s->rx_ie = 0;
    memset(s->fifo_size, 0, sizeof(s->fifo_size));
    memset(s->fifo_addr, 0, sizeof(s->fifo_addr));
    qemu_set_irq(s->irq, 0);
}

static void reset(DeviceState *dev)
{
    listenai_musb_reset(LISTENAI_MUSB(dev));
}

static void instance_init(Object *obj)
{
    ListenAIMUSB *s = LISTENAI_MUSB(obj);

    memory_region_init_io(&s->io, obj, &ops, s, TYPE_LISTENAI_MUSB, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->io);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static void class_init(ObjectClass *oc, const void *data)
{
    device_class_set_legacy_reset(DEVICE_CLASS(oc), reset);
}

static const TypeInfo type_info = {
    .name = TYPE_LISTENAI_MUSB,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ListenAIMUSB),
    .instance_init = instance_init,
    .class_init = class_init,
};

static void register_types(void)
{
    type_register_static(&type_info);
}
type_init(register_types)
