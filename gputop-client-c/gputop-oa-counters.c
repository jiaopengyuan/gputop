/*
 * GPU Top
 *
 * Copyright (C) 2015-2016 Intel Corporation
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

#include <string.h>

#include "gputop-oa-counters.h"
#include "gputop-util.h"

/* #ifdef GPUTOP_CLIENT */
#include "gputop-client-c-runtime.h"
#define dbg gputop_cr_console_log
/* #else */
/* #include "gputop-log.h" */
/* #endif */

struct gputop_devinfo gputop_devinfo;

static uint64_t
timebase_scale(uint64_t u32_time)
{
    return (u32_time * 1000000000) / gputop_devinfo.timestamp_frequency;
}

void
gputop_u32_clock_init(struct gputop_u32_clock *clock, uint32_t u32_start)
{
    clock->timestamp = clock->start = timebase_scale(u32_start);
    clock->last_u32 = u32_start;
    clock->initialized = true;
}

uint64_t
gputop_u32_clock_get_time(struct gputop_u32_clock *clock)
{
    return clock->timestamp;
}

void
gputop_u32_clock_progress(struct gputop_u32_clock *clock,
                          uint32_t u32_timestamp)
{
    uint32_t delta = u32_timestamp - clock->last_u32;

    clock->timestamp += timebase_scale(delta);
    clock->last_u32 = u32_timestamp;
}

static void
accumulate_uint32(const uint32_t *report0,
                  const uint32_t *report1,
                  uint64_t *deltas)
{
   *deltas += (uint32_t)(*report1 - *report0);
}

static void
accumulate_uint40(int a_index,
                  const uint32_t *report0,
                  const uint32_t *report1,
                  uint64_t *deltas)
{
    const uint8_t *high_bytes0 = (uint8_t *)(report0 + 40);
    const uint8_t *high_bytes1 = (uint8_t *)(report1 + 40);
    uint64_t high0 = (uint64_t)(high_bytes0[a_index]) << 32;
    uint64_t high1 = (uint64_t)(high_bytes1[a_index]) << 32;
    uint64_t value0 = report0[a_index + 4] | high0;
    uint64_t value1 = report1[a_index + 4] | high1;
    uint64_t delta;

    if (value0 > value1)
       delta = (1ULL << 40) + value1 - value0;
    else
       delta = value1 - value0;

    *deltas += delta;
}

static void
accumulate_reports(struct gputop_metric_set *metric_set,
                   struct gputop_u32_clock *clock,
                   const uint32_t *start, const uint32_t *end,
                   uint64_t *deltas,
                   uint64_t *first_timestamp,
                   uint64_t *last_timestamp)
{
    int idx = 0;
    int i;

    if (clock->initialized)
        gputop_u32_clock_init(clock, start[1]);

    switch (metric_set->perf_oa_format) {
    case I915_OA_FORMAT_A32u40_A4u32_B8_C8:

        accumulate_uint32(start + 1, end + 1, deltas + idx++); /* timestamp */
        accumulate_uint32(start + 3, end + 3, deltas + idx++); /* clock */

        /* 32x 40bit A counters... */
        for (i = 0; i < 32; i++)
            accumulate_uint40(i, start, end, deltas + idx++);

        /* 4x 32bit A counters... */
        for (i = 0; i < 4; i++)
            accumulate_uint32(start + 36 + i, end + 36 + i, deltas + idx++);

        /* 8x 32bit B counters + 8x 32bit C counters... */
        for (i = 0; i < 16; i++)
            accumulate_uint32(start + 48 + i, end + 48 + i, deltas + idx++);
        break;

    case I915_OA_FORMAT_A45_B8_C8:

        accumulate_uint32(start + 1, end + 1, deltas); /* timestamp */

        for (i = 0; i < 61; i++)
            accumulate_uint32(start + 3 + i, end + 3 + i, deltas + 1 + i);
        break;

    default:
        assert(0);
    }

    gputop_u32_clock_progress(clock, start[1]);
    if (*first_timestamp == 0)
        *first_timestamp = gputop_u32_clock_get_time(clock);

    gputop_u32_clock_progress(clock, end[1]);
    *last_timestamp = gputop_u32_clock_get_time(clock);
}

bool
gputop_cc_oa_accumulate_reports(struct gputop_cc_oa_accumulator *accumulator,
                                const uint8_t *report0,
                                const uint8_t *report1)
{
    const uint32_t *start = (const uint32_t *)report0;
    const uint32_t *end = (const uint32_t *)report1;

    assert(report0 != report1);

    /* technically a timestamp of zero is valid, but much more likely it
     * indicates a problem...
     */
    if (start[1] == 0 || end[1] == 0) {
        dbg("i915_oa: spurious report with timestamp of zero\n");
        return false;
    }

    accumulate_reports(accumulator->metric_set,
                       &accumulator->clock,
                       start, end,
                       accumulator->deltas,
                       &accumulator->first_timestamp,
                       &accumulator->last_timestamp);

    return true;
}

void EMSCRIPTEN_KEEPALIVE
gputop_cc_oa_accumulator_clear(struct gputop_cc_oa_accumulator *accumulator)
{
    memset(accumulator->deltas, 0, sizeof(accumulator->deltas));
    accumulator->first_timestamp = 0;
    accumulator->last_timestamp = 0;
    accumulator->flags = 0;
}

struct gputop_cc_oa_accumulator * EMSCRIPTEN_KEEPALIVE
gputop_cc_oa_accumulator_new(struct gputop_cc_stream *stream,
                             int aggregation_period,
                             bool enable_ctx_switch_events)
{
    struct gputop_cc_oa_accumulator *accumulator = malloc(sizeof(*accumulator));

    assert(accumulator);
    assert(stream);
    assert(stream->oa_metric_set);

    memset(accumulator, 0, sizeof(*accumulator));
    accumulator->metric_set = stream->oa_metric_set;
    accumulator->aggregation_period = aggregation_period;
    accumulator->enable_ctx_switch_events = enable_ctx_switch_events;

    return accumulator;
}

void EMSCRIPTEN_KEEPALIVE
gputop_cc_oa_accumulator_set_period(struct gputop_cc_oa_accumulator *accumulator,
                                    uint32_t aggregation_period)
{
    assert(accumulator);

    accumulator->aggregation_period = aggregation_period;
}

void EMSCRIPTEN_KEEPALIVE
gputop_cc_oa_accumulator_destroy(struct gputop_cc_oa_accumulator *accumulator)
{
    assert(accumulator);

    gputop_cr_console_log("Freeing client-c OA accumulator %p\n", accumulator);

    free(accumulator);
}

struct gputop_cc_oa_timeline * EMSCRIPTEN_KEEPALIVE
gputop_cc_oa_timeline_new(struct gputop_cc_stream *stream,
                          int aggregation_period)
{
    struct gputop_cc_oa_timeline *timeline = malloc(sizeof(*timeline));

    assert(timeline);
    assert(stream);
    assert(stream->oa_metric_set);

    gputop_cr_console_log("New timeline aggregation_period=%i\n", aggregation_period);

    timeline->aggregation_period = aggregation_period;

    timeline->metric_set = stream->oa_metric_set;

    timeline->n_items = 0;
    memset(timeline->items, 0, sizeof(timeline->items));

    return timeline;
}

void EMSCRIPTEN_KEEPALIVE
gputop_cc_oa_timeline_destroy(struct gputop_cc_oa_timeline *timeline)
{
    assert(timeline);

    gputop_cr_console_log("Freeing client-c OA timeline %p\n", timeline);

    free(timeline);
}

void
gputop_cc_oa_timeline_clear(struct gputop_cc_oa_timeline *timeline)
{
   timeline->n_items = 0;
   memset(timeline->items, 0, sizeof(timeline->items));
}

void
gputop_cc_oa_timeline_advance(struct gputop_cc_oa_timeline *timeline,
                              int item)
{
   assert(timeline);
   assert(item < timeline->n_items);

   for (int i = 0; i < (timeline->n_items - item); i++) {
       memcpy(&timeline->items[i],
              &timeline->items[item + i],
              sizeof(timeline->items[0]));
   }
   memset(&timeline->items[timeline->n_items - item], 0,
          sizeof(timeline->items[0]) * item);
   timeline->n_items -= item;
}

static bool
different_context(struct gputop_cc_oa_timeline *timeline,
                  int item,
                  const uint32_t *start,
                  const uint32_t *end)
{
    uint32_t old_ctx_id = timeline->items[item].ctx_id;
    uint32_t new_ctx_id = oa_report_get_ctx_id(end);

    if (old_ctx_id == new_ctx_id)
        return false;

    if (new_ctx_id != INVALID_CTX_ID)
        return true;

    /*
     * Dealing with switch to an invalid ctx id from here. This usually mean
     * the GPU has becomed idle. Give it a chance to pickup another job of the
     * same ctx before considering the report idle.
     */
    if (!timeline->items[item].seen_idle) {
        timeline->items[item].seen_idle = true;
        return false;
    }

    return true;
}

bool
gputop_cc_oa_timeline_accumulate_reports(struct gputop_cc_oa_timeline *timeline,
                                         const uint8_t *report0,
                                         const uint8_t *report1)
{
    const uint32_t *start = (const uint32_t *)report0;
    const uint32_t *end = (const uint32_t *)report1;
    uint32_t ctx_id = oa_report_get_ctx_id(end);
    int item = MAX(timeline->n_items - 1, 0);

    assert(timeline);

    if (different_context(timeline, item, start, end)) {
        if (timeline->clock.initialized)
            item = timeline->n_items++;

        timeline->items[item].ctx_id = ctx_id;
    }

    accumulate_reports(timeline->metric_set,
                       &timeline->clock,
                       start, end,
                       timeline->items[item].deltas,
                       &timeline->items[item].first_timestamp,
                       &timeline->items[item].last_timestamp);

    return true;
}

uint64_t
gputop_cc_oa_timeline_elapsed(struct gputop_cc_oa_timeline *timeline)
{
    return timeline->n_items > 0 &&
        (timeline->items[timeline->n_items - 1].last_timestamp -
         timeline->items[0].first_timestamp) > timeline->aggregation_period;
}

bool
gputop_cc_oa_timeline_full(struct gputop_cc_oa_timeline *timeline)
{
    return timeline->n_items == (ARRAY_SIZE(timeline->items) - 1);
}
