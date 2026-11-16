/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_RISCV_VENUSA_JPEG_H
#define HW_RISCV_VENUSA_JPEG_H

#include "hw/sysbus.h"
#include "qemu/timer.h"

#define TYPE_VENUSA_JPEG "venusa-jpeg"
#ifndef OBJECT_DECLARE_SIMPLE_TYPE /* cppcheck fallback; QEMU defines the real macro. */
#define OBJECT_DECLARE_SIMPLE_TYPE(type, name) typedef struct type type
#endif
OBJECT_DECLARE_SIMPLE_TYPE(VenusAJPEG, VENUSA_JPEG)
#define VENUSA_JPEG_BASE 0x42001800
#define VENUSA_JPEG_PIXEL 0x42002800
#define VENUSA_JPEG_ECS 0x42003000
#define VENUSA_JPEG_MAX_SOURCE 32768
#define VENUSA_JPEG_MAX_PIXELS (640 * 480 * 3)

typedef struct VenusAJPEGWindow {
    MemoryRegion io;
    VenusAJPEG *jpeg;
    unsigned offset;
} VenusAJPEGWindow;

struct VenusAJPEG {
    SysBusDevice parent_obj;
    VenusAJPEGWindow windows[9];
    qemu_irq irq;
    uint32_t regs[0x6000 / 4];
    void (*fault)(void *opaque, hwaddr address, unsigned size,
                  bool write, uint64_t value);
    void *fault_opaque;
    void (*request_changed)(void *opaque);
    QEMUTimer *timer;
    int64_t remaining_ns;
    uint8_t input[VENUSA_JPEG_MAX_PIXELS + 4];
    uint8_t *output;
    uint32_t received, consumed, output_size;
    uint64_t decoded_frames, output_bytes;
    bool active, input_started, output_started, decoded, failed, clock, pending;
};

bool venusa_jpeg_encoding(const VenusAJPEG *s);
bool venusa_jpeg_request(VenusAJPEG *s, bool write);
void venusa_jpeg_reset(VenusAJPEG *s);
void venusa_jpeg_clock(VenusAJPEG *s, bool enabled);
#endif
