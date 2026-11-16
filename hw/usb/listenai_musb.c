/* SPDX-License-Identifier: GPL-2.0-or-later */
/* MUSB host subset used by the ChryUSB port: EP0 PIO and direct ISO IN. */
#include "qemu/osdep.h"
#include "hw/usb/listenai_musb.h"
#include "hw/irq.h"
#include "qemu/bswap.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "hw/qdev-properties.h"

#define PACKET_NS 125000

static G_NORETURN void unsupported(hwaddr offset, unsigned size, bool write,
                                  uint64_t value)
{
    error_report("ListenAI MUSB unsupported %s offset=0x%" HWADDR_PRIx
                 " size=%u value=0x%" PRIx64,
                 write ? "write" : "read", offset, size, value);
    exit(1);
}

static void update_irq(ListenAIMUSB *s)
{
    qemu_set_irq(s->irq, s->clock_enabled &&
                 ((s->tx_is & s->tx_ie) || (s->rx_is & s->rx_ie) ||
                  (s->usb_is & s->usb_ie)));
}

static bool present(ListenAIMUSB *s)
{
    return !s->iddig && s->session && s->port.dev && s->port.dev->attached;
}

static void connection_update(ListenAIMUSB *s)
{
    bool connected = present(s);

    if (connected != s->connected) {
        s->usb_is |= connected ? 0x10 : 0x20;
        s->connected = connected;
        s->port_enabled = false;
    }
    update_irq(s);
}

static void attach(USBPort *port)
{
    connection_update(port->opaque);
}

static void detach(USBPort *port)
{
    ListenAIMUSB *s = port->opaque;

    if (s->connected) {
        s->usb_is |= 0x20;
    }
    s->connected = s->port_enabled = false;
    timer_del(s->timer);
    memset(s->endpoint, 0, sizeof(s->endpoint));
    memset(s->count, 0, sizeof(s->count));
    memset(s->tx_count, 0, sizeof(s->tx_count));
    update_irq(s);
}

static USBPortOps port_ops = { .attach = attach, .detach = detach };
static USBBusOps bus_ops;

static void schedule(ListenAIMUSB *s)
{
    if (s->clock_enabled && s->session && !s->iddig &&
        !timer_pending(s->timer)) {
        timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + PACKET_NS);
    }
}

static void transfer(ListenAIMUSB *s, unsigned ep)
{
    uint8_t *r = s->endpoint[ep];
    bool in = r[2] & 0x20;
    int pid = in ? USB_TOKEN_IN : (r[2] & 8) ? USB_TOKEN_SETUP : USB_TOKEN_OUT;
    unsigned len = in ? 64 : s->tx_count[ep];
    USBDevice *dev = s->port_enabled ?
        usb_find_device(&s->port, s->target[ep][0]) : NULL;
    USBPacket packet = {0};

    usb_packet_init(&packet);
    usb_packet_setup(&packet, pid, dev ? usb_ep_get(dev, pid, 0) : NULL,
                     0, 0, false, true);
    usb_packet_addbuf(&packet, s->fifo[ep], len);
    usb_handle_packet(dev, &packet);
    if (packet.status == USB_RET_NAK) {
        usb_packet_cleanup(&packet);
        schedule(s);
        return;
    }
    if (packet.status == USB_RET_ASYNC) {
        usb_cancel_packet(&packet);
        unsupported(0x102, 1, true, r[2]);
    }
    r[2] &= ~0x2a;
    if (packet.status == USB_RET_SUCCESS) {
        s->tx_count[ep] = 0;
        if (in) {
            s->count[ep] = packet.actual_length;
            s->position[ep] = 0;
            r[2] |= 1;
        }
    } else {
        r[2] |= packet.status == USB_RET_STALL ? 4 : 0x10;
    }
    s->tx_is |= 1 << ep;
    usb_packet_cleanup(&packet);
}

static void receive_iso(ListenAIMUSB *s, unsigned ep)
{
    uint8_t *r = s->endpoint[ep];
    unsigned max_packet = lduw_le_p(r + 4);
    USBDevice *dev = s->port_enabled ?
        usb_find_device(&s->port, s->target[ep][4]) : NULL;
    USBPacket packet = {0};

    if ((r[12] & 0xf0) != 0x50 || !(r[12] & 15) || r[13] != 1 ||
        !max_packet || max_packet > 1024 ||
        max_packet > (8u << (s->fifo_size[ep][1] & 15))) {
        unsupported(0x106 + ep * 16, 1, true, r[6]);
    }
    usb_packet_init(&packet);
    usb_packet_setup(&packet, USB_TOKEN_IN,
                     dev ? usb_ep_get(dev, USB_TOKEN_IN, r[12] & 15) : NULL,
                     0, 0, false, true);
    usb_packet_addbuf(&packet, s->fifo[ep], max_packet);
    usb_handle_packet(dev, &packet);
    if (packet.status == USB_RET_ASYNC) {
        usb_cancel_packet(&packet);
        unsupported(0x106 + ep * 16, 1, true, r[6]);
    }
    r[6] &= ~0x20;
    s->position[ep] = s->count[ep] = 0;
    if (packet.status == USB_RET_SUCCESS) {
        s->count[ep] = packet.actual_length;
        r[6] |= 1;
    } else {
        r[6] |= packet.status == USB_RET_STALL ? 0x40 : 4;
    }
    s->rx_is |= 1 << ep;
    usb_packet_cleanup(&packet);
}

static void tick(void *opaque)
{
    ListenAIMUSB *s = opaque;

    if (!s->clock_enabled || !s->session) {
        return;
    }
    if (s->endpoint[0][2] & 0x22) {
        transfer(s, 0);
    }
    for (unsigned ep = 1; ep < 8; ep++) {
        if (s->endpoint[ep][6] & 0x20) {
            receive_iso(s, ep);
        }
    }
    update_irq(s);
}

static unsigned register_width(hwaddr offset)
{
    switch (offset) {
    case 0x00: case 0x01: case 0x0a: case 0x0b: case 0x0e:
    case 0x60: case 0x62: case 0x63:
        return 1;
    case 0x02: case 0x04: case 0x06: case 0x08: case 0x0c:
    case 0x64: case 0x66:
        return 2;
    default:
        return 0;
    }
}

static bool endpoint_offset(ListenAIMUSB *s, hwaddr off,
                            unsigned *ep, unsigned *reg)
{
    if (off >= 0x10 && off <= 0x1d) {
        *ep = s->index;
        *reg = off - 0x10;
        return true;
    }
    if (off >= 0x100 && off < 0x180 && (off & 15) <= 13) {
        *ep = (off - 0x100) / 16;
        *reg = off & 15;
        return true;
    }
    return false;
}

static bool endpoint_width(unsigned reg, unsigned size)
{
    return size == 1 || (size == 2 && (reg == 0 || reg == 2 ||
                                       reg == 4 || reg == 6 || reg == 8));
}

static uint64_t read_reg(void *opaque, hwaddr offset, unsigned size)
{
    ListenAIMUSB *s = opaque;
    unsigned ep, reg;
    uint64_t value;

    if (!s->clock_enabled) {
        goto fail;
    }
    if (offset >= 0x20 && offset <= 0x3c && !(offset & 3) &&
        (size == 1 || size == 2 || size == 4)) {
        ep = (offset - 0x20) / 4;
        if (s->position[ep] + size > s->count[ep]) {
            goto fail;
        }
        value = 0;
        for (unsigned i = 0; i < size; i++) {
            value |= (uint64_t)s->fifo[ep][s->position[ep]++] << (8 * i);
        }
        return value;
    }
    if (endpoint_offset(s, offset, &ep, &reg)) {
        if (!endpoint_width(reg, size) || reg + size > 14) {
            goto fail;
        }
        if (reg == 8) {
            return s->count[ep] & (size == 1 ? 255 : 65535);
        }
        if (reg == 9) {
            return s->count[ep] >> 8;
        }
        return size == 1 ? s->endpoint[ep][reg] :
                           lduw_le_p(s->endpoint[ep] + reg);
    }
    if (offset >= 0x80 && offset < 0xc0 && size == 1) {
        return s->target[(offset - 0x80) / 8][offset & 7];
    }
    if (size != register_width(offset)) {
        goto fail;
    }
    switch (offset) {
    case 0: return s->address;
    case 1: return s->power;
    case 2: value = s->tx_is; s->tx_is = 0; break;
    case 4: value = s->rx_is; s->rx_is = 0; break;
    case 0x0a: value = s->usb_is; s->usb_is = 0; break;
    case 6: return s->tx_ie;
    case 8: return s->rx_ie;
    case 0x0b: return s->usb_ie;
    case 0x0c: return (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1000000) & 2047;
    case 0x0e: return s->index;
    case 0x60:
        return s->session |
               (s->iddig ? 0x80 : (s->session ? 4 : 0)) |
               (s->connected ? 0x18 | ((s->power & 0x10) ? 0 : 0x40) : 0);
    case 0x62: case 0x63: return s->fifo_size[s->index][offset - 0x62];
    case 0x64: case 0x66: return s->fifo_addr[s->index][(offset - 0x64) / 2];
    default: goto fail;
    }
    update_irq(s);
    return value;
fail:
    unsupported(offset, size, false, 0);
}

static bool fifo_fits(unsigned size, unsigned address)
{
    /* TX/RX may share an allocation, as in ChryUSB's FIFO_TXRX layout. */
    return !(size & ~0x1fu) && (size & 15) <= 9 &&
           address * 8 + ((8u << (size & 15)) << ((size >> 4) & 1)) <= 4096;
}

static void write_reg(void *opaque, hwaddr offset, uint64_t value,
                      unsigned size)
{
    ListenAIMUSB *s = opaque;
    unsigned direction, ep, reg;

    if (!s->clock_enabled) {
        goto fail;
    }
    if (offset == 0x20 && (size == 1 || size == 2 || size == 4)) {
        if (s->tx_count[0] + size > 64 || (s->endpoint[0][2] & 2)) {
            goto fail;
        }
        for (unsigned i = 0; i < size; i++) {
            s->fifo[0][s->tx_count[0]++] = value >> (8 * i);
        }
        return;
    }
    if (endpoint_offset(s, offset, &ep, &reg)) {
        if (!endpoint_width(reg, size) || reg + size > 8) {
            /* Host type/interval bytes are separate from endpoint CSRs. */
            if (size != 1 || reg < 10 || reg > 13) {
                goto fail;
            }
        }
        if (!ep && reg == 2) {
            if (size != 1 || (value & 0x80) ||
                (s->iddig && (value & 0x22)) ||
                ((value & 0x22) == 0x22) ||
                ((value & 8) && (!(value & 2) || s->tx_count[0] != 8))) {
                goto fail;
            }
            if (!(value & 1)) {
                s->count[0] = s->position[0] = 0;
            }
            s->endpoint[0][2] = value;
            if (value & 0x22) {
                schedule(s);
            }
        } else if (!ep && (reg == 0 || reg == 10 || reg == 11)) {
            if (size == 1) {
                s->endpoint[ep][reg] = value;
            } else {
                stw_le_p(s->endpoint[ep] + reg, value);
            }
        } else if (ep && reg == 6 && size == 1) {
            if ((s->iddig && (value & 1)) ||
                ((value & 0x20) && (value & 1))) {
                goto fail;
            }
            if (!(value & 1) || (value & 0x10)) {
                s->count[ep] = s->position[ep] = 0;
            }
            s->endpoint[ep][6] = (s->endpoint[ep][6] & value &
                                  ((value & 0x10) ? 0x4c : 0x4d)) |
                                 (value & 0x20);
            if (value & 0x20) {
                schedule(s);
            }
        } else if (ep && reg == 4 && size == 2 && value <= 1024) {
            stw_le_p(s->endpoint[ep] + reg, value);
        } else if (ep && reg == 12 && size == 1 &&
                   (!value || ((value & 0xf0) == 0x50 && (value & 15)))) {
            s->endpoint[ep][reg] = value;
        } else if (ep && reg == 13 && size == 1 && value <= 1) {
            s->endpoint[ep][reg] = value;
        } else if (ep && (reg == 3 || reg == 7) && size == 1 && !value) {
            s->endpoint[ep][reg] = 0;
        } else {
            goto fail;
        }
        return;
    }
    if (offset >= 0x80 && offset < 0xc0 && size == 1) {
        reg = offset & 7;
        if ((reg == 0 || reg == 4) ? value > 127 : value != 0) {
            goto fail;
        }
        s->target[(offset - 0x80) / 8][reg] = value;
        return;
    }
    if (size != register_width(offset)) {
        goto fail;
    }
    switch (offset) {
    case 0:
        if (value > 127) {
            goto fail;
        }
        s->address = value;
        s->target[0][0] = value;
        break;
    case 1:
        if (s->iddig) {
            /* Device mode has no host: only SDK init/deinit controls exist. */
            if (value & ~0x61u) {
                goto fail;
            }
            s->power = value & 0x61;
            break;
        }
        if ((value & 0xc4) || (present(s) && !(value & 0x20))) {
            goto fail;
        }
        if (!(s->power & 8) && (value & 8)) {
            timer_del(s->timer);
            s->port_enabled = false;
            memset(s->endpoint, 0, sizeof(s->endpoint));
            memset(s->count, 0, sizeof(s->count));
            memset(s->position, 0, sizeof(s->position));
            memset(s->tx_count, 0, sizeof(s->tx_count));
            s->tx_is = s->rx_is = 0;
            if (present(s)) {
                usb_device_reset(s->port.dev);
            }
        }
        if ((s->power & 8) && !(value & 8) && s->connected) {
            s->port_enabled = true;
            s->power = value & 0x21;
            if (value & 0x20) {
                s->power |= 0x10;
            }
        } else {
            s->power = (value & 0x29) | (s->power & 0x10);
        }
        break;
    case 2: s->tx_is &= ~value; break;
    case 4: s->rx_is &= ~value; break;
    case 0x0a: s->usb_is &= ~value; break;
    case 6: s->tx_ie = value; break;
    case 8: s->rx_ie = value; break;
    case 0x0b: s->usb_ie = value; break;
    case 0x0e:
        if (value > 7) {
            goto fail;
        }
        s->index = value;
        break;
    case 0x60:
        if ((value & ~0xfdu) || ((value & 1) && !s->utmi16)) {
            goto fail;
        }
        s->session = value & 1;
        connection_update(s);
        if (!s->session) {
            timer_del(s->timer);
        }
        break;
    case 0x62: case 0x63:
        direction = offset - 0x62;
        if (!fifo_fits(value, s->fifo_addr[s->index][direction])) {
            goto fail;
        }
        s->fifo_size[s->index][direction] = value;
        break;
    case 0x64: case 0x66:
        direction = (offset - 0x64) / 2;
        if (!fifo_fits(s->fifo_size[s->index][direction], value)) {
            goto fail;
        }
        s->fifo_addr[s->index][direction] = value;
        break;
    default: goto fail;
    }
    update_irq(s);
    return;
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
    if (s->session && (iddig != s->iddig || utmi16 != s->utmi16)) {
        return false;
    }
    s->clock_enabled = clock_enabled;
    s->iddig = iddig;
    s->utmi16 = utmi16;
    if (!clock_enabled) {
        timer_del(s->timer);
    } else {
        schedule(s);
    }
    update_irq(s);
    return true;
}

void listenai_musb_reset(ListenAIMUSB *s)
{
    s->power = s->index = s->session = s->usb_ie = s->usb_is = s->address = 0;
    s->tx_ie = s->rx_ie = s->tx_is = s->rx_is = 0;
    s->connected = s->port_enabled = false;
    memset(s->fifo_size, 0, sizeof(s->fifo_size));
    memset(s->fifo_addr, 0, sizeof(s->fifo_addr));
    memset(s->endpoint, 0, sizeof(s->endpoint));
    memset(s->target, 0, sizeof(s->target));
    memset(s->count, 0, sizeof(s->count));
    memset(s->position, 0, sizeof(s->position));
    memset(s->tx_count, 0, sizeof(s->tx_count));
    if (s->timer) {
        timer_del(s->timer);
    }
    if (s->port.dev && s->port.dev->attached) {
        usb_device_reset(s->port.dev);
    }
    qemu_set_irq(s->irq, 0);
}

static void reset(DeviceState *dev)
{
    listenai_musb_reset(LISTENAI_MUSB(dev));
}

static void realize(DeviceState *dev, Error **errp)
{
    ListenAIMUSB *s = LISTENAI_MUSB(dev);

    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, tick, s);
    usb_bus_new(&s->bus, sizeof(s->bus), &bus_ops, dev);
    usb_register_port(&s->bus, &s->port, s, 0, &port_ops, USB_SPEED_MASK_HIGH);
    if (getenv("LISEM_QEMU_UVC_IMAGE")) {
        USBDevice *camera = USB_DEVICE(qdev_new("usb-uvc"));
        qdev_prop_set_string(DEVICE(camera), "image", getenv("LISEM_QEMU_UVC_IMAGE"));
        object_property_add_child(OBJECT(s), "camera", OBJECT(camera));
        usb_realize_and_unref(camera, &s->bus, errp);
    }
}

static void unrealize(DeviceState *dev)
{
    ListenAIMUSB *s = LISTENAI_MUSB(dev);

    usb_unregister_port(&s->bus, &s->port);
    usb_bus_release(&s->bus);
    timer_free(s->timer);
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
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = realize;
    dc->unrealize = unrealize;
    device_class_set_legacy_reset(dc, reset);
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
