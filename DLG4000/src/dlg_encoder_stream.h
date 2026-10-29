#ifndef DLG_ENCODER_STREAM_H
#define DLG_ENCODER_STREAM_H

#include <winsock2.h>
#include <stdio.h>
#include <stdint.h>
#include "encoder_state.h"

typedef struct {
    SOCKET acquisition_socket;
    SOCKET control_socket;
    struct sockaddr_in control_address;
    FILE *data_file;
    FILE *encoder_file;
    FILE *compat_file;
    FILE *timing_file;
    int compat_decimation;
    int total_rows;
    int use_ipc;
    double rate_hz;
    double force_normal_n;
    int64_t frequency;
    uint64_t session;
    double slope[8], intercept[8];
    int calibrated[8], normalize_degrees[8];
    encoder_state_t *encoder;
    int (*poll_command)(int use_ipc); /* 0 NONE, 1 STOP, 2 PAUSE, 3 RESUME */
    void (*log_event)(const char *format, ...);
} dlg_encoder_stream_config_t;

/* Dedicated acquisition/control producer + bounded archival consumer.
   No disk IO is performed by the producer. CSV pairs retain identical idx/QPC. */
int dlg_encoder_stream_run(const dlg_encoder_stream_config_t *config);

#endif
