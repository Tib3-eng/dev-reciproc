#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "encoder_state.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int finite_positive(double value)
{
    return isfinite(value) && value > 0.0;
}

static int unit_is_degrees(const char *unit)
{
    return unit && (
        _stricmp(unit, "deg") == 0 ||
        _stricmp(unit, "degree") == 0 ||
        _stricmp(unit, "degrees") == 0 ||
        _stricmp(unit, "grau") == 0 ||
        _stricmp(unit, "graus") == 0
    );
}

static void set_reason(char *reason, int reason_size, const char *text)
{
    if (!reason || reason_size <= 0) return;
    _snprintf(reason, (size_t)reason_size, "%s", text ? text : "");
    reason[reason_size - 1] = '\0';
}

void encoder_state_config_default(encoder_state_config_t *config)
{
    if (!config) return;
    memset(config, 0, sizeof(*config));
    config->radius_mm = 10.0;
    config->max_physical_speed_mm_s = 100.0;
    config->innovation_factor = 5.0;
    config->innovation_margin_deg = 2.0;
    config->innovation_min_gate_deg = 3.0;
    config->velocity_window_s = 0.10;
    config->movement_deadband_mm = 0.001;
    config->direction_confirm_mm = 0.02;
    config->direction_confirm_samples = 2;
    config->stationary_confirm_s = 0.25;
    config->stale_stop_s = 0.10;
    config->failure_timeout_s = 3.0;
}

int encoder_calibration_validate(
    const encoder_calibration_t *calibration,
    char *reason,
    int reason_size)
{
    if (!calibration) {
        set_reason(reason, reason_size, "calibracao ausente");
        return 0;
    }
    if (calibration->schema_version != 1) {
        set_reason(reason, reason_size, "schema_version deve ser 1");
        return 0;
    }
    if (calibration->channel != 3) {
        set_reason(reason, reason_size, "canal deve ser CH3");
        return 0;
    }
    if (calibration->sensor_type != 1 || calibration->gain_index != 1 || calibration->lpf_index != 0) {
        set_reason(reason, reason_size, "preset deve ser corrente/x3/LPF0");
        return 0;
    }
    if (!calibration->quality_accepted) {
        set_reason(reason, reason_size, "quality.accepted deve ser true");
        return 0;
    }
    if (!calibration->purpose || strcmp(calibration->purpose, "encoder_ch3_angle_deg") != 0) {
        set_reason(reason, reason_size, "purpose incompativel");
        return 0;
    }
    if (!unit_is_degrees(calibration->unit)) {
        set_reason(reason, reason_size, "unidade deve ser graus");
        return 0;
    }
    if (!isfinite(calibration->slope) || fabs(calibration->slope) < 1.0e-15 ||
        !isfinite(calibration->intercept)) {
        set_reason(reason, reason_size, "fit linear invalido");
        return 0;
    }
    if (!isfinite(calibration->modulus_deg) || fabs(calibration->modulus_deg - 360.0) > 1.0e-9) {
        set_reason(reason, reason_size, "modulo deve ser 360 graus");
        return 0;
    }
    set_reason(reason, reason_size, "OK");
    return 1;
}

double encoder_normalize_angle(double angle_deg)
{
    double result = fmod(angle_deg, 360.0);
    if (result < 0.0) result += 360.0;
    return result;
}

double encoder_wrapped_delta(double current_deg, double previous_deg)
{
    double delta = current_deg - previous_deg;
    while (delta > 180.0) delta -= 360.0;
    while (delta < -180.0) delta += 360.0;
    return delta;
}

static double seconds_between(const encoder_state_t *state, int64_t later, int64_t earlier)
{
    return (double)(later - earlier) / (double)state->qpc_frequency;
}

static double mm_per_degree(const encoder_state_t *state)
{
    return (2.0 * M_PI * state->config.radius_mm) / 360.0;
}

static encoder_health_t health_at(const encoder_state_t *state, int64_t qpc, double *age_out)
{
    double age;
    if (!state->has_accepted) {
        age = state->has_input ? seconds_between(state, qpc, state->first_input_qpc) : 0.0;
        if (age_out) *age_out = age;
        if (age >= state->config.failure_timeout_s) return ENCODER_HEALTH_FAILED;
        if (age >= state->config.stale_stop_s) return ENCODER_HEALTH_STALE_STOP;
        return ENCODER_HEALTH_WAITING;
    }
    age = seconds_between(state, qpc, state->last_accepted_qpc);
    if (age < 0.0) age = 0.0;
    if (age_out) *age_out = age;
    if (age >= state->config.failure_timeout_s) return ENCODER_HEALTH_FAILED;
    if (age >= state->config.stale_stop_s) return ENCODER_HEALTH_STALE_STOP;
    if (age > 0.0) return ENCODER_HEALTH_DEGRADED;
    return ENCODER_HEALTH_OK;
}

static void fill_output(const encoder_state_t *state, int64_t qpc, encoder_state_output_t *output)
{
    double age = 0.0;
    double stationary_duration = 0.0;
    if (!output) return;
    memset(output, 0, sizeof(*output));
    output->sample_status = ENCODER_SAMPLE_MISSING;
    output->health = health_at(state, qpc, &age);
    output->accepted_age_s = age;
    output->initialized = state->has_accepted;
    output->direction = state->direction;
    output->path_distance_mm = state->path_distance_mm;
    output->speed_mm_s = state->speed_mm_s;
    output->disk_rpm = state->config.radius_mm > 0.0
        ? state->speed_mm_s * 60.0 / (2.0 * M_PI * state->config.radius_mm)
        : 0.0;
    if (state->has_accepted) {
        output->angle_deg = state->last_angle_deg;
        output->unwrapped_deg = state->last_unwrapped_deg;
        output->relative_deg = state->last_unwrapped_deg - state->origin_unwrapped_deg;
        output->relative_mm = output->relative_deg * mm_per_degree(state);
        output->signed_turns = output->relative_deg / 360.0;
        output->path_turns = state->path_distance_mm / (2.0 * M_PI * state->config.radius_mm);
        stationary_duration = seconds_between(state, qpc, state->last_motion_qpc);
        if (stationary_duration < 0.0) stationary_duration = 0.0;
        output->stationary_duration_s = stationary_duration;
        output->stationary = stationary_duration >= state->config.stationary_confirm_s;
    }
    output->accepted_gap_s = state->quality.max_accepted_gap_s;
}

static void velocity_push(encoder_state_t *state, int64_t qpc, double unwrapped_deg)
{
    int first_keep = 0;
    double window = state->config.velocity_window_s;
    while (first_keep < state->velocity_count &&
           seconds_between(state, qpc, state->velocity_qpc[first_keep]) > window) {
        first_keep++;
    }
    if (first_keep > 0) {
        int remaining = state->velocity_count - first_keep;
        memmove(state->velocity_qpc, state->velocity_qpc + first_keep,
                (size_t)remaining * sizeof(state->velocity_qpc[0]));
        memmove(state->velocity_unwrapped_deg, state->velocity_unwrapped_deg + first_keep,
                (size_t)remaining * sizeof(state->velocity_unwrapped_deg[0]));
        state->velocity_count = remaining;
    }
    if (state->velocity_count == ENCODER_STATE_VELOCITY_CAPACITY) {
        memmove(state->velocity_qpc, state->velocity_qpc + 1,
                (ENCODER_STATE_VELOCITY_CAPACITY - 1u) * sizeof(state->velocity_qpc[0]));
        memmove(state->velocity_unwrapped_deg, state->velocity_unwrapped_deg + 1,
                (ENCODER_STATE_VELOCITY_CAPACITY - 1u) * sizeof(state->velocity_unwrapped_deg[0]));
        state->velocity_count--;
    }
    state->velocity_qpc[state->velocity_count] = qpc;
    state->velocity_unwrapped_deg[state->velocity_count] = unwrapped_deg;
    state->velocity_count++;
}

static double velocity_regression_mm_s(const encoder_state_t *state)
{
    int count = state->velocity_count;
    double sum_t = 0.0, sum_a = 0.0, sum_tt = 0.0, sum_ta = 0.0;
    double denominator;
    if (count < 2) return 0.0;
    for (int i = 0; i < count; ++i) {
        double t = seconds_between(state, state->velocity_qpc[i], state->velocity_qpc[0]);
        double a = state->velocity_unwrapped_deg[i];
        sum_t += t;
        sum_a += a;
        sum_tt += t * t;
        sum_ta += t * a;
    }
    denominator = (double)count * sum_tt - sum_t * sum_t;
    if (fabs(denominator) < 1.0e-18) return 0.0;
    return (((double)count * sum_ta - sum_t * sum_a) / denominator) * mm_per_degree(state);
}

static void reset_candidate(encoder_state_t *state)
{
    state->candidate_direction = ENCODER_DIRECTION_UNKNOWN;
    state->candidate_samples = 0;
    state->candidate_distance_mm = 0.0;
}

static void update_direction(
    encoder_state_t *state,
    int64_t qpc,
    double increment_mm,
    encoder_state_output_t *output)
{
    encoder_direction_t sign;
    double abs_increment = fabs(increment_mm);
    if (abs_increment < state->config.movement_deadband_mm) return;
    sign = increment_mm > 0.0 ? ENCODER_DIRECTION_FORWARD : ENCODER_DIRECTION_REVERSE;

    if (state->direction == ENCODER_DIRECTION_FORWARD &&
        state->last_unwrapped_deg > state->current_extreme_unwrapped_deg) {
        state->current_extreme_unwrapped_deg = state->last_unwrapped_deg;
        state->current_extreme_qpc = qpc;
    } else if (state->direction == ENCODER_DIRECTION_REVERSE &&
               state->last_unwrapped_deg < state->current_extreme_unwrapped_deg) {
        state->current_extreme_unwrapped_deg = state->last_unwrapped_deg;
        state->current_extreme_qpc = qpc;
    }

    if (state->direction != ENCODER_DIRECTION_UNKNOWN && sign == state->direction) {
        reset_candidate(state);
        return;
    }
    if (state->candidate_direction != sign) {
        state->candidate_direction = sign;
        state->candidate_samples = 1;
        state->candidate_distance_mm = abs_increment;
    } else {
        state->candidate_samples++;
        state->candidate_distance_mm += abs_increment;
    }

    if (state->candidate_samples < state->config.direction_confirm_samples ||
        state->candidate_distance_mm < state->config.direction_confirm_mm) {
        return;
    }

    if (state->direction != ENCODER_DIRECTION_UNKNOWN) {
        output->reversal_confirmed = 1;
        output->extreme_relative_mm =
            (state->current_extreme_unwrapped_deg - state->origin_unwrapped_deg) * mm_per_degree(state);
        output->extreme_qpc = state->current_extreme_qpc;
        state->quality.reversals++;
    }
    state->direction = sign;
    state->current_extreme_unwrapped_deg = state->last_unwrapped_deg;
    state->current_extreme_qpc = qpc;
    reset_candidate(state);
}

int encoder_state_init(
    encoder_state_t *state,
    const encoder_state_config_t *config,
    int64_t qpc_frequency,
    const encoder_calibration_t *calibration)
{
    if (!state || !config || qpc_frequency <= 0 ||
        !finite_positive(config->radius_mm) ||
        !finite_positive(config->max_physical_speed_mm_s) ||
        !finite_positive(config->innovation_factor) ||
        !finite_positive(config->innovation_min_gate_deg) ||
        !finite_positive(config->velocity_window_s) ||
        config->direction_confirm_samples < 1 ||
        config->direction_confirm_mm < 0.0 ||
        config->movement_deadband_mm < 0.0 ||
        config->stale_stop_s <= 0.0 ||
        config->failure_timeout_s <= config->stale_stop_s) {
        return 0;
    }
    memset(state, 0, sizeof(*state));
    state->config = *config;
    state->qpc_frequency = qpc_frequency;
    if (calibration) {
        state->calibration = *calibration;
        state->calibration_valid = encoder_calibration_validate(calibration, NULL, 0);
        if (!state->calibration_valid) return 0;
    }
    return 1;
}

void encoder_state_reset(encoder_state_t *state)
{
    encoder_state_config_t config;
    encoder_calibration_t calibration;
    int has_calibration;
    int64_t frequency;
    if (!state) return;
    config = state->config;
    calibration = state->calibration;
    has_calibration = state->calibration_valid;
    frequency = state->qpc_frequency;
    memset(state, 0, sizeof(*state));
    state->config = config;
    state->qpc_frequency = frequency;
    if (has_calibration) {
        state->calibration = calibration;
        state->calibration_valid = 1;
    }
}

static encoder_sample_status_t reject_sample(
    encoder_state_t *state,
    int64_t qpc,
    encoder_sample_status_t status,
    encoder_state_output_t *output)
{
    fill_output(state, qpc, output);
    if (output) {
        output->sample_status = status;
        output->accepted = 0;
        output->health = health_at(state, qpc, &output->accepted_age_s);
    }
    return status;
}

encoder_sample_status_t encoder_state_update_angle(
    encoder_state_t *state,
    int64_t qpc,
    double angle_deg,
    unsigned input_flags,
    encoder_state_output_t *output)
{
    double normalized;
    double delta = 0.0;
    double dt = 0.0;
    double gate;
    double increment_mm = 0.0;
    if (!state || !output) return ENCODER_SAMPLE_MISSING;
    state->quality.input_samples++;
    if (!state->has_input) {
        state->has_input = 1;
        state->first_input_qpc = qpc;
    } else if (qpc <= state->last_input_qpc) {
        state->quality.non_monotonic_samples++;
        return reject_sample(state, qpc, ENCODER_SAMPLE_NON_MONOTONIC_TIME, output);
    }
    state->last_input_qpc = qpc;

    if (!(input_flags & ENCODER_INPUT_HAS_VALUE) || !isfinite(angle_deg)) {
        state->quality.missing_samples++;
        return reject_sample(state, qpc, ENCODER_SAMPLE_MISSING, output);
    }
    if (input_flags & ENCODER_INPUT_SATURATED) {
        state->quality.saturated_samples++;
        return reject_sample(state, qpc, ENCODER_SAMPLE_SATURATED, output);
    }
    if (input_flags & ENCODER_INPUT_QUARANTINE) {
        state->quality.quarantine_samples++;
        return reject_sample(state, qpc, ENCODER_SAMPLE_QUARANTINE, output);
    }

    normalized = encoder_normalize_angle(angle_deg);
    if (state->has_accepted) {
        double max_rate_deg_s = state->config.max_physical_speed_mm_s / mm_per_degree(state);
        dt = seconds_between(state, qpc, state->last_accepted_qpc);
        delta = encoder_wrapped_delta(normalized, state->last_angle_deg);
        gate = max_rate_deg_s * dt * state->config.innovation_factor +
               state->config.innovation_margin_deg;
        if (gate < state->config.innovation_min_gate_deg) gate = state->config.innovation_min_gate_deg;
        if (gate > 179.5) gate = 179.5;
        if (fabs(delta) > gate) {
            state->quality.impossible_jump_samples++;
            return reject_sample(state, qpc, ENCODER_SAMPLE_IMPOSSIBLE_JUMP, output);
        }
        if (dt > state->quality.max_accepted_gap_s) state->quality.max_accepted_gap_s = dt;
        state->last_unwrapped_deg += delta;
        increment_mm = delta * mm_per_degree(state);
        if (fabs(increment_mm) < state->config.movement_deadband_mm) {
            increment_mm = 0.0;
        }
        state->path_distance_mm += fabs(increment_mm);
        state->last_angle_deg = normalized;
        state->last_accepted_qpc = qpc;
        if (increment_mm != 0.0) {
            state->last_motion_qpc = qpc;
        }
    } else {
        state->has_accepted = 1;
        state->last_angle_deg = normalized;
        state->last_unwrapped_deg = normalized;
        state->origin_unwrapped_deg = normalized;
        state->last_accepted_qpc = qpc;
        state->last_motion_qpc = qpc;
        state->current_extreme_unwrapped_deg = normalized;
        state->current_extreme_qpc = qpc;
    }

    state->quality.accepted_samples++;
    velocity_push(state, qpc, state->last_unwrapped_deg);
    state->speed_mm_s = velocity_regression_mm_s(state);
    fill_output(state, qpc, output);
    output->sample_status = ENCODER_SAMPLE_ACCEPTED;
    output->accepted = 1;
    output->health = ENCODER_HEALTH_OK;
    output->accepted_age_s = 0.0;
    output->signed_increment_mm = increment_mm;
    output->absolute_increment_mm = fabs(increment_mm);
    update_direction(state, qpc, increment_mm, output);
    output->direction = state->direction;
    return ENCODER_SAMPLE_ACCEPTED;
}

encoder_sample_status_t encoder_state_update_raw(
    encoder_state_t *state,
    int64_t qpc,
    int raw_value,
    unsigned input_flags,
    encoder_state_output_t *output)
{
    double angle;
    if (!state || !state->calibration_valid) {
        if (state) state->quality.input_samples++;
        if (output) {
            memset(output, 0, sizeof(*output));
            output->sample_status = ENCODER_SAMPLE_INVALID_CALIBRATION;
            output->health = ENCODER_HEALTH_FAILED;
        }
        return ENCODER_SAMPLE_INVALID_CALIBRATION;
    }
    angle = (double)raw_value * state->calibration.slope + state->calibration.intercept;
    return encoder_state_update_angle(state, qpc, angle, input_flags, output);
}

void encoder_state_get_quality(const encoder_state_t *state, encoder_quality_counters_t *quality)
{
    if (!state || !quality) return;
    *quality = state->quality;
}

int encoder_interpolate_crossing_qpc(
    int64_t qpc0,
    double position0,
    int64_t qpc1,
    double position1,
    double boundary,
    int64_t *crossing_qpc)
{
    double fraction;
    double ticks;
    if (!crossing_qpc || qpc1 <= qpc0 || position1 == position0) return 0;
    if ((boundary < position0 && boundary < position1) ||
        (boundary > position0 && boundary > position1)) return 0;
    fraction = (boundary - position0) / (position1 - position0);
    if (fraction < 0.0 || fraction > 1.0) return 0;
    ticks = (double)qpc0 + fraction * (double)(qpc1 - qpc0);
    *crossing_qpc = (int64_t)floor(ticks + 0.5);
    return 1;
}

const char *encoder_sample_status_name(encoder_sample_status_t status)
{
    switch (status) {
        case ENCODER_SAMPLE_ACCEPTED: return "ACCEPTED";
        case ENCODER_SAMPLE_MISSING: return "MISSING";
        case ENCODER_SAMPLE_QUARANTINE: return "QUARANTINE";
        case ENCODER_SAMPLE_SATURATED: return "SATURATED";
        case ENCODER_SAMPLE_NON_MONOTONIC_TIME: return "NON_MONOTONIC_TIME";
        case ENCODER_SAMPLE_IMPOSSIBLE_JUMP: return "IMPOSSIBLE_JUMP";
        case ENCODER_SAMPLE_INVALID_CALIBRATION: return "INVALID_CALIBRATION";
        default: return "UNKNOWN";
    }
}

const char *encoder_health_name(encoder_health_t health)
{
    switch (health) {
        case ENCODER_HEALTH_WAITING: return "WAITING";
        case ENCODER_HEALTH_OK: return "OK";
        case ENCODER_HEALTH_DEGRADED: return "DEGRADED";
        case ENCODER_HEALTH_STALE_STOP: return "STALE_STOP";
        case ENCODER_HEALTH_FAILED: return "FAILED";
        default: return "UNKNOWN";
    }
}
