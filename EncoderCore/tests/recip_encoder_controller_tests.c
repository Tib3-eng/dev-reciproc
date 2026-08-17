#include "recip_encoder_controller.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(expr) do { \
    if(!(expr)){ \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        failures++; \
    } \
} while(0)

static encoder_control_packet_t make_packet(
    uint64_t session,
    uint64_t sequence,
    int64_t qpc,
    double position_mm,
    double path_mm,
    int direction,
    unsigned extra_flags,
    encoder_health_t health
){
    encoder_state_output_t output;
    encoder_control_packet_t packet;
    memset(&output, 0, sizeof(output));
    output.accepted = (extra_flags & ENCODER_CONTROL_FLAG_ACCEPTED) != 0u;
    output.initialized = (extra_flags & ENCODER_CONTROL_FLAG_INITIALIZED) != 0u;
    output.stationary = (extra_flags & ENCODER_CONTROL_FLAG_STATIONARY) != 0u;
    output.reversal_confirmed = (extra_flags & ENCODER_CONTROL_FLAG_REVERSAL) != 0u;
    output.relative_mm = position_mm;
    output.path_distance_mm = path_mm;
    output.speed_mm_s = direction * 5.0;
    output.extreme_relative_mm = position_mm;
    output.accepted_age_s = 0.0;
    output.direction = (encoder_direction_t)direction;
    output.health = health;
    output.sample_status = output.accepted
        ? ENCODER_SAMPLE_ACCEPTED : ENCODER_SAMPLE_QUARANTINE;
    CHECK(encoder_control_packet_build(&packet, session, sequence, qpc, &output));
    return packet;
}

static recip_encoder_config_t config_for(uint64_t session){
    recip_encoder_config_t config;
    recip_encoder_config_default(&config);
    config.session_id = session;
    config.course_mm = 10.0;
    config.total_mm = 30.0;
    config.tolerance_mm = 0.5;
    config.stroke_timeout_s = 30.0;
    config.position_filter_samples = 1;
    config.target_band_mm = 0.0;
    return config;
}

static void test_nominal_fixed_endpoints_and_completion(void){
    const uint64_t session = 11u;
    const unsigned valid = ENCODER_CONTROL_FLAG_ACCEPTED |
                           ENCODER_CONTROL_FLAG_INITIALIZED;
    recip_encoder_config_t config = config_for(session);
    recip_encoder_controller_t controller;
    recip_encoder_decision_t decision;
    encoder_control_packet_t packet = make_packet(
        session, 1u, 100, 0.0, 0.0, 0, valid, ENCODER_HEALTH_OK);
    CHECK(recip_encoder_controller_start(&controller, &config, 1000, &packet, &decision));
    CHECK(fabs(decision.target_mm - 10.0) < 1.0e-12);

    packet = make_packet(session, 2u, 200, 9.9, 9.9, 1, valid, ENCODER_HEALTH_OK);
    CHECK(recip_encoder_controller_update(&controller, &packet, sizeof(packet), &decision) ==
          RECIP_ENCODER_ACTION_NONE);
    packet = make_packet(session, 3u, 210, 10.2, 10.2, 1, valid, ENCODER_HEALTH_OK);
    CHECK(recip_encoder_controller_update(&controller, &packet, sizeof(packet), &decision) ==
          RECIP_ENCODER_ACTION_REVERSE);
    CHECK(decision.command_direction == -1);
    CHECK(decision.completed_strokes == 1u);
    CHECK((decision.events & RECIP_ENCODER_EVENT_OUTSIDE_TOLERANCE) == 0u);
    CHECK(fabs(decision.target_mm) < 1.0e-12);

    /* Duplicata e oscilacao perto do extremo nao podem inverter novamente. */
    CHECK(recip_encoder_controller_update(&controller, &packet, sizeof(packet), &decision) ==
          RECIP_ENCODER_ACTION_NONE);
    CHECK(controller.completed_strokes == 1u && !controller.faulted);
    packet = make_packet(session, 4u, 220, 10.3, 10.3, 1, valid, ENCODER_HEALTH_OK);
    CHECK(recip_encoder_controller_update(&controller, &packet, sizeof(packet), &decision) ==
          RECIP_ENCODER_ACTION_NONE);
    packet = make_packet(session, 5u, 230, 10.4, 10.4, -1,
                         valid, ENCODER_HEALTH_OK);
    CHECK(recip_encoder_controller_update(&controller, &packet, sizeof(packet), &decision) ==
          RECIP_ENCODER_ACTION_NONE);
    packet = make_packet(session, 6u, 240, 10.0, 10.8, -1,
                         valid, ENCODER_HEALTH_OK);
    CHECK(recip_encoder_controller_update(&controller, &packet, sizeof(packet), &decision) ==
          RECIP_ENCODER_ACTION_NONE);
    CHECK((decision.events & RECIP_ENCODER_EVENT_PHYSICAL_REVERSAL) != 0u);

    packet = make_packet(session, 7u, 300, 0.0, 20.8, -1, valid, ENCODER_HEALTH_OK);
    CHECK(recip_encoder_controller_update(&controller, &packet, sizeof(packet), &decision) ==
          RECIP_ENCODER_ACTION_REVERSE);
    CHECK(decision.command_direction == 1);
    CHECK(fabs(decision.target_mm - 10.0) < 1.0e-12);
    packet = make_packet(
        session, 8u, 310, -0.2, 21.0, 1,
        valid, ENCODER_HEALTH_OK);
    CHECK(recip_encoder_controller_update(&controller, &packet, sizeof(packet), &decision) ==
          RECIP_ENCODER_ACTION_NONE);
    packet = make_packet(session, 9u, 320, 0.2, 21.4, 1, valid, ENCODER_HEALTH_OK);
    CHECK(recip_encoder_controller_update(&controller, &packet, sizeof(packet), &decision) ==
          RECIP_ENCODER_ACTION_NONE);
    CHECK((decision.events & RECIP_ENCODER_EVENT_PHYSICAL_REVERSAL) != 0u);

    /* Distancia total so conclui ao fechar o stroke no extremo fixo. */
    packet = make_packet(session, 10u, 400, 9.0, 30.1, 1, valid, ENCODER_HEALTH_OK);
    CHECK(recip_encoder_controller_update(&controller, &packet, sizeof(packet), &decision) ==
          RECIP_ENCODER_ACTION_NONE);
    packet = make_packet(session, 11u, 410, 10.0, 31.1, 1, valid, ENCODER_HEALTH_OK);
    CHECK(recip_encoder_controller_update(&controller, &packet, sizeof(packet), &decision) ==
          RECIP_ENCODER_ACTION_COMPLETE);
    CHECK(decision.completed_strokes == 3u);
}

static void test_quarantine_and_stroke_limit(void){
    const uint64_t session = 22u;
    const unsigned valid = ENCODER_CONTROL_FLAG_ACCEPTED |
                           ENCODER_CONTROL_FLAG_INITIALIZED;
    recip_encoder_config_t config = config_for(session);
    recip_encoder_controller_t controller;
    recip_encoder_decision_t decision;
    encoder_control_packet_t packet = make_packet(
        session, 1u, 100, 0.0, 0.0, 0, valid, ENCODER_HEALTH_OK);
    CHECK(recip_encoder_controller_start(&controller, &config, 1000, &packet, &decision));
    packet = make_packet(
        session, 2u, 110, 50.0, 0.0, 1,
        ENCODER_CONTROL_FLAG_INITIALIZED, ENCODER_HEALTH_DEGRADED);
    CHECK(recip_encoder_controller_update(&controller, &packet, sizeof(packet), &decision) ==
          RECIP_ENCODER_ACTION_NONE);
    CHECK(controller.completed_strokes == 0u && !controller.faulted);
    packet = make_packet(session, 3u, 120, 10.0, 10.0, 1, valid, ENCODER_HEALTH_OK);
    CHECK(recip_encoder_controller_update(&controller, &packet, sizeof(packet), &decision) ==
          RECIP_ENCODER_ACTION_REVERSE);
    packet = make_packet(session, 4u, 130, 20.0, 20.0, 1, valid, ENCODER_HEALTH_OK);
    CHECK(recip_encoder_controller_update(&controller, &packet, sizeof(packet), &decision) ==
          RECIP_ENCODER_ACTION_FAULT);
    CHECK(decision.fault == RECIP_ENCODER_FAULT_STROKE_LIMIT);
}

static void test_timeouts_and_failure(void){
    const unsigned valid = ENCODER_CONTROL_FLAG_ACCEPTED |
                           ENCODER_CONTROL_FLAG_INITIALIZED;
    recip_encoder_controller_t controller;
    recip_encoder_decision_t decision;
    encoder_control_packet_t packet;
    recip_encoder_config_t config = config_for(33u);
    packet = make_packet(33u, 1u, 100, 0.0, 0.0, 0, valid, ENCODER_HEALTH_OK);
    CHECK(recip_encoder_controller_start(&controller, &config, 1000, &packet, &decision));
    CHECK(recip_encoder_controller_tick(&controller, 3101, &decision) ==
          RECIP_ENCODER_ACTION_FAULT);
    CHECK(decision.fault == RECIP_ENCODER_FAULT_ENCODER_TIMEOUT);

    config = config_for(34u);
    config.stroke_timeout_s = 1.0;
    packet = make_packet(34u, 1u, 100, 0.0, 0.0, 0, valid, ENCODER_HEALTH_OK);
    CHECK(recip_encoder_controller_start(&controller, &config, 1000, &packet, &decision));
    packet = make_packet(34u, 2u, 600, 1.0, 1.0, 1, valid, ENCODER_HEALTH_OK);
    CHECK(recip_encoder_controller_update(&controller, &packet, sizeof(packet), &decision) ==
          RECIP_ENCODER_ACTION_NONE);
    CHECK(recip_encoder_controller_tick(&controller, 1200, &decision) ==
          RECIP_ENCODER_ACTION_FAULT);
    CHECK(decision.fault == RECIP_ENCODER_FAULT_STROKE_TIMEOUT);

    config = config_for(36u);
    packet = make_packet(36u, 1u, 100, 0.0, 0.0, 0, valid, ENCODER_HEALTH_OK);
    CHECK(recip_encoder_controller_start(&controller, &config, 1000, &packet, &decision));
    packet = make_packet(36u, 2u, 110, 0.0, 0.0, 0, valid, ENCODER_HEALTH_FAILED);
    CHECK(recip_encoder_controller_update(&controller, &packet, sizeof(packet), &decision) ==
          RECIP_ENCODER_ACTION_FAULT);
    CHECK(decision.fault == RECIP_ENCODER_FAULT_ENCODER_FAILED);
}

static void test_noisy_signal_uses_strokes_not_total_variation(void){
    const uint64_t session = 55u;
    const unsigned valid = ENCODER_CONTROL_FLAG_ACCEPTED |
                           ENCODER_CONTROL_FLAG_INITIALIZED;
    recip_encoder_config_t config;
    recip_encoder_controller_t controller;
    recip_encoder_decision_t decision;
    encoder_control_packet_t packet;
    uint64_t sequence = 1u;
    int64_t qpc = 100;
    int reverse_actions = 0;
    int physical_reversals = 0;
    int completed = 0;
    double noisy_path = 0.0;
    recip_encoder_config_default(&config);
    config.session_id = session;
    config.course_mm = 10.0;
    config.total_mm = 30.0;
    config.initial_direction = 1;
    config.stroke_timeout_s = 10.0;
    packet = make_packet(session, sequence++, qpc, 0.0, 0.0, 0,
                         valid, ENCODER_HEALTH_OK);
    CHECK(recip_encoder_controller_start(&controller, &config, 1000, &packet, &decision));
    for(int stroke = 0; stroke < 3 && !completed; ++stroke){
        int direction = (stroke % 2 == 0) ? 1 : -1;
        double start = direction > 0 ? 0.0 : 10.0;
        for(int sample = 1; sample <= 430 && !completed; ++sample){
            double ideal = start + direction * (10.5 * (double)sample / 430.0);
            double noise = (sample % 4 == 0) ? 0.18 :
                           (sample % 4 == 1) ? -0.16 :
                           (sample % 4 == 2) ? 0.12 : -0.14;
            double position = ideal + noise;
            noisy_path += fabs(direction * 10.5 / 430.0 + noise);
            qpc += 5;
            packet = make_packet(session, sequence++, qpc, position,
                                 noisy_path, direction, valid, ENCODER_HEALTH_OK);
            switch(recip_encoder_controller_update(
                       &controller, &packet, sizeof(packet), &decision)){
                case RECIP_ENCODER_ACTION_REVERSE: reverse_actions++; break;
                case RECIP_ENCODER_ACTION_COMPLETE: completed = 1; break;
                case RECIP_ENCODER_ACTION_FAULT: completed = -1; break;
                default: break;
            }
            if((decision.events & RECIP_ENCODER_EVENT_PHYSICAL_REVERSAL) != 0u){
                physical_reversals++;
            }
        }
    }
    CHECK(completed == 1);
    CHECK(reverse_actions == 2);
    CHECK(physical_reversals == 2);
    CHECK(controller.completed_strokes == 3u);
    CHECK(fabs(controller.last_path_mm - 30.0) < 1.0e-9);
    CHECK(noisy_path > 100.0);
}

static void test_reverse_initial_direction_and_bad_datagram(void){
    const uint64_t session = 44u;
    const unsigned valid = ENCODER_CONTROL_FLAG_ACCEPTED |
                           ENCODER_CONTROL_FLAG_INITIALIZED;
    recip_encoder_config_t config = config_for(session);
    recip_encoder_controller_t controller;
    recip_encoder_decision_t decision;
    encoder_control_packet_t packet;
    config.initial_direction = -1;
    packet = make_packet(session, 1u, 100, 5.0, 0.0, 0, valid, ENCODER_HEALTH_OK);
    CHECK(recip_encoder_controller_start(&controller, &config, 1000, &packet, &decision));
    CHECK(fabs(decision.target_mm + 5.0) < 1.0e-12);
    packet = make_packet(session + 1u, 2u, 110, -5.0, 10.0, -1, valid, ENCODER_HEALTH_OK);
    CHECK(recip_encoder_controller_update(&controller, &packet, sizeof(packet), &decision) ==
          RECIP_ENCODER_ACTION_NONE);
    CHECK(controller.last_sequence == 1u && !controller.faulted);
    packet = make_packet(session, 2u, 120, -5.0, 10.0, -1, valid, ENCODER_HEALTH_OK);
    CHECK(recip_encoder_controller_update(&controller, &packet, sizeof(packet), &decision) ==
          RECIP_ENCODER_ACTION_REVERSE);
    CHECK(decision.command_direction == 1);
    CHECK(fabs(decision.target_mm - 5.0) < 1.0e-12);
}

static void test_causal_stopping_anticipation_and_clamp(void){
    const uint64_t session = 66u;
    const unsigned valid = ENCODER_CONTROL_FLAG_ACCEPTED |
                           ENCODER_CONTROL_FLAG_INITIALIZED;
    recip_encoder_config_t config = config_for(session);
    recip_encoder_controller_t controller;
    recip_encoder_decision_t decision;
    encoder_control_packet_t packet;

    config.stop_anticipation_enabled = 1;
    config.stop_model_slope_s = 0.1;
    config.stop_margin_mm = 0.2;
    config.stop_velocity_window_s = 0.25;
    config.stop_max_course_fraction = 0.45;
    packet = make_packet(session, 1u, 100, 0.0, 0.0, 0, valid, ENCODER_HEALTH_OK);
    CHECK(recip_encoder_controller_start(&controller, &config, 1000, &packet, &decision));

    /* Movimento linear de 10 mm/s: antecipacao = 0.1*10 + 0.2 = 1.2 mm. */
    packet = make_packet(session, 2u, 700, 6.0, 6.0, 1, valid, ENCODER_HEALTH_OK);
    CHECK(recip_encoder_controller_update(&controller, &packet, sizeof(packet), &decision) ==
          RECIP_ENCODER_ACTION_NONE);
    packet = make_packet(session, 3u, 800, 7.0, 7.0, 1, valid, ENCODER_HEALTH_OK);
    CHECK(recip_encoder_controller_update(&controller, &packet, sizeof(packet), &decision) ==
          RECIP_ENCODER_ACTION_NONE);
    packet = make_packet(session, 4u, 970, 8.7, 8.7, 1, valid, ENCODER_HEALTH_OK);
    CHECK(recip_encoder_controller_update(&controller, &packet, sizeof(packet), &decision) ==
          RECIP_ENCODER_ACTION_NONE);
    packet = make_packet(session, 5u, 990, 8.9, 8.9, 1, valid, ENCODER_HEALTH_OK);
    CHECK(recip_encoder_controller_update(&controller, &packet, sizeof(packet), &decision) ==
          RECIP_ENCODER_ACTION_REVERSE);
    CHECK(fabs(decision.velocity_estimate_mm_s - 10.0) < 1.0e-9);
    CHECK(fabs(decision.stopping_anticipation_mm - 1.2) < 1.0e-9);
    CHECK(fabs(decision.trigger_position_mm - 8.8) < 1.0e-9);
    CHECK(fabs(decision.physical_target_mm - 10.0) < 1.0e-9);
    CHECK(!decision.stopping_anticipation_clamped);
    packet = make_packet(session, 6u, 1010, 9.5, 9.5, 1, valid, ENCODER_HEALTH_OK);
    CHECK(recip_encoder_controller_update(&controller, &packet, sizeof(packet), &decision) ==
          RECIP_ENCODER_ACTION_NONE);
    packet = make_packet(session, 7u, 1030, 9.2, 9.8, -1, valid, ENCODER_HEALTH_OK);
    CHECK(recip_encoder_controller_update(&controller, &packet, sizeof(packet), &decision) ==
          RECIP_ENCODER_ACTION_NONE);
    CHECK((decision.events & RECIP_ENCODER_EVENT_PHYSICAL_REVERSAL) != 0u);
    CHECK(fabs(decision.physical_target_mm - 10.0) < 1.0e-9);
    CHECK(fabs(decision.trigger_position_mm - 8.8) < 1.0e-9);
    CHECK(fabs(decision.stopping_anticipation_mm - 1.2) < 1.0e-9);

    config = config_for(67u);
    config.course_mm = 4.0;
    config.total_mm = 8.0;
    config.stop_anticipation_enabled = 1;
    config.stop_model_slope_s = 0.132;
    config.stop_margin_mm = 0.27025;
    config.stop_velocity_window_s = 0.25;
    config.stop_max_course_fraction = 0.45;
    packet = make_packet(67u, 1u, 100, 0.0, 0.0, 0, valid, ENCODER_HEALTH_OK);
    CHECK(recip_encoder_controller_start(&controller, &config, 1000, &packet, &decision));
    packet = make_packet(67u, 2u, 200, 2.0, 2.0, 1, valid, ENCODER_HEALTH_OK);
    CHECK(recip_encoder_controller_update(&controller, &packet, sizeof(packet), &decision) ==
          RECIP_ENCODER_ACTION_NONE);
    packet = make_packet(67u, 3u, 210, 2.2, 2.2, 1, valid, ENCODER_HEALTH_OK);
    CHECK(recip_encoder_controller_update(&controller, &packet, sizeof(packet), &decision) ==
          RECIP_ENCODER_ACTION_REVERSE);
    CHECK(decision.stopping_anticipation_clamped);
    CHECK(fabs(decision.stopping_anticipation_mm - 1.8) < 1.0e-9);
    CHECK(fabs(decision.trigger_position_mm - 2.2) < 1.0e-9);
}

int main(void){
    test_nominal_fixed_endpoints_and_completion();
    test_quarantine_and_stroke_limit();
    test_timeouts_and_failure();
    test_reverse_initial_direction_and_bad_datagram();
    test_noisy_signal_uses_strokes_not_total_variation();
    test_causal_stopping_anticipation_and_clamp();
    if(failures){
        fprintf(stderr, "recip_encoder_controller_tests: %d falha(s)\n", failures);
        return 1;
    }
    puts("recip_encoder_controller_tests: OK");
    return 0;
}
