#define _CRT_SECURE_NO_WARNINGS

#include "recip_encoder_controller.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static void usage(void){
    puts("recip_encoder_replay --input encoder_state.csv --course-mm MM --total-mm MM [--forward-sign -1|0|1] [--stroke-timeout-s S]");
    puts("  [--stop-compensation --stop-slope-s S --stop-margin-mm MM]");
    puts("  [--stop-velocity-window-s S --stop-max-course-fraction F]");
    puts("  [--events-out events.csv]");
}

static encoder_health_t parse_health(const char *text){
    if(text && strcmp(text, "FAILED") == 0) return ENCODER_HEALTH_FAILED;
    if(text && strcmp(text, "STALE_STOP") == 0) return ENCODER_HEALTH_STALE_STOP;
    if(text && strcmp(text, "DEGRADED") == 0) return ENCODER_HEALTH_DEGRADED;
    return ENCODER_HEALTH_OK;
}

int main(int argc, char **argv){
    const uint64_t session = 0x5245504C415931ull;
    const char *input_path = NULL;
    double course_mm = 0.0;
    double total_mm = 0.0;
    double stroke_timeout_s = 30.0;
    int stop_compensation = 0;
    double stop_slope_s = 0.0;
    double stop_margin_mm = 0.0;
    double stop_velocity_window_s = 0.25;
    double stop_max_course_fraction = 0.45;
    int forward_sign = 0;
    const char *events_path = NULL;
    FILE *input;
    FILE *events = NULL;
    char line[2048];
    int header = 1;
    int have_origin = 0;
    int controller_started = 0;
    int complete = 0;
    int fault = 0;
    uint64_t reverse_events = 0;
    uint64_t physical_reversals = 0;
    uint64_t clamped_events = 0;
    uint64_t rows = 0;
    uint64_t valid = 0;
    encoder_control_packet_t origin = {0};
    recip_encoder_controller_t controller = {0};
    recip_encoder_decision_t decision = {0};

    for(int i = 1; i < argc; ++i){
        if(strcmp(argv[i], "--input") == 0 && i + 1 < argc) input_path = argv[++i];
        else if(strcmp(argv[i], "--course-mm") == 0 && i + 1 < argc) course_mm = atof(argv[++i]);
        else if(strcmp(argv[i], "--total-mm") == 0 && i + 1 < argc) total_mm = atof(argv[++i]);
        else if(strcmp(argv[i], "--forward-sign") == 0 && i + 1 < argc) forward_sign = atoi(argv[++i]);
        else if(strcmp(argv[i], "--stroke-timeout-s") == 0 && i + 1 < argc) stroke_timeout_s = atof(argv[++i]);
        else if(strcmp(argv[i], "--stop-compensation") == 0) stop_compensation = 1;
        else if(strcmp(argv[i], "--stop-slope-s") == 0 && i + 1 < argc) stop_slope_s = atof(argv[++i]);
        else if(strcmp(argv[i], "--stop-margin-mm") == 0 && i + 1 < argc) stop_margin_mm = atof(argv[++i]);
        else if(strcmp(argv[i], "--stop-velocity-window-s") == 0 && i + 1 < argc) stop_velocity_window_s = atof(argv[++i]);
        else if(strcmp(argv[i], "--stop-max-course-fraction") == 0 && i + 1 < argc) stop_max_course_fraction = atof(argv[++i]);
        else if(strcmp(argv[i], "--events-out") == 0 && i + 1 < argc) events_path = argv[++i];
        else { usage(); return 2; }
    }
    if(!input_path || course_mm <= 0.0 || total_mm <= 0.0 || stroke_timeout_s <= 0.0 ||
       forward_sign < -1 || forward_sign > 1 ||
       !isfinite(stop_slope_s) || stop_slope_s < 0.0 ||
       !isfinite(stop_margin_mm) || stop_margin_mm < 0.0 ||
       !isfinite(stop_velocity_window_s) || stop_velocity_window_s <= 0.0 ||
       !isfinite(stop_max_course_fraction) || stop_max_course_fraction <= 0.0 ||
       stop_max_course_fraction >= 1.0){
        usage();
        return 2;
    }
    input = fopen(input_path, "rb");
    if(!input){
        fprintf(stderr, "Nao foi possivel abrir %s\n", input_path);
        return 2;
    }
    if(events_path){
        events = fopen(events_path, "wb");
        if(!events){
            fprintf(stderr, "Nao foi possivel criar %s\n", events_path);
            fclose(input);
            return 2;
        }
        fputs("event,sequence,qpc,stroke,active_command_direction,position_mm,physical_target_mm,trigger_mm,anticipation_mm,velocity_estimate_mm_s,anticipation_clamped,path_mm,endpoint_error_mm\n", events);
    }
    while(fgets(line, sizeof(line), input)){
        char *columns[24] = {0};
        int column_count = 0;
        char *token;
        encoder_state_output_t output;
        encoder_control_packet_t packet;
        int64_t qpc;
        uint64_t sequence;
        double relative_mm;
        double path_mm;
        double speed_mm_s;
        double extreme_mm;
        double accepted_age_s;
        int accepted;
        int initialized;
        int direction;
        int stationary;
        int reversal;
        encoder_health_t health;
        if(header){ header = 0; continue; }
        token = strtok(line, ",\r\n");
        while(token && column_count < (int)(sizeof(columns) / sizeof(columns[0]))){
            columns[column_count++] = token;
            token = strtok(NULL, ",\r\n");
        }
        if(column_count < 18) continue;
        rows++;
        sequence = (uint64_t)_strtoui64(columns[0], NULL, 10) + 1u;
        qpc = (int64_t)_strtoi64(columns[1], NULL, 10);
        accepted = atoi(columns[4]) != 0;
        initialized = accepted || strcmp(columns[8], "NULL") != 0;
        relative_mm = strcmp(columns[9], "NULL") == 0 ? 0.0 : atof(columns[9]);
        path_mm = strcmp(columns[10], "NULL") == 0 ? 0.0 : atof(columns[10]);
        speed_mm_s = strcmp(columns[11], "NULL") == 0 ? 0.0 : atof(columns[11]);
        direction = atoi(columns[13]);
        stationary = atoi(columns[14]);
        reversal = atoi(columns[15]);
        extreme_mm = strcmp(columns[16], "NULL") == 0 ? relative_mm : atof(columns[16]);
        accepted_age_s = strcmp(columns[17], "NULL") == 0 ? 0.0 : atof(columns[17]);
        health = parse_health(columns[6]);
        memset(&output, 0, sizeof(output));
        output.accepted = accepted;
        output.initialized = initialized;
        output.relative_mm = relative_mm;
        output.path_distance_mm = path_mm;
        output.speed_mm_s = speed_mm_s;
        output.extreme_relative_mm = extreme_mm;
        output.accepted_age_s = accepted_age_s;
        output.direction = (encoder_direction_t)direction;
        output.health = health;
        output.sample_status = accepted ? ENCODER_SAMPLE_ACCEPTED : ENCODER_SAMPLE_MISSING;
        output.stationary = stationary;
        output.reversal_confirmed = reversal;
        if(!encoder_control_packet_build(&packet, session, sequence, qpc, &output)) continue;
        if(accepted) valid++;
        if(!have_origin && accepted && initialized && health == ENCODER_HEALTH_OK){
            origin = packet;
            have_origin = 1;
        }
        if(!controller_started && have_origin && accepted){
            if(forward_sign == 0 && packet.sequence > origin.sequence &&
               fabs(packet.relative_mm - origin.relative_mm) >= 0.5){
                forward_sign = packet.relative_mm >= origin.relative_mm ? 1 : -1;
            }
            if(forward_sign != 0){
                recip_encoder_config_t config;
                recip_encoder_config_default(&config);
                config.session_id = session;
                config.course_mm = course_mm;
                config.total_mm = total_mm;
                config.stroke_timeout_s = stroke_timeout_s;
                config.initial_direction = forward_sign;
                config.stop_anticipation_enabled = stop_compensation;
                config.stop_model_slope_s = stop_slope_s;
                config.stop_margin_mm = stop_margin_mm;
                config.stop_velocity_window_s = stop_velocity_window_s;
                config.stop_max_course_fraction = stop_max_course_fraction;
                if(!recip_encoder_controller_start(
                       &controller, &config, 10000000LL, &origin, &decision)){
                    fault = RECIP_ENCODER_FAULT_INVALID_START;
                    break;
                }
                controller_started = 1;
            }
        }
        if(controller_started && packet.sequence > controller.last_sequence){
            recip_encoder_action_t action = recip_encoder_controller_update(
                &controller, &packet, sizeof(packet), &decision);
            if((decision.events & RECIP_ENCODER_EVENT_PHYSICAL_REVERSAL) != 0u){
                physical_reversals++;
                if(events){
                    fprintf(events, "PHYSICAL_REVERSAL,%llu,%lld,%llu,%d,%.9f,%.9f,%.9f,%.9f,%.9f,%d,%.9f,%.9f\n",
                            (unsigned long long)packet.sequence,
                            (long long)packet.qpc,
                            (unsigned long long)decision.completed_strokes,
                            decision.command_direction, decision.position_mm,
                            decision.physical_target_mm,
                            decision.trigger_position_mm,
                            decision.stopping_anticipation_mm,
                            decision.velocity_estimate_mm_s,
                            decision.stopping_anticipation_clamped,
                            decision.path_progress_mm,
                            decision.endpoint_error_mm);
                }
            }
            if(action == RECIP_ENCODER_ACTION_REVERSE ||
               action == RECIP_ENCODER_ACTION_COMPLETE){
                const char *event_name = action == RECIP_ENCODER_ACTION_REVERSE
                    ? "REVERSE" : "COMPLETE";
                if(decision.stopping_anticipation_clamped) clamped_events++;
                if(events){
                    fprintf(events, "%s,%llu,%lld,%llu,%d,%.9f,%.9f,%.9f,%.9f,%.9f,%d,%.9f,%.9f\n",
                            event_name,
                            (unsigned long long)packet.sequence,
                            (long long)packet.qpc,
                            (unsigned long long)decision.completed_strokes,
                            decision.command_direction, decision.position_mm,
                            decision.physical_target_mm,
                            decision.trigger_position_mm,
                            decision.stopping_anticipation_mm,
                            decision.velocity_estimate_mm_s,
                            decision.stopping_anticipation_clamped,
                            decision.path_progress_mm,
                            decision.endpoint_error_mm);
                }
            }
            if(action == RECIP_ENCODER_ACTION_REVERSE) reverse_events++;
            else if(action == RECIP_ENCODER_ACTION_COMPLETE){ complete = 1; break; }
            else if(action == RECIP_ENCODER_ACTION_FAULT){ fault = decision.fault; break; }
        }
    }
    fclose(input);
    if(events) fclose(events);
    printf("rows=%llu valid=%llu forward_sign=%d controller_started=%d strokes=%llu reverses=%llu physical_reversals=%llu stop_compensation=%d clamped_events=%llu path_mm=%.6f complete=%d fault=%s\n",
           (unsigned long long)rows, (unsigned long long)valid, forward_sign,
           controller_started,
           (unsigned long long)(controller_started ? controller.completed_strokes : 0u),
           (unsigned long long)reverse_events,
           (unsigned long long)physical_reversals,
           stop_compensation,
           (unsigned long long)clamped_events,
           controller_started ? controller.last_path_mm : 0.0,
           complete, recip_encoder_fault_name((recip_encoder_fault_t)fault));
    return complete && !fault ? 0 : 1;
}
