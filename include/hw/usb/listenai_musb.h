/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_USB_LISTENAI_MUSB_H
#define HW_USB_LISTENAI_MUSB_H

#include "hw/sysbus.h"
#include "hw/usb.h"
#include "qemu/timer.h"

#define TYPE_LISTENAI_MUSB "listenai-musb-empty-host"
OBJECT_DECLARE_SIMPLE_TYPE(ListenAIMUSB, LISTENAI_MUSB)

/* Eight-pipe MUSB host, with a single direct-attached USB port. */
struct ListenAIMUSB {
    SysBusDevice parent_obj;
    MemoryRegion io;
    qemu_irq irq;
    USBBus bus;
    USBPort port;
    QEMUTimer *timer;
    uint8_t power, index, session, usb_ie, usb_is, address;
    uint16_t tx_ie, rx_ie, tx_is, rx_is;
    uint8_t endpoint[8][16], target[8][8];
    uint8_t fifo[8][2048];
    uint16_t count[8], position[8], tx_count[8];
    bool connected, port_enabled;
    uint8_t fifo_size[8][2];
    uint16_t fifo_addr[8][2];
    bool clock_enabled, iddig, utmi16;
};

/* Reject incompatible role/PHY inputs during SESSION without changing state. */
bool listenai_musb_configure(ListenAIMUSB *s, bool clock_enabled,
                            bool iddig, bool utmi16);
void listenai_musb_reset(ListenAIMUSB *s);

#endif
