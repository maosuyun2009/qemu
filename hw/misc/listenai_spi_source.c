/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Generic external SPI signal fixture.  This is deliberately a signal source,
 * not a camera, packet parser, or board protocol implementation.
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "hw/qdev-core.h"
#include "hw/qdev-properties.h"
#include "hw/irq.h"
#include "qom/object.h"
#include "system/reset.h"

#define TYPE_LISTENAI_SPI_SOURCE "listenai-spi-source"
#define SPI_SOURCE_HEADER_SIZE 8
#define SPI_SOURCE_FILE_MAX (32 * 1024 * 1024)
#define SPI_SOURCE_RECORD_MAX (2 * 1024 * 1024)
#ifndef OBJECT_DECLARE_SIMPLE_TYPE /* cppcheck fallback; QEMU defines the real macro. */
#define OBJECT_DECLARE_SIMPLE_TYPE(type, name)
#endif
OBJECT_DECLARE_SIMPLE_TYPE(ListenAISource, LISTENAI_SPI_SOURCE)

typedef struct ListenAISource {
    DeviceState parent_obj;

    char *sink;
    char *file;
    uint32_t clock_pad;
    uint32_t mosi_pad;
    uint32_t cs_pad;
    uint32_t clock_hz;
    uint32_t mode;
    uint64_t delay_ns;
    uint64_t gap_ns;
    uint64_t period_ns;
    uint64_t repeats;

    uint8_t *data;
    size_t length;
    uint64_t record_count;
    uint64_t serialized_ns;
    size_t next_record;
    size_t record_offset;
    uint32_t record_length;
    uint64_t record_index;
    uint64_t edge_index;
    uint64_t cycle_start;
    uint64_t record_start;
    uint64_t cycles;
    uint64_t transactions;
    uint64_t bytes;
    bool waiting_record;
    bool running;

    qemu_irq clock;
    qemu_irq mosi;
    qemu_irq cs;
    QEMUTimer *timer;
} ListenAISource;

static uint64_t ceil_div_u64(uint64_t value, uint64_t divisor)
{
    return value / divisor + !!(value % divisor);
}

static bool find_gpio_count(DeviceState *dev, const char *name,
                            unsigned *count)
{
    NamedGPIOList *list;

    QLIST_FOREACH(list, &dev->gpios, node) {
        if (!g_strcmp0(list->name, name)) {
            *count = list->num_in;
            return true;
        }
    }
    return false;
}

static bool parse_records(const uint8_t *data, size_t length,
                          uint32_t clock_hz, uint64_t gap_ns,
                          uint64_t *records, uint64_t *serialized_ns,
                          Error **errp)
{
    size_t position = SPI_SOURCE_HEADER_SIZE;
    uint64_t count = 0;
    __uint128_t serial = 0;

    if (length < SPI_SOURCE_HEADER_SIZE ||
        memcmp(data, "LSPI0001", SPI_SOURCE_HEADER_SIZE)) {
        error_setg(errp, "SPI source file must start with ASCII LSPI0001");
        return false;
    }
    while (position < length) {
        uint32_t record_length;
        uint64_t bits;
        uint64_t duration;

        if (length - position < sizeof(record_length)) {
            error_setg(errp, "SPI source file has a truncated record length");
            return false;
        }
        record_length = ldl_le_p(data + position);
        position += sizeof(record_length);
        if (!record_length || record_length > SPI_SOURCE_RECORD_MAX ||
            record_length > length - position) {
            error_setg(errp, "SPI source record length is invalid or truncated");
            return false;
        }
        bits = (uint64_t)record_length * 8;
        duration = ceil_div_u64(bits * NANOSECONDS_PER_SECOND, clock_hz);
        serial += duration;
        if (serial > UINT64_MAX) {
            error_setg(errp, "SPI source serialized duration overflows");
            return false;
        }
        position += record_length;
        count++;
    }
    if (!count) {
        error_setg(errp, "SPI source file must contain at least one record");
        return false;
    }

    __uint128_t cycle = serial + (__uint128_t)gap_ns * count;
    if (cycle > UINT64_MAX) {
        error_setg(errp, "SPI source cycle duration overflows");
        return false;
    }
    *records = count;
    *serialized_ns = (uint64_t)serial;
    return true;
}

static uint64_t edge_time(uint64_t edge, uint32_t clock_hz)
{
    return ceil_div_u64(edge * NANOSECONDS_PER_SECOND,
                        (uint64_t)clock_hz * 2);
}

static bool cpol(const ListenAISource *s)
{
    return s->mode & 2;
}

static bool current_bit(const ListenAISource *s)
{
    uint64_t bit = s->edge_index / 2;
    return (s->data[s->record_offset + bit / 8] >> (7 - bit % 8)) & 1;
}

static void idle_outputs(ListenAISource *s)
{
    qemu_set_irq(s->cs, 1);
    qemu_set_irq(s->clock, cpol(s));
    qemu_set_irq(s->mosi, 0);
}

static void stop_source(ListenAISource *s)
{
    s->running = false;
    s->waiting_record = false;
    timer_del(s->timer);
    idle_outputs(s);
}

static bool deadline_add(uint64_t base, uint64_t delta, uint64_t *result)
{
    if (base > INT64_MAX || delta > INT64_MAX - base) {
        return false;
    }
    *result = base + delta;
    return true;
}

static void start_record(ListenAISource *s, uint64_t now)
{
    uint64_t deadline;

    s->record_offset = s->next_record + sizeof(uint32_t);
    s->record_length = ldl_le_p(s->data + s->next_record);
    s->next_record += sizeof(uint32_t) + s->record_length;
    s->edge_index = 0;
    s->waiting_record = false;
    s->record_start = now;

    qemu_set_irq(s->cs, 0);
    qemu_set_irq(s->clock, cpol(s));
    qemu_set_irq(s->mosi, current_bit(s));
    if (!deadline_add(now, edge_time(1, s->clock_hz), &deadline)) {
        error_report("SPI source bit deadline overflow"); stop_source(s); return;
    }
    timer_mod(s->timer, deadline);
}

static void source_step(void *opaque)
{
    ListenAISource *s = opaque;
    uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t edge_count = (uint64_t)s->record_length * 8 * 2;
    uint64_t deadline;

    if (!s->running) {
        return;
    }
    if (s->waiting_record) {
        start_record(s, now);
        return;
    }

    s->edge_index++;
    qemu_set_irq(s->clock, cpol(s) ^ (s->edge_index & 1));
    if (!(s->edge_index & 1)) {
        /* The trailing edge completes the bit and ends the CS transaction. */
        if (s->edge_index == edge_count) {
            qemu_set_irq(s->clock, cpol(s));
            qemu_set_irq(s->cs, 1);
            s->transactions++;
            s->bytes += s->record_length;
            s->record_index++;
            if (s->record_index < s->record_count) {
                s->waiting_record = true;
                if (!deadline_add(now, s->gap_ns, &deadline)) {
                    error_report("SPI source deadline overflow");
                    stop_source(s);
                    return;
                }
                timer_mod(s->timer, deadline);
                return;
            }

            s->cycles++;
            if (s->repeats && s->cycles >= s->repeats) {
                stop_source(s);
                return;
            }
            if (!deadline_add(s->cycle_start, s->period_ns,
                              &s->cycle_start)) {
                error_report("SPI source cycle deadline overflow");
                stop_source(s);
                return;
            }
            s->record_index = 0;
            s->next_record = SPI_SOURCE_HEADER_SIZE;
            s->waiting_record = true;
            timer_mod(s->timer, s->cycle_start);
            return;
        }
        qemu_set_irq(s->mosi, current_bit(s));
    }

    if (!deadline_add(s->record_start, edge_time(s->edge_index + 1, s->clock_hz), &deadline)) {
        error_report("SPI source bit deadline overflow"); stop_source(s); return;
    }
    timer_mod(s->timer, deadline);
}

static void source_reset(DeviceState *dev)
{
    ListenAISource *s = LISTENAI_SPI_SOURCE(dev);
    uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    timer_del(s->timer);
    s->next_record = SPI_SOURCE_HEADER_SIZE;
    s->record_index = 0;
    s->edge_index = 0;
    s->cycle_start = 0;
    s->record_start = 0;
    s->cycles = 0;
    s->transactions = 0;
    s->bytes = 0;
    s->running = true;
    s->waiting_record = true;
    idle_outputs(s);
    if (!deadline_add(now, s->delay_ns, &s->cycle_start)) {
        error_report("SPI source reset deadline overflow");
        s->running = false;
        return;
    }
    timer_mod(s->timer, s->cycle_start);
}

static char *source_stats(Object *obj, Error **errp)
{
    ListenAISource *s = LISTENAI_SPI_SOURCE(obj);
    uint64_t time = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    return g_strdup_printf(
        "{\"active\":%s,\"transactions\":%" PRIu64
        ",\"bytes\":%" PRIu64 ",\"cycles\":%" PRIu64
        ",\"time-ns\":%" PRIu64 ",\"records\":%" PRIu64
        ",\"delivered-physical-bytes\":%" PRIu64 "}",
        s->running ? "true" : "false", s->transactions, s->bytes,
        s->cycles, time, s->record_count, s->bytes);
}

static void source_realize(DeviceState *dev, Error **errp)
{
    ListenAISource *s = LISTENAI_SPI_SOURCE(dev);
    Object *object;
    DeviceState *sink;
    struct stat statbuf;
    unsigned pad_count = 0;
    uint8_t *data = NULL;
    gsize length;
    GError *gerror = NULL;
    uint64_t records, serialized_ns;
    __uint128_t cycle;

    if (!s->sink || !s->file || s->clock_hz < 1 || s->clock_hz > 24000000 ||
        s->mode > 3 || s->period_ns > INT64_MAX || s->delay_ns > INT64_MAX ||
        s->gap_ns > INT64_MAX) {
        error_setg(errp, "SPI source has invalid sink, mode, clock or timing");
        return;
    }
    if (s->clock_pad == s->mosi_pad || s->clock_pad == s->cs_pad ||
        s->mosi_pad == s->cs_pad) {
        error_setg(errp, "SPI source clock, MOSI and CS pads must be distinct");
        return;
    }

    object = object_resolve_path(s->sink, NULL);
    if (!object || !object_dynamic_cast(object, TYPE_DEVICE)) {
        error_setg(errp, "SPI source sink is not a device: %s", s->sink);
        return;
    }
    sink = DEVICE(object);
    if (!find_gpio_count(sink, "pad-in", &pad_count) ||
        s->clock_pad >= pad_count || s->mosi_pad >= pad_count ||
        s->cs_pad >= pad_count) {
        error_setg(errp, "SPI source pad is outside sink pad-in[%u]", pad_count);
        return;
    }

    if (stat(s->file, &statbuf) || !S_ISREG(statbuf.st_mode) ||
        statbuf.st_size <= SPI_SOURCE_HEADER_SIZE ||
        statbuf.st_size > SPI_SOURCE_FILE_MAX) {
        error_setg(errp, "SPI source file must be a regular file up to 32 MiB");
        return;
    }
    if (!g_file_get_contents(s->file, (char **)&data, &length, &gerror)) {
        error_setg(errp, "read SPI source file: %s", gerror->message);
        g_error_free(gerror);
        return;
    }
    if (length > SPI_SOURCE_FILE_MAX || length != statbuf.st_size) {
        error_setg(errp, "SPI source file changed size while loading");
        g_free(data);
        return;
    }
    if (!parse_records(data, length, s->clock_hz, s->gap_ns, &records,
                       &serialized_ns, errp)) {
        g_free(data);
        return;
    }
    cycle = (__uint128_t)serialized_ns + (__uint128_t)s->gap_ns * records;
    if (cycle > INT64_MAX || s->period_ns < (uint64_t)cycle ||
        s->delay_ns > INT64_MAX - (uint64_t)cycle) {
        error_setg(errp, "SPI source period is shorter than one whole-file cycle");
        g_free(data);
        return;
    }

    s->data = data;
    s->length = length;
    s->record_count = records;
    s->serialized_ns = serialized_ns;
    s->clock = qdev_get_gpio_in_named(sink, "pad-in", s->clock_pad);
    s->mosi = qdev_get_gpio_in_named(sink, "pad-in", s->mosi_pad);
    s->cs = qdev_get_gpio_in_named(sink, "pad-in", s->cs_pad);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, source_step, s);
    qemu_register_resettable(OBJECT(s));
}

static void source_unrealize(DeviceState *dev)
{
    ListenAISource *s = LISTENAI_SPI_SOURCE(dev);

    qemu_unregister_resettable(OBJECT(s));
    timer_free(s->timer);
    s->timer = NULL;
    g_clear_pointer(&s->data, g_free);
    s->length = 0;
}

static void source_init(Object *obj)
{
    object_property_add_str(obj, "x-lisem-spi-source", source_stats, NULL);
}

static const Property source_properties[] = {
    DEFINE_PROP_STRING("sink", ListenAISource, sink),
    DEFINE_PROP_STRING("file", ListenAISource, file),
    DEFINE_PROP_UINT32("clock-pad", ListenAISource, clock_pad, 0),
    DEFINE_PROP_UINT32("mosi-pad", ListenAISource, mosi_pad, 1),
    DEFINE_PROP_UINT32("cs-pad", ListenAISource, cs_pad, 2),
    DEFINE_PROP_UINT32("clock-hz", ListenAISource, clock_hz, 1000000),
    DEFINE_PROP_UINT32("mode", ListenAISource, mode, 0),
    DEFINE_PROP_UINT64("delay-ns", ListenAISource, delay_ns, 1000000000),
    DEFINE_PROP_UINT64("gap-ns", ListenAISource, gap_ns, 1000000),
    DEFINE_PROP_UINT64("period-ns", ListenAISource, period_ns, 100000000),
    DEFINE_PROP_UINT64("repeats", ListenAISource, repeats, 0),
};

static void source_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = source_realize;
    dc->unrealize = source_unrealize;
    dc->user_creatable = true;
    dc->hotpluggable = false;
    device_class_set_legacy_reset(dc, source_reset);
    device_class_set_props(dc, source_properties);
    dc->desc = "Generic external SPI diagnostic signal source";
}

static const TypeInfo source_info = {
    .name = TYPE_LISTENAI_SPI_SOURCE,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(ListenAISource),
    .instance_init = source_init,
    .class_init = source_class_init,
};

static void source_register_types(void)
{
    type_register_static(&source_info);
}

type_init(source_register_types)
