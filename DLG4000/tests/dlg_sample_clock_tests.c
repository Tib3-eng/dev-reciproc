#include "dlg_sample_clock.h"
#include "recip_encoder_controller.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(test) do { if(!(test)){ \
    fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#test);failures++; \
} } while(0)

static void test_wire_and_padding(void){
    dlg_data_packet_t packet;
    size_t header = offsetof(dlg_data_packet_t,samples);
    size_t packet_size = sizeof(packet);
    memset(&packet,0,sizeof(packet));
    packet.code=0x100B;packet.signals=8;packet.bursts=3;packet.rate_hz=200.0f;
    CHECK(header==32u);
    CHECK(packet_size==1472u);
    CHECK(dlg_packet_sample_count(&packet,header+3*8*sizeof(int16_t),200)==3);
    CHECK(dlg_packet_sample_count(&packet,sizeof(packet),200)==3);
    CHECK(dlg_packet_sample_count(&packet,header+2*8*sizeof(int16_t),200)==0);
    CHECK(dlg_packet_sample_count(&packet,header-1,200)==0);
    CHECK(dlg_packet_sample_count(&packet,header+1,200)==0);
    packet.signals=1;
    CHECK(dlg_packet_sample_count(&packet,sizeof(packet),200)==0);
    packet.signals=8;packet.rate_hz=100;
    CHECK(dlg_packet_sample_count(&packet,sizeof(packet),200)==0);
}

static void test_long_clock_drift(double ppm){
    dlg_sample_clock_t clock;
    dlg_sample_plan_t plan;
    const int64_t frequency=10000000;
    const int64_t origin=100000000;
    double source_rate=200.0*(1.0+ppm*1e-6);
    int64_t previous=0;
    double max_age=0;
    CHECK(dlg_sample_clock_init(&clock,frequency,200));
    for(int packet=0;packet<150000;++packet){
        int64_t received=origin+(int64_t)llround((3.0*packet+2)*frequency/source_rate);
        /* Receipt jitter, not acquisition drift. */
        received+=(packet%7)*700;
        CHECK(dlg_sample_clock_plan(&clock,(uint32_t)packet,3,received,&plan));
        for(int i=0;i<3;++i){
            int64_t qpc=dlg_sample_plan_qpc(&clock,&plan,i);
            CHECK(qpc>previous);
            CHECK(qpc<=received);
            previous=qpc;
        }
        if((double)(received-plan.last_qpc)>max_age) max_age=(double)(received-plan.last_qpc);
    }
    /* 37.5 minutes, +/-100 ppm: age remains bounded by receipt jitter.
       A nominal FIFO consumer would accumulate +/-45 samples at 100 ppm. */
    CHECK(max_age/frequency<0.002);
    CHECK(clock.timing_rejects==0u);
}

static void test_delay_is_not_relabelled_fresh(void){
    dlg_sample_clock_t clock;
    dlg_sample_plan_t plan;
    const int64_t frequency=10000000, origin=100000000;
    CHECK(dlg_sample_clock_init(&clock,frequency,200));
    CHECK(dlg_sample_clock_plan(&clock,10,3,origin,&plan));
    CHECK(dlg_sample_clock_plan(&clock,11,3,origin+2150000,&plan));
    CHECK((origin+2150000-plan.last_qpc)/(double)frequency>.199);
    for(uint32_t frame=12;frame<=24;++frame){
        CHECK(dlg_sample_clock_plan(&clock,frame,3,origin+2150000,&plan));
        CHECK(plan.last_qpc<=origin+2150000);
    }
}

static void test_frames_and_gap(void){
    dlg_sample_clock_t clock;
    dlg_sample_plan_t plan;
    CHECK(dlg_sample_clock_init(&clock,10000000,200));
    CHECK(dlg_sample_clock_plan(&clock,0xFFFFFFFEu,3,100000000,&plan));
    CHECK(dlg_sample_clock_plan(&clock,0xFFFFFFFFu,3,100150000,&plan));
    CHECK(dlg_sample_clock_plan(&clock,0u,3,100300000,&plan));
    CHECK(!dlg_sample_clock_plan(&clock,0u,3,100300000,&plan));
    CHECK(!dlg_sample_clock_plan(&clock,0xFFFFFFFFu,3,100300000,&plan));
    CHECK(dlg_sample_clock_plan(&clock,3u,3,100750000,&plan));
    CHECK(plan.missing_frames==2u && plan.missing_samples==6u);
    CHECK(clock.duplicates==1u && clock.reordered==1u);
    CHECK(clock.missing_frames==2u);
}

static void test_closed_loop_no_opening_after_37_minutes(void){
    dlg_sample_clock_t clock;
    dlg_sample_plan_t plan;
    recip_encoder_controller_t controller;
    recip_encoder_config_t config;
    recip_encoder_decision_t decision;
    encoder_state_output_t output;
    encoder_control_packet_t control_packet;
    const int64_t frequency=10000000,origin=1000000000;
    const double source_rate=200.00329419034, speed=20;
    double position=0,previous_t=0,extreme=0,first_sum=0,last_sum=0;
    int direction=1,strokes=0,first_n=0,last_n=0,started=0;
    uint64_t sequence=0;
    CHECK(dlg_sample_clock_init(&clock,frequency,200));
    recip_encoder_config_default(&config);
    config.session_id=33;config.course_mm=10;config.total_mm=1e9;
    config.max_source_age_s=.1;config.stroke_timeout_s=3;
    config.stop_anticipation_enabled=1;config.stop_model_slope_s=.035;
    config.stop_margin_mm=0;config.physical_distance_enabled=1;
    for(int packet=0;packet<150000;++packet){
        double samples[3];
        double receive_t=(3.0*packet+2)/source_rate;
        int64_t received=origin+(int64_t)llround(receive_t*frequency);
        for(int i=0;i<3;++i){
            double t=(3.0*packet+i)/source_rate;
            position+=direction*speed*(t-previous_t);previous_t=t;
            samples[i]=position;
        }
        CHECK(dlg_sample_clock_plan(&clock,(uint32_t)packet,3,received,&plan));
        for(int i=0;i<3;++i){
            memset(&output,0,sizeof(output));
            output.accepted=output.initialized=1;output.health=ENCODER_HEALTH_OK;
            output.sample_status=ENCODER_SAMPLE_ACCEPTED;
            output.relative_mm=samples[i];output.direction=(encoder_direction_t)direction;
            CHECK(encoder_control_packet_build(&control_packet,33,++sequence,
                dlg_sample_plan_qpc(&clock,&plan,i),&output));
            if(!started){
                CHECK(recip_encoder_controller_start(&controller,&config,frequency,
                    &control_packet,&decision));started=1;
            }else{
                recip_encoder_controller_update(&controller,&control_packet,sizeof(control_packet),&decision);
                CHECK(decision.action!=RECIP_ENCODER_ACTION_FAULT);
                if(decision.action==RECIP_ENCODER_ACTION_REVERSE){
                    double width=fabs(position-extreme);
                    if(strokes>10 && receive_t<225){first_sum+=width;first_n++;}
                    if(receive_t>2025){last_sum+=width;last_n++;}
                    extreme=position;direction=-direction;strokes++;
                }
            }
            CHECK(controller.target_mm==0.0 || controller.target_mm==10.0);
        }
        CHECK(recip_encoder_controller_tick(&controller,received,&decision)!=RECIP_ENCODER_ACTION_FAULT);
    }
    CHECK(first_n>100 && last_n>100);
    CHECK(fabs(last_sum/last_n-first_sum/first_n)<0.04);
    printf("closed_loop 37.5min strokes=%d first_width=%.6f last_width=%.6f delta=%.6f mm\n",
        strokes,first_sum/first_n,last_sum/last_n,last_sum/last_n-first_sum/first_n);
}

int main(void){
    test_wire_and_padding();
    test_long_clock_drift(16.47);
    test_long_clock_drift(100);
    test_long_clock_drift(-100);
    test_delay_is_not_relabelled_fresh();
    test_frames_and_gap();
    test_closed_loop_no_opening_after_37_minutes();
    if(failures){fprintf(stderr,"dlg_sample_clock_tests: %d failures\n",failures);return 1;}
    puts("dlg_sample_clock_tests: OK (37.5min +/-100ppm, jitter, bursts, stale, loss, wrap)");
    return 0;
}
