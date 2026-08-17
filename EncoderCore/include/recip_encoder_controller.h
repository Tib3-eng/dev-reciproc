#ifndef RECIP_ENCODER_CONTROLLER_H
#define RECIP_ENCODER_CONTROLLER_H

#include <stdint.h>

#include "encoder_control_protocol.h"

#define RECIP_ENCODER_POSITION_FILTER_CAPACITY 31
#define RECIP_ENCODER_VELOCITY_CAPACITY 128

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RECIP_ENCODER_ACTION_NONE = 0,
    RECIP_ENCODER_ACTION_REVERSE,
    RECIP_ENCODER_ACTION_COMPLETE,
    RECIP_ENCODER_ACTION_FAULT
} recip_encoder_action_t;

typedef enum {
    RECIP_ENCODER_FAULT_NONE = 0,
    RECIP_ENCODER_FAULT_INVALID_CONFIG,
    RECIP_ENCODER_FAULT_INVALID_START,
    RECIP_ENCODER_FAULT_PACKET,
    RECIP_ENCODER_FAULT_ENCODER_FAILED,
    RECIP_ENCODER_FAULT_ENCODER_TIMEOUT,
    RECIP_ENCODER_FAULT_STROKE_TIMEOUT,
    RECIP_ENCODER_FAULT_STROKE_LIMIT
} recip_encoder_fault_t;

enum {
    RECIP_ENCODER_EVENT_TARGET_REACHED = 1u << 0,
    RECIP_ENCODER_EVENT_PHYSICAL_REVERSAL = 1u << 1,
    RECIP_ENCODER_EVENT_OUTSIDE_TOLERANCE = 1u << 2
};

typedef struct {
    uint64_t session_id;
    double course_mm;
    double total_mm;
    double tolerance_mm;
    double stroke_limit_factor;
    double target_band_mm;
    double reversal_hysteresis_mm;
    double encoder_timeout_s;
    double stroke_timeout_s;
    int position_filter_samples;
    int initial_direction;
    int stop_anticipation_enabled;
    double stop_model_slope_s;
    double stop_margin_mm;
    double stop_velocity_window_s;
    double stop_max_course_fraction;
} recip_encoder_config_t;

typedef struct {
    recip_encoder_action_t action;
    recip_encoder_fault_t fault;
    unsigned events;
    int command_direction;
    uint64_t completed_strokes;
    double position_mm;
    double raw_position_mm;
    double target_mm;
    double path_progress_mm;
    double endpoint_error_mm;
    double physical_extreme_mm;
    double physical_target_mm;
    double trigger_position_mm;
    double stopping_anticipation_mm;
    double velocity_estimate_mm_s;
    int stopping_anticipation_clamped;
} recip_encoder_decision_t;

typedef struct {
    recip_encoder_config_t config;
    int initialized;
    int complete;
    int faulted;
    int command_direction;
    int awaiting_physical_reversal;
    uint64_t completed_strokes;
    uint64_t last_sequence;
    int64_t qpc_frequency;
    int64_t last_packet_qpc;
    int64_t stroke_started_qpc;
    double home_mm;
    double far_mm;
    double target_mm;
    double stroke_origin_mm;
    double last_path_mm;
    double last_raw_position_mm;
    double last_position_mm;
    double last_endpoint_error_mm;
    double last_physical_extreme_mm;
    double reversal_pending_extreme_mm;
    double last_physical_target_mm;
    double last_trigger_position_mm;
    double last_stopping_anticipation_mm;
    double last_velocity_estimate_mm_s;
    int last_stopping_anticipation_clamped;
    double position_filter[RECIP_ENCODER_POSITION_FILTER_CAPACITY];
    int position_filter_count;
    int position_filter_next;
    int64_t velocity_qpc[RECIP_ENCODER_VELOCITY_CAPACITY];
    double velocity_position_mm[RECIP_ENCODER_VELOCITY_CAPACITY];
    int velocity_count;
    recip_encoder_fault_t fault;
} recip_encoder_controller_t;

void recip_encoder_config_default(recip_encoder_config_t *config);

int recip_encoder_controller_start(
    recip_encoder_controller_t *controller,
    const recip_encoder_config_t *config,
    int64_t qpc_frequency,
    const encoder_control_packet_t *first_packet,
    recip_encoder_decision_t *decision
);

recip_encoder_action_t recip_encoder_controller_update(
    recip_encoder_controller_t *controller,
    const encoder_control_packet_t *packet,
    size_t packet_size,
    recip_encoder_decision_t *decision
);

recip_encoder_action_t recip_encoder_controller_tick(
    recip_encoder_controller_t *controller,
    int64_t now_qpc,
    recip_encoder_decision_t *decision
);

const char *recip_encoder_action_name(recip_encoder_action_t action);
const char *recip_encoder_fault_name(recip_encoder_fault_t fault);

#ifdef __cplusplus
}
#endif

#endif
