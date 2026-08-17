#ifndef ENCODER_STATE_H
#define ENCODER_STATE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ENCODER_STATE_VELOCITY_CAPACITY 256

typedef enum {
    ENCODER_SAMPLE_ACCEPTED = 0,
    ENCODER_SAMPLE_MISSING,
    ENCODER_SAMPLE_QUARANTINE,
    ENCODER_SAMPLE_SATURATED,
    ENCODER_SAMPLE_NON_MONOTONIC_TIME,
    ENCODER_SAMPLE_IMPOSSIBLE_JUMP,
    ENCODER_SAMPLE_INVALID_CALIBRATION
} encoder_sample_status_t;

typedef enum {
    ENCODER_HEALTH_WAITING = 0,
    ENCODER_HEALTH_OK,
    ENCODER_HEALTH_DEGRADED,
    ENCODER_HEALTH_STALE_STOP,
    ENCODER_HEALTH_FAILED
} encoder_health_t;

typedef enum {
    ENCODER_DIRECTION_REVERSE = -1,
    ENCODER_DIRECTION_UNKNOWN = 0,
    ENCODER_DIRECTION_FORWARD = 1
} encoder_direction_t;

enum {
    ENCODER_INPUT_HAS_VALUE = 1u << 0,
    ENCODER_INPUT_QUARANTINE = 1u << 1,
    ENCODER_INPUT_SATURATED = 1u << 2
};

typedef struct {
    int schema_version;
    int channel;
    int sensor_type;
    int gain_index;
    int lpf_index;
    int quality_accepted;
    double slope;
    double intercept;
    double modulus_deg;
    const char *purpose;
    const char *unit;
} encoder_calibration_t;

typedef struct {
    double radius_mm;
    double max_physical_speed_mm_s;
    double innovation_factor;
    double innovation_margin_deg;
    double innovation_min_gate_deg;
    double velocity_window_s;
    double movement_deadband_mm;
    double direction_confirm_mm;
    int direction_confirm_samples;
    double stationary_confirm_s;
    double stale_stop_s;
    double failure_timeout_s;
} encoder_state_config_t;

typedef struct {
    encoder_sample_status_t sample_status;
    encoder_health_t health;
    int accepted;
    int initialized;
    double accepted_age_s;
    double angle_deg;
    double unwrapped_deg;
    double relative_deg;
    double relative_mm;
    double signed_increment_mm;
    double absolute_increment_mm;
    double path_distance_mm;
    double signed_turns;
    double path_turns;
    double speed_mm_s;
    double disk_rpm;
    encoder_direction_t direction;
    int stationary;
    double stationary_duration_s;
    int reversal_confirmed;
    double extreme_relative_mm;
    int64_t extreme_qpc;
    double accepted_gap_s;
} encoder_state_output_t;

typedef struct {
    uint64_t input_samples;
    uint64_t accepted_samples;
    uint64_t missing_samples;
    uint64_t quarantine_samples;
    uint64_t saturated_samples;
    uint64_t non_monotonic_samples;
    uint64_t impossible_jump_samples;
    uint64_t reversals;
    double max_accepted_gap_s;
} encoder_quality_counters_t;

typedef struct {
    encoder_state_config_t config;
    encoder_calibration_t calibration;
    int calibration_valid;
    int64_t qpc_frequency;
    int has_input;
    int has_accepted;
    int64_t first_input_qpc;
    int64_t last_input_qpc;
    int64_t last_accepted_qpc;
    int64_t last_motion_qpc;
    double last_angle_deg;
    double last_unwrapped_deg;
    double origin_unwrapped_deg;
    double path_distance_mm;
    double speed_mm_s;
    encoder_direction_t direction;
    encoder_direction_t candidate_direction;
    int candidate_samples;
    double candidate_distance_mm;
    double current_extreme_unwrapped_deg;
    int64_t current_extreme_qpc;
    int64_t velocity_qpc[ENCODER_STATE_VELOCITY_CAPACITY];
    double velocity_unwrapped_deg[ENCODER_STATE_VELOCITY_CAPACITY];
    int velocity_count;
    encoder_quality_counters_t quality;
} encoder_state_t;

void encoder_state_config_default(encoder_state_config_t *config);

int encoder_calibration_validate(
    const encoder_calibration_t *calibration,
    char *reason,
    int reason_size
);

double encoder_normalize_angle(double angle_deg);
double encoder_wrapped_delta(double current_deg, double previous_deg);

int encoder_state_init(
    encoder_state_t *state,
    const encoder_state_config_t *config,
    int64_t qpc_frequency,
    const encoder_calibration_t *calibration
);

void encoder_state_reset(encoder_state_t *state);

encoder_sample_status_t encoder_state_update_angle(
    encoder_state_t *state,
    int64_t qpc,
    double angle_deg,
    unsigned input_flags,
    encoder_state_output_t *output
);

encoder_sample_status_t encoder_state_update_raw(
    encoder_state_t *state,
    int64_t qpc,
    int raw_value,
    unsigned input_flags,
    encoder_state_output_t *output
);

void encoder_state_get_quality(
    const encoder_state_t *state,
    encoder_quality_counters_t *quality
);

int encoder_interpolate_crossing_qpc(
    int64_t qpc0,
    double position0,
    int64_t qpc1,
    double position1,
    double boundary,
    int64_t *crossing_qpc
);

const char *encoder_sample_status_name(encoder_sample_status_t status);
const char *encoder_health_name(encoder_health_t health);

#ifdef __cplusplus
}
#endif

#endif
