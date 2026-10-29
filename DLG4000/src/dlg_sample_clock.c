#include "dlg_sample_clock.h"

#include <math.h>
#include <string.h>

int dlg_packet_sample_count(const dlg_data_packet_t *packet, size_t bytes,
                            double requested_rate_hz){
    size_t header = offsetof(dlg_data_packet_t, samples);
    size_t values;
    int count;
    if(!packet || bytes < header || bytes > sizeof(*packet) ||
       packet->code != 0x100Bu || packet->signals != DLG_SAMPLE_CHANNELS ||
       packet->bursts <= 0 || !isfinite(packet->rate_hz) ||
       packet->rate_hz <= 0.0 || !isfinite(requested_rate_hz) ||
       requested_rate_hz <= 0.0 ||
       fabs(packet->rate_hz - requested_rate_hz) >
           fmax(0.5, requested_rate_hz * 0.01) ||
       (bytes - header) % sizeof(int16_t) != 0u){
        return 0;
    }
    values = (bytes - header) / sizeof(int16_t);
    if(values == 0u || values % DLG_SAMPLE_CHANNELS != 0u) return 0;
    count = (int)(values / DLG_SAMPLE_CHANNELS);
    /* Some firmware pads the packet. The declared burst count is authoritative. */
    if(packet->bursts > count) return 0;
    return packet->bursts;
}

int dlg_sample_clock_init(dlg_sample_clock_t *clock, int64_t frequency,
                          double rate_hz){
    if(!clock || frequency <= 0 || !isfinite(rate_hz) || rate_hz <= 0.0 ||
       (double)frequency / rate_hz < 2.0) return 0;
    memset(clock, 0, sizeof(*clock));
    clock->frequency = frequency;
    clock->period_ticks = (double)frequency / rate_hz;
    return 1;
}

int dlg_sample_clock_plan(dlg_sample_clock_t *clock, uint32_t frame,
                          int samples, int64_t receive_qpc,
                          dlg_sample_plan_t *plan){
    uint32_t delta = 1u;
    uint64_t missing = 0u;
    double last;
    int64_t first;
    if(!clock || !plan || samples <= 0 || samples > DLG_PACKET_VALUES /
       DLG_SAMPLE_CHANNELS || receive_qpc <= 0 || clock->period_ticks <= 0.0){
        return 0;
    }
    memset(plan, 0, sizeof(*plan));
    if(clock->initialized){
        double elapsed, error, correction;
        delta = frame - clock->frame;
        if(delta == 0u){ clock->duplicates++; return 0; }
        if(delta >= 0x80000000u){ clock->reordered++; return 0; }
        /* Limit untrusted frame gaps before multiplying or materializing NULLs. */
        if(delta > 100000u){ clock->timing_rejects++; return 0; }
        missing = (uint64_t)(delta - 1u) * (uint64_t)clock->previous_samples;
        elapsed = ((double)missing + samples) * clock->period_ticks;
        last = (double)clock->last_source_qpc + elapsed;
        error = (double)receive_qpc - last;
        correction = error < 0.0 ? error : fmin(error, elapsed * 0.0005);
        last += correction;
    }else{
        last = (double)receive_qpc;
    }
    plan->last_qpc = (int64_t)llround(last);
    if(plan->last_qpc > receive_qpc) plan->last_qpc = receive_qpc;
    first = plan->last_qpc - (int64_t)llround((samples - 1) * clock->period_ticks);
    if(first <= 0 || (clock->initialized && first <= clock->last_source_qpc)){
        clock->timing_rejects++;
        return 0;
    }
    plan->samples = samples;
    plan->first_qpc = first;
    plan->missing_frames = delta - 1u;
    plan->missing_samples = missing;
    clock->initialized = 1;
    clock->frame = frame;
    clock->previous_samples = samples;
    clock->last_source_qpc = plan->last_qpc;
    clock->missing_frames += plan->missing_frames;
    return 1;
}

int64_t dlg_sample_plan_qpc(const dlg_sample_clock_t *clock,
                            const dlg_sample_plan_t *plan, int sample){
    if(!clock || !plan || sample < 0 || sample >= plan->samples) return 0;
    return plan->last_qpc - (int64_t)llround(
        (plan->samples - 1 - sample) * clock->period_ticks);
}
