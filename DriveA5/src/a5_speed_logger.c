/*
a5_speed_logger.c
-----------------
Logger sem UI para ensaios em modo velocidade no Drive A5.

Objetivo geral:
- Executar um schedule de segmentos (rpm,duration_s) e registrar telemetria.
- Coletar posicao + rpm em taxa fixa, com slots perdidos marcados como NULL.
- Integrar com supervisorio via IPC textual (START/PAUSE/RESUME/STOP).

Fluxo principal:
1) Parse de argumentos e carregamento do schedule CSV.
2) Setup opcional do drive para modo velocidade (P06-03 como fonte unica).
3) Espera START (quando --ipc) e entra no loop de segmentos com rampa.
4) Leitura periodica de posicao/rpm por deadline real (QPC), sem drift.
5) STOP reforcado no fim/interrupcao e fechamento do CSV.

Variaveis/configuracoes principais:
- REG_*: registradores usados para controle de velocidade e telemetria.
- WORD_RUN/WORD_RDY: palavras Modbus para RUN e parada imediata.
- CMD_* e FAST_*: timeouts para escrita/comando e leitura rapida.
- RAMP_TIME_S: duracao da rampa linear entre setpoints.
- seg_t: segmento de schedule (rpm + duracao + tempo final acumulado).
- rpm_ramp_t: estado da rampa ativa de transicao de velocidade.

Resumo de funcoes:
- set_timeouts_us/trim/make_port_path: utilitarios de IO serial e texto.
- read_*: leitura de registradores com fallback/cache de modo FC03/FC04.
- read_s32: leitura 32-bit respeitando ordem de palavras.
- setup_speed_mode: aplica parametros para estabilizar modo velocidade.
- cmd_run/cmd_stop/cmd_vdi_stop/cmd_rpm: comandos de atuacao no drive.
- qpc_now_ticks/qpc_freq_ticks: temporizacao de alta precisao para deadlines.
- stop_drive_now: rotina de parada segura com rampa curta e reforco de STOP.
- ipc_poll_command: parse de comandos recebidos por stdin.
- round_to_int/ramp_begin/ramp_eval: utilitarios da logica de rampa.
- parse_two_numbers/load_schedule: parse de linhas do schedule.
- print_usage/main: entrada principal do executavel.
*/

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>

#include <modbus.h>

enum {
    REG_CTRL    = 12544,  // 12545-1 : RUN/RDY
    REG_RPM_CMD = 1539,   // 1540-1  : RPM setpoint (int16)
    REG_P02_00  = 0x0200, // control mode (speed/pos/torque)
    REG_P03_02  = 0x0302, // DI1 func (disable)
    REG_P06_00  = 0x0600, // main speed source A
    REG_P06_01  = 0x0601, // aux speed source B
    REG_P06_02  = 0x0602, // speed command source
    REG_P0C_09  = 0x0C09, // comm VDI enable
    REG_P31_00  = 0x3100, // VDI virtual
    REG_P0B_07  = 0x0B07, // P0B-07 (abs pos, 32-bit) - historical naming
    REG_P0B_09  = 0x0B09, // P0B-09 (pos per revolution, 0..65535)
    REG_P0B_00  = 0x0B00, // P0B-00 (actual motor speed, rpm)
    REG_P05_02  = 0x0502, // P05-02 (command units per revolution)
    REG_P0C_26  = 0x0C26  // word order
};

static const uint16_t WORD_RUN = 0x0001;
static const uint16_t WORD_RDY = 0x0000;

#define CMD_RESP_US 50000
#define CMD_BYTE_US 50000
#define FAST_RESP_US 3000
#define FAST_BYTE_US 2000
#define RAMP_TIME_S 3.0

typedef struct {
    int rpm;
    double dur_s;
    double t_end;
} seg_t;

typedef struct {
    int start_rpm;
    int target_rpm;
    int active;
    int64_t t_start;
    int64_t t_end;
} rpm_ramp_t;

static void set_timeouts_us(modbus_t *ctx, int resp_us, int byte_us){
    modbus_set_response_timeout(ctx, 0, resp_us);
    modbus_set_byte_timeout(ctx, 0, byte_us);
}

static void trim(char *s){
    size_t n = strlen(s);
    while(n && (s[n-1]=='\n'||s[n-1]=='\r'||s[n-1]==' '||s[n-1]=='\t')) s[--n]=0;
    char *p = s; while(*p && isspace((unsigned char)*p)) p++;
    if(p != s) memmove(s, p, strlen(p)+1);
}

static void make_port_path(const char *shortName, char *out, size_t outsz){
    int num = 0;
    if(_strnicmp(shortName, "COM", 3) == 0) num = atoi(shortName + 3);
    if(num >= 10) _snprintf(out, outsz, "\\\\.\\%s", shortName);
    else _snprintf(out, outsz, "%s", shortName);
}

static int read_u16(modbus_t *ctx, int reg, uint16_t *out){
    uint16_t v = 0;
    if(modbus_read_registers(ctx, reg, 1, &v) != 1){
        return -1;
    }
    *out = v;
    return 0;
}

static int read_u16_in(modbus_t *ctx, int reg, uint16_t *out){
    uint16_t v = 0;
    if(modbus_read_input_registers(ctx, reg, 1, &v) != 1){
        return -1;
    }
    *out = v;
    return 0;
}

static int read_p0b09(modbus_t *ctx, uint16_t *out){
    if(read_u16(ctx, REG_P0B_09, out) == 0) return 0;
    if(read_u16_in(ctx, REG_P0B_09, out) == 0) return 0;
    return -1;
}

static int read_u16_cached(modbus_t *ctx, int reg, uint16_t *out, int *mode){
    /* mode: 0=unknown, 3=FC03 holding, 4=FC04 input */
    if(mode && *mode == 3){
        if(read_u16(ctx, reg, out) == 0) return 0;
        if(read_u16_in(ctx, reg, out) == 0){ *mode = 4; return 0; }
        return -1;
    }
    if(mode && *mode == 4){
        if(read_u16_in(ctx, reg, out) == 0) return 0;
        if(read_u16(ctx, reg, out) == 0){ *mode = 3; return 0; }
        return -1;
    }

    if(read_u16(ctx, reg, out) == 0){
        if(mode) *mode = 3;
        return 0;
    }
    if(read_u16_in(ctx, reg, out) == 0){
        if(mode) *mode = 4;
        return 0;
    }
    return -1;
}

static int read_p0b09_cached(modbus_t *ctx, uint16_t *out, int *mode){
    return read_u16_cached(ctx, REG_P0B_09, out, mode);
}

static int read_rpm_cached(modbus_t *ctx, int16_t *out, int *mode){
    uint16_t u = 0;
    if(read_u16_cached(ctx, REG_P0B_00, &u, mode) != 0) return -1;
    *out = (int16_t)u;
    return 0;
}

static int write_u16_seq(modbus_t *ctx, int reg, uint16_t v, const char *label,
                         int retries, int retry_ms){
    for(int i = 0; i < retries; ++i){
        if(modbus_write_register(ctx, reg, v) != -1){
            if(label) printf("WRITE 0x%04X %s = %u -> OK\n", reg, label, (unsigned)v);
            return 0;
        }
        if(i + 1 < retries) Sleep(retry_ms);
    }
    if(label) printf("WRITE 0x%04X %s = %u -> FAIL\n", reg, label, (unsigned)v);
    return -1;
}

static int read_s32(modbus_t *ctx, int reg, int low_first, int32_t *out){
    uint16_t regs[2] = {0, 0};
    if(modbus_read_registers(ctx, reg, 2, regs) != 2) return -1;
    uint32_t u;
    if(low_first){
        u = ((uint32_t)regs[1] << 16) | regs[0];
    }else{
        u = ((uint32_t)regs[0] << 16) | regs[1];
    }
    *out = (int32_t)u;
    return 0;
}

static void setup_speed_mode(modbus_t *ctx){
    // NOTE: Ajusta parametros para aceitar RPM via Modbus.
    // P02-00 = 0 (modo velocidade)
    // P06-00 = 0 (fonte A = P06-03 / given number)
    // P06-01 = 3 (fonte B = 0/no effect)
    // P06-02 = 0 (seletor = source A only)
    // P03-02 = 0 (desabilita DI1)
    // P0C-09 = 1 (VDI via comunicacao)
    puts("Setup speed mode (P02-00/P06-00/P06-01/P06-02/P03-02/P0C-09)...");
    (void)write_u16_seq(ctx, REG_P31_00, 0, "P31-00 (VDI STOP)", 2, 30);
    (void)write_u16_seq(ctx, REG_P0C_09, 1, "P0C-09 (Comm VDI)", 2, 30);
    (void)write_u16_seq(ctx, REG_P03_02, 0, "P03-02 (DI1 func)", 2, 30);
    (void)write_u16_seq(ctx, REG_P02_00, 0, "P02-00 (control mode)", 2, 30);
    (void)write_u16_seq(ctx, REG_P06_00, 0, "P06-00 (A src=given)", 2, 30);
    (void)write_u16_seq(ctx, REG_P06_01, 3, "P06-01 (B src=0)", 2, 30);
    (void)write_u16_seq(ctx, REG_P06_02, 0, "P06-02 (sel=A)", 2, 30);
}

static int cmd_run(modbus_t *ctx){
    set_timeouts_us(ctx, CMD_RESP_US, CMD_BYTE_US);
    if(modbus_write_register(ctx, REG_CTRL, WORD_RUN) == -1) return -1;
    return 0;
}

static int cmd_stop(modbus_t *ctx){
    set_timeouts_us(ctx, CMD_RESP_US, CMD_BYTE_US);
    for(int k=0;k<3;++k){
        if(modbus_write_register(ctx, REG_CTRL, WORD_RDY) != -1) return 0;
        Sleep(30);
    }
    return -1;
}

static int cmd_vdi_stop(modbus_t *ctx){
    set_timeouts_us(ctx, CMD_RESP_US, CMD_BYTE_US);
    return write_u16_seq(ctx, REG_P31_00, 0, NULL, 3, 30);
}

static int cmd_rpm(modbus_t *ctx, int rpm){
    if(rpm < -32768 || rpm > 32767) return -1;
    set_timeouts_us(ctx, CMD_RESP_US, CMD_BYTE_US);
    uint16_t v = (uint16_t)(rpm & 0xFFFF);
    for(int k=0;k<2;++k){
        if(modbus_write_register(ctx, REG_RPM_CMD, v) != -1) return 0;
        Sleep(20);
    }
    return -1;
}

static int round_to_int(double v);

static int64_t qpc_now_ticks(void){
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (int64_t)c.QuadPart;
}

static void stop_drive_now(modbus_t *ctx, int *current_cmd_rpm, int64_t ramp_ticks){
    /* Soft stop vector: ramp to zero setpoint, then force RDY/VDI stop. */
    int start_rpm = (current_cmd_rpm != NULL) ? *current_cmd_rpm : 0;
    if(ramp_ticks > 0 && start_rpm != 0){
        int64_t t0 = qpc_now_ticks();
        int64_t t1 = t0 + ramp_ticks;
        int last_cmd = start_rpm;

        for(;;){
            int64_t now = qpc_now_ticks();
            if(now >= t1) break;
            double alpha = (double)(now - t0) / (double)(t1 - t0);
            if(alpha < 0.0) alpha = 0.0;
            if(alpha > 1.0) alpha = 1.0;
            int cmd = round_to_int((double)start_rpm * (1.0 - alpha));
            if(cmd != last_cmd){
                (void)cmd_rpm(ctx, cmd);
                last_cmd = cmd;
            }
            Sleep(20);
        }
    }

    (void)cmd_rpm(ctx, 0);
    if(current_cmd_rpm) *current_cmd_rpm = 0;
    for(int i = 0; i < 4; ++i){
        (void)cmd_stop(ctx);
        (void)cmd_vdi_stop(ctx);
        Sleep(20);
    }
}

typedef enum {
    IPC_CMD_NONE = 0,
    IPC_CMD_STOP = 1,
    IPC_CMD_PAUSE = 2,
    IPC_CMD_RESUME = 3
} ipc_cmd_t;

static ipc_cmd_t ipc_poll_command(int use_ipc){
    static char pending[256];
    static size_t pending_len = 0;
    if(!use_ipc) return IPC_CMD_NONE;

    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    if(h == NULL || h == INVALID_HANDLE_VALUE) return IPC_CMD_NONE;

    DWORD avail = 0;
    if(!PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL) || avail == 0){
        return IPC_CMD_NONE;
    }

    char chunk[128];
    DWORD to_read = (avail < (DWORD)(sizeof(chunk) - 1)) ? avail : (DWORD)(sizeof(chunk) - 1);
    DWORD got = 0;
    if(!ReadFile(h, chunk, to_read, &got, NULL) || got == 0){
        return IPC_CMD_NONE;
    }
    chunk[got] = 0;

    if(got + pending_len >= sizeof(pending)){
        pending_len = 0;
    }
    memcpy(pending + pending_len, chunk, got);
    pending_len += got;
    pending[pending_len] = 0;

    for(size_t i = 0; i < pending_len; ++i){
        if(pending[i] != '\n') continue;

        size_t line_len = i;
        while(line_len > 0 && (pending[line_len - 1] == '\r' || pending[line_len - 1] == '\n')){
            line_len--;
        }

        char line[64];
        size_t copy_len = (line_len < sizeof(line) - 1) ? line_len : (sizeof(line) - 1);
        memcpy(line, pending, copy_len);
        line[copy_len] = 0;
        trim(line);

        size_t remain = pending_len - (i + 1);
        memmove(pending, pending + i + 1, remain);
        pending_len = remain;
        pending[pending_len] = 0;

        if(_stricmp(line, "STOP") == 0) return IPC_CMD_STOP;
        if(_stricmp(line, "PAUSE") == 0) return IPC_CMD_PAUSE;
        if(_stricmp(line, "RESUME") == 0) return IPC_CMD_RESUME;

        i = (size_t)-1;
    }

    return IPC_CMD_NONE;
}
static int64_t qpc_freq_ticks(void){
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return (int64_t)f.QuadPart;
}

static int round_to_int(double v){
    return (v >= 0.0) ? (int)(v + 0.5) : (int)(v - 0.5);
}

static void ramp_begin(rpm_ramp_t *r, int start_rpm, int target_rpm, int64_t now_ticks, int64_t ramp_ticks){
    if(!r) return;
    r->start_rpm = start_rpm;
    r->target_rpm = target_rpm;
    r->t_start = now_ticks;
    r->t_end = now_ticks + ((ramp_ticks > 0) ? ramp_ticks : 0);
    if(ramp_ticks <= 0 || start_rpm == target_rpm){
        r->active = 0;
        r->t_end = now_ticks;
    }else{
        r->active = 1;
    }
}

static int ramp_eval(const rpm_ramp_t *r, int64_t now_ticks){
    if(!r) return 0;
    if(!r->active) return r->target_rpm;
    if(now_ticks <= r->t_start) return r->start_rpm;
    if(now_ticks >= r->t_end) return r->target_rpm;

    int64_t span = r->t_end - r->t_start;
    int64_t elapsed = now_ticks - r->t_start;
    double alpha = (span > 0) ? ((double)elapsed / (double)span) : 1.0;
    double v = (double)r->start_rpm + ((double)(r->target_rpm - r->start_rpm) * alpha);
    return round_to_int(v);
}

static int parse_two_numbers(const char *line, double *a, double *b){
    char *end = NULL;
    double v1 = strtod(line, &end);
    if(end == line) return 0;
    while(*end && (*end == ' ' || *end == '\t')) end++;
    if(*end == ',' || *end == ';') end++;
    while(*end && (*end == ' ' || *end == '\t')) end++;
    double v2 = strtod(end, &end);
    if(end == NULL) return 0;
    *a = v1;
    *b = v2;
    return 1;
}

static int load_schedule(const char *path, seg_t **out, int *count, double *total_s){
    FILE *f = fopen(path, "r");
    if(!f) return 0;
    seg_t *arr = NULL;
    int n = 0;
    double total = 0.0;
    char line[256];
    while(fgets(line, sizeof(line), f)){
        trim(line);
        if(!line[0]) continue;
        if(line[0] == '#') continue;
        /* skip header line */
        if(isalpha((unsigned char)line[0])) continue;
        double a = 0.0, b = 0.0;
        if(!parse_two_numbers(line, &a, &b)) continue;
        int rpm = (int)(a >= 0 ? a + 0.5 : a - 0.5);
        double dur = b;
        if(dur <= 0) continue;
        seg_t s; s.rpm = rpm; s.dur_s = dur; s.t_end = total + dur;
        seg_t *tmp = (seg_t*)realloc(arr, (n + 1) * sizeof(seg_t));
        if(!tmp){ free(arr); fclose(f); return 0; }
        arr = tmp;
        arr[n++] = s;
        total += dur;
    }
    fclose(f);
    if(n <= 0){ free(arr); return 0; }
    *out = arr;
    *count = n;
    *total_s = total;
    return 1;
}

static void print_usage(void){
    puts("Uso:");
    puts("  a5_speed_logger --port COM4 --out <csv> --schedule <csv>");
    puts("                 [--rate <hz>] [--duration <s>] [--slave <id>]");
    puts("                 [--baud <n>] [--parity N|E|O] [--ipc] [--setup]");
}

int main(int argc, char **argv){
    const char *port = "COM4";
    const char *out_path = NULL;
    const char *sched_path = NULL;
    int slave = 1;
    int baud = 115200;
    char parity = 'E';
    int databits = 8;
    int stopbits = 1;
    double rate_hz = 200.0;
    double duration_s = 0.0;
    int use_ipc = 0;
    int do_setup = 0;

    for(int i = 1; i < argc; ++i){
        if(strcmp(argv[i], "--port") == 0 && i + 1 < argc){ port = argv[++i]; continue; }
        if(strcmp(argv[i], "--out") == 0 && i + 1 < argc){ out_path = argv[++i]; continue; }
        if(strcmp(argv[i], "--schedule") == 0 && i + 1 < argc){ sched_path = argv[++i]; continue; }
        if(strcmp(argv[i], "--rate") == 0 && i + 1 < argc){ rate_hz = atof(argv[++i]); continue; }
        if(strcmp(argv[i], "--duration") == 0 && i + 1 < argc){ duration_s = atof(argv[++i]); continue; }
        if(strcmp(argv[i], "--slave") == 0 && i + 1 < argc){ slave = atoi(argv[++i]); continue; }
        if(strcmp(argv[i], "--baud") == 0 && i + 1 < argc){ baud = atoi(argv[++i]); continue; }
        if(strcmp(argv[i], "--parity") == 0 && i + 1 < argc){ parity = (char)toupper((unsigned char)argv[++i][0]); continue; }
        if(strcmp(argv[i], "--ipc") == 0){ use_ipc = 1; continue; }
        if(strcmp(argv[i], "--setup") == 0){ do_setup = 1; continue; }
        print_usage();
        return 1;
    }
    if(!out_path || !sched_path){
        print_usage();
        return 1;
    }

    seg_t *segs = NULL;
    int seg_count = 0;
    double sched_total = 0.0;
    if(!load_schedule(sched_path, &segs, &seg_count, &sched_total)){
        fprintf(stderr, "Falha lendo schedule: %s\n", sched_path);
        return 1;
    }
    if(duration_s <= 0.0) duration_s = sched_total;
    if(duration_s <= 0.0 || rate_hz <= 0.0){
        free(segs);
        return 1;
    }

    FILE *f = fopen(out_path, "w");
    if(!f){
        fprintf(stderr, "Falha abrindo CSV: %s\n", out_path);
        free(segs);
        return 1;
    }
    fprintf(f, "idx,t_qpc,t_s,pos,rpm,pos_err,rpm_err\n");
    fflush(f);

    char port_path[64];
    make_port_path(port, port_path, sizeof(port_path));

    modbus_t *ctx = modbus_new_rtu(port_path, baud, parity, databits, stopbits);
    if(!ctx){
        fprintf(stderr, "modbus_new_rtu failed\n");
        fclose(f);
        free(segs);
        return 1;
    }
    modbus_set_slave(ctx, slave);
    modbus_set_error_recovery(ctx, MODBUS_ERROR_RECOVERY_LINK | MODBUS_ERROR_RECOVERY_PROTOCOL);

    if(modbus_connect(ctx) == -1){
        fprintf(stderr, "connect failed: %s\n", modbus_strerror(errno));
        modbus_free(ctx);
        fclose(f);
        free(segs);
        return 1;
    }

    if(do_setup){
        setup_speed_mode(ctx);
    }

    /* Read word order once (used only if we ever need 32-bit regs) */
    uint16_t order = 1;
    if(read_u16(ctx, REG_P0C_26, &order) != 0) order = 1;
    int low_first = (order != 0);

    /* Read command units per revolution (P05-02) once for scaling. */
    uint16_t cmd_units_per_rev = 0;
    if(read_u16(ctx, REG_P05_02, &cmd_units_per_rev) != 0){
        cmd_units_per_rev = 0;
    }

    if(use_ipc){
        puts("READY");
        fflush(stdout);
        char line[64];
        if(!fgets(line, sizeof(line), stdin)){
            modbus_close(ctx);
            modbus_free(ctx);
            fclose(f);
            free(segs);
            return 1;
        }
        trim(line);
        if(_stricmp(line, "START") != 0){
            modbus_close(ctx);
            modbus_free(ctx);
            fclose(f);
            free(segs);
            return 1;
        }
    }

    int seg_idx = 0;
    int total_samples = (int)(duration_s * rate_hz + 0.5);
    int64_t qpc_freq = qpc_freq_ticks();
    int64_t dt_ticks = (int64_t)((double)qpc_freq / rate_hz + 0.5);
    int64_t ramp_ticks = (int64_t)(RAMP_TIME_S * (double)qpc_freq + 0.5);
    int64_t start_ticks = qpc_now_ticks();
    int64_t next_ticks = start_ticks;

    int stop_requested = 0;
    int stop_sent = 0;
    int paused = 0;
    int64_t pause_start_ticks = 0;
    int64_t hard_stop_ticks = start_ticks + (int64_t)(duration_s * (double)qpc_freq);
    int p0b09_mode = 0;
    int rpm_mode = 0;
    int current_cmd_rpm = 0;
    int desired_seg_rpm = segs[0].rpm;
    rpm_ramp_t rpm_ramp = {0, 0, 0, start_ticks, start_ticks};

    /* Smooth first engagement: start at 0 rpm and ramp to the first segment target. */
    (void)cmd_rpm(ctx, 0);
    if(cmd_run(ctx) != 0){
        fprintf(stderr, "Falha RUN.\n");
    }
    ramp_begin(&rpm_ramp, current_cmd_rpm, desired_seg_rpm, start_ticks, ramp_ticks);

    for(int idx = 0; idx < total_samples; ){
        ipc_cmd_t ipc_cmd = ipc_poll_command(use_ipc);
        if(ipc_cmd == IPC_CMD_STOP){
            stop_requested = 1;
            break;
        }
        if(ipc_cmd == IPC_CMD_PAUSE && !paused){
            paused = 1;
            pause_start_ticks = qpc_now_ticks();
            stop_drive_now(ctx, &current_cmd_rpm, ramp_ticks);
            current_cmd_rpm = 0;
            ramp_begin(&rpm_ramp, 0, 0, pause_start_ticks, 0);
        }else if(ipc_cmd == IPC_CMD_RESUME && paused){
            int64_t now_resume = qpc_now_ticks();
            int64_t paused_ticks = now_resume - pause_start_ticks;
            if(paused_ticks < 0) paused_ticks = 0;
            start_ticks += paused_ticks;
            next_ticks += paused_ticks;
            hard_stop_ticks += paused_ticks;
            paused = 0;
            if(!stop_sent){
                (void)cmd_run(ctx);
                desired_seg_rpm = segs[seg_idx].rpm;
                ramp_begin(&rpm_ramp, current_cmd_rpm, desired_seg_rpm, now_resume, ramp_ticks);
            }
        }

        if(paused){
            Sleep(2);
            continue;
        }

        int64_t now = qpc_now_ticks();
        if(!stop_sent && now >= hard_stop_ticks){
            stop_drive_now(ctx, &current_cmd_rpm, ramp_ticks);
            current_cmd_rpm = 0;
            ramp_begin(&rpm_ramp, 0, 0, now, 0);
            stop_sent = 1;
        }
        if(now < next_ticks){
            Sleep(1);
            continue;
        }

        if(!stop_sent){
            int rpm_cmd_now = ramp_eval(&rpm_ramp, now);
            if(rpm_ramp.active && now >= rpm_ramp.t_end){
                rpm_ramp.active = 0;
                rpm_cmd_now = rpm_ramp.target_rpm;
            }
            if(rpm_cmd_now != current_cmd_rpm){
                if(cmd_rpm(ctx, rpm_cmd_now) == 0){
                    current_cmd_rpm = rpm_cmd_now;
                }
            }
        }

        int sampled_pos_ok = 0;
        int sampled_rpm_ok = 0;
        int32_t sampled_pos = 0;
        int16_t sampled_rpm = 0;
        set_timeouts_us(ctx, FAST_RESP_US, FAST_BYTE_US);
        uint16_t pos_raw = 0;
        if(read_p0b09_cached(ctx, &pos_raw, &p0b09_mode) == 0){
            if(cmd_units_per_rev > 0){
                /* Escala 0..65535 -> 0..(P05-02-1) */
                sampled_pos = (int32_t)(((uint32_t)pos_raw * (uint32_t)cmd_units_per_rev) / 65536u);
            }else{
                sampled_pos = (int32_t)pos_raw;
            }
            sampled_pos_ok = 1;
        }
        if(read_rpm_cached(ctx, &sampled_rpm, &rpm_mode) == 0){
            sampled_rpm_ok = 1;
        }

        /* Lost slots are explicit NULL. We never copy position values. */
        while(idx < total_samples && now >= (next_ticks + dt_ticks)){
            double t_s = (double)idx / rate_hz;
            int64_t t_qpc = start_ticks + (int64_t)idx * dt_ticks;

            while(seg_idx + 1 < seg_count && t_s >= segs[seg_idx].t_end){
                seg_idx++;
            }
            if(!stop_sent && segs[seg_idx].rpm != desired_seg_rpm){
                desired_seg_rpm = segs[seg_idx].rpm;
                ramp_begin(&rpm_ramp, current_cmd_rpm, desired_seg_rpm, now, ramp_ticks);
            }

            fprintf(f, "%d,%lld,%.6f,NULL,NULL,1,1\n", idx, (long long)t_qpc, t_s);
            idx++;
            next_ticks += dt_ticks;
        }

        if(idx < total_samples && now >= next_ticks){
            double t_s = (double)idx / rate_hz;
            int64_t t_qpc = start_ticks + (int64_t)idx * dt_ticks;

            while(seg_idx + 1 < seg_count && t_s >= segs[seg_idx].t_end){
                seg_idx++;
            }
            if(!stop_sent && segs[seg_idx].rpm != desired_seg_rpm){
                desired_seg_rpm = segs[seg_idx].rpm;
                ramp_begin(&rpm_ramp, current_cmd_rpm, desired_seg_rpm, now, ramp_ticks);
            }

            if(sampled_pos_ok) fprintf(f, "%d,%lld,%.6f,%ld,", idx, (long long)t_qpc, t_s, (long)sampled_pos);
            else               fprintf(f, "%d,%lld,%.6f,NULL,", idx, (long long)t_qpc, t_s);

            if(sampled_rpm_ok) fprintf(f, "%d,", (int)sampled_rpm);
            else               fprintf(f, "NULL,");

            fprintf(f, "%d,%d\n", sampled_pos_ok ? 0 : 1, sampled_rpm_ok ? 0 : 1);
            idx++;
            next_ticks += dt_ticks;
        }
    }

    if(stop_requested){
        puts("STOP requested (IPC).");
        fflush(stdout);
    }
    if(!stop_sent){
        stop_drive_now(ctx, &current_cmd_rpm, ramp_ticks);
    }
    modbus_close(ctx);
    modbus_free(ctx);
    fclose(f);
    free(segs);
    return 0;
}
