#ifndef DLG_SAMPLE_CLOCK_H
#define DLG_SAMPLE_CLOCK_H

#include <stddef.h>
#include <stdint.h>

#define DLG_SAMPLE_CHANNELS 8
#define DLG_PACKET_VALUES 720

/* DLG wire layout, unchanged. Device timestamp units are not assumed. */
#pragma pack(push, 1)
typedef struct {
    uint16_t code, res;
    int32_t frame, ts;
    float rate_hz;
    int16_t bursts, signals;
    int32_t reserved[3];
    int16_t samples[DLG_PACKET_VALUES];
} dlg_data_packet_t;
#pragma pack(pop)

typedef struct {
    int initialized;
    uint32_t frame;
    int previous_samples;
    int64_t frequency;
    double period_ticks;
    int64_t last_source_qpc;
    uint64_t duplicates;
    uint64_t reordered;
    uint64_t missing_frames;
    uint64_t timing_rejects;
} dlg_sample_clock_t;

typedef struct {
    int samples;
    uint32_t missing_frames;
    uint64_t missing_samples;
    int64_t first_qpc;
    int64_t last_qpc;
} dlg_sample_plan_t;

int dlg_packet_sample_count(const dlg_data_packet_t *packet, size_t bytes,
                            double requested_rate_hz);
int dlg_sample_clock_init(dlg_sample_clock_t *clock, int64_t frequency,
                          double rate_hz);
/* 1 accepted, 0 duplicate/reordered/timing rejection. Never dates a sample
   after receipt. Positive phase correction is bounded to 500 ppm so a
   delayed burst cannot be relabelled as fresh. */
int dlg_sample_clock_plan(dlg_sample_clock_t *clock, uint32_t frame,
                          int samples, int64_t receive_qpc,
                          dlg_sample_plan_t *plan);
int64_t dlg_sample_plan_qpc(const dlg_sample_clock_t *clock,
                            const dlg_sample_plan_t *plan, int sample);

#endif
