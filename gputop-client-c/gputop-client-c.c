/*
 * GPU Top
 *
 * Copyright (C) 2015 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#define _GNU_SOURCE

#ifdef EMSCRIPTEN
#include <emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>
#include <inttypes.h>
#include <string.h>
#include <sys/types.h>

#include <intel_chipset.h>
#include <i915_oa_drm.h>

#include "gputop-client-c.h"
#include "gputop-client-c-runtime.h"
#include "gputop-util.h"

#include "oa-hsw.h"
#include "oa-bdw.h"
#include "oa-chv.h"
#include "oa-sklgt2.h"
#include "oa-sklgt3.h"
#include "oa-sklgt4.h"
#include "oa-bxt.h"
#include "oa-kblgt2.h"
#include "oa-kblgt3.h"
#include "oa-glk.h"

#define PERF_RECORD_SAMPLE 9

struct perf_event_header {
    uint32_t type;
    uint16_t misc;
    uint16_t size;
};

struct oa_sample {
   struct i915_perf_record_header header;
   uint8_t oa_report[];
};

#define U32_TO_VOID(val) ((void*) (uintptr_t) (val))

static uint32_t
oa_report_get_ctx_id(uint32_t *report32)
{
    /* TODO: deal with HSW... */
    return report32[2] & 0x1fffff;
}

static void
gputop_update_device_info_for_report(struct gputop_cc_stream *stream,
                                     uint32_t *report32)
{
    uint32_t ctx_id = oa_report_get_ctx_id(report32);
    struct gputop_hash_entry *sseu_entry =
        gputop_hash_table_search(stream->sseu_configs, U32_TO_VOID(ctx_id));
    struct drm_i915_perf_sseu_change sseu_data = { 0, };
    uint32_t s;
    uint64_t s_max, ss_max, ss_mask;
    uint64_t slice_mask;
    uint64_t subslice_mask;

    if (sseu_entry) {
        memcpy(&sseu_data, sseu_entry->data, sizeof(sseu_data));
    } else {
        gputop_cr_console_error("missing sseu configuration for context %u/0x%x",
                                ctx_id, ctx_id);
    }

    slice_mask = sseu_data.sseu.packed.slice_mask;
    ss_mask = sseu_data.sseu.packed.subslice_mask;

    gputop_devinfo.slice_mask = slice_mask;
    gputop_devinfo.n_eu_slices = __builtin_popcount(slice_mask);

    if (IS_HASWELL(gputop_devinfo.devid)) {
        s_max = 2;
        ss_max = 3;
    } else if (IS_BROADWELL(gputop_devinfo.devid)) {
        s_max = 2;
        ss_max = 3;
    } else if (IS_CHERRYVIEW(gputop_devinfo.devid)) {
        s_max = 1;
        ss_max = 2;
    } else if (IS_GEN9(gputop_devinfo.devid)) {
        s_max = 3;
        ss_max = 3;
    }

    /* Note: the _SUBSLICE_MASK param only reports a global subslice
     * mask which applies to all slices.
     *
     * Note: some of the metrics we have (as described in XML) are
     * conditional on a $SubsliceMask variable which is expected to
     * also reflect the slice mask by packing together subslice masks
     * for each slice in one value..
     */
    for (s = 0; s < s_max; s++) {
        if (slice_mask & (1ULL << s)) {
            subslice_mask |= ss_mask << (ss_max * s);
        }
    }
    gputop_devinfo.subslice_mask = subslice_mask;
    gputop_devinfo.n_eu_sub_slices = __builtin_popcount(subslice_mask);
}

static void __attribute__((noreturn))
assert_not_reached(void)
{
    gputop_cr_console_assert(0, "code should not be reached");
    assert(0); /* just to hide compiler warning about noreturn */
}

#define JS_MAX_SAFE_INTEGER (((uint64_t)1<<53) - 1)

/* Returns the ID for a counter_name using the symbol_name */
int EMSCRIPTEN_KEEPALIVE
gputop_cc_get_counter_id(const char *hw_config_guid, const char *counter_symbol_name)
{
    struct gputop_metric_set *metric_set = gputop_cr_lookup_metric_set(hw_config_guid);

    for (int t=0; t<metric_set->n_counters; t++) {
        struct gputop_metric_set_counter *counter = &metric_set->counters[t];
        if (!strcmp(counter->symbol_name, counter_symbol_name))
            return t;
    }
    return -1;
}

static void
forward_oa_accumulator_events(struct gputop_cc_stream *stream,
                              struct gputop_cc_oa_accumulator *oa_accumulator,
                              uint32_t events)
{
    struct gputop_metric_set *oa_metric_set = stream->oa_metric_set;
    int i;

    //printf("start ts = %"PRIu64" end ts = %"PRIu64" agg. period =%"PRIu64"\n",
    //        stream->start_timestamp, stream->end_timestamp, oa_accumulator->aggregation_period);

    if (!_gputop_cr_accumulator_start_update(stream,
                                             oa_accumulator,
                                             events,
                                             oa_accumulator->first_timestamp,
                                             oa_accumulator->last_timestamp))
        return;

    for (i = 0; i < oa_metric_set->n_counters; i++) {
        uint64_t u53_check;
        double d_value = 0;
        uint64_t max = 0;

        struct gputop_metric_set_counter *counter = &oa_metric_set->counters[i];

        /* Don't update the counter if the current configuration makes it
         * unavailable. */
        if (counter->available && !counter->available(&gputop_devinfo))
            continue;

        switch(counter->data_type) {
            case GPUTOP_PERFQUERY_COUNTER_DATA_UINT64:
                if (counter->max_uint64) {
                    u53_check = counter->max_uint64(&gputop_devinfo, oa_metric_set,
                                                    oa_accumulator->deltas);
                    if (u53_check > JS_MAX_SAFE_INTEGER) {
                        gputop_cr_console_error("'Max' value is to large to represent in JavaScript: %s ", counter->symbol_name);
                        u53_check = JS_MAX_SAFE_INTEGER;
                    }
                    max = u53_check;
                }

                u53_check = counter->oa_counter_read_uint64(&gputop_devinfo,
                                                            oa_metric_set,
                                                            oa_accumulator->deltas);
                if (u53_check > JS_MAX_SAFE_INTEGER) {
                    gputop_cr_console_error("Clamping counter to large to represent in JavaScript %s ", counter->symbol_name);
                    u53_check = JS_MAX_SAFE_INTEGER;
                }
                d_value = u53_check;
                break;
            case GPUTOP_PERFQUERY_COUNTER_DATA_FLOAT:
                if (counter->max_float) {
                    max = counter->max_float(&gputop_devinfo, oa_metric_set,
                                             oa_accumulator->deltas);
                }

                d_value = counter->oa_counter_read_float(&gputop_devinfo,
                                                         oa_metric_set,
                                                         oa_accumulator->deltas);
                break;
            case GPUTOP_PERFQUERY_COUNTER_DATA_UINT32:
            case GPUTOP_PERFQUERY_COUNTER_DATA_DOUBLE:
            case GPUTOP_PERFQUERY_COUNTER_DATA_BOOL32:
                gputop_cr_console_assert(0, "Unexpected counter data type");
                break;
        }

        _gputop_cr_accumulator_append_count(i, max, d_value);
    }

    _gputop_cr_accumulator_end_update();
}

void EMSCRIPTEN_KEEPALIVE
gputop_cc_handle_tracepoint_message(struct gputop_cc_stream *stream,
                                    uint8_t *data,
                                    int len)
{
    const struct perf_event_header *header;

    gputop_cr_console_error("Tracepoint message: stream=%p, data=%p, len=%d\n",
                            stream, data, len);

    for (header = (void *)data;
         (uint8_t *)header < (data + len);
         header = (void *)(((uint8_t *)header) + header->size))
    {
        if (header->size == 0) {
            gputop_cr_console_error("Spurious header size == 0\n");
            break;
        }

        if (((uint8_t *)header) + header->size > (data + len)) {
            gputop_cr_console_error("Spurious incomplete perf record forwarded\n");
            break;
        }

        switch (header->type) {
        case PERF_RECORD_SAMPLE:
            gputop_cr_console_log("Tracepoint sample received");
            break;
        default:
            break;
        }
    }

    gputop_cr_console_log("FIXME: parse perf tracepoint data");
}

void EMSCRIPTEN_KEEPALIVE
gputop_cc_handle_i915_perf_message(struct gputop_cc_stream *stream,
                                   uint8_t *data, int data_len,
                                   struct gputop_cc_oa_accumulator **accumulators,
                                   int n_accumulators)
{
    const struct i915_perf_record_header *header;
    uint8_t *last = NULL;

    assert(stream);

    if (stream->continuation_report)
        last = stream->continuation_report;
    else {
        for (int i = 0; i < n_accumulators; i++) {
            struct gputop_cc_oa_accumulator *oa_accumulator =
                accumulators[i];

            assert(oa_accumulator);
            gputop_cc_oa_accumulator_clear(oa_accumulator);
        }
    }

    //int i = 0;
    for (header = (void *)data;
         (uint8_t *)header < (data + data_len);
         header = (void *)(((uint8_t *)header) + header->size))
    {
#if 0
        gputop_cr_console_log("header[%d] = %p size=%d type = %d", i, header, header->size, header->type);

        i++;
        if (i > 200) {
            gputop_cr_console_log("perf message too large!\n");
            return;
        }
#endif

        switch (header->type) {

        case DRM_I915_PERF_RECORD_OA_BUFFER_LOST:
            gputop_cr_console_log("i915_oa: OA buffer error - all records lost\n");
            break;
        case DRM_I915_PERF_RECORD_OA_REPORT_LOST:
            gputop_cr_console_log("i915_oa: OA report lost\n");
            break;

        case DRM_I915_PERF_RECORD_SAMPLE: {
            struct oa_sample *sample = (struct oa_sample *)header;

            if (last) {
                gputop_update_device_info_for_report(stream,
                                                     (uint32_t *) sample->oa_report);

                for (int i = 0; i < n_accumulators; i++) {
                    struct gputop_cc_oa_accumulator *oa_accumulator =
                        accumulators[i];

                    assert(oa_accumulator);

                    if (gputop_cc_oa_accumulate_reports(oa_accumulator,
                                                        last, sample->oa_report))
                    {
                        uint64_t elapsed = (oa_accumulator->last_timestamp -
                                            oa_accumulator->first_timestamp);
                        uint32_t events = 0;
                        //gputop_cr_console_log("i915_oa: accumulated reports\n");

                        if (elapsed > oa_accumulator->aggregation_period) {
                            //gputop_cr_console_log("i915_oa: PERIOD ELAPSED (%d)\n", (int)oa_accumulator->aggregation_period);
                            events |= ACCUMULATOR_EVENT_PERIOD_ELAPSED;
                        }

                        if (events)
                            forward_oa_accumulator_events(stream, oa_accumulator, events);
                    }
                }
            }

            last = sample->oa_report;

            break;
        }

        case DRM_I915_PERF_RECORD_SSEU_CHANGE: {
            struct drm_i915_perf_sseu_change *_sseu_change =
                (struct drm_i915_perf_sseu_change *)(header + 1);
            struct gputop_hash_entry *sseu_entry =
                gputop_hash_table_search(stream->sseu_configs,
                                         U32_TO_VOID(_sseu_change->hw_id));

            if (sseu_entry == NULL) {
                struct drm_i915_perf_sseu_change *sseu_change =
                    malloc(sizeof(*sseu_change));
                memcpy(sseu_change, _sseu_change, sizeof(*sseu_change));
                gputop_hash_table_insert(stream->sseu_configs,
                                         U32_TO_VOID(sseu_change->hw_id),
                                         sseu_change);
            } else {
                struct drm_i915_perf_sseu_change *sseu_change =
                    (struct drm_i915_perf_sseu_change *) sseu_entry->data;
                memcpy(sseu_change, _sseu_change, sizeof(*sseu_change));
            }
            break;
        }

        default:
            gputop_cr_console_log("i915 perf: Spurious header type = %d\n", header->type);
            return;
        }
    }

    if (last) {
        int raw_size = stream->oa_metric_set->perf_raw_size;

        if (!stream->continuation_report)
            stream->continuation_report = malloc(raw_size);

        memcpy(stream->continuation_report, last, raw_size);
    }
}

/* The C code generated by gputop-oa-codegen.py calls this function for each
 * metric set.
 */
void
gputop_register_oa_metric_set(struct gputop_metric_set *metric_set)
{
    gputop_cr_index_metric_set(metric_set->hw_config_guid, metric_set);
}

void EMSCRIPTEN_KEEPALIVE
gputop_cc_reset_system_properties(void)
{
    memset(&gputop_devinfo, 0, sizeof(gputop_devinfo));
}

void EMSCRIPTEN_KEEPALIVE
gputop_cc_set_system_property(const char *name, double value)
{
    /* Use _Generic so we don't get caught out by a silent error if
     * we mess with struct gputop_devinfo...
     */
#define PROP(NAME) { #NAME, _Generic(gputop_devinfo.NAME, \
                                     uint32_t: TYPE_U32, \
                                     uint64_t: TYPE_U64, \
                                     default: TYPE_UNKNOWN), \
                    &gputop_devinfo.NAME }
    static const struct {
        const char *name;
        enum {
            TYPE_UNKNOWN,
            TYPE_U32,
            TYPE_U64,
        } type;
        void *symbol;
    } table[] = {
        PROP(devid),
        PROP(gen),
        PROP(timestamp_frequency),
        PROP(n_eus),
        PROP(n_eu_slices),
        PROP(n_eu_sub_slices),
        PROP(eu_threads_count),
        PROP(subslice_mask),
        PROP(slice_mask),
        PROP(gt_min_freq),
        PROP(gt_max_freq),
        { 0 },
    };
#undef PROP

    for (int i = 0; table[i].name; i++) {
        if (strcmp(name, table[i].name) == 0) {
            switch (table[i].type) {
            case TYPE_U32:
                gputop_cr_console_assert(value >= 0 && value <= UINT32_MAX,
                                         "Value for uint32 property out of range");
                *((uint32_t *)table[i].symbol) = (uint32_t)value;
                return;
            case TYPE_U64:
                gputop_cr_console_assert(value >= 0,
                                         "Value for uint64 property out of range");
                *((uint64_t *)table[i].symbol) = (uint64_t)value;
                return;
            case TYPE_UNKNOWN:
                gputop_cr_console_assert(0, "Unexpected struct gputop_devinfo %s member type",
                                         table[i].name);
            }
        }
    }

    gputop_cr_console_error("Unknown system property %s\n", name);
}

void EMSCRIPTEN_KEEPALIVE
gputop_cc_update_system_metrics(void)
{
    uint32_t devid = gputop_devinfo.devid;

    gputop_cr_console_assert(devid != 0, "Device ID not initialized before trying to update system metrics");

    if (IS_HASWELL(devid))
        gputop_oa_add_metrics_hsw(&gputop_devinfo);
    else if (IS_BROADWELL(devid))
        gputop_oa_add_metrics_bdw(&gputop_devinfo);
    else if (IS_CHERRYVIEW(devid))
        gputop_oa_add_metrics_chv(&gputop_devinfo);
    else if (IS_SKYLAKE(devid)) {
        if (IS_SKL_GT2(devid))
            gputop_oa_add_metrics_sklgt2(&gputop_devinfo);
        else if (IS_SKL_GT3(devid))
            gputop_oa_add_metrics_sklgt3(&gputop_devinfo);
        else if (IS_SKL_GT4(devid))
            gputop_oa_add_metrics_sklgt4(&gputop_devinfo);
        else {
            gputop_cr_console_error("Unsupported Skylake GT size");
            assert_not_reached();
        }
    } else if (IS_BROXTON(devid)) {
        gputop_oa_add_metrics_bxt(&gputop_devinfo);
    } else if (IS_KABYLAKE(devid)) {
        if (IS_KBL_GT2(devid))
            gputop_oa_add_metrics_kblgt2(&gputop_devinfo);
        else if (IS_KBL_GT3(devid))
            gputop_oa_add_metrics_kblgt3(&gputop_devinfo);
        else {
            gputop_cr_console_error("Unsupported Kabylake GT size");
            assert_not_reached();
        }
    } else {
        gputop_cr_console_error("FIXME: Unknown platform device ID 0x%x: " __FILE__, devid);
        assert_not_reached();
    }
}

struct gputop_cc_stream * EMSCRIPTEN_KEEPALIVE
gputop_cc_oa_stream_new(const char *hw_config_guid)
{
    struct gputop_cc_stream *stream =
        (struct gputop_cc_stream *) xmalloc0(sizeof(*stream));

    assert(stream);

    stream->type = STREAM_TYPE_OA;

    stream->sseu_configs = gputop_hash_table_create(gputop_hash_pointer,
                                                    gputop_key_pointer_equal);
    stream->oa_metric_set = gputop_cr_lookup_metric_set(hw_config_guid);
    assert(stream->oa_metric_set);
    assert(stream->oa_metric_set->perf_oa_format);

    return stream;
}

struct gputop_cc_stream * EMSCRIPTEN_KEEPALIVE
gputop_cc_tracepoint_stream_new(void)
{
    struct gputop_cc_stream *stream =
        (struct gputop_cc_stream *) xmalloc0(sizeof(*stream));

    assert(stream);

    stream->type = STREAM_TYPE_TRACEPOINT;

    return stream;
}

void EMSCRIPTEN_KEEPALIVE
gputop_cc_tracepoint_add_field(struct gputop_cc_stream *stream,
                               const char *name,
                               const char *type_name,
                               int offset,
                               int size,
                               bool is_signed)
{
    enum gputop_cc_field_type type;
    struct gputop_cc_tracepoint_field *field;

    gputop_cr_console_log("Ading field: %s\n", name);
    assert(stream->n_fields < GPUTOP_CC_MAX_FIELDS);

    if (strcmp(type_name, "char") == 0 && size == 1 && is_signed) {
        type = FIELD_TYPE_INT8;
    } else if (strcmp(type_name, "unsigned char") == 0 && size == 1 && !is_signed) {
        type = FIELD_TYPE_UINT8;
    } else if (strcmp(type_name, "short") == 0 && size == 2 && is_signed) {
        type = FIELD_TYPE_INT16;
    } else if (strcmp(type_name, "unsigned short") == 0 && size == 2 && !is_signed) {
        type = FIELD_TYPE_UINT16;
    } else if (strcmp(type_name, "int") == 0 && size == 4 && is_signed) {
        type = FIELD_TYPE_INT32;
    } else if (strcmp(type_name, "unsigned int") == 0 && size == 4 && !is_signed) {
        type = FIELD_TYPE_UINT32;
    } else if (strcmp(type_name, "int") == 0 && size == 8 && is_signed) {
        type = FIELD_TYPE_INT64;
    } else if (strcmp(type_name, "unsigned int") == 0 && size == 6 && !is_signed) {
        type = FIELD_TYPE_UINT64;
    } else {
        /* skip unsupport types */
        return;
    }

    field = &stream->fields[stream->n_fields++];
    field->name = strdup(name);
    field->type = type;
    field->offset = offset;
}

static void delete_sseu_entry(struct gputop_hash_entry *entry)
{
    free(entry->data);
}

void EMSCRIPTEN_KEEPALIVE
gputop_cc_stream_destroy(struct gputop_cc_stream *stream)
{
    gputop_cr_console_log("Freeing client-c stream %p\n", stream);

    assert(stream);

    gputop_hash_table_destroy(stream->sseu_configs, delete_sseu_entry);
    free(stream->continuation_report);
    free(stream);
}

#ifdef EMSCRIPTEN
static void
dummy_mainloop_callback(void)
{
}

int
main() {
    /* XXX: this is a hack to ensure we leave the Runtime initialized
     * even though we don't use the emscripten mainloop callback itself
     */
    emscripten_set_main_loop(dummy_mainloop_callback, 1, 1);

    return 0;
}
#endif
