/*
dlg_encoder_test.c
------------------
Teste de bancada para encoder BRT25-A0M16bit-RT1-X3, com saida 4-20 mA,
no CH3 do DLG4000.

Fluxo:
1) Sem argumentos, abre um menu para monitorar, autocalibrar com o Drive,
   autocalibrar manualmente, calibrar com referencia, verificar e configurar.
2) Cada operacao abre e fecha sua propria conexao UDP.
3) Envia ACQSTOP, configura CH3 como corrente e inicia somente o CH3.
4) Tenta validar o preset por GETCHCFG em fases; se o firmware nao responder,
   usa o fluxo compativel comprovado e ainda exige tres ACQDATA antes do motor.
5) A cada 500 ms, mostra a mediana movel das ultimas 9 amostras.
6) Com JSON angular, converte raw diretamente para graus e normaliza em 0..360.
7) Mantem compatibilidade com calibracao manual raw->mA.
8) Atualiza uma unica linha somente quando o valor muda em 0,01 grau/mA.

Importante:
- ACQDATA entrega int16_t bruto. O manual nao define uma conversao universal para mA.
- A autocalibracao motorizada usa quatro wraps para obter tres voltas completas:
  duas treinam um fit robusto raw->graus e a terceira e um holdout.
- A autocalibracao manual por extremos permanece apenas como normalizacao
  nominal 4-20 mA, nao rastreavel.
- A autocalibracao grava CSV bruto e log textual com as decisoes do detector.
- Use --calibrate para criar out\encoder_CH3_mA.json com dois pontos medidos
  por instrumento de referencia.
- Este programa altera o estado de aquisicao global do DLG. Nao use junto com outro
  logger, CalibraDLG ou o supervisorio.
- Na autocalibracao motorizada, o programa inicia a5_speed_logger por IPC,
  espera o DLG receber dados e somente entao comanda o Drive.
*/
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <conio.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stddef.h>
#include <stdarg.h>
#include <limits.h>
#include <math.h>

#pragma comment(lib, "ws2_32.lib")

#define ENCODER_CHANNEL          3
#define ENCODER_MODEL            "BRT25-A0M16bit-RT1-X3"
#define ENCODER_TEST_BUILD_ID    "2026-07-29-gain3-drivefilter-r4"
#define SENSOR_CURRENT           1
#define ENCODER_GAIN_INDEX       1
#define ENCODER_GAIN_NOMINAL     3
#define DEFAULT_LPF_INDEX        0
#define DEFAULT_SENSPWR_INDEX    1
#define DEFAULT_INPUT_DC_IMPEDANCE 1
#define DEFAULT_INPUT_AC_IMPEDANCE 0

#define DEFAULT_DLG_IP           "192.168.1.100"
#define DEFAULT_DLG_PORT         41401
#define DEFAULT_LOCAL_PORT       41402
#define DEFAULT_SAMPLE_RATE_HZ   200

#define RECV_TIMEOUT_MS          100
#define DRAIN_TIMEOUT_MS         10
#define FIRST_DATA_TIMEOUT_MS    3000
#define FIRST_DATA_PACKETS       3
#define CHANNEL_SET_WAIT_MS      300
#define CHANNEL_GET_WAIT_MS      500
#define CHANNEL_GET_REQUESTS     2
#define CHANNEL_STRICT_ATTEMPTS  2
#define ACQ_SETUP_WAIT_MS        200
#define STREAM_TIMEOUT_MS        1500
#define RETRY_DELAY_MS           500
#define UPDATE_INTERVAL_MS       500
#define MONITOR_FILTER_SAMPLES   9
#define CALIB_CAPTURE_MS         2000
#define AUTO_CALIB_MANUAL_TIMEOUT_MS 120000
#define AUTO_CALIB_MOTOR_TIMEOUT_MS  300000
#define AUTO_CALIB_TRANSITIONS   4
#define AUTO_CALIB_FILTER_SAMPLES 5
#define AUTO_CALIB_PRE_SAMPLES   9
#define AUTO_CALIB_GUARD_SAMPLES 5
#define AUTO_CALIB_POST_SAMPLES  9
#define AUTO_CALIB_HISTORY_SAMPLES \
    (AUTO_CALIB_PRE_SAMPLES + AUTO_CALIB_GUARD_SAMPLES + \
     AUTO_CALIB_POST_SAMPLES)
#define AUTO_CALIB_TRANSITION_ORIGIN_OFFSET \
    (AUTO_CALIB_POST_SAMPLES + AUTO_CALIB_GUARD_SAMPLES / 2)
#define AUTO_CALIB_MIN_JUMP      1000.0
#define AUTO_CALIB_MIN_SPAN      2000.0
#define AUTO_CALIB_WINDOW_THRESHOLD_FRACTION 0.45
#define AUTO_CALIB_WINDOW_MAJORITY 7
#define AUTO_CALIB_WINDOW_MAD_FRACTION 0.25
#define AUTO_CALIB_RANGE_MAD_MULTIPLIER 3.0
#define AUTO_CALIB_RANGE_MIN_BAND 250.0
#define AUTO_CALIB_WINDOW_RETRY_SAMPLES 3
#define AUTO_CALIB_CONFIRM_SAMPLES 9
#define AUTO_CALIB_CONFIRM_MAJORITY 8
#define AUTO_CALIB_SPAN_TOL      0.15
#define AUTO_CALIB_REARM_FRACTION 0.65
#define AUTO_CALIB_MAX_TOLERATED_FRAME_DELTA 2U
#define AUTO_CALIB_DIAGNOSTIC_FLUSH_MS 1000
#define AUTO_CALIB_ENDPOINT_NEAR_MS 100
#define AUTO_CALIB_ENDPOINT_FAR_MS 350
#define AUTO_CALIB_ENDPOINT_TRIM_FRACTION 0.10
#define AUTO_CALIB_ENDPOINT_MAX_SAMPLES 4096
#define ANGULAR_COMPLETE_REVOLUTIONS \
    (AUTO_CALIB_TRANSITIONS - 1)
#define ANGULAR_BIN_COUNT        360
#define ANGULAR_EDGE_EXCLUSION_DEG 5
#define ANGULAR_MIN_BIN_SAMPLES  8
#define ANGULAR_HUBER_ITERATIONS 6
#define ANGULAR_HUBER_K          1.5
#define ANGULAR_MIN_VALID_BINS   320
#define ANGULAR_MAX_RATIO_ERROR_FRACTION 0.01
#define ANGULAR_MAX_RMSE_DEG     0.5
#define ANGULAR_MAX_P95_DEG      1.0
#define ANGULAR_MAX_ERROR_DEG    2.0
#define ANGULAR_MAX_DRIVE_GAP_S  0.5
#define ANGULAR_DRIVE_SPEED_MARGIN 3.0
#define ANGULAR_DRIVE_JITTER_COUNTS 64.0
#define ADC_SATURATION_LOW       (-32760)
#define ADC_SATURATION_HIGH      32760
#define AUTO_CALIB_LOSS_WARNING_FRACTION 0.10
#define AUTO_CALIB_LOSS_REJECT_FRACTION 0.25
#define DLG_RECEIVE_BUFFER_BYTES (1024 * 1024)

#define ENCODER_MIN_CURRENT_MA    4.0
#define ENCODER_MAX_CURRENT_MA    20.0
#define ENCODER_FULL_SCALE_DEG    360.0

#define DEFAULT_CALIB_PATH       "out\\encoder_CH3_mA.json"
#define MAX_PACKET_SAMPLES       720
#define MAX_CALIB_FILE_BYTES     (1024L * 1024L)

#define DEFAULT_DRIVE_PORT       "COM5"
#define DEFAULT_DRIVE_SLAVE      1
#define DEFAULT_DRIVE_BAUD       115200
#define DEFAULT_DRIVE_PARITY     'E'
#define DEFAULT_ENCODER_RPM      1.0
#define DEFAULT_MECHANICAL_RATIO 1.0
#define DRIVE_LOG_RATE_HZ        10.0
#define DRIVE_READY_TIMEOUT_MS   15000
#define DRIVE_START_TIMEOUT_MS   5000
#define DRIVE_STOP_TIMEOUT_MS    15000
#define DRIVE_STATUS_TIMEOUT_MS  3500
#define DRIVE_DURATION_MARGIN_S  30
#define DRIVE_LINE_CAPACITY      512

/* Protocol structs must match the DLG byte layout. */
#pragma pack(push, 1)
typedef struct {
    uint16_t code;
    uint16_t reserved;
} PktHdr;

typedef struct {
    uint16_t code;
    uint16_t reserved;
    uint16_t error;
} PktResponse;

typedef struct {
    uint16_t code;
    uint16_t reserved;
    float sample_freq;
    uint16_t clock_div;
    uint16_t period;
    uint16_t prescaler;
    uint16_t use_adjust;
    int16_t bursts;
    int16_t n_signals;
    int16_t icm[8];
} PktAcqSetup;

typedef struct {
    uint16_t code;
    uint16_t reserved;
    int32_t frame;
    int32_t timestamp;
    float sample_freq;
    int16_t bursts;
    int16_t n_signals;
    int32_t reserved2;
    int32_t reserved3;
    int32_t reserved4;
    int16_t samples[MAX_PACKET_SAMPLES];
} PktData;

typedef struct {
    uint16_t code;
    uint16_t reserved;
    uint16_t error;
    int16_t channel;
    int16_t sensor_type;
    int16_t gain_index;
    int16_t lpf_index;
    int16_t sensor_power_index;
    int16_t balance;
    uint16_t input_dc_impedance;
    uint16_t input_ac;
    uint16_t use_balance;
} PktSetChannel;
#pragma pack(pop)

#define OP_ACQSETUP    0x1000
#define OP_ACQSETUP_R  0x1001
#define OP_ACQSTART    0x1002
#define OP_ACQSTART_R  0x1003
#define OP_ACQSTOP     0x1006
#define OP_ACQSTOP_R   0x1007
#define OP_ACQDATA     0x100B
#define OP_GETCHCFG    0x2000
#define OP_GETCHCFG_R  0x2001
#define OP_SETCHCFG    0x2002
#define OP_SETCHCFG_R  0x2003

typedef struct {
    char dlg_ip[INET_ADDRSTRLEN];
    char local_ip[INET_ADDRSTRLEN];
    char drive_port[32];
    char drive_exe[MAX_PATH];
    char relation_source[MAX_PATH];
    uint16_t dlg_port;
    uint16_t local_port;
    int sample_rate_hz;
    int drive_direction;
    int relation_loaded;
    int mechanical_ratio_explicit;
    double encoder_target_rpm;
    double mechanical_ratio;
    int calibrate;
    int self_test;
    const char *calib_path;
    const char *calib_out_path;
    const char *replay_path;
} AppConfig;

typedef struct {
    double slope_ma_per_count;
    double intercept_ma;
    double slope_deg_per_count;
    double intercept_deg;
    int output_is_degrees;
    int gain_index;
    int lpf_index;
    int sensor_power_index;
    int input_dc_impedance;
    int input_ac_impedance;
    char source_path[MAX_PATH];
} Calibration;

typedef struct {
    double training_slope_deg_per_count;
    double training_intercept_deg;
    double slope_deg_per_count;
    double intercept_deg;
    double training_rmse_deg;
    double operational_rmse_deg;
    double validation_rmse_deg;
    double validation_p95_deg;
    double validation_max_error_deg;
    double validation_filtered_rmse_deg;
    double validation_filtered_p95_deg;
    double validation_filtered_max_error_deg;
    double r_squared;
    double measured_ratio_mean;
    double measured_ratio_max_error_fraction;
    double raw_min;
    double raw_max;
    unsigned int drive_position_modulus;
    int training_bins;
    int validation_bins;
    int validation_filtered_samples;
    int saturation_samples;
    unsigned long drive_rows_total;
    unsigned long drive_missing_rows;
    unsigned long drive_invalid_rows;
    unsigned long drive_outlier_rows;
    unsigned long drive_valid_samples;
    double drive_max_valid_gap_s;
    double ratio_per_revolution[
        ANGULAR_COMPLETE_REVOLUTIONS
    ];
} AngularFit;

typedef struct {
    SOCKET socket_handle;
    struct sockaddr_in dlg_addr;
} DlgConnection;

typedef struct {
    PROCESS_INFORMATION process;
    HANDLE stdin_write;
    HANDLE stdout_read;
    int launched;
    int started;
    int ready_seen;
    int started_seen;
    int stopped_seen;
    int stop_sent;
    int status_seen;
    int status_parse_warning_seen;
    int comm_active;
    int command_rpm;
    unsigned int position_p0b09;
    long long unwrapped_counts;
    double motor_turns;
    int position_errors;
    unsigned long long last_position_age_ms;
    ULONGLONG started_received_ms;
    ULONGLONG status_received_ms;
    char line[DRIVE_LINE_CAPACITY];
    size_t line_length;
    char drive_csv[MAX_PATH];
    char schedule_csv[MAX_PATH];
} DriveSession;

typedef struct {
    int enabled;
    int drive_command_rpm;
    double encoder_target_rpm;
    double mechanical_ratio;
    char drive_port[32];
    char drive_csv[MAX_PATH];
} AutoCalMotion;

typedef struct {
    int saw_set_channel_ack;
    int saw_get_channel_response;
    int get_channel_matches;
    int saw_acq_setup_response;
    int saw_acq_start_response;
    PktSetChannel channel_expected;
    PktSetChannel channel_readback;
    int command_rejected;
    uint16_t rejected_code;
    uint16_t error_code;
    int valid_data_packets;
    int socket_error;
    int sent_set_channel;
    int sent_get_channel;
    int sent_acq_setup;
    int sent_acq_start;
    int fallback_used;
    unsigned int attempt;
    int have_last_data_frame;
    int32_t last_data_frame;
    float observed_sample_freq;
    int data_rate_rejects;
    int data_frame_rejects;
} HandshakeState;

typedef struct {
    FILE *event_log;
    ULONGLONG log_start_ms;
    HandshakeState *result;
} StreamStartDiagnostics;

enum {
    AUTO_EVENT_NONE = 0,
    AUTO_EVENT_TRANSITION = 1,
    AUTO_EVENT_REJECTED = 2,
    AUTO_EVENT_CANDIDATE = 3,
    AUTO_EVENT_REARMED = 4,
    AUTO_EVENT_WINDOW_WAIT = 5,
    AUTO_EVENT_REVERSED = -1
};

enum {
    AUTO_REASON_NONE = 0,
    AUTO_REASON_JUMP_BELOW_LIMIT = 1,
    AUTO_REASON_POST_NOT_STABLE = 2,
    AUTO_REASON_PRE_WINDOW_UNSTABLE = 3,
    AUTO_REASON_POST_WINDOW_UNSTABLE = 4,
    AUTO_REASON_DIRECTION_REVERSED = 5,
    AUTO_REASON_SPAN_TOO_SMALL = 6,
    AUTO_REASON_INCONSISTENT_WITH_PRIOR = 7,
    AUTO_REASON_WINDOW_NOT_PERSISTENT = 8,
    AUTO_REASON_WINDOW_TOO_NOISY = 9,
    AUTO_REASON_CONFIRM_NOT_PERSISTENT = 10,
    AUTO_REASON_CONFIRM_TOO_NOISY = 11
};

typedef struct {
    int event;
    int reason;
    int transitions;
    int direction_sign;
    int post_on_new_side;
    int pre_on_old_side;
    int confirm_on_new_side;
    long long sample_index;
    double delta;
    double threshold;
    double jump;
    double window_jump;
    double filtered_raw;
    double raw_low;
    double raw_high;
    double pre_center;
    double post_center;
    double pre_spread;
    double post_spread;
    double pre_mad;
    double post_mad;
    double confirm_center;
    double confirm_spread;
    double confirm_mad;
    double prior_low;
    double prior_high;
    double prior_span;
    double prior_jump;
    double allowed_deviation;
} AutoCalEvent;

typedef struct {
    int16_t filter_ring[AUTO_CALIB_FILTER_SAMPLES];
    int filter_count;
    int filter_next;

    int have_previous;
    int have_running_range;
    int16_t previous;
    long long sample_index;
    int16_t running_min;
    int16_t running_max;
    double typical_step;

    int16_t history_ring[AUTO_CALIB_HISTORY_SAMPLES];
    int history_count;
    int history_next;

    int candidate_active;
    int candidate_count;
    int16_t candidate_pre[AUTO_CALIB_PRE_SAMPLES];
    int candidate_pre_count;
    int16_t candidate_guard[AUTO_CALIB_GUARD_SAMPLES];
    int16_t candidate_post[AUTO_CALIB_POST_SAMPLES];
    int candidate_post_count;
    int16_t candidate_confirm[AUTO_CALIB_CONFIRM_SAMPLES];
    int candidate_confirm_count;
    long long candidate_index;
    double candidate_threshold;

    int transition_count;
    int rejected_count;
    int window_wait_count;
    int direction_sign;
    int motion_sign;
    int armed;
    double rearm_progress;
    double rearm_required;
    long long last_transition_index;
    long long last_candidate_index;
    int min_transition_interval_samples;
    long long transition_indices[AUTO_CALIB_TRANSITIONS];
    double raw_lows[AUTO_CALIB_TRANSITIONS];
    double raw_highs[AUTO_CALIB_TRANSITIONS];
    double jumps[AUTO_CALIB_TRANSITIONS];
    int endpoint_low_samples[AUTO_CALIB_TRANSITIONS];
    int endpoint_high_samples[AUTO_CALIB_TRANSITIONS];
    int endpoints_refined;
} AutoCalDetector;

static volatile LONG g_stop_requested = 0;
static size_t g_status_width = 0;
static int g_status_active = 0;

static int auto_cal_log_line(
    FILE *log_file,
    ULONGLONG elapsed_ms,
    const char *event_name,
    const char *format,
    ...);

static int load_supervisor_mechanical_ratio(
    double *ratio,
    char *source_path,
    size_t source_path_size);
static int resolve_drive_executable(
    char *path,
    size_t path_size);
static int drive_session_is_running(
    const DriveSession *session);

static void calibration_init_defaults(Calibration *calibration)
{
    if (!calibration) {
        return;
    }
    memset(calibration, 0, sizeof(*calibration));
    calibration->gain_index = ENCODER_GAIN_INDEX;
    calibration->lpf_index = DEFAULT_LPF_INDEX;
    calibration->sensor_power_index =
        DEFAULT_SENSPWR_INDEX;
    calibration->input_dc_impedance =
        DEFAULT_INPUT_DC_IMPEDANCE;
    calibration->input_ac_impedance =
        DEFAULT_INPUT_AC_IMPEDANCE;
}

static BOOL WINAPI console_ctrl_handler(DWORD control_type)
{
    if (control_type == CTRL_C_EVENT ||
        control_type == CTRL_BREAK_EVENT ||
        control_type == CTRL_CLOSE_EVENT) {
        InterlockedExchange(&g_stop_requested, 1);
        return TRUE;
    }
    return FALSE;
}

static int stop_requested(void)
{
    return InterlockedCompareExchange(&g_stop_requested, 0, 0) != 0;
}

static size_t console_status_capacity(void)
{
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO info;

    if (output != INVALID_HANDLE_VALUE &&
        output != NULL &&
        GetConsoleScreenBufferInfo(output, &info)) {
        int columns =
            (int)info.srWindow.Right -
            (int)info.srWindow.Left + 1;
        if (columns > 1) {
            size_t capacity = (size_t)(columns - 1);
            if (capacity >= 255U) {
                return 255U;
            }
            return capacity;
        }
    }
    return 255U;
}

static void status_line(const char *format, ...)
{
    char text[256];
    va_list args;
    int written;
    size_t len;
    size_t capacity;

    va_start(args, format);
    written = vsnprintf(text, sizeof(text), format, args);
    va_end(args);

    if (written < 0 || written >= (int)sizeof(text)) {
        text[sizeof(text) - 1] = '\0';
    }

    len = strlen(text);
    capacity = console_status_capacity();
    if (len > capacity) {
        if (capacity >= 4U) {
            text[capacity - 3U] = '.';
            text[capacity - 2U] = '.';
            text[capacity - 1U] = '.';
        }
        text[capacity] = '\0';
        len = capacity;
    }
    if (g_status_width > capacity) {
        g_status_width = capacity;
    }
    if (len > g_status_width) {
        g_status_width = len;
    }
    printf("\r%-*s", (int)g_status_width, text);
    fflush(stdout);
    g_status_active = 1;
}

static void clear_status_line(void)
{
    if (!g_status_active) {
        return;
    }
    printf("\r%*s\r", (int)g_status_width, "");
    fflush(stdout);
    g_status_width = 0;
    g_status_active = 0;
}

static void finish_status_line(void)
{
    if (g_status_active) {
        printf("\n");
        fflush(stdout);
    }
    g_status_width = 0;
    g_status_active = 0;
}

static int poll_quit_key(void)
{
    while (_kbhit()) {
        int key = _getch();
        if (key == 'q' || key == 'Q') {
            InterlockedExchange(&g_stop_requested, 1);
            return 1;
        }
    }
    return stop_requested();
}

static int parse_u16(const char *text, uint16_t *value)
{
    char *end = NULL;
    long parsed;

    if (!text || !value) {
        return 0;
    }
    parsed = strtol(text, &end, 10);
    if (end == text || *end != '\0' || parsed < 1 || parsed > 65535) {
        return 0;
    }
    *value = (uint16_t)parsed;
    return 1;
}

static int supported_sample_rate(int rate_hz)
{
    static const int rates[] = {
        25, 50, 100, 200, 400, 800, 1600, 3200, 6400, 12800
    };
    size_t i;

    for (i = 0; i < sizeof(rates) / sizeof(rates[0]); ++i) {
        if (rates[i] == rate_hz) {
            return 1;
        }
    }
    return 0;
}

static double auto_cal_abs(double value)
{
    return value < 0.0 ? -value : value;
}

static int compare_int16(const void *left, const void *right)
{
    int a = (int)*(const int16_t *)left;
    int b = (int)*(const int16_t *)right;
    return (a > b) - (a < b);
}

static int compare_double(const void *left, const void *right)
{
    double a = *(const double *)left;
    double b = *(const double *)right;
    return (a > b) - (a < b);
}

static double median_int16(const int16_t *values, int count)
{
    int16_t copy[AUTO_CALIB_PRE_SAMPLES];

    if (!values || count <= 0 ||
        count > AUTO_CALIB_PRE_SAMPLES) {
        return 0.0;
    }
    memcpy(copy, values, (size_t)count * sizeof(copy[0]));
    qsort(copy, (size_t)count, sizeof(copy[0]), compare_int16);
    if ((count & 1) != 0) {
        return (double)copy[count / 2];
    }
    return 0.5 *
           ((double)copy[count / 2 - 1] +
            (double)copy[count / 2]);
}

static double median_double(const double *values, int count)
{
    double copy[AUTO_CALIB_TRANSITIONS];

    if (!values || count <= 0 ||
        count > AUTO_CALIB_TRANSITIONS) {
        return 0.0;
    }
    memcpy(copy, values, (size_t)count * sizeof(copy[0]));
    qsort(copy, (size_t)count, sizeof(copy[0]), compare_double);
    if ((count & 1) != 0) {
        return copy[count / 2];
    }
    return 0.5 * (copy[count / 2 - 1] + copy[count / 2]);
}

static double median_absolute_deviation_int16(
    const int16_t *values,
    int count,
    double center)
{
    double deviations[AUTO_CALIB_PRE_SAMPLES];
    int i;

    if (!values || count <= 0 ||
        count > AUTO_CALIB_PRE_SAMPLES) {
        return 0.0;
    }
    for (i = 0; i < count; ++i) {
        deviations[i] = auto_cal_abs(
            (double)values[i] - center
        );
    }
    qsort(
        deviations,
        (size_t)count,
        sizeof(deviations[0]),
        compare_double
    );
    if ((count & 1) != 0) {
        return deviations[count / 2];
    }
    return 0.5 *
           (deviations[count / 2 - 1] +
            deviations[count / 2]);
}

static int auto_cal_support_around_center(
    const int16_t *values,
    int count,
    double center,
    double mad)
{
    double band = mad * AUTO_CALIB_RANGE_MAD_MULTIPLIER;
    int support = 0;
    int i;

    if (!values || count <= 0) {
        return 0;
    }
    if (band < AUTO_CALIB_RANGE_MIN_BAND) {
        band = AUTO_CALIB_RANGE_MIN_BAND;
    }
    for (i = 0; i < count; ++i) {
        if (auto_cal_abs(
                (double)values[i] - center) <= band) {
            ++support;
        }
    }
    return support;
}

static void auto_cal_update_running_range(
    AutoCalDetector *detector,
    double value)
{
    int rounded;
    int16_t robust_value;

    if (value >= 0.0) {
        rounded = (int)(value + 0.5);
    } else {
        rounded = (int)(value - 0.5);
    }
    if (rounded < INT16_MIN) {
        rounded = INT16_MIN;
    } else if (rounded > INT16_MAX) {
        rounded = INT16_MAX;
    }
    robust_value = (int16_t)rounded;
    if (!detector->have_running_range) {
        detector->running_min = robust_value;
        detector->running_max = robust_value;
        detector->have_running_range = 1;
        return;
    }
    if (robust_value < detector->running_min) {
        detector->running_min = robust_value;
    }
    if (robust_value > detector->running_max) {
        detector->running_max = robust_value;
    }
}

static void int16_min_max(
    const int16_t *values,
    int count,
    int16_t *minimum,
    int16_t *maximum)
{
    int i;

    *minimum = values[0];
    *maximum = values[0];
    for (i = 1; i < count; ++i) {
        if (values[i] < *minimum) {
            *minimum = values[i];
        }
        if (values[i] > *maximum) {
            *maximum = values[i];
        }
    }
}

static void auto_cal_add_history_sample(
    AutoCalDetector *detector,
    int16_t raw)
{
    detector->history_ring[detector->history_next] = raw;
    detector->history_next =
        (detector->history_next + 1) %
        AUTO_CALIB_HISTORY_SAMPLES;
    if (detector->history_count <
        AUTO_CALIB_HISTORY_SAMPLES) {
        ++detector->history_count;
    }
}

static int auto_cal_copy_detection_windows(
    const AutoCalDetector *detector,
    int16_t *pre_window,
    int16_t *guard_window,
    int16_t *post_window)
{
    int start;
    int i;

    if (!detector || !pre_window || !guard_window ||
        !post_window ||
        detector->history_count <
            AUTO_CALIB_HISTORY_SAMPLES) {
        return 0;
    }
    start = detector->history_next;
    for (i = 0; i < AUTO_CALIB_PRE_SAMPLES; ++i) {
        pre_window[i] = detector->history_ring[
            (start + i) % AUTO_CALIB_HISTORY_SAMPLES
        ];
    }
    for (i = 0; i < AUTO_CALIB_GUARD_SAMPLES; ++i) {
        guard_window[i] = detector->history_ring[
            (start + AUTO_CALIB_PRE_SAMPLES + i) %
                AUTO_CALIB_HISTORY_SAMPLES
        ];
    }
    for (i = 0; i < AUTO_CALIB_POST_SAMPLES; ++i) {
        post_window[i] = detector->history_ring[
            (start + AUTO_CALIB_PRE_SAMPLES +
             AUTO_CALIB_GUARD_SAMPLES + i) %
                AUTO_CALIB_HISTORY_SAMPLES
        ];
    }
    return 1;
}

static int16_t auto_cal_filter_sample(
    AutoCalDetector *detector,
    int16_t raw)
{
    double filtered;

    detector->filter_ring[detector->filter_next] = raw;
    detector->filter_next =
        (detector->filter_next + 1) %
        AUTO_CALIB_FILTER_SAMPLES;
    if (detector->filter_count <
        AUTO_CALIB_FILTER_SAMPLES) {
        ++detector->filter_count;
    }
    filtered = median_int16(
        detector->filter_ring,
        detector->filter_count
    );
    if (filtered >= 0.0) {
        filtered += 0.5;
    } else {
        filtered -= 0.5;
    }
    return (int16_t)filtered;
}

static void auto_cal_detector_init(
    AutoCalDetector *detector,
    int sample_rate_hz)
{
    memset(detector, 0, sizeof(*detector));
    detector->armed = 1;
    detector->last_transition_index = -1000000000LL;
    detector->last_candidate_index = -1000000000LL;
    detector->min_transition_interval_samples =
        sample_rate_hz / 4;
    if (detector->min_transition_interval_samples <
        AUTO_CALIB_POST_SAMPLES * 2) {
        detector->min_transition_interval_samples =
            AUTO_CALIB_POST_SAMPLES * 2;
    }
}

static void auto_cal_reset_stream_history(
    AutoCalDetector *detector)
{
    detector->filter_count = 0;
    detector->filter_next = 0;
    detector->have_previous = 0;
    detector->history_count = 0;
    detector->history_next = 0;
    detector->candidate_active = 0;
    detector->candidate_pre_count = 0;
    detector->candidate_post_count = 0;
    detector->candidate_confirm_count = 0;
    if (!detector->armed) {
        detector->rearm_progress = 0.0;
    }
}

static int auto_cal_gap_requires_reset(uint32_t frame_delta)
{
    return frame_delta == 0U ||
           frame_delta > AUTO_CALIB_MAX_TOLERATED_FRAME_DELTA;
}

static double auto_cal_jump_threshold(
    const AutoCalDetector *detector)
{
    double observed_range =
        (double)detector->running_max -
        (double)detector->running_min;
    double threshold =
        observed_range *
        AUTO_CALIB_WINDOW_THRESHOLD_FRACTION;

    if (threshold < AUTO_CALIB_MIN_JUMP) {
        threshold = AUTO_CALIB_MIN_JUMP;
    }
    return threshold;
}

static int auto_cal_evaluate_candidate(
    AutoCalDetector *detector,
    AutoCalEvent *event)
{
    int16_t pre_min;
    int16_t pre_max;
    int16_t post_min;
    int16_t post_max;
    int16_t confirm_min;
    int16_t confirm_max;
    double pre_center;
    double post_center;
    double confirm_center;
    double jump;
    double jump_abs;
    double raw_low;
    double raw_high;
    double span;
    double midpoint;
    int sign;
    int index;
    int pre_on_old_side = 0;
    int post_on_new_side = 0;
    int confirm_on_new_side = 0;
    int i;

    pre_center = median_int16(
        detector->candidate_pre,
        detector->candidate_pre_count
    );
    post_center = median_int16(
        detector->candidate_post,
        detector->candidate_post_count
    );
    int16_min_max(
        detector->candidate_pre,
        detector->candidate_pre_count,
        &pre_min,
        &pre_max
    );
    int16_min_max(
        detector->candidate_post,
        detector->candidate_post_count,
        &post_min,
        &post_max
    );
    confirm_center = median_int16(
        detector->candidate_confirm,
        detector->candidate_confirm_count
    );
    int16_min_max(
        detector->candidate_confirm,
        detector->candidate_confirm_count,
        &confirm_min,
        &confirm_max
    );
    jump = post_center - pre_center;
    jump_abs = auto_cal_abs(jump);
    event->jump = jump;
    event->window_jump = jump;
    event->sample_index = detector->candidate_index;
    event->threshold = detector->candidate_threshold;
    event->pre_center = pre_center;
    event->post_center = post_center;
    event->pre_spread = (double)pre_max - (double)pre_min;
    event->post_spread =
        (double)post_max - (double)post_min;
    event->pre_mad = median_absolute_deviation_int16(
        detector->candidate_pre,
        detector->candidate_pre_count,
        pre_center
    );
    event->post_mad = median_absolute_deviation_int16(
        detector->candidate_post,
        detector->candidate_post_count,
        post_center
    );
    event->confirm_center = confirm_center;
    event->confirm_spread =
        (double)confirm_max - (double)confirm_min;
    event->confirm_mad = median_absolute_deviation_int16(
        detector->candidate_confirm,
        detector->candidate_confirm_count,
        confirm_center
    );

    detector->candidate_active = 0;
    sign = jump >= 0.0 ? 1 : -1;
    midpoint = 0.5 * (pre_center + post_center);
    for (i = 0; i < detector->candidate_pre_count; ++i) {
        if ((sign > 0 &&
             (double)detector->candidate_pre[i] < midpoint) ||
            (sign < 0 &&
             (double)detector->candidate_pre[i] > midpoint)) {
            ++pre_on_old_side;
        }
    }
    for (i = 0; i < detector->candidate_post_count; ++i) {
        if ((sign > 0 &&
             (double)detector->candidate_post[i] > midpoint) ||
            (sign < 0 &&
             (double)detector->candidate_post[i] < midpoint)) {
            ++post_on_new_side;
        }
    }
    for (i = 0; i < detector->candidate_confirm_count; ++i) {
        if ((sign > 0 &&
             (double)detector->candidate_confirm[i] > midpoint) ||
            (sign < 0 &&
             (double)detector->candidate_confirm[i] < midpoint)) {
            ++confirm_on_new_side;
        }
    }
    event->pre_on_old_side = pre_on_old_side;
    event->post_on_new_side = post_on_new_side;
    event->confirm_on_new_side = confirm_on_new_side;
    detector->candidate_confirm_count = 0;
    if (jump_abs < detector->candidate_threshold ||
        jump_abs < AUTO_CALIB_MIN_JUMP) {
        event->reason = AUTO_REASON_JUMP_BELOW_LIMIT;
        ++detector->rejected_count;
        event->event = AUTO_EVENT_REJECTED;
        auto_cal_reset_stream_history(detector);
        return AUTO_EVENT_REJECTED;
    }
    if (pre_on_old_side <
            AUTO_CALIB_WINDOW_MAJORITY ||
        post_on_new_side <
        AUTO_CALIB_WINDOW_MAJORITY) {
        event->reason =
            AUTO_REASON_WINDOW_NOT_PERSISTENT;
        ++detector->rejected_count;
        event->event = AUTO_EVENT_REJECTED;
        auto_cal_reset_stream_history(detector);
        return AUTO_EVENT_REJECTED;
    }
    if (event->pre_mad >
            jump_abs *
                AUTO_CALIB_WINDOW_MAD_FRACTION ||
        event->post_mad >
            jump_abs *
                AUTO_CALIB_WINDOW_MAD_FRACTION) {
        event->reason = AUTO_REASON_WINDOW_TOO_NOISY;
        ++detector->rejected_count;
        event->event = AUTO_EVENT_REJECTED;
        auto_cal_reset_stream_history(detector);
        return AUTO_EVENT_REJECTED;
    }
    if (confirm_on_new_side <
        AUTO_CALIB_CONFIRM_MAJORITY) {
        event->reason =
            AUTO_REASON_CONFIRM_NOT_PERSISTENT;
        ++detector->rejected_count;
        event->event = AUTO_EVENT_REJECTED;
        auto_cal_reset_stream_history(detector);
        return AUTO_EVENT_REJECTED;
    }
    if (event->confirm_mad >
        jump_abs * AUTO_CALIB_WINDOW_MAD_FRACTION) {
        event->reason =
            AUTO_REASON_CONFIRM_TOO_NOISY;
        ++detector->rejected_count;
        event->event = AUTO_EVENT_REJECTED;
        auto_cal_reset_stream_history(detector);
        return AUTO_EVENT_REJECTED;
    }

    if (detector->direction_sign != 0 &&
        detector->direction_sign != sign) {
        event->reason = AUTO_REASON_DIRECTION_REVERSED;
        event->event = AUTO_EVENT_REVERSED;
        event->direction_sign = sign;
        return AUTO_EVENT_REVERSED;
    }

    /*
    Use the persistent plateau medians as endpoints. Selecting the most
    extreme individual samples made the 60 Hz component look like useful
    4-20 mA span and created a large dead angular range after calibration.
    */
    raw_low = pre_center < post_center
        ? pre_center
        : post_center;
    raw_high = pre_center > post_center
        ? pre_center
        : post_center;
    span = raw_high - raw_low;
    event->raw_low = raw_low;
    event->raw_high = raw_high;
    if (span < AUTO_CALIB_MIN_SPAN) {
        event->reason = AUTO_REASON_SPAN_TOO_SMALL;
        ++detector->rejected_count;
        event->event = AUTO_EVENT_REJECTED;
        auto_cal_reset_stream_history(detector);
        return AUTO_EVENT_REJECTED;
    }

    index = detector->transition_count;
    if (index > 0) {
        double prior_low = median_double(
            detector->raw_lows,
            index
        );
        double prior_high = median_double(
            detector->raw_highs,
            index
        );
        double prior_span = prior_high - prior_low;
        double allowed = prior_span * AUTO_CALIB_SPAN_TOL;
        double prior_jump = 0.0;

        for (i = 0; i < index; ++i) {
            prior_jump += auto_cal_abs(detector->jumps[i]);
        }
        prior_jump /= (double)index;
        event->prior_low = prior_low;
        event->prior_high = prior_high;
        event->prior_span = prior_span;
        event->prior_jump = prior_jump;
        event->allowed_deviation = allowed;
        if (prior_span < AUTO_CALIB_MIN_SPAN ||
            auto_cal_abs(raw_low - prior_low) > allowed ||
            auto_cal_abs(raw_high - prior_high) > allowed ||
            auto_cal_abs(span - prior_span) > allowed) {
            event->reason =
                AUTO_REASON_INCONSISTENT_WITH_PRIOR;
            ++detector->rejected_count;
            event->event = AUTO_EVENT_REJECTED;
            auto_cal_reset_stream_history(detector);
            return AUTO_EVENT_REJECTED;
        }
    }

    detector->raw_lows[index] = raw_low;
    detector->raw_highs[index] = raw_high;
    if (pre_center <= post_center) {
        detector->endpoint_low_samples[index] =
            detector->candidate_pre_count;
        detector->endpoint_high_samples[index] =
            detector->candidate_post_count;
    } else {
        detector->endpoint_low_samples[index] =
            detector->candidate_post_count;
        detector->endpoint_high_samples[index] =
            detector->candidate_pre_count;
    }
    auto_cal_update_running_range(detector, raw_low);
    auto_cal_update_running_range(detector, raw_high);
    detector->direction_sign = sign;
    detector->motion_sign = -sign;
    detector->transition_indices[index] =
        detector->candidate_index;
    detector->jumps[index] = jump;
    detector->last_transition_index =
        detector->candidate_index;
    ++detector->transition_count;
    detector->armed = 0;
    detector->rearm_progress = 0.0;
    detector->rearm_required =
        span * AUTO_CALIB_REARM_FRACTION;

    event->event = AUTO_EVENT_TRANSITION;
    event->transitions = detector->transition_count;
    event->direction_sign = sign;
    event->sample_index = detector->candidate_index;
    event->raw_low = raw_low;
    event->raw_high = raw_high;
    return AUTO_EVENT_TRANSITION;
}

static int auto_cal_process_sample(
    AutoCalDetector *detector,
    int16_t raw,
    AutoCalEvent *event)
{
    long long index = detector->sample_index++;
    int16_t pre_window[AUTO_CALIB_PRE_SAMPLES];
    int16_t guard_window[AUTO_CALIB_GUARD_SAMPLES];
    int16_t post_window[AUTO_CALIB_POST_SAMPLES];
    int16_t filtered =
        auto_cal_filter_sample(detector, raw);
    double delta = 0.0;
    double threshold = AUTO_CALIB_MIN_JUMP;
    double pre_center = 0.0;
    double post_center = 0.0;
    double window_jump = 0.0;
    int have_windows = 0;
    int result = AUTO_EVENT_NONE;

    memset(event, 0, sizeof(*event));
    event->sample_index = index;
    event->transitions = detector->transition_count;
    event->direction_sign = detector->direction_sign;
    event->filtered_raw = (double)filtered;

    if (!detector->have_previous) {
        detector->have_previous = 1;
        detector->previous = filtered;
        event->threshold = threshold;
        auto_cal_add_history_sample(detector, filtered);
        return AUTO_EVENT_NONE;
    }

    delta =
        (double)filtered - (double)detector->previous;
    event->delta = delta;
    auto_cal_add_history_sample(detector, filtered);
    threshold = auto_cal_jump_threshold(detector);
    have_windows = auto_cal_copy_detection_windows(
        detector,
        pre_window,
        guard_window,
        post_window
    );
    if (have_windows) {
        int16_t pre_min;
        int16_t pre_max;
        int16_t post_min;
        int16_t post_max;
        double midpoint;
        int sign;
        int post_center_support;
        int i;

        pre_center = median_int16(
            pre_window,
            AUTO_CALIB_PRE_SAMPLES
        );
        post_center = median_int16(
            post_window,
            AUTO_CALIB_POST_SAMPLES
        );
        window_jump = post_center - pre_center;
        event->pre_center = pre_center;
        event->post_center = post_center;
        event->window_jump = window_jump;
        event->pre_mad =
            median_absolute_deviation_int16(
                pre_window,
                AUTO_CALIB_PRE_SAMPLES,
                pre_center
            );
        event->post_mad =
            median_absolute_deviation_int16(
                post_window,
                AUTO_CALIB_POST_SAMPLES,
                post_center
            );
        int16_min_max(
            pre_window,
            AUTO_CALIB_PRE_SAMPLES,
            &pre_min,
            &pre_max
        );
        int16_min_max(
            post_window,
            AUTO_CALIB_POST_SAMPLES,
            &post_min,
            &post_max
        );
        event->pre_spread =
            (double)pre_max - (double)pre_min;
        event->post_spread =
            (double)post_max - (double)post_min;
        midpoint = 0.5 * (pre_center + post_center);
        sign = window_jump >= 0.0 ? 1 : -1;
        for (i = 0; i < AUTO_CALIB_PRE_SAMPLES; ++i) {
            if ((sign > 0 &&
                 (double)pre_window[i] < midpoint) ||
                (sign < 0 &&
                 (double)pre_window[i] > midpoint)) {
                ++event->pre_on_old_side;
            }
        }
        for (i = 0; i < AUTO_CALIB_POST_SAMPLES; ++i) {
            if ((sign > 0 &&
                 (double)post_window[i] > midpoint) ||
                (sign < 0 &&
                 (double)post_window[i] < midpoint)) {
                ++event->post_on_new_side;
            }
        }
        post_center_support = auto_cal_support_around_center(
            post_window,
            AUTO_CALIB_POST_SAMPLES,
            post_center,
            event->post_mad
        );
        if (!detector->candidate_active &&
            post_center_support >=
                AUTO_CALIB_WINDOW_MAJORITY &&
            auto_cal_abs(window_jump) < threshold) {
            auto_cal_update_running_range(
                detector,
                post_center
            );
        }
    }
    threshold = auto_cal_jump_threshold(detector);
    event->threshold = threshold;

    if (detector->candidate_active) {
        if (detector->candidate_confirm_count <
            AUTO_CALIB_CONFIRM_SAMPLES) {
            detector->candidate_confirm[
                detector->candidate_confirm_count++
            ] = filtered;
        }
        if (detector->candidate_confirm_count >=
            AUTO_CALIB_CONFIRM_SAMPLES) {
            result = auto_cal_evaluate_candidate(
                detector,
                event
            );
        }
    } else {
        int normal_step =
            auto_cal_abs(delta) < threshold;

        if (!detector->armed &&
            detector->transition_count <
                AUTO_CALIB_TRANSITIONS &&
            normal_step) {
            detector->rearm_progress +=
                (double)detector->motion_sign * delta;
            if (detector->rearm_progress < 0.0) {
                detector->rearm_progress = 0.0;
            }
            if (index - detector->last_transition_index >=
                    detector->min_transition_interval_samples &&
                detector->rearm_progress >=
                    detector->rearm_required) {
                detector->armed = 1;
                event->event = AUTO_EVENT_REARMED;
                result = AUTO_EVENT_REARMED;
            }
        }

        if (normal_step) {
            if (detector->typical_step <= 0.0) {
                detector->typical_step =
                    auto_cal_abs(delta);
            } else {
                detector->typical_step =
                    detector->typical_step * 0.95 +
                    auto_cal_abs(delta) * 0.05;
            }
        }
        if (result == AUTO_EVENT_NONE &&
            detector->armed &&
            detector->transition_count <
                AUTO_CALIB_TRANSITIONS &&
            have_windows &&
            auto_cal_abs(window_jump) >= threshold &&
            index - detector->last_transition_index >=
                detector->min_transition_interval_samples &&
            index - detector->last_candidate_index >=
                AUTO_CALIB_WINDOW_RETRY_SAMPLES) {
            detector->last_candidate_index = index;
            event->jump = window_jump;
            event->window_jump = window_jump;
            if (event->pre_on_old_side <
                    AUTO_CALIB_WINDOW_MAJORITY ||
                event->post_on_new_side <
                    AUTO_CALIB_WINDOW_MAJORITY) {
                event->reason =
                    AUTO_REASON_WINDOW_NOT_PERSISTENT;
                event->event = AUTO_EVENT_WINDOW_WAIT;
                ++detector->window_wait_count;
                result = AUTO_EVENT_WINDOW_WAIT;
            } else if (
                event->pre_mad >
                    auto_cal_abs(window_jump) *
                        AUTO_CALIB_WINDOW_MAD_FRACTION ||
                event->post_mad >
                    auto_cal_abs(window_jump) *
                        AUTO_CALIB_WINDOW_MAD_FRACTION) {
                event->reason =
                    AUTO_REASON_WINDOW_TOO_NOISY;
                event->event = AUTO_EVENT_WINDOW_WAIT;
                ++detector->window_wait_count;
                result = AUTO_EVENT_WINDOW_WAIT;
            } else {
                ++detector->candidate_count;
                memcpy(
                    detector->candidate_pre,
                    pre_window,
                    sizeof(detector->candidate_pre)
                );
                memcpy(
                    detector->candidate_guard,
                    guard_window,
                    sizeof(detector->candidate_guard)
                );
                memcpy(
                    detector->candidate_post,
                    post_window,
                    sizeof(detector->candidate_post)
                );
                detector->candidate_pre_count =
                    AUTO_CALIB_PRE_SAMPLES;
                detector->candidate_post_count =
                    AUTO_CALIB_POST_SAMPLES;
                detector->candidate_confirm_count = 0;
                /*
                 * Anchor the transition at the center of the guard window,
                 * not at the end of the later post window. This keeps the
                 * time-based endpoint windows symmetric around the wrap.
                 */
                detector->candidate_index =
                    index -
                    AUTO_CALIB_TRANSITION_ORIGIN_OFFSET;
                detector->candidate_threshold = threshold;
                detector->candidate_active = 1;
                event->sample_index =
                    detector->candidate_index;
                event->event = AUTO_EVENT_CANDIDATE;
                result = AUTO_EVENT_CANDIDATE;
            }
        }
    }

    detector->previous = filtered;
    event->transitions = detector->transition_count;
    event->direction_sign = detector->direction_sign;
    return result;
}

static int auto_cal_finalize(
    const AutoCalDetector *detector,
    double *raw_low,
    double *raw_high,
    double *maximum_relative_deviation)
{
    double spans[AUTO_CALIB_TRANSITIONS];
    double span_median;
    double max_deviation = 0.0;
    int i;

    if (detector->transition_count !=
        AUTO_CALIB_TRANSITIONS) {
        return 0;
    }
    *raw_low = median_double(
        detector->raw_lows,
        AUTO_CALIB_TRANSITIONS
    );
    *raw_high = median_double(
        detector->raw_highs,
        AUTO_CALIB_TRANSITIONS
    );
    if (*raw_high - *raw_low < AUTO_CALIB_MIN_SPAN) {
        return 0;
    }

    for (i = 0; i < AUTO_CALIB_TRANSITIONS; ++i) {
        spans[i] =
            detector->raw_highs[i] -
            detector->raw_lows[i];
    }
    span_median = median_double(
        spans,
        AUTO_CALIB_TRANSITIONS
    );
    if (span_median < AUTO_CALIB_MIN_SPAN) {
        return 0;
    }
    for (i = 0; i < AUTO_CALIB_TRANSITIONS; ++i) {
        double span_deviation =
            auto_cal_abs(spans[i] - span_median) /
            span_median;
        double low_deviation =
            auto_cal_abs(
                detector->raw_lows[i] - *raw_low
            ) / span_median;
        double high_deviation =
            auto_cal_abs(
                detector->raw_highs[i] - *raw_high
            ) / span_median;

        if (span_deviation > max_deviation) {
            max_deviation = span_deviation;
        }
        if (low_deviation > max_deviation) {
            max_deviation = low_deviation;
        }
        if (high_deviation > max_deviation) {
            max_deviation = high_deviation;
        }
    }
    *maximum_relative_deviation = max_deviation;
    return max_deviation <= AUTO_CALIB_SPAN_TOL;
}

static int test_auto_cal_detector(int reverse_direction)
{
    AutoCalDetector detector;
    AutoCalEvent event;
    double raw_low;
    double raw_high;
    double deviation;
    int finalized;
    int i;

    auto_cal_detector_init(&detector, 200);
    for (i = 0; i < 850; ++i) {
        int phase = (i + 50) % 200;
        int raw = reverse_direction
            ? 15000 - phase * 60
            : 3000 + phase * 60;
        int result;

        if (i == 80) {
            raw = reverse_direction ? -20000 : 30000;
        }
        result = auto_cal_process_sample(
            &detector,
            (int16_t)raw,
            &event
        );
        if (result == AUTO_EVENT_REVERSED) {
            return 0;
        }
    }
    finalized = auto_cal_finalize(
        &detector,
        &raw_low,
        &raw_high,
        &deviation
    );
    if (detector.transition_count !=
            AUTO_CALIB_TRANSITIONS ||
        detector.candidate_count !=
            AUTO_CALIB_TRANSITIONS ||
        detector.rejected_count != 0 ||
        detector.direction_sign !=
            (reverse_direction ? 1 : -1) ||
        !finalized ||
        raw_low < 2900.0 || raw_low > 3900.0 ||
        raw_high < 13900.0 || raw_high > 15100.0 ||
        deviation > 0.02) {
        printf(
            "AUTO detector detalhe: reverse=%d transitions=%d "
            "candidates=%d rejected=%d armed=%d direction=%d "
            "finalized=%d low=%.1f high=%.1f deviation=%.4f\n",
            reverse_direction,
            detector.transition_count,
            detector.candidate_count,
            detector.rejected_count,
            detector.armed,
            detector.direction_sign,
            finalized,
            finalized ? raw_low : 0.0,
            finalized ? raw_high : 0.0,
            finalized ? deviation : 0.0
        );
        return 0;
    }
    return 1;
}

static int test_auto_cal_rejects_pulse(void)
{
    AutoCalDetector detector;
    AutoCalEvent event;
    double raw_low;
    double raw_high;
    double deviation;
    int i;

    auto_cal_detector_init(&detector, 200);
    for (i = 0; i < 80; ++i) {
        int16_t raw =
            (i >= 30 && i < 42)
                ? (int16_t)20000
                : (int16_t)5000;
        int result = auto_cal_process_sample(
            &detector,
            raw,
            &event
        );

        if (result == AUTO_EVENT_TRANSITION ||
            result == AUTO_EVENT_REVERSED) {
            printf(
                "AUTO pulso detalhe: evento=%d idx=%lld "
                "reason=%d candidates=%d rejected=%d waits=%d "
                "range=[%d,%d]\n",
                result,
                event.sample_index,
                event.reason,
                detector.candidate_count,
                detector.rejected_count,
                detector.window_wait_count,
                (int)detector.running_min,
                (int)detector.running_max
            );
            return 0;
        }
    }
    if (detector.transition_count != 0 ||
        detector.candidate_active ||
        detector.candidate_count == 0 ||
        detector.rejected_count == 0 ||
        !detector.have_running_range ||
        (int)detector.running_max -
            (int)detector.running_min >
                (int)AUTO_CALIB_RANGE_MIN_BAND) {
        printf(
            "AUTO pulso detalhe: transitions=%d active=%d "
            "candidates=%d rejected=%d waits=%d have_range=%d "
            "range=[%d,%d]\n",
            detector.transition_count,
            detector.candidate_active,
            detector.candidate_count,
            detector.rejected_count,
            detector.window_wait_count,
            detector.have_running_range,
            (int)detector.running_min,
            (int)detector.running_max
        );
        return 0;
    }

    for (i = 0; i < 850; ++i) {
        int phase = (i + 50) % 200;
        int16_t raw = (int16_t)(3000 + phase * 60);
        int result = auto_cal_process_sample(
            &detector,
            raw,
            &event
        );

        if (result == AUTO_EVENT_REVERSED) {
            return 0;
        }
    }
    return detector.transition_count ==
               AUTO_CALIB_TRANSITIONS &&
           auto_cal_finalize(
               &detector,
               &raw_low,
               &raw_high,
               &deviation
           ) &&
           raw_low >= 2900.0 && raw_low <= 3900.0 &&
           raw_high >= 13900.0 && raw_high <= 15100.0;
}

static int test_auto_cal_noisy_persistent_trace(void)
{
    static const int noise[] = {
        0, 420, -360, 610, -520, 250,
        -180, 540, -430, 130, -290, 360
    };
    AutoCalDetector detector;
    AutoCalEvent event;
    double raw_low;
    double raw_high;
    double deviation;
    int i;

    auto_cal_detector_init(&detector, 200);
    for (i = 0; i < 650; ++i) {
        int phase = (i + 50) % 140;
        int raw =
            -500 - phase * 25 +
            noise[i % (int)(sizeof(noise) / sizeof(noise[0]))];
        int result;

        if (i == 170 || i == 171) {
            raw += 5000;
        }
        result = auto_cal_process_sample(
            &detector,
            (int16_t)raw,
            &event
        );
        if (result == AUTO_EVENT_REVERSED) {
            return 0;
        }
    }
    return detector.transition_count ==
               AUTO_CALIB_TRANSITIONS &&
           detector.direction_sign == 1 &&
           auto_cal_finalize(
               &detector,
               &raw_low,
               &raw_high,
               &deviation) &&
           raw_high - raw_low >= AUTO_CALIB_MIN_SPAN &&
           deviation <= AUTO_CALIB_SPAN_TOL;
}

static int test_auto_cal_ignores_gradual_ramp(void)
{
    AutoCalDetector detector;
    AutoCalEvent event;
    int i;

    auto_cal_detector_init(&detector, 200);
    for (i = 0; i < 600; ++i) {
        int result = auto_cal_process_sample(
            &detector,
            (int16_t)(2000 + i * 10),
            &event
        );

        if (result == AUTO_EVENT_TRANSITION ||
            result == AUTO_EVENT_REVERSED) {
            return 0;
        }
    }
    return detector.transition_count == 0 &&
           detector.candidate_count == 0;
}

static int test_auto_cal_detects_reversal(void)
{
    AutoCalDetector detector;
    AutoCalEvent event;
    int saw_reversal = 0;
    int i;

    auto_cal_detector_init(&detector, 200);
    for (i = 0; i < 320; ++i) {
        int phase = (i + 50) % 200;
        int16_t raw =
            (int16_t)(3000 + phase * 60);
        int result = auto_cal_process_sample(
            &detector,
            raw,
            &event
        );

        if (result == AUTO_EVENT_REVERSED) {
            return 0;
        }
    }
    if (detector.transition_count != 1 ||
        !detector.armed ||
        detector.direction_sign != -1) {
        return 0;
    }
    for (i = 0; i <= 170; ++i) {
        int raw = 13140 - i * 60;
        if (raw < 3000) {
            raw = 3000;
        }
        (void)auto_cal_process_sample(
            &detector,
            (int16_t)raw,
            &event
        );
    }
    for (i = 0; i < 20; ++i) {
        int result = auto_cal_process_sample(
            &detector,
            (int16_t)(15000 - i * 60),
            &event
        );

        if (result == AUTO_EVENT_REVERSED) {
            saw_reversal = 1;
            break;
        }
    }
    return saw_reversal &&
           detector.transition_count == 1;
}

static int test_auto_cal_rearm_ignores_pulse(void)
{
    AutoCalDetector detector;
    AutoCalEvent event;
    int i;

    auto_cal_detector_init(&detector, 200);
    for (i = 0; i <= 220; ++i) {
        int phase = (i + 50) % 200;
        int16_t raw =
            (int16_t)(3000 + phase * 60);
        (void)auto_cal_process_sample(
            &detector,
            raw,
            &event
        );
    }
    if (detector.transition_count != 1 ||
        detector.armed) {
        return 0;
    }
    for (i = 0; i < 3; ++i) {
        (void)auto_cal_process_sample(
            &detector,
            (int16_t)15000,
            &event
        );
    }
    for (i = 0; i < 8; ++i) {
        (void)auto_cal_process_sample(
            &detector,
            (int16_t)(7260 + i * 60),
            &event
        );
    }
    return detector.transition_count == 1 &&
           !detector.armed;
}

static int test_auto_cal_gap_resets_history(void)
{
    AutoCalDetector detector;
    AutoCalEvent event;
    int i;

    auto_cal_detector_init(&detector, 200);
    for (i = 0; i < 30; ++i) {
        (void)auto_cal_process_sample(
            &detector,
            (int16_t)(14500 + i * 10),
            &event
        );
    }
    auto_cal_reset_stream_history(&detector);
    for (i = 0; i < 30; ++i) {
        int result = auto_cal_process_sample(
            &detector,
            (int16_t)(3000 + i * 10),
            &event
        );

        if (result != AUTO_EVENT_NONE) {
            return 0;
        }
    }
    return detector.transition_count == 0 &&
           !detector.candidate_active;
}

static int test_auto_cal_tolerates_isolated_gaps(void)
{
    AutoCalDetector detector;
    AutoCalEvent event;
    double raw_low = 0.0;
    double raw_high = 0.0;
    double deviation = 0.0;
    int i;

    if (auto_cal_gap_requires_reset(2U) ||
        !auto_cal_gap_requires_reset(3U) ||
        !auto_cal_gap_requires_reset(0U)) {
        return 0;
    }
    auto_cal_detector_init(&detector, 400);
    for (i = 0; i < 1700; ++i) {
        int phase = (i + 100) % 400;
        int16_t raw = (int16_t)(3000 + phase * 30);
        int result;

        if (i % 5 == 0) {
            continue;
        }
        result = auto_cal_process_sample(
            &detector,
            raw,
            &event
        );
        if (result == AUTO_EVENT_REVERSED) {
            return 0;
        }
    }
    return detector.transition_count ==
               AUTO_CALIB_TRANSITIONS &&
           auto_cal_finalize(
               &detector,
               &raw_low,
               &raw_high,
               &deviation) &&
           raw_low >= 2900.0 && raw_low <= 3900.0 &&
           raw_high >= 13900.0 && raw_high <= 15100.0 &&
           deviation <= AUTO_CALIB_SPAN_TOL;
}

static double auto_cal_trimmed_mean(
    int16_t *values,
    int count)
{
    int trim_count;
    int start;
    int end;
    int i;
    double sum = 0.0;

    if (!values || count <= 0) {
        return 0.0;
    }
    qsort(
        values,
        (size_t)count,
        sizeof(values[0]),
        compare_int16
    );
    trim_count = (int)(
        (double)count *
        AUTO_CALIB_ENDPOINT_TRIM_FRACTION
    );
    if (trim_count * 2 >= count) {
        trim_count = 0;
    }
    start = trim_count;
    end = count - trim_count;
    for (i = start; i < end; ++i) {
        sum += (double)values[i];
    }
    return sum / (double)(end - start);
}

static int auto_cal_refine_endpoints_from_csv(
    const char *path,
    int sample_rate_hz,
    AutoCalDetector *detector)
{
    int16_t pre_values[
        AUTO_CALIB_TRANSITIONS
    ][AUTO_CALIB_ENDPOINT_MAX_SAMPLES];
    int16_t post_values[
        AUTO_CALIB_TRANSITIONS
    ][AUTO_CALIB_ENDPOINT_MAX_SAMPLES];
    int pre_count[AUTO_CALIB_TRANSITIONS] = {0};
    int post_count[AUTO_CALIB_TRANSITIONS] = {0};
    int hard_gap_in_window[AUTO_CALIB_TRANSITIONS] = {0};
    unsigned long long transition_time_ms[
        AUTO_CALIB_TRANSITIONS
    ] = {0};
    int transition_time_found[AUTO_CALIB_TRANSITIONS] = {0};
    int expected_window_samples;
    int minimum_samples;
    FILE *file = NULL;
    char line[2048];
    int i;

    if (!path || !*path || !detector ||
        detector->transition_count != AUTO_CALIB_TRANSITIONS ||
        sample_rate_hz <= 0) {
        return 0;
    }
    expected_window_samples =
        (sample_rate_hz *
             (AUTO_CALIB_ENDPOINT_FAR_MS -
              AUTO_CALIB_ENDPOINT_NEAR_MS) +
         999) /
        1000;
    if (expected_window_samples >
        AUTO_CALIB_ENDPOINT_MAX_SAMPLES) {
        return 0;
    }
    minimum_samples = expected_window_samples / 2;
    if (minimum_samples < 5) {
        minimum_samples = 5;
    }
    if (fopen_s(&file, path, "rb") != 0 || !file) {
        return 0;
    }
    /*
     * Resolve each detector index to the acquisition clock first. Frame
     * losses make detector-index windows wider than their intended duration,
     * which biases the endpoint estimate on a continuously rotating encoder.
     */
    while (fgets(line, sizeof(line), file)) {
        long long rx_index;
        long long detector_index;
        unsigned long long elapsed_ms;
        long long qpc;
        long frame;
        unsigned long frame_delta;
        int frame_gap;
        int raw;
        int parsed = sscanf_s(
            line,
            "%lld;%lld;%llu;%lld;%ld;%lu;%d;%d",
            &rx_index,
            &detector_index,
            &elapsed_ms,
            &qpc,
            &frame,
            &frame_delta,
            &frame_gap,
            &raw
        );

        (void)rx_index;
        (void)qpc;
        (void)frame;
        (void)frame_delta;
        (void)frame_gap;
        if (parsed != 8 ||
            detector_index < 0 ||
            raw < INT16_MIN || raw > INT16_MAX) {
            continue;
        }
        for (i = 0; i < AUTO_CALIB_TRANSITIONS; ++i) {
            if (detector_index ==
                detector->transition_indices[i]) {
                transition_time_ms[i] = elapsed_ms;
                transition_time_found[i] = 1;
            }
        }
    }
    if (ferror(file)) {
        fclose(file);
        return 0;
    }
    for (i = 0; i < AUTO_CALIB_TRANSITIONS; ++i) {
        if (!transition_time_found[i] ||
            transition_time_ms[i] <
                AUTO_CALIB_ENDPOINT_FAR_MS) {
            fclose(file);
            return 0;
        }
    }
    clearerr(file);
    rewind(file);
    while (fgets(line, sizeof(line), file)) {
        long long rx_index;
        long long detector_index;
        unsigned long long elapsed_ms;
        long long qpc;
        long frame;
        unsigned long frame_delta;
        int frame_gap;
        int raw;
        int parsed = sscanf_s(
            line,
            "%lld;%lld;%llu;%lld;%ld;%lu;%d;%d",
            &rx_index,
            &detector_index,
            &elapsed_ms,
            &qpc,
            &frame,
            &frame_delta,
            &frame_gap,
            &raw
        );

        (void)rx_index;
        (void)qpc;
        (void)frame;
        (void)frame_gap;
        if (parsed != 8 ||
            detector_index < 0 ||
            raw < INT16_MIN || raw > INT16_MAX) {
            continue;
        }
        for (i = 0; i < AUTO_CALIB_TRANSITIONS; ++i) {
            long long relative_ms =
                (long long)elapsed_ms -
                (long long)transition_time_ms[i];
            if (frame_delta >
                    AUTO_CALIB_MAX_TOLERATED_FRAME_DELTA &&
                frame_delta < 0x80000000UL &&
                relative_ms >=
                    -AUTO_CALIB_ENDPOINT_FAR_MS &&
                relative_ms <=
                    AUTO_CALIB_ENDPOINT_FAR_MS) {
                hard_gap_in_window[i] = 1;
            }
            if (relative_ms >=
                    -AUTO_CALIB_ENDPOINT_FAR_MS &&
                relative_ms <=
                    -AUTO_CALIB_ENDPOINT_NEAR_MS &&
                pre_count[i] <
                    AUTO_CALIB_ENDPOINT_MAX_SAMPLES) {
                pre_values[i][pre_count[i]++] =
                    (int16_t)raw;
            }
            if (relative_ms >=
                    AUTO_CALIB_ENDPOINT_NEAR_MS &&
                relative_ms <=
                    AUTO_CALIB_ENDPOINT_FAR_MS &&
                post_count[i] <
                    AUTO_CALIB_ENDPOINT_MAX_SAMPLES) {
                post_values[i][post_count[i]++] =
                    (int16_t)raw;
            }
        }
    }
    {
        int read_error = ferror(file);
        int close_error = fclose(file);
        if (read_error || close_error != 0) {
            return 0;
        }
    }
    for (i = 0; i < AUTO_CALIB_TRANSITIONS; ++i) {
        double pre_center;
        double post_center;

        if (hard_gap_in_window[i] ||
            pre_count[i] < minimum_samples ||
            post_count[i] < minimum_samples) {
            return 0;
        }
        pre_center = auto_cal_trimmed_mean(
            pre_values[i],
            pre_count[i]
        );
        post_center = auto_cal_trimmed_mean(
            post_values[i],
            post_count[i]
        );
        detector->raw_lows[i] =
            pre_center < post_center
                ? pre_center
                : post_center;
        detector->raw_highs[i] =
            pre_center > post_center
                ? pre_center
                : post_center;
        detector->jumps[i] = post_center - pre_center;
        if (pre_center <= post_center) {
            detector->endpoint_low_samples[i] =
                pre_count[i];
            detector->endpoint_high_samples[i] =
                post_count[i];
        } else {
            detector->endpoint_low_samples[i] =
                post_count[i];
            detector->endpoint_high_samples[i] =
                pre_count[i];
        }
    }
    detector->endpoints_refined = 1;
    return 1;
}

typedef struct {
    long long detector_index;
    long long qpc;
    double raw;
} AngularDlgSample;

typedef struct {
    long long slot_index;
    long long qpc;
    int position;
    unsigned int position_modulus;
    long long unwrapped;
} AngularDriveSample;

typedef struct {
    unsigned long rows_total;
    unsigned long missing_rows;
    unsigned long invalid_rows;
    unsigned long outlier_rows;
    unsigned long valid_samples;
    double max_valid_gap_s;
} AngularDriveQuality;

typedef struct {
    int revolution;
    int bin;
    double raw;
    double reference_degrees;
} AngularTaggedSample;

typedef struct {
    int revolution;
    int bin;
    double raw;
    double degrees;
} AngularBinPoint;

static int split_fields(
    char *line,
    const char *delimiters,
    char **fields,
    int capacity)
{
    char *context = NULL;
    char *token;
    int count = 0;

    if (!line || !delimiters || !fields || capacity <= 0) {
        return 0;
    }
    token = strtok_s(line, delimiters, &context);
    while (token && count < capacity) {
        fields[count++] = token;
        token = strtok_s(NULL, delimiters, &context);
    }
    return count;
}

static int append_dlg_sample(
    AngularDlgSample **samples,
    size_t *count,
    size_t *capacity,
    const AngularDlgSample *sample)
{
    AngularDlgSample *resized;
    size_t new_capacity;

    if (!samples || !count || !capacity || !sample) {
        return 0;
    }
    if (*count >= *capacity) {
        new_capacity = *capacity == 0 ? 4096U : *capacity * 2U;
        if (new_capacity < *capacity ||
            new_capacity > SIZE_MAX / sizeof(**samples)) {
            return 0;
        }
        resized = (AngularDlgSample *)realloc(
            *samples,
            new_capacity * sizeof(**samples)
        );
        if (!resized) {
            return 0;
        }
        *samples = resized;
        *capacity = new_capacity;
    }
    (*samples)[(*count)++] = *sample;
    return 1;
}

static int append_drive_sample(
    AngularDriveSample **samples,
    size_t *count,
    size_t *capacity,
    const AngularDriveSample *sample)
{
    AngularDriveSample *resized;
    size_t new_capacity;

    if (!samples || !count || !capacity || !sample) {
        return 0;
    }
    if (*count >= *capacity) {
        new_capacity = *capacity == 0 ? 1024U : *capacity * 2U;
        if (new_capacity < *capacity ||
            new_capacity > SIZE_MAX / sizeof(**samples)) {
            return 0;
        }
        resized = (AngularDriveSample *)realloc(
            *samples,
            new_capacity * sizeof(**samples)
        );
        if (!resized) {
            return 0;
        }
        *samples = resized;
        *capacity = new_capacity;
    }
    (*samples)[(*count)++] = *sample;
    return 1;
}

static int read_angular_dlg_samples(
    const char *path,
    AngularDlgSample **samples,
    size_t *sample_count,
    int *saturation_samples,
    double *raw_min,
    double *raw_max)
{
    FILE *file = NULL;
    AngularDlgSample *values = NULL;
    size_t count = 0;
    size_t capacity = 0;
    char line[2048];
    int saturation = 0;
    double minimum = 0.0;
    double maximum = 0.0;

    if (!path || !samples || !sample_count ||
        !saturation_samples || !raw_min || !raw_max ||
        fopen_s(&file, path, "rb") != 0 || !file) {
        return 0;
    }
    while (fgets(line, sizeof(line), file)) {
        char *fields[8];
        int field_count = split_fields(
            line,
            ";\r\n",
            fields,
            (int)(sizeof(fields) / sizeof(fields[0]))
        );
        AngularDlgSample sample;
        char *end_index = NULL;
        char *end_qpc = NULL;
        char *end_raw = NULL;
        long long detector_index;
        long long qpc;
        long raw;

        if (field_count < 8) {
            continue;
        }
        detector_index = strtoll(fields[1], &end_index, 10);
        qpc = strtoll(fields[3], &end_qpc, 10);
        raw = strtol(fields[7], &end_raw, 10);
        if (end_index == fields[1] || *end_index != '\0' ||
            end_qpc == fields[3] || *end_qpc != '\0' ||
            end_raw == fields[7] || *end_raw != '\0' ||
            detector_index < 0 || qpc <= 0 ||
            raw < INT16_MIN || raw > INT16_MAX) {
            continue;
        }
        sample.detector_index = detector_index;
        sample.qpc = qpc;
        sample.raw = (double)raw;
        if (count > 0 &&
            sample.detector_index <=
                values[count - 1].detector_index) {
            continue;
        }
        if (!append_dlg_sample(
                &values,
                &count,
                &capacity,
                &sample)) {
            free(values);
            fclose(file);
            return 0;
        }
        if (count == 1 || sample.raw < minimum) {
            minimum = sample.raw;
        }
        if (count == 1 || sample.raw > maximum) {
            maximum = sample.raw;
        }
        if (raw <= ADC_SATURATION_LOW ||
            raw >= ADC_SATURATION_HIGH) {
            ++saturation;
        }
    }
    {
        int read_error = ferror(file);
        int close_error = fclose(file);
        if (read_error || close_error != 0 || count == 0) {
            free(values);
            return 0;
        }
    }
    *samples = values;
    *sample_count = count;
    *saturation_samples = saturation;
    *raw_min = minimum;
    *raw_max = maximum;
    return 1;
}

static long drive_modular_delta(
    int current,
    int previous,
    unsigned int position_modulus)
{
    long delta = (long)current - (long)previous;
    long half = (long)(position_modulus / 2U);

    if (delta > half) {
        delta -= (long)position_modulus;
    } else if (delta < -half) {
        delta += (long)position_modulus;
    }
    return delta;
}

static int drive_delta_is_plausible(
    long delta,
    long long qpc_delta,
    long long qpc_frequency,
    unsigned int position_modulus,
    double configured_ratio,
    double encoder_target_rpm,
    int expected_direction)
{
    double elapsed_s;
    double expected_counts;
    double allowed_counts;
    double directed_delta;

    if (qpc_delta <= 0 || qpc_frequency <= 0 ||
        position_modulus < 2U ||
        configured_ratio <= 0.0 ||
        encoder_target_rpm <= 0.0 ||
        (expected_direction != -1 &&
         expected_direction != 1)) {
        return 0;
    }
    elapsed_s =
        (double)qpc_delta / (double)qpc_frequency;
    expected_counts =
        (double)position_modulus *
        configured_ratio *
        encoder_target_rpm *
        elapsed_s / 60.0;
    allowed_counts =
        ANGULAR_DRIVE_JITTER_COUNTS +
        ANGULAR_DRIVE_SPEED_MARGIN * expected_counts;
    directed_delta =
        (double)expected_direction * (double)delta;
    return directed_delta >= -ANGULAR_DRIVE_JITTER_COUNTS &&
           auto_cal_abs((double)delta) <= allowed_counts;
}

static int read_angular_drive_samples(
    const char *path,
    double configured_ratio,
    double encoder_target_rpm,
    int expected_direction,
    long long qpc_frequency,
    AngularDriveSample **samples,
    size_t *sample_count,
    AngularDriveQuality *quality)
{
    FILE *file = NULL;
    AngularDriveSample *values = NULL;
    AngularDriveQuality measured;
    size_t count = 0;
    size_t capacity = 0;
    char line[512];

    memset(&measured, 0, sizeof(measured));
    if (!path || configured_ratio <= 0.0 ||
        encoder_target_rpm <= 0.0 ||
        (expected_direction != -1 &&
         expected_direction != 1) ||
        qpc_frequency <= 0 ||
        !samples || !sample_count || !quality ||
        fopen_s(&file, path, "rb") != 0 || !file) {
        return 0;
    }
    while (fgets(line, sizeof(line), file)) {
        char *fields[8];
        int field_count = split_fields(
            line,
            ",\r\n",
            fields,
            (int)(sizeof(fields) / sizeof(fields[0]))
        );
        AngularDriveSample sample;
        char *end_slot_index = NULL;
        char *end_qpc = NULL;
        char *end_position = NULL;
        char *end_error = NULL;
        char *end_modulus = NULL;
        long long slot_index;
        long long qpc;
        long position;
        long position_error;
        unsigned long position_modulus;

        if (field_count > 0 &&
            _stricmp(fields[0], "idx") == 0) {
            continue;
        }
        ++measured.rows_total;
        if (field_count < 8) {
            ++measured.invalid_rows;
            continue;
        }
        if (_stricmp(fields[3], "NULL") == 0) {
            ++measured.missing_rows;
            continue;
        }
        slot_index =
            strtoll(fields[0], &end_slot_index, 10);
        qpc = strtoll(fields[1], &end_qpc, 10);
        position = strtol(fields[3], &end_position, 10);
        position_error = strtol(fields[5], &end_error, 10);
        position_modulus =
            strtoul(fields[7], &end_modulus, 10);
        if (end_slot_index == fields[0] ||
            *end_slot_index != '\0' ||
            end_qpc == fields[1] || *end_qpc != '\0' ||
            end_position == fields[3] ||
            *end_position != '\0' ||
            end_error == fields[5] || *end_error != '\0' ||
            end_modulus == fields[7] ||
            *end_modulus != '\0' ||
            slot_index < 0 || qpc <= 0 ||
            position_modulus < 2UL ||
            position_modulus > 65536UL ||
            position < 0 ||
            (unsigned long)position >= position_modulus) {
            ++measured.invalid_rows;
            continue;
        }
        if (position_error != 0) {
            ++measured.missing_rows;
            continue;
        }
        sample.slot_index = slot_index;
        sample.qpc = qpc;
        sample.position = (int)position;
        sample.position_modulus =
            (unsigned int)position_modulus;
        sample.unwrapped = position;
        if (count > 0) {
            long delta;
            long long slot_delta =
                sample.slot_index -
                values[count - 1].slot_index;
            long long qpc_delta =
                sample.qpc - values[count - 1].qpc;
            double gap_s;

            if (qpc_delta <= 0 || slot_delta <= 0) {
                ++measured.invalid_rows;
                continue;
            }
            if (sample.position_modulus !=
                values[count - 1].position_modulus) {
                free(values);
                fclose(file);
                return 0;
            }
            delta = drive_modular_delta(
                sample.position,
                values[count - 1].position,
                sample.position_modulus
            );
            if (!drive_delta_is_plausible(
                    delta,
                    qpc_delta,
                    qpc_frequency,
                    sample.position_modulus,
                    configured_ratio,
                    encoder_target_rpm,
                    expected_direction)) {
                ++measured.outlier_rows;
                continue;
            }
            sample.unwrapped =
                values[count - 1].unwrapped +
                (long long)delta;
            gap_s =
                (double)qpc_delta /
                (double)qpc_frequency;
            if (gap_s > measured.max_valid_gap_s) {
                measured.max_valid_gap_s = gap_s;
            }
        }
        if (!append_drive_sample(
                &values,
                &count,
                &capacity,
                &sample)) {
            free(values);
            fclose(file);
            return 0;
        }
        ++measured.valid_samples;
    }
    {
        int read_error = ferror(file);
        int close_error = fclose(file);
        if (read_error || close_error != 0 || count < 2) {
            free(values);
            return 0;
        }
    }
    *samples = values;
    *sample_count = count;
    *quality = measured;
    return 1;
}

static int interpolate_drive_unwrapped(
    const AngularDriveSample *samples,
    size_t count,
    long long qpc,
    long long maximum_gap_qpc,
    double *unwrapped)
{
    size_t low = 0;
    size_t high;

    if (!samples || count < 2 || !unwrapped ||
        qpc < samples[0].qpc ||
        qpc > samples[count - 1].qpc) {
        return 0;
    }
    high = count - 1;
    while (low + 1 < high) {
        size_t middle = low + (high - low) / 2;
        if (samples[middle].qpc <= qpc) {
            low = middle;
        } else {
            high = middle;
        }
    }
    if (samples[high].qpc == samples[low].qpc) {
        *unwrapped = (double)samples[low].unwrapped;
        return 1;
    }
    if (maximum_gap_qpc > 0 &&
        samples[high].qpc - samples[low].qpc >
            maximum_gap_qpc) {
        return 0;
    }
    {
        double fraction =
            (double)(qpc - samples[low].qpc) /
            (double)(samples[high].qpc -
                     samples[low].qpc);
        *unwrapped =
            (double)samples[low].unwrapped +
            fraction *
                (double)(samples[high].unwrapped -
                         samples[low].unwrapped);
    }
    return 1;
}

static int find_dlg_qpc_for_index(
    const AngularDlgSample *samples,
    size_t count,
    long long detector_index,
    long long *qpc)
{
    size_t low = 0;
    size_t high = count;

    if (!samples || count == 0 || !qpc) {
        return 0;
    }
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (samples[middle].detector_index <
            detector_index) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    if (low >= count ||
        samples[low].detector_index != detector_index) {
        return 0;
    }
    *qpc = samples[low].qpc;
    return 1;
}

static int compare_angular_tagged(
    const void *left,
    const void *right)
{
    const AngularTaggedSample *a =
        (const AngularTaggedSample *)left;
    const AngularTaggedSample *b =
        (const AngularTaggedSample *)right;

    if (a->revolution != b->revolution) {
        return (a->revolution > b->revolution) -
               (a->revolution < b->revolution);
    }
    if (a->bin != b->bin) {
        return (a->bin > b->bin) - (a->bin < b->bin);
    }
    return (a->raw > b->raw) - (a->raw < b->raw);
}

static double median_sorted_tagged_raw(
    const AngularTaggedSample *samples,
    size_t count)
{
    if ((count & 1U) != 0U) {
        return samples[count / 2U].raw;
    }
    return 0.5 *
        (samples[count / 2U - 1U].raw +
         samples[count / 2U].raw);
}

static int weighted_linear_fit(
    const AngularBinPoint *points,
    const double *weights,
    int count,
    double *slope,
    double *intercept)
{
    double sum_w = 0.0;
    double sum_x = 0.0;
    double sum_y = 0.0;
    double sum_xx = 0.0;
    double sum_xy = 0.0;
    double denominator;
    int i;

    if (!points || count < 2 || !slope || !intercept) {
        return 0;
    }
    for (i = 0; i < count; ++i) {
        double weight = weights ? weights[i] : 1.0;
        double x = points[i].raw;
        double y = points[i].degrees;

        sum_w += weight;
        sum_x += weight * x;
        sum_y += weight * y;
        sum_xx += weight * x * x;
        sum_xy += weight * x * y;
    }
    denominator = sum_w * sum_xx - sum_x * sum_x;
    if (sum_w <= 0.0 ||
        auto_cal_abs(denominator) < 1.0e-12) {
        return 0;
    }
    *slope =
        (sum_w * sum_xy - sum_x * sum_y) /
        denominator;
    *intercept = (sum_y - *slope * sum_x) / sum_w;
    return *slope != 0.0;
}

static int robust_angular_linear_fit(
    const AngularBinPoint *points,
    int count,
    double *slope,
    double *intercept,
    double *rmse)
{
    double *weights = NULL;
    double *absolute_residuals = NULL;
    double sum_squared = 0.0;
    int iteration;
    int i;
    int ok = 0;

    if (!points || count < 2 || !slope || !intercept ||
        !rmse) {
        return 0;
    }
    weights = (double *)malloc((size_t)count * sizeof(*weights));
    absolute_residuals =
        (double *)malloc(
            (size_t)count * sizeof(*absolute_residuals)
        );
    if (!weights || !absolute_residuals) {
        goto cleanup;
    }
    for (i = 0; i < count; ++i) {
        weights[i] = 1.0;
    }
    if (!weighted_linear_fit(
            points,
            NULL,
            count,
            slope,
            intercept)) {
        goto cleanup;
    }
    for (iteration = 0;
         iteration < ANGULAR_HUBER_ITERATIONS;
         ++iteration) {
        double median_absolute;
        double scale;
        double cutoff;

        for (i = 0; i < count; ++i) {
            absolute_residuals[i] = auto_cal_abs(
                *slope * points[i].raw +
                *intercept -
                points[i].degrees
            );
        }
        qsort(
            absolute_residuals,
            (size_t)count,
            sizeof(absolute_residuals[0]),
            compare_double
        );
        median_absolute =
            (count & 1) != 0
                ? absolute_residuals[count / 2]
                : 0.5 *
                    (absolute_residuals[count / 2 - 1] +
                     absolute_residuals[count / 2]);
        scale = 1.4826 * median_absolute;
        if (scale < 1.0e-9) {
            break;
        }
        cutoff = ANGULAR_HUBER_K * scale;
        for (i = 0; i < count; ++i) {
            double residual = auto_cal_abs(
                *slope * points[i].raw +
                *intercept -
                points[i].degrees
            );
            weights[i] =
                residual <= cutoff || residual == 0.0
                    ? 1.0
                    : cutoff / residual;
        }
        if (!weighted_linear_fit(
                points,
                weights,
                count,
                slope,
                intercept)) {
            goto cleanup;
        }
    }
    for (i = 0; i < count; ++i) {
        double error =
            *slope * points[i].raw +
            *intercept -
            points[i].degrees;
        sum_squared += error * error;
    }
    *rmse = sqrt(sum_squared / (double)count);
    ok = 1;

cleanup:
    free(weights);
    free(absolute_residuals);
    return ok;
}

static double normalize_degrees(double degrees)
{
    double normalized = fmod(degrees, ENCODER_FULL_SCALE_DEG);

    if (normalized < 0.0) {
        normalized += ENCODER_FULL_SCALE_DEG;
    }
    return normalized;
}

static double circular_error_degrees(
    double predicted,
    double reference)
{
    double error = normalize_degrees(predicted - reference);

    if (error > ENCODER_FULL_SCALE_DEG / 2.0) {
        error -= ENCODER_FULL_SCALE_DEG;
    }
    return error;
}

static int derive_drive_csv_path(
    const char *diagnostic_path,
    char *drive_path,
    size_t drive_path_size)
{
    const char *extension;
    size_t prefix_length;

    if (!diagnostic_path || !drive_path ||
        drive_path_size == 0) {
        return 0;
    }
    extension = strrchr(diagnostic_path, '.');
    if (!extension || _stricmp(extension, ".csv") != 0) {
        return 0;
    }
    prefix_length = (size_t)(extension - diagnostic_path);
    if (prefix_length + strlen("_drive\\drive.csv") + 1 >
        drive_path_size) {
        return 0;
    }
    memcpy(drive_path, diagnostic_path, prefix_length);
    drive_path[prefix_length] = '\0';
    strcat_s(
        drive_path,
        drive_path_size,
        "_drive\\drive.csv"
    );
    return 1;
}

static int compute_angular_fit_from_csv(
    const char *diagnostic_path,
    const char *drive_path,
    const AutoCalDetector *detector,
    double configured_ratio,
    double encoder_target_rpm,
    AngularFit *fit)
{
    AngularDlgSample *dlg = NULL;
    AngularDriveSample *drive = NULL;
    AngularDriveQuality drive_quality;
    AngularTaggedSample *tagged = NULL;
    AngularBinPoint points[
        ANGULAR_BIN_COUNT *
        ANGULAR_COMPLETE_REVOLUTIONS
    ];
    AngularBinPoint training[
        ANGULAR_BIN_COUNT *
        (ANGULAR_COMPLETE_REVOLUTIONS - 1)
    ];
    AngularBinPoint validation[ANGULAR_BIN_COUNT];
    long long wrap_qpc[AUTO_CALIB_TRANSITIONS];
    double wrap_motor[AUTO_CALIB_TRANSITIONS];
    size_t dlg_count = 0;
    size_t drive_count = 0;
    size_t tagged_count = 0;
    int point_count = 0;
    int training_count = 0;
    int validation_count = 0;
    int saturation_samples = 0;
    double raw_min = 0.0;
    double raw_max = 0.0;
    double validation_sum_squared = 0.0;
    double validation_sum_reference = 0.0;
    double validation_sum_total = 0.0;
    double *absolute_errors = NULL;
    double *filtered_absolute_errors = NULL;
    double ratio_sum = 0.0;
    double ratio_max_error = 0.0;
    double filtered_sum_squared = 0.0;
    int valid_bins_per_revolution[
        ANGULAR_COMPLETE_REVOLUTIONS
    ] = {0};
    int motor_direction_sign = 0;
    long long maximum_drive_gap_qpc = 0;
    LARGE_INTEGER qpc_frequency;
    int revolution = 0;
    size_t i;
    int ok = 0;

    if (!diagnostic_path || !drive_path || !detector || !fit ||
        detector->transition_count != AUTO_CALIB_TRANSITIONS ||
        configured_ratio <= 0.0 ||
        encoder_target_rpm <= 0.0 ||
        (detector->motion_sign != -1 &&
         detector->motion_sign != 1)) {
        return 0;
    }
    memset(fit, 0, sizeof(*fit));
    memset(&drive_quality, 0, sizeof(drive_quality));
    memset(&qpc_frequency, 0, sizeof(qpc_frequency));
    if (!QueryPerformanceFrequency(&qpc_frequency) ||
        qpc_frequency.QuadPart <= 0) {
        return 0;
    }
    maximum_drive_gap_qpc = (long long)(
        (double)qpc_frequency.QuadPart *
        ANGULAR_MAX_DRIVE_GAP_S
    );
    if (!read_angular_dlg_samples(
            diagnostic_path,
            &dlg,
            &dlg_count,
            &saturation_samples,
            &raw_min,
            &raw_max) ||
        !read_angular_drive_samples(
            drive_path,
            configured_ratio,
            encoder_target_rpm,
            detector->motion_sign,
            qpc_frequency.QuadPart,
            &drive,
            &drive_count,
            &drive_quality)) {
        goto cleanup;
    }
    fit->drive_rows_total = drive_quality.rows_total;
    fit->drive_missing_rows = drive_quality.missing_rows;
    fit->drive_invalid_rows = drive_quality.invalid_rows;
    fit->drive_outlier_rows = drive_quality.outlier_rows;
    fit->drive_valid_samples = drive_quality.valid_samples;
    fit->drive_max_valid_gap_s =
        drive_quality.max_valid_gap_s;
    for (revolution = 0;
         revolution < AUTO_CALIB_TRANSITIONS;
         ++revolution) {
        if (!find_dlg_qpc_for_index(
                dlg,
                dlg_count,
                detector->transition_indices[revolution],
                &wrap_qpc[revolution]) ||
            !interpolate_drive_unwrapped(
                drive,
                drive_count,
                wrap_qpc[revolution],
                maximum_drive_gap_qpc,
                &wrap_motor[revolution])) {
            goto cleanup;
        }
    }
    fit->drive_position_modulus =
        drive[0].position_modulus;
    for (revolution = 0;
         revolution < ANGULAR_COMPLETE_REVOLUTIONS;
         ++revolution) {
        double signed_counts =
            wrap_motor[revolution + 1] -
            wrap_motor[revolution];
        int current_direction =
            signed_counts >= 0.0 ? 1 : -1;
        double counts = auto_cal_abs(signed_counts);
        double ratio =
            counts /
            (double)fit->drive_position_modulus;
        double ratio_error =
            auto_cal_abs(ratio - configured_ratio) /
            configured_ratio;

        if (counts < 1.0) {
            goto cleanup;
        }
        if (motor_direction_sign == 0) {
            motor_direction_sign = current_direction;
        } else if (motor_direction_sign != current_direction) {
            goto cleanup;
        }
        fit->ratio_per_revolution[revolution] = ratio;
        ratio_sum += ratio;
        if (ratio_error > ratio_max_error) {
            ratio_max_error = ratio_error;
        }
    }
    fit->measured_ratio_mean =
        ratio_sum / (double)ANGULAR_COMPLETE_REVOLUTIONS;
    fit->measured_ratio_max_error_fraction =
        ratio_max_error;
    fit->raw_min = raw_min;
    fit->raw_max = raw_max;
    fit->saturation_samples = saturation_samples;

    tagged = (AngularTaggedSample *)malloc(
        dlg_count * sizeof(*tagged)
    );
    if (!tagged) {
        goto cleanup;
    }
    revolution = 0;
    for (i = 0; i < dlg_count; ++i) {
        double motor;
        double progress_counts;
        double progress_degrees;
        double degrees;
        int bin;

        while (revolution <
                   ANGULAR_COMPLETE_REVOLUTIONS &&
               dlg[i].detector_index >=
                   detector->transition_indices[
                       revolution + 1]) {
            ++revolution;
        }
        if (revolution >= ANGULAR_COMPLETE_REVOLUTIONS ||
            dlg[i].detector_index <
                detector->transition_indices[revolution] ||
            !interpolate_drive_unwrapped(
                drive,
                drive_count,
                dlg[i].qpc,
                maximum_drive_gap_qpc,
                &motor)) {
            continue;
        }
        progress_counts =
            (double)motor_direction_sign *
            (motor - wrap_motor[revolution]);
        progress_degrees =
            progress_counts *
            ENCODER_FULL_SCALE_DEG /
            ((double)fit->drive_position_modulus *
             configured_ratio);
        if (progress_degrees < 0.0 ||
            progress_degrees >
                ENCODER_FULL_SCALE_DEG *
                    (1.0 +
                     ANGULAR_MAX_RATIO_ERROR_FRACTION *
                         2.0)) {
            continue;
        }
        degrees =
            detector->direction_sign < 0
                ? progress_degrees
                : ENCODER_FULL_SCALE_DEG -
                    progress_degrees;
        if (degrees <
                (double)ANGULAR_EDGE_EXCLUSION_DEG ||
            degrees >
                ENCODER_FULL_SCALE_DEG -
                    (double)ANGULAR_EDGE_EXCLUSION_DEG) {
            continue;
        }
        bin = (int)degrees;
        if (bin < 0 || bin >= ANGULAR_BIN_COUNT) {
            continue;
        }
        tagged[tagged_count].revolution = revolution;
        tagged[tagged_count].bin = bin;
        tagged[tagged_count].raw = dlg[i].raw;
        tagged[tagged_count].reference_degrees = degrees;
        ++tagged_count;
    }
    if (tagged_count == 0) {
        goto cleanup;
    }
    qsort(
        tagged,
        tagged_count,
        sizeof(tagged[0]),
        compare_angular_tagged
    );
    i = 0;
    while (i < tagged_count) {
        size_t group_start = i;
        size_t group_count;
        double reference_sum = 0.0;
        size_t group_index;
        int group_revolution = tagged[i].revolution;
        int group_bin = tagged[i].bin;

        while (i < tagged_count &&
               tagged[i].revolution == group_revolution &&
               tagged[i].bin == group_bin) {
            ++i;
        }
        group_count = i - group_start;
        if (group_count <
            (size_t)ANGULAR_MIN_BIN_SAMPLES) {
            continue;
        }
        if (point_count >=
            (int)(sizeof(points) / sizeof(points[0]))) {
            goto cleanup;
        }
        points[point_count].revolution = group_revolution;
        points[point_count].bin = group_bin;
        points[point_count].raw =
            median_sorted_tagged_raw(
                &tagged[group_start],
                group_count
            );
        for (group_index = group_start;
             group_index < i;
             ++group_index) {
            reference_sum +=
                tagged[group_index].reference_degrees;
        }
        points[point_count].degrees =
            reference_sum / (double)group_count;
        ++valid_bins_per_revolution[group_revolution];
        ++point_count;
    }
    for (revolution = 0;
         revolution < ANGULAR_COMPLETE_REVOLUTIONS;
         ++revolution) {
        if (valid_bins_per_revolution[revolution] <
            ANGULAR_MIN_VALID_BINS) {
            goto cleanup;
        }
    }
    for (revolution = 0;
         revolution < point_count;
         ++revolution) {
        if (points[revolution].revolution <
            ANGULAR_COMPLETE_REVOLUTIONS - 1) {
            training[training_count++] = points[revolution];
        } else {
            validation[validation_count++] =
                points[revolution];
        }
    }
    fit->training_bins = training_count;
    fit->validation_bins = validation_count;
    if (training_count <
            ANGULAR_MIN_VALID_BINS *
                (ANGULAR_COMPLETE_REVOLUTIONS - 1) ||
        validation_count < ANGULAR_MIN_VALID_BINS ||
        !robust_angular_linear_fit(
            training,
            training_count,
            &fit->training_slope_deg_per_count,
            &fit->training_intercept_deg,
            &fit->training_rmse_deg)) {
        goto cleanup;
    }
    absolute_errors = (double *)malloc(
        (size_t)validation_count *
        sizeof(*absolute_errors)
    );
    if (!absolute_errors) {
        goto cleanup;
    }
    for (i = 0; i < (size_t)validation_count; ++i) {
        double predicted =
            fit->training_slope_deg_per_count *
                validation[i].raw +
            fit->training_intercept_deg;
        double error = circular_error_degrees(
            predicted,
            validation[i].degrees
        );

        validation_sum_squared += error * error;
        validation_sum_reference += validation[i].degrees;
        absolute_errors[i] = auto_cal_abs(error);
    }
    fit->validation_rmse_deg = sqrt(
        validation_sum_squared /
        (double)validation_count
    );
    qsort(
        absolute_errors,
        (size_t)validation_count,
        sizeof(absolute_errors[0]),
        compare_double
    );
    {
        size_t p95_index =
            ((size_t)validation_count * 95U + 99U) /
            100U;
        double reference_mean =
            validation_sum_reference /
            (double)validation_count;

        if (p95_index == 0U) {
            p95_index = 1U;
        }
        if (p95_index > (size_t)validation_count) {
            p95_index = (size_t)validation_count;
        }
        fit->validation_p95_deg =
            absolute_errors[p95_index - 1U];
        fit->validation_max_error_deg =
            absolute_errors[validation_count - 1];
        for (i = 0; i < (size_t)validation_count; ++i) {
            double centered =
                validation[i].degrees -
                reference_mean;
            validation_sum_total += centered * centered;
        }
    }
    fit->r_squared =
        validation_sum_total > 0.0
            ? 1.0 -
                validation_sum_squared /
                validation_sum_total
            : 0.0;

    filtered_absolute_errors = (double *)malloc(
        dlg_count * sizeof(*filtered_absolute_errors)
    );
    if (!filtered_absolute_errors) {
        goto cleanup;
    }
    {
        int16_t filter_ring[MONITOR_FILTER_SAMPLES];
        int filter_count = 0;
        int filter_next = 0;
        long long previous_detector_index = -1;
        int filtered_count = 0;

        revolution = 0;
        for (i = 0; i < dlg_count; ++i) {
            double motor;
            double progress_counts;
            double progress_degrees;
            double reference_degrees;
            double raw_median;
            double predicted;
            double error;

            while (revolution <
                       ANGULAR_COMPLETE_REVOLUTIONS &&
                   dlg[i].detector_index >=
                       detector->transition_indices[
                           revolution + 1]) {
                ++revolution;
                filter_count = 0;
                filter_next = 0;
                previous_detector_index = -1;
            }
            if (revolution !=
                    ANGULAR_COMPLETE_REVOLUTIONS - 1 ||
                dlg[i].detector_index <
                    detector->transition_indices[revolution]) {
                continue;
            }
            if (previous_detector_index >= 0 &&
                dlg[i].detector_index !=
                    previous_detector_index + 1) {
                filter_count = 0;
                filter_next = 0;
            }
            previous_detector_index =
                dlg[i].detector_index;
            filter_ring[filter_next] =
                (int16_t)dlg[i].raw;
            filter_next =
                (filter_next + 1) %
                MONITOR_FILTER_SAMPLES;
            if (filter_count < MONITOR_FILTER_SAMPLES) {
                ++filter_count;
            }
            if (filter_count < MONITOR_FILTER_SAMPLES ||
                !interpolate_drive_unwrapped(
                    drive,
                    drive_count,
                    dlg[i].qpc,
                    maximum_drive_gap_qpc,
                    &motor)) {
                continue;
            }
            progress_counts =
                (double)motor_direction_sign *
                (motor - wrap_motor[revolution]);
            progress_degrees =
                progress_counts *
                ENCODER_FULL_SCALE_DEG /
                ((double)fit->drive_position_modulus *
                 configured_ratio);
            reference_degrees =
                detector->direction_sign < 0
                    ? progress_degrees
                    : ENCODER_FULL_SCALE_DEG -
                        progress_degrees;
            if (reference_degrees <
                    (double)ANGULAR_EDGE_EXCLUSION_DEG ||
                reference_degrees >
                    ENCODER_FULL_SCALE_DEG -
                        (double)ANGULAR_EDGE_EXCLUSION_DEG) {
                continue;
            }
            raw_median = median_int16(
                filter_ring,
                filter_count
            );
            predicted =
                fit->training_slope_deg_per_count *
                    raw_median +
                fit->training_intercept_deg;
            error = circular_error_degrees(
                predicted,
                reference_degrees
            );
            filtered_sum_squared += error * error;
            filtered_absolute_errors[filtered_count++] =
                auto_cal_abs(error);
        }
        if (filtered_count <= 0) {
            goto cleanup;
        }
        fit->validation_filtered_samples = filtered_count;
        fit->validation_filtered_rmse_deg = sqrt(
            filtered_sum_squared / (double)filtered_count
        );
        qsort(
            filtered_absolute_errors,
            (size_t)filtered_count,
            sizeof(filtered_absolute_errors[0]),
            compare_double
        );
        {
            size_t p95_index =
                ((size_t)filtered_count * 95U + 99U) /
                100U;
            if (p95_index == 0U) {
                p95_index = 1U;
            }
            if (p95_index > (size_t)filtered_count) {
                p95_index = (size_t)filtered_count;
            }
            fit->validation_filtered_p95_deg =
                filtered_absolute_errors[p95_index - 1U];
            fit->validation_filtered_max_error_deg =
                filtered_absolute_errors[
                    filtered_count - 1];
        }
    }
    if (!robust_angular_linear_fit(
            points,
            point_count,
            &fit->slope_deg_per_count,
            &fit->intercept_deg,
            &fit->operational_rmse_deg)) {
        goto cleanup;
    }
    ok = 1;

cleanup:
    free(filtered_absolute_errors);
    free(absolute_errors);
    free(tagged);
    free(drive);
    free(dlg);
    return ok;
}

static int angular_fit_passes_quality(
    const AngularFit *fit)
{
    return fit &&
           fit->saturation_samples == 0 &&
           fit->measured_ratio_max_error_fraction <=
               ANGULAR_MAX_RATIO_ERROR_FRACTION &&
           fit->validation_rmse_deg <=
               ANGULAR_MAX_RMSE_DEG &&
           fit->validation_p95_deg <=
               ANGULAR_MAX_P95_DEG &&
           fit->validation_max_error_deg <=
               ANGULAR_MAX_ERROR_DEG &&
           fit->validation_filtered_rmse_deg <=
               ANGULAR_MAX_RMSE_DEG &&
           fit->validation_filtered_p95_deg <=
               ANGULAR_MAX_P95_DEG &&
           fit->validation_filtered_max_error_deg <=
               ANGULAR_MAX_ERROR_DEG;
}

static int run_auto_cal_replay(
    const char *path,
    int sample_rate_hz,
    double mechanical_ratio)
{
    AutoCalDetector detector;
    AutoCalEvent event;
    AngularFit angular_fit;
    FILE *file = NULL;
    char line[2048];
    char drive_path[MAX_PATH];
    long long rows = 0;
    unsigned long tolerated_gaps = 0;
    unsigned long reset_gaps = 0;
    unsigned long reordered = 0;
    unsigned long long lost = 0;
    double loss_fraction = 0.0;
    int have_valid_row = 0;
    int loss_acceptable = 1;
    int reversed = 0;
    int finalized = 0;

    if (!path || !*path || sample_rate_hz <= 0 ||
        mechanical_ratio <= 0.0 ||
        fopen_s(&file, path, "rb") != 0 || !file) {
        fprintf(stderr, "Falha ao abrir replay: %s\n", path ? path : "");
        return 0;
    }
    auto_cal_detector_init(&detector, sample_rate_hz);
    while (fgets(line, sizeof(line), file)) {
        long long rx_index;
        long long detector_index;
        unsigned long long elapsed_ms;
        long long qpc;
        long frame;
        unsigned long frame_delta;
        int frame_gap;
        int raw;
        int parsed = sscanf_s(
            line,
            "%lld;%lld;%llu;%lld;%ld;%lu;%d;%d",
            &rx_index,
            &detector_index,
            &elapsed_ms,
            &qpc,
            &frame,
            &frame_delta,
            &frame_gap,
            &raw
        );
        int result;

        (void)rx_index;
        (void)elapsed_ms;
        (void)qpc;
        (void)frame;
        (void)frame_gap;
        if (parsed != 8 || raw < INT16_MIN || raw > INT16_MAX) {
            continue;
        }
        if (detector_index < 0) {
            ++reordered;
            continue;
        }
        if (frame_delta == 0UL && have_valid_row) {
            ++reordered;
            continue;
        }
        ++rows;
        if (frame_delta > 1UL &&
            frame_delta < 0x80000000UL) {
            lost += (unsigned long long)(frame_delta - 1UL);
            if (auto_cal_gap_requires_reset(
                    (uint32_t)frame_delta)) {
                ++reset_gaps;
                auto_cal_reset_stream_history(&detector);
            } else {
                ++tolerated_gaps;
            }
        } else if (frame_delta >= 0x80000000UL) {
            ++reordered;
            continue;
        }
        have_valid_row = 1;
        result = auto_cal_process_sample(
            &detector,
            (int16_t)raw,
            &event
        );
        if (result == AUTO_EVENT_TRANSITION) {
            printf(
                "REPLAY WRAP %d/%d idx=%lld "
                "raw_low=%.1f raw_high=%.1f jump=%+.1f\n",
                detector.transition_count,
                AUTO_CALIB_TRANSITIONS,
                event.sample_index,
                event.raw_low,
                event.raw_high,
                event.jump
            );
        } else if (result == AUTO_EVENT_REVERSED) {
            reversed = 1;
            fprintf(
                stderr,
                "REPLAY falhou: inversao de sentido no idx=%lld.\n",
                event.sample_index
            );
            break;
        }
    }
    if (ferror(file)) {
        fprintf(stderr, "Falha lendo replay: %s\n", path);
        (void)fclose(file);
        return 0;
    }
    if (fclose(file) != 0) {
        fprintf(stderr, "Falha fechando replay: %s\n", path);
        return 0;
    }
    if (rows > 0 || lost > 0) {
        loss_fraction =
            (double)lost / ((double)rows + (double)lost);
        if (loss_fraction >
            AUTO_CALIB_LOSS_WARNING_FRACTION) {
            fprintf(
                stderr,
                "REPLAY aviso: perda de frames %.2f%%.\n",
                loss_fraction * 100.0
            );
        }
        if (loss_fraction >
            AUTO_CALIB_LOSS_REJECT_FRACTION) {
            fprintf(
                stderr,
                "REPLAY falhou: perda acima do limite de %.0f%%.\n",
                AUTO_CALIB_LOSS_REJECT_FRACTION * 100.0
            );
            loss_acceptable = 0;
        }
    }
    memset(&angular_fit, 0, sizeof(angular_fit));
    drive_path[0] = '\0';
    if (loss_acceptable &&
        !reversed &&
        detector.transition_count ==
            AUTO_CALIB_TRANSITIONS &&
        derive_drive_csv_path(
            path,
            drive_path,
            sizeof(drive_path)) &&
        compute_angular_fit_from_csv(
            path,
            drive_path,
            &detector,
            mechanical_ratio,
            DEFAULT_ENCODER_RPM,
            &angular_fit)) {
        finalized =
            angular_fit_passes_quality(&angular_fit);
        printf(
            "REPLAY ANGULAR fit_deg=%.12g*raw%+.12g "
            "train_rmse=%.4f holdout_bins=%d "
            "holdout_rmse=%.4f p95=%.4f max=%.4f "
            "monitor_rmse=%.4f p95=%.4f max=%.4f "
            "ratio=%.6f ratio_err=%.3f%% sat=%d "
            "drive_valid=%lu missing=%lu invalid=%lu "
            "outliers=%lu max_gap=%.3fs result=%s\n",
            angular_fit.slope_deg_per_count,
            angular_fit.intercept_deg,
            angular_fit.training_rmse_deg,
            angular_fit.validation_bins,
            angular_fit.validation_rmse_deg,
            angular_fit.validation_p95_deg,
            angular_fit.validation_max_error_deg,
            angular_fit.validation_filtered_rmse_deg,
            angular_fit.validation_filtered_p95_deg,
            angular_fit.validation_filtered_max_error_deg,
            angular_fit.measured_ratio_mean,
            angular_fit.measured_ratio_max_error_fraction *
                100.0,
            angular_fit.saturation_samples,
            angular_fit.drive_valid_samples,
            angular_fit.drive_missing_rows,
            angular_fit.drive_invalid_rows,
            angular_fit.drive_outlier_rows,
            angular_fit.drive_max_valid_gap_s,
            finalized ? "OK" : "FAILED"
        );
    } else if (loss_acceptable && !reversed) {
        fprintf(
            stderr,
            "REPLAY falhou: sao necessarios quatro wraps, "
            "drive.csv correspondente e dados sincronizados validos.\n"
        );
    }
    printf(
        "REPLAY RESULT rows=%lld wraps=%d candidates=%d "
        "rejected=%d window_waits=%d gaps_tolerated=%lu "
        "gaps_reset=%lu lost=%llu loss_fraction=%.6f reordered=%lu "
        "drive_csv=%s ratio_config=%.9g result=%s\n",
        rows,
        detector.transition_count,
        detector.candidate_count,
        detector.rejected_count,
        detector.window_wait_count,
        tolerated_gaps,
        reset_gaps,
        lost,
        loss_fraction,
        reordered,
        drive_path[0] ? drive_path : "NA",
        mechanical_ratio,
        finalized ? "OK" : "FAILED"
    );
    return finalized;
}

static void print_usage(const char *program)
{
    printf(
        "Uso:\n"
        "  %s                    Abre o menu interativo.\n"
        "  %s [opcoes]           Modo avancado por argumentos.\n"
        "  %s --calibrate [opcoes]\n"
        "  %s --self-test\n"
        "  %s --replay-autocal CSV [--rate HZ] [--ratio I]\n"
        "\n"
        "Modos:\n"
        "  sem argumentos       Menu: monitorar/calibrar/configurar/verificar.\n"
        "  opcoes de rede       Monitora CH3 com JSON em mA ou graus.\n"
        "  --calibrate          Calibracao manual de dois pontos com referencia.\n"
        "  --self-test          Valida conversao, layout e detector sem hardware.\n"
        "  --replay-autocal CSV Reprocessa CSV DLG + drive.csv sem hardware.\n"
        "\n"
        "Calibracao:\n"
        "  --calib ARQUIVO      Usa JSON CH3 em mA ou graus.\n"
        "  --calib-out ARQUIVO  Saida de --calibrate (padrao: %s).\n"
        "\n"
        "Rede:\n"
        "  --dlg-ip IP          Padrao: %s\n"
        "  --dlg-port PORTA     Padrao: %u\n"
        "  --local-ip IP        Interface local; padrao: 0.0.0.0\n"
        "  --local-port PORTA   Padrao: %u\n"
        "  --rate HZ            25/50/100/200/400/800/1600/3200/6400/12800\n"
        "  --ratio I            Relacao mecanica i=D2/D1 usada no replay.\n"
        "\n"
        "Durante o monitor: Q ou Ctrl+C encerra e envia ACQSTOP.\n",
        program,
        program,
        program,
        program,
        program,
        DEFAULT_CALIB_PATH,
        DEFAULT_DLG_IP,
        (unsigned)DEFAULT_DLG_PORT,
        (unsigned)DEFAULT_LOCAL_PORT
    );
}

static int parse_args(int argc, char **argv, AppConfig *config)
{
    int i;

    memset(config, 0, sizeof(*config));
    strcpy_s(config->dlg_ip, sizeof(config->dlg_ip), DEFAULT_DLG_IP);
    config->local_ip[0] = '\0';
    strcpy_s(
        config->drive_port,
        sizeof(config->drive_port),
        DEFAULT_DRIVE_PORT
    );
    config->drive_exe[0] = '\0';
    config->relation_source[0] = '\0';
    config->dlg_port = DEFAULT_DLG_PORT;
    config->local_port = DEFAULT_LOCAL_PORT;
    config->sample_rate_hz = DEFAULT_SAMPLE_RATE_HZ;
    config->drive_direction = 1;
    config->encoder_target_rpm = DEFAULT_ENCODER_RPM;
    config->mechanical_ratio = DEFAULT_MECHANICAL_RATIO;
    config->calib_out_path = DEFAULT_CALIB_PATH;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--calibrate") == 0) {
            config->calibrate = 1;
        } else if (strcmp(argv[i], "--self-test") == 0) {
            config->self_test = 1;
        } else if (strcmp(argv[i], "--replay-autocal") == 0 &&
                   i + 1 < argc) {
            config->replay_path = argv[++i];
        } else if (strcmp(argv[i], "--calib") == 0 && i + 1 < argc) {
            config->calib_path = argv[++i];
        } else if (strcmp(argv[i], "--calib-out") == 0 && i + 1 < argc) {
            config->calib_out_path = argv[++i];
        } else if (strcmp(argv[i], "--dlg-ip") == 0 && i + 1 < argc) {
            if (strcpy_s(config->dlg_ip, sizeof(config->dlg_ip), argv[++i]) != 0) {
                fprintf(stderr, "IP do DLG muito longo.\n");
                return -1;
            }
        } else if (strcmp(argv[i], "--local-ip") == 0 && i + 1 < argc) {
            if (strcpy_s(config->local_ip, sizeof(config->local_ip), argv[++i]) != 0) {
                fprintf(stderr, "IP local muito longo.\n");
                return -1;
            }
        } else if (strcmp(argv[i], "--dlg-port") == 0 && i + 1 < argc) {
            if (!parse_u16(argv[++i], &config->dlg_port)) {
                fprintf(stderr, "Porta do DLG invalida.\n");
                return -1;
            }
        } else if (strcmp(argv[i], "--local-port") == 0 && i + 1 < argc) {
            if (!parse_u16(argv[++i], &config->local_port)) {
                fprintf(stderr, "Porta local invalida.\n");
                return -1;
            }
        } else if (strcmp(argv[i], "--rate") == 0 && i + 1 < argc) {
            char *end = NULL;
            long rate = strtol(argv[++i], &end, 10);
            if (end == argv[i] || *end != '\0' ||
                rate < 1 || rate > INT_MAX ||
                !supported_sample_rate((int)rate)) {
                fprintf(stderr, "Taxa de amostragem nao suportada.\n");
                return -1;
            }
            config->sample_rate_hz = (int)rate;
        } else if (strcmp(argv[i], "--ratio") == 0 &&
                   i + 1 < argc) {
            char *end = NULL;
            double ratio = strtod(argv[++i], &end);
            if (end == argv[i] || *end != '\0' ||
                !(ratio > 0.0 && ratio <= 1000.0)) {
                fprintf(stderr, "Relacao mecanica invalida.\n");
                return -1;
            }
            config->mechanical_ratio = ratio;
            config->mechanical_ratio_explicit = 1;
        } else {
            fprintf(stderr, "Opcao invalida ou sem valor: %s\n", argv[i]);
            print_usage(argv[0]);
            return -1;
        }
    }

    if (config->calibrate && config->calib_path) {
        fprintf(stderr, "Use --calibrate ou --calib, nao os dois juntos.\n");
        return -1;
    }
    if (config->self_test && (config->calibrate || config->calib_path)) {
        fprintf(stderr, "--self-test nao aceita modo de calibracao.\n");
        return -1;
    }
    if (config->replay_path &&
        (config->self_test || config->calibrate ||
         config->calib_path)) {
        fprintf(
            stderr,
            "--replay-autocal nao aceita outros modos.\n"
        );
        return -1;
    }
    return 1;
}

static char *read_text_file(const char *path)
{
    FILE *file = NULL;
    char *buffer = NULL;
    long size;
    size_t read_count;

    if (fopen_s(&file, path, "rb") != 0 || !file) {
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    size = ftell(file);
    if (size < 0 || size > MAX_CALIB_FILE_BYTES ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }

    buffer = (char *)malloc((size_t)size + 1);
    if (!buffer) {
        fclose(file);
        return NULL;
    }
    read_count = fread(buffer, 1, (size_t)size, file);
    fclose(file);
    buffer[read_count] = '\0';
    return buffer;
}

static int json_number(const char *json, const char *key, double *value)
{
    const char *cursor = strstr(json, key);
    char *end = NULL;

    if (!cursor) {
        return 0;
    }
    cursor = strchr(cursor, ':');
    if (!cursor) {
        return 0;
    }
    ++cursor;
    while (*cursor && isspace((unsigned char)*cursor)) {
        ++cursor;
    }
    *value = strtod(cursor, &end);
    return end != cursor;
}

static int json_int(const char *json, const char *key, int *value)
{
    double parsed;
    int converted;

    if (!json_number(json, key, &parsed)) {
        return 0;
    }
    if (!(parsed >= (double)INT_MIN && parsed <= (double)INT_MAX)) {
        return 0;
    }
    converted = (int)parsed;
    if (parsed != (double)converted) {
        return 0;
    }
    *value = converted;
    return 1;
}

static int channel_in_header(const char *json, int channel)
{
    const char *cursor = strstr(json, "\"channels\"");

    if (!cursor) {
        return 0;
    }
    cursor = strchr(cursor, '[');
    if (!cursor) {
        return 0;
    }
    ++cursor;

    while (*cursor && *cursor != ']') {
        char *end = NULL;
        long value;

        while (*cursor && *cursor != ']' &&
               !isdigit((unsigned char)*cursor) && *cursor != '-') {
            ++cursor;
        }
        if (!*cursor || *cursor == ']') {
            break;
        }
        value = strtol(cursor, &end, 10);
        if (end != cursor) {
            if ((int)value == channel) {
                return 1;
            }
            cursor = end;
        } else {
            ++cursor;
        }
    }
    return 0;
}

static int channel_matches(const char *json, int channel)
{
    const char *cursor = strstr(json, "\"channel\"");
    char expected[16];
    size_t expected_length;

    if (!cursor) {
        return 0;
    }
    cursor = strchr(cursor, ':');
    if (!cursor) {
        return 0;
    }
    ++cursor;
    while (*cursor && isspace((unsigned char)*cursor)) {
        ++cursor;
    }
    if (*cursor != '"') {
        return 0;
    }
    ++cursor;
    _snprintf_s(expected, sizeof(expected), _TRUNCATE, "CH%d", channel);
    expected_length = strlen(expected);
    return strncmp(cursor, expected, expected_length) == 0 &&
           cursor[expected_length] == '"';
}

static int json_string_value_equals(
    const char *json,
    const char *key,
    const char *expected)
{
    const char *cursor = strstr(json, key);
    size_t expected_len;

    if (!cursor) {
        return 0;
    }
    cursor = strchr(cursor, ':');
    if (!cursor) {
        return 0;
    }
    ++cursor;
    while (*cursor && isspace((unsigned char)*cursor)) {
        ++cursor;
    }
    if (*cursor != '"') {
        return 0;
    }
    ++cursor;
    expected_len = strlen(expected);
    return strncmp(cursor, expected, expected_len) == 0 &&
           cursor[expected_len] == '"';
}

static int json_boolean(
    const char *json,
    const char *key,
    int *value)
{
    const char *cursor;

    if (!json || !key || !value) {
        return 0;
    }
    cursor = strstr(json, key);
    if (!cursor) {
        return 0;
    }
    cursor = strchr(cursor, ':');
    if (!cursor) {
        return 0;
    }
    ++cursor;
    while (*cursor && isspace((unsigned char)*cursor)) {
        ++cursor;
    }
    if (strncmp(cursor, "true", 4) == 0 &&
        !isalnum((unsigned char)cursor[4]) &&
        cursor[4] != '_') {
        *value = 1;
        return 1;
    }
    if (strncmp(cursor, "false", 5) == 0 &&
        !isalnum((unsigned char)cursor[5]) &&
        cursor[5] != '_') {
        *value = 0;
        return 1;
    }
    return 0;
}

/*
Return:
  1 = valid calibration
  0 = file not found
 -1 = file found but invalid
*/
static int load_calibration_file(
    const char *path,
    int require_encoder_marker,
    Calibration *calibration)
{
    char *json = read_text_file(path);
    double slope = 0.0;
    double intercept = 0.0;
    int sensor_type = -1;
    int gain_index = -1;
    int lpf_index = DEFAULT_LPF_INDEX;
    int sensor_power_index = DEFAULT_SENSPWR_INDEX;
    int input_dc_impedance = -1;
    int input_ac_impedance = -1;
    int quality_accepted = 0;
    int marked_as_ma;
    int marked_as_degrees;

    if (!json) {
        return 0;
    }

    marked_as_ma =
        json_string_value_equals(json, "\"unit\"", "mA") ||
        json_string_value_equals(
            json,
            "\"purpose\"",
            "encoder_ch3_current_ma"
        );
    marked_as_degrees =
        json_string_value_equals(json, "\"unit\"", "deg") ||
        json_string_value_equals(
            json,
            "\"purpose\"",
            "encoder_ch3_angle_deg"
        );

    if ((!channel_in_header(json, ENCODER_CHANNEL) &&
         !channel_matches(json, ENCODER_CHANNEL)) ||
        !json_int(json, "\"tSensor\"", &sensor_type) ||
        !json_int(json, "\"iGain\"", &gain_index) ||
        !json_int(json, "\"iLPF\"", &lpf_index) ||
        !json_int(
            json,
            "\"iSensPwr\"",
            &sensor_power_index
        ) ||
        !json_int(
            json,
            "\"fInputDCImp\"",
            &input_dc_impedance
        ) ||
        !json_int(
            json,
            "\"fInputACImp\"",
            &input_ac_impedance
        ) ||
        !json_number(json, "\"slope\"", &slope) ||
        !json_number(json, "\"intercept\"", &intercept)) {
        free(json);
        return -1;
    }

    (void)json_boolean(
        json,
        "\"accepted\"",
        &quality_accepted
    );
    free(json);

    if (sensor_type != SENSOR_CURRENT ||
        gain_index != ENCODER_GAIN_INDEX ||
        lpf_index != DEFAULT_LPF_INDEX ||
        sensor_power_index != DEFAULT_SENSPWR_INDEX ||
        input_dc_impedance !=
            DEFAULT_INPUT_DC_IMPEDANCE ||
        input_ac_impedance !=
            DEFAULT_INPUT_AC_IMPEDANCE ||
        !(slope > -1000000000.0 && slope < 1000000000.0) ||
        !(intercept > -1000000000.0 && intercept < 1000000000.0) ||
        slope == 0.0 ||
        (!marked_as_ma && !marked_as_degrees) ||
        (marked_as_ma && marked_as_degrees) ||
        (marked_as_degrees && !quality_accepted) ||
        (require_encoder_marker &&
         !marked_as_ma &&
         !marked_as_degrees)) {
        return -1;
    }

    calibration_init_defaults(calibration);
    calibration->output_is_degrees = marked_as_degrees;
    if (marked_as_degrees) {
        calibration->slope_deg_per_count = slope;
        calibration->intercept_deg = intercept;
    } else {
        calibration->slope_ma_per_count = slope;
        calibration->intercept_ma = intercept;
    }
    calibration->gain_index = gain_index;
    calibration->lpf_index = lpf_index;
    calibration->sensor_power_index = sensor_power_index;
    calibration->input_dc_impedance =
        input_dc_impedance;
    calibration->input_ac_impedance =
        input_ac_impedance;
    strncpy_s(
        calibration->source_path,
        sizeof(calibration->source_path),
        path,
        _TRUNCATE
    );
    return 1;
}

static int get_executable_dir(char *buffer, size_t buffer_size)
{
    DWORD length;
    size_t i;

    if (!buffer || buffer_size == 0) {
        return 0;
    }
    length = GetModuleFileNameA(NULL, buffer, (DWORD)buffer_size);
    if (length == 0 || length >= buffer_size) {
        buffer[0] = '\0';
        return 0;
    }

    for (i = (size_t)length; i > 0; --i) {
        if (buffer[i - 1] == '\\' || buffer[i - 1] == '/') {
            buffer[i - 1] = '\0';
            return 1;
        }
    }
    buffer[0] = '\0';
    return 0;
}

static int path_is_file(const char *path)
{
    DWORD attributes;

    if (!path || !*path) {
        return 0;
    }
    attributes = GetFileAttributesA(path);
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

static int load_supervisor_mechanical_ratio(
    double *ratio,
    char *source_path,
    size_t source_path_size)
{
    char local_app_data[MAX_PATH];
    char settings_path[MAX_PATH];
    char *json = NULL;
    DWORD length;
    double parsed = 0.0;
    int written;
    int ok = 0;

    if (!ratio || !source_path || source_path_size == 0) {
        return 0;
    }
    source_path[0] = '\0';
    length = GetEnvironmentVariableA(
        "LOCALAPPDATA",
        local_app_data,
        (DWORD)sizeof(local_app_data)
    );
    if (length == 0 || length >= sizeof(local_app_data)) {
        return 0;
    }
    written = _snprintf_s(
        settings_path,
        sizeof(settings_path),
        _TRUNCATE,
        "%s\\LATRIB\\supervisorio_settings.json",
        local_app_data
    );
    if (written < 0 || written >= (int)sizeof(settings_path)) {
        return 0;
    }

    json = read_text_file(settings_path);
    if (json &&
        json_number(json, "\"relacao\"", &parsed) &&
        parsed > 0.0 &&
        parsed <= 1000.0) {
        *ratio = parsed;
        strncpy_s(
            source_path,
            source_path_size,
            settings_path,
            _TRUNCATE
        );
        ok = 1;
    }
    free(json);
    return ok;
}

static int resolve_drive_executable(
    char *path,
    size_t path_size)
{
    char executable_dir[MAX_PATH];
    char candidates[7][MAX_PATH];
    char absolute[MAX_PATH];
    size_t count = 0;
    size_t i;

    if (!path || path_size == 0 ||
        !get_executable_dir(
            executable_dir,
            sizeof(executable_dir))) {
        return 0;
    }

#define ADD_DRIVE_CANDIDATE(format, value) \
    do { \
        int candidate_written = _snprintf_s( \
            candidates[count], \
            sizeof(candidates[count]), \
            _TRUNCATE, \
            format, \
            value); \
        if (candidate_written >= 0 && \
            candidate_written < (int)sizeof(candidates[count])) { \
            ++count; \
        } \
    } while (0)

    ADD_DRIVE_CANDIDATE(
        "%s\\a5_speed_logger.exe",
        executable_dir
    );
    ADD_DRIVE_CANDIDATE(
        "%s\\..\\..\\..\\DriveA5\\build_vs2022\\Release\\"
        "a5_speed_logger.exe",
        executable_dir
    );
    ADD_DRIVE_CANDIDATE(
        "%s\\..\\..\\DriveA5\\build_vs2022\\Release\\"
        "a5_speed_logger.exe",
        executable_dir
    );
    ADD_DRIVE_CANDIDATE(
        "%s\\..\\..\\..\\DriveA5\\build\\Release\\"
        "a5_speed_logger.exe",
        executable_dir
    );
    ADD_DRIVE_CANDIDATE(
        "%s\\..\\..\\DriveA5\\build\\Release\\"
        "a5_speed_logger.exe",
        executable_dir
    );
    ADD_DRIVE_CANDIDATE(
        "%s\\..\\..\\..\\DriveA5\\build\\a5_speed_logger.exe",
        executable_dir
    );
    ADD_DRIVE_CANDIDATE(
        "%s\\..\\..\\DriveA5\\build\\a5_speed_logger.exe",
        executable_dir
    );
#undef ADD_DRIVE_CANDIDATE

    for (i = 0; i < count; ++i) {
        DWORD absolute_length = GetFullPathNameA(
            candidates[i],
            (DWORD)sizeof(absolute),
            absolute,
            NULL
        );
        if (absolute_length > 0 &&
            absolute_length < sizeof(absolute) &&
            path_is_file(absolute)) {
            strncpy_s(path, path_size, absolute, _TRUNCATE);
            return 1;
        }
    }
    path[0] = '\0';
    return 0;
}

static int load_auto_calibration(Calibration *calibration)
{
    const char *relative_paths[] = {
        "out\\encoder_CH3_mA.json",
        "encoder_CH3_mA.json"
    };
    char executable_dir[MAX_PATH];
    char executable_paths[2][MAX_PATH];
    size_t i;

    for (i = 0; i < sizeof(relative_paths) / sizeof(relative_paths[0]); ++i) {
        int result = load_calibration_file(
            relative_paths[i],
            1,
            calibration
        );
        if (result != 0) {
            return result;
        }
    }

    if (!get_executable_dir(executable_dir, sizeof(executable_dir))) {
        return 0;
    }
    _snprintf_s(
        executable_paths[0],
        sizeof(executable_paths[0]),
        _TRUNCATE,
        "%s\\out\\encoder_CH3_mA.json",
        executable_dir
    );
    _snprintf_s(
        executable_paths[1],
        sizeof(executable_paths[1]),
        _TRUNCATE,
        "%s\\encoder_CH3_mA.json",
        executable_dir
    );

    for (i = 0; i < 2; ++i) {
        int result = load_calibration_file(
            executable_paths[i],
            1,
            calibration
        );
        if (result != 0) {
            return result;
        }
    }
    return 0;
}

static int create_parent_directories(const char *path)
{
    char copy[MAX_PATH];
    size_t length;
    size_t i;

    if (!path || strlen(path) >= sizeof(copy)) {
        return 0;
    }
    strcpy_s(copy, sizeof(copy), path);
    length = strlen(copy);

    for (i = 0; i < length; ++i) {
        if (copy[i] == '\\' || copy[i] == '/') {
            char separator = copy[i];
            copy[i] = '\0';
            if (copy[0] != '\0' &&
                !(i == 2 && copy[1] == ':') &&
                !CreateDirectoryA(copy, NULL) &&
                GetLastError() != ERROR_ALREADY_EXISTS) {
                return 0;
            }
            copy[i] = separator;
        }
    }
    return 1;
}

static int open_atomic_output(
    const char *path,
    char *temporary_path,
    size_t temporary_path_size,
    FILE **file)
{
    int written;

    if (!path || !*path || !temporary_path ||
        temporary_path_size == 0 || !file) {
        return 0;
    }
    written = _snprintf_s(
        temporary_path,
        temporary_path_size,
        _TRUNCATE,
        "%s.tmp",
        path
    );
    if (written < 0 ||
        (size_t)written >= temporary_path_size ||
        !create_parent_directories(path) ||
        fopen_s(file, temporary_path, "wb") != 0 ||
        !*file) {
        return 0;
    }
    return 1;
}

static int commit_atomic_output(
    FILE *file,
    const char *temporary_path,
    const char *path)
{
    int write_failed;

    if (!file || !temporary_path || !path) {
        return 0;
    }
    write_failed = ferror(file);
    if (fclose(file) != 0) {
        write_failed = 1;
    }
    if (write_failed ||
        !MoveFileExA(
            temporary_path,
            path,
            MOVEFILE_REPLACE_EXISTING |
                MOVEFILE_WRITE_THROUGH)) {
        (void)DeleteFileA(temporary_path);
        return 0;
    }
    return 1;
}

static int write_calibration_file(
    const char *path,
    double raw1,
    double reference1_ma,
    long long samples1,
    double raw2,
    double reference2_ma,
    long long samples2,
    const Calibration *calibration)
{
    FILE *file = NULL;
    char temporary_path[MAX_PATH];

    if (!open_atomic_output(
            path,
            temporary_path,
            sizeof(temporary_path),
            &file)) {
        return 0;
    }

    fprintf(file, "{\n");
    fprintf(file, "  \"purpose\": \"encoder_ch3_current_ma\",\n");
    fprintf(file, "  \"unit\": \"mA\",\n");
    fprintf(file, "  \"channels\": [3],\n");
    fprintf(file, "  \"channel\": \"CH3\",\n");
    fprintf(file, "  \"tSensor\": 1,\n");
    fprintf(file, "  \"iGain\": %d,\n", calibration->gain_index);
    fprintf(file, "  \"gain_nom\": %d,\n", ENCODER_GAIN_NOMINAL);
    fprintf(file, "  \"iLPF\": %d,\n", calibration->lpf_index);
    fprintf(
        file,
        "  \"iSensPwr\": %d,\n",
        calibration->sensor_power_index
    );
    fprintf(
        file,
        "  \"fInputDCImp\": %d,\n",
        calibration->input_dc_impedance
    );
    fprintf(
        file,
        "  \"fInputACImp\": %d,\n",
        calibration->input_ac_impedance
    );
    fprintf(file, "  \"points\": [\n");
    fprintf(
        file,
        "    {\"raw\": %.12g, \"ref\": %.12g, \"samples\": %lld},\n",
        raw1,
        reference1_ma,
        samples1
    );
    fprintf(
        file,
        "    {\"raw\": %.12g, \"ref\": %.12g, \"samples\": %lld}\n",
        raw2,
        reference2_ma,
        samples2
    );
    fprintf(file, "  ],\n");
    fprintf(file, "  \"fit\": {\n");
    fprintf(
        file,
        "    \"slope\": %.15g,\n",
        calibration->slope_ma_per_count
    );
    fprintf(
        file,
        "    \"intercept\": %.15g\n",
        calibration->intercept_ma
    );
    fprintf(file, "  }\n");
    fprintf(file, "}\n");

    return commit_atomic_output(
        file,
        temporary_path,
        path
    );
}

static int build_auto_cal_sidecar_path(
    const char *json_path,
    const char *suffix,
    char *output_path,
    size_t output_path_size)
{
    const char *last_separator;
    const char *last_separator_alt;
    const char *extension;
    size_t stem_length;
    int written;

    if (!json_path || !*json_path ||
        !suffix || !*suffix ||
        !output_path || output_path_size == 0) {
        return 0;
    }
    last_separator = strrchr(json_path, '\\');
    last_separator_alt = strrchr(json_path, '/');
    if (!last_separator ||
        (last_separator_alt &&
         last_separator_alt > last_separator)) {
        last_separator = last_separator_alt;
    }
    extension = strrchr(json_path, '.');
    if (extension &&
        last_separator &&
        extension < last_separator) {
        extension = NULL;
    }
    stem_length = extension
        ? (size_t)(extension - json_path)
        : strlen(json_path);
    if (stem_length > (size_t)INT_MAX) {
        return 0;
    }
    written = _snprintf_s(
        output_path,
        output_path_size,
        _TRUNCATE,
        "%.*s%s",
        (int)stem_length,
        json_path,
        suffix
    );
    return written >= 0 &&
           (size_t)written < output_path_size;
}

static int valid_drive_port(const char *port)
{
    const char *cursor;
    long number;
    char *end = NULL;

    if (!port || _strnicmp(port, "COM", 3) != 0) {
        return 0;
    }
    cursor = port + 3;
    if (!isdigit((unsigned char)*cursor)) {
        return 0;
    }
    number = strtol(cursor, &end, 10);
    return end && *end == '\0' &&
           number >= 1 && number <= 256;
}

static int compute_drive_command_rpm(
    double encoder_target_rpm,
    double mechanical_ratio,
    int direction,
    int *drive_rpm)
{
    double magnitude_exact;
    int magnitude;

    if (!drive_rpm ||
        !(encoder_target_rpm > 0.0 &&
          encoder_target_rpm <= 1000.0) ||
        !(mechanical_ratio > 0.0 &&
          mechanical_ratio <= 1000.0) ||
        (direction != 1 && direction != -1)) {
        return 0;
    }
    magnitude_exact = encoder_target_rpm * mechanical_ratio;
    if (!(magnitude_exact >= 0.5 &&
          magnitude_exact <= 32767.0)) {
        return 0;
    }
    magnitude = (int)(magnitude_exact + 0.5);
    if (magnitude < 1 || magnitude > 32767) {
        return 0;
    }
    *drive_rpm = direction * magnitude;
    return 1;
}

static int write_drive_schedule(
    const char *path,
    int drive_rpm,
    int duration_s)
{
    FILE *file = NULL;
    int ok;
    int close_result;

    if (!path || !*path || duration_s <= 0 ||
        !create_parent_directories(path) ||
        fopen_s(&file, path, "wb") != 0 ||
        !file) {
        return 0;
    }
    fprintf(file, "rpm,duration_s\n");
    fprintf(file, "%d,%d\n", drive_rpm, duration_s);
    ok = !ferror(file);
    close_result = fclose(file);
    if (close_result != 0) {
        ok = 0;
    }
    return ok;
}

static void drive_session_init(DriveSession *session)
{
    if (!session) {
        return;
    }
    memset(session, 0, sizeof(*session));
    session->stdin_write = INVALID_HANDLE_VALUE;
    session->stdout_read = INVALID_HANDLE_VALUE;
    session->process.hProcess = INVALID_HANDLE_VALUE;
    session->process.hThread = INVALID_HANDLE_VALUE;
}

static void drive_session_observe_line(
    DriveSession *session,
    const char *line)
{
    int comm_active;
    int command_rpm;
    unsigned int position_p0b09;
    long long unwrapped_counts;
    double motor_turns;
    int position_errors;
    unsigned long long last_position_age_ms;

    if (!session || !line || !*line) {
        return;
    }
    if (_strnicmp(line, "STATUS_DRIVE ", 13) == 0) {
        if (sscanf_s(
                line,
                "STATUS_DRIVE comm_active=%d cmd_rpm=%d "
                "pos_p0b09=%u unwrapped_counts=%lld "
                "motor_turns=%lf errors_total=%d last_age_ms=%llu",
                &comm_active,
                &command_rpm,
                &position_p0b09,
                &unwrapped_counts,
                &motor_turns,
                &position_errors,
                &last_position_age_ms
            ) == 7) {
            session->status_seen = 1;
            session->comm_active = comm_active != 0;
            session->command_rpm = command_rpm;
            session->position_p0b09 = position_p0b09;
            session->unwrapped_counts = unwrapped_counts;
            session->motor_turns = motor_turns;
            session->position_errors = position_errors;
            session->last_position_age_ms =
                last_position_age_ms;
            session->status_received_ms = GetTickCount64();
        } else if (!session->status_parse_warning_seen) {
            finish_status_line();
            fprintf(
                stderr,
                "[Drive] STATUS_DRIVE invalido; "
                "aguardando o timeout de seguranca.\n"
            );
            session->status_parse_warning_seen = 1;
        }
        return;
    }
    if (_stricmp(line, "READY") == 0) {
        session->ready_seen = 1;
        return;
    } else if (_stricmp(line, "STARTED") == 0) {
        session->started_seen = 1;
        session->started_received_ms = GetTickCount64();
        return;
    } else if (_stricmp(line, "STOPPED") == 0) {
        session->stopped_seen = 1;
        return;
    }
    if (_strnicmp(line, "Setup speed mode", 16) == 0 ||
        ((_strnicmp(line, "WRITE ", 6) == 0 ||
          _strnicmp(line, "READBACK ", 9) == 0) &&
         strstr(line, "-> OK") != NULL)) {
        return;
    }
    finish_status_line();
    printf("[Drive] %s\n", line);
}

static int drive_session_pump_output(DriveSession *session)
{
    char buffer[512];
    DWORD available = 0;

    if (!session ||
        session->stdout_read == INVALID_HANDLE_VALUE) {
        return 0;
    }
    for (;;) {
        DWORD read_count = 0;
        DWORD to_read;
        size_t i;

        if (!PeekNamedPipe(
                session->stdout_read,
                NULL,
                0,
                NULL,
                &available,
                NULL)) {
            DWORD error = GetLastError();
            if (error == ERROR_BROKEN_PIPE) {
                return session->stopped_seen ||
                       !drive_session_is_running(session);
            }
            return 0;
        }
        if (available == 0) {
            return 1;
        }
        to_read = available;
        if (to_read > sizeof(buffer)) {
            to_read = (DWORD)sizeof(buffer);
        }
        if (!ReadFile(
                session->stdout_read,
                buffer,
                to_read,
                &read_count,
                NULL)) {
            DWORD error = GetLastError();
            if (error == ERROR_BROKEN_PIPE) {
                return session->stopped_seen ||
                       !drive_session_is_running(session);
            }
            return 0;
        }
        if (read_count == 0) {
            return 1;
        }
        for (i = 0; i < read_count; ++i) {
            char character = buffer[i];

            if (character == '\n') {
                while (session->line_length > 0 &&
                       session->line[
                           session->line_length - 1
                       ] == '\r') {
                    --session->line_length;
                }
                session->line[session->line_length] = '\0';
                drive_session_observe_line(
                    session,
                    session->line
                );
                session->line_length = 0;
            } else if (session->line_length + 1 <
                       sizeof(session->line)) {
                session->line[session->line_length++] =
                    character;
            }
        }
    }
}

static int drive_session_is_running(
    const DriveSession *session)
{
    DWORD exit_code = 0;

    if (!session || !session->launched ||
        session->process.hProcess == INVALID_HANDLE_VALUE ||
        !GetExitCodeProcess(
            session->process.hProcess,
            &exit_code)) {
        return 0;
    }
    return exit_code == STILL_ACTIVE;
}

static int drive_session_wait_flag(
    DriveSession *session,
    const int *flag,
    DWORD timeout_ms)
{
    ULONGLONG deadline = GetTickCount64() + timeout_ms;

    while (GetTickCount64() < deadline) {
        if (!drive_session_pump_output(session)) {
            return 0;
        }
        if (*flag) {
            return 1;
        }
        if (!drive_session_is_running(session)) {
            (void)drive_session_pump_output(session);
            return *flag != 0;
        }
        if (stop_requested()) {
            return 0;
        }
        Sleep(20);
    }
    (void)drive_session_pump_output(session);
    return *flag != 0;
}

static int drive_session_send(
    DriveSession *session,
    const char *command)
{
    char line[64];
    DWORD written = 0;
    int length;

    if (!session || !command ||
        session->stdin_write == INVALID_HANDLE_VALUE) {
        return 0;
    }
    length = _snprintf_s(
        line,
        sizeof(line),
        _TRUNCATE,
        "%s\n",
        command
    );
    if (length <= 0 || length >= (int)sizeof(line) ||
        !WriteFile(
            session->stdin_write,
            line,
            (DWORD)length,
            &written,
            NULL) ||
        written != (DWORD)length) {
        return 0;
    }
    return 1;
}

static void drive_session_close_handles(
    DriveSession *session)
{
    if (!session) {
        return;
    }
    if (session->stdin_write != INVALID_HANDLE_VALUE) {
        CloseHandle(session->stdin_write);
        session->stdin_write = INVALID_HANDLE_VALUE;
    }
    if (session->stdout_read != INVALID_HANDLE_VALUE) {
        CloseHandle(session->stdout_read);
        session->stdout_read = INVALID_HANDLE_VALUE;
    }
    if (session->process.hThread != INVALID_HANDLE_VALUE) {
        CloseHandle(session->process.hThread);
        session->process.hThread = INVALID_HANDLE_VALUE;
    }
    if (session->process.hProcess != INVALID_HANDLE_VALUE) {
        CloseHandle(session->process.hProcess);
        session->process.hProcess = INVALID_HANDLE_VALUE;
    }
}

static int drive_session_launch(
    const AppConfig *config,
    int timeout_ms,
    DriveSession *session,
    AutoCalMotion *motion)
{
    SECURITY_ATTRIBUTES security;
    STARTUPINFOA startup;
    HANDLE child_stdout_write = INVALID_HANDLE_VALUE;
    HANDLE child_stdin_read = INVALID_HANDLE_VALUE;
    char command_line[4096];
    int drive_rpm = 0;
    int duration_s =
        timeout_ms / 1000 + DRIVE_DURATION_MARGIN_S;
    int command_length;

    if (!config || !session || !motion ||
        !path_is_file(config->drive_exe) ||
        !valid_drive_port(config->drive_port) ||
        !compute_drive_command_rpm(
            config->encoder_target_rpm,
            config->mechanical_ratio,
            config->drive_direction,
            &drive_rpm)) {
        return 0;
    }
    if (!build_auto_cal_sidecar_path(
            config->calib_out_path,
            "_autocal_drive\\drive.csv",
            session->drive_csv,
            sizeof(session->drive_csv)) ||
        !build_auto_cal_sidecar_path(
            config->calib_out_path,
            "_autocal_drive\\schedule.csv",
            session->schedule_csv,
            sizeof(session->schedule_csv)) ||
        !write_drive_schedule(
            session->schedule_csv,
            drive_rpm,
            duration_s)) {
        return 0;
    }

    command_length = _snprintf_s(
        command_line,
        sizeof(command_line),
        _TRUNCATE,
        "\"%s\" --port %s --out \"%s\" "
        "--schedule \"%s\" --duration %d --rate %.3f "
        "--slave %d --baud %d --parity %c --setup --ipc "
        "--encoder-calibration",
        config->drive_exe,
        config->drive_port,
        session->drive_csv,
        session->schedule_csv,
        duration_s,
        DRIVE_LOG_RATE_HZ,
        DEFAULT_DRIVE_SLAVE,
        DEFAULT_DRIVE_BAUD,
        DEFAULT_DRIVE_PARITY
    );
    if (command_length <= 0 ||
        command_length >= (int)sizeof(command_line)) {
        return 0;
    }

    memset(&security, 0, sizeof(security));
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    if (!CreatePipe(
            &session->stdout_read,
            &child_stdout_write,
            &security,
            0) ||
        !SetHandleInformation(
            session->stdout_read,
            HANDLE_FLAG_INHERIT,
            0) ||
        !CreatePipe(
            &child_stdin_read,
            &session->stdin_write,
            &security,
            0) ||
        !SetHandleInformation(
            session->stdin_write,
            HANDLE_FLAG_INHERIT,
            0)) {
        if (child_stdout_write != INVALID_HANDLE_VALUE) {
            CloseHandle(child_stdout_write);
        }
        if (child_stdin_read != INVALID_HANDLE_VALUE) {
            CloseHandle(child_stdin_read);
        }
        drive_session_close_handles(session);
        return 0;
    }

    memset(&startup, 0, sizeof(startup));
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = child_stdin_read;
    startup.hStdOutput = child_stdout_write;
    startup.hStdError = child_stdout_write;
    memset(&session->process, 0, sizeof(session->process));
    if (!CreateProcessA(
            config->drive_exe,
            command_line,
            NULL,
            NULL,
            TRUE,
            CREATE_NO_WINDOW,
            NULL,
            NULL,
            &startup,
            &session->process)) {
        CloseHandle(child_stdout_write);
        CloseHandle(child_stdin_read);
        drive_session_close_handles(session);
        return 0;
    }
    CloseHandle(child_stdout_write);
    CloseHandle(child_stdin_read);
    session->launched = 1;

    memset(motion, 0, sizeof(*motion));
    motion->enabled = 1;
    motion->drive_command_rpm = drive_rpm;
    motion->encoder_target_rpm =
        config->encoder_target_rpm;
    motion->mechanical_ratio =
        config->mechanical_ratio;
    strncpy_s(
        motion->drive_port,
        sizeof(motion->drive_port),
        config->drive_port,
        _TRUNCATE
    );
    strncpy_s(
        motion->drive_csv,
        sizeof(motion->drive_csv),
        session->drive_csv,
        _TRUNCATE
    );

    if (!drive_session_wait_flag(
            session,
            &session->ready_seen,
            DRIVE_READY_TIMEOUT_MS)) {
        return 0;
    }
    return 1;
}

static int drive_session_start(DriveSession *session)
{
    if (!session || !session->ready_seen ||
        !drive_session_send(session, "START") ||
        !drive_session_wait_flag(
            session,
            &session->started_seen,
            DRIVE_START_TIMEOUT_MS)) {
        return 0;
    }
    session->started = 1;
    return 1;
}

static int drive_session_stop(DriveSession *session)
{
    int stopped_and_exited = 0;
    ULONGLONG deadline;

    if (!session || !session->launched) {
        return 1;
    }
    if (!session->stop_sent) {
        session->stop_sent =
            drive_session_send(session, "STOP");
    }
    if (session->stdin_write != INVALID_HANDLE_VALUE) {
        CloseHandle(session->stdin_write);
        session->stdin_write = INVALID_HANDLE_VALUE;
    }

    deadline = GetTickCount64() + DRIVE_STOP_TIMEOUT_MS;
    while (GetTickCount64() < deadline) {
        (void)drive_session_pump_output(session);
        if (!drive_session_is_running(session)) {
            (void)drive_session_pump_output(session);
            stopped_and_exited =
                session->stopped_seen != 0;
            break;
        }
        Sleep(20);
    }
    if (!stopped_and_exited) {
        finish_status_line();
        fprintf(
            stderr,
            "ATENCAO: o Drive nao confirmou STOPPED em %d ms. "
            "Nao foi usado TerminateProcess; acione a parada fisica "
            "se o eixo ainda estiver em movimento.\n",
            DRIVE_STOP_TIMEOUT_MS
        );
    }
    drive_session_close_handles(session);
    session->launched = 0;
    return stopped_and_exited;
}

static int write_auto_calibration_file(
    const char *path,
    const AppConfig *config,
    const AutoCalDetector *detector,
    double raw_low,
    double raw_high,
    double maximum_relative_deviation,
    long long received_samples,
    unsigned long frame_gap_events,
    unsigned long tolerated_gap_events,
    unsigned long reset_gap_events,
    unsigned long long lost_packets,
    unsigned long reordered_packets,
    const AutoCalMotion *motion,
    const Calibration *calibration)
{
    FILE *file = NULL;
    char temporary_path[MAX_PATH];
    const char *wrap_direction =
        detector->direction_sign > 0
            ? "low_to_high"
            : "high_to_low";
    int low_sample_total = 0;
    int high_sample_total = 0;
    double loss_fraction =
        received_samples > 0 || lost_packets > 0
            ? (double)lost_packets /
                ((double)received_samples +
                 (double)lost_packets)
            : 0.0;
    int i;

    for (i = 0; i < AUTO_CALIB_TRANSITIONS; ++i) {
        low_sample_total +=
            detector->endpoint_low_samples[i];
        high_sample_total +=
            detector->endpoint_high_samples[i];
    }

    if (!open_atomic_output(
            path,
            temporary_path,
            sizeof(temporary_path),
            &file)) {
        return 0;
    }

    fprintf(file, "{\n");
    fprintf(file, "  \"purpose\": \"encoder_ch3_current_ma\",\n");
    fprintf(file, "  \"unit\": \"mA\",\n");
    fprintf(file, "  \"encoder_model\": \"%s\",\n", ENCODER_MODEL);
    fprintf(
        file,
        "  \"calibration_kind\": "
        "\"nominal_wrap_normalization\",\n"
    );
    fprintf(file, "  \"metrological\": false,\n");
    fprintf(file, "  \"traceable_reference\": false,\n");
    fprintf(file, "  \"nominal_endpoints\": true,\n");
    fprintf(file, "  \"channels\": [3],\n");
    fprintf(file, "  \"channel\": \"CH3\",\n");
    fprintf(file, "  \"tSensor\": 1,\n");
    fprintf(file, "  \"iGain\": %d,\n", calibration->gain_index);
    fprintf(file, "  \"gain_nom\": %d,\n", ENCODER_GAIN_NOMINAL);
    fprintf(file, "  \"iLPF\": %d,\n", calibration->lpf_index);
    fprintf(
        file,
        "  \"iSensPwr\": %d,\n",
        calibration->sensor_power_index
    );
    fprintf(
        file,
        "  \"fInputDCImp\": %d,\n",
        calibration->input_dc_impedance
    );
    fprintf(
        file,
        "  \"fInputACImp\": %d,\n",
        calibration->input_ac_impedance
    );
    fprintf(
        file,
        "  \"sample_rate_hz\": %d,\n",
        config->sample_rate_hz
    );
    if (motion && motion->enabled) {
        fprintf(file, "  \"motion_control\": {\n");
        fprintf(file, "    \"enabled\": true,\n");
        fprintf(
            file,
            "    \"controller\": \"a5_speed_logger_ipc\",\n"
        );
        fprintf(
            file,
            "    \"position_reference\": \"P0B-09\",\n"
        );
        fprintf(
            file,
            "    \"drive_port\": \"%s\",\n",
            motion->drive_port
        );
        fprintf(
            file,
            "    \"mechanical_ratio_i_D2_D1\": %.12g,\n",
            motion->mechanical_ratio
        );
        fprintf(
            file,
            "    \"encoder_target_rpm\": %.12g,\n",
            motion->encoder_target_rpm
        );
        fprintf(
            file,
            "    \"drive_command_rpm\": %d\n",
            motion->drive_command_rpm
        );
        fprintf(file, "  },\n");
    } else {
        fprintf(
            file,
            "  \"motion_control\": {\"enabled\": false},\n"
        );
    }
    fprintf(
        file,
        "  \"wraps_required\": %d,\n",
        AUTO_CALIB_TRANSITIONS
    );
    fprintf(
        file,
        "  \"wraps_detected\": %d,\n",
        detector->transition_count
    );
    fprintf(
        file,
        "  \"wrap_direction\": \"%s\",\n",
        wrap_direction
    );
    fprintf(file, "  \"detector\": {\n");
    fprintf(
        file,
        "    \"method\": \"persistent_window_median\",\n"
    );
    fprintf(
        file,
        "    \"transition_origin\": \"center_of_guard_window\",\n"
    );
    fprintf(
        file,
        "    \"transition_origin_offset_samples\": %d,\n",
        AUTO_CALIB_TRANSITION_ORIGIN_OFFSET
    );
    fprintf(
        file,
        "    \"filter_samples\": %d,\n",
        AUTO_CALIB_FILTER_SAMPLES
    );
    fprintf(
        file,
        "    \"pre_samples\": %d,\n",
        AUTO_CALIB_PRE_SAMPLES
    );
    fprintf(
        file,
        "    \"guard_samples\": %d,\n",
        AUTO_CALIB_GUARD_SAMPLES
    );
    fprintf(
        file,
        "    \"post_samples\": %d,\n",
        AUTO_CALIB_POST_SAMPLES
    );
    fprintf(
        file,
        "    \"threshold_fraction\": %.12g,\n",
        AUTO_CALIB_WINDOW_THRESHOLD_FRACTION
    );
    fprintf(
        file,
        "    \"minimum_jump_counts\": %.12g,\n",
        AUTO_CALIB_MIN_JUMP
    );
    fprintf(
        file,
        "    \"minimum_window_support\": %d,\n",
        AUTO_CALIB_WINDOW_MAJORITY
    );
    fprintf(
        file,
        "    \"maximum_mad_fraction\": %.12g,\n",
        AUTO_CALIB_WINDOW_MAD_FRACTION
    );
    fprintf(
        file,
        "    \"confirm_samples\": %d,\n",
        AUTO_CALIB_CONFIRM_SAMPLES
    );
    fprintf(
        file,
        "    \"minimum_confirm_support\": %d,\n",
        AUTO_CALIB_CONFIRM_MAJORITY
    );
    fprintf(
        file,
        "    \"range_method\": "
        "\"persistent_post_center_outside_jumps\",\n"
    );
    fprintf(
        file,
        "    \"endpoint_method\": "
        "\"%s\",\n",
        detector->endpoints_refined
            ? "trimmed_mean_10pct_100_350ms_plateaus"
            : "median_of_persistent_pre_post_plateaus"
    );
    fprintf(
        file,
        "    \"endpoint_near_ms\": %d,\n",
        AUTO_CALIB_ENDPOINT_NEAR_MS
    );
    fprintf(
        file,
        "    \"endpoint_far_ms\": %d,\n",
        AUTO_CALIB_ENDPOINT_FAR_MS
    );
    fprintf(
        file,
        "    \"endpoint_trim_fraction\": %.12g,\n",
        AUTO_CALIB_ENDPOINT_TRIM_FRACTION
    );
    fprintf(
        file,
        "    \"maximum_tolerated_frame_delta\": %u,\n",
        (unsigned)AUTO_CALIB_MAX_TOLERATED_FRAME_DELTA
    );
    fprintf(
        file,
        "    \"range_mad_multiplier\": %.12g,\n",
        AUTO_CALIB_RANGE_MAD_MULTIPLIER
    );
    fprintf(
        file,
        "    \"range_minimum_band_counts\": %.12g,\n",
        AUTO_CALIB_RANGE_MIN_BAND
    );
    fprintf(
        file,
        "    \"candidate_retry_samples\": %d,\n",
        AUTO_CALIB_WINDOW_RETRY_SAMPLES
    );
    fprintf(
        file,
        "    \"rearm_fraction\": %.12g\n",
        AUTO_CALIB_REARM_FRACTION
    );
    fprintf(file, "  },\n");
    fprintf(file, "  \"points\": [\n");
    fprintf(
        file,
        "    {\"raw\": %.12g, \"ref\": 4.0, \"samples\": %d},\n",
        raw_low,
        low_sample_total
    );
    fprintf(
        file,
        "    {\"raw\": %.12g, \"ref\": 20.0, \"samples\": %d}\n",
        raw_high,
        high_sample_total
    );
    fprintf(file, "  ],\n");
    fprintf(file, "  \"transitions\": [\n");
    for (i = 0; i < AUTO_CALIB_TRANSITIONS; ++i) {
        fprintf(
            file,
            "    {\"number\": %d, \"sample_index\": %lld, "
            "\"raw_low\": %.12g, \"raw_high\": %.12g, "
            "\"jump\": %.12g}%s\n",
            i + 1,
            detector->transition_indices[i],
            detector->raw_lows[i],
            detector->raw_highs[i],
            detector->jumps[i],
            i + 1 < AUTO_CALIB_TRANSITIONS ? "," : ""
        );
    }
    fprintf(file, "  ],\n");
    fprintf(file, "  \"quality\": {\n");
    fprintf(
        file,
        "    \"received_samples\": %lld,\n",
        received_samples
    );
    fprintf(
        file,
        "    \"maximum_relative_deviation\": %.12g,\n",
        maximum_relative_deviation
    );
    fprintf(
        file,
        "    \"candidates\": %d,\n",
        detector->candidate_count
    );
    fprintf(
        file,
        "    \"window_waits\": %d,\n",
        detector->window_wait_count
    );
    fprintf(
        file,
        "    \"rejected_candidates\": %d,\n",
        detector->rejected_count
    );
    fprintf(
        file,
        "    \"frame_gap_events\": %lu,\n",
        frame_gap_events
    );
    fprintf(
        file,
        "    \"tolerated_gap_events\": %lu,\n",
        tolerated_gap_events
    );
    fprintf(
        file,
        "    \"reset_gap_events\": %lu,\n",
        reset_gap_events
    );
    fprintf(
        file,
        "    \"lost_packets\": %llu,\n",
        lost_packets
    );
    fprintf(
        file,
        "    \"loss_fraction\": %.12g,\n",
        loss_fraction
    );
    fprintf(
        file,
        "    \"reordered_packets\": %lu,\n",
        reordered_packets
    );
    fprintf(
        file,
        "    \"typical_step_counts\": %.12g\n",
        detector->typical_step
    );
    fprintf(file, "  },\n");
    fprintf(file, "  \"fit\": {\n");
    fprintf(
        file,
        "    \"slope\": %.15g,\n",
        calibration->slope_ma_per_count
    );
    fprintf(
        file,
        "    \"intercept\": %.15g\n",
        calibration->intercept_ma
    );
    fprintf(file, "  }\n");
    fprintf(file, "}\n");

    return commit_atomic_output(
        file,
        temporary_path,
        path
    );
}

static int write_angular_calibration_file(
    const char *path,
    const AppConfig *config,
    const AutoCalDetector *detector,
    const HandshakeState *handshake,
    long long received_samples,
    unsigned long frame_gap_events,
    unsigned long tolerated_gap_events,
    unsigned long reset_gap_events,
    unsigned long long lost_packets,
    unsigned long reordered_packets,
    const AutoCalMotion *motion,
    const Calibration *calibration,
    const AngularFit *fit)
{
    FILE *file = NULL;
    char temporary_path[MAX_PATH];
    double loss_fraction =
        received_samples > 0 || lost_packets > 0
            ? (double)lost_packets /
                ((double)received_samples +
                 (double)lost_packets)
            : 0.0;
    int i;

    if (!path || !config || !detector || !handshake || !motion ||
        !calibration || !fit ||
        !angular_fit_passes_quality(fit) ||
        !open_atomic_output(
            path,
            temporary_path,
            sizeof(temporary_path),
            &file)) {
        return 0;
    }

    fprintf(file, "{\n");
    fprintf(file, "  \"purpose\": \"encoder_ch3_angle_deg\",\n");
    fprintf(file, "  \"unit\": \"deg\",\n");
    fprintf(file, "  \"encoder_model\": \"%s\",\n", ENCODER_MODEL);
    fprintf(
        file,
        "  \"calibration_kind\": "
        "\"drive_referenced_robust_linear_angle\",\n"
    );
    fprintf(file, "  \"metrological\": false,\n");
    fprintf(file, "  \"traceable_reference\": false,\n");
    fprintf(file, "  \"channels\": [3],\n");
    fprintf(file, "  \"channel\": \"CH3\",\n");
    fprintf(file, "  \"tSensor\": %d,\n", SENSOR_CURRENT);
    fprintf(file, "  \"iGain\": %d,\n", calibration->gain_index);
    fprintf(file, "  \"gain_nom\": %d,\n", ENCODER_GAIN_NOMINAL);
    fprintf(file, "  \"iLPF\": %d,\n", calibration->lpf_index);
    fprintf(file, "  \"software_lpf_enabled\": false,\n");
    fprintf(
        file,
        "  \"iSensPwr\": %d,\n",
        calibration->sensor_power_index
    );
    fprintf(
        file,
        "  \"fInputDCImp\": %d,\n",
        calibration->input_dc_impedance
    );
    fprintf(
        file,
        "  \"fInputACImp\": %d,\n",
        calibration->input_ac_impedance
    );
    fprintf(file, "  \"balance\": 0,\n");
    fprintf(file, "  \"use_balance\": false,\n");
    fprintf(
        file,
        "  \"configuration_readback_verified\": %s,\n",
        handshake->saw_get_channel_response &&
        handshake->get_channel_matches &&
        !handshake->fallback_used
            ? "true"
            : "false"
    );
    fprintf(
        file,
        "  \"configuration_validation_mode\": \"%s\",\n",
        handshake->fallback_used
            ? "FALLBACK_ACQDATA"
            : "STRICT_READBACK"
    );
    fprintf(
        file,
        "  \"sample_rate_hz\": %d,\n",
        config->sample_rate_hz
    );
    fprintf(file, "  \"output_range_deg\": [0.0, 360.0],\n");
    fprintf(
        file,
        "  \"zero_reference\": "
        "\"electrical_current_minimum\",\n"
    );
    /*
     * Keep the operational fit first: the lightweight loader intentionally
     * searches the first exact "slope"/"intercept" pair.
     */
    fprintf(file, "  \"fit\": {\n");
    fprintf(
        file,
        "    \"model\": \"degrees_equals_slope_raw_plus_intercept\",\n"
    );
    fprintf(
        file,
        "    \"slope\": %.15g,\n",
        fit->slope_deg_per_count
    );
    fprintf(
        file,
        "    \"intercept\": %.15g,\n",
        fit->intercept_deg
    );
    fprintf(file, "    \"normalize_modulo_deg\": 360.0\n");
    fprintf(file, "  },\n");
    fprintf(file, "  \"reference\": {\n");
    fprintf(file, "    \"source\": \"DriveA5_P0B-09\",\n");
    fprintf(
        file,
        "    \"alignment\": "
        "\"QPC_gap_aware_linear_interpolation\",\n"
    );
    fprintf(
        file,
        "    \"position_filter\": "
        "\"modular_unwrap_physical_speed_gate\",\n"
    );
    fprintf(
        file,
        "    \"maximum_interpolation_gap_s\": %.12g,\n",
        ANGULAR_MAX_DRIVE_GAP_S
    );
    fprintf(
        file,
        "    \"physical_speed_margin\": %.12g,\n",
        ANGULAR_DRIVE_SPEED_MARGIN
    );
    fprintf(
        file,
        "    \"physical_jitter_allowance_counts\": %.12g,\n",
        ANGULAR_DRIVE_JITTER_COUNTS
    );
    fprintf(
        file,
        "    \"position_modulus\": %u,\n",
        fit->drive_position_modulus
    );
    fprintf(
        file,
        "    \"rows_total\": %lu,\n",
        fit->drive_rows_total
    );
    fprintf(
        file,
        "    \"valid_samples\": %lu,\n",
        fit->drive_valid_samples
    );
    fprintf(
        file,
        "    \"missing_rows\": %lu,\n",
        fit->drive_missing_rows
    );
    fprintf(
        file,
        "    \"invalid_rows\": %lu,\n",
        fit->drive_invalid_rows
    );
    fprintf(
        file,
        "    \"outlier_rows\": %lu,\n",
        fit->drive_outlier_rows
    );
    fprintf(
        file,
        "    \"maximum_observed_valid_gap_s\": %.12g,\n",
        fit->drive_max_valid_gap_s
    );
    fprintf(
        file,
        "    \"mechanical_ratio_i_D2_D1_configured\": %.12g,\n",
        motion->mechanical_ratio
    );
    fprintf(
        file,
        "    \"mechanical_ratio_measured_mean\": %.12g,\n",
        fit->measured_ratio_mean
    );
    fprintf(file, "    \"mechanical_ratio_per_revolution\": [");
    for (i = 0; i < ANGULAR_COMPLETE_REVOLUTIONS; ++i) {
        fprintf(
            file,
            "%s%.12g",
            i == 0 ? "" : ", ",
            fit->ratio_per_revolution[i]
        );
    }
    fprintf(file, "]\n");
    fprintf(file, "  },\n");
    fprintf(file, "  \"acquisition\": {\n");
    fprintf(
        file,
        "    \"drive_port\": \"%s\",\n",
        motion->drive_port
    );
    fprintf(
        file,
        "    \"encoder_target_rpm\": %.12g,\n",
        motion->encoder_target_rpm
    );
    fprintf(
        file,
        "    \"drive_command_rpm\": %d,\n",
        motion->drive_command_rpm
    );
    fprintf(
        file,
        "    \"wraps_required\": %d,\n",
        AUTO_CALIB_TRANSITIONS
    );
    fprintf(
        file,
        "    \"complete_revolutions\": %d,\n",
        ANGULAR_COMPLETE_REVOLUTIONS
    );
    fprintf(file, "    \"training_revolutions\": 2,\n");
    fprintf(file, "    \"holdout_revolutions\": 1,\n");
    fprintf(file, "    \"bin_width_deg\": 1.0,\n");
    fprintf(
        file,
        "    \"edge_exclusion_deg\": %d,\n",
        ANGULAR_EDGE_EXCLUSION_DEG
    );
    fprintf(
        file,
        "    \"minimum_samples_per_bin\": %d\n",
        ANGULAR_MIN_BIN_SAMPLES
    );
    fprintf(file, "  },\n");
    fprintf(file, "  \"transitions\": [\n");
    for (i = 0; i < AUTO_CALIB_TRANSITIONS; ++i) {
        fprintf(
            file,
            "    {\"number\": %d, \"sample_index\": %lld, "
            "\"jump\": %.12g}%s\n",
            i + 1,
            detector->transition_indices[i],
            detector->jumps[i],
            i + 1 < AUTO_CALIB_TRANSITIONS ? "," : ""
        );
    }
    fprintf(file, "  ],\n");
    fprintf(file, "  \"quality\": {\n");
    fprintf(file, "    \"accepted\": true,\n");
    fprintf(
        file,
        "    \"training_slope\": %.15g,\n",
        fit->training_slope_deg_per_count
    );
    fprintf(
        file,
        "    \"training_intercept\": %.15g,\n",
        fit->training_intercept_deg
    );
    fprintf(
        file,
        "    \"training_bins\": %d,\n",
        fit->training_bins
    );
    fprintf(
        file,
        "    \"training_rmse_deg\": %.12g,\n",
        fit->training_rmse_deg
    );
    fprintf(
        file,
        "    \"operational_all_revolutions_rmse_deg\": %.12g,\n",
        fit->operational_rmse_deg
    );
    fprintf(
        file,
        "    \"holdout_bins\": %d,\n",
        fit->validation_bins
    );
    fprintf(
        file,
        "    \"holdout_bin_rmse_deg\": %.12g,\n",
        fit->validation_rmse_deg
    );
    fprintf(
        file,
        "    \"holdout_bin_p95_deg\": %.12g,\n",
        fit->validation_p95_deg
    );
    fprintf(
        file,
        "    \"holdout_bin_max_error_deg\": %.12g,\n",
        fit->validation_max_error_deg
    );
    fprintf(
        file,
        "    \"holdout_filtered_samples\": %d,\n",
        fit->validation_filtered_samples
    );
    fprintf(
        file,
        "    \"holdout_filtered_rmse_deg\": %.12g,\n",
        fit->validation_filtered_rmse_deg
    );
    fprintf(
        file,
        "    \"holdout_filtered_p95_deg\": %.12g,\n",
        fit->validation_filtered_p95_deg
    );
    fprintf(
        file,
        "    \"holdout_filtered_max_error_deg\": %.12g,\n",
        fit->validation_filtered_max_error_deg
    );
    fprintf(
        file,
        "    \"holdout_r_squared_linear\": %.12g,\n",
        fit->r_squared
    );
    fprintf(
        file,
        "    \"maximum_ratio_error_fraction\": %.12g,\n",
        fit->measured_ratio_max_error_fraction
    );
    fprintf(
        file,
        "    \"raw_min\": %.12g,\n",
        fit->raw_min
    );
    fprintf(
        file,
        "    \"raw_max\": %.12g,\n",
        fit->raw_max
    );
    fprintf(
        file,
        "    \"adc_saturation_samples\": %d,\n",
        fit->saturation_samples
    );
    fprintf(
        file,
        "    \"received_samples\": %lld,\n",
        received_samples
    );
    fprintf(
        file,
        "    \"frame_gap_events\": %lu,\n",
        frame_gap_events
    );
    fprintf(
        file,
        "    \"tolerated_gap_events\": %lu,\n",
        tolerated_gap_events
    );
    fprintf(
        file,
        "    \"reset_gap_events\": %lu,\n",
        reset_gap_events
    );
    fprintf(
        file,
        "    \"lost_packets\": %llu,\n",
        lost_packets
    );
    fprintf(
        file,
        "    \"loss_fraction\": %.12g,\n",
        loss_fraction
    );
    fprintf(
        file,
        "    \"reordered_packets\": %lu,\n",
        reordered_packets
    );
    fprintf(file, "    \"acceptance_limits\": {\n");
    fprintf(
        file,
        "      \"rmse_deg\": %.12g,\n",
        ANGULAR_MAX_RMSE_DEG
    );
    fprintf(
        file,
        "      \"p95_deg\": %.12g,\n",
        ANGULAR_MAX_P95_DEG
    );
    fprintf(
        file,
        "      \"max_error_deg\": %.12g,\n",
        ANGULAR_MAX_ERROR_DEG
    );
    fprintf(
        file,
        "      \"mechanical_ratio_error_fraction\": %.12g\n",
        ANGULAR_MAX_RATIO_ERROR_FRACTION
    );
    fprintf(file, "    }\n");
    fprintf(file, "  }\n");
    fprintf(file, "}\n");

    return commit_atomic_output(
        file,
        temporary_path,
        path
    );
}

static int send_packet(
    SOCKET socket_handle,
    const struct sockaddr_in *destination,
    const void *packet,
    int packet_size)
{
    int sent = sendto(
        socket_handle,
        (const char *)packet,
        packet_size,
        0,
        (const struct sockaddr *)destination,
        sizeof(*destination)
    );
    return sent == packet_size;
}

static int send_stop(DlgConnection *connection)
{
    PktHdr packet;
    packet.code = OP_ACQSTOP;
    packet.reserved = 0;
    return send_packet(
        connection->socket_handle,
        &connection->dlg_addr,
        &packet,
        (int)sizeof(packet)
    );
}

static int send_channel_config(
    DlgConnection *connection,
    const Calibration *calibration)
{
    PktSetChannel packet;
    memset(&packet, 0, sizeof(packet));
    packet.code = OP_SETCHCFG;
    packet.channel = ENCODER_CHANNEL;
    packet.sensor_type = SENSOR_CURRENT;
    packet.gain_index = (int16_t)calibration->gain_index;
    packet.lpf_index = (int16_t)calibration->lpf_index;
    packet.sensor_power_index =
        (int16_t)calibration->sensor_power_index;
    packet.input_dc_impedance =
        (uint16_t)calibration->input_dc_impedance;
    packet.input_ac =
        (uint16_t)calibration->input_ac_impedance;
    packet.use_balance = 0;
    return send_packet(
        connection->socket_handle,
        &connection->dlg_addr,
        &packet,
        (int)sizeof(packet)
    );
}

static int send_get_channel_config(
    DlgConnection *connection)
{
    PktHdr packet;

    packet.code = OP_GETCHCFG;
    packet.reserved = 0;
    return send_packet(
        connection->socket_handle,
        &connection->dlg_addr,
        &packet,
        (int)sizeof(packet)
    );
}

static int send_acq_setup(
    DlgConnection *connection,
    int sample_rate_hz)
{
    PktAcqSetup packet;
    memset(&packet, 0, sizeof(packet));
    packet.code = OP_ACQSETUP;
    packet.sample_freq = (float)sample_rate_hz;
    packet.bursts = 1;
    packet.n_signals = 1;
    packet.icm[0] = ENCODER_CHANNEL;
    return send_packet(
        connection->socket_handle,
        &connection->dlg_addr,
        &packet,
        (int)sizeof(packet)
    );
}

static int send_acq_start(DlgConnection *connection)
{
    PktHdr packet;
    packet.code = OP_ACQSTART;
    packet.reserved = 0;
    return send_packet(
        connection->socket_handle,
        &connection->dlg_addr,
        &packet,
        (int)sizeof(packet)
    );
}

static int drain_socket(DlgConnection *connection)
{
    DWORD short_timeout = DRAIN_TIMEOUT_MS;
    DWORD normal_timeout = RECV_TIMEOUT_MS;
    ULONGLONG deadline = GetTickCount64() + 200;
    char packet[2048];
    int drained = 0;

    setsockopt(
        connection->socket_handle,
        SOL_SOCKET,
        SO_RCVTIMEO,
        (const char *)&short_timeout,
        sizeof(short_timeout)
    );
    while (GetTickCount64() < deadline && drained < 4096) {
        int received = recvfrom(
            connection->socket_handle,
            packet,
            (int)sizeof(packet),
            0,
            NULL,
            NULL
        );
        if (received <= 0) {
            break;
        }
        ++drained;
    }
    setsockopt(
        connection->socket_handle,
        SOL_SOCKET,
        SO_RCVTIMEO,
        (const char *)&normal_timeout,
        sizeof(normal_timeout)
    );
    return drained;
}

static int open_connection(
    const AppConfig *config,
    DlgConnection *connection)
{
    struct sockaddr_in local_addr;
    DWORD timeout_ms = RECV_TIMEOUT_MS;
    int receive_buffer_bytes = DLG_RECEIVE_BUFFER_BYTES;

    memset(connection, 0, sizeof(*connection));
    connection->socket_handle = INVALID_SOCKET;
    connection->socket_handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (connection->socket_handle == INVALID_SOCKET) {
        fprintf(stderr, "Falha ao criar socket UDP: %d\n", WSAGetLastError());
        return 0;
    }
    if (setsockopt(
            connection->socket_handle,
            SOL_SOCKET,
            SO_RCVBUF,
            (const char *)&receive_buffer_bytes,
            sizeof(receive_buffer_bytes)) != 0) {
        fprintf(
            stderr,
            "Aviso: nao foi possivel ampliar o buffer UDP para %d bytes "
            "(erro %d).\n",
            receive_buffer_bytes,
            WSAGetLastError()
        );
    }

    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_port = htons(config->local_port);
    if (config->local_ip[0] == '\0') {
        local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (inet_pton(
                   AF_INET,
                   config->local_ip,
                   &local_addr.sin_addr) != 1) {
        fprintf(stderr, "IP local invalido: %s\n", config->local_ip);
        closesocket(connection->socket_handle);
        connection->socket_handle = INVALID_SOCKET;
        return 0;
    }

    if (bind(
            connection->socket_handle,
            (const struct sockaddr *)&local_addr,
            sizeof(local_addr)) != 0) {
        int error = WSAGetLastError();
        fprintf(
            stderr,
            "Falha no bind UDP local %s:%u (erro %d).\n",
            config->local_ip[0] ? config->local_ip : "0.0.0.0",
            (unsigned)config->local_port,
            error
        );
        if (error == WSAEADDRINUSE) {
            fprintf(
                stderr,
                "A porta esta em uso. Feche logger, CalibraDLG e supervisorio.\n"
            );
        }
        closesocket(connection->socket_handle);
        connection->socket_handle = INVALID_SOCKET;
        return 0;
    }

    if (setsockopt(
            connection->socket_handle,
            SOL_SOCKET,
            SO_RCVTIMEO,
            (const char *)&timeout_ms,
            sizeof(timeout_ms)) != 0) {
        fprintf(
            stderr,
            "Falha ao configurar timeout UDP: %d\n",
            WSAGetLastError()
        );
        closesocket(connection->socket_handle);
        connection->socket_handle = INVALID_SOCKET;
        return 0;
    }

    memset(&connection->dlg_addr, 0, sizeof(connection->dlg_addr));
    connection->dlg_addr.sin_family = AF_INET;
    connection->dlg_addr.sin_port = htons(config->dlg_port);
    if (inet_pton(
            AF_INET,
            config->dlg_ip,
            &connection->dlg_addr.sin_addr) != 1) {
        fprintf(stderr, "IP do DLG invalido: %s\n", config->dlg_ip);
        closesocket(connection->socket_handle);
        connection->socket_handle = INVALID_SOCKET;
        return 0;
    }
    return 1;
}

static int source_is_dlg(
    const struct sockaddr_in *source,
    const struct sockaddr_in *dlg)
{
    return source->sin_family == AF_INET &&
           source->sin_port == dlg->sin_port &&
           source->sin_addr.s_addr == dlg->sin_addr.s_addr;
}

static void observe_command_response(
    const void *packet_data,
    int packet_bytes,
    HandshakeState *handshake)
{
    const PktHdr *header;
    const PktResponse *response;

    if (!handshake || !packet_data ||
        packet_bytes < (int)sizeof(PktHdr)) {
        return;
    }
    header = (const PktHdr *)packet_data;
    if (header->code != OP_GETCHCFG &&
        header->code != OP_GETCHCFG_R &&
        header->code != OP_SETCHCFG_R &&
        header->code != OP_ACQSETUP_R &&
        header->code != OP_ACQSTART_R) {
        return;
    }
    if (header->code == OP_ACQSETUP_R) {
        handshake->saw_acq_setup_response = 1;
    } else if (header->code == OP_ACQSTART_R) {
        handshake->saw_acq_start_response = 1;
    }

    /*
    The opcode table documents 0x2001 for GETCHCFG_R, while the prose in
    older revisions repeats 0x2000. Accept either only for a full 24-byte
    packet received from the DLG; the outbound request itself is 4 bytes.
    */
    if ((header->code == OP_GETCHCFG_R ||
         header->code == OP_GETCHCFG) &&
        packet_bytes >= (int)sizeof(PktSetChannel)) {
        const PktSetChannel *channel =
            (const PktSetChannel *)packet_data;
        const PktSetChannel *expected =
            &handshake->channel_expected;

        if (channel->channel != expected->channel) {
            return;
        }
        handshake->saw_get_channel_response = 1;
        handshake->get_channel_matches =
            channel->error == 0 &&
            channel->channel == expected->channel &&
            channel->sensor_type == expected->sensor_type &&
            channel->gain_index == expected->gain_index &&
            channel->lpf_index == expected->lpf_index &&
            channel->sensor_power_index ==
                expected->sensor_power_index &&
            channel->balance == expected->balance &&
            channel->input_dc_impedance ==
                expected->input_dc_impedance &&
            channel->input_ac == expected->input_ac &&
            channel->use_balance == expected->use_balance;
        handshake->channel_readback = *channel;
        if (channel->error != 0) {
            handshake->command_rejected = 1;
            handshake->rejected_code = channel->code;
            handshake->error_code = channel->error;
        }
        return;
    }

    /*
    Some older ACQSETUP responses contain only the 4-byte header. Treat a
    present error field as authoritative, but keep short responses compatible.
    */
    if (packet_bytes >= (int)sizeof(PktResponse)) {
        response = (const PktResponse *)packet_data;
        if (response->code == OP_SETCHCFG_R &&
            response->error == 0) {
            handshake->saw_set_channel_ack = 1;
        }
        if (response->error != 0) {
            handshake->command_rejected = 1;
            handshake->rejected_code = response->code;
            handshake->error_code = response->error;
        }
    }
}

/*
Return:
  1 = valid ACQDATA packet
  0 = timeout or unrelated packet
 -1 = socket error
*/
static int receive_data_packet(
    DlgConnection *connection,
    PktData *packet,
    int *packet_bytes,
    HandshakeState *handshake)
{
    struct sockaddr_in source;
    int source_size = (int)sizeof(source);
    int received;

    memset(&source, 0, sizeof(source));
    received = recvfrom(
        connection->socket_handle,
        (char *)packet,
        (int)sizeof(*packet),
        0,
        (struct sockaddr *)&source,
        &source_size
    );
    if (received < 0) {
        int error = WSAGetLastError();
        if (error == WSAETIMEDOUT || error == WSAEWOULDBLOCK) {
            return 0;
        }
        return -1;
    }
    if (!source_is_dlg(&source, &connection->dlg_addr)) {
        return 0;
    }
    observe_command_response(packet, received, handshake);
    if (
        received < (int)offsetof(PktData, samples) +
                   (int)sizeof(packet->samples[0]) ||
        packet->code != OP_ACQDATA ||
        packet->n_signals != 1 ||
        packet->bursts != 1 ||
        !(packet->sample_freq > 0.0f &&
          packet->sample_freq < 1000000.0f) ||
        (received - (int)offsetof(PktData, samples)) %
            (int)sizeof(packet->samples[0]) != 0) {
        return 0;
    }
    *packet_bytes = received;
    return 1;
}

static int add_packet_samples(
    const PktData *packet,
    int packet_bytes,
    double *sum,
    long long *sample_count)
{
    int header_bytes = (int)offsetof(PktData, samples);
    int available = (packet_bytes - header_bytes) /
                    (int)sizeof(packet->samples[0]);
    int expected = packet->n_signals * packet->bursts;
    int i;

    if (expected <= 0 ||
        expected > MAX_PACKET_SAMPLES ||
        available < expected) {
        return 0;
    }
    for (i = 0; i < expected; ++i) {
        *sum += (double)packet->samples[i];
        ++(*sample_count);
    }
    return 1;
}

static int add_packet_samples_to_ring(
    const PktData *packet,
    int packet_bytes,
    int16_t *ring,
    int ring_capacity,
    int *ring_count,
    int *ring_next)
{
    int header_bytes = (int)offsetof(PktData, samples);
    int available = (packet_bytes - header_bytes) /
                    (int)sizeof(packet->samples[0]);
    int expected = packet->n_signals * packet->bursts;
    int i;

    if (!ring || ring_capacity <= 0 ||
        !ring_count || !ring_next ||
        expected <= 0 ||
        expected > MAX_PACKET_SAMPLES ||
        available < expected) {
        return 0;
    }
    for (i = 0; i < expected; ++i) {
        ring[*ring_next] = packet->samples[i];
        *ring_next = (*ring_next + 1) % ring_capacity;
        if (*ring_count < ring_capacity) {
            ++(*ring_count);
        }
    }
    return 1;
}

static void initialize_handshake(
    HandshakeState *handshake,
    const Calibration *calibration,
    unsigned int attempt,
    int fallback_used)
{
    memset(handshake, 0, sizeof(*handshake));
    handshake->channel_expected.channel = ENCODER_CHANNEL;
    handshake->channel_expected.sensor_type = SENSOR_CURRENT;
    handshake->channel_expected.gain_index =
        (int16_t)calibration->gain_index;
    handshake->channel_expected.lpf_index =
        (int16_t)calibration->lpf_index;
    handshake->channel_expected.sensor_power_index =
        (int16_t)calibration->sensor_power_index;
    handshake->channel_expected.balance = 0;
    handshake->channel_expected.input_dc_impedance =
        (uint16_t)calibration->input_dc_impedance;
    handshake->channel_expected.input_ac =
        (uint16_t)calibration->input_ac_impedance;
    handshake->channel_expected.use_balance = 0;
    handshake->attempt = attempt;
    handshake->fallback_used = fallback_used;
}

static int handshake_has_explicit_failure(
    const HandshakeState *handshake)
{
    return handshake->command_rejected ||
           (handshake->saw_get_channel_response &&
            !handshake->get_channel_matches);
}

/*
desired_response:
  0 = collect until timeout
  1 = wait for SETCHCFG_R
  2 = wait for a matching GETCHCFG_R for CH3
*/
static int collect_handshake_responses(
    DlgConnection *connection,
    HandshakeState *handshake,
    DWORD timeout_ms,
    int desired_response)
{
    ULONGLONG deadline = GetTickCount64() + timeout_ms;

    while (!stop_requested() && GetTickCount64() < deadline) {
        PktData packet;
        int packet_bytes = 0;
        int result;

        if (poll_quit_key()) {
            return 0;
        }
        result = receive_data_packet(
            connection,
            &packet,
            &packet_bytes,
            handshake
        );
        if (result < 0) {
            handshake->socket_error = 1;
            return -1;
        }
        if (handshake_has_explicit_failure(handshake)) {
            return -1;
        }
        if (desired_response == 1 &&
            handshake->saw_set_channel_ack) {
            return 1;
        }
        if (desired_response == 2 &&
            handshake->saw_get_channel_response &&
            handshake->get_channel_matches) {
            return 1;
        }
    }
    return 0;
}

static int handshake_data_is_ready(
    const HandshakeState *handshake)
{
    return handshake &&
           !handshake_has_explicit_failure(handshake) &&
           handshake->valid_data_packets >= FIRST_DATA_PACKETS;
}

static int startup_sample_rate_matches(
    float observed_rate_hz,
    int expected_rate_hz)
{
    double tolerance_hz = (double)expected_rate_hz * 0.01;

    if (tolerance_hz < 0.5) {
        tolerance_hz = 0.5;
    }
    return expected_rate_hz > 0 &&
           observed_rate_hz > 0.0f &&
           fabs(
               (double)observed_rate_hz -
               (double)expected_rate_hz
           ) <= tolerance_hz;
}

static void observe_startup_data_packet(
    HandshakeState *handshake,
    const PktData *packet,
    int expected_rate_hz)
{
    uint32_t frame_delta;

    handshake->observed_sample_freq = packet->sample_freq;
    if (!startup_sample_rate_matches(
            packet->sample_freq,
            expected_rate_hz)) {
        ++handshake->data_rate_rejects;
        return;
    }
    if (!handshake->have_last_data_frame) {
        handshake->have_last_data_frame = 1;
        handshake->last_data_frame = packet->frame;
        handshake->valid_data_packets = 1;
        return;
    }

    frame_delta =
        (uint32_t)packet->frame -
        (uint32_t)handshake->last_data_frame;
    if (frame_delta == 0U) {
        ++handshake->data_frame_rejects;
        return;
    }
    if (frame_delta >= 0x80000000U) {
        /*
        Reordered packet or a frame counter restart: use it only as the new
        anchor and require two more forward packets.
        */
        ++handshake->data_frame_rejects;
        handshake->last_data_frame = packet->frame;
        handshake->valid_data_packets = 1;
        return;
    }
    handshake->last_data_frame = packet->frame;
    ++handshake->valid_data_packets;
}

static int wait_for_first_data(
    DlgConnection *connection,
    HandshakeState *handshake,
    int expected_rate_hz)
{
    ULONGLONG deadline = GetTickCount64() + FIRST_DATA_TIMEOUT_MS;

    while (!stop_requested() && GetTickCount64() < deadline) {
        PktData packet;
        int packet_bytes = 0;
        int result;

        if (poll_quit_key()) {
            return 0;
        }
        result = receive_data_packet(
            connection,
            &packet,
            &packet_bytes,
            handshake
        );
        if (handshake_has_explicit_failure(handshake)) {
            return 0;
        }
        if (result > 0) {
            observe_startup_data_packet(
                handshake,
                &packet,
                expected_rate_hz
            );
            if (handshake_data_is_ready(handshake)) {
                return 1;
            }
        } else if (result < 0) {
            handshake->socket_error = 1;
            return 0;
        }
    }
    return 0;
}

static void report_stream_handshake(
    const StreamStartDiagnostics *diagnostics,
    const HandshakeState *handshake,
    const char *result)
{
    if (!diagnostics || !handshake || !result) {
        return;
    }
    if (diagnostics->result) {
        *diagnostics->result = *handshake;
    }
    if (diagnostics->event_log) {
        (void)auto_cal_log_line(
            diagnostics->event_log,
            GetTickCount64() - diagnostics->log_start_ms,
            "DLG_HANDSHAKE",
            "attempt=%u mode=%s result=%s "
            "sent_set=%d get_requests=%d sent_setup=%d sent_start=%d "
            "set_ack=%d get_seen=%d get_match=%d "
            "setup_resp=%d start_resp=%d acq_packets=%d "
            "observed_hz=%.6g rate_rejects=%d frame_rejects=%d "
            "socket_error=%d rejected=0x%04X error=%u "
            "rb_ch=%d rb_sensor=%d rb_gain=%d rb_lpf=%d "
            "rb_power=%d rb_balance=%d rb_dc=%u rb_ac=%u rb_use_bal=%u",
            handshake->attempt,
            handshake->fallback_used ? "FALLBACK" : "STRICT",
            result,
            handshake->sent_set_channel,
            handshake->sent_get_channel,
            handshake->sent_acq_setup,
            handshake->sent_acq_start,
            handshake->saw_set_channel_ack,
            handshake->saw_get_channel_response,
            handshake->get_channel_matches,
            handshake->saw_acq_setup_response,
            handshake->saw_acq_start_response,
            handshake->valid_data_packets,
            (double)handshake->observed_sample_freq,
            handshake->data_rate_rejects,
            handshake->data_frame_rejects,
            handshake->socket_error,
            (unsigned)handshake->rejected_code,
            (unsigned)handshake->error_code,
            (int)handshake->channel_readback.channel,
            (int)handshake->channel_readback.sensor_type,
            (int)handshake->channel_readback.gain_index,
            (int)handshake->channel_readback.lpf_index,
            (int)handshake->channel_readback.sensor_power_index,
            (int)handshake->channel_readback.balance,
            (unsigned)handshake->channel_readback.input_dc_impedance,
            (unsigned)handshake->channel_readback.input_ac,
            (unsigned)handshake->channel_readback.use_balance
        );
    }
}

static void show_handshake_failure(
    const HandshakeState *handshake)
{
    if (handshake->command_rejected) {
        status_line(
            "CH3 | resposta 0x%04X rejeitada (erro %u); tentando...",
            (unsigned)handshake->rejected_code,
            (unsigned)handshake->error_code
        );
    } else if (handshake->saw_get_channel_response &&
               !handshake->get_channel_matches) {
        const PktSetChannel *readback =
            &handshake->channel_readback;
        status_line(
            "CH3 | readback divergiu: G%d LPF%d P%d DC%d AC%d; "
            "tentando...",
            (int)readback->gain_index,
            (int)readback->lpf_index,
            (int)readback->sensor_power_index,
            (int)readback->input_dc_impedance,
            (int)readback->input_ac
        );
    } else if (!handshake->fallback_used &&
               handshake->sent_get_channel > 0 &&
               !handshake->saw_get_channel_response) {
        status_line(
            "CH3 | readback GETCHCFG nao recebido; tentando..."
        );
    } else if (!handshake->sent_acq_start) {
        status_line(
            "CH3 | falha enviando configuracao/comandos; tentando..."
        );
    } else if (handshake->socket_error) {
        status_line("CH3 | erro no socket UDP; tentando...");
    } else {
        status_line(
            "CH3 | sem ACQDATA valido (%d/%d); tentando...",
            handshake->valid_data_packets,
            FIRST_DATA_PACKETS
        );
    }
}

static int start_stream_until_data_diagnostic(
    const AppConfig *config,
    DlgConnection *connection,
    const Calibration *calibration,
    StreamStartDiagnostics *diagnostics)
{
    unsigned int attempt = 0;
    int block_fallback = 0;

    while (!stop_requested()) {
        HandshakeState handshake;
        int strict_mode;
        int get_request;
        int send_failed = 0;
        const char *failure_result = "NO_ACQDATA";

        ++attempt;
        strict_mode =
            block_fallback ||
            attempt <= (unsigned int)CHANNEL_STRICT_ATTEMPTS;
        initialize_handshake(
            &handshake,
            calibration,
            attempt,
            !strict_mode
        );
        if (strict_mode) {
            status_line(
                "CH3 | validando DLG %s:%u | tentativa %u | Q encerra",
                config->dlg_ip,
                (unsigned)config->dlg_port,
                attempt
            );
        } else {
            status_line(
                "CH3 | DLG sem readback; modo compativel | "
                "tentativa %u | Q encerra",
                attempt
            );
        }

        (void)send_stop(connection);
        Sleep(50);
        (void)drain_socket(connection);

        handshake.sent_set_channel =
            send_channel_config(connection, calibration);
        if (!handshake.sent_set_channel) {
            send_failed = 1;
            failure_result = "SEND_SET_FAILED";
            goto attempt_failed;
        }
        (void)collect_handshake_responses(
            connection,
            &handshake,
            CHANNEL_SET_WAIT_MS,
            1
        );
        if (stop_requested()) {
            failure_result = "CANCELLED";
            goto attempt_failed;
        }
        if (handshake_has_explicit_failure(&handshake)) {
            failure_result = "SET_REJECTED";
            if (handshake.rejected_code == OP_SETCHCFG_R) {
                block_fallback = 1;
            }
            goto attempt_failed;
        }

        if (strict_mode) {
            for (get_request = 0;
                 get_request < CHANNEL_GET_REQUESTS &&
                 !handshake.saw_get_channel_response;
                 ++get_request) {
                if (!send_get_channel_config(connection)) {
                    send_failed = 1;
                    failure_result = "SEND_GET_FAILED";
                    break;
                }
                ++handshake.sent_get_channel;
                (void)collect_handshake_responses(
                    connection,
                    &handshake,
                    CHANNEL_GET_WAIT_MS,
                    2
                );
                if (stop_requested()) {
                    failure_result = "CANCELLED";
                    break;
                }
                if (handshake_has_explicit_failure(&handshake)) {
                    break;
                }
            }
            if (stop_requested()) {
                goto attempt_failed;
            }
            if (handshake.command_rejected) {
                failure_result = "GET_REJECTED";
                goto attempt_failed;
            }
            if (handshake.saw_get_channel_response &&
                !handshake.get_channel_matches) {
                block_fallback = 1;
                failure_result = "READBACK_MISMATCH";
                goto attempt_failed;
            }
            if (!handshake.saw_get_channel_response) {
                failure_result = send_failed
                    ? failure_result
                    : "READBACK_MISSING";
                goto attempt_failed;
            }
            block_fallback = 0;
        }

        handshake.sent_acq_setup =
            send_acq_setup(connection, config->sample_rate_hz);
        if (!handshake.sent_acq_setup) {
            failure_result = "SEND_SETUP_FAILED";
            goto attempt_failed;
        }
        (void)collect_handshake_responses(
            connection,
            &handshake,
            ACQ_SETUP_WAIT_MS,
            0
        );
        if (stop_requested()) {
            failure_result = "CANCELLED";
            goto attempt_failed;
        }
        if (handshake_has_explicit_failure(&handshake)) {
            failure_result = "SETUP_REJECTED";
            goto attempt_failed;
        }
        handshake.sent_acq_start = send_acq_start(connection);
        if (!handshake.sent_acq_start) {
            failure_result = "SEND_START_FAILED";
            goto attempt_failed;
        }
        if (wait_for_first_data(
                connection,
                &handshake,
                config->sample_rate_hz)) {
            report_stream_handshake(
                diagnostics,
                &handshake,
                "READY"
            );
            return 1;
        }
        if (handshake.command_rejected) {
            failure_result = "START_REJECTED";
        } else if (handshake.socket_error) {
            failure_result = "SOCKET_ERROR";
        } else if (stop_requested()) {
            failure_result = "CANCELLED";
        }

attempt_failed:
        report_stream_handshake(
            diagnostics,
            &handshake,
            failure_result
        );
        if (stop_requested()) {
            break;
        }
        if (strict_mode &&
            !block_fallback &&
            !handshake.saw_get_channel_response &&
            attempt == (unsigned int)CHANNEL_STRICT_ATTEMPTS) {
            status_line(
                "CH3 | GETCHCFG sem resposta; ativando fluxo compativel..."
            );
        } else {
            show_handshake_failure(&handshake);
        }
        (void)send_stop(connection);
        Sleep(RETRY_DELAY_MS);
    }
    return 0;
}

static int start_stream_until_data(
    const AppConfig *config,
    DlgConnection *connection,
    const Calibration *calibration)
{
    return start_stream_until_data_diagnostic(
        config,
        connection,
        calibration,
        NULL
    );
}

static int capture_raw_average(
    const AppConfig *config,
    DlgConnection *connection,
    const Calibration *calibration,
    const char *point_name,
    double *raw_average,
    long long *captured_samples)
{
    for (;;) {
        LARGE_INTEGER frequency;
        LARGE_INTEGER start;
        ULONGLONG last_data_ms;
        ULONGLONG next_status_ms;
        double sum = 0.0;
        long long count = 0;
        int stream_failed = 0;

        if (!start_stream_until_data(config, connection, calibration)) {
            return 0;
        }

        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&start);
        last_data_ms = GetTickCount64();
        next_status_ms = last_data_ms;

        while (!stop_requested()) {
            PktData packet;
            int packet_bytes = 0;
            int result;
            LARGE_INTEGER now;
            double elapsed_ms;

            if (GetTickCount64() >= next_status_ms) {
                status_line(
                    "CH3 | calibrando %s | amostras %lld | Q encerra",
                    point_name,
                    count
                );
                next_status_ms = GetTickCount64() + UPDATE_INTERVAL_MS;
            }
            if (poll_quit_key()) {
                break;
            }

            result = receive_data_packet(
                connection,
                &packet,
                &packet_bytes,
                NULL
            );
            if (result > 0) {
                if (add_packet_samples(
                        &packet,
                        packet_bytes,
                        &sum,
                        &count)) {
                    last_data_ms = GetTickCount64();
                }
            } else if (result < 0) {
                stream_failed = 1;
                break;
            }

            if (GetTickCount64() - last_data_ms > STREAM_TIMEOUT_MS) {
                stream_failed = 1;
                break;
            }

            QueryPerformanceCounter(&now);
            elapsed_ms =
                1000.0 *
                (double)(now.QuadPart - start.QuadPart) /
                (double)frequency.QuadPart;
            if (elapsed_ms >= CALIB_CAPTURE_MS && count > 0) {
                *raw_average = sum / (double)count;
                *captured_samples = count;
                (void)send_stop(connection);
                return 1;
            }
        }

        (void)send_stop(connection);
        if (stop_requested()) {
            return 0;
        }
        if (stream_failed) {
            status_line("CH3 | fluxo interrompido; reconectando...");
            Sleep(RETRY_DELAY_MS);
        }
    }
}

static int prompt_reference_current(
    const char *point_name,
    double default_ma,
    double *reference_ma)
{
    char line[128];
    char *end = NULL;
    double parsed;

    for (;;) {
        if (stop_requested()) {
            return 0;
        }
        printf(
            "%s - corrente medida por instrumento de referencia [%.3f mA]: ",
            point_name,
            default_ma
        );
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) {
            if (stop_requested()) {
                clearerr(stdin);
            }
            return 0;
        }
        if (stop_requested()) {
            return 0;
        }

        {
            size_t length = strlen(line);
            while (length > 0 &&
                   (line[length - 1] == '\r' ||
                    line[length - 1] == '\n' ||
                    isspace((unsigned char)line[length - 1]))) {
                line[--length] = '\0';
            }
        }

        if (line[0] == '\0') {
            *reference_ma = default_ma;
            return 1;
        }
        if (_stricmp(line, "q") == 0) {
            InterlockedExchange(&g_stop_requested, 1);
            return 0;
        }
        parsed = strtod(line, &end);
        while (end && *end && isspace((unsigned char)*end)) {
            ++end;
        }
        if (end != line && end && *end == '\0' &&
            parsed > -1000.0 && parsed < 1000.0) {
            *reference_ma = parsed;
            return 1;
        }
        printf("Valor invalido. Informe mA ou pressione ENTER para o padrao.\n");
    }
}

static int run_calibration(
    const AppConfig *config,
    DlgConnection *connection,
    Calibration *calibration)
{
    double raw1;
    double raw2;
    double reference1;
    double reference2;
    double raw_delta;
    double reference_delta;
    long long samples1;
    long long samples2;

    calibration_init_defaults(calibration);

    printf(
        "\nCalibracao manual CH3 em mA\n"
        "----------------------------\n"
        "Amperimetro: ligue em serie com a saida do encoder.\n"
        "Calibrador ativo: desconecte o encoder e use o calibrador em seu lugar.\n"
        "Desenergize o circuito antes de alterar qualquer ligacao.\n"
        "Nao encoste os fios amarelo/laranja no preto: eles reprogramam o encoder.\n"
        "Cada captura dura %.1f s e usa a media das amostras.\n\n",
        CALIB_CAPTURE_MS / 1000.0
    );

    if (!start_stream_until_data(config, connection, calibration)) {
        return 0;
    }
    (void)send_stop(connection);
    clear_status_line();
    printf("Comunicacao com o DLG confirmada; CH3 esta recebendo ACQDATA.\n\n");

    printf(
        "PONTO 1: posicione o encoder proximo do limite baixo e estabilize.\n"
    );
    if (!prompt_reference_current("Ponto 1", 4.0, &reference1)) {
        return 0;
    }
    if (!capture_raw_average(
            config,
            connection,
            calibration,
            "ponto 1",
            &raw1,
            &samples1)) {
        return 0;
    }
    clear_status_line();
    printf(
        "Ponto 1 capturado: referencia=%.6f mA raw_medio=%.6f amostras=%lld\n\n",
        reference1,
        raw1,
        samples1
    );

    printf(
        "PONTO 2: posicione o encoder proximo do limite alto e estabilize.\n"
    );
    if (!prompt_reference_current("Ponto 2", 20.0, &reference2)) {
        return 0;
    }
    if (!capture_raw_average(
            config,
            connection,
            calibration,
            "ponto 2",
            &raw2,
            &samples2)) {
        return 0;
    }
    clear_status_line();
    printf(
        "Ponto 2 capturado: referencia=%.6f mA raw_medio=%.6f amostras=%lld\n",
        reference2,
        raw2,
        samples2
    );

    raw_delta = raw2 - raw1;
    reference_delta = reference2 - reference1;
    if (raw_delta > -32.0 && raw_delta < 32.0) {
        fprintf(
            stderr,
            "Falha: os dois pontos diferem menos de 32 counts. "
            "Verifique ligacao e movimento.\n"
        );
        return 0;
    }
    if (reference_delta > -0.01 && reference_delta < 0.01) {
        fprintf(
            stderr,
            "Falha: as duas referencias de corrente sao praticamente iguais.\n"
        );
        return 0;
    }
    if (raw1 <= -32700.0 || raw1 >= 32700.0 ||
        raw2 <= -32700.0 || raw2 >= 32700.0) {
        fprintf(
            stderr,
            "Falha: ponto proximo da saturacao do A/D. "
            "Use referencias dentro da faixa util.\n"
        );
        return 0;
    }

    calibration->slope_ma_per_count = reference_delta / raw_delta;
    calibration->intercept_ma =
        reference1 - calibration->slope_ma_per_count * raw1;
    strncpy_s(
        calibration->source_path,
        sizeof(calibration->source_path),
        config->calib_out_path,
        _TRUNCATE
    );

    if (!write_calibration_file(
            config->calib_out_path,
            raw1,
            reference1,
            samples1,
            raw2,
            reference2,
            samples2,
            calibration)) {
        fprintf(
            stderr,
            "Falha ao gravar calibracao: %s\n",
            config->calib_out_path
        );
        return 0;
    }

    printf(
        "Calibracao salva em %s\n"
        "Fit: mA = %.12g * raw + %.12g\n\n",
        config->calib_out_path,
        calibration->slope_ma_per_count,
        calibration->intercept_ma
    );
    return 1;
}

static const char *auto_cal_event_name(int event)
{
    if (event == AUTO_EVENT_TRANSITION) {
        return "TRANSITION";
    }
    if (event == AUTO_EVENT_REJECTED) {
        return "REJECTED";
    }
    if (event == AUTO_EVENT_CANDIDATE) {
        return "CANDIDATE";
    }
    if (event == AUTO_EVENT_REARMED) {
        return "REARMED";
    }
    if (event == AUTO_EVENT_WINDOW_WAIT) {
        return "WINDOW_WAIT";
    }
    if (event == AUTO_EVENT_REVERSED) {
        return "REVERSED";
    }
    return "NONE";
}

static const char *auto_cal_reason_name(int reason)
{
    switch (reason) {
    case AUTO_REASON_JUMP_BELOW_LIMIT:
        return "JUMP_BELOW_CONFIRMATION";
    case AUTO_REASON_POST_NOT_STABLE:
        return "POST_NOT_PERSISTENT";
    case AUTO_REASON_PRE_WINDOW_UNSTABLE:
        return "PRE_WINDOW_UNSTABLE";
    case AUTO_REASON_POST_WINDOW_UNSTABLE:
        return "POST_WINDOW_UNSTABLE";
    case AUTO_REASON_DIRECTION_REVERSED:
        return "DIRECTION_REVERSED";
    case AUTO_REASON_SPAN_TOO_SMALL:
        return "SPAN_TOO_SMALL";
    case AUTO_REASON_INCONSISTENT_WITH_PRIOR:
        return "INCONSISTENT_WITH_PRIOR";
    case AUTO_REASON_WINDOW_NOT_PERSISTENT:
        return "WINDOW_NOT_PERSISTENT";
    case AUTO_REASON_WINDOW_TOO_NOISY:
        return "WINDOW_TOO_NOISY";
    case AUTO_REASON_CONFIRM_NOT_PERSISTENT:
        return "CONFIRM_NOT_PERSISTENT";
    case AUTO_REASON_CONFIRM_TOO_NOISY:
        return "CONFIRM_TOO_NOISY";
    default:
        return "NONE";
    }
}

static int auto_cal_log_line(
    FILE *log_file,
    ULONGLONG elapsed_ms,
    const char *event_name,
    const char *format,
    ...)
{
    va_list args;
    int ok = 1;

    if (!log_file || !event_name || !format) {
        return 0;
    }
    if (fprintf(
            log_file,
            "%10llu %-18s ",
            (unsigned long long)elapsed_ms,
            event_name) < 0) {
        ok = 0;
    }
    va_start(args, format);
    if (vfprintf(log_file, format, args) < 0) {
        ok = 0;
    }
    va_end(args);
    if (fputc('\n', log_file) == EOF) {
        ok = 0;
    }
    if (fflush(log_file) != 0) {
        ok = 0;
    }
    return ok;
}

static int auto_cal_close_event_log(
    FILE **log_file,
    ULONGLONG elapsed_ms,
    const char *result,
    int capture_complete,
    int stopped,
    int failure_reported,
    long long received_samples,
    const AutoCalDetector *detector,
    unsigned long frame_gap_events,
    unsigned long tolerated_gap_events,
    unsigned long reset_gap_events,
    unsigned long long lost_packets,
    unsigned long reordered_packets)
{
    int ok;

    if (!log_file || !*log_file || !detector || !result) {
        return 0;
    }
    ok = auto_cal_log_line(
        *log_file,
        elapsed_ms,
        "END",
        "result=%s capture_complete=%d stopped=%d failure=%d "
        "samples=%lld candidates=%d window_waits=%d "
        "rejected=%d wraps=%d "
        "gaps=%lu tolerated=%lu resets=%lu "
        "lost=%llu reordered=%lu",
        result,
        capture_complete,
        stopped,
        failure_reported,
        received_samples,
        detector->candidate_count,
        detector->window_wait_count,
        detector->rejected_count,
        detector->transition_count,
        frame_gap_events,
        tolerated_gap_events,
        reset_gap_events,
        lost_packets,
        reordered_packets
    );
    if (ferror(*log_file)) {
        ok = 0;
    }
    if (fclose(*log_file) != 0) {
        ok = 0;
    }
    *log_file = NULL;
    return ok;
}

static int prompt_auto_calibration_start(
    const AppConfig *config,
    int motorized,
    int drive_command_rpm,
    int timeout_ms)
{
    char line[64];

    if (motorized) {
        printf(
            "\nAUTOCALIBRACAO ANGULAR POR DLG + DRIVE\n"
            "--------------------------------------\n"
            "Serao detectados %d wraps para delimitar %d voltas "
            "completas.\n"
            "Duas voltas treinam a regressao raw->graus; a terceira "
            "valida a precisao.\n"
            "O JSON anterior so sera substituido se todos os limites "
            "forem aprovados.\n\n",
            AUTO_CALIB_TRANSITIONS,
            ANGULAR_COMPLETE_REVOLUTIONS
        );
    } else {
        printf(
            "\nAUTOCALIBRACAO NOMINAL POR %d TRANSICOES\n"
            "-----------------------------------------\n"
            "Esta operacao NAO e uma calibracao metrologica "
            "rastreavel.\n"
            "Ela associa os extremos observados aos valores nominais "
            "4 e 20 mA.\n\n",
            AUTO_CALIB_TRANSITIONS
        );
    }
    if (motorized) {
        printf(
            "O programa controlara o Drive somente depois de receber "
            "dados validos do DLG.\n"
            "Alvo no eixo do encoder : %.3f RPM\n"
            "Relacao i = D2/D1       : %.6g%s\n"
            "Comando enviado ao motor: %+d RPM\n"
            "Drive                    : %s, slave %d, %d %c\n"
            "Tempo limite             : %d s\n"
            "A partida e a parada usam o a5_speed_logger, com rampa e "
            "STOP reforcado.\n"
            "A referencia angular usa P0B-09 a 10 Hz, alinhado ao DLG "
            "pelo QPC.\n"
            "A relacao deve representar motor->eixo do encoder; use i=1 "
            "se ambos estiverem no mesmo eixo.\n"
            "Confirme protecoes, area livre e parada fisica acessivel.\n",
            config->encoder_target_rpm,
            config->mechanical_ratio,
            config->relation_loaded
                ? " [configuracao do supervisorio]"
                : " [valor local]",
            drive_command_rpm,
            config->drive_port,
            DEFAULT_DRIVE_SLAVE,
            DEFAULT_DRIVE_BAUD,
            DEFAULT_DRIVE_PARITY,
            timeout_ms / 1000
        );
    } else {
        printf(
            "Depois de iniciar, gire o eixo devagar, continuamente e "
            "sempre\n"
            "no mesmo sentido. O salto pode ser alto->baixo ou "
            "baixo->alto;\n"
            "o primeiro salto confirmado define o sentido esperado.\n"
            "Cada salto confirmado sera contado como uma revolucao.\n"
            "Tempo limite: %d s.\n",
            timeout_ms / 1000
        );
    }
    printf(
        "Valide antes a polaridade CH3: pino 8=I+ e pino 1=I-;\n"
        "polaridade invertida invalida a associacao nominal 4-20 mA.\n"
        "Durante a captura, Q ou Ctrl+C cancela.\n\n"
    );
    for (;;) {
        size_t length;

        printf(
            motorized
                ? "Pressione ENTER para autorizar o movimento ou "
                  "Q para cancelar: "
                : "Pressione ENTER para iniciar ou Q para cancelar: "
        );
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) {
            return 0;
        }
        length = strlen(line);
        while (length > 0 &&
               isspace((unsigned char)line[length - 1])) {
            line[--length] = '\0';
        }
        if (_stricmp(line, "q") == 0) {
            InterlockedExchange(&g_stop_requested, 1);
            return 0;
        }
        if (line[0] == '\0') {
            return 1;
        }
        printf("Entrada invalida. Use somente ENTER ou Q.\n");
    }
}

static int run_auto_calibration(
    const AppConfig *config,
    DlgConnection *connection,
    Calibration *calibration,
    int motorized)
{
    AutoCalDetector detector;
    AngularFit angular_fit;
    DriveSession drive_session;
    AutoCalMotion motion;
    HandshakeState dlg_handshake;
    StreamStartDiagnostics stream_diagnostics;
    FILE *diagnostic = NULL;
    FILE *event_log = NULL;
    char diagnostic_path[MAX_PATH];
    char event_log_path[MAX_PATH];
    ULONGLONG capture_start_ms = 0;
    ULONGLONG log_start_ms = 0;
    ULONGLONG last_data_ms;
    ULONGLONG next_status_ms;
    ULONGLONG next_log_ms;
    ULONGLONG next_diagnostic_flush_ms;
    double raw_low = 0.0;
    double raw_high = 0.0;
    double maximum_relative_deviation = 0.0;
    double last_delta = 0.0;
    double last_window_jump = 0.0;
    double last_threshold = 0.0;
    double peak_delta = 0.0;
    double peak_window_jump = 0.0;
    double capture_peak_delta = 0.0;
    double capture_peak_window_jump = 0.0;
    int16_t last_raw = 0;
    double last_filtered = 0.0;
    int capture_complete = 0;
    int failure_reported = 0;
    int close_ok = 1;
    int operation_success = 0;
    const char *final_result = "capture_failed";
    const char *capture_reason = "INCOMPLETE";
    int dlg_not_ready = 0;
    int stream_history_stale = 0;
    int have_frame = 0;
    int32_t previous_frame = 0;
    unsigned long frame_gap_events = 0;
    unsigned long tolerated_gap_events = 0;
    unsigned long reset_gap_events = 0;
    unsigned long long lost_packets = 0;
    unsigned long reordered_packets = 0;
    long long diagnostic_index = 0;
    long long received_samples = 0;
    long long endpoint_tail_target_index = -1;
    LARGE_INTEGER qpc_frequency;
    int auto_timeout_ms = motorized
        ? AUTO_CALIB_MOTOR_TIMEOUT_MS
        : AUTO_CALIB_MANUAL_TIMEOUT_MS;
    int drive_command_rpm = 0;
    int drive_stop_confirmed = 1;
    int endpoint_far_samples =
        (config->sample_rate_hz *
             AUTO_CALIB_ENDPOINT_FAR_MS +
         999) /
        1000;
    int stream_history_reset_ms =
        (4000 + config->sample_rate_hz - 1) /
        config->sample_rate_hz;
    int i;

    memset(&detector, 0, sizeof(detector));
    memset(&angular_fit, 0, sizeof(angular_fit));
    if (stream_history_reset_ms < 100) {
        stream_history_reset_ms = 100;
    }
    drive_session_init(&drive_session);
    memset(&motion, 0, sizeof(motion));
    memset(&dlg_handshake, 0, sizeof(dlg_handshake));
    memset(&stream_diagnostics, 0, sizeof(stream_diagnostics));
    memset(&qpc_frequency, 0, sizeof(qpc_frequency));
    (void)QueryPerformanceFrequency(&qpc_frequency);
    calibration_init_defaults(calibration);

    if (motorized &&
        (!path_is_file(config->drive_exe) ||
         !compute_drive_command_rpm(
             config->encoder_target_rpm,
             config->mechanical_ratio,
             config->drive_direction,
             &drive_command_rpm))) {
        fprintf(
            stderr,
            "Configuracao do Drive invalida. Verifique o executavel, "
            "a relacao mecanica e o alvo de RPM no menu.\n"
        );
        return 0;
    }
    if (!prompt_auto_calibration_start(
            config,
            motorized,
            drive_command_rpm,
            auto_timeout_ms)) {
        return 0;
    }
    if (!build_auto_cal_sidecar_path(
            config->calib_out_path,
            "_autocal.csv",
            diagnostic_path,
            sizeof(diagnostic_path))) {
        fprintf(
            stderr,
            "Falha ao formar o caminho do CSV de diagnostico.\n"
        );
        return 0;
    }
    if (!build_auto_cal_sidecar_path(
            config->calib_out_path,
            "_autocal_events.log",
            event_log_path,
            sizeof(event_log_path))) {
        fprintf(
            stderr,
            "Falha ao formar o caminho do log de eventos.\n"
        );
        return 0;
    }
    if (!create_parent_directories(diagnostic_path) ||
        fopen_s(&diagnostic, diagnostic_path, "wb") != 0 ||
        !diagnostic) {
        fprintf(
            stderr,
            "Falha ao criar CSV de diagnostico: %s\n",
            diagnostic_path
        );
        return 0;
    }
    if (fopen_s(&event_log, event_log_path, "wb") != 0 ||
        !event_log) {
        (void)fclose(diagnostic);
        fprintf(
            stderr,
            "Falha ao criar log de eventos: %s\n",
            event_log_path
        );
        return 0;
    }
    (void)setvbuf(diagnostic, NULL, _IOFBF, 64U * 1024U);
    fprintf(
        diagnostic,
        "rx_idx;detector_idx;t_ms;t_qpc;frame;frame_delta;frame_gap;"
        "raw;filtered_raw;delta;window_jump;threshold;"
        "event;event_origin_idx;wraps;armed\n"
    );
    log_start_ms = GetTickCount64();
    auto_cal_log_line(
        event_log,
        0,
        "START",
        "model=%s build=%s channel=CH3 rate_hz=%d timeout_s=%d "
        "filter=%d pre=%d guard=%d post=%d "
        "window_threshold_fraction=%.2f majority=%d "
        "mad_fraction=%.2f min_jump=%.0f min_span=%.0f "
        "range_mad_multiplier=%.1f range_min_band=%.0f "
        "candidate_retry_samples=%d confirm=%d/%d "
        "rearm_fraction=%.2f tolerated_frame_delta=%u "
        "transition_origin=CENTER_GUARD offset_samples=%d "
        "wraps_required=%d calibration_mode=%s",
        ENCODER_MODEL,
        ENCODER_TEST_BUILD_ID,
        config->sample_rate_hz,
        auto_timeout_ms / 1000,
        AUTO_CALIB_FILTER_SAMPLES,
        AUTO_CALIB_PRE_SAMPLES,
        AUTO_CALIB_GUARD_SAMPLES,
        AUTO_CALIB_POST_SAMPLES,
        AUTO_CALIB_WINDOW_THRESHOLD_FRACTION,
        AUTO_CALIB_WINDOW_MAJORITY,
        AUTO_CALIB_WINDOW_MAD_FRACTION,
        AUTO_CALIB_MIN_JUMP,
        AUTO_CALIB_MIN_SPAN,
        AUTO_CALIB_RANGE_MAD_MULTIPLIER,
        AUTO_CALIB_RANGE_MIN_BAND,
        AUTO_CALIB_WINDOW_RETRY_SAMPLES,
        AUTO_CALIB_CONFIRM_MAJORITY,
        AUTO_CALIB_CONFIRM_SAMPLES,
        AUTO_CALIB_REARM_FRACTION,
        (unsigned)AUTO_CALIB_MAX_TOLERATED_FRAME_DELTA,
        AUTO_CALIB_TRANSITION_ORIGIN_OFFSET,
        AUTO_CALIB_TRANSITIONS,
        motorized
            ? "DRIVE_REFERENCED_ANGLE"
            : "NOMINAL_ENDPOINT_MA"
    );
    auto_cal_log_line(
        event_log,
        0,
        "MOTION_CONFIG",
        "enabled=%d encoder_target_rpm=%.6f "
        "mechanical_ratio=%.9g drive_command_rpm=%+d "
        "drive_port=%s relation_loaded=%d relation_source=%s",
        motorized,
        config->encoder_target_rpm,
        config->mechanical_ratio,
        drive_command_rpm,
        config->drive_port,
        config->relation_loaded,
        config->relation_source[0]
            ? config->relation_source
            : "LOCAL"
    );
    auto_cal_log_line(
        event_log,
        0,
        "OUTPUT",
        "json=%s csv=%s log=%s",
        config->calib_out_path,
        diagnostic_path,
        event_log_path
    );
    printf("Log de eventos: %s\n", event_log_path);

    if (motorized) {
        printf(
            "Preparando Drive em %s; o eixo ainda nao sera "
            "movimentado...\n",
            config->drive_port
        );
        if (!drive_session_launch(
                config,
                auto_timeout_ms,
                &drive_session,
                &motion)) {
            auto_cal_log_line(
                event_log,
                GetTickCount64() - log_start_ms,
                "DRIVE_NOT_READY",
                "exe=%s port=%s launched=%d ready=%d",
                config->drive_exe,
                config->drive_port,
                drive_session.launched,
                drive_session.ready_seen
            );
            fprintf(
                stderr,
                "Falha preparando o Drive. O motor nao sera iniciado.\n"
            );
            failure_reported = 1;
            goto cleanup;
        }
        auto_cal_log_line(
            event_log,
            GetTickCount64() - log_start_ms,
            "DRIVE_READY",
            "command_rpm=%+d drive_csv=%s schedule=%s",
            motion.drive_command_rpm,
            drive_session.drive_csv,
            drive_session.schedule_csv
        );
        printf(
            "Drive pronto e parado. Aguardando dados validos do DLG...\n"
        );
    }

    stream_diagnostics.event_log = event_log;
    stream_diagnostics.log_start_ms = log_start_ms;
    stream_diagnostics.result = &dlg_handshake;
    if (!start_stream_until_data_diagnostic(
            config,
            connection,
            calibration,
            &stream_diagnostics)) {
        auto_cal_log_line(
            event_log,
            GetTickCount64() - log_start_ms,
            "DLG_NOT_READY",
            "stopped=%d attempt=%u mode=%s "
            "set_ack=%d get_seen=%d get_match=%d "
            "setup_resp=%d start_resp=%d acq_packets=%d "
            "observed_hz=%.6g rate_rejects=%d frame_rejects=%d "
            "socket_error=%d rejected=0x%04X error=%u",
            stop_requested(),
            dlg_handshake.attempt,
            dlg_handshake.fallback_used ? "FALLBACK" : "STRICT",
            dlg_handshake.saw_set_channel_ack,
            dlg_handshake.saw_get_channel_response,
            dlg_handshake.get_channel_matches,
            dlg_handshake.saw_acq_setup_response,
            dlg_handshake.saw_acq_start_response,
            dlg_handshake.valid_data_packets,
            (double)dlg_handshake.observed_sample_freq,
            dlg_handshake.data_rate_rejects,
            dlg_handshake.data_frame_rejects,
            dlg_handshake.socket_error,
            (unsigned)dlg_handshake.rejected_code,
            (unsigned)dlg_handshake.error_code
        );
        if (!stop_requested()) {
            failure_reported = 1;
            dlg_not_ready = 1;
        }
        goto cleanup;
    }
    if (dlg_handshake.fallback_used) {
        clear_status_line();
        printf(
            "DLG enviou ACQDATA, mas nao respondeu ao GETCHCFG. "
            "Preset CH3 reenviado e fluxo compativel ativado.\n"
        );
    }
    auto_cal_log_line(
        event_log,
        GetTickCount64() - log_start_ms,
        "DLG_READY",
        "stream_confirmed=1 attempt=%u mode=%s "
        "set_ack=%d get_seen=%d get_match=%d "
        "setup_resp=%d start_resp=%d acq_packets=%d "
        "observed_hz=%.6g rate_rejects=%d frame_rejects=%d",
        dlg_handshake.attempt,
        dlg_handshake.fallback_used ? "FALLBACK" : "STRICT",
        dlg_handshake.saw_set_channel_ack,
        dlg_handshake.saw_get_channel_response,
        dlg_handshake.get_channel_matches,
        dlg_handshake.saw_acq_setup_response,
        dlg_handshake.saw_acq_start_response,
        dlg_handshake.valid_data_packets,
        (double)dlg_handshake.observed_sample_freq,
        dlg_handshake.data_rate_rejects,
        dlg_handshake.data_frame_rejects
    );
    if (motorized) {
        LARGE_INTEGER motor_start_qpc;
        int drained_packets;

        memset(&motor_start_qpc, 0, sizeof(motor_start_qpc));
        (void)QueryPerformanceCounter(&motor_start_qpc);
        if (!drive_session_start(&drive_session)) {
            auto_cal_log_line(
                event_log,
                GetTickCount64() - log_start_ms,
                "DRIVE_START_FAILED",
                "running=%d started_ack=%d qpc=%lld",
                drive_session_is_running(&drive_session),
                drive_session.started_seen,
                (long long)motor_start_qpc.QuadPart
            );
            fprintf(
                stderr,
                "Falha iniciando o Drive; a autocalibracao foi "
                "cancelada.\n"
            );
            failure_reported = 1;
            goto cleanup;
        }
        auto_cal_log_line(
            event_log,
            GetTickCount64() - log_start_ms,
            "DRIVE_STARTED",
            "command_rpm=%+d encoder_target_rpm=%.6f "
            "mechanical_ratio=%.9g qpc=%lld",
            motion.drive_command_rpm,
            motion.encoder_target_rpm,
            motion.mechanical_ratio,
            (long long)motor_start_qpc.QuadPart
        );
        drained_packets = drain_socket(connection);
        auto_cal_log_line(
            event_log,
            GetTickCount64() - log_start_ms,
            "DLG_RESYNC",
            "after_drive_started=1 discarded_packets=%d "
            "capture_clock_reanchored=1",
            drained_packets
        );
    }
    auto_cal_detector_init(
        &detector,
        config->sample_rate_hz
    );
    capture_start_ms = GetTickCount64();
    last_data_ms = capture_start_ms;
    next_status_ms = capture_start_ms;
    next_log_ms = capture_start_ms + 1000;
    next_diagnostic_flush_ms =
        capture_start_ms + AUTO_CALIB_DIAGNOSTIC_FLUSH_MS;

    while (!stop_requested()) {
        PktData packet;
        LARGE_INTEGER packet_qpc;
        int packet_bytes = 0;
        int receive_result;
        ULONGLONG now_ms;

        memset(&packet_qpc, 0, sizeof(packet_qpc));
        if (motorized) {
            if (!drive_session_is_running(&drive_session)) {
                (void)drive_session_pump_output(&drive_session);
                auto_cal_log_line(
                    event_log,
                    GetTickCount64() - log_start_ms,
                    "DRIVE_EXITED",
                    "started=%d stopped_ack=%d",
                    drive_session.started_seen,
                    drive_session.stopped_seen
                );
                fprintf(
                    stderr,
                    "O processo do Drive encerrou durante a captura.\n"
                );
                failure_reported = 1;
                break;
            }
        }
        if (poll_quit_key()) {
            break;
        }
        receive_result = receive_data_packet(
            connection,
            &packet,
            &packet_bytes,
            NULL
        );
        (void)QueryPerformanceCounter(&packet_qpc);
        now_ms = GetTickCount64();
        if (motorized && now_ms >= next_status_ms &&
            !drive_session_pump_output(&drive_session)) {
            auto_cal_log_line(
                event_log,
                now_ms - log_start_ms,
                "DRIVE_PIPE_FAILED",
                "running=%d status_seen=%d",
                drive_session_is_running(&drive_session),
                drive_session.status_seen
            );
            fprintf(
                stderr,
                "Falha lendo a telemetria IPC do Drive.\n"
            );
            failure_reported = 1;
            break;
        }
        if (motorized && drive_session.started_seen) {
            ULONGLONG drive_status_reference =
                drive_session.status_seen
                    ? drive_session.status_received_ms
                    : drive_session.started_received_ms;
            if (drive_status_reference > 0 &&
                now_ms - drive_status_reference >
                    DRIVE_STATUS_TIMEOUT_MS) {
                auto_cal_log_line(
                    event_log,
                    now_ms - log_start_ms,
                    "DRIVE_STATUS_TIMEOUT",
                    "status_seen=%d age_ms=%llu limit_ms=%d",
                    drive_session.status_seen,
                    (unsigned long long)(
                        now_ms - drive_status_reference
                    ),
                    DRIVE_STATUS_TIMEOUT_MS
                );
                fprintf(
                    stderr,
                    "Telemetria do Drive ficou sem atualizacao por "
                    "mais de %d ms.\n",
                    DRIVE_STATUS_TIMEOUT_MS
                );
                failure_reported = 1;
                break;
            }
            if (drive_session.status_seen &&
                !drive_session.comm_active) {
                auto_cal_log_line(
                    event_log,
                    now_ms - log_start_ms,
                    "DRIVE_COMM_LOST",
                    "last_position_age_ms=%llu errors=%d",
                    drive_session.last_position_age_ms,
                    drive_session.position_errors
                );
                fprintf(
                    stderr,
                    "O Drive informou perda da leitura P0B-09.\n"
                );
                failure_reported = 1;
                break;
            }
        }

        if (receive_result > 0) {
            int header_bytes =
                (int)offsetof(PktData, samples);
            int available =
                (packet_bytes - header_bytes) /
                (int)sizeof(packet.samples[0]);
            int expected =
                packet.n_signals * packet.bursts;
            uint32_t frame_delta = 0;
            int frame_gap = 0;
            int hard_frame_gap = 0;
            int drop_reordered = 0;

            if (expected <= 0 ||
                expected > MAX_PACKET_SAMPLES ||
                available < expected) {
                clear_status_line();
                fprintf(
                    stderr,
                    "Pacote ACQDATA inconsistente durante "
                    "a autocalibracao.\n"
                );
                auto_cal_log_line(
                    event_log,
                    now_ms - log_start_ms,
                    "PACKET_INVALID",
                    "frame=%ld bytes=%d expected=%d available=%d",
                    (long)packet.frame,
                    packet_bytes,
                    expected,
                    available
                );
                failure_reported = 1;
                break;
            }
            if (!stream_history_stale &&
                now_ms - last_data_ms >
                    (ULONGLONG)stream_history_reset_ms) {
                auto_cal_reset_stream_history(&detector);
                auto_cal_log_line(
                    event_log,
                    now_ms - log_start_ms,
                    "STREAM_HISTORY_RESET",
                    "last_data_age_ms=%llu threshold_ms=%d "
                    "reason=DATA_RESUME",
                    (unsigned long long)(
                        now_ms - last_data_ms
                    ),
                    stream_history_reset_ms
                );
            }
            stream_history_stale = 0;
            if (have_frame) {
                frame_delta =
                    (uint32_t)packet.frame -
                    (uint32_t)previous_frame;
                if (frame_delta != 1U) {
                    frame_gap = 1;
                    ++frame_gap_events;
                    if (frame_delta > 1U &&
                        frame_delta < 0x80000000U) {
                        lost_packets +=
                            (unsigned long long)(
                                frame_delta - 1U
                            );
                        if (auto_cal_gap_requires_reset(
                                frame_delta)) {
                            hard_frame_gap = 1;
                            ++reset_gap_events;
                            auto_cal_log_line(
                                event_log,
                                now_ms - log_start_ms,
                                "GAP_RESET",
                                "previous_frame=%ld frame=%ld "
                                "delta=%lu lost_now=%lu "
                                "candidate=%d armed=%d "
                                "rearm=%.0f/%.0f",
                                (long)previous_frame,
                                (long)packet.frame,
                                (unsigned long)frame_delta,
                                (unsigned long)(
                                    frame_delta - 1U
                                ),
                                detector.candidate_active,
                                detector.armed,
                                detector.rearm_progress,
                                detector.rearm_required
                            );
                            auto_cal_reset_stream_history(
                                &detector
                            );
                        } else {
                            ++tolerated_gap_events;
                        }
                    } else {
                        ++reordered_packets;
                        drop_reordered = 1;
                        auto_cal_log_line(
                            event_log,
                            now_ms - log_start_ms,
                            "REORDER_DROP",
                            "reference_frame=%ld dropped_frame=%ld "
                            "delta=%lu wraps=%d armed=%d",
                            (long)previous_frame,
                            (long)packet.frame,
                            (unsigned long)frame_delta,
                            detector.transition_count,
                            detector.armed
                        );
                    }
                }
            } else {
                have_frame = 1;
            }

            if (drop_reordered) {
                for (i = 0; i < expected; ++i) {
                    LONGLONG sample_qpc =
                        packet_qpc.QuadPart;
                    if (qpc_frequency.QuadPart > 0 &&
                        config->sample_rate_hz > 0) {
                        LONGLONG samples_behind =
                            (LONGLONG)(expected - 1 - i);
                        sample_qpc -=
                            samples_behind *
                            qpc_frequency.QuadPart /
                            config->sample_rate_hz;
                    }
                    fprintf(
                        diagnostic,
                        "%lld;-1;%llu;%lld;%ld;%lu;1;"
                        "%d;NULL;NULL;NULL;NULL;REORDER_DROP;"
                        "-1;%d;%d\n",
                        diagnostic_index++,
                        (unsigned long long)(
                            now_ms - capture_start_ms
                        ),
                        (long long)sample_qpc,
                        (long)packet.frame,
                        (unsigned long)frame_delta,
                        (int)packet.samples[i],
                        detector.transition_count,
                        detector.armed
                    );
                }
                fflush(diagnostic);
            } else {
                last_data_ms = now_ms;
                previous_frame = packet.frame;
                for (i = 0; i < expected; ++i) {
                    AutoCalEvent event;
                    LONGLONG sample_qpc =
                        packet_qpc.QuadPart;
                    int event_result =
                        auto_cal_process_sample(
                            &detector,
                            packet.samples[i],
                            &event
                        );
                    long long detector_index =
                        detector.sample_index - 1;

                    if (qpc_frequency.QuadPart > 0 &&
                        config->sample_rate_hz > 0) {
                        LONGLONG samples_behind =
                            (LONGLONG)(expected - 1 - i);
                        sample_qpc -=
                            samples_behind *
                            qpc_frequency.QuadPart /
                            config->sample_rate_hz;
                    }
                    ++received_samples;
                    last_raw = packet.samples[i];
                    last_filtered = event.filtered_raw;
                    last_delta = event.delta;
                    last_window_jump = event.window_jump;
                    last_threshold = event.threshold;
                    if (auto_cal_abs(event.delta) >
                        peak_delta) {
                        peak_delta =
                            auto_cal_abs(event.delta);
                    }
                    if (auto_cal_abs(event.delta) >
                        capture_peak_delta) {
                        capture_peak_delta =
                            auto_cal_abs(event.delta);
                    }
                    if (auto_cal_abs(event.window_jump) >
                        peak_window_jump) {
                        peak_window_jump =
                            auto_cal_abs(event.window_jump);
                    }
                    if (auto_cal_abs(event.window_jump) >
                        capture_peak_window_jump) {
                        capture_peak_window_jump =
                            auto_cal_abs(event.window_jump);
                    }
                    fprintf(
                        diagnostic,
                        "%lld;%lld;%llu;%lld;%ld;%lu;%d;"
                        "%d;%.0f;%.0f;%.0f;%.0f;%s;"
                        "%lld;%d;%d\n",
                        diagnostic_index++,
                        detector_index,
                        (unsigned long long)(
                            now_ms - capture_start_ms
                        ),
                        (long long)sample_qpc,
                        (long)packet.frame,
                        (unsigned long)frame_delta,
                        frame_gap,
                        (int)packet.samples[i],
                        event.filtered_raw,
                        event.delta,
                        event.window_jump,
                        event.threshold,
                        hard_frame_gap
                            ? "GAP_RESET"
                            : frame_gap
                                ? "GAP_TOLERATED"
                            : auto_cal_event_name(event_result),
                        event.sample_index,
                        detector.transition_count,
                        detector.armed
                    );
                    if (hard_frame_gap ||
                        event_result != AUTO_EVENT_NONE) {
                        fflush(diagnostic);
                    }
                    if (event_result ==
                        AUTO_EVENT_CANDIDATE) {
                        auto_cal_log_line(
                            event_log,
                            now_ms - log_start_ms,
                            "CANDIDATE_START",
                            "frame=%ld idx=%lld raw=%d filtered=%.0f "
                            "delta=%+.0f window_jump=%+.0f "
                            "threshold=%.0f pre=%.0f post=%.0f "
                            "support=%d/%d,%d/%d mad=%.0f,%.0f "
                            "range=[%d,%d] span=%d typical_step=%.1f",
                            (long)packet.frame,
                            detector_index,
                            (int)packet.samples[i],
                            event.filtered_raw,
                            event.delta,
                            event.window_jump,
                            event.threshold,
                            event.pre_center,
                            event.post_center,
                            event.pre_on_old_side,
                            AUTO_CALIB_PRE_SAMPLES,
                            event.post_on_new_side,
                            AUTO_CALIB_POST_SAMPLES,
                            event.pre_mad,
                            event.post_mad,
                            (int)detector.running_min,
                            (int)detector.running_max,
                            (int)detector.running_max -
                                (int)detector.running_min,
                            detector.typical_step
                        );
                    } else if (event_result ==
                               AUTO_EVENT_WINDOW_WAIT) {
                        auto_cal_log_line(
                            event_log,
                            now_ms - log_start_ms,
                            "WINDOW_WAIT",
                            "frame=%ld idx=%lld reason=%s "
                            "jump=%+.0f threshold=%.0f "
                            "pre=%.0f post=%.0f "
                            "support=%d/%d,%d/%d "
                            "mad=%.0f,%.0f retry_samples=%d",
                            (long)packet.frame,
                            event.sample_index,
                            auto_cal_reason_name(event.reason),
                            event.jump,
                            event.threshold,
                            event.pre_center,
                            event.post_center,
                            event.pre_on_old_side,
                            AUTO_CALIB_PRE_SAMPLES,
                            event.post_on_new_side,
                            AUTO_CALIB_POST_SAMPLES,
                            event.pre_mad,
                            event.post_mad,
                            AUTO_CALIB_WINDOW_RETRY_SAMPLES
                        );
                    } else if (event_result ==
                               AUTO_EVENT_REJECTED) {
                        if (event.reason ==
                            AUTO_REASON_INCONSISTENT_WITH_PRIOR) {
                            auto_cal_log_line(
                                event_log,
                                now_ms - log_start_ms,
                                "CANDIDATE_REJECT",
                                "frame=%ld idx=%lld reason=%s "
                                "jump=%+.0f threshold=%.0f "
                                "pre=%.0f post=%.0f "
                                "support=%d/%d,%d/%d "
                                "mad=%.0f,%.0f spread=%.0f,%.0f "
                                "confirm=%d/%d center=%.0f "
                                "mad=%.0f spread=%.0f "
                                "raw_low=%.0f raw_high=%.0f "
                                "span=%.0f prior_low=%.0f "
                                "prior_high=%.0f prior_span=%.0f "
                                "prior_jump=%.0f allowed=%.0f",
                                (long)packet.frame,
                                event.sample_index,
                                auto_cal_reason_name(event.reason),
                                event.jump,
                                event.threshold,
                                event.pre_center,
                                event.post_center,
                                event.pre_on_old_side,
                                AUTO_CALIB_PRE_SAMPLES,
                                event.post_on_new_side,
                                AUTO_CALIB_POST_SAMPLES,
                                event.pre_mad,
                                event.post_mad,
                                event.pre_spread,
                                event.post_spread,
                                event.confirm_on_new_side,
                                AUTO_CALIB_CONFIRM_SAMPLES,
                                event.confirm_center,
                                event.confirm_mad,
                                event.confirm_spread,
                                event.raw_low,
                                event.raw_high,
                                event.raw_high - event.raw_low,
                                event.prior_low,
                                event.prior_high,
                                event.prior_span,
                                event.prior_jump,
                                event.allowed_deviation
                            );
                        } else if (event.reason ==
                                   AUTO_REASON_SPAN_TOO_SMALL) {
                            auto_cal_log_line(
                                event_log,
                                now_ms - log_start_ms,
                                "CANDIDATE_REJECT",
                                "frame=%ld idx=%lld reason=%s "
                                "jump=%+.0f threshold=%.0f "
                                "pre=%.0f post=%.0f "
                                "support=%d/%d,%d/%d "
                                "mad=%.0f,%.0f spread=%.0f,%.0f "
                                "confirm=%d/%d center=%.0f "
                                "mad=%.0f spread=%.0f "
                                "raw_low=%.0f raw_high=%.0f "
                                "span=%.0f",
                                (long)packet.frame,
                                event.sample_index,
                                auto_cal_reason_name(event.reason),
                                event.jump,
                                event.threshold,
                                event.pre_center,
                                event.post_center,
                                event.pre_on_old_side,
                                AUTO_CALIB_PRE_SAMPLES,
                                event.post_on_new_side,
                                AUTO_CALIB_POST_SAMPLES,
                                event.pre_mad,
                                event.post_mad,
                                event.pre_spread,
                                event.post_spread,
                                event.confirm_on_new_side,
                                AUTO_CALIB_CONFIRM_SAMPLES,
                                event.confirm_center,
                                event.confirm_mad,
                                event.confirm_spread,
                                event.raw_low,
                                event.raw_high,
                                event.raw_high - event.raw_low
                            );
                        } else {
                            auto_cal_log_line(
                                event_log,
                                now_ms - log_start_ms,
                                "CANDIDATE_REJECT",
                                "frame=%ld idx=%lld reason=%s "
                                "jump=%+.0f threshold=%.0f "
                                "pre=%.0f post=%.0f "
                                "support=%d/%d,%d/%d "
                                "mad=%.0f,%.0f spread=%.0f,%.0f "
                                "confirm=%d/%d center=%.0f "
                                "mad=%.0f spread=%.0f "
                                "raw_low=NA raw_high=NA span=NA",
                                (long)packet.frame,
                                event.sample_index,
                                auto_cal_reason_name(event.reason),
                                event.jump,
                                event.threshold,
                                event.pre_center,
                                event.post_center,
                                event.pre_on_old_side,
                                AUTO_CALIB_PRE_SAMPLES,
                                event.post_on_new_side,
                                AUTO_CALIB_POST_SAMPLES,
                                event.pre_mad,
                                event.post_mad,
                                event.pre_spread,
                                event.post_spread,
                                event.confirm_on_new_side,
                                AUTO_CALIB_CONFIRM_SAMPLES,
                                event.confirm_center,
                                event.confirm_mad,
                                event.confirm_spread
                            );
                        }
                    } else if (event_result ==
                               AUTO_EVENT_TRANSITION) {
                        auto_cal_log_line(
                            event_log,
                            now_ms - log_start_ms,
                            "WRAP_ACCEPT",
                            "number=%d frame=%ld idx=%lld sign=%+d "
                            "jump=%+.0f threshold=%.0f pre=%.0f "
                            "post=%.0f support=%d/%d,%d/%d "
                            "mad=%.0f,%.0f confirm=%d/%d "
                            "confirm_center=%.0f confirm_mad=%.0f "
                            "raw_low=%.0f "
                            "raw_high=%.0f span=%.0f",
                            detector.transition_count,
                            (long)packet.frame,
                            event.sample_index,
                            event.direction_sign,
                            event.jump,
                            event.threshold,
                            event.pre_center,
                            event.post_center,
                            event.pre_on_old_side,
                            AUTO_CALIB_PRE_SAMPLES,
                            event.post_on_new_side,
                            AUTO_CALIB_POST_SAMPLES,
                            event.pre_mad,
                            event.post_mad,
                            event.confirm_on_new_side,
                            AUTO_CALIB_CONFIRM_SAMPLES,
                            event.confirm_center,
                            event.confirm_mad,
                            event.raw_low,
                            event.raw_high,
                            event.raw_high - event.raw_low
                        );
                    } else if (event_result ==
                               AUTO_EVENT_REARMED) {
                        auto_cal_log_line(
                            event_log,
                            now_ms - log_start_ms,
                            "REARM",
                            "frame=%ld idx=%lld progress=%.0f "
                            "required=%.0f wraps=%d",
                            (long)packet.frame,
                            detector_index,
                            detector.rearm_progress,
                            detector.rearm_required,
                            detector.transition_count
                        );
                    } else if (event_result ==
                               AUTO_EVENT_REVERSED) {
                        auto_cal_log_line(
                            event_log,
                            now_ms - log_start_ms,
                            "DIRECTION_REVERSED",
                            "frame=%ld idx=%lld jump=%+.0f "
                            "expected_sign=%+d received_sign=%+d",
                            (long)packet.frame,
                            event.sample_index,
                            event.jump,
                            detector.direction_sign,
                            event.direction_sign
                        );
                    }
                    if (event_result == AUTO_EVENT_REVERSED) {
                        clear_status_line();
                        fprintf(
                            stderr,
                            "Falha: foi confirmado um salto no sentido "
                            "oposto. Gire continuamente no mesmo sentido.\n"
                        );
                        failure_reported = 1;
                        break;
                    }
                    if (event_result == AUTO_EVENT_TRANSITION &&
                        detector.transition_count >=
                            AUTO_CALIB_TRANSITIONS) {
                        if (motorized) {
                            endpoint_tail_target_index =
                                event.sample_index +
                                endpoint_far_samples;
                            auto_cal_log_line(
                                event_log,
                                now_ms - log_start_ms,
                                "REFERENCE_TAIL",
                                "transition_idx=%lld "
                                "target_idx=%lld duration_ms=%d "
                                "reason=DRIVE_INTERPOLATION_AFTER_LAST_WRAP",
                                event.sample_index,
                                endpoint_tail_target_index,
                                AUTO_CALIB_ENDPOINT_FAR_MS
                            );
                        } else {
                            capture_complete = 1;
                            break;
                        }
                    }
                    if (motorized &&
                        endpoint_tail_target_index >= 0 &&
                        detector_index >=
                            endpoint_tail_target_index) {
                        capture_complete = 1;
                        break;
                    }
                }
            }
            if (failure_reported || capture_complete) {
                break;
            }
        } else if (receive_result < 0) {
            clear_status_line();
            fprintf(
                stderr,
                "Falha no socket durante a autocalibracao.\n"
            );
            auto_cal_log_line(
                event_log,
                now_ms - log_start_ms,
                "SOCKET_ERROR",
                "wsa_error=%d",
                WSAGetLastError()
            );
            failure_reported = 1;
            break;
        }

        if (receive_result == 0 &&
            !stream_history_stale &&
            now_ms - last_data_ms >
                (ULONGLONG)stream_history_reset_ms) {
            auto_cal_reset_stream_history(&detector);
            stream_history_stale = 1;
            auto_cal_log_line(
                event_log,
                now_ms - log_start_ms,
                "STREAM_HISTORY_RESET",
                "last_data_age_ms=%llu threshold_ms=%d",
                (unsigned long long)(
                    now_ms - last_data_ms
                ),
                stream_history_reset_ms
            );
        }
        if (now_ms - last_data_ms > STREAM_TIMEOUT_MS) {
            clear_status_line();
            fprintf(
                stderr,
                "Falha: fluxo ACQDATA ficou %d ms sem amostras. "
                "A captura nao sera reconectada para nao misturar voltas.\n",
                STREAM_TIMEOUT_MS
            );
            auto_cal_log_line(
                event_log,
                now_ms - log_start_ms,
                "DATA_TIMEOUT",
                "timeout_ms=%d last_data_age_ms=%llu",
                STREAM_TIMEOUT_MS,
                (unsigned long long)(
                    now_ms - last_data_ms
                )
            );
            failure_reported = 1;
            break;
        }
        if (now_ms - capture_start_ms >=
            (ULONGLONG)auto_timeout_ms) {
            clear_status_line();
            fprintf(
                stderr,
                "Tempo limite de %d s sem %d transicoes validas.\n",
                auto_timeout_ms / 1000,
                AUTO_CALIB_TRANSITIONS
            );
            auto_cal_log_line(
                event_log,
                now_ms - log_start_ms,
                "TIME_LIMIT",
                "wraps=%d candidates=%d window_waits=%d "
                "rejected=%d samples=%lld "
                "capture_peak_abs_delta=%.0f "
                "capture_peak_abs_window_jump=%.0f threshold=%.0f",
                detector.transition_count,
                detector.candidate_count,
                detector.window_wait_count,
                detector.rejected_count,
                received_samples,
                capture_peak_delta,
                capture_peak_window_jump,
                last_threshold
            );
            failure_reported = 1;
            break;
        }
        if (now_ms >= next_log_ms) {
            auto_cal_log_line(
                event_log,
                now_ms - log_start_ms,
                "STATE",
                "samples=%lld raw=%d filtered=%.0f delta=%+.0f "
                "window_jump=%+.0f peak_abs_delta=%.0f "
                "peak_abs_window_jump=%.0f threshold=%.0f "
                "range=[%d,%d] span=%d typical_step=%.1f "
                "candidate_active=%d candidates=%d "
                "window_waits=%d rejected=%d "
                "wraps=%d armed=%d rearm=%.0f/%.0f "
                "gaps=%lu tolerated=%lu resets=%lu "
                "lost=%llu reordered=%lu "
                "drive_status=%d drive_comm=%d drive_cmd_rpm=%d "
                "drive_pos=%u drive_motor_turns=%.6f "
                "drive_encoder_turns=%.6f drive_errors=%d "
                "drive_age_ms=%llu",
                received_samples,
                (int)last_raw,
                last_filtered,
                last_delta,
                last_window_jump,
                peak_delta,
                peak_window_jump,
                last_threshold,
                detector.have_running_range
                    ? (int)detector.running_min
                    : 0,
                detector.have_running_range
                    ? (int)detector.running_max
                    : 0,
                detector.have_running_range
                    ? (int)detector.running_max -
                        (int)detector.running_min
                    : 0,
                detector.typical_step,
                detector.candidate_active,
                detector.candidate_count,
                detector.window_wait_count,
                detector.rejected_count,
                detector.transition_count,
                detector.armed,
                detector.rearm_progress,
                detector.rearm_required,
                frame_gap_events,
                tolerated_gap_events,
                reset_gap_events,
                lost_packets,
                reordered_packets,
                drive_session.status_seen,
                drive_session.comm_active,
                drive_session.command_rpm,
                drive_session.position_p0b09,
                drive_session.motor_turns,
                motorized &&
                        config->mechanical_ratio > 0.0
                    ? drive_session.motor_turns /
                        config->mechanical_ratio
                    : 0.0,
                drive_session.position_errors,
                drive_session.last_position_age_ms
            );
            peak_delta = 0.0;
            peak_window_jump = 0.0;
            next_log_ms = now_ms + 1000;
        }
        if (now_ms >= next_diagnostic_flush_ms) {
            (void)fflush(diagnostic);
            next_diagnostic_flush_ms =
                now_ms + AUTO_CALIB_DIAGNOSTIC_FLUSH_MS;
        }
        if (now_ms >= next_status_ms) {
            const char *direction =
                detector.direction_sign > 0
                    ? "baixo->alto"
                    : detector.direction_sign < 0
                        ? "alto->baixo"
                        : "aguardando";
            if (motorized) {
                const char *drive_comm =
                    !drive_session.status_seen
                        ? "AGUARD"
                        : drive_session.comm_active
                            ? "OK"
                            : "FALHA";
                double encoder_turns =
                    config->mechanical_ratio > 0.0
                        ? drive_session.motor_turns /
                            config->mechanical_ratio
                        : 0.0;

                status_line(
                    "AUTO %d/%d raw %6d g%lu/%lu | "
                    "DRV %s %+drpm pos%u enc%.2fv err%d | %llus Q",
                    detector.transition_count,
                    AUTO_CALIB_TRANSITIONS,
                    detector.have_previous
                        ? (int)detector.previous
                        : 0,
                    tolerated_gap_events,
                    reset_gap_events,
                    drive_comm,
                    drive_session.command_rpm,
                    drive_session.position_p0b09,
                    encoder_turns,
                    drive_session.position_errors,
                    (unsigned long long)(
                        (now_ms - capture_start_ms) / 1000
                    )
                );
            } else {
                status_line(
                    "AUTO %d/%d raw %6d c%d esp%d rej%d "
                    "dir %s g%lu/%lu | %llus Q",
                    detector.transition_count,
                    AUTO_CALIB_TRANSITIONS,
                    detector.have_previous
                        ? (int)detector.previous
                        : 0,
                    detector.candidate_count,
                    detector.window_wait_count,
                    detector.rejected_count,
                    direction,
                    tolerated_gap_events,
                    reset_gap_events,
                    (unsigned long long)(
                        (now_ms - capture_start_ms) / 1000
                    )
                );
            }
            next_status_ms = now_ms + UPDATE_INTERVAL_MS;
        }
    }

cleanup:
    if (motorized && drive_session.launched) {
        auto_cal_log_line(
            event_log,
            GetTickCount64() - log_start_ms,
            "DRIVE_STOP_REQUEST",
            "started=%d capture_complete=%d stopped=%d",
            drive_session.started,
            capture_complete,
            stop_requested()
        );
        drive_stop_confirmed =
            drive_session_stop(&drive_session);
        auto_cal_log_line(
            event_log,
            GetTickCount64() - log_start_ms,
            drive_stop_confirmed
                ? "DRIVE_STOPPED"
                : "DRIVE_STOP_UNCONFIRMED",
            "confirmed=%d",
            drive_stop_confirmed
        );
        if (!drive_stop_confirmed) {
            failure_reported = 1;
            capture_complete = 0;
            capture_reason = "DRIVE_STOP_UNCONFIRMED";
            final_result = "drive_stop_unconfirmed";
        }
    }
    (void)send_stop(connection);
    finish_status_line();
    if (diagnostic) {
        if (ferror(diagnostic)) {
            close_ok = 0;
        }
        if (fclose(diagnostic) != 0) {
            close_ok = 0;
        }
        diagnostic = NULL;
    }
    if (!close_ok) {
        fprintf(
            stderr,
            "Falha ao finalizar CSV de diagnostico.\n"
        );
        auto_cal_log_line(
            event_log,
            GetTickCount64() - log_start_ms,
            "DIAGNOSTIC_CLOSE_FAILED",
            "path=%s",
            diagnostic_path
        );
        failure_reported = 1;
        final_result = "diagnostic_close_failed";
        goto finalize_event_log;
    }
    if (!capture_complete) {
        if (!stop_requested() && !failure_reported) {
            fprintf(
                stderr,
                "Autocalibracao interrompida antes de %d transicoes.\n",
                AUTO_CALIB_TRANSITIONS
            );
        }
        if (stop_requested()) {
            capture_reason = "USER_CANCELLED";
            final_result = "cancelled";
        } else if (dlg_not_ready) {
            capture_reason = "DLG_NOT_READY";
            final_result = "dlg_not_ready";
        } else if (motorized && !drive_stop_confirmed) {
            capture_reason = "DRIVE_STOP_UNCONFIRMED";
            final_result = "drive_stop_unconfirmed";
        } else if (failure_reported) {
            capture_reason = "RUNTIME_FAILURE";
            final_result = "capture_failed";
        }
        if (received_samples > 0 &&
            detector.candidate_count == 0) {
            auto_cal_log_line(
                event_log,
                GetTickCount64() - log_start_ms,
                "NO_CANDIDATE",
                "capture_peak_abs_delta=%.0f "
                "capture_peak_abs_window_jump=%.0f threshold=%.0f "
                "range=[%d,%d] span=%d typical_step=%.1f "
                "window_waits=%d diagnosis=%s",
                capture_peak_delta,
                capture_peak_window_jump,
                last_threshold,
                detector.have_running_range
                    ? (int)detector.running_min
                    : 0,
                detector.have_running_range
                    ? (int)detector.running_max
                    : 0,
                detector.have_running_range
                    ? (int)detector.running_max -
                        (int)detector.running_min
                    : 0,
                detector.typical_step,
                detector.window_wait_count,
                detector.history_count <
                        AUTO_CALIB_HISTORY_SAMPLES &&
                    capture_peak_window_jump == 0.0
                    ? "NO_COMPLETE_DETECTION_WINDOW"
                    : capture_peak_window_jump <
                            last_threshold
                        ? "WINDOW_JUMP_BELOW_THRESHOLD"
                        : "CHECK_WINDOW_SUPPORT_MAD_AND_INTERVAL"
            );
        }
        auto_cal_log_line(
            event_log,
            GetTickCount64() - log_start_ms,
            "CAPTURE_INCOMPLETE",
            "reason=%s wraps=%d candidates=%d window_waits=%d "
            "rejected=%d samples=%lld",
            capture_reason,
            detector.transition_count,
            detector.candidate_count,
            detector.window_wait_count,
            detector.rejected_count,
            received_samples
        );
        goto finalize_event_log;
    }
    if (received_samples > 0 || lost_packets > 0) {
        double loss_fraction =
            (double)lost_packets /
            ((double)received_samples +
             (double)lost_packets);

        if (loss_fraction >
            AUTO_CALIB_LOSS_WARNING_FRACTION) {
            fprintf(
                stderr,
                "Aviso: %.2f%% dos frames do DLG foram perdidos.\n",
                loss_fraction * 100.0
            );
            auto_cal_log_line(
                event_log,
                GetTickCount64() - log_start_ms,
                "LOSS_WARNING",
                "loss_fraction=%.9f received=%lld lost=%llu "
                "warning_limit=%.3f reject_limit=%.3f",
                loss_fraction,
                received_samples,
                lost_packets,
                AUTO_CALIB_LOSS_WARNING_FRACTION,
                AUTO_CALIB_LOSS_REJECT_FRACTION
            );
        }
        if (loss_fraction >
            AUTO_CALIB_LOSS_REJECT_FRACTION) {
            fprintf(
                stderr,
                "Falha: perda de frames acima do limite de %.0f%%.\n",
                AUTO_CALIB_LOSS_REJECT_FRACTION * 100.0
            );
            auto_cal_log_line(
                event_log,
                GetTickCount64() - log_start_ms,
                "VALIDATION_FAILED",
                "reason=LOSS_FRACTION loss_fraction=%.9f "
                "limit=%.9f",
                loss_fraction,
                AUTO_CALIB_LOSS_REJECT_FRACTION
            );
            failure_reported = 1;
            final_result = "loss_fraction_failed";
            goto finalize_event_log;
        }
    }
    if (motorized) {
        if (!compute_angular_fit_from_csv(
                diagnostic_path,
                drive_session.drive_csv,
                &detector,
                config->mechanical_ratio,
                config->encoder_target_rpm,
                &angular_fit)) {
            fprintf(
                stderr,
                "Falha: nao foi possivel correlacionar os CSVs "
                "do DLG e do Drive. Verifique quatro wraps, P0B-09, "
                "pos_mod e gaps de comunicacao. Leituras ausentes "
                "curtas sao toleradas; lacunas acima de %.1f s nao "
                "sao preenchidas.\n",
                ANGULAR_MAX_DRIVE_GAP_S
            );
            auto_cal_log_line(
                event_log,
                GetTickCount64() - log_start_ms,
                "VALIDATION_FAILED",
                "reason=ANGULAR_CORRELATION_FAILED "
                "dlg_csv=%s drive_csv=%s ratio=%.12g "
                "drive_rows=%lu drive_valid=%lu drive_missing=%lu "
                "drive_invalid=%lu drive_outliers=%lu "
                "drive_max_gap_s=%.6f",
                diagnostic_path,
                drive_session.drive_csv,
                config->mechanical_ratio,
                angular_fit.drive_rows_total,
                angular_fit.drive_valid_samples,
                angular_fit.drive_missing_rows,
                angular_fit.drive_invalid_rows,
                angular_fit.drive_outlier_rows,
                angular_fit.drive_max_valid_gap_s
            );
            failure_reported = 1;
            final_result = "angular_correlation_failed";
            goto finalize_event_log;
        }
        auto_cal_log_line(
            event_log,
            GetTickCount64() - log_start_ms,
            "ANGULAR_FIT",
            "training_slope=%.15g training_intercept=%.15g "
            "operational_slope=%.15g operational_intercept=%.15g "
            "train_bins=%d train_rmse_deg=%.6f "
            "holdout_bins=%d holdout_rmse_deg=%.6f "
            "holdout_p95_deg=%.6f holdout_max_deg=%.6f "
            "filtered_samples=%d filtered_rmse_deg=%.6f "
            "filtered_p95_deg=%.6f filtered_max_deg=%.6f "
            "r2=%.9f ratio_config=%.9f ratio_mean=%.9f "
            "ratio_max_error_fraction=%.9f pos_mod=%u "
            "raw_min=%.1f raw_max=%.1f saturation_samples=%d "
            "drive_rows=%lu drive_valid=%lu drive_missing=%lu "
            "drive_invalid=%lu drive_outliers=%lu "
            "drive_max_gap_s=%.6f",
            angular_fit.training_slope_deg_per_count,
            angular_fit.training_intercept_deg,
            angular_fit.slope_deg_per_count,
            angular_fit.intercept_deg,
            angular_fit.training_bins,
            angular_fit.training_rmse_deg,
            angular_fit.validation_bins,
            angular_fit.validation_rmse_deg,
            angular_fit.validation_p95_deg,
            angular_fit.validation_max_error_deg,
            angular_fit.validation_filtered_samples,
            angular_fit.validation_filtered_rmse_deg,
            angular_fit.validation_filtered_p95_deg,
            angular_fit.validation_filtered_max_error_deg,
            angular_fit.r_squared,
            config->mechanical_ratio,
            angular_fit.measured_ratio_mean,
            angular_fit.measured_ratio_max_error_fraction,
            angular_fit.drive_position_modulus,
            angular_fit.raw_min,
            angular_fit.raw_max,
            angular_fit.saturation_samples,
            angular_fit.drive_rows_total,
            angular_fit.drive_valid_samples,
            angular_fit.drive_missing_rows,
            angular_fit.drive_invalid_rows,
            angular_fit.drive_outlier_rows,
            angular_fit.drive_max_valid_gap_s
        );
        if (!angular_fit_passes_quality(&angular_fit)) {
            fprintf(
                stderr,
                "Calibracao angular REPROVADA; o JSON anterior foi "
                "preservado.\n"
                "  Holdout por grau: RMSE %.3f, P95 %.3f, max %.3f deg\n"
                "  Sinal filtrado:   RMSE %.3f, P95 %.3f, max %.3f deg\n"
                "  Relacao: %.6f (erro max %.3f%%); saturacoes: %d\n"
                "  Drive: %lu/%lu validas; ausentes %lu; invalidas %lu; "
                "outliers %lu; maior gap %.3f s\n"
                "Limites: RMSE <= %.2f, P95 <= %.2f, max <= %.2f deg; "
                "erro da relacao <= %.1f%%; nenhuma saturacao.\n",
                angular_fit.validation_rmse_deg,
                angular_fit.validation_p95_deg,
                angular_fit.validation_max_error_deg,
                angular_fit.validation_filtered_rmse_deg,
                angular_fit.validation_filtered_p95_deg,
                angular_fit.validation_filtered_max_error_deg,
                angular_fit.measured_ratio_mean,
                angular_fit.measured_ratio_max_error_fraction *
                    100.0,
                angular_fit.saturation_samples,
                angular_fit.drive_valid_samples,
                angular_fit.drive_rows_total,
                angular_fit.drive_missing_rows,
                angular_fit.drive_invalid_rows,
                angular_fit.drive_outlier_rows,
                angular_fit.drive_max_valid_gap_s,
                ANGULAR_MAX_RMSE_DEG,
                ANGULAR_MAX_P95_DEG,
                ANGULAR_MAX_ERROR_DEG,
                ANGULAR_MAX_RATIO_ERROR_FRACTION * 100.0
            );
            auto_cal_log_line(
                event_log,
                GetTickCount64() - log_start_ms,
                "VALIDATION_FAILED",
                "reason=ANGULAR_QUALITY "
                "holdout_rmse=%.9f holdout_p95=%.9f "
                "holdout_max=%.9f filtered_rmse=%.9f "
                "filtered_p95=%.9f filtered_max=%.9f "
                "ratio_error=%.9f saturation=%d "
                "drive_rows=%lu drive_valid=%lu drive_missing=%lu "
                "drive_invalid=%lu drive_outliers=%lu "
                "drive_max_gap_s=%.6f",
                angular_fit.validation_rmse_deg,
                angular_fit.validation_p95_deg,
                angular_fit.validation_max_error_deg,
                angular_fit.validation_filtered_rmse_deg,
                angular_fit.validation_filtered_p95_deg,
                angular_fit.validation_filtered_max_error_deg,
                angular_fit.measured_ratio_max_error_fraction,
                angular_fit.saturation_samples,
                angular_fit.drive_rows_total,
                angular_fit.drive_valid_samples,
                angular_fit.drive_missing_rows,
                angular_fit.drive_invalid_rows,
                angular_fit.drive_outlier_rows,
                angular_fit.drive_max_valid_gap_s
            );
            failure_reported = 1;
            final_result = "angular_quality_failed";
            goto finalize_event_log;
        }
        calibration->slope_deg_per_count =
            angular_fit.slope_deg_per_count;
        calibration->intercept_deg =
            angular_fit.intercept_deg;
        calibration->output_is_degrees = 1;
        strncpy_s(
            calibration->source_path,
            sizeof(calibration->source_path),
            config->calib_out_path,
            _TRUNCATE
        );
        if (!write_angular_calibration_file(
                config->calib_out_path,
                config,
                &detector,
                &dlg_handshake,
                received_samples,
                frame_gap_events,
                tolerated_gap_events,
                reset_gap_events,
                lost_packets,
                reordered_packets,
                &motion,
                calibration,
                &angular_fit)) {
            fprintf(
                stderr,
                "Falha ao gravar calibracao angular: %s\n",
                config->calib_out_path
            );
            auto_cal_log_line(
                event_log,
                GetTickCount64() - log_start_ms,
                "JSON_WRITE_FAILED",
                "path=%s",
                config->calib_out_path
            );
            failure_reported = 1;
            final_result = "json_write_failed";
            goto finalize_event_log;
        }
        auto_cal_log_line(
            event_log,
            GetTickCount64() - log_start_ms,
            "SUCCESS",
            "unit=deg slope=%.15g intercept=%.15g "
            "holdout_rmse=%.6f filtered_rmse=%.6f json=%s",
            calibration->slope_deg_per_count,
            calibration->intercept_deg,
            angular_fit.validation_rmse_deg,
            angular_fit.validation_filtered_rmse_deg,
            config->calib_out_path
        );
        final_result = "success";
        operation_success = 1;
        goto finalize_event_log;
    }
    if (!auto_cal_finalize(
            &detector,
            &raw_low,
            &raw_high,
            &maximum_relative_deviation)) {
        fprintf(
            stderr,
            "Falha: os extremos das %d transicoes nao foram "
            "consistentes o suficiente.\n",
            AUTO_CALIB_TRANSITIONS
        );
        auto_cal_log_line(
            event_log,
            GetTickCount64() - log_start_ms,
            "VALIDATION_FAILED",
            "reason=TRANSITIONS_INCONSISTENT "
            "low=[%.0f,%.0f,%.0f,%.0f] "
            "high=[%.0f,%.0f,%.0f,%.0f] "
            "jump=[%+.0f,%+.0f,%+.0f,%+.0f]",
            detector.raw_lows[0],
            detector.raw_lows[1],
            detector.raw_lows[2],
            detector.raw_lows[3],
            detector.raw_highs[0],
            detector.raw_highs[1],
            detector.raw_highs[2],
            detector.raw_highs[3],
            detector.jumps[0],
            detector.jumps[1],
            detector.jumps[2],
            detector.jumps[3]
        );
        failure_reported = 1;
        final_result = "validation_failed";
        goto finalize_event_log;
    }
    if (raw_low <= -32700.0 || raw_high >= 32700.0) {
        fprintf(
            stderr,
            "Falha: extremos proximos da saturacao do A/D "
            "(baixo %.1f, alto %.1f).\n",
            raw_low,
            raw_high
        );
        auto_cal_log_line(
            event_log,
            GetTickCount64() - log_start_ms,
            "VALIDATION_FAILED",
            "reason=ADC_SATURATION raw_low=%.1f raw_high=%.1f",
            raw_low,
            raw_high
        );
        failure_reported = 1;
        final_result = "validation_failed";
        goto finalize_event_log;
    }

    calibration->slope_ma_per_count =
        16.0 / (raw_high - raw_low);
    calibration->intercept_ma =
        4.0 -
        calibration->slope_ma_per_count * raw_low;
    strncpy_s(
        calibration->source_path,
        sizeof(calibration->source_path),
        config->calib_out_path,
        _TRUNCATE
    );
    if (!write_auto_calibration_file(
            config->calib_out_path,
            config,
            &detector,
            raw_low,
            raw_high,
            maximum_relative_deviation,
            received_samples,
            frame_gap_events,
            tolerated_gap_events,
            reset_gap_events,
            lost_packets,
            reordered_packets,
            &motion,
            calibration)) {
        fprintf(
            stderr,
            "Falha ao gravar normalizacao: %s\n",
            config->calib_out_path
        );
        auto_cal_log_line(
            event_log,
            GetTickCount64() - log_start_ms,
            "JSON_WRITE_FAILED",
            "path=%s",
            config->calib_out_path
        );
        failure_reported = 1;
        final_result = "json_write_failed";
        goto finalize_event_log;
    }
    auto_cal_log_line(
        event_log,
        GetTickCount64() - log_start_ms,
        "SUCCESS",
        "raw_low=%.1f raw_high=%.1f slope=%.12g "
        "intercept=%.12g max_relative_deviation=%.6f json=%s",
        raw_low,
        raw_high,
        calibration->slope_ma_per_count,
        calibration->intercept_ma,
        maximum_relative_deviation,
        config->calib_out_path
    );
    final_result = "success";
    operation_success = 1;

finalize_event_log:
    if (event_log &&
        !auto_cal_close_event_log(
            &event_log,
            GetTickCount64() - log_start_ms,
            final_result,
            capture_complete,
            motorized ? drive_stop_confirmed : 1,
            failure_reported,
            received_samples,
            &detector,
            frame_gap_events,
            tolerated_gap_events,
            reset_gap_events,
            lost_packets,
            reordered_packets)) {
        close_ok = 0;
        fprintf(
            stderr,
            "Falha ao finalizar log de eventos: %s\n",
            event_log_path
        );
    }
    if (!operation_success) {
        printf("Diagnostico preservado em %s\n", diagnostic_path);
        printf("Log de eventos preservado em %s\n", event_log_path);
        return 0;
    }
    if (!close_ok) {
        fprintf(
            stderr,
            "Aviso: a calibracao foi salva, mas o log pode "
            "estar incompleto.\n"
        );
    }

    if (motorized) {
        printf(
            "Autocalibracao angular concluida e APROVADA.\n"
            "Modelo operacional: graus = %.12g * raw %+.12g "
            "(saida normalizada em 0..360)\n"
            "Treino: %d bins, RMSE %.3f deg\n"
            "Holdout por grau: %d bins, RMSE %.3f, "
            "P95 %.3f, max %.3f deg\n"
            "Holdout com mediana de 9: %d amostras, RMSE %.3f, "
            "P95 %.3f, max %.3f deg\n"
            "Relacao configurada %.6f; medida %.6f; "
            "erro max %.3f%%; saturacoes %d\n"
            "Drive: %lu/%lu validas; ausentes %lu; invalidas %lu; "
            "outliers %lu; maior gap %.3f s\n"
            "JSON: %s\n"
            "CSV DLG: %s\n"
            "CSV Drive: %s\n"
            "Log de eventos: %s\n\n",
            calibration->slope_deg_per_count,
            calibration->intercept_deg,
            angular_fit.training_bins,
            angular_fit.training_rmse_deg,
            angular_fit.validation_bins,
            angular_fit.validation_rmse_deg,
            angular_fit.validation_p95_deg,
            angular_fit.validation_max_error_deg,
            angular_fit.validation_filtered_samples,
            angular_fit.validation_filtered_rmse_deg,
            angular_fit.validation_filtered_p95_deg,
            angular_fit.validation_filtered_max_error_deg,
            config->mechanical_ratio,
            angular_fit.measured_ratio_mean,
            angular_fit.measured_ratio_max_error_fraction * 100.0,
            angular_fit.saturation_samples,
            angular_fit.drive_valid_samples,
            angular_fit.drive_rows_total,
            angular_fit.drive_missing_rows,
            angular_fit.drive_invalid_rows,
            angular_fit.drive_outlier_rows,
            angular_fit.drive_max_valid_gap_s,
            config->calib_out_path,
            diagnostic_path,
            drive_session.drive_csv,
            event_log_path
        );
        return 1;
    }

    printf(
        "Autocalibracao nominal concluida.\n"
        "Sentido eletrico dos saltos: %s\n",
        detector.direction_sign > 0
            ? "baixo -> alto"
            : "alto -> baixo"
    );
    for (i = 0; i < AUTO_CALIB_TRANSITIONS; ++i) {
        printf(
            "  Transicao %d: baixo %.1f, alto %.1f, salto %+.1f\n",
            i + 1,
            detector.raw_lows[i],
            detector.raw_highs[i],
            detector.jumps[i]
        );
    }
    printf(
        "Extremos finais: baixo %.1f = 4 mA; alto %.1f = 20 mA\n"
        "Desvio relativo maximo entre transicoes: %.2f%%\n"
        "Fluxo UDP: %lu gap(s), %llu perdido(s), %lu reordenado(s)\n"
        "Fit: mA = %.12g * raw + %.12g\n"
        "JSON: %s\n"
        "CSV de diagnostico: %s\n"
        "Log de eventos: %s\n"
        "ATENCAO: resultado nominal e nao rastreavel.\n\n",
        raw_low,
        raw_high,
        maximum_relative_deviation * 100.0,
        frame_gap_events,
        lost_packets,
        reordered_packets,
        calibration->slope_ma_per_count,
        calibration->intercept_ma,
        config->calib_out_path,
        diagnostic_path,
        event_log_path
    );
    return 1;
}

static int round_to_centi_ma(double current_ma, long *result)
{
    double scaled = current_ma * 100.0;
    double rounded;

    if (!result ||
        !(scaled >= (double)LONG_MIN + 1.0 &&
          scaled <= (double)LONG_MAX - 1.0)) {
        return 0;
    }
    rounded = scaled >= 0.0 ? scaled + 0.5 : scaled - 0.5;
    *result = (long)rounded;
    return 1;
}

static double current_ma_to_degrees(double current_ma)
{
    if (current_ma <= ENCODER_MIN_CURRENT_MA) {
        return 0.0;
    }
    if (current_ma >= ENCODER_MAX_CURRENT_MA) {
        return ENCODER_FULL_SCALE_DEG;
    }
    return
        (current_ma - ENCODER_MIN_CURRENT_MA) *
        ENCODER_FULL_SCALE_DEG /
        (ENCODER_MAX_CURRENT_MA - ENCODER_MIN_CURRENT_MA);
}

static int monitor_current(
    const AppConfig *config,
    DlgConnection *connection,
    const Calibration *calibration)
{
    long previous_display_value = LONG_MIN;

    while (!stop_requested()) {
        LARGE_INTEGER frequency;
        LARGE_INTEGER next_update;
        ULONGLONG last_data_ms;
        int16_t recent_raw[MONITOR_FILTER_SAMPLES];
        int recent_count = 0;
        int recent_next = 0;
        int stream_failed = 0;

        memset(recent_raw, 0, sizeof(recent_raw));
        if (!start_stream_until_data(config, connection, calibration)) {
            return 0;
        }
        previous_display_value = LONG_MIN;
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&next_update);
        next_update.QuadPart +=
            (frequency.QuadPart * UPDATE_INTERVAL_MS) / 1000;
        last_data_ms = GetTickCount64();

        while (!stop_requested()) {
            PktData packet;
            int packet_bytes = 0;
            int result;
            LARGE_INTEGER now;

            if (poll_quit_key()) {
                break;
            }
            result = receive_data_packet(
                connection,
                &packet,
                &packet_bytes,
                NULL
            );
            if (result > 0) {
                if (add_packet_samples_to_ring(
                        &packet,
                        packet_bytes,
                        recent_raw,
                        MONITOR_FILTER_SAMPLES,
                        &recent_count,
                        &recent_next)) {
                    last_data_ms = GetTickCount64();
                }
            } else if (result < 0) {
                stream_failed = 1;
                break;
            }

            if (GetTickCount64() - last_data_ms > STREAM_TIMEOUT_MS) {
                stream_failed = 1;
                break;
            }

            QueryPerformanceCounter(&now);
            if (now.QuadPart >= next_update.QuadPart) {
                if (recent_count > 0) {
                    double raw_median =
                        median_int16(
                            recent_raw,
                            recent_count
                        );
                    if (calibration->output_is_degrees) {
                        double angle_degrees = normalize_degrees(
                            calibration->slope_deg_per_count *
                                raw_median +
                            calibration->intercept_deg
                        );
                        double nominal_ma =
                            ENCODER_MIN_CURRENT_MA +
                            angle_degrees *
                                (ENCODER_MAX_CURRENT_MA -
                                 ENCODER_MIN_CURRENT_MA) /
                                ENCODER_FULL_SCALE_DEG;
                        long angle_centidegrees;

                        if (!round_to_centi_ma(
                                angle_degrees,
                                &angle_centidegrees)) {
                            clear_status_line();
                            fprintf(
                                stderr,
                                "Calibracao produziu angulo fora da faixa "
                                "numerica; monitor interrompido.\n"
                            );
                            return 0;
                        }
                        if (angle_centidegrees !=
                            previous_display_value) {
                            status_line(
                                "CH3 | angulo %7.2f graus | "
                                "mA nominal %6.2f | "
                                "raw mediana %9.2f | Q encerra",
                                angle_centidegrees / 100.0,
                                nominal_ma,
                                raw_median
                            );
                            previous_display_value =
                                angle_centidegrees;
                        }
                    } else {
                        double current_ma =
                            calibration->slope_ma_per_count *
                                raw_median +
                            calibration->intercept_ma;
                        long current_centi_ma;

                        if (!round_to_centi_ma(
                                current_ma,
                                &current_centi_ma)) {
                            clear_status_line();
                            fprintf(
                                stderr,
                                "Calibracao produziu corrente fora da faixa "
                                "numerica; monitor interrompido.\n"
                            );
                            return 0;
                        }
                        if (current_centi_ma !=
                            previous_display_value) {
                            double displayed_ma =
                                current_centi_ma / 100.0;
                            double angle_degrees =
                                current_ma_to_degrees(displayed_ma);

                            status_line(
                                "CH3 | corrente %8.2f mA | "
                                "angulo nominal %7.2f graus | "
                                "raw mediana %9.2f | Q encerra",
                                displayed_ma,
                                angle_degrees,
                                raw_median
                            );
                            previous_display_value =
                                current_centi_ma;
                        }
                    }
                }
                next_update = now;
                next_update.QuadPart +=
                    (frequency.QuadPart * UPDATE_INTERVAL_MS) / 1000;
            }
        }

        (void)send_stop(connection);
        if (stop_requested()) {
            break;
        }
        if (stream_failed) {
            status_line("CH3 | fluxo interrompido; reconectando...");
            Sleep(RETRY_DELAY_MS);
        }
    }
    return 1;
}

static int test_angular_fit_pipeline(int reverse_direction)
{
    const int sample_rate_hz = 200;
    const int samples_per_revolution = 12000;
    const int first_wrap_index = 2000;
    const unsigned int position_modulus = 10000U;
    const double mechanical_ratio = 4.0;
    const int total_dlg_samples =
        first_wrap_index +
        ANGULAR_COMPLETE_REVOLUTIONS *
            samples_per_revolution +
        100;
    AutoCalDetector detector;
    AngularFit fit;
    LARGE_INTEGER qpc_frequency;
    FILE *dlg_file = NULL;
    FILE *drive_file = NULL;
    char temp_directory[MAX_PATH];
    char dlg_path[MAX_PATH];
    char drive_path[MAX_PATH];
    long long qpc_base;
    int i;
    int ok = 0;

    memset(&detector, 0, sizeof(detector));
    memset(&fit, 0, sizeof(fit));
    if (!QueryPerformanceFrequency(&qpc_frequency) ||
        qpc_frequency.QuadPart <= 0 ||
        GetTempPathA(
            (DWORD)sizeof(temp_directory),
            temp_directory) == 0 ||
        GetTempFileNameA(
            temp_directory,
            "dle",
            0,
            dlg_path) == 0 ||
        GetTempFileNameA(
            temp_directory,
            "dra",
            0,
            drive_path) == 0) {
        return 0;
    }
    if (fopen_s(&dlg_file, dlg_path, "wb") != 0 ||
        !dlg_file ||
        fopen_s(&drive_file, drive_path, "wb") != 0 ||
        !drive_file) {
        goto cleanup;
    }
    qpc_base = qpc_frequency.QuadPart * 10LL;
    fprintf(
        dlg_file,
        "rx_idx;detector_idx;t_ms;t_qpc;frame;"
        "frame_delta;frame_gap;raw\n"
    );
    for (i = 0; i < total_dlg_samples; ++i) {
        int relative = i - first_wrap_index;
        int phase = relative % samples_per_revolution;
        double progress;
        double reference_degrees;
        double raw_double;
        int raw;
        long long qpc =
            qpc_base +
            (long long)i * qpc_frequency.QuadPart /
                sample_rate_hz;

        if (phase < 0) {
            phase += samples_per_revolution;
        }
        progress =
            (double)phase * ENCODER_FULL_SCALE_DEG /
            (double)samples_per_revolution;
        reference_degrees =
            reverse_direction
                ? (phase == 0
                    ? ENCODER_FULL_SCALE_DEG
                    : ENCODER_FULL_SCALE_DEG - progress)
                : progress;
        raw_double =
            -30000.0 + 50.0 * reference_degrees +
            (double)((i % 7) - 3);
        if (i > first_wrap_index &&
            i % 4093 == 0) {
            raw_double += 300.0;
        }
        raw = (int)(
            raw_double >= 0.0
                ? raw_double + 0.5
                : raw_double - 0.5
        );
        fprintf(
            dlg_file,
            "%d;%d;%llu;%lld;%d;1;0;%d\n",
            i,
            i,
            (unsigned long long)(
                (long long)i * 1000LL /
                sample_rate_hz
            ),
            qpc,
            i,
            raw
        );
    }
    fprintf(
        drive_file,
        "idx,t_qpc,t_s,pos,rpm,pos_err,rpm_err,pos_mod\n"
    );
    {
        int total_drive_slots =
            total_dlg_samples * 10 /
                sample_rate_hz +
            2;
        for (i = 0; i <= total_drive_slots; ++i) {
            double time_s = (double)i / 10.0;
            double counts =
                time_s *
                (double)position_modulus *
                mechanical_ratio /
                60.0;
            long long unwrapped =
                (long long)llround(
                    reverse_direction ? -counts : counts
                );
            long long position =
                unwrapped % (long long)position_modulus;
            long long qpc =
                qpc_base +
                (long long)i *
                    qpc_frequency.QuadPart /
                    10LL;

            if (position < 0) {
                position += position_modulus;
            }
            if (i >= 99 && i <= 101) {
                fprintf(
                    drive_file,
                    "%d,%lld,%.6f,NULL,NULL,1,0,%u\n",
                    i,
                    qpc,
                    time_s,
                    position_modulus
                );
                continue;
            }
            if (i == 400) {
                long long expected_direction =
                    reverse_direction ? -1LL : 1LL;
                position -= expected_direction * 167LL;
                position %= (long long)position_modulus;
                if (position < 0) {
                    position += position_modulus;
                }
            }
            if (i == 500) {
                position =
                    (position +
                     (long long)(position_modulus / 3U)) %
                    (long long)position_modulus;
            }
            fprintf(
                drive_file,
                "%d,%lld,%.6f,%lld,%d,0,0,%u\n",
                i,
                qpc,
                time_s,
                position,
                reverse_direction ? -4 : 4,
                position_modulus
            );
        }
    }
    if (fclose(dlg_file) != 0) {
        dlg_file = NULL;
        goto cleanup;
    }
    dlg_file = NULL;
    if (fclose(drive_file) != 0) {
        drive_file = NULL;
        goto cleanup;
    }
    drive_file = NULL;

    detector.transition_count = AUTO_CALIB_TRANSITIONS;
    detector.direction_sign = reverse_direction ? 1 : -1;
    detector.motion_sign = -detector.direction_sign;
    for (i = 0; i < AUTO_CALIB_TRANSITIONS; ++i) {
        detector.transition_indices[i] =
            first_wrap_index +
            (long long)i * samples_per_revolution;
    }
    if (!compute_angular_fit_from_csv(
            dlg_path,
            drive_path,
            &detector,
            mechanical_ratio,
            DEFAULT_ENCODER_RPM,
            &fit) ||
        !angular_fit_passes_quality(&fit) ||
        fit.drive_position_modulus != position_modulus ||
        fit.training_bins <
            2 * ANGULAR_MIN_VALID_BINS ||
        fit.validation_bins < ANGULAR_MIN_VALID_BINS ||
        auto_cal_abs(
            fit.slope_deg_per_count - 0.02
        ) > 0.0005 ||
        auto_cal_abs(
            fit.measured_ratio_mean -
                mechanical_ratio
        ) > 0.001 ||
        fit.drive_missing_rows != 3UL ||
        fit.drive_outlier_rows != 2UL ||
        fit.drive_invalid_rows != 0UL ||
        fit.drive_max_valid_gap_s < 0.39 ||
        fit.drive_max_valid_gap_s > 0.41 ||
        fit.validation_filtered_samples < 1000) {
        goto cleanup;
    }
    {
        AngularDriveSample gap_samples[2];
        double interpolated = 0.0;
        long long maximum_gap_qpc = (long long)(
            (double)qpc_frequency.QuadPart *
            ANGULAR_MAX_DRIVE_GAP_S
        );

        memset(gap_samples, 0, sizeof(gap_samples));
        gap_samples[0].slot_index = 0;
        gap_samples[0].qpc = qpc_base;
        gap_samples[0].unwrapped = 0;
        gap_samples[1].slot_index = 6;
        gap_samples[1].qpc =
            qpc_base + qpc_frequency.QuadPart / 2;
        gap_samples[1].unwrapped = 100;
        if (!interpolate_drive_unwrapped(
                gap_samples,
                2,
                qpc_base + qpc_frequency.QuadPart / 4,
                maximum_gap_qpc,
                &interpolated) ||
            auto_cal_abs(interpolated - 50.0) > 1.0e-9) {
            goto cleanup;
        }
        gap_samples[1].qpc =
            qpc_base +
            qpc_frequency.QuadPart * 6LL / 10LL;
        if (interpolate_drive_unwrapped(
                gap_samples,
                2,
                qpc_base + qpc_frequency.QuadPart * 3LL / 10LL,
                maximum_gap_qpc,
                &interpolated)) {
            goto cleanup;
        }
    }
    {
        AngularFit rejected = fit;
        rejected.saturation_samples = 1;
        if (angular_fit_passes_quality(&rejected)) {
            goto cleanup;
        }
    }
    if (auto_cal_abs(normalize_degrees(-1.0) - 359.0) >
            1.0e-9 ||
        auto_cal_abs(
            circular_error_degrees(359.0, 1.0) + 2.0
        ) > 1.0e-9) {
        goto cleanup;
    }
    ok = 1;

cleanup:
    if (dlg_file) {
        (void)fclose(dlg_file);
    }
    if (drive_file) {
        (void)fclose(drive_file);
    }
    (void)DeleteFileA(dlg_path);
    (void)DeleteFileA(drive_path);
    return ok;
}

static int test_calibration_loader(void)
{
    const char *angular_json =
        "{\"purpose\":\"encoder_ch3_angle_deg\",\"unit\":\"deg\","
        "\"channels\":[3],\"channel\":\"CH3\",\"tSensor\":1,"
        "\"iGain\":1,\"iLPF\":0,\"iSensPwr\":1,"
        "\"fInputDCImp\":1,\"fInputACImp\":0,"
        "\"fit\":{\"slope\":0.02,\"intercept\":600.0},"
        "\"quality\":{\"accepted\":true}}";
    const char *current_json =
        "{\"purpose\":\"encoder_ch3_current_ma\",\"unit\":\"mA\","
        "\"channels\":[3],\"channel\":\"CH3\",\"tSensor\":1,"
        "\"iGain\":1,\"iLPF\":0,\"iSensPwr\":1,"
        "\"fInputDCImp\":1,\"fInputACImp\":0,"
        "\"fit\":{\"slope\":0.001,\"intercept\":10.0}}";
    const char *legacy_x10_json =
        "{\"purpose\":\"encoder_ch3_current_ma\",\"unit\":\"mA\","
        "\"channels\":[3],\"channel\":\"CH3\",\"tSensor\":1,"
        "\"iGain\":2,\"iLPF\":0,\"iSensPwr\":1,"
        "\"fInputDCImp\":1,\"fInputACImp\":0,"
        "\"fit\":{\"slope\":0.001,\"intercept\":10.0}}";
    char temp_directory[MAX_PATH];
    char path[MAX_PATH];
    Calibration calibration;
    FILE *file = NULL;
    int ok = 0;

    path[0] = '\0';
    if (GetTempPathA(
            (DWORD)sizeof(temp_directory),
            temp_directory) == 0 ||
        GetTempFileNameA(
            temp_directory,
            "cal",
            0,
            path) == 0) {
        return 0;
    }
    if (fopen_s(&file, path, "wb") != 0 || !file) {
        goto cleanup;
    }
    fputs(angular_json, file);
    if (fclose(file) != 0) {
        file = NULL;
        goto cleanup;
    }
    file = NULL;
    if (load_calibration_file(path, 1, &calibration) != 1 ||
        !calibration.output_is_degrees ||
        auto_cal_abs(
            calibration.slope_deg_per_count - 0.02
        ) > 1.0e-12 ||
        auto_cal_abs(
            calibration.intercept_deg - 600.0
        ) > 1.0e-12) {
        goto cleanup;
    }
    if (fopen_s(&file, path, "wb") != 0 || !file) {
        goto cleanup;
    }
    fputs(current_json, file);
    if (fclose(file) != 0) {
        file = NULL;
        goto cleanup;
    }
    file = NULL;
    if (load_calibration_file(path, 1, &calibration) != 1 ||
        calibration.output_is_degrees ||
        auto_cal_abs(
            calibration.slope_ma_per_count - 0.001
        ) > 1.0e-12) {
        goto cleanup;
    }
    if (fopen_s(&file, path, "wb") != 0 || !file) {
        goto cleanup;
    }
    fputs(legacy_x10_json, file);
    if (fclose(file) != 0) {
        file = NULL;
        goto cleanup;
    }
    file = NULL;
    if (load_calibration_file(path, 1, &calibration) != -1) {
        goto cleanup;
    }
    ok = 1;

cleanup:
    if (file) {
        (void)fclose(file);
    }
    if (path[0]) {
        (void)DeleteFileA(path);
    }
    return ok;
}

static int run_self_test(void)
{
    Calibration calibration;
    DriveSession drive_status;
    HandshakeState handshake;
    PktResponse response;
    PktSetChannel channel_response;
    PktData data_packet;
    volatile size_t packet_header_size = sizeof(PktHdr);
    volatile size_t response_size = sizeof(PktResponse);
    volatile size_t setup_size = sizeof(PktAcqSetup);
    volatile size_t data_header_size = offsetof(PktData, samples);
    volatile size_t channel_config_size = sizeof(PktSetChannel);
    const char *json =
        "{\"purpose\":\"encoder_ch3_current_ma\",\"unit\":\"mA\","
        "\"channels\":[3],\"channel\":\"CH3\",\"tSensor\":1,"
        "\"iGain\":1,\"iLPF\":0,\"iSensPwr\":1,"
        "\"fInputDCImp\":1,\"fInputACImp\":0,"
        "\"fit\":{\"slope\":0.004,\"intercept\":0.1}}";
    double raw1 = 1000.0;
    double raw2 = 5000.0;
    double ref1 = 4.0;
    double ref2 = 20.0;
    double calculated;
    double packet_sum = 0.0;
    double parsed_number = 0.0;
    char sidecar_path[MAX_PATH];
    int16_t monitor_ring[MONITOR_FILTER_SAMPLES];
    int16_t trim_values[10] = {
        -1000, 0, 10, 20, 30,
        40, 50, 60, 70, 1000
    };
    long long packet_sample_count = 0;
    long rounded_ma = 0;
    int drive_rpm = 0;
    int monitor_count = 0;
    int monitor_next = 0;
    int parsed_int = -1;
    int failures = 0;

    calibration_init_defaults(&calibration);
    if (calibration.gain_index != ENCODER_GAIN_INDEX ||
        calibration.lpf_index != DEFAULT_LPF_INDEX ||
        calibration.sensor_power_index !=
            DEFAULT_SENSPWR_INDEX ||
        calibration.input_dc_impedance !=
            DEFAULT_INPUT_DC_IMPEDANCE ||
        calibration.input_ac_impedance !=
            DEFAULT_INPUT_AC_IMPEDANCE) {
        ++failures;
    }
    drive_session_init(&drive_status);
    memset(monitor_ring, 0, sizeof(monitor_ring));
    calibration.slope_ma_per_count = (ref2 - ref1) / (raw2 - raw1);
    calibration.intercept_ma =
        ref1 - calibration.slope_ma_per_count * raw1;
    drive_session_observe_line(
        &drive_status,
        "STATUS_DRIVE comm_active=1 cmd_rpm=4 "
        "pos_p0b09=12345 unwrapped_counts=65536 "
        "motor_turns=1.000000000 errors_total=2 last_age_ms=25"
    );
    if (!drive_status.status_seen ||
        !drive_status.comm_active ||
        drive_status.command_rpm != 4 ||
        drive_status.position_p0b09 != 12345U ||
        drive_status.unwrapped_counts != 65536LL ||
        auto_cal_abs(drive_status.motor_turns - 1.0) > 1.0e-9 ||
        drive_status.position_errors != 2 ||
        drive_status.last_position_age_ms != 25ULL) {
        ++failures;
    }

    calculated =
        calibration.slope_ma_per_count * raw1 +
        calibration.intercept_ma;
    if (calculated < 3.999999 || calculated > 4.000001) {
        ++failures;
    }
    calculated =
        calibration.slope_ma_per_count * raw2 +
        calibration.intercept_ma;
    if (calculated < 19.999999 || calculated > 20.000001) {
        ++failures;
    }
    if (!round_to_centi_ma(12.345, &rounded_ma) ||
        rounded_ma != 1235 ||
        !round_to_centi_ma(-1.235, &rounded_ma) ||
        rounded_ma != -124 ||
        round_to_centi_ma(1.0e100, &rounded_ma)) {
        ++failures;
    }
    if (auto_cal_abs(
            auto_cal_trimmed_mean(
                trim_values,
                (int)(sizeof(trim_values) /
                    sizeof(trim_values[0]))
            ) -
            35.0) > 1.0e-9) {
        ++failures;
    }
    if (auto_cal_abs(current_ma_to_degrees(3.0)) > 1.0e-9 ||
        auto_cal_abs(current_ma_to_degrees(4.0)) > 1.0e-9 ||
        auto_cal_abs(
            current_ma_to_degrees(8.0) - 90.0
        ) > 1.0e-9 ||
        auto_cal_abs(
            current_ma_to_degrees(12.0) - 180.0
        ) > 1.0e-9 ||
        auto_cal_abs(
            current_ma_to_degrees(20.0) - 360.0
        ) > 1.0e-9 ||
        auto_cal_abs(
            current_ma_to_degrees(21.0) - 360.0
        ) > 1.0e-9) {
        ++failures;
    }
    if (packet_header_size != 4 ||
        response_size != 6 ||
        setup_size != 36 ||
        data_header_size != 32 ||
        channel_config_size != 24) {
        ++failures;
    }
    if (!channel_in_header(json, ENCODER_CHANNEL) ||
        !channel_matches(json, ENCODER_CHANNEL) ||
        !json_string_value_equals(json, "\"unit\"", "mA") ||
        !json_int(json, "\"tSensor\"", &parsed_int) ||
        parsed_int != SENSOR_CURRENT ||
        !json_number(json, "\"slope\"", &parsed_number) ||
        parsed_number < 0.003999999 ||
        parsed_number > 0.004000001) {
        ++failures;
    }
    if (!valid_drive_port("COM5") ||
        !valid_drive_port("com10") ||
        valid_drive_port("COM0") ||
        valid_drive_port("COM257") ||
        valid_drive_port("5") ||
        !compute_drive_command_rpm(
            1.0,
            4.0,
            1,
            &drive_rpm) ||
        drive_rpm != 4 ||
        !compute_drive_command_rpm(
            1.0,
            4.0,
            -1,
            &drive_rpm) ||
        drive_rpm != -4 ||
        compute_drive_command_rpm(
            1.0,
            0.0,
            1,
            &drive_rpm)) {
        ++failures;
    }
    if (!build_auto_cal_sidecar_path(
            "out\\encoder_CH3_mA.json",
            "_autocal_events.log",
            sidecar_path,
            sizeof(sidecar_path)) ||
        strcmp(
            sidecar_path,
            "out\\encoder_CH3_mA_autocal_events.log"
        ) != 0) {
        ++failures;
    }
    if (!build_auto_cal_sidecar_path(
            "out\\encoder_CH3_mA.json",
            "_autocal_drive\\drive.csv",
            sidecar_path,
            sizeof(sidecar_path)) ||
        strcmp(
            sidecar_path,
            "out\\encoder_CH3_mA_autocal_drive\\drive.csv"
        ) != 0) {
        ++failures;
    }

    memset(&handshake, 0, sizeof(handshake));
    memset(&response, 0, sizeof(response));
    response.code = OP_SETCHCFG_R;
    response.error = 7;
    observe_command_response(
        &response,
        (int)sizeof(response),
        &handshake
    );
    if (handshake.saw_set_channel_ack ||
        !handshake.command_rejected ||
        handshake.rejected_code != OP_SETCHCFG_R ||
        handshake.error_code != 7) {
        ++failures;
    }
    memset(&handshake, 0, sizeof(handshake));
    response.error = 0;
    observe_command_response(
        &response,
        (int)sizeof(response),
        &handshake
    );
    if (!handshake.saw_set_channel_ack ||
        handshake.command_rejected) {
        ++failures;
    }
    memset(&handshake, 0, sizeof(handshake));
    memset(&channel_response, 0, sizeof(channel_response));
    handshake.channel_expected.code = OP_GETCHCFG_R;
    handshake.channel_expected.channel = ENCODER_CHANNEL;
    handshake.channel_expected.sensor_type = SENSOR_CURRENT;
    handshake.channel_expected.gain_index = ENCODER_GAIN_INDEX;
    handshake.channel_expected.lpf_index = DEFAULT_LPF_INDEX;
    handshake.channel_expected.sensor_power_index =
        DEFAULT_SENSPWR_INDEX;
    handshake.channel_expected.input_dc_impedance =
        DEFAULT_INPUT_DC_IMPEDANCE;
    handshake.channel_expected.input_ac =
        DEFAULT_INPUT_AC_IMPEDANCE;
    channel_response = handshake.channel_expected;
    channel_response.code = OP_GETCHCFG_R;
    observe_command_response(
        &channel_response,
        (int)sizeof(channel_response),
        &handshake
    );
    if (!handshake.saw_get_channel_response ||
        !handshake.get_channel_matches ||
        handshake.command_rejected) {
        ++failures;
    }
    memset(&handshake, 0, sizeof(handshake));
    handshake.channel_expected = channel_response;
    channel_response.gain_index = 2;
    observe_command_response(
        &channel_response,
        (int)sizeof(channel_response),
        &handshake
    );
    if (!handshake.saw_get_channel_response ||
        handshake.get_channel_matches) {
        ++failures;
    }
    memset(&handshake, 0, sizeof(handshake));
    response.code = OP_GETCHCFG_R;
    response.error = 0;
    observe_command_response(
        &response,
        (int)sizeof(response),
        &handshake
    );
    if (handshake.saw_get_channel_response ||
        handshake.get_channel_matches) {
        ++failures;
    }
    initialize_handshake(
        &handshake,
        &calibration,
        1U,
        0
    );
    channel_response = handshake.channel_expected;
    channel_response.code = OP_GETCHCFG;
    observe_command_response(
        &channel_response,
        (int)sizeof(channel_response),
        &handshake
    );
    if (!handshake.saw_get_channel_response ||
        !handshake.get_channel_matches) {
        ++failures;
    }
    memset(&handshake, 0, sizeof(handshake));
    handshake.fallback_used = 1;
    handshake.valid_data_packets = FIRST_DATA_PACKETS;
    if (!handshake_data_is_ready(&handshake)) {
        ++failures;
    }
    handshake.valid_data_packets = FIRST_DATA_PACKETS - 1;
    if (handshake_data_is_ready(&handshake)) {
        ++failures;
    }
    handshake.valid_data_packets = FIRST_DATA_PACKETS;
    handshake.saw_get_channel_response = 1;
    handshake.get_channel_matches = 0;
    if (handshake_data_is_ready(&handshake)) {
        ++failures;
    }

    memset(&data_packet, 0, sizeof(data_packet));
    memset(&handshake, 0, sizeof(handshake));
    data_packet.sample_freq = 200.0f;
    data_packet.frame = 10;
    observe_startup_data_packet(&handshake, &data_packet, 200);
    observe_startup_data_packet(&handshake, &data_packet, 200);
    if (handshake.valid_data_packets != 1 ||
        handshake.data_frame_rejects != 1) {
        ++failures;
    }
    data_packet.frame = 9;
    observe_startup_data_packet(&handshake, &data_packet, 200);
    if (handshake.valid_data_packets != 1 ||
        handshake.data_frame_rejects != 2) {
        ++failures;
    }
    data_packet.frame = 10;
    observe_startup_data_packet(&handshake, &data_packet, 200);
    data_packet.frame = 11;
    observe_startup_data_packet(&handshake, &data_packet, 200);
    if (!handshake_data_is_ready(&handshake)) {
        ++failures;
    }
    memset(&handshake, 0, sizeof(handshake));
    data_packet.sample_freq = 100.0f;
    data_packet.frame = 1;
    observe_startup_data_packet(&handshake, &data_packet, 200);
    if (handshake.valid_data_packets != 0 ||
        handshake.data_rate_rejects != 1 ||
        startup_sample_rate_matches(197.0f, 200) ||
        !startup_sample_rate_matches(199.0f, 200)) {
        ++failures;
    }
    data_packet.n_signals = 1;
    data_packet.bursts = 1;
    data_packet.samples[0] = 1234;
    data_packet.samples[1] = 9999;
    if (!add_packet_samples(
            &data_packet,
            (int)offsetof(PktData, samples) +
                2 * (int)sizeof(data_packet.samples[0]),
            &packet_sum,
            &packet_sample_count) ||
        packet_sample_count != 1 ||
        packet_sum != 1234.0) {
        ++failures;
    }
    data_packet.samples[0] = 100;
    if (!add_packet_samples_to_ring(
            &data_packet,
            (int)offsetof(PktData, samples) +
                (int)sizeof(data_packet.samples[0]),
            monitor_ring,
            MONITOR_FILTER_SAMPLES,
            &monitor_count,
            &monitor_next)) {
        ++failures;
    }
    data_packet.samples[0] = 300;
    (void)add_packet_samples_to_ring(
        &data_packet,
        (int)offsetof(PktData, samples) +
            (int)sizeof(data_packet.samples[0]),
        monitor_ring,
        MONITOR_FILTER_SAMPLES,
        &monitor_count,
        &monitor_next
    );
    data_packet.samples[0] = 200;
    (void)add_packet_samples_to_ring(
        &data_packet,
        (int)offsetof(PktData, samples) +
            (int)sizeof(data_packet.samples[0]),
        monitor_ring,
        MONITOR_FILTER_SAMPLES,
        &monitor_count,
        &monitor_next
    );
    if (monitor_count != 3 ||
        median_int16(monitor_ring, monitor_count) != 200.0) {
        ++failures;
    }
    if (!test_auto_cal_detector(0)) {
        printf("SELF-TEST detalhe: detector sentido normal.\n");
        ++failures;
    }
    if (!test_auto_cal_detector(1)) {
        printf("SELF-TEST detalhe: detector sentido inverso.\n");
        ++failures;
    }
    if (!test_auto_cal_rejects_pulse()) {
        printf("SELF-TEST detalhe: rejeicao de pulso.\n");
        ++failures;
    }
    if (!test_auto_cal_noisy_persistent_trace()) {
        printf(
            "SELF-TEST detalhe: transicao persistente com ruido.\n"
        );
        ++failures;
    }
    if (!test_auto_cal_ignores_gradual_ramp()) {
        printf("SELF-TEST detalhe: rampa gradual sem wrap.\n");
        ++failures;
    }
    if (!test_auto_cal_detects_reversal()) {
        printf("SELF-TEST detalhe: inversao de sentido.\n");
        ++failures;
    }
    if (!test_auto_cal_rearm_ignores_pulse()) {
        printf("SELF-TEST detalhe: rearme apos pulso.\n");
        ++failures;
    }
    if (!test_auto_cal_gap_resets_history()) {
        printf("SELF-TEST detalhe: reset apos gap.\n");
        ++failures;
    }
    if (!test_auto_cal_tolerates_isolated_gaps()) {
        printf(
            "SELF-TEST detalhe: tolerancia a perda isolada.\n"
        );
        ++failures;
    }
    if (!test_angular_fit_pipeline(0)) {
        printf(
            "SELF-TEST detalhe: regressao angular sentido normal.\n"
        );
        ++failures;
    }
    if (!test_angular_fit_pipeline(1)) {
        printf(
            "SELF-TEST detalhe: regressao angular sentido inverso.\n"
        );
        ++failures;
    }
    if (!test_calibration_loader()) {
        printf(
            "SELF-TEST detalhe: loader JSON mA/deg e rejeicao x10.\n"
        );
        ++failures;
    }

    if (failures != 0) {
        printf("SELF-TEST FALHOU: %d verificacao(oes).\n", failures);
        return 0;
    }
    printf(
        "SELF-TEST OK: preset/readback/fallback, fit angular DLG+Drive, "
        "pos_mod, gaps, pulsos e ambos os sentidos validados.\n"
    );
    return 1;
}

enum {
    ACTION_MONITOR = 1,
    ACTION_AUTO_CALIBRATE_MOTOR = 2,
    ACTION_AUTO_CALIBRATE_MANUAL = 3,
    ACTION_REFERENCE_CALIBRATE = 4,
    ACTION_CHECK_DLG = 5
};

typedef struct {
    AppConfig config;
    char calib_path[MAX_PATH];
    char calib_out_path[MAX_PATH];
} MenuState;

static int load_selected_calibration(
    const AppConfig *config,
    Calibration *calibration)
{
    if (config->calib_path) {
        return load_calibration_file(
            config->calib_path,
            1,
            calibration
        );
    }
    return load_auto_calibration(calibration);
}

static void print_operation_header(
    const AppConfig *config,
    const Calibration *calibration)
{
    printf(
        "\nDLG4000 - teste do encoder %s\n"
        "Canal: CH3 | corrente | ganho: x3 | excitacao: 2.5 V | "
        "LPF: indice 0 / sem pos-filtro no programa | taxa: %d Hz\n"
        "Entrada: DC conectada (100 kohm) | AC desconectada | "
        "balanco desativado\n"
        "DLG: %s:%u | bind local: %s:%u\n"
        "Nao execute junto com outro software que use o DLG/porta UDP.\n",
        ENCODER_MODEL,
        config->sample_rate_hz,
        config->dlg_ip,
        (unsigned)config->dlg_port,
        config->local_ip[0] ? config->local_ip : "0.0.0.0",
        (unsigned)config->local_port
    );
    if (calibration && calibration->source_path[0]) {
        if (calibration->output_is_degrees) {
            printf(
                "Calibracao angular: %s\n"
                "Fit: graus = %.12g * raw %+.12g; modulo 360\n",
                calibration->source_path,
                calibration->slope_deg_per_count,
                calibration->intercept_deg
            );
        } else {
            printf(
                "Calibracao de corrente: %s\n"
                "Fit: mA = %.12g * raw %+.12g\n",
                calibration->source_path,
                calibration->slope_ma_per_count,
                calibration->intercept_ma
            );
        }
    }
    printf("\n");
}

static int run_hardware_action(
    const AppConfig *config,
    int action)
{
    Calibration calibration;
    DlgConnection connection;
    HandshakeState dlg_handshake;
    StreamStartDiagnostics stream_diagnostics;
    WSADATA winsock_data;
    int calibration_result = 0;
    int operation_ok = 0;
    int cancelled = 0;
    int winsock_started = 0;
    int connection_open = 0;

    calibration_init_defaults(&calibration);
    memset(&connection, 0, sizeof(connection));
    memset(&dlg_handshake, 0, sizeof(dlg_handshake));
    memset(&stream_diagnostics, 0, sizeof(stream_diagnostics));
    connection.socket_handle = INVALID_SOCKET;
    finish_status_line();
    InterlockedExchange(&g_stop_requested, 0);

    if (action == ACTION_MONITOR) {
        calibration_result =
            load_selected_calibration(config, &calibration);
        if (calibration_result <= 0) {
            fprintf(
                stderr,
                calibration_result < 0
                    ? "Calibracao encontrada, mas invalida para "
                      "o preset CH3 (x3, 2.5 V, LPF 0, DC conectado) "
                      "ou unidade mA/deg.\n"
                    : "Calibracao CH3 em mA ou graus nao encontrada.\n"
            );
            fprintf(
                stderr,
                "Use as opcoes 2, 3 ou 4 do menu para calibrar "
                "antes de monitorar.\n"
            );
            return 3;
        }
    } else {
        calibration_init_defaults(&calibration);
    }

    print_operation_header(
        config,
        action == ACTION_MONITOR ? &calibration : NULL
    );

    if (!SetConsoleCtrlHandler(console_ctrl_handler, TRUE)) {
        fprintf(stderr, "Falha ao instalar handler de Ctrl+C.\n");
        return 1;
    }
    if (WSAStartup(MAKEWORD(2, 2), &winsock_data) != 0) {
        fprintf(stderr, "Falha no WSAStartup.\n");
        goto cleanup;
    }
    winsock_started = 1;

    if (!open_connection(config, &connection)) {
        goto cleanup;
    }
    connection_open = 1;

    if (action == ACTION_AUTO_CALIBRATE_MOTOR ||
        action == ACTION_AUTO_CALIBRATE_MANUAL) {
        operation_ok = run_auto_calibration(
            config,
            &connection,
            &calibration,
            action == ACTION_AUTO_CALIBRATE_MOTOR
        );
    } else if (action == ACTION_REFERENCE_CALIBRATE) {
        operation_ok = run_calibration(
            config,
            &connection,
            &calibration
        );
    } else if (action == ACTION_CHECK_DLG) {
        printf(
            "Verificando configuracao do CH3 e recebimento de dados...\n"
        );
        stream_diagnostics.result = &dlg_handshake;
        operation_ok = start_stream_until_data_diagnostic(
            config,
            &connection,
            &calibration,
            &stream_diagnostics
        );
        if (operation_ok) {
            clear_status_line();
            if (dlg_handshake.saw_get_channel_response &&
                dlg_handshake.get_channel_matches) {
                printf(
                    "DLG OK: readback CH3 confirmou corrente, x3, "
                    "2.5 V, LPF 0, DC conectado, AC desconectado e "
                    "ACQDATA valido.\n"
                );
            } else {
                printf(
                    "DLG operacional: ACQDATA valido recebido. "
                    "GETCHCFG nao respondeu; o preset CH3 foi reenviado "
                    "sem confirmacao por readback.\n"
                );
            }
        }
    } else {
        printf(
            "Monitor iniciado: mediana movel das ultimas %d amostras, "
            "no maximo 2 atualizacoes/s.\n",
            MONITOR_FILTER_SAMPLES
        );
        operation_ok = monitor_current(
            config,
            &connection,
            &calibration
        );
    }

cleanup:
    cancelled = stop_requested();
    if (connection_open) {
        (void)send_stop(&connection);
        closesocket(connection.socket_handle);
        connection.socket_handle = INVALID_SOCKET;
    }
    finish_status_line();
    if (winsock_started) {
        WSACleanup();
    }
    SetConsoleCtrlHandler(console_ctrl_handler, FALSE);

    if (cancelled) {
        printf("Operacao cancelada; ACQSTOP enviado ao DLG.\n");
    } else if (operation_ok) {
        printf("Operacao concluida; ACQSTOP enviado ao DLG.\n");
    }
    InterlockedExchange(&g_stop_requested, 0);
    if (cancelled) {
        return 2;
    }
    return operation_ok ? 0 : 1;
}

static int read_menu_line(
    const char *prompt,
    char *buffer,
    size_t buffer_size)
{
    size_t length;
    int extra;

    if (!buffer || buffer_size < 2) {
        return 0;
    }
    finish_status_line();
    printf("%s", prompt);
    fflush(stdout);
    if (!fgets(buffer, (int)buffer_size, stdin)) {
        return 0;
    }

    length = strlen(buffer);
    if (length > 0 && buffer[length - 1] != '\n' &&
        !feof(stdin)) {
        do {
            extra = getchar();
        } while (extra != '\n' && extra != EOF);
    }
    while (length > 0 &&
           isspace((unsigned char)buffer[length - 1])) {
        buffer[--length] = '\0';
    }
    return 1;
}

static void wait_for_menu(void)
{
    char line[8];
    (void)read_menu_line(
        "\nPressione ENTER para voltar ao menu...",
        line,
        sizeof(line)
    );
}

static int parse_menu_number(const char *text, int *value)
{
    char *end = NULL;
    long parsed;

    if (!text || !value) {
        return 0;
    }
    while (*text && isspace((unsigned char)*text)) {
        ++text;
    }
    parsed = strtol(text, &end, 10);
    while (end && *end && isspace((unsigned char)*end)) {
        ++end;
    }
    if (end == text || !end || *end != '\0' ||
        parsed < INT_MIN || parsed > INT_MAX) {
        return 0;
    }
    *value = (int)parsed;
    return 1;
}

static int parse_positive_double_text(
    const char *text,
    double minimum,
    double maximum,
    double *value)
{
    char copy[64];
    char *cursor;
    char *end = NULL;
    double parsed;

    if (!text || !value || strlen(text) >= sizeof(copy)) {
        return 0;
    }
    strcpy_s(copy, sizeof(copy), text);
    cursor = strchr(copy, ',');
    if (cursor) {
        *cursor = '.';
        if (strchr(cursor + 1, ',')) {
            return 0;
        }
    }
    parsed = strtod(copy, &end);
    while (end && *end && isspace((unsigned char)*end)) {
        ++end;
    }
    if (end == copy || !end || *end != '\0' ||
        !(parsed >= minimum && parsed <= maximum)) {
        return 0;
    }
    *value = parsed;
    return 1;
}

static int valid_ipv4_text(const char *text)
{
    const char *cursor = text;
    int part;

    if (!text || !*text) {
        return 0;
    }
    for (part = 0; part < 4; ++part) {
        char *end = NULL;
        long value;

        if (!isdigit((unsigned char)*cursor)) {
            return 0;
        }
        value = strtol(cursor, &end, 10);
        if (!end || end == cursor || value < 0 || value > 255) {
            return 0;
        }
        if (part < 3) {
            if (*end != '.') {
                return 0;
            }
            cursor = end + 1;
        } else if (*end != '\0') {
            return 0;
        }
    }
    return 1;
}

static void print_model_and_wiring(void)
{
    printf(
        "\n============================================================\n"
        "MODELO E LIGACAO DO ENCODER\n"
        "============================================================\n"
        "Modelo registrado: %s\n"
        "\n"
        "Decodificacao confirmada pelo manual do encoder:\n"
        "  BRT25  : corpo de 25 mm e eixo de 4 mm\n"
        "  A0M    : saida analogica de 4-20 mA\n"
        "  16bit  : 65.536 divisoes por volta; nao significa 16 voltas\n"
        "  RT1    : cabo com saida lateral\n"
        "  X3     : codigo interno, sem indicar tres ou quatro fios\n"
        "\n"
        "O manual do DLG determina, para entrada de corrente:\n"
        "  - usar os pinos 8 e 1 do DB9 do proprio CH3;\n"
        "  - usar o modo de sensor Corrente.\n"
        "\n"
        "Preset CH3 baseado na configuracao estavel da Lynx:\n"
        "  sensor Corrente; ganho x3; excitacao 2.5 V;\n"
        "  O ganho foi reduzido de x10 para x3 porque x10 saturou o A/D.\n"
        "  iLPF=0 e sem LPF adicional; mediana 9 no monitor;\n"
        "  impedancia DC conectada (100 kohm); AC desconectada.\n"
        "  A impedancia DC conectada e um campo do front-end e nao "
        "aterra o loop.\n"
        "\n"
        "Ligacao adotada para validacao inicial:\n"
        "  Encoder cinza  (4-20 mA +) -> CH3 pino 8\n"
        "  Encoder marrom (4-20 mA -) -> CH3 pino 1  [se existir]\n"
        "  Encoder vermelho           -> +12 a +24 Vcc da fonte externa\n"
        "  Encoder preto              -> 0 V da fonte externa\n"
        "\n"
        "Se o encoder for de tres fios:\n"
        "  CH3 pino 1 -> preto / 0 V da fonte externa\n"
        "  Marrom fica ausente ou isolado.\n"
        "\n"
        "DB9 femea do DLG, olhando a face onde o plugue encaixa:\n"
        "          5   4   3   2   1\n"
        "            9   8   7   6\n"
        "  A vista pelo lado da solda e espelhada.\n"
        "\n"
        "ATENCAO: o manual confirma os pinos 8/1, mas nao escreve a\n"
        "polaridade. O mapeamento 8=I+ e 1=I- e uma inferencia tecnica\n"
        "de I_IN e MUX IN-. Valide primeiro com calibrador 4-20 mA\n"
        "limitado ou confirme com a Lynx antes de ligar o encoder.\n"
        "\n"
        "Use fonte externa isolada. Nao use a saida auxiliar do DLG.\n"
        "Mantenha laranja (SETH) e amarelo (SETL) isolados.\n",
        ENCODER_MODEL
    );
}

static void init_menu_state(
    MenuState *state,
    const AppConfig *defaults)
{
    char executable_dir[MAX_PATH];

    memset(state, 0, sizeof(*state));
    state->config = *defaults;
    state->config.relation_loaded =
        load_supervisor_mechanical_ratio(
            &state->config.mechanical_ratio,
            state->config.relation_source,
            sizeof(state->config.relation_source)
        );
    (void)resolve_drive_executable(
        state->config.drive_exe,
        sizeof(state->config.drive_exe)
    );

    if (defaults->calib_path) {
        strncpy_s(
            state->calib_path,
            sizeof(state->calib_path),
            defaults->calib_path,
            _TRUNCATE
        );
        state->config.calib_path = state->calib_path;
    } else {
        state->config.calib_path = NULL;
    }
    if ((!defaults->calib_out_path ||
         strcmp(defaults->calib_out_path, DEFAULT_CALIB_PATH) == 0) &&
        get_executable_dir(executable_dir, sizeof(executable_dir))) {
        int written = _snprintf_s(
            state->calib_out_path,
            sizeof(state->calib_out_path),
            _TRUNCATE,
            "%s\\out\\encoder_CH3_mA.json",
            executable_dir
        );
        if (written < 0 ||
            written >= (int)sizeof(state->calib_out_path)) {
            strcpy_s(
                state->calib_out_path,
                sizeof(state->calib_out_path),
                DEFAULT_CALIB_PATH
            );
        }
    } else {
        strncpy_s(
            state->calib_out_path,
            sizeof(state->calib_out_path),
            defaults->calib_out_path
                ? defaults->calib_out_path
                : DEFAULT_CALIB_PATH,
            _TRUNCATE
        );
    }
    state->config.calib_out_path = state->calib_out_path;
}

static void print_menu_header(const MenuState *state)
{
    Calibration calibration;
    int drive_command_rpm = 0;
    int calibration_result =
        load_selected_calibration(&state->config, &calibration);
    int drive_config_ok =
        path_is_file(state->config.drive_exe) &&
        compute_drive_command_rpm(
            state->config.encoder_target_rpm,
            state->config.mechanical_ratio,
            state->config.drive_direction,
            &drive_command_rpm
        );

    printf(
        "\n============================================================\n"
        " TESTE DO ENCODER %s\n"
        " DLG4000 / CH3 / 4-20 mA\n"
        " Versao: %s\n"
        "============================================================\n"
        " DLG       : %s:%u\n"
        " Porta PC  : %s:%u\n"
        " Taxa      : %d Hz\n"
        " Preset CH3: corrente | x3 | 2.5 V | LPF 0 | DC ON | AC OFF\n"
        " Drive     : %s | alvo encoder %.3f RPM | "
        "i=%.6g | motor %+d RPM [%s]\n"
        " Calibracao: %s",
        ENCODER_MODEL,
        ENCODER_TEST_BUILD_ID,
        state->config.dlg_ip,
        (unsigned)state->config.dlg_port,
        state->config.local_ip[0]
            ? state->config.local_ip
            : "0.0.0.0",
        (unsigned)state->config.local_port,
        state->config.sample_rate_hz,
        state->config.drive_port,
        state->config.encoder_target_rpm,
        state->config.mechanical_ratio,
        drive_command_rpm,
        drive_config_ok ? "OK" : "INDISPONIVEL",
        state->config.calib_path
            ? state->config.calib_path
            : "AUTO"
    );
    if (calibration_result == 1) {
        printf(
            " [OK / %s]\n",
            calibration.output_is_degrees
                ? "GRAUS DIRETOS"
                : "mA"
        );
    } else if (calibration_result < 0) {
        printf(" [INVALIDA]\n");
    } else {
        printf(" [NAO ENCONTRADA]\n");
    }
    printf(
        "------------------------------------------------------------\n"
        " 1 - Monitorar CH3 (angulo e sinal 4-20 mA)\n"
        " 2 - Autocalibrar graus com Drive (4 wraps / 3 voltas)\n"
        " 3 - Diagnostico nominal manual (4 transicoes)\n"
        " 4 - Calibracao de corrente com referencia\n"
        " 5 - Verificar comunicacao com o DLG\n"
        " 6 - Configuracoes\n"
        " 7 - Modelo e esquema de ligacao\n"
        " 8 - Executar autoteste interno\n"
        " 0 - Sair\n"
        "============================================================\n"
    );
}

static void configure_menu(MenuState *state)
{
    char line[512];

    for (;;) {
        int choice;

        printf(
            "\n---------------- CONFIGURACOES ----------------\n"
            " 1 - IP do DLG              : %s\n"
            " 2 - Porta UDP do DLG       : %u\n"
            " 3 - IP/interface local     : %s\n"
            " 4 - Porta UDP local        : %u\n"
            " 5 - Taxa de amostragem     : %d Hz\n"
            " 6 - Arquivo de calibracao  : %s\n"
            " 7 - Saida da calibracao    : %s\n"
            " 8 - Recarregar relacao do supervisorio\n"
            " 9 - Relacao local i=D2/D1  : %.9g [%s]\n"
            "10 - Porta do Drive          : %s\n"
            "11 - Alvo do eixo encoder    : %.3f RPM\n"
            "12 - Sentido do Drive        : %s\n"
            "13 - Relocalizar Drive EXE   : %s\n"
            " 0 - Voltar\n"
            "As alteracoes valem durante esta execucao do programa.\n",
            state->config.dlg_ip,
            (unsigned)state->config.dlg_port,
            state->config.local_ip[0]
                ? state->config.local_ip
                : "0.0.0.0",
            (unsigned)state->config.local_port,
            state->config.sample_rate_hz,
            state->config.calib_path
                ? state->config.calib_path
                : "AUTO",
            state->config.calib_out_path,
            state->config.mechanical_ratio,
            state->config.relation_loaded
                ? "SUPERVISORIO"
                : "LOCAL",
            state->config.drive_port,
            state->config.encoder_target_rpm,
            state->config.drive_direction > 0
                ? "POSITIVO"
                : "NEGATIVO",
            state->config.drive_exe[0]
                ? state->config.drive_exe
                : "NAO ENCONTRADO"
        );
        if (!read_menu_line("Escolha: ", line, sizeof(line))) {
            return;
        }
        if (!parse_menu_number(line, &choice)) {
            printf("Opcao invalida.\n");
            continue;
        }
        if (choice == 0) {
            return;
        }

        if (choice == 1) {
            if (!read_menu_line(
                    "Novo IP do DLG (ENTER mantem): ",
                    line,
                    sizeof(line))) {
                return;
            }
            if (line[0] && valid_ipv4_text(line)) {
                strcpy_s(
                    state->config.dlg_ip,
                    sizeof(state->config.dlg_ip),
                    line
                );
            } else if (line[0]) {
                printf("IP invalido; valor anterior mantido.\n");
            }
        } else if (choice == 2 || choice == 4) {
            uint16_t port;
            if (!read_menu_line(
                    choice == 2
                        ? "Nova porta do DLG (ENTER mantem): "
                        : "Nova porta local (ENTER mantem): ",
                    line,
                    sizeof(line))) {
                return;
            }
            if (!line[0]) {
                continue;
            }
            if (!parse_u16(line, &port)) {
                printf("Porta invalida; valor anterior mantido.\n");
            } else if (choice == 2) {
                state->config.dlg_port = port;
            } else {
                state->config.local_port = port;
            }
        } else if (choice == 3) {
            if (!read_menu_line(
                    "Novo IP local; 0.0.0.0 usa qualquer interface "
                    "(ENTER mantem): ",
                    line,
                    sizeof(line))) {
                return;
            }
            if (!line[0]) {
                continue;
            }
            if (!valid_ipv4_text(line)) {
                printf("IP invalido; valor anterior mantido.\n");
            } else if (strcmp(line, "0.0.0.0") == 0) {
                state->config.local_ip[0] = '\0';
            } else {
                strcpy_s(
                    state->config.local_ip,
                    sizeof(state->config.local_ip),
                    line
                );
            }
        } else if (choice == 5) {
            int rate;
            if (!read_menu_line(
                    "Taxa 25/50/100/200/400/800/1600/3200/6400/12800 "
                    "(ENTER mantem): ",
                    line,
                    sizeof(line))) {
                return;
            }
            if (!line[0]) {
                continue;
            }
            if (!parse_menu_number(line, &rate) ||
                !supported_sample_rate(rate)) {
                printf("Taxa invalida; valor anterior mantido.\n");
            } else {
                state->config.sample_rate_hz = rate;
            }
        } else if (choice == 6) {
            if (!read_menu_line(
                    "Arquivo de calibracao; AUTO busca automaticamente "
                    "(ENTER mantem): ",
                    line,
                    sizeof(line))) {
                return;
            }
            if (!line[0]) {
                continue;
            }
            if (_stricmp(line, "AUTO") == 0) {
                state->calib_path[0] = '\0';
                state->config.calib_path = NULL;
            } else if (strlen(line) >= sizeof(state->calib_path)) {
                printf("Caminho muito longo; valor anterior mantido.\n");
            } else {
                strcpy_s(
                    state->calib_path,
                    sizeof(state->calib_path),
                    line
                );
                state->config.calib_path = state->calib_path;
            }
        } else if (choice == 7) {
            if (!read_menu_line(
                    "Novo arquivo de saida (ENTER mantem): ",
                    line,
                    sizeof(line))) {
                return;
            }
            if (!line[0]) {
                continue;
            }
            if (line[0] == '\\' && line[1] == '\\') {
                printf(
                    "Saida em caminho UNC nao e suportada; "
                    "valor anterior mantido.\n"
                );
            } else if (strlen(line) >= sizeof(state->calib_out_path)) {
                printf("Caminho muito longo; valor anterior mantido.\n");
            } else {
                strcpy_s(
                    state->calib_out_path,
                    sizeof(state->calib_out_path),
                    line
                );
            }
        } else if (choice == 8) {
            double loaded_ratio =
                state->config.mechanical_ratio;
            char loaded_source[MAX_PATH];

            if (load_supervisor_mechanical_ratio(
                    &loaded_ratio,
                    loaded_source,
                    sizeof(loaded_source))) {
                state->config.mechanical_ratio =
                    loaded_ratio;
                state->config.relation_loaded = 1;
                strcpy_s(
                    state->config.relation_source,
                    sizeof(state->config.relation_source),
                    loaded_source
                );
                printf(
                    "Relacao %.9g carregada de %s\n",
                    loaded_ratio,
                    loaded_source
                );
            } else {
                printf(
                    "Nao foi possivel carregar a configuracao; "
                    "valor atual mantido.\n"
                );
            }
        } else if (choice == 9) {
            double ratio;

            if (!read_menu_line(
                    "Nova relacao i=D2/D1 (ENTER mantem): ",
                    line,
                    sizeof(line))) {
                return;
            }
            if (!line[0]) {
                continue;
            }
            if (!parse_positive_double_text(
                    line,
                    0.001,
                    1000.0,
                    &ratio)) {
                printf(
                    "Relacao invalida; valor anterior mantido.\n"
                );
            } else {
                state->config.mechanical_ratio = ratio;
                state->config.relation_loaded = 0;
                strcpy_s(
                    state->config.relation_source,
                    sizeof(state->config.relation_source),
                    "MENU"
                );
            }
        } else if (choice == 10) {
            if (!read_menu_line(
                    "Porta do Drive, por exemplo COM5 "
                    "(ENTER mantem): ",
                    line,
                    sizeof(line))) {
                return;
            }
            if (!line[0]) {
                continue;
            }
            if (!valid_drive_port(line) ||
                strlen(line) >=
                    sizeof(state->config.drive_port)) {
                printf(
                    "Porta invalida; valor anterior mantido.\n"
                );
            } else {
                strcpy_s(
                    state->config.drive_port,
                    sizeof(state->config.drive_port),
                    line
                );
            }
        } else if (choice == 11) {
            double target_rpm;

            if (!read_menu_line(
                    "RPM desejado no eixo do encoder "
                    "(ENTER mantem): ",
                    line,
                    sizeof(line))) {
                return;
            }
            if (!line[0]) {
                continue;
            }
            if (!parse_positive_double_text(
                    line,
                    0.001,
                    1000.0,
                    &target_rpm)) {
                printf(
                    "RPM invalido; valor anterior mantido.\n"
                );
            } else {
                state->config.encoder_target_rpm =
                    target_rpm;
            }
        } else if (choice == 12) {
            if (!read_menu_line(
                    "Sentido: + para positivo, - para negativo "
                    "(ENTER mantem): ",
                    line,
                    sizeof(line))) {
                return;
            }
            if (!line[0]) {
                continue;
            }
            if (strcmp(line, "+") == 0 ||
                strcmp(line, "1") == 0) {
                state->config.drive_direction = 1;
            } else if (strcmp(line, "-") == 0 ||
                       strcmp(line, "-1") == 0) {
                state->config.drive_direction = -1;
            } else {
                printf(
                    "Sentido invalido; valor anterior mantido.\n"
                );
            }
        } else if (choice == 13) {
            if (resolve_drive_executable(
                    state->config.drive_exe,
                    sizeof(state->config.drive_exe))) {
                printf(
                    "Drive encontrado: %s\n",
                    state->config.drive_exe
                );
            } else {
                printf(
                    "a5_speed_logger.exe nao foi encontrado.\n"
                );
            }
        } else {
            printf("Opcao invalida.\n");
        }
    }
}

static int run_interactive_menu(const AppConfig *defaults)
{
    MenuState state;
    char line[64];

    init_menu_state(&state, defaults);
    for (;;) {
        int choice;

        print_menu_header(&state);
        if (!read_menu_line("Escolha uma opcao: ", line, sizeof(line))) {
            printf("\nEntrada encerrada. Finalizando.\n");
            return 0;
        }
        if (!parse_menu_number(line, &choice)) {
            printf("Opcao invalida.\n");
            wait_for_menu();
            continue;
        }

        if (choice == 0) {
            printf("Programa encerrado.\n");
            return 0;
        }
        if (choice == 1) {
            (void)run_hardware_action(
                &state.config,
                ACTION_MONITOR
            );
            wait_for_menu();
        } else if (choice == 2 ||
                   choice == 3 ||
                   choice == 4) {
            Calibration written;
            int action_result;
            int action =
                choice == 2
                    ? ACTION_AUTO_CALIBRATE_MOTOR
                    : choice == 3
                        ? ACTION_AUTO_CALIBRATE_MANUAL
                        : ACTION_REFERENCE_CALIBRATE;

            action_result = run_hardware_action(
                &state.config,
                action
            );
            if (action_result == 0 &&
                load_calibration_file(
                    state.config.calib_out_path,
                    1,
                    &written) == 1) {
                strcpy_s(
                    state.calib_path,
                    sizeof(state.calib_path),
                    state.config.calib_out_path
                );
                state.config.calib_path = state.calib_path;
                printf(
                    "A nova calibracao foi selecionada para o monitor.\n"
                );
            }
            wait_for_menu();
        } else if (choice == 5) {
            (void)run_hardware_action(
                &state.config,
                ACTION_CHECK_DLG
            );
            wait_for_menu();
        } else if (choice == 6) {
            configure_menu(&state);
        } else if (choice == 7) {
            print_model_and_wiring();
            wait_for_menu();
        } else if (choice == 8) {
            (void)run_self_test();
            wait_for_menu();
        } else {
            printf("Opcao invalida.\n");
            wait_for_menu();
        }
    }
}

int main(int argc, char **argv)
{
    AppConfig config;
    int parse_result = parse_args(argc, argv, &config);
    int action_result;

    if (parse_result <= 0) {
        return parse_result == 0 ? 0 : 2;
    }
    if (argc == 1) {
        return run_interactive_menu(&config);
    }
    if (config.self_test) {
        return run_self_test() ? 0 : 1;
    }
    if (config.replay_path) {
        if (!config.mechanical_ratio_explicit) {
            config.relation_loaded =
                load_supervisor_mechanical_ratio(
                    &config.mechanical_ratio,
                    config.relation_source,
                    sizeof(config.relation_source)
                );
        }
        printf(
            "REPLAY configuracao: taxa=%d Hz relacao=%.9g fonte=%s\n",
            config.sample_rate_hz,
            config.mechanical_ratio,
            config.mechanical_ratio_explicit
                ? "CLI"
                : config.relation_loaded
                    ? config.relation_source
                    : "padrao_local"
        );
        return run_auto_cal_replay(
            config.replay_path,
            config.sample_rate_hz,
            config.mechanical_ratio
        ) ? 0 : 1;
    }
    action_result = run_hardware_action(
        &config,
        config.calibrate
            ? ACTION_REFERENCE_CALIBRATE
            : ACTION_MONITOR
    );
    return action_result == 2 ? 0 : action_result;
}
