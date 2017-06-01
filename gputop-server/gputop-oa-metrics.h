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

#pragma once

#include <stdint.h>

#include <i915_oa_drm.h>

#ifdef __cplusplus
extern "C" {
#endif

struct gputop_devinfo {
    uint32_t devid;
    uint32_t gen;
    uint64_t timestamp_frequency;
    uint64_t n_eus;
    uint64_t n_eu_slices;
    uint64_t n_eu_sub_slices;
    uint64_t eu_threads_count;
    uint64_t subslice_mask;
    uint64_t slice_mask;
    uint64_t gt_min_freq;
    uint64_t gt_max_freq;
};

extern struct gputop_devinfo gputop_devinfo;

typedef enum {
    GPUTOP_PERFQUERY_COUNTER_DATA_UINT64,
    GPUTOP_PERFQUERY_COUNTER_DATA_UINT32,
    GPUTOP_PERFQUERY_COUNTER_DATA_DOUBLE,
    GPUTOP_PERFQUERY_COUNTER_DATA_FLOAT,
    GPUTOP_PERFQUERY_COUNTER_DATA_BOOL32,
} gputop_counter_data_type_t;

typedef enum {
    GPUTOP_PERFQUERY_COUNTER_RAW,
    GPUTOP_PERFQUERY_COUNTER_DURATION_RAW,
    GPUTOP_PERFQUERY_COUNTER_DURATION_NORM,
    GPUTOP_PERFQUERY_COUNTER_EVENT,
    GPUTOP_PERFQUERY_COUNTER_THROUGHPUT,
    GPUTOP_PERFQUERY_COUNTER_TIMESTAMP,
} gputop_counter_type_t;


#define OAREPORT_REASON_MASK           0x3f
#define OAREPORT_REASON_SHIFT          19
#define OAREPORT_REASON_TIMER          (1<<0)
#define OAREPORT_REASON_CTX_SWITCH     (1<<3)

struct gputop_metric_set;
struct gputop_metric_set_counter
{
   const char *name;
   const char *symbol_name;
   const char *desc;
   gputop_counter_type_t type;
   gputop_counter_data_type_t data_type;
   union {
       uint64_t (*max_uint64)(struct gputop_devinfo *devinfo,
                              const struct gputop_metric_set *metric_set,
                              uint64_t *deltas);
       double (*max_float)(struct gputop_devinfo *devinfo,
                           const struct gputop_metric_set *metric_set,
                           uint64_t *deltas);
   };

   union {
      uint64_t (*oa_counter_read_uint64)(struct gputop_devinfo *devinfo,
                                         const struct gputop_metric_set *metric_set,
                                         uint64_t *deltas);
      double (*oa_counter_read_float)(struct gputop_devinfo *devinfo,
                                      const struct gputop_metric_set *metric_set,
                                      uint64_t *deltas);
   };

   bool (*available)(struct gputop_devinfo *devinfo);
};

struct gputop_metric_set
{
    const char *name;
    const char *symbol_name;
    const char *hw_config_guid;
    struct gputop_metric_set_counter *counters;
    int n_counters;

    int perf_oa_metrics_set;
    int perf_oa_format;
    int perf_raw_size;

    /* For indexing into accumulator->deltas[] ... */
    int gpu_time_offset;
    int gpu_clock_offset;
    int a_offset;
    int b_offset;
    int c_offset;
};


#ifdef __cplusplus
}
#endif
