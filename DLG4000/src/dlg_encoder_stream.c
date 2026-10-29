#include "dlg_encoder_stream.h"
#include "dlg_sample_clock.h"
#include "encoder_control_protocol.h"

#include <windows.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef STREAM_QUEUE_CAPACITY
#define STREAM_QUEUE_CAPACITY 8192
#endif
#define STREAM_GAP_GRACE_S 0.050
#define STREAM_MAX_SOURCE_AGE_S 0.100

#ifdef DLG_STREAM_TESTING
void dlg_stream_test_before_write(void);
#endif

typedef struct {
    int idx; /* -1: late/rejected raw sample, timing archive only */
    int has_value;
    int burst_index, burst_samples;
    uint32_t frame;
    int32_t device_ts;
    int64_t qpc, receive_qpc, publish_qpc;
    int16_t raw[8];
    encoder_state_output_t encoder;
} stream_record_t;

typedef struct {
    const dlg_encoder_stream_config_t *config;
    CRITICAL_SECTION mutex;
    stream_record_t *records;
    int head, tail, count, max_count;
    volatile LONG stop, done, ready, failure;
    int produced, have, missing, late, malformed, startup_discarded;
    uint64_t sequence, sent, send_errors;
    int failure_published;
    int64_t origin_qpc, last_emitted_qpc, last_packet_receive_qpc;
    double max_source_age_ms;
    double reported_rate_hz;
    dlg_sample_clock_t clock;
    encoder_state_output_t last_output;
} stream_context_t;

enum { STREAM_OK, STREAM_QUEUE_FULL, STREAM_SOCKET_ERROR, STREAM_SEND_ERROR,
       STREAM_START_TIMEOUT, STREAM_TIMING_ERROR, STREAM_WRITE_ERROR };

static int64_t stream_now(void){
    LARGE_INTEGER value;
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

static LONG stream_read(volatile LONG *value){
    return InterlockedCompareExchange(value, 0, 0);
}

static void stream_fail(stream_context_t *context, LONG reason){
    InterlockedCompareExchange(&context->failure, reason, STREAM_OK);
    InterlockedExchange(&context->stop, 1);
}

static int stream_queue_push(stream_context_t *context, const stream_record_t *record){
    int ok = 0;
    EnterCriticalSection(&context->mutex);
    if(context->count < STREAM_QUEUE_CAPACITY){
        context->records[context->head] = *record;
        context->head = (context->head + 1) % STREAM_QUEUE_CAPACITY;
        context->count++;
        if(context->count > context->max_count) context->max_count = context->count;
        ok = 1;
    }
    LeaveCriticalSection(&context->mutex);
    if(!ok) stream_fail(context, STREAM_QUEUE_FULL);
    return ok;
}

static int stream_queue_pop(stream_context_t *context, stream_record_t *record){
    int ok = 0;
    EnterCriticalSection(&context->mutex);
    if(context->count > 0){
        *record = context->records[context->tail];
        context->tail = (context->tail + 1) % STREAM_QUEUE_CAPACITY;
        context->count--;
        ok = 1;
    }
    LeaveCriticalSection(&context->mutex);
    return ok;
}

static int stream_publish(stream_context_t *context, int64_t qpc,
                           const encoder_state_output_t *output){
    const dlg_encoder_stream_config_t *c = context->config;
    encoder_control_packet_t packet;
    int result;
    context->sequence++;
    if(!encoder_control_packet_build(&packet, c->session, context->sequence,
                                     qpc, output)) return 0;
    result = sendto(c->control_socket, (const char *)&packet, sizeof(packet), 0,
                    (const struct sockaddr *)&c->control_address,
                    sizeof(c->control_address));
    if(result != sizeof(packet)){
        context->send_errors++;
        return 0;
    }
    context->sent++;
    return 1;
}

static int stream_emit(stream_context_t *context, stream_record_t *record){
    const dlg_encoder_stream_config_t *c = context->config;
    unsigned flags = 0;
    double angle = 0.0;
    double age_s;
    int64_t now;
    if(context->produced >= c->total_rows || stream_read(&context->stop)) return 0;
    record->idx = context->produced;
    if(context->origin_qpc == 0) context->origin_qpc = record->qpc;
    now = stream_now();
    age_s = (double)(now - record->qpc) / (double)c->frequency;
    if(age_s < 0.0){ stream_fail(context, STREAM_TIMING_ERROR); return 0; }
    if(1000.0 * age_s > context->max_source_age_ms)
        context->max_source_age_ms = 1000.0 * age_s;
    if(record->has_value){
        context->have++;
        angle = c->slope[2] * record->raw[2] + c->intercept[2];
        flags = ENCODER_INPUT_HAS_VALUE;
        if(record->raw[2] <= -32760 || record->raw[2] >= 32760)
            flags |= ENCODER_INPUT_SATURATED;
        /* Old acquisition records remain in the archive, but cannot refresh
           the operational encoder or issue a reversal after a receive stall. */
        if(age_s >= STREAM_MAX_SOURCE_AGE_S) flags = 0;
    }else{
        context->missing++;
    }
    encoder_state_update_angle(c->encoder, record->qpc, angle, flags,
                                &record->encoder);
    context->last_output = record->encoder;
    record->publish_qpc = stream_now();
    if(!stream_publish(context, record->qpc, &record->encoder)){
        stream_fail(context, STREAM_SEND_ERROR);
        return 0;
    }
    if(record->encoder.accepted){
        LONG ready = stream_read(&context->ready);
        if(ready < 3) InterlockedIncrement(&context->ready);
    }else if(stream_read(&context->ready) < 3){
        InterlockedExchange(&context->ready, 0);
    }
    context->last_emitted_qpc = record->qpc;
    context->produced++;
    return stream_queue_push(context, record);
}

static int stream_fill_until(stream_context_t *context, int64_t limit_qpc){
    int64_t period = (int64_t)llround(context->clock.period_ticks);
    while(context->last_emitted_qpc != 0 &&
          context->last_emitted_qpc + period <= limit_qpc &&
          context->produced < context->config->total_rows &&
          !stream_read(&context->stop)){
        stream_record_t record;
        memset(&record, 0, sizeof(record));
        record.qpc = context->last_emitted_qpc + period;
        record.receive_qpc = stream_now();
        if(!stream_emit(context, &record)) return 0;
    }
    return 1;
}

static void stream_archive_late(stream_context_t *context,
                                 const dlg_data_packet_t *packet, int samples,
                                 int64_t receive_qpc){
    for(int i = 0; i < samples && !stream_read(&context->stop); ++i){
        stream_record_t record;
        memset(&record, 0, sizeof(record));
        record.idx = -1;
        record.has_value = 1;
        record.frame = (uint32_t)packet->frame;
        record.device_ts = packet->ts;
        record.receive_qpc = receive_qpc;
        record.burst_index = i;
        record.burst_samples = samples;
        memcpy(record.raw, &packet->samples[i * 8], sizeof(record.raw));
        context->late++;
        if(!stream_queue_push(context, &record)) break;
    }
}

static DWORD WINAPI stream_producer(LPVOID argument){
    stream_context_t *context = (stream_context_t *)argument;
    const dlg_encoder_stream_config_t *c = context->config;
    int64_t started = stream_now();
    /* DLG may have streamed throughout READY. Discard the pre-START socket
       backlog before establishing a fresh receipt/sequence anchor. */
    for(int i = 0; i < 4096; ++i){
        dlg_data_packet_t old;
        int n = recvfrom(c->acquisition_socket, (char *)&old, sizeof(old), 0, NULL, NULL);
        if(n < 0) break;
        context->startup_discarded++;
    }
    while(!stream_read(&context->stop) && context->produced < c->total_rows){
        dlg_data_packet_t packet;
        int n = recvfrom(c->acquisition_socket, (char *)&packet, sizeof(packet), 0, NULL, NULL);
        int64_t received = stream_now();
        if(n < 0){
            int error=WSAGetLastError();
            if(error==WSAEMSGSIZE){ context->malformed++;continue; }
            if(error != WSAEWOULDBLOCK){
                stream_fail(context, STREAM_SOCKET_ERROR);
                break;
            }
            if(context->origin_qpc != 0){
                int64_t grace = (int64_t)(STREAM_GAP_GRACE_S * c->frequency);
                if(!stream_fill_until(context, received - grace)) break;
            }else if((double)(received - started) / c->frequency >= 6.0){
                stream_fail(context, STREAM_START_TIMEOUT);
                break;
            }
            Sleep(1);
            continue;
        }
        {
            int samples = dlg_packet_sample_count(&packet, (size_t)n, c->rate_hz);
            dlg_sample_plan_t plan;
            if(samples == 0){ context->malformed++; continue; }
            if((samples - 1) / (double)packet.rate_hz >= STREAM_MAX_SOURCE_AGE_S){
                stream_fail(context, STREAM_TIMING_ERROR);
                break;
            }
            /* The hardware reports its configured/quantized frequency. Use it
               for burst spacing; the requested rate only defines row count. */
            context->reported_rate_hz = packet.rate_hz;
            context->clock.period_ticks = (double)c->frequency / packet.rate_hz;
            context->last_packet_receive_qpc = received;
            if(!dlg_sample_clock_plan(&context->clock, (uint32_t)packet.frame,
                                      samples, received, &plan)){
                stream_archive_late(context, &packet, samples, received);
                continue;
            }
            if(context->last_emitted_qpc != 0){
                /* Explicit frame/time gaps become NULL; a timeout already
                   materialized in the archive must not be inserted twice. */
                int64_t limit = plan.first_qpc -
                    (int64_t)llround(context->clock.period_ticks * 0.5);
                if(!stream_fill_until(context, limit)) break;
            }
            for(int i = 0; i < samples && !stream_read(&context->stop) &&
                            context->produced < c->total_rows; ++i){
                stream_record_t record;
                memset(&record, 0, sizeof(record));
                record.qpc = dlg_sample_plan_qpc(&context->clock, &plan, i);
                record.has_value = 1;
                record.frame = (uint32_t)packet.frame;
                record.device_ts = packet.ts;
                record.receive_qpc = received;
                record.burst_index = i;
                record.burst_samples = samples;
                memcpy(record.raw, &packet.samples[i * 8], sizeof(record.raw));
                if(record.qpc <= context->last_emitted_qpc){
                    record.idx = -1;
                    context->late++;
                    if(!stream_queue_push(context, &record)) break;
                    continue;
                }
                if(!stream_emit(context, &record)) break;
            }
        }
    }
    if(stream_read(&context->failure) != STREAM_OK){
        encoder_state_output_t failed = context->last_output;
        failed.accepted = 0;
        failed.health = ENCODER_HEALTH_FAILED;
        failed.sample_status = ENCODER_SAMPLE_MISSING;
        context->failure_published = stream_publish(context, stream_now(), &failed);
    }
    InterlockedExchange(&context->done, 1);
    return 0;
}

static double stream_channel(const dlg_encoder_stream_config_t *c,
                              const stream_record_t *r, int ch){
    double value = c->calibrated[ch]
        ? c->slope[ch] * r->raw[ch] + c->intercept[ch] : r->raw[ch];
    if(c->normalize_degrees[ch]){
        value = fmod(value, 360.0);
        if(value < 0.0) value += 360.0;
    }
    return value;
}

static int stream_write_data(FILE *file, const dlg_encoder_stream_config_t *c,
                              const stream_record_t *r, int idx, double t_s){
    char values[8][48],atrito[48]="NULL";
    for(int ch = 0; ch < 8; ++ch){
        int n;
        if(!r->has_value) strcpy(values[ch],"NULL");
        else if(c->calibrated[ch]){
            n=_snprintf_s(values[ch],sizeof(values[ch]),_TRUNCATE,"%.6f",stream_channel(c,r,ch));
            if(n<0) return 0;
        }else{
            n=_snprintf_s(values[ch],sizeof(values[ch]),_TRUNCATE,"%d",r->raw[ch]);
            if(n<0) return 0;
        }
    }
    if(r->has_value && c->force_normal_n > 0.0){
        if(_snprintf_s(atrito,sizeof(atrito),_TRUNCATE,"%.6f",stream_channel(c,r,0)/c->force_normal_n)<0) return 0;
    }
    return fprintf(file,"%d,%lld,%.9f,%s,%s,%s,%s,%s,%s,%s,%s,%s,%d\n",
        idx,(long long)r->qpc,t_s,values[0],values[1],values[2],values[3],
        values[4],values[5],values[6],values[7],atrito,r->has_value?0:1)>=0;
}

static int stream_write_record(const dlg_encoder_stream_config_t *c,
                                const stream_record_t *r, int64_t origin){
    const encoder_state_output_t *e = &r->encoder;
    double t_s = (double)(r->qpc - origin) / c->frequency;
    int64_t written;
#ifdef DLG_STREAM_TESTING
    dlg_stream_test_before_write();
#endif
    written = stream_now();
    if(r->idx >= 0){
        char angle[32] = "NULL";
        if(!stream_write_data(c->data_file, c, r, r->idx, t_s)) return 0;
        if(c->compat_file && r->idx % c->compat_decimation == 0 &&
           !stream_write_data(c->compat_file,c,r,r->idx/c->compat_decimation,t_s)) return 0;
        if(r->has_value) _snprintf_s(angle,sizeof(angle),_TRUNCATE,"%.6f",stream_channel(c,r,2));
        if(fprintf(c->encoder_file,
            "%d,%lld,%.9f,%s,%d,%s,%s,%.10g,%.10g,%.10g,%.10g,%.10g,%.10g,%d,%d,%d,%.10g,%.10g\n",
            r->idx,(long long)r->qpc,t_s,angle,e->accepted,
            encoder_sample_status_name(e->sample_status),encoder_health_name(e->health),
            e->unwrapped_deg,e->relative_deg,e->relative_mm,e->path_distance_mm,
            e->speed_mm_s,e->disk_rpm,(int)e->direction,e->stationary,
            e->reversal_confirmed,e->extreme_relative_mm,e->accepted_age_s) < 0) return 0;
    }
    if(fprintf(c->timing_file,"%d,%lld,%lld,%lld,%u,%ld,%d,%d,%.6f,%.6f,%d",
        r->idx,(long long)r->qpc,(long long)r->receive_qpc,
        (long long)r->publish_qpc,(unsigned)r->frame,(long)r->device_ts,
        r->burst_index,r->burst_samples,
        r->publish_qpc ? 1000.0*(double)(r->publish_qpc-r->qpc)/c->frequency : -1.0,
        r->publish_qpc ? 1000.0*(double)(written-r->publish_qpc)/c->frequency : -1.0,
        r->idx < 0 ? 2 : r->has_value ? 0 : 1) < 0) return 0;
    for(int ch=0;ch<8;++ch){
        if(r->has_value){ if(fprintf(c->timing_file,",%d",r->raw[ch])<0) return 0; }
        else if(fprintf(c->timing_file,",NULL")<0) return 0;
    }
    return fprintf(c->timing_file,"\n")>=0;
}

int dlg_encoder_stream_run(const dlg_encoder_stream_config_t *c){
    stream_context_t *context;
    HANDLE thread;
    int written = 0, stop_requested = 0, data_emitted = 0, timeout_emitted = 0;
    int64_t started = stream_now(), next_log;
    u_long nonblocking = 1;
    LONG failure;
    if(!c || !c->data_file || !c->encoder_file || !c->timing_file ||
       !c->encoder || c->frequency <= 0 || c->total_rows <= 0 ||
       c->control_socket == INVALID_SOCKET || !c->poll_command || !c->log_event ||
       (c->compat_file && c->compat_decimation <= 0)) return 2;
    context = (stream_context_t *)calloc(1,sizeof(*context));
    if(!context) return 2;
    context->config = c;
    context->records = (stream_record_t *)calloc(STREAM_QUEUE_CAPACITY,sizeof(stream_record_t));
    if(!context->records || !dlg_sample_clock_init(&context->clock,c->frequency,c->rate_hz) ||
       ioctlsocket(c->control_socket,FIONBIO,&nonblocking)!=0){
        free(context->records);free(context);return 2;
    }
    InitializeCriticalSection(&context->mutex);
    c->log_event("ACQUISITION_CLOCK mode=RECEIVE_DRIVEN_V1 control_thread=1 archive_queue_limit=%d source_max_age_ms=100.000 gap_grace_ms=50.000 clock_phase_limit_ppm=500 csv_pair=source_sample nominal_rows=%d",
                 STREAM_QUEUE_CAPACITY,c->total_rows);
    if(ferror(c->data_file)||ferror(c->encoder_file)||
       fprintf(c->timing_file,"idx,source_qpc,receive_qpc,publish_qpc,frame,device_ts,burst_index,burst_samples,source_age_ms,archive_delay_ms,err,raw_ch1,raw_ch2,raw_ch3,raw_ch4,raw_ch5,raw_ch6,raw_ch7,raw_ch8\n")<0){
        DeleteCriticalSection(&context->mutex);free(context->records);free(context);return 2;
    }
    thread = CreateThread(NULL,0,stream_producer,context,0,NULL);
    if(!thread){ DeleteCriticalSection(&context->mutex);free(context->records);free(context);return 2; }
    next_log = started + c->frequency;
    for(;;){
        stream_record_t record;
        int command = c->poll_command(c->use_ipc);
        int64_t now = stream_now();
        if(command == 1 || command == 2){
            stop_requested = 1;
            InterlockedExchange(&context->stop,1);
            c->log_event("STOP received at idx=%d/%d source=ACQUISITION_CLOCK command=%s",
                         written,c->total_rows,command==2?"PAUSE":"STOP");
        }
        if(!data_emitted && stream_read(&context->ready)>=3){
            data_emitted=1;
            if(c->use_ipc){ puts("DATA_OK");fflush(stdout); }
            c->log_event("DATA state emitted: DATA_OK source=ACQUISITION_CLOCK");
        }
        if(!timeout_emitted && !data_emitted && (double)(now-started)/c->frequency>=3.0){
            timeout_emitted=1;
            if(c->use_ipc){ puts("DATA_TIMEOUT");fflush(stdout); }
            c->log_event("DATA state emitted: DATA_TIMEOUT source=ACQUISITION_CLOCK");
        }
        if(stream_queue_pop(context,&record)){
            /* origin_qpc is assigned before enqueue; acquire/release is protected
               by the queue lock. It is immutable for the life of this capture. */
            if(!stream_write_record(c,&record,context->origin_qpc)){
                stream_fail(context,STREAM_WRITE_ERROR);
                break;
            }
            if(record.idx>=0) written++;
        }else{
            if(stream_read(&context->done)) break;
            Sleep(1);
        }
        if(now>=next_log){
            int queued;
            EnterCriticalSection(&context->mutex);queued=context->count;LeaveCriticalSection(&context->mutex);
            c->log_event("ACQUISITION_PROGRESS written=%d/%d archive_queue=%d feedback_fifo=0",
                         written,c->total_rows,queued);
            next_log=now+c->frequency;
        }
    }
    InterlockedExchange(&context->stop,1);
    WaitForSingleObject(thread,INFINITE); /* nonblocking sockets; no disk IO in producer */
    CloseHandle(thread);
    failure=stream_read(&context->failure);
    if(failure==STREAM_OK && (ferror(c->data_file)||ferror(c->encoder_file)||
       ferror(c->timing_file)||(c->compat_file&&ferror(c->compat_file)))) failure=STREAM_WRITE_ERROR;
    if(failure!=STREAM_OK && !context->failure_published){
        encoder_state_output_t failed=context->last_output;
        failed.accepted=0;failed.health=ENCODER_HEALTH_FAILED;
        failed.sample_status=ENCODER_SAMPLE_MISSING;
        context->failure_published=stream_publish(context,stream_now(),&failed);
    }
    c->log_event("ACQUISITION_END produced=%d written=%d have=%d null=%d late_raw=%d malformed=%d startup_discarded=%d archive_queue_max=%d source_age_max_ms=%.6f duplicates=%llu reordered=%llu frame_gaps=%llu timing_rejects=%llu reported_rate_hz=%.9f failure=%ld",
        context->produced,written,context->have,context->missing,context->late,
        context->malformed,context->startup_discarded,context->max_count,
        context->max_source_age_ms,(unsigned long long)context->clock.duplicates,
        (unsigned long long)context->clock.reordered,
        (unsigned long long)context->clock.missing_frames,
        (unsigned long long)context->clock.timing_rejects,context->reported_rate_hz,(long)failure);
    c->log_event("END idx=%d/%d stop=%d have=%d null=%d loss_pct=%.6f ring_overrun=0 effective_rate_hz=%.6f clock=RECEIVE_DRIVEN_V1 failure=%ld",
        written,c->total_rows,stop_requested,context->have,context->missing,
        context->produced>0?100.0*context->missing/context->produced:0.0,
        context->origin_qpc>0 && context->last_emitted_qpc>context->origin_qpc
        ? (context->produced-1)*(double)c->frequency/(context->last_emitted_qpc-context->origin_qpc):0.0,
        (long)failure);
    c->log_event("ENCODER_CONTROL_END session=%llu sequence=%llu sent=%llu send_errors=%llu",
        (unsigned long long)c->session,(unsigned long long)context->sequence,
        (unsigned long long)context->sent,(unsigned long long)context->send_errors);
    {
        encoder_quality_counters_t quality;
        encoder_state_get_quality(c->encoder,&quality);
        c->log_event("ENCODER_END input=%llu accepted=%llu missing=%llu quarantine=%llu saturated=%llu non_monotonic=%llu impossible_jump=%llu reversals=%llu max_gap_s=%.9f",
            (unsigned long long)quality.input_samples,(unsigned long long)quality.accepted_samples,
            (unsigned long long)quality.missing_samples,(unsigned long long)quality.quarantine_samples,
            (unsigned long long)quality.saturated_samples,(unsigned long long)quality.non_monotonic_samples,
            (unsigned long long)quality.impossible_jump_samples,(unsigned long long)quality.reversals,
            quality.max_accepted_gap_s);
    }
    if(failure!=STREAM_OK){
        fprintf(stderr,"ENCODER_FAILED reason=ACQUISITION_CLOCK failure=%ld\n",(long)failure);
        c->log_event("FATAL ACQUISITION_CLOCK failure=%ld",(long)failure);
    }
    DeleteCriticalSection(&context->mutex);
    free(context->records);free(context);
    return failure==STREAM_OK?0:2;
}
