#include "recip_encoder_controller.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static void clear_decision(
    const recip_encoder_controller_t *controller,
    recip_encoder_decision_t *decision
){
    if(!decision) return;
    memset(decision, 0, sizeof(*decision));
    if(!controller) return;
    decision->fault = controller->fault;
    decision->command_direction = controller->command_direction;
    decision->completed_strokes = controller->completed_strokes;
    decision->position_mm = controller->last_position_mm;
    decision->raw_position_mm = controller->last_raw_position_mm;
    decision->target_mm = controller->target_mm;
    decision->path_progress_mm = controller->last_path_mm;
    decision->endpoint_error_mm = controller->last_endpoint_error_mm;
    decision->physical_extreme_mm = controller->last_physical_extreme_mm;
    decision->physical_target_mm = controller->last_physical_target_mm;
    decision->trigger_position_mm = controller->last_trigger_position_mm;
    decision->stopping_anticipation_mm = controller->last_stopping_anticipation_mm;
    decision->velocity_estimate_mm_s = controller->last_velocity_estimate_mm_s;
    decision->stopping_anticipation_clamped =
        controller->last_stopping_anticipation_clamped;
}

static recip_encoder_action_t fail_controller(
    recip_encoder_controller_t *controller,
    recip_encoder_fault_t fault,
    recip_encoder_decision_t *decision
){
    if(controller){
        controller->faulted = 1;
        controller->fault = fault;
    }
    clear_decision(controller, decision);
    if(decision){
        decision->action = RECIP_ENCODER_ACTION_FAULT;
        decision->fault = fault;
    }
    return RECIP_ENCODER_ACTION_FAULT;
}

static int valid_config(const recip_encoder_config_t *config){
    return config && config->session_id != 0u &&
           isfinite(config->course_mm) && config->course_mm > 0.0 &&
           isfinite(config->total_mm) && config->total_mm > 0.0 &&
           isfinite(config->tolerance_mm) && config->tolerance_mm >= 0.0 &&
           isfinite(config->stroke_limit_factor) && config->stroke_limit_factor >= 2.0 &&
           isfinite(config->target_band_mm) && config->target_band_mm >= 0.0 &&
           config->target_band_mm < config->course_mm &&
           isfinite(config->reversal_hysteresis_mm) && config->reversal_hysteresis_mm > 0.0 &&
           config->reversal_hysteresis_mm < config->course_mm &&
           isfinite(config->encoder_timeout_s) && config->encoder_timeout_s > 0.0 &&
           isfinite(config->stroke_timeout_s) && config->stroke_timeout_s > 0.0 &&
           config->position_filter_samples >= 1 &&
           config->position_filter_samples <= RECIP_ENCODER_POSITION_FILTER_CAPACITY &&
           (config->position_filter_samples % 2) == 1 &&
           (config->stop_anticipation_enabled == 0 ||
            config->stop_anticipation_enabled == 1) &&
           isfinite(config->stop_model_slope_s) &&
           config->stop_model_slope_s >= 0.0 &&
           isfinite(config->stop_margin_mm) && config->stop_margin_mm >= 0.0 &&
           isfinite(config->stop_velocity_window_s) &&
           config->stop_velocity_window_s > 0.0 &&
           isfinite(config->stop_max_course_fraction) &&
           config->stop_max_course_fraction > 0.0 &&
           config->stop_max_course_fraction < 1.0 &&
           (config->initial_direction == 1 || config->initial_direction == -1);
}

static int compare_double(const void *left, const void *right){
    double a = *(const double *)left;
    double b = *(const double *)right;
    return (a > b) - (a < b);
}

static double filter_position(recip_encoder_controller_t *controller, double raw_position){
    double ordered[RECIP_ENCODER_POSITION_FILTER_CAPACITY];
    int count;
    controller->position_filter[controller->position_filter_next] = raw_position;
    controller->position_filter_next =
        (controller->position_filter_next + 1) % controller->config.position_filter_samples;
    if(controller->position_filter_count < controller->config.position_filter_samples){
        controller->position_filter_count++;
    }
    count = controller->position_filter_count;
    memcpy(ordered, controller->position_filter, (size_t)count * sizeof(ordered[0]));
    qsort(ordered, (size_t)count, sizeof(ordered[0]), compare_double);
    return ordered[count / 2];
}

static void velocity_reset(
    recip_encoder_controller_t *controller,
    int64_t qpc,
    double position_mm
){
    controller->velocity_count = 1;
    controller->velocity_qpc[0] = qpc;
    controller->velocity_position_mm[0] = position_mm;
}

static void velocity_push(
    recip_encoder_controller_t *controller,
    int64_t qpc,
    double position_mm
){
    int first_keep = 0;
    while(first_keep < controller->velocity_count &&
          (double)(qpc - controller->velocity_qpc[first_keep]) /
              (double)controller->qpc_frequency >
              controller->config.stop_velocity_window_s){
        first_keep++;
    }
    if(first_keep > 0){
        int remaining = controller->velocity_count - first_keep;
        memmove(
            controller->velocity_qpc,
            controller->velocity_qpc + first_keep,
            (size_t)remaining * sizeof(controller->velocity_qpc[0])
        );
        memmove(
            controller->velocity_position_mm,
            controller->velocity_position_mm + first_keep,
            (size_t)remaining * sizeof(controller->velocity_position_mm[0])
        );
        controller->velocity_count = remaining;
    }
    if(controller->velocity_count == RECIP_ENCODER_VELOCITY_CAPACITY){
        memmove(
            controller->velocity_qpc,
            controller->velocity_qpc + 1,
            (RECIP_ENCODER_VELOCITY_CAPACITY - 1u) *
                sizeof(controller->velocity_qpc[0])
        );
        memmove(
            controller->velocity_position_mm,
            controller->velocity_position_mm + 1,
            (RECIP_ENCODER_VELOCITY_CAPACITY - 1u) *
                sizeof(controller->velocity_position_mm[0])
        );
        controller->velocity_count--;
    }
    controller->velocity_qpc[controller->velocity_count] = qpc;
    controller->velocity_position_mm[controller->velocity_count] = position_mm;
    controller->velocity_count++;
}

static double velocity_regression_mm_s(const recip_encoder_controller_t *controller){
    double sum_t = 0.0;
    double sum_p = 0.0;
    double sum_tt = 0.0;
    double sum_tp = 0.0;
    double denominator;
    int count = controller->velocity_count;
    if(count < 3) return 0.0;
    for(int i = 0; i < count; ++i){
        double t = (double)(controller->velocity_qpc[i] -
                            controller->velocity_qpc[0]) /
                   (double)controller->qpc_frequency;
        double p = controller->velocity_position_mm[i];
        sum_t += t;
        sum_p += p;
        sum_tt += t * t;
        sum_tp += t * p;
    }
    denominator = (double)count * sum_tt - sum_t * sum_t;
    if(fabs(denominator) < 1.0e-18) return 0.0;
    return ((double)count * sum_tp - sum_t * sum_p) / denominator;
}

static double update_stopping_trigger(recip_encoder_controller_t *controller){
    double anticipation = 0.0;
    double maximum = controller->config.course_mm *
                     controller->config.stop_max_course_fraction;
    int clamped = 0;
    controller->last_velocity_estimate_mm_s =
        velocity_regression_mm_s(controller);
    if(controller->config.stop_anticipation_enabled){
        anticipation = controller->config.stop_model_slope_s *
                       fabs(controller->last_velocity_estimate_mm_s) +
                       controller->config.stop_margin_mm;
        if(anticipation > maximum){
            anticipation = maximum;
            clamped = 1;
        }
    }
    controller->last_stopping_anticipation_mm = anticipation;
    controller->last_stopping_anticipation_clamped = clamped;
    controller->last_physical_target_mm = controller->target_mm;
    controller->last_trigger_position_mm = controller->target_mm -
        controller->command_direction * anticipation;
    return controller->last_trigger_position_mm;
}

static double geometric_path_progress(const recip_encoder_controller_t *controller){
    double completed;
    double current;
    if(!controller || !controller->initialized) return 0.0;
    completed = (double)controller->completed_strokes * controller->config.course_mm;
    if(controller->awaiting_physical_reversal || controller->complete) return completed;
    current = controller->command_direction *
              (controller->last_position_mm - controller->stroke_origin_mm);
    if(current < 0.0) current = 0.0;
    if(current > controller->config.course_mm) current = controller->config.course_mm;
    return completed + current;
}

static int packet_ready_to_start(const encoder_control_packet_t *packet){
    return packet &&
           (packet->flags & ENCODER_CONTROL_FLAG_ACCEPTED) != 0u &&
           (packet->flags & ENCODER_CONTROL_FLAG_INITIALIZED) != 0u &&
           packet->health == (uint32_t)ENCODER_HEALTH_OK &&
           packet->accepted_age_s >= 0.0;
}

void recip_encoder_config_default(recip_encoder_config_t *config){
    if(!config) return;
    memset(config, 0, sizeof(*config));
    config->tolerance_mm = 0.5;
    config->stroke_limit_factor = 2.0;
    config->target_band_mm = 0.2;
    config->reversal_hysteresis_mm = 0.25;
    config->encoder_timeout_s = 3.0;
    config->stroke_timeout_s = 30.0;
    config->position_filter_samples = 15;
    config->initial_direction = 1;
    config->stop_anticipation_enabled = 0;
    config->stop_model_slope_s = 0.0;
    config->stop_margin_mm = 0.0;
    config->stop_velocity_window_s = 0.25;
    config->stop_max_course_fraction = 0.45;
}

int recip_encoder_controller_start(
    recip_encoder_controller_t *controller,
    const recip_encoder_config_t *config,
    int64_t qpc_frequency,
    const encoder_control_packet_t *first_packet,
    recip_encoder_decision_t *decision
){
    encoder_control_packet_result_t packet_result;
    if(!controller){
        clear_decision(NULL, decision);
        return 0;
    }
    memset(controller, 0, sizeof(*controller));
    if(!valid_config(config) || qpc_frequency <= 0){
        (void)fail_controller(controller, RECIP_ENCODER_FAULT_INVALID_CONFIG, decision);
        return 0;
    }
    packet_result = encoder_control_packet_validate(
        first_packet, sizeof(*first_packet), config->session_id, 0u);
    if(packet_result != ENCODER_CONTROL_PACKET_OK || !packet_ready_to_start(first_packet)){
        (void)fail_controller(controller, RECIP_ENCODER_FAULT_INVALID_START, decision);
        return 0;
    }
    controller->config = *config;
    controller->initialized = 1;
    controller->command_direction = config->initial_direction;
    controller->last_sequence = first_packet->sequence;
    controller->qpc_frequency = qpc_frequency;
    controller->last_packet_qpc = first_packet->qpc;
    controller->stroke_started_qpc = first_packet->qpc;
    controller->home_mm = first_packet->relative_mm;
    controller->far_mm = controller->home_mm +
                         config->initial_direction * config->course_mm;
    controller->target_mm = controller->far_mm;
    controller->stroke_origin_mm = controller->home_mm;
    controller->last_path_mm = 0.0;
    controller->last_raw_position_mm = first_packet->relative_mm;
    controller->last_position_mm = filter_position(controller, first_packet->relative_mm);
    controller->last_physical_extreme_mm = controller->last_position_mm;
    velocity_reset(controller, first_packet->qpc, first_packet->relative_mm);
    (void)update_stopping_trigger(controller);
    clear_decision(controller, decision);
    return 1;
}

recip_encoder_action_t recip_encoder_controller_tick(
    recip_encoder_controller_t *controller,
    int64_t now_qpc,
    recip_encoder_decision_t *decision
){
    double encoder_age_s;
    double stroke_age_s;
    if(!controller || !controller->initialized){
        return fail_controller(controller, RECIP_ENCODER_FAULT_INVALID_START, decision);
    }
    if(controller->faulted){
        return fail_controller(controller, controller->fault, decision);
    }
    clear_decision(controller, decision);
    if(controller->complete){
        if(decision) decision->action = RECIP_ENCODER_ACTION_COMPLETE;
        return RECIP_ENCODER_ACTION_COMPLETE;
    }
    if(now_qpc < controller->last_packet_qpc || now_qpc < controller->stroke_started_qpc){
        return fail_controller(controller, RECIP_ENCODER_FAULT_PACKET, decision);
    }
    encoder_age_s = (double)(now_qpc - controller->last_packet_qpc) /
                    (double)controller->qpc_frequency;
    if(encoder_age_s > controller->config.encoder_timeout_s){
        return fail_controller(controller, RECIP_ENCODER_FAULT_ENCODER_TIMEOUT, decision);
    }
    stroke_age_s = (double)(now_qpc - controller->stroke_started_qpc) /
                   (double)controller->qpc_frequency;
    if(stroke_age_s > controller->config.stroke_timeout_s){
        return fail_controller(controller, RECIP_ENCODER_FAULT_STROKE_TIMEOUT, decision);
    }
    return RECIP_ENCODER_ACTION_NONE;
}

recip_encoder_action_t recip_encoder_controller_update(
    recip_encoder_controller_t *controller,
    const encoder_control_packet_t *packet,
    size_t packet_size,
    recip_encoder_decision_t *decision
){
    encoder_control_packet_result_t packet_result;
    double position;
    double stroke_travel;
    int reached;
    if(!controller || !controller->initialized){
        return fail_controller(controller, RECIP_ENCODER_FAULT_INVALID_START, decision);
    }
    if(controller->faulted){
        return fail_controller(controller, controller->fault, decision);
    }
    if(controller->complete){
        clear_decision(controller, decision);
        if(decision) decision->action = RECIP_ENCODER_ACTION_COMPLETE;
        return RECIP_ENCODER_ACTION_COMPLETE;
    }
    packet_result = encoder_control_packet_validate(
        packet, packet_size, controller->config.session_id,
        controller->last_sequence);
    if(packet_result != ENCODER_CONTROL_PACKET_OK){
        /* Datagramas antigos, de outra sessao ou corrompidos nao alimentam o
           controle. A ausencia continuada de pacotes validos converge para o
           timeout seguro, sem abortar por uma unica datagrama espurio. */
        clear_decision(controller, decision);
        return RECIP_ENCODER_ACTION_NONE;
    }
    if(packet->qpc <= controller->last_packet_qpc){
        return fail_controller(controller, RECIP_ENCODER_FAULT_PACKET, decision);
    }
    controller->last_sequence = packet->sequence;
    controller->last_packet_qpc = packet->qpc;
    if(packet->health == (uint32_t)ENCODER_HEALTH_FAILED ||
       packet->accepted_age_s > controller->config.encoder_timeout_s){
        return fail_controller(controller, RECIP_ENCODER_FAULT_ENCODER_FAILED, decision);
    }
    if((packet->flags & ENCODER_CONTROL_FLAG_ACCEPTED) == 0u ||
       (packet->flags & ENCODER_CONTROL_FLAG_INITIALIZED) == 0u){
        clear_decision(controller, decision);
        return RECIP_ENCODER_ACTION_NONE;
    }
    controller->last_raw_position_mm = packet->relative_mm;
    velocity_push(controller, packet->qpc, packet->relative_mm);
    position = filter_position(controller, packet->relative_mm);
    controller->last_position_mm = position;
    controller->last_path_mm = geometric_path_progress(controller);
    stroke_travel = fabs(position - controller->stroke_origin_mm);
    if(stroke_travel >= controller->config.stroke_limit_factor *
                        controller->config.course_mm){
        return fail_controller(controller, RECIP_ENCODER_FAULT_STROKE_LIMIT, decision);
    }

    if(controller->awaiting_physical_reversal){
        if(controller->command_direction > 0){
            if(position < controller->reversal_pending_extreme_mm){
                controller->reversal_pending_extreme_mm = position;
            }
        }else if(position > controller->reversal_pending_extreme_mm){
            controller->reversal_pending_extreme_mm = position;
        }
        if(controller->command_direction *
           (position - controller->reversal_pending_extreme_mm) >=
           controller->config.reversal_hysteresis_mm){
            double completed_physical_target_mm =
                controller->last_physical_target_mm;
            double completed_trigger_position_mm =
                controller->last_trigger_position_mm;
            double completed_stopping_anticipation_mm =
                controller->last_stopping_anticipation_mm;
            double completed_velocity_estimate_mm_s =
                controller->last_velocity_estimate_mm_s;
            int completed_stopping_anticipation_clamped =
                controller->last_stopping_anticipation_clamped;
            int completed_direction = -controller->command_direction;
            double physical_endpoint_error_mm;
            controller->awaiting_physical_reversal = 0;
            controller->stroke_origin_mm = controller->reversal_pending_extreme_mm;
            controller->last_physical_extreme_mm = controller->reversal_pending_extreme_mm;
            physical_endpoint_error_mm = completed_direction *
                (controller->last_physical_extreme_mm - completed_physical_target_mm);
            controller->last_endpoint_error_mm = physical_endpoint_error_mm;
            controller->stroke_started_qpc = packet->qpc;
            controller->last_path_mm = geometric_path_progress(controller);
            velocity_reset(controller, packet->qpc, packet->relative_mm);
            (void)update_stopping_trigger(controller);
            clear_decision(controller, decision);
            if(decision){
                decision->events |= RECIP_ENCODER_EVENT_PHYSICAL_REVERSAL;
                decision->physical_extreme_mm = controller->last_physical_extreme_mm;
                /* A confirmacao ocorre depois de o proximo stroke ja ter sido
                   selecionado. Preserve no evento os dados do stroke que
                   efetivamente terminou, nao o alvo seguinte. */
                decision->physical_target_mm = completed_physical_target_mm;
                decision->trigger_position_mm = completed_trigger_position_mm;
                decision->stopping_anticipation_mm =
                    completed_stopping_anticipation_mm;
                decision->velocity_estimate_mm_s =
                    completed_velocity_estimate_mm_s;
                decision->stopping_anticipation_clamped =
                    completed_stopping_anticipation_clamped;
                decision->endpoint_error_mm = physical_endpoint_error_mm;
                if(fabs(physical_endpoint_error_mm) > controller->config.tolerance_mm){
                    decision->events |= RECIP_ENCODER_EVENT_OUTSIDE_TOLERANCE;
                }
            }
        }else{
            clear_decision(controller, decision);
        }
        return RECIP_ENCODER_ACTION_NONE;
    }

    {
        double trigger_position = update_stopping_trigger(controller);
        double band = controller->config.stop_anticipation_enabled
            ? 0.0 : controller->config.target_band_mm;
        reached = controller->command_direction > 0
            ? position >= trigger_position - band
            : position <= trigger_position + band;
    }
    if(!reached){
        clear_decision(controller, decision);
        return RECIP_ENCODER_ACTION_NONE;
    }

    controller->completed_strokes++;
    controller->last_endpoint_error_mm = controller->command_direction *
                                         (position - controller->target_mm);
    controller->last_physical_target_mm = controller->target_mm;
    controller->last_path_mm =
        (double)controller->completed_strokes * controller->config.course_mm;
    clear_decision(controller, decision);
    if(decision){
        decision->events |= RECIP_ENCODER_EVENT_TARGET_REACHED;
        if(!controller->config.stop_anticipation_enabled &&
           fabs(controller->last_endpoint_error_mm) > controller->config.tolerance_mm){
            decision->events |= RECIP_ENCODER_EVENT_OUTSIDE_TOLERANCE;
        }
        decision->endpoint_error_mm = controller->last_endpoint_error_mm;
    }
    if((double)controller->completed_strokes * controller->config.course_mm + 1.0e-9 >=
       controller->config.total_mm){
        controller->complete = 1;
        if(decision) decision->action = RECIP_ENCODER_ACTION_COMPLETE;
        return RECIP_ENCODER_ACTION_COMPLETE;
    }

    controller->command_direction = -controller->command_direction;
    controller->target_mm = controller->command_direction == controller->config.initial_direction
        ? controller->far_mm : controller->home_mm;
    controller->awaiting_physical_reversal = 1;
    controller->reversal_pending_extreme_mm = position;
    if(decision){
        decision->action = RECIP_ENCODER_ACTION_REVERSE;
        decision->command_direction = controller->command_direction;
        decision->target_mm = controller->target_mm;
    }
    return RECIP_ENCODER_ACTION_REVERSE;
}

const char *recip_encoder_action_name(recip_encoder_action_t action){
    switch(action){
        case RECIP_ENCODER_ACTION_NONE: return "NONE";
        case RECIP_ENCODER_ACTION_REVERSE: return "REVERSE";
        case RECIP_ENCODER_ACTION_COMPLETE: return "COMPLETE";
        case RECIP_ENCODER_ACTION_FAULT: return "FAULT";
        default: return "UNKNOWN";
    }
}

const char *recip_encoder_fault_name(recip_encoder_fault_t fault){
    switch(fault){
        case RECIP_ENCODER_FAULT_NONE: return "NONE";
        case RECIP_ENCODER_FAULT_INVALID_CONFIG: return "INVALID_CONFIG";
        case RECIP_ENCODER_FAULT_INVALID_START: return "INVALID_START";
        case RECIP_ENCODER_FAULT_PACKET: return "PACKET";
        case RECIP_ENCODER_FAULT_ENCODER_FAILED: return "ENCODER_FAILED";
        case RECIP_ENCODER_FAULT_ENCODER_TIMEOUT: return "ENCODER_TIMEOUT";
        case RECIP_ENCODER_FAULT_STROKE_TIMEOUT: return "STROKE_TIMEOUT";
        case RECIP_ENCODER_FAULT_STROKE_LIMIT: return "STROKE_LIMIT";
        default: return "UNKNOWN";
    }
}
