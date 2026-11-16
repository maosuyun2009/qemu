/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_DMA_VENUSA_DMA2D_H
#define HW_DMA_VENUSA_DMA2D_H
#include "system/memory.h"
#include "qemu/timer.h"
#include "qemu/int128.h"
#define VENUSA_DMA2D_CHANNELS 6

typedef struct VenusADMA2DCallbacks {
    void *opaque;
    void (*irq)(void *opaque, unsigned channel, bool level);
    void (*fail)(void *opaque, hwaddr address, unsigned size, bool write, uint64_t value);
    bool (*memory_allowed)(void *opaque, hwaddr address, unsigned size, bool write);
    /* Peripheral request mux and FIFO readiness, checked for every beat. */
    bool (*request)(void *opaque, unsigned handshake, hwaddr address, bool write);
} VenusADMA2DCallbacks;

typedef struct VenusADMA2DDescriptor {
    uint32_t control, src, dst, length, irq_enable, block_mask;
    uint32_t sg_num, sg_itv, ds_num, ds_itv;
    uint32_t jpeg_width, jpeg_stride;
    uint16_t in_width, in_height, out_width, out_height;
    uint16_t in_stride, out_stride, phase_x, phase_y;
    uint16_t start_x, start_y;
    uint8_t in_format, out_format, in_yuv;
    uint8_t merge_x, merge_y;
    bool scaler, converter, in_rgb, out_rgb, last_tile, last_band;
} VenusADMA2DDescriptor;

typedef struct VenusADMA2DChannel {
    VenusADMA2DDescriptor working, queued;
    uint32_t transferred, raw, src_cursor, dst_cursor;
    uint8_t scaler_pixel[3];
    uint8_t packed[4], packed_left, packed_pos;
    uint8_t image_pending[8], image_pending_len, image_pending_pos;
    uint8_t image_stream[8], image_stream_len;
    uint32_t image_pending_dst;
    uint32_t image_pixels, image_in_bytes, image_out_bytes;
    bool active, pending;
} VenusADMA2DChannel;

typedef struct VenusADMA2D {
    MemoryRegion io;
    hwaddr base;
    VenusADMA2DCallbacks cb;
    uint32_t regs[0x504 / 4];
    VenusADMA2DChannel channels[VENUSA_DMA2D_CHANNELS];
    QEMUTimer *event;
    uint64_t clock_n, clock_d;
    Int128 clock_work;
    int64_t event_epoch;
    bool clock_enabled, servicing;
} VenusADMA2D;

void venusa_dma2d_init(VenusADMA2D *s, Object *owner, hwaddr base, VenusADMA2DCallbacks cb);
void venusa_dma2d_reset(VenusADMA2D *s);
void venusa_dma2d_clock(VenusADMA2D *s, bool enabled, uint64_t numerator, uint64_t denominator);
void venusa_dma2d_request(VenusADMA2D *s);
#endif
