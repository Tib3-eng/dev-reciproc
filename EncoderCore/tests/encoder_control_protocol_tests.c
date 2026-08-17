#include "encoder_control_protocol.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(expr) do { \
    if(!(expr)){ \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        failures++; \
    } \
} while(0)

int main(void){
    encoder_state_output_t output;
    encoder_control_packet_t packet;
    encoder_control_packet_t damaged;
    size_t packet_size = sizeof(packet);
    memset(&output, 0, sizeof(output));
    output.accepted = 1;
    output.initialized = 1;
    output.relative_mm = 12.5;
    output.path_distance_mm = 15.0;
    output.speed_mm_s = -2.5;
    output.extreme_relative_mm = 13.0;
    output.accepted_age_s = 0.005;
    output.direction = ENCODER_DIRECTION_REVERSE;
    output.health = ENCODER_HEALTH_OK;
    output.sample_status = ENCODER_SAMPLE_ACCEPTED;
    output.reversal_confirmed = 1;

    CHECK(packet_size == 92u);
    CHECK(encoder_control_packet_build(&packet, 123u, 7u, 456, &output));
    CHECK(encoder_control_packet_validate(&packet, sizeof(packet), 123u, 6u) ==
          ENCODER_CONTROL_PACKET_OK);
    CHECK(packet.flags == (ENCODER_CONTROL_FLAG_ACCEPTED |
                           ENCODER_CONTROL_FLAG_INITIALIZED |
                           ENCODER_CONTROL_FLAG_REVERSAL));
    CHECK(encoder_control_packet_validate(&packet, sizeof(packet), 124u, 6u) ==
          ENCODER_CONTROL_PACKET_BAD_SESSION);
    CHECK(encoder_control_packet_validate(&packet, sizeof(packet), 123u, 7u) ==
          ENCODER_CONTROL_PACKET_OLD_SEQUENCE);
    CHECK(encoder_control_packet_validate(&packet, sizeof(packet) - 1u, 123u, 6u) ==
          ENCODER_CONTROL_PACKET_BAD_SIZE);

    damaged = packet;
    damaged.relative_mm += 1.0;
    CHECK(encoder_control_packet_validate(&damaged, sizeof(damaged), 123u, 6u) ==
          ENCODER_CONTROL_PACKET_BAD_CRC);
    damaged = packet;
    damaged.direction = 2;
    damaged.crc32 = encoder_control_crc32(
        &damaged, offsetof(encoder_control_packet_t, crc32));
    CHECK(encoder_control_packet_validate(&damaged, sizeof(damaged), 123u, 6u) ==
          ENCODER_CONTROL_PACKET_BAD_VALUE);

    if(failures){
        fprintf(stderr, "encoder_control_protocol_tests: %d falha(s)\n", failures);
        return 1;
    }
    puts("encoder_control_protocol_tests: OK");
    return 0;
}
