#include "encoder_control_protocol.h"

#include <math.h>
#include <string.h>

uint32_t encoder_control_crc32(const void *data, size_t size){
    const unsigned char *bytes = (const unsigned char *)data;
    uint32_t crc = 0xFFFFFFFFu;
    size_t i;
    int bit;
    if(!data && size != 0u) return 0u;
    for(i = 0u; i < size; ++i){
        crc ^= bytes[i];
        for(bit = 0; bit < 8; ++bit){
            uint32_t mask = (uint32_t)-(int32_t)(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

int encoder_control_packet_build(
    encoder_control_packet_t *packet,
    uint64_t session_id,
    uint64_t sequence,
    int64_t qpc,
    const encoder_state_output_t *output
){
    if(!packet || !output || session_id == 0u || sequence == 0u || qpc <= 0){
        return 0;
    }
    memset(packet, 0, sizeof(*packet));
    packet->magic = ENCODER_CONTROL_MAGIC;
    packet->version = ENCODER_CONTROL_VERSION;
    packet->size = (uint16_t)sizeof(*packet);
    packet->session_id = session_id;
    packet->sequence = sequence;
    packet->qpc = qpc;
    packet->relative_mm = output->relative_mm;
    packet->path_distance_mm = output->path_distance_mm;
    packet->speed_mm_s = output->speed_mm_s;
    packet->extreme_relative_mm = output->extreme_relative_mm;
    packet->accepted_age_s = output->accepted_age_s;
    packet->direction = (int32_t)output->direction;
    packet->health = (uint32_t)output->health;
    packet->sample_status = (uint32_t)output->sample_status;
    if(output->accepted) packet->flags |= ENCODER_CONTROL_FLAG_ACCEPTED;
    if(output->initialized) packet->flags |= ENCODER_CONTROL_FLAG_INITIALIZED;
    if(output->stationary) packet->flags |= ENCODER_CONTROL_FLAG_STATIONARY;
    if(output->reversal_confirmed) packet->flags |= ENCODER_CONTROL_FLAG_REVERSAL;
    packet->crc32 = encoder_control_crc32(packet, offsetof(encoder_control_packet_t, crc32));
    return 1;
}

encoder_control_packet_result_t encoder_control_packet_validate(
    const encoder_control_packet_t *packet,
    size_t received_size,
    uint64_t expected_session_id,
    uint64_t last_sequence
){
    uint32_t expected_crc;
    if(!packet) return ENCODER_CONTROL_PACKET_BAD_ARGUMENT;
    if(received_size != sizeof(*packet) || packet->size != sizeof(*packet)){
        return ENCODER_CONTROL_PACKET_BAD_SIZE;
    }
    if(packet->magic != ENCODER_CONTROL_MAGIC) return ENCODER_CONTROL_PACKET_BAD_MAGIC;
    if(packet->version != ENCODER_CONTROL_VERSION) return ENCODER_CONTROL_PACKET_BAD_VERSION;
    if(expected_session_id != 0u && packet->session_id != expected_session_id){
        return ENCODER_CONTROL_PACKET_BAD_SESSION;
    }
    if(packet->sequence <= last_sequence) return ENCODER_CONTROL_PACKET_OLD_SEQUENCE;
    expected_crc = encoder_control_crc32(packet, offsetof(encoder_control_packet_t, crc32));
    if(packet->crc32 != expected_crc) return ENCODER_CONTROL_PACKET_BAD_CRC;
    if(packet->qpc <= 0 || packet->direction < -1 || packet->direction > 1 ||
       packet->health > (uint32_t)ENCODER_HEALTH_FAILED ||
       packet->sample_status > (uint32_t)ENCODER_SAMPLE_INVALID_CALIBRATION ||
       !isfinite(packet->relative_mm) || !isfinite(packet->path_distance_mm) ||
       !isfinite(packet->speed_mm_s) || !isfinite(packet->extreme_relative_mm) ||
       !isfinite(packet->accepted_age_s) || packet->accepted_age_s < 0.0){
        return ENCODER_CONTROL_PACKET_BAD_VALUE;
    }
    return ENCODER_CONTROL_PACKET_OK;
}

const char *encoder_control_packet_result_name(
    encoder_control_packet_result_t result
){
    switch(result){
        case ENCODER_CONTROL_PACKET_OK: return "OK";
        case ENCODER_CONTROL_PACKET_BAD_ARGUMENT: return "BAD_ARGUMENT";
        case ENCODER_CONTROL_PACKET_BAD_SIZE: return "BAD_SIZE";
        case ENCODER_CONTROL_PACKET_BAD_MAGIC: return "BAD_MAGIC";
        case ENCODER_CONTROL_PACKET_BAD_VERSION: return "BAD_VERSION";
        case ENCODER_CONTROL_PACKET_BAD_SESSION: return "BAD_SESSION";
        case ENCODER_CONTROL_PACKET_OLD_SEQUENCE: return "OLD_SEQUENCE";
        case ENCODER_CONTROL_PACKET_BAD_CRC: return "BAD_CRC";
        case ENCODER_CONTROL_PACKET_BAD_VALUE: return "BAD_VALUE";
        default: return "UNKNOWN";
    }
}
