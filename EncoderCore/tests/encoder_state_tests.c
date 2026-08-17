#include "encoder_state.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define QPC_FREQ 1000000LL
#define TICKS_MS(ms) ((int64_t)(ms) * 1000LL)

static int failures = 0;

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        failures++; \
    } \
} while (0)

static int near(double actual, double expected, double tolerance)
{
    return fabs(actual - expected) <= tolerance;
}

static encoder_state_t new_state(double radius, double max_speed)
{
    encoder_state_t state;
    encoder_state_config_t config;
    encoder_state_config_default(&config);
    config.radius_mm = radius;
    config.max_physical_speed_mm_s = max_speed;
    CHECK(encoder_state_init(&state, &config, QPC_FREQ, NULL));
    return state;
}

static void test_calibration(void)
{
    encoder_calibration_t calibration = {
        1, 3, 1, 1, 0, 1,
        0.045, 450.0, 360.0,
        "encoder_ch3_angle_deg", "deg"
    };
    encoder_state_config_t config;
    encoder_state_t state;
    encoder_state_output_t output;
    char reason[128];
    encoder_state_config_default(&config);
    CHECK(encoder_calibration_validate(&calibration, reason, sizeof(reason)));
    CHECK(strcmp(reason, "OK") == 0);
    CHECK(encoder_state_init(&state, &config, QPC_FREQ, &calibration));
    CHECK(encoder_state_update_raw(&state, 0, -10000, ENCODER_INPUT_HAS_VALUE, &output) == ENCODER_SAMPLE_ACCEPTED);
    CHECK(near(output.angle_deg, 0.0, 1.0e-9));
    calibration.gain_index = 2;
    CHECK(!encoder_calibration_validate(&calibration, reason, sizeof(reason)));
}

static void test_wrap_and_relative_zero(void)
{
    encoder_state_t state = new_state(10.0, 100.0);
    encoder_state_output_t output;
    double values[] = {358.0, 359.0, 0.0, 1.0};
    double expected[] = {358.0, 359.0, 360.0, 361.0};
    for (int i = 0; i < 4; ++i) {
        CHECK(encoder_state_update_angle(&state, TICKS_MS(i * 5), values[i], ENCODER_INPUT_HAS_VALUE, &output) == ENCODER_SAMPLE_ACCEPTED);
        CHECK(near(output.unwrapped_deg, expected[i], 1.0e-9));
    }
    CHECK(near(output.relative_deg, 3.0, 1.0e-9));
    CHECK(near(output.relative_mm, 3.0 * 2.0 * 3.14159265358979323846 * 10.0 / 360.0, 1.0e-9));
}

static void test_reverse_wrap(void)
{
    encoder_state_t state = new_state(10.0, 100.0);
    encoder_state_output_t output;
    double values[] = {2.0, 1.0, 0.0, 359.0, 358.0};
    double expected[] = {2.0, 1.0, 0.0, -1.0, -2.0};
    for (int i = 0; i < 5; ++i) {
        encoder_state_update_angle(&state, TICKS_MS(i * 5), values[i], ENCODER_INPUT_HAS_VALUE, &output);
        CHECK(near(output.unwrapped_deg, expected[i], 1.0e-9));
    }
    CHECK(output.direction == ENCODER_DIRECTION_REVERSE);
}

static void test_quarantine_gap_and_health(void)
{
    encoder_state_t state;
    encoder_state_config_t config;
    encoder_state_output_t output;
    encoder_quality_counters_t quality;
    encoder_state_config_default(&config);
    config.stale_stop_s = 0.10;
    config.failure_timeout_s = 3.0;
    CHECK(encoder_state_init(&state, &config, QPC_FREQ, NULL));
    encoder_state_update_angle(&state, 0, 10.0, ENCODER_INPUT_HAS_VALUE, &output);
    CHECK(encoder_state_update_angle(&state, TICKS_MS(50), 200.0,
        ENCODER_INPUT_HAS_VALUE | ENCODER_INPUT_QUARANTINE, &output) == ENCODER_SAMPLE_QUARANTINE);
    CHECK(output.health == ENCODER_HEALTH_DEGRADED);
    CHECK(encoder_state_update_angle(&state, TICKS_MS(100), 0.0, 0, &output) == ENCODER_SAMPLE_MISSING);
    CHECK(output.health == ENCODER_HEALTH_STALE_STOP);
    CHECK(encoder_state_update_angle(&state, TICKS_MS(3000), 0.0, 0, &output) == ENCODER_SAMPLE_MISSING);
    CHECK(output.health == ENCODER_HEALTH_FAILED);
    encoder_state_get_quality(&state, &quality);
    CHECK(quality.quarantine_samples == 1);
    CHECK(quality.missing_samples == 2);
}

static void test_impossible_jump_does_not_move_anchor(void)
{
    encoder_state_t state = new_state(10.0, 20.0);
    encoder_state_output_t output;
    encoder_state_update_angle(&state, TICKS_MS(0), 10.0, ENCODER_INPUT_HAS_VALUE, &output);
    encoder_state_update_angle(&state, TICKS_MS(5), 10.5, ENCODER_INPUT_HAS_VALUE, &output);
    CHECK(encoder_state_update_angle(&state, TICKS_MS(10), 150.0, ENCODER_INPUT_HAS_VALUE, &output) == ENCODER_SAMPLE_IMPOSSIBLE_JUMP);
    CHECK(encoder_state_update_angle(&state, TICKS_MS(15), 11.5, ENCODER_INPUT_HAS_VALUE, &output) == ENCODER_SAMPLE_ACCEPTED);
    CHECK(near(output.unwrapped_deg, 11.5, 1.0e-9));
}

static void test_velocity_regression_at_200_hz(void)
{
    encoder_state_t state = new_state(10.0, 40.0);
    encoder_state_output_t output;
    double speed = 20.0;
    double mm_per_deg = 2.0 * 3.14159265358979323846 * 10.0 / 360.0;
    for (int i = 0; i <= 40; ++i) {
        double t = (double)i / 200.0;
        double angle = (speed * t) / mm_per_deg;
        CHECK(encoder_state_update_angle(&state, (int64_t)(t * QPC_FREQ + 0.5), angle,
            ENCODER_INPUT_HAS_VALUE, &output) == ENCODER_SAMPLE_ACCEPTED);
    }
    CHECK(near(output.speed_mm_s, 20.0, 1.0e-9));
    CHECK(near(output.disk_rpm, speed * 60.0 / (2.0 * 3.14159265358979323846 * 10.0), 1.0e-9));
}

static void test_stationary_jitter_does_not_accumulate_distance(void)
{
    encoder_state_t state = new_state(10.0, 20.0);
    encoder_state_output_t output;
    for (int i = 0; i <= 60; ++i) {
        double jitter = (i % 2) ? 0.002 : 0.0;
        encoder_state_update_angle(
            &state,
            TICKS_MS(i * 5),
            45.0 + jitter,
            ENCODER_INPUT_HAS_VALUE,
            &output
        );
    }
    CHECK(near(output.path_distance_mm, 0.0, 1.0e-12));
    CHECK(output.stationary == 1);
    CHECK(output.stationary_duration_s >= 0.25);
}

static void test_reversal_reports_physical_extreme(void)
{
    encoder_state_t state;
    encoder_state_config_t config;
    encoder_state_output_t output;
    double values[] = {0.0, 1.0, 2.0, 3.0, 4.0, 3.5, 3.0, 2.0};
    int reversal_seen = 0;
    double physical_extreme_mm = 0.0;
    int64_t physical_extreme_qpc = 0;
    encoder_state_config_default(&config);
    config.radius_mm = 10.0;
    config.max_physical_speed_mm_s = 100.0;
    config.direction_confirm_mm = 0.05;
    config.direction_confirm_samples = 2;
    CHECK(encoder_state_init(&state, &config, QPC_FREQ, NULL));
    for (int i = 0; i < 8; ++i) {
        encoder_state_update_angle(&state, TICKS_MS(i * 5), values[i], ENCODER_INPUT_HAS_VALUE, &output);
        if (output.reversal_confirmed) {
            reversal_seen++;
            physical_extreme_mm = output.extreme_relative_mm;
            physical_extreme_qpc = output.extreme_qpc;
        }
    }
    CHECK(reversal_seen == 1);
    CHECK(state.quality.reversals == 1);
    CHECK(state.direction == ENCODER_DIRECTION_REVERSE);
    CHECK(near(physical_extreme_mm,
        4.0 * 2.0 * 3.14159265358979323846 * 10.0 / 360.0, 1.0e-9));
    CHECK(physical_extreme_qpc == TICKS_MS(20));
}

static void test_crossing_interpolation(void)
{
    int64_t crossing = 0;
    CHECK(encoder_interpolate_crossing_qpc(180000, 3.6, 200000, 4.0, 4.0, &crossing));
    CHECK(crossing == 200000);
    CHECK(encoder_interpolate_crossing_qpc(200000, 4.0, 220000, 4.4, 4.2, &crossing));
    CHECK(crossing == 210000);
    CHECK(!encoder_interpolate_crossing_qpc(200000, 4.0, 220000, 4.4, 5.0, &crossing));
}

int main(void)
{
    test_calibration();
    test_wrap_and_relative_zero();
    test_reverse_wrap();
    test_quarantine_gap_and_health();
    test_impossible_jump_does_not_move_anchor();
    test_velocity_regression_at_200_hz();
    test_stationary_jitter_does_not_accumulate_distance();
    test_reversal_reports_physical_extreme();
    test_crossing_interpolation();
    if (failures) {
        fprintf(stderr, "encoder_state_tests: %d failure(s)\n", failures);
        return 1;
    }
    puts("encoder_state_tests: OK");
    return 0;
}
