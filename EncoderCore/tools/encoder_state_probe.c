#define _CRT_SECURE_NO_WARNINGS

#include "encoder_state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define PROBE_QPC_FREQUENCY 1000000LL

static void usage(void)
{
    puts("encoder_state_probe --input samples.csv --radius MM --max-speed MM_S [--target-mm MM] [--allow-quarantine]");
    puts("CSV: t_s,angle_deg,quarantine (angle may be NULL)");
}

int main(int argc, char **argv)
{
    const char *input_path = NULL;
    double radius = 10.0;
    double max_speed = 20.0;
    double target_mm = 0.0;
    int allow_quarantine = 0;
    FILE *input;
    char line[256];
    encoder_state_config_t config;
    encoder_state_t state;
    encoder_state_output_t output;
    int first = 1;
    int accepted = 0;
    int rejected = 0;
    int quarantine = 0;
    int reversals = 0;
    int direction_lock = 0;
    double progress_mm = 0.0;
    double max_progress_mm = 0.0;
    double previous_progress_mm = 0.0;
    int64_t previous_qpc = 0;
    int has_previous_progress = 0;
    int64_t target_qpc = -1;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--input") == 0 && i + 1 < argc) input_path = argv[++i];
        else if (strcmp(argv[i], "--radius") == 0 && i + 1 < argc) radius = atof(argv[++i]);
        else if (strcmp(argv[i], "--max-speed") == 0 && i + 1 < argc) max_speed = atof(argv[++i]);
        else if (strcmp(argv[i], "--target-mm") == 0 && i + 1 < argc) target_mm = atof(argv[++i]);
        else if (strcmp(argv[i], "--allow-quarantine") == 0) allow_quarantine = 1;
        else { usage(); return 2; }
    }
    if (!input_path) { usage(); return 2; }
    input = fopen(input_path, "rb");
    if (!input) { fprintf(stderr, "cannot open %s\n", input_path); return 2; }

    encoder_state_config_default(&config);
    config.radius_mm = radius;
    config.max_physical_speed_mm_s = max_speed;
    if (!encoder_state_init(&state, &config, PROBE_QPC_FREQUENCY, NULL)) {
        fclose(input);
        return 2;
    }

    printf("{\"unwrapped\":[");
    while (fgets(line, sizeof(line), input)) {
        char *time_text;
        char *angle_text;
        char *quarantine_text;
        double t_s;
        double angle = 0.0;
        int has_value;
        int is_quarantine;
        unsigned flags = 0;
        encoder_sample_status_t status;
        if (strncmp(line, "t_s", 3) == 0) continue;
        time_text = strtok(line, ",");
        angle_text = strtok(NULL, ",");
        quarantine_text = strtok(NULL, ",\r\n");
        if (!time_text || !angle_text) continue;
        t_s = atof(time_text);
        has_value = _stricmp(angle_text, "NULL") != 0;
        if (has_value) angle = atof(angle_text);
        is_quarantine = quarantine_text ? atoi(quarantine_text) != 0 : 0;
        if (has_value) flags |= ENCODER_INPUT_HAS_VALUE;
        if (is_quarantine && !allow_quarantine) flags |= ENCODER_INPUT_QUARANTINE;
        if (is_quarantine) quarantine++;
        status = encoder_state_update_angle(
            &state,
            (int64_t)(t_s * (double)PROBE_QPC_FREQUENCY + 0.5),
            angle,
            flags,
            &output
        );
        if (!first) putchar(',');
        first = 0;
        if (status == ENCODER_SAMPLE_ACCEPTED) {
            int64_t current_qpc = (int64_t)(t_s * (double)PROBE_QPC_FREQUENCY + 0.5);
            printf("%.12g", output.unwrapped_deg);
            accepted++;
            if (direction_lock == 0 && fabs(output.relative_mm) >= 1.0) {
                direction_lock = output.relative_mm >= 0.0 ? 1 : -1;
            }
            progress_mm = direction_lock ? direction_lock * output.relative_mm : 0.0;
            if (progress_mm < 0.0) progress_mm = 0.0;
            if (progress_mm > max_progress_mm) max_progress_mm = progress_mm;
            if (target_mm > 0.0 && target_qpc < 0 && has_previous_progress &&
                previous_progress_mm < target_mm && progress_mm >= target_mm) {
                if (!encoder_interpolate_crossing_qpc(
                        previous_qpc, previous_progress_mm,
                        current_qpc, progress_mm, target_mm, &target_qpc)) {
                    target_qpc = current_qpc;
                }
            }
            previous_progress_mm = progress_mm;
            previous_qpc = current_qpc;
            has_previous_progress = 1;
        } else {
            printf("null");
            if (status == ENCODER_SAMPLE_IMPOSSIBLE_JUMP) rejected++;
        }
        if (output.reversal_confirmed) reversals++;
    }
    fclose(input);
    printf(
        "],\"accepted\":%d,\"rejected_glitches\":%d,"
        "\"quarantine\":%d,\"reversals\":%d,\"max_gap_s\":%.12g,"
        "\"direction_lock\":%d,\"progress_mm\":%.12g,\"max_progress_mm\":%.12g,"
        "\"target_crossing_s\":%.12g}\n",
        accepted, rejected, quarantine, reversals, state.quality.max_accepted_gap_s,
        direction_lock, progress_mm, max_progress_mm,
        target_qpc >= 0 ? (double)target_qpc / (double)PROBE_QPC_FREQUENCY : -1.0
    );
    return 0;
}
