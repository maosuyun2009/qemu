/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_USB_LISTENAI_MUSB_H
#define HW_USB_LISTENAI_MUSB_H

#include "hw/sysbus.h"

#define TYPE_LISTENAI_MUSB "listenai-musb-empty-host"
OBJECT_DECLARE_SIMPLE_TYPE(ListenAIMUSB, LISTENAI_MUSB)

/* Init-only MUSB host with an empty port; no USB bus or transfer engine. */
struct ListenAIMUSB {
    SysBusDevice parent_obj;
    MemoryRegion io;
    qemu_irq irq;
    uint8_t power, index, session, usb_ie;
    uint16_t tx_ie, rx_ie;
    uint8_t fifo_size[8][2];
    uint16_t fifo_addr[8][2];
    bool clock_enabled, iddig, utmi16;
};

/* Reject incompatible role/PHY inputs during SESSION without changing state. */
bool listenai_musb_configure(ListenAIMUSB *s, bool clock_enabled,
                            bool iddig, bool utmi16);
void listenai_musb_reset(ListenAIMUSB *s);

#endif
