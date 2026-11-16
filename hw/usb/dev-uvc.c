/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Deterministic UVC 1.0 MJPEG source. */
#include "qemu/osdep.h"
#include "hw/usb.h"
#include "qemu/module.h"
#include "qemu/bswap.h"
#include "qemu/timer.h"
#include "hw/qdev-properties.h"
#include <jpeglib.h>
#include <setjmp.h>
#include "desc.h"

#define TYPE_USB_UVC "usb-uvc"
#define FRAME_INTERVAL_NS 50000000
OBJECT_DECLARE_SIMPLE_TYPE(USBUVC, USB_UVC)

struct USBUVC {
    USBDevice dev;
    uint8_t probe[26], commit[26];
    bool committed;
    char *image;
    uint8_t jpeg[32769];
    unsigned jpeg_size;
    QEMUTimer *frame_timer;
    bool streaming;
    unsigned offset;
    uint8_t fid;
    uint64_t frames, completed_frames, bytes, dropped_frames;
    int64_t frame_ns, next_frame_ns;
};

#define U16(v) ((v) & 255), (((v) >> 8) & 255)
#define U32(v) U16(v), U16((v) >> 16)

static const USBDescIface interfaces[] = {
    {
        .bInterfaceNumber = 0,
        .bInterfaceClass = 14,
        .bInterfaceSubClass = 1,
        .ndesc = 3,
        .descs = (USBDescOther[]) {
            { .data = (uint8_t[]) {
                /* VC header: UVC 1.0, 48 MHz clock, VS interface 1. */
                13, 0x24, 1, U16(0x0100), U16(40), U32(48000000), 1, 1,
            } },
            { .data = (uint8_t[]) {
                /* Camera input terminal 1; no optional controls. */
                18, 0x24, 2, 1, U16(0x0201), 0, 0,
                U16(0), U16(0), U16(0), 3, 0, 0, 0,
            } },
            { .data = (uint8_t[]) {
                /* USB streaming output terminal 2, sourced from camera 1. */
                9, 0x24, 3, 2, U16(0x0101), 0, 1, 0,
            } },
        },
    }, {
        .bInterfaceNumber = 1,
        .bInterfaceClass = 14,
        .bInterfaceSubClass = 2,
        .ndesc = 4,
        .descs = (USBDescOther[]) {
            { .data = (uint8_t[]) {
                /* VS input header, endpoint 0x81, terminal 2. */
                14, 0x24, 1, 1, U16(61), 0x81, 0, 2, 0, 0, 0, 1, 0,
            } },
            { .data = (uint8_t[]) {
                11, 0x24, 6, 1, 1, 1, 1, 0, 0, 0, 0,
            } },
            { .data = (uint8_t[]) {
                /* MJPEG 640x480, one discrete interval: 20 fps. */
                30, 0x24, 7, 1, 0, U16(640), U16(480),
                U32(5242880), U32(5242880), U32(32768),
                U32(500000), 1, U32(500000),
            } },
            { .data = (uint8_t[]) { 6, 0x24, 13, 1, 1, 4 } },
        },
    }, {
        .bInterfaceNumber = 1,
        .bAlternateSetting = 1,
        .bNumEndpoints = 1,
        .bInterfaceClass = 14,
        .bInterfaceSubClass = 2,
        .eps = (USBDescEndpoint[]) {
            {
                .bEndpointAddress = 0x81,
                .bmAttributes = USB_ENDPOINT_XFER_ISOC,
                .wMaxPacketSize = 1024,
                .bInterval = 1,
            },
        },
    },
};

static const USBDescDevice device = {
    .bcdUSB = 0x0200,
    .bDeviceClass = 0xef,
    .bDeviceSubClass = 2,
    .bDeviceProtocol = 1,
    .bMaxPacketSize0 = 64,
    .bNumConfigurations = 1,
    .confs = (USBDescConfig[]) {
        {
            .bNumInterfaces = 2,
            .bConfigurationValue = 1,
            .bmAttributes = USB_CFG_ATT_ONE,
            .bMaxPower = 50,
            .nif_groups = 1,
            .if_groups = (USBDescIfaceAssoc[]) {
                {
                    .bFirstInterface = 0,
                    .bInterfaceCount = 2,
                    .bFunctionClass = 14,
                    .bFunctionSubClass = 3,
                    .nif = ARRAY_SIZE(interfaces),
                    .ifs = interfaces,
                },
            },
        },
    },
};

static const USBDesc descriptor = {
    .id = {
        .idVendor = 0x46f4, /* QEMU */
        .idProduct = 0x0010,
        .bcdDevice = 0x0100,
        .iManufacturer = 1,
        .iProduct = 2,
        .iSerialNumber = 3,
    },
    .high = &device,
    .str = (USBDescStrings) {
        [1] = "QEMU", [2] = "QEMU UVC MJPEG Camera", [3] = "1",
    },
};

static void default_probe(uint8_t *data)
{
    memset(data, 0, 26);
    data[2] = data[3] = 1;
    stl_le_p(data + 4, 500000);
    stl_le_p(data + 18, 32768);
    stl_le_p(data + 22, 1024);
}

static bool streaming_active(USBUVC *s)
{
    USBDevice *dev = &s->dev;

    return s->streaming && s->committed && dev->attached &&
           dev->state != USB_STATE_NOTATTACHED &&
           dev->configuration == 1 && dev->altsetting[1] == 1;
}

static void stop_stream(USBUVC *s)
{
    timer_del(s->frame_timer);
    s->dropped_frames += s->streaming && s->offset < s->jpeg_size;
    s->streaming = false;
    s->offset = s->jpeg_size;
    s->next_frame_ns = 0;
}

static void frame_tick(void *opaque)
{
    USBUVC *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t due;

    if (!streaming_active(s)) {
        stop_stream(s);
        s->committed = false;
        return;
    }
    due = (now - s->next_frame_ns) / FRAME_INTERVAL_NS + 1;
    s->frames += due;
    /* Keep only the newest scheduled frame, including after a clock jump. */
    s->dropped_frames += due - 1 + (s->offset < s->jpeg_size);
    s->fid ^= due & 1;
    s->offset = 0;
    s->frame_ns = s->next_frame_ns + (due - 1) * FRAME_INTERVAL_NS;
    s->next_frame_ns = s->frame_ns + FRAME_INTERVAL_NS;
    timer_mod(s->frame_timer, s->next_frame_ns);
}

static void set_interface(USBDevice *dev, int iface, int old, int value)
{
    USBUVC *s = USB_UVC(dev);

    if (iface != 1) {
        return;
    }
    stop_stream(s);
    if (value == 1 && s->committed) {
        s->streaming = true;
        s->next_frame_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                           FRAME_INTERVAL_NS;
        timer_mod(s->frame_timer, s->next_frame_ns);
    }
}

static void reset(USBDevice *dev)
{
    USBUVC *s = USB_UVC(dev);

    stop_stream(s);
    s->fid = 0;
    default_probe(s->probe);
    default_probe(s->commit);
    s->committed = false;
}

static void bus_reset(USBDevice *dev)
{
    reset(dev);
    /* USB core's reset helper does not abort a partially completed EP0 setup. */
    dev->setup_state = 0;
    dev->setup_len = dev->setup_index = 0;
    usb_desc_attach(dev);
}

static void control(USBDevice *dev, USBPacket *p, int request,
                    int value, int index, int length, uint8_t *data)
{
    USBUVC *s = USB_UVC(dev);
    uint8_t *cur;

    if (request == (DeviceOutRequest | USB_REQ_SET_CONFIGURATION) &&
        value != 0 && value != 1) {
        goto stall;
    }
    if (request == (InterfaceOutRequest | USB_REQ_SET_INTERFACE) &&
        (dev->configuration != 1 ||
         (index == 1 && value == 1 && !s->committed))) {
        goto stall;
    }
    if (usb_desc_handle_control(dev, p, request, value, index,
                                length, data) >= 0) {
        if (request == (DeviceOutRequest | USB_REQ_SET_CONFIGURATION)) {
            reset(dev);
        }
        return;
    }
    if (index != 1 || (value != 0x100 && value != 0x200)) {
        goto stall;
    }
    cur = value == 0x100 ? s->probe : s->commit;
    switch (request) {
    case ClassInterfaceOutRequest | 1: /* SET_CUR */
        if (length != 26 || lduw_le_p(data) & ~1 || data[2] != 1 ||
            data[3] != 1 || ldl_le_p(data + 4) != 500000) {
            goto stall;
        }
        default_probe(cur);
        stw_le_p(cur, lduw_le_p(data));
        if (value == 0x200) {
            s->committed = true;
        }
        return;
    case ClassInterfaceRequest | 0x81: /* GET_CUR */
        memcpy(data, cur, MIN(length, 26));
        p->actual_length = MIN(length, 26);
        return;
    case ClassInterfaceRequest | 0x82: /* GET_MIN */
    case ClassInterfaceRequest | 0x83: /* GET_MAX */
    case ClassInterfaceRequest | 0x87: /* GET_DEF */
        default_probe(data);
        p->actual_length = MIN(length, 26);
        return;
    case ClassInterfaceRequest | 0x84: /* GET_RES: single discrete mode. */
        memset(data, 0, 26);
        p->actual_length = MIN(length, 26);
        return;
    case ClassInterfaceRequest | 0x85: /* GET_LEN */
        stw_le_p(data, 26);
        p->actual_length = MIN(length, 2);
        return;
    case ClassInterfaceRequest | 0x86: /* GET_INFO */
        data[0] = 3;
        p->actual_length = MIN(length, 1);
        return;
    }
stall:
    p->status = USB_RET_STALL;
}

static void handle_data(USBDevice *dev, USBPacket *p)
{
    USBUVC *s = USB_UVC(dev);
    uint8_t header[2] = { 2, 0x80 | s->fid };
    size_t capacity = MIN(p->iov.size, 1024);
    unsigned n;

    if (p->pid != USB_TOKEN_IN || p->ep->nr != 1 || !streaming_active(s)) {
        p->status = USB_RET_STALL;
        return;
    }
    p->status = USB_RET_SUCCESS;
    p->actual_length = 0;
    if (s->offset == s->jpeg_size || capacity < sizeof(header)) {
        return;
    }
    n = MIN(capacity - sizeof(header), s->jpeg_size - s->offset);
    if (s->offset + n == s->jpeg_size) {
        header[1] |= 2; /* EOF */
        s->completed_frames++;
    }
    usb_packet_copy(p, header, sizeof(header));
    usb_packet_copy(p, s->jpeg + s->offset, n);
    s->offset += n;
    s->bytes += n;
}

static char *get_state(Object *obj, Error **errp)
{
    USBUVC *s = USB_UVC(obj);
    bool active = streaming_active(s);

    return g_strdup_printf("{\"streaming\":%s,\"frames\":%" PRIu64
        ",\"completed_frames\":%" PRIu64 ",\"bytes\":%" PRIu64
        ",\"dropped_frames\":%" PRIu64 ",\"frame_ns\":%" PRId64
        ",\"next_frame_ns\":%" PRId64 "}", active ? "true" : "false",
        s->frames, s->completed_frames, s->bytes, s->dropped_frames,
        s->frame_ns, active ? s->next_frame_ns : 0);
}

static void unrealize(USBDevice *dev)
{
    USBUVC *s = USB_UVC(dev);

    stop_stream(s);
    timer_free(s->frame_timer);
    s->frame_timer = NULL;
}

typedef struct JPEGValidator {
    struct jpeg_decompress_struct codec;
    struct jpeg_error_mgr error;
    jmp_buf jump;
} JPEGValidator;

static void jpeg_error(j_common_ptr codec)
{
    JPEGValidator *v = codec->client_data;

    longjmp(v->jump, 1);
}

static void jpeg_message(j_common_ptr codec, int level)
{
    /* Truncation and corrupt entropy warnings are not valid source images. */
    if (level < 0) {
        jpeg_error(codec);
    }
}

static bool baseline_sof(const uint8_t *data, unsigned size)
{
    unsigned i = 2;

    while (i + 4 <= size) {
        if (data[i++] != 0xff) {
            return false;
        }
        while (i < size && data[i] == 0xff) {
            i++;
        }
        if (i + 3 > size) {
            return false;
        }
        unsigned marker = data[i++];
        unsigned length = lduw_be_p(data + i);
        if (length < 2 || length > size - i) {
            return false;
        }
        if (marker >= 0xc0 && marker <= 0xcf && marker != 0xc4 &&
            marker != 0xc8 && marker != 0xcc) {
            return marker == 0xc0;
        }
        i += length;
    }
    return false;
}

static bool validate_jpeg(USBUVC *s)
{
    g_autofree JPEGValidator *v = g_new0(JPEGValidator, 1);
    uint8_t row[640 * 3] = { 0 };
    JSAMPROW rows[] = { row };
    bool valid = false;

    if (s->jpeg_size < 4 || s->jpeg_size > 32768 ||
        lduw_be_p(s->jpeg) != 0xffd8 ||
        lduw_be_p(s->jpeg + s->jpeg_size - 2) != 0xffd9 ||
        !baseline_sof(s->jpeg, s->jpeg_size)) {
        return false;
    }
    v->codec.err = jpeg_std_error(&v->error);
    v->error.error_exit = jpeg_error;
    v->error.emit_message = jpeg_message;
    v->codec.client_data = v;
    if (setjmp(v->jump)) {
        jpeg_destroy_decompress(&v->codec);
        return false;
    }
    jpeg_create_decompress(&v->codec);
    jpeg_mem_src(&v->codec, s->jpeg, s->jpeg_size);
    if (jpeg_read_header(&v->codec, true) == JPEG_HEADER_OK &&
        v->codec.image_width == 640 && v->codec.image_height == 480 &&
        v->codec.data_precision == 8 && !v->codec.progressive_mode &&
        !v->codec.arith_code &&
        (v->codec.num_components == 1 || v->codec.num_components == 3)) {
        v->codec.out_color_space = JCS_RGB;
        jpeg_start_decompress(&v->codec);
        if (v->codec.output_width != 640 || v->codec.output_height != 480 ||
            v->codec.output_components != 3) {
            jpeg_destroy_decompress(&v->codec);
            return false;
        }
        while (v->codec.output_scanline < v->codec.output_height) {
            jpeg_read_scanlines(&v->codec, rows, 1);
        }
        valid = jpeg_finish_decompress(&v->codec);
    }
    jpeg_destroy_decompress(&v->codec);
    return valid;
}

static void realize(USBDevice *dev, Error **errp)
{
    USBUVC *s = USB_UVC(dev);
    FILE *file = s->image ? fopen(s->image, "rb") : NULL;
    bool valid = false;

    if (file) {
        s->jpeg_size = fread(s->jpeg, 1, sizeof(s->jpeg), file);
        valid = !ferror(file) && validate_jpeg(s);
        fclose(file);
    }
    if (!valid) {
        error_setg(errp, "UVC image '%s' must be a valid baseline 640x480 JPEG "
                   "with SOI/EOI and at most 32768 bytes", s->image ?: "");
        return;
    }
    s->frame_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, frame_tick, s);
    usb_desc_init(dev);
    reset(dev);
}

static const Property properties[] = {
    DEFINE_PROP_STRING("image", USBUVC, image),
};

static void class_init(ObjectClass *klass, const void *data)
{
    USBDeviceClass *uc = USB_DEVICE_CLASS(klass);

    uc->product_desc = "QEMU UVC MJPEG Camera";
    uc->usb_desc = &descriptor;
    uc->realize = realize;
    uc->handle_attach = bus_reset;
    uc->handle_control = control;
    uc->handle_reset = bus_reset;
    uc->handle_data = handle_data;
    uc->set_interface = set_interface;
    uc->unrealize = unrealize;
    object_class_property_add_str(klass, "x-lisem-uvc-state", get_state, NULL);
    device_class_set_props(DEVICE_CLASS(klass), properties);
}

static const TypeInfo uvc_type = {
    .name = TYPE_USB_UVC,
    .parent = TYPE_USB_DEVICE,
    .instance_size = sizeof(USBUVC),
    .class_init = class_init,
};

static void register_types(void)
{
    type_register_static(&uvc_type);
}
type_init(register_types)
