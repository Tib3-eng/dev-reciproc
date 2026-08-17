#ifndef ENCODER_CONTROL_PROTOCOL_H
#define ENCODER_CONTROL_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#include "encoder_state.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ENCODER_CONTROL_MAGIC 0x31434E45u /* "ENC1" em little-endian */
#define ENCODER_CONTROL_VERSION 1u

enum {
    ENCODER_CONTROL_FLAG_ACCEPTED = 1u << 0,
    ENCODER_CONTROL_FLAG_INITIALIZED = 1u << 1,
    ENCODER_CONTROL_FLAG_STATIONARY = 1u << 2,
    ENCODER_CONTROL_FLAG_REVERSAL = 1u << 3
};

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint64_t session_id;
    uint64_t sequence;
    int64_t qpc;
    double relative_mm;
    double path_distance_mm;
    double speed_mm_s;
    double extreme_relative_mm;
    double accepted_age_s;
    int32_t direction;
    uint32_t health;
    uint32_t sample_status;
    uint32_t flags;
    uint32_t crc32;
} encoder_control_packet_t;
#pragma pack(pop)

typedef enum {
    ENCODER_CONTROL_PACKET_OK = 0,
    ENCODER_CONTROL_PACKET_BAD_ARGUMENT,
    ENCODER_CONTROL_PACKET_BAD_SIZE,
    ENCODER_CONTROL_PACKET_BAD_MAGIC,
    ENCODER_CONTROL_PACKET_BAD_VERSION,
    ENCODER_CONTROL_PACKET_BAD_SESSION,
    ENCODER_CONTROL_PACKET_OLD_SEQUENCE,
    ENCODER_CONTROL_PACKET_BAD_CRC,
    ENCODER_CONTROL_PACKET_BAD_VALUE
} encoder_control_packet_result_t;

uint32_t encoder_control_crc32(const void *data, size_t size);

int encoder_control_packet_build(
    encoder_control_packet_t *packet,
    uint64_t session_id,
    uint64_t sequence,
    int64_t qpc,
    const encoder_state_output_t *output
);

encoder_control_packet_result_t encoder_control_packet_validate(
    const encoder_control_packet_t *packet,
    size_t received_size,
    uint64_t expected_session_id,
    uint64_t last_sequence
);

const char *encoder_control_packet_result_name(
    encoder_control_packet_result_t result
);

#ifdef __cplusplus
}
#endif

#endif
